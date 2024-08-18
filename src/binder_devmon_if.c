/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2021-2022 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "binder_devmon.h"
#include "binder_log.h"

#include <ofono/log.h>

#include <mce_battery.h>
#include <mce_charger.h>
#include <mce_display.h>

#include <upower.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_network_types.h>

#include <gbinder_writer.h>

#include <gutil_macros.h>

#define BATMAN_SCREEN_PATH "/var/lib/batman/screen"

enum binder_devmon_if_battery_event {
    BATTERY_EVENT_VALID,
    BATTERY_EVENT_STATUS,
    BATTERY_EVENT_COUNT
};

enum binder_devmon_if_charger_event {
    CHARGER_EVENT_VALID,
    CHARGER_EVENT_STATE,
    CHARGER_EVENT_COUNT
};

enum binder_devmon_if_display_event {
    DISPLAY_EVENT_VALID,
    DISPLAY_EVENT_STATE,
    DISPLAY_EVENT_COUNT
};

typedef struct binder_devmon_if {
    BinderDevmon pub;
    MceBattery* battery;
    MceCharger* charger;
    MceDisplay* display;
    int cell_info_interval_short_ms;
    int cell_info_interval_long_ms;
    UpClient* upower;
} DevMon;

typedef struct binder_devmon_if_io {
    BinderDevmonIo pub;
    struct ofono_slot* slot;
    MceBattery* battery;
    MceCharger* charger;
    MceDisplay* display;
    RadioClient* client;
    RadioRequest* req;
    gboolean display_on;
    gboolean ind_filter_supported;
    gulong battery_event_id[BATTERY_EVENT_COUNT];
    gulong charger_event_id[CHARGER_EVENT_COUNT];
    gulong display_event_id[DISPLAY_EVENT_COUNT];
    int cell_info_interval_short_ms;
    int cell_info_interval_long_ms;
    UpClient* upower;
} DevMonIo;

#define DBG_(self,fmt,args...) \
    DBG("%s: " fmt, radio_client_slot((self)->client), ##args)

inline static DevMon* binder_devmon_if_cast(BinderDevmon* pub)
    { return G_CAST(pub, DevMon, pub); }

inline static DevMonIo* binder_devmon_if_io_cast(BinderDevmonIo* pub)
    { return G_CAST(pub, DevMonIo, pub); }

static inline gboolean binder_devmon_if_battery_ok(MceBattery* battery)
    { return battery->valid && battery->status >= MCE_BATTERY_OK; }

static inline gboolean binder_devmon_if_charging(MceCharger* charger)
    { return charger->valid && charger->state == MCE_CHARGER_ON; }

static gboolean binder_devmon_if_display_on(MceDisplay* display)
    { return display->valid && display->state != MCE_DISPLAY_STATE_OFF; }

typedef enum {
    BATMAN_NO_BATTERY = 0,      /** No battery present in the system */
    BATMAN_CHARGING = 1,        /** Battery is currently charging */
    BATMAN_DISCHARGING = 2,     /** Battery is currently discharging */
    BATMAN_FULLY_CHARGED = 3,   /** Battery is fully charged */
    BATMAN_UNKNOWN = 4          /** Battery state cannot be determined */
} batman_state_t;

static batman_state_t
get_battery_state(UpClient *upower)
{
    UpDevice *device = NULL;
    batman_state_t state = BATMAN_NO_BATTERY;

    device = up_device_new();

    if (!up_device_set_object_path_sync(device,
                                        "/org/freedesktop/UPower/devices/DisplayDevice",
                                        NULL,
                                        NULL)) {
        g_debug("Failed to set device object path");
        g_object_unref(device);
        return BATMAN_NO_BATTERY;
    }

    if (device != NULL) {
        UpDeviceState up_state;
        gboolean power_supply;
        UpDeviceKind kind;
        gdouble percent; /* still required for g_object_get even if unused */

        g_object_get(device,
                     "power-supply", &power_supply,
                     "kind", &kind,
                     "state", &up_state,
                     "percentage", &percent,
                     NULL);

        if (power_supply == TRUE && kind == UP_DEVICE_KIND_BATTERY) {
            switch (up_state) {
                case UP_DEVICE_STATE_CHARGING:
                    state = BATMAN_CHARGING;
                    break;

                case UP_DEVICE_STATE_DISCHARGING:
                    state = BATMAN_DISCHARGING;
                    break;

                case UP_DEVICE_STATE_FULLY_CHARGED:
                    state = BATMAN_FULLY_CHARGED;
                    break;

                default:
                    state = BATMAN_UNKNOWN;
                    break;
            }
        }

        g_object_unref(device);
    }

    if (state == BATMAN_NO_BATTERY)
        g_debug("no battery");

    return state;
}

static
void
binder_devmon_if_io_indication_filter_sent(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    DevMonIo* self = user_data;

    GASSERT(self->req == req);
    radio_request_unref(self->req);
    self->req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        const RADIO_AIDL_INTERFACE iface_aidl =
            radio_client_aidl_interface(self->client);
        guint32 code = iface_aidl == RADIO_NETWORK_INTERFACE ?
            RADIO_NETWORK_RESP_SET_INDICATION_FILTER :
            RADIO_RESP_SET_INDICATION_FILTER;

        if (resp == code) {
            if (error == RADIO_ERROR_REQUEST_NOT_SUPPORTED) {
                /* This is a permanent failure */
                DBG_(self, "Indication response filter is not supported");
                self->ind_filter_supported = FALSE;
            }
        } else {
            ofono_error("Unexpected setIndicationFilter response %d", resp);
        }
    }
}

static
void
binder_devmon_if_io_set_indication_filter(
    DevMonIo* self)
{
    if (self->ind_filter_supported) {
        GBinderWriter args;
        RADIO_REQ code;
        gint32 value;
        const RADIO_AIDL_INTERFACE iface_aidl =
            radio_client_aidl_interface(self->client);

        if (iface_aidl == RADIO_AIDL_INTERFACE_NONE) {
            /*
             * Both requests take the same args:
             *
             * setIndicationFilter(serial, bitfield<IndicationFilter>)
             * setIndicationFilter_1_2(serial, bitfield<IndicationFilter>)
             *
             * and both produce IRadioResponse.setIndicationFilterResponse()
             *
             * However setIndicationFilter_1_2 comments says "If unset, defaults
             * to @1.2::IndicationFilter:ALL" and it's unclear what "unset" means
             * wrt a bitmask. How is "unset" different from NONE which is zero.
             * To be on the safe side, let's always set the most innocently
             * looking bit which I think is DATA_CALL_DORMANCY.
             */
            if (radio_client_interface(self->client) < RADIO_INTERFACE_1_2) {
                code = RADIO_REQ_SET_INDICATION_FILTER;
                value = self->display_on ? RADIO_IND_FILTER_ALL :
                    RADIO_IND_FILTER_DATA_CALL_DORMANCY;
            } else if (radio_client_interface(self->client) < RADIO_INTERFACE_1_5) {
                code = RADIO_REQ_SET_INDICATION_FILTER_1_2;
                value = self->display_on ? RADIO_IND_FILTER_ALL_1_2 :
                    RADIO_IND_FILTER_DATA_CALL_DORMANCY;
            } else {
                code = RADIO_REQ_SET_INDICATION_FILTER_1_5;
                value = self->display_on ? RADIO_IND_FILTER_ALL_1_5 :
                    RADIO_IND_FILTER_DATA_CALL_DORMANCY;
            }
        } else {
            code = RADIO_NETWORK_REQ_SET_INDICATION_FILTER;
            /* Some devices don't like setting all filters */
            value = self->display_on ?
                RADIO_IND_FILTER_SIGNAL_STRENGTH |
                    RADIO_IND_FILTER_FULL_NETWORK_STATE |
                    RADIO_IND_FILTER_DATA_CALL_DORMANCY |
                    RADIO_IND_FILTER_LINK_CAPACITY_ESTIMATE |
                    RADIO_IND_FILTER_PHYSICAL_CHANNEL_CONFIG |
                    RADIO_IND_FILTER_REGISTRATION_FAILURE |
                    RADIO_IND_FILTER_BARRING_INFO :
                RADIO_IND_FILTER_DATA_CALL_DORMANCY;
        }

        radio_request_drop(self->req);
        self->req = radio_request_new(self->client, code, &args,
            binder_devmon_if_io_indication_filter_sent, NULL, self);
        gbinder_writer_append_int32(&args, value);
        DBG_(self, "Setting indication filter: 0x%02x", value);
        radio_request_submit(self->req);
    }
}

static
void
binder_devmon_if_io_set_cell_info_update_interval(
    DevMonIo* self)
{
    ofono_slot_set_cell_info_update_interval(self->slot, self,
        (self->display_on && (binder_devmon_if_charging(self->charger) ||
            binder_devmon_if_battery_ok(self->battery))) ?
                self->cell_info_interval_short_ms :
                self->cell_info_interval_long_ms);
}

static
void
binder_devmon_if_io_battery_cb(
    MceBattery* battery,
    void* user_data)
{
    binder_devmon_if_io_set_cell_info_update_interval((DevMonIo*)user_data);
}

static
void binder_devmon_if_io_charger_cb(
    MceCharger* charger,
    void* user_data)
{
    binder_devmon_if_io_set_cell_info_update_interval((DevMonIo*)user_data);
}

static
void
binder_devmon_if_io_display_cb(
    MceDisplay* display,
    void* user_data)
{
    DevMonIo* self = user_data;
    const gboolean display_on = binder_devmon_if_display_on(display);

    if (self->display_on != display_on) {
        self->display_on = display_on;
        binder_devmon_if_io_set_indication_filter(self);
        binder_devmon_if_io_set_cell_info_update_interval(self);
    }
}

static
gboolean
binder_devmon_if_io_batman_powersave(
   gpointer user_data)
{
    DevMonIo* self = (DevMonIo*)user_data;

    int display = 0;
    int state = BATMAN_UNKNOWN;

    /* would be nice to have a dbus system service that reports status of session instead of this */
    FILE *screen_file = fopen(BATMAN_SCREEN_PATH, "r");
    if (screen_file != NULL) {
        char screen_state[4];
        if (fgets(screen_state, sizeof(screen_state), screen_file) != NULL) {
            if (strncmp(screen_state, "yes", 3) == 0)
                display = 1;
            DBG_(self, "screen state: %s", screen_state);
        } else {
            DBG_(self, "Failed to read screen state");
        }
        fclose(screen_file);
    } else {
        DBG_(self, "Failed to open screen state file: %s", strerror(errno));
    }

    state = get_battery_state(self->upower);
    DBG_(self, "Battery state: %s",
         state == BATMAN_NO_BATTERY ? "no battery" :
         state == BATMAN_CHARGING ? "charging" :
         state == BATMAN_DISCHARGING ? "discharging" :
         state == BATMAN_FULLY_CHARGED ? "fully charged" : "unknown");

    const gboolean charging = (state == 1 || state == 2);
    gint cell_info_interval = (display || charging) ?
                               self->cell_info_interval_short_ms :
                               self->cell_info_interval_long_ms;

    DBG_(self, "Setting cell info interval: %d (display:%d charging:%d)",
         cell_info_interval, display, charging);

    ofono_slot_set_cell_info_update_interval(self->slot, self, cell_info_interval);

    return G_SOURCE_CONTINUE;
}


static
void
binder_devmon_if_io_free(
    BinderDevmonIo* io)
{
    DevMonIo* self = binder_devmon_if_io_cast(io);

    mce_battery_remove_all_handlers(self->battery, self->battery_event_id);
    mce_battery_unref(self->battery);

    mce_charger_remove_all_handlers(self->charger, self->charger_event_id);
    mce_charger_unref(self->charger);

    mce_display_remove_all_handlers(self->display, self->display_event_id);
    mce_display_unref(self->display);

    radio_request_drop(self->req);
    radio_client_unref(self->client);

    ofono_slot_drop_cell_info_requests(self->slot, self);
    ofono_slot_unref(self->slot);
    g_free(self);
}

static
BinderDevmonIo*
binder_devmon_if_start_io(
    BinderDevmon* devmon,
    RadioClient* ds_client,
    RadioClient* if_client,
    struct ofono_slot* slot)
{
    DevMon* impl = binder_devmon_if_cast(devmon);
    DevMonIo* self = g_new0(DevMonIo, 1);

    self->pub.free = binder_devmon_if_io_free;
    self->ind_filter_supported = TRUE;
    self->client = radio_client_ref(if_client);
    self->slot = ofono_slot_ref(slot);

    self->battery = mce_battery_ref(impl->battery);
    self->battery_event_id[BATTERY_EVENT_VALID] =
        mce_battery_add_valid_changed_handler(self->battery,
            binder_devmon_if_io_battery_cb, self);
    self->battery_event_id[BATTERY_EVENT_STATUS] =
        mce_battery_add_status_changed_handler(self->battery,
            binder_devmon_if_io_battery_cb, self);

    self->charger = mce_charger_ref(impl->charger);
    self->charger_event_id[CHARGER_EVENT_VALID] =
        mce_charger_add_valid_changed_handler(self->charger,
            binder_devmon_if_io_charger_cb, self);
    self->charger_event_id[CHARGER_EVENT_STATE] =
        mce_charger_add_state_changed_handler(self->charger,
            binder_devmon_if_io_charger_cb, self);

    self->display = mce_display_ref(impl->display);
    self->display_on = binder_devmon_if_display_on(self->display);
    self->display_event_id[DISPLAY_EVENT_VALID] =
        mce_display_add_valid_changed_handler(self->display,
            binder_devmon_if_io_display_cb, self);
    self->display_event_id[DISPLAY_EVENT_STATE] =
        mce_display_add_state_changed_handler(self->display,
            binder_devmon_if_io_display_cb, self);

    self->cell_info_interval_short_ms = impl->cell_info_interval_short_ms;
    self->cell_info_interval_long_ms = impl->cell_info_interval_long_ms;

    self->upower = impl->upower;

    binder_devmon_if_io_set_indication_filter(self);
    binder_devmon_if_io_set_cell_info_update_interval(self);

    g_timeout_add_seconds(5, binder_devmon_if_io_batman_powersave, self);

    return &self->pub;
}

static
void
binder_devmon_if_free(
    BinderDevmon* devmon)
{
    DevMon* self = binder_devmon_if_cast(devmon);

    mce_battery_unref(self->battery);
    mce_charger_unref(self->charger);
    mce_display_unref(self->display);
    g_object_unref(self->upower);
    g_free(self);
}

/*==========================================================================*
 * API
 *==========================================================================*/

BinderDevmon*
binder_devmon_if_new(
    const BinderSlotConfig* config)
{
    DevMon* self = g_new0(DevMon, 1);

    self->pub.free = binder_devmon_if_free;
    self->pub.start_io = binder_devmon_if_start_io;
    self->battery = mce_battery_new();
    self->charger = mce_charger_new();
    self->display = mce_display_new();
    self->upower = up_client_new();
    self->cell_info_interval_short_ms = config->cell_info_interval_short_ms;
    self->cell_info_interval_long_ms = config->cell_info_interval_long_ms;
    return &self->pub;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
