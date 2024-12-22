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
#include "binder_connman.h"
#include "binder_log.h"

#include <ofono/log.h>

#include <mce_battery.h>
#include <mce_charger.h>
#include <mce_display.h>

#include <radio_client.h>
#include <radio_request.h>

#include <gbinder_writer.h>

#include <gutil_macros.h>

#include <batman/batman-wrappers.h>

#include <sys/inotify.h>
#include <limits.h>
#include <errno.h>

#define BATMAN_SCREEN_PATH "/var/lib/batman/screen"
#define EVENT_BUF_LEN     (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))

enum binder_devmon_ds_battery_event {
    BATTERY_EVENT_VALID,
    BATTERY_EVENT_STATUS,
    BATTERY_EVENT_COUNT
};

enum binder_devmon_ds_charger_event {
    CHARGER_EVENT_VALID,
    CHARGER_EVENT_STATE,
    CHARGER_EVENT_COUNT
};

enum binder_devmon_ds_display_event {
    DISPLAY_EVENT_VALID,
    DISPLAY_EVENT_STATE,
    DISPLAY_EVENT_COUNT
};

enum binder_devmon_ds_connman_event {
    CONNMAN_EVENT_VALID,
    CONNMAN_EVENT_TETHERING,
    CONNMAN_EVENT_COUNT
};

typedef struct binder_devmon_ds {
    BinderDevmon pub;
    BinderConnman* connman;
    MceBattery* battery;
    MceCharger* charger;
    MceDisplay* display;
    int cell_info_interval_short_ms;
    int cell_info_interval_long_ms;
    UpClient* upower;
} DevMon;

typedef struct binder_devmon_ds_io {
    BinderDevmonIo pub;
    BinderConnman* connman;
    struct ofono_slot* slot;
    MceBattery* battery;
    MceCharger* charger;
    MceDisplay* display;
    RadioClient* client;
    RadioRequest* low_data_req;
    RadioRequest* charging_req;
    gboolean low_data;
    gboolean charging;
    gboolean low_data_supported;
    gboolean charging_supported;
    gulong connman_event_id[CONNMAN_EVENT_COUNT];
    gulong battery_event_id[BATTERY_EVENT_COUNT];
    gulong charger_event_id[CHARGER_EVENT_COUNT];
    gulong display_event_id[DISPLAY_EVENT_COUNT];
    int cell_info_interval_short_ms;
    int cell_info_interval_long_ms;
    UpClient* upower;
    int batman_inotify_fd;
    int batman_screen_wd;
    guint batman_watch_source;
} DevMonIo;

#define DBG_(self,fmt,args...) \
    DBG("%s: " fmt, radio_client_slot((self)->client), ##args)

static inline DevMon* binder_devmon_ds_cast(BinderDevmon* pub)
    { return G_CAST(pub, DevMon, pub); }

static inline DevMonIo* binder_devmon_ds_io_cast(BinderDevmonIo* pub)
    { return G_CAST(pub, DevMonIo, pub); }

static inline gboolean binder_devmon_ds_tethering_on(BinderConnman* connman)
    { return connman->valid && connman->tethering; }

static inline gboolean binder_devmon_ds_battery_ok(MceBattery* battery)
    { return battery->valid && battery->status >= MCE_BATTERY_OK; }

static inline gboolean binder_devmon_ds_charging(MceCharger* charger)
    { return charger->valid && charger->state == MCE_CHARGER_ON; }

static inline gboolean binder_devmon_ds_display_on(MceDisplay* display)
    { return display->valid && display->state != MCE_DISPLAY_STATE_OFF; }

static
void
binder_devmon_ds_io_low_data_state_sent(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    DevMonIo* self = user_data;

    GASSERT(self->low_data_req == req);
    radio_request_unref(self->low_data_req);
    self->low_data_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_SEND_DEVICE_STATE) {
            if (error == RADIO_ERROR_REQUEST_NOT_SUPPORTED) {
                DBG_(self, "LOW_DATA_EXPECTED state is not supported");
                self->low_data_supported = FALSE;
            }
        } else {
            ofono_error("Unexpected sendDeviceState response %d", resp);
            self->low_data_supported = FALSE;
        }
    }
}

static
void
binder_devmon_ds_io_charging_state_sent(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    DevMonIo* self = user_data;

    GASSERT(self->charging_req == req);
    radio_request_unref(self->charging_req);
    self->charging_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_SEND_DEVICE_STATE) {
            if (error == RADIO_ERROR_REQUEST_NOT_SUPPORTED) {
                DBG_(self, "CHARGING state is not supported");
                self->charging_supported = FALSE;
            }
        } else {
            ofono_error("Unexpected sendDeviceState response %d", resp);
            self->charging_supported = FALSE;
        }
    }
}

static
RadioRequest*
binder_devmon_ds_io_send_device_state(
    DevMonIo* self,
    RADIO_DEVICE_STATE type,
    gboolean state,
    RadioRequestCompleteFunc callback)
{
    GBinderWriter writer;
    RadioRequest* req = radio_request_new(self->client,
        RADIO_REQ_SEND_DEVICE_STATE, &writer, callback, NULL, self);

    /* sendDeviceState(int32_t serial, DeviceStateType type, bool state); */
    gbinder_writer_append_int32(&writer, type);
    gbinder_writer_append_bool(&writer, state);
    if (radio_request_submit(req)) {
        return req;
    } else {
        radio_request_unref(req);
        return NULL;
    }
}

static
void
binder_devmon_ds_io_update_charging(
    DevMonIo* self)
{
    const gboolean charging = binder_devmon_ds_charging(self->charger);

    if (self->charging != charging) {
        self->charging = charging;
        DBG_(self, "Charging %s", charging ? "on" : "off");
        if (self->charging_supported) {
            radio_request_drop(self->charging_req);
            self->charging_req = binder_devmon_ds_io_send_device_state(self,
                RADIO_DEVICE_STATE_CHARGING_STATE, charging,
                binder_devmon_ds_io_charging_state_sent);
        }
    }
}

static
void
binder_devmon_ds_io_update_low_data(
    DevMonIo* self)
{
    const gboolean low_data =
        !binder_devmon_ds_tethering_on(self->connman) &&
        !binder_devmon_ds_charging(self->charger) &&
        !binder_devmon_ds_display_on(self->display);

    if (self->low_data != low_data) {
        self->low_data = low_data;
        DBG_(self, "Low data is%s expected", low_data ? "" : " not");
        if (self->low_data_supported) {
            radio_request_drop(self->low_data_req);
            self->low_data_req = binder_devmon_ds_io_send_device_state(self,
                RADIO_DEVICE_STATE_LOW_DATA_EXPECTED, low_data,
                binder_devmon_ds_io_low_data_state_sent);
        }
    }
}

static
void
binder_devmon_ds_io_set_cell_info_update_interval(
    DevMonIo* self)
{
    ofono_slot_set_cell_info_update_interval(self->slot, self,
        (binder_devmon_ds_display_on(self->display) &&
            (binder_devmon_ds_charging(self->charger) ||
                binder_devmon_ds_battery_ok(self->battery))) ?
                    self->cell_info_interval_short_ms :
                    self->cell_info_interval_long_ms);
}

static
void
binder_devmon_ds_io_connman_cb(
    BinderConnman* connman,
    BINDER_CONNMAN_PROPERTY property,
    void* user_data)
{
    binder_devmon_ds_io_update_low_data((DevMonIo*)user_data);
}

static
void
binder_devmon_ds_io_battery_cb(
    MceBattery* battery,
    void* user_data)
{
    binder_devmon_ds_io_set_cell_info_update_interval((DevMonIo*)user_data);
}

static
void
binder_devmon_ds_io_display_cb(
    MceDisplay* display,
    void* user_data)
{
    DevMonIo* self = user_data;

    binder_devmon_ds_io_update_low_data(self);
    binder_devmon_ds_io_set_cell_info_update_interval(self);
}

static
void
binder_devmon_ds_io_charger_cb(
    MceCharger* charger,
    void* user_data)
{
    DevMonIo* self = user_data;

    binder_devmon_ds_io_update_low_data(self);
    binder_devmon_ds_io_update_charging(self);
    binder_devmon_ds_io_set_cell_info_update_interval(self);
}

static
gboolean
handle_batman_inotify_events_ds(
    GIOChannel* channel,
    GIOCondition condition,
    gpointer user_data)
{
    DevMonIo* self = user_data;

    if (channel) {
        gchar buf[EVENT_BUF_LEN];
        gsize bytes_read;
        GError* error = NULL;

        if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
            DBG_(self, "inotify watch failed, condition: %d", condition);
            self->batman_watch_source = 0;
            return G_SOURCE_REMOVE;
        }

        GIOStatus status = g_io_channel_read_chars(channel, buf, sizeof(buf),
            &bytes_read, &error);

        if (status == G_IO_STATUS_ERROR) {
            if (error) {
                DBG_(self, "inotify read failed, error: %s", error->message);
                g_error_free(error);
            }
            self->batman_watch_source = 0;
            return G_SOURCE_REMOVE;
        }

        if (bytes_read == 0) {
            DBG_(self, "No bytes read from inotify");
            return G_SOURCE_CONTINUE;
        }
    }

    int state = BATMAN_UNKNOWN;
    int display = 0;

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

    // Handle low data state changes
    const gboolean low_data = (display == 0 && state == BATMAN_DISCHARGING);
    if (self->low_data != low_data) {
        DBG_(self, "Low data changed from %s to %s (screen:%d battery:%s)",
             self->low_data ? "true" : "false", low_data ? "true" : "false",
             display,
             state == BATMAN_NO_BATTERY ? "no battery" :
             state == BATMAN_CHARGING ? "charging" :
             state == BATMAN_DISCHARGING ? "discharging" :
             state == BATMAN_FULLY_CHARGED ? "fully charged" : "unknown");
        self->low_data = low_data;
        if (self->low_data_supported) {
            radio_request_drop(self->low_data_req);
            self->low_data_req = binder_devmon_ds_io_send_device_state(self,
                RADIO_DEVICE_STATE_LOW_DATA_EXPECTED, low_data,
                binder_devmon_ds_io_low_data_state_sent);
        }
    }

    // Handle charging state changes
    const gboolean charging = (state == BATMAN_CHARGING || state == BATMAN_FULLY_CHARGED);
    if (self->charging != charging) {
        DBG_(self, "Charging changed from %s to %s",
             self->charging ? "true" : "false", charging ? "true" : "false");
        self->charging = charging;
        if (self->charging_supported) {
            radio_request_drop(self->charging_req);
            self->charging_req = binder_devmon_ds_io_send_device_state(self,
                RADIO_DEVICE_STATE_CHARGING_STATE, charging,
                binder_devmon_ds_io_charging_state_sent);
        }
    }

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
binder_devmon_ds_io_init_batman_watch(
    DevMonIo* self)
{
    GIOChannel* channel;

    self->batman_inotify_fd = inotify_init();
    if (self->batman_inotify_fd < 0) {
        DBG_(self, "Failed to initialize inotify: %s", strerror(errno));
        return;
    }

    DBG_(self, "inotify initialized successfully, fd=%d", self->batman_inotify_fd);

    self->batman_screen_wd = inotify_add_watch(self->batman_inotify_fd,
        BATMAN_SCREEN_PATH, IN_MODIFY | IN_CLOSE_WRITE);

    if (self->batman_screen_wd < 0) {
        DBG_(self, "Failed to add watch: %s", strerror(errno));
        close(self->batman_inotify_fd);
        self->batman_inotify_fd = -1;
        return;
    }

    DBG_(self, "watcher successfully added, wd=%d", self->batman_screen_wd);

    channel = g_io_channel_unix_new(self->batman_inotify_fd);
    g_io_channel_set_encoding(channel, NULL, NULL);
    g_io_channel_set_flags(channel, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_buffered(channel, FALSE);

    self->batman_watch_source = g_io_add_watch(channel,
        G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
        handle_batman_inotify_events_ds, self);

    DBG_(self, "GIO watcher added, source=%u", self->batman_watch_source);

    g_io_channel_unref(channel);

    handle_batman_inotify_events_ds(NULL, G_IO_IN, self);
}


static
void
binder_devmon_ds_io_free(
    BinderDevmonIo* io)
{
    DevMonIo* self = binder_devmon_ds_io_cast(io);

    binder_connman_remove_all_handlers(self->connman, self->connman_event_id);
    binder_connman_unref(self->connman);

    mce_battery_remove_all_handlers(self->battery, self->battery_event_id);
    mce_battery_unref(self->battery);

    mce_charger_remove_all_handlers(self->charger, self->charger_event_id);
    mce_charger_unref(self->charger);

    mce_display_remove_all_handlers(self->display, self->display_event_id);
    mce_display_unref(self->display);

    radio_request_drop(self->low_data_req);
    radio_request_drop(self->charging_req);
    radio_client_unref(self->client);

    ofono_slot_drop_cell_info_requests(self->slot, self);
    ofono_slot_unref(self->slot);

    if (self->batman_watch_source)
        g_source_remove(self->batman_watch_source);
    if (self->batman_screen_wd >= 0)
        inotify_rm_watch(self->batman_inotify_fd, self->batman_screen_wd);
    if (self->batman_inotify_fd >= 0)
        close(self->batman_inotify_fd);

    g_free(self);
}

static
BinderDevmonIo*
binder_devmon_ds_start_io(
    BinderDevmon* devmon,
    RadioClient* client,
    struct ofono_slot* slot)
{
    DevMon* ds = binder_devmon_ds_cast(devmon);
    DevMonIo* self = g_new0(DevMonIo, 1);

    self->pub.free = binder_devmon_ds_io_free;
    self->low_data_supported = TRUE;
    self->charging_supported = TRUE;
    self->client = radio_client_ref(client);
    self->slot = ofono_slot_ref(slot);

    self->connman = binder_connman_ref(ds->connman);
    self->connman_event_id[CONNMAN_EVENT_VALID] =
        binder_connman_add_property_changed_handler(self->connman,
            BINDER_CONNMAN_PROPERTY_VALID,
            binder_devmon_ds_io_connman_cb, self);
    self->connman_event_id[CONNMAN_EVENT_TETHERING] =
        binder_connman_add_property_changed_handler(self->connman,
            BINDER_CONNMAN_PROPERTY_TETHERING,
            binder_devmon_ds_io_connman_cb, self);

    self->battery = mce_battery_ref(ds->battery);
    self->battery_event_id[BATTERY_EVENT_VALID] =
        mce_battery_add_valid_changed_handler(self->battery,
            binder_devmon_ds_io_battery_cb, self);
    self->battery_event_id[BATTERY_EVENT_STATUS] =
        mce_battery_add_status_changed_handler(self->battery,
            binder_devmon_ds_io_battery_cb, self);

    self->charger = mce_charger_ref(ds->charger);
    self->charger_event_id[CHARGER_EVENT_VALID] =
        mce_charger_add_valid_changed_handler(self->charger,
            binder_devmon_ds_io_charger_cb, self);
    self->charger_event_id[CHARGER_EVENT_STATE] =
        mce_charger_add_state_changed_handler(self->charger,
            binder_devmon_ds_io_charger_cb, self);

    self->display = mce_display_ref(ds->display);
    self->display_event_id[DISPLAY_EVENT_VALID] =
        mce_display_add_valid_changed_handler(self->display,
            binder_devmon_ds_io_display_cb, self);
    self->display_event_id[DISPLAY_EVENT_STATE] =
        mce_display_add_state_changed_handler(self->display,
            binder_devmon_ds_io_display_cb, self);

    self->cell_info_interval_short_ms = ds->cell_info_interval_short_ms;
    self->cell_info_interval_long_ms = ds->cell_info_interval_long_ms;

    self->upower = ds->upower;

    binder_devmon_ds_io_update_low_data(self);
    binder_devmon_ds_io_update_charging(self);
    binder_devmon_ds_io_set_cell_info_update_interval(self);

    binder_devmon_ds_io_init_batman_watch(self);

    return &self->pub;
}

static
void
binder_devmon_ds_free(
    BinderDevmon* devmon)
{
    DevMon* self = binder_devmon_ds_cast(devmon);

    binder_connman_unref(self->connman);
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
binder_devmon_ds_new(
    const BinderSlotConfig* config)
{
    DevMon* self = g_new0(DevMon, 1);

    self->pub.free = binder_devmon_ds_free;
    self->pub.start_io = binder_devmon_ds_start_io;
    self->connman = binder_connman_new();
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
