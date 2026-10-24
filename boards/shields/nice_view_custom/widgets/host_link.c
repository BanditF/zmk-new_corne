/*
 * PC link over Raw HID: the host panel (0xD5) and right-hand art pushes (0xD6). Protocol in
 * host_link.h.
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <raw_hid/events.h>
#include <zmk/behavior.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>

#include "half_marker.h"
#include "host_link.h"
#if IS_ENABLED(CONFIG_FOXGLOVE_LAMPARRAY)
#include "lamp_strip.h"
#endif
#include "screen_snap.h"
#include "art_relay.h"
#include "rgb_idle.h"
#include "behavior_safety.h"
#if IS_ENABLED(CONFIG_FOXGLOVE_TEST_HOOKS)
#include "test_hooks.h"
#endif

#if IS_ENABLED(CONFIG_NICE_VIEW_RGB_IDLE)
#include <zmk/rgb_underglow.h>
#endif

enum { PANEL_HELLO = 0x01, PANEL_FILL, PANEL_ROWS, PANEL_PRESENT, PANEL_RELEASE, PANEL_SCREENS };
enum { ART_BEGIN = 0x01, ART_DATA, ART_COMMIT, ART_SELECT, ART_STATUS, ART_SAVE, ART_SLOT, ART_ERASE };
enum { SETTING_RGB_SYNC = 0x01, SETTING_INVOKE, SETTING_RGB_STATE, SETTING_CONN_STATE, SETTING_POSITION,
       SETTING_SCREEN_SNAP, SETTING_SCREEN_READ, SETTING_BOOTLOADER,
       SETTING_BUILD_INFO, SETTING_LAMP_TEST, SETTING_GUARD_TEST };
enum {
    INVOKE_OK, INVOKE_UNKNOWN, INVOKE_BUSY, INVOKE_FAILED, INVOKE_UNSUPPORTED,
    INVOKE_BAD_LENGTH, INVOKE_REFUSED
};

#define REPORT_SIZE CONFIG_RAW_HID_REPORT_SIZE
#define ALIVE_TIMEOUT K_SECONDS(CONFIG_NICE_VIEW_HOST_PANEL_TIMEOUT_S)

/*
 * Reports arrive on the USB thread. Rows go straight into the back buffer and PRESENT copies it
 * to the front one; the display thread only reads the front buffer (host_panel_get), so drawing
 * never waits on USB and USB never waits on drawing. The panel only takes the area at the first
 * PRESENT, so neither a half-written back buffer nor an empty front one is ever shown, and it keeps
 * the last presented frame when the PC goes quiet (CONFIG_NICE_VIEW_HOST_PANEL_TIMEOUT_S = 0).
 */
static struct k_spinlock lock;
static uint8_t back[HOST_PANEL_BYTES];
static uint8_t front[HOST_PANEL_BYTES];
static bool alive;

static atomic_t host_seen;

static uint8_t right_battery = 0xff; // unknown

bool host_panel_get(uint8_t buf[HOST_PANEL_BYTES]) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    bool on = alive;
    if (on) {
        memcpy(buf, front, HOST_PANEL_BYTES);
    }
    k_spin_unlock(&lock, key);
    return on;
}

BUILD_ASSERT(HOST_PANEL_W % 8 == 0, "panel rows are whole bytes");

#if !defined(FOXGLOVE_HAS_WIDGETS) && !defined(FOXGLOVE_HAS_PANEL)
// A board without a panel drawer (status.c, foxglove_link/panel_lvgl.c) has nothing to redraw.
void host_panel_redraw(void) {}
#endif

static void redraw_cb(struct k_work *work) { host_panel_redraw(); }
static K_WORK_DEFINE(redraw_work, redraw_cb);

static void redraw(void) {
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &redraw_work);
    }
}

static void set_alive(bool on) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    bool changed = alive != on;
    alive = on;
    k_spin_unlock(&lock, key);
    if (changed) {
        redraw();
    }
}

static void timeout_cb(struct k_work *work) { set_alive(false); }
static K_WORK_DELAYABLE_DEFINE(timeout_work, timeout_cb);

// Replies go out from the system work queue, not from inside the USB request that asked.
K_MSGQ_DEFINE(reply_msgq, REPORT_SIZE, 4, 1);

static void reply_cb(struct k_work *work) {
    static uint8_t report[REPORT_SIZE];
    while (k_msgq_get(&reply_msgq, report, K_NO_WAIT) == 0) {
        raise_raw_hid_sent_event((struct raw_hid_sent_event){.data = report, .length = REPORT_SIZE});
    }
}
static K_WORK_DEFINE(reply_work, reply_cb);

static void reply(const uint8_t *data, int len) {
    uint8_t report[REPORT_SIZE] = {0};
    memcpy(report, data, MIN(len, REPORT_SIZE));
    if (k_msgq_put(&reply_msgq, report, K_NO_WAIT) == 0) {
        k_work_submit(&reply_work);
    }
}

void host_link_screen_reply(const uint8_t *data, int len) { reply(data, len); }

#ifndef FOXGLOVE_HAS_WIDGETS
// Without our widgets there's no screen readback (screen_snap.c): D7 06/07 answer 4, not built in.
void screen_snap_command(const uint8_t *data, int len) {
    uint8_t r[7] = {HOST_LINK_SETTINGS, 0x80 | data[1], 4, len >= 3 ? data[2] : 0};
    reply(r, sizeof(r));
}
#endif

void host_link_send(const uint8_t *data, int len) { reply(data, len); }

void host_link_build_info_reply(uint8_t status, uint8_t half,
                                const struct foxglove_build_info *build) {
    uint8_t r[4 + sizeof(struct foxglove_build_info)] = {HOST_LINK_SETTINGS,
                                                         0x80 | SETTING_BUILD_INFO, status, half};
    if (build != NULL) {
        memcpy(&r[4], build, sizeof(*build));
    }
    reply(r, sizeof(r));
}

static uint8_t right_link(void) {
#if IS_ENABLED(CONFIG_NICE_VIEW_ART_RELAY)
    return art_relay_link();
#else
    return 0;
#endif
}

static void hello_reply(void) {
    int8_t discovery = 0;
    uint8_t tries = 0;
#if IS_ENABLED(CONFIG_NICE_VIEW_ART_RELAY)
    art_relay_discovery(&discovery, &tries);
#endif
    uint8_t r[11 + 10 + 4 + 2 + 4 + 1] = {HOST_LINK_PANEL, 0x80 | PANEL_HELLO, HOST_LINK_VERSION, 0,
                          0,               0,                  0,                 right_link(),
                          right_battery,   (uint8_t)discovery, tries};
    // Bytes 3-6: the host panel (the first panel in the board's table) for apps without SCREENS.
    for (int i = 0; i < foxglove_screen_count; i++) {
        const struct screen_descriptor *p = &foxglove_screens[i];
        if (p->kind == SCREEN_PANEL) {
            r[3] = MIN(p->width, UINT8_MAX);
            r[4] = MIN(p->height, UINT8_MAX);
            r[5] = MIN(p->x, UINT8_MAX);
            r[6] = MIN(p->y, UINT8_MAX);
            break;
        }
    }
#if IS_ENABLED(CONFIG_NICE_VIEW_ART_RELAY)
    art_relay_debug(&r[11]);
#endif
    uint32_t caps = HOST_LINK_CAP_PANEL | HOST_LINK_CAP_CONN_STATE | HOST_LINK_CAP_SCREEN;
#if IS_ENABLED(CONFIG_NICE_VIEW_ART_RELAY)
    caps |= HOST_LINK_CAP_ART_RELAY | HOST_LINK_CAP_ART_STATUS_EVENT | HOST_LINK_CAP_ART_SLOTS;
#endif
#if IS_ENABLED(CONFIG_NICE_VIEW_RGB_IDLE)
    caps |= HOST_LINK_CAP_SETTINGS_RGB | HOST_LINK_CAP_RGB_STATE_EVENT;
#endif
#if IS_ENABLED(CONFIG_FOXGLOVE_TEST_HOOKS)
    caps |= HOST_LINK_CAP_POSITION | HOST_LINK_CAP_BOOTLOADER;
#endif
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
    caps |= HOST_LINK_CAP_INVOKE_REFUSES; // INVOKE exists, with the refuse list
#endif
    caps |= HOST_LINK_CAP_CONN_STATE_EVENT | HOST_LINK_CAP_BUILD_INFO | HOST_LINK_CAP_SCREENS;
    sys_put_le32(caps, &r[21]);
    sys_put_le16(CONFIG_NICE_VIEW_HOST_PANEL_TIMEOUT_S, &r[25]);
    struct foxglove_build_info build;
    foxglove_build_info(&build);
    r[27] = build.major;
    r[28] = build.minor;
    r[29] = build.patch;
    r[30] = build.profile;
#if IS_ENABLED(CONFIG_NICE_VIEW_ART_RELAY)
    r[31] = art_relay_guard_drops();
#endif
    reply(r, sizeof(r));
}

/* D5 06 SCREENS (#160): the board's table (foxglove_screens[], e.g. screens_corne.c) as the PC
 * sees it. */

static void screens_reply(uint8_t index) {
    uint8_t r[5 + sizeof(struct screen_descriptor)] = {HOST_LINK_PANEL, 0x80 | PANEL_SCREENS, 5,
                                                       foxglove_screen_count, index};
    if (index < foxglove_screen_count) {
        struct screen_descriptor s = foxglove_screens[index];
        s.version = SCREEN_DESCRIPTOR_VERSION;
        r[2] = 0;
        memcpy(&r[5], &s, sizeof(s));
    }
    reply(r, sizeof(r));
}

static void panel_command(const uint8_t *d) {
    if (CONFIG_NICE_VIEW_HOST_PANEL_TIMEOUT_S > 0 && d[1] != PANEL_RELEASE) {
        k_work_reschedule(&timeout_work, ALIVE_TIMEOUT);
    }

    switch (d[1]) {
    case PANEL_HELLO: {
        hello_reply();
        break;
    }
    case PANEL_FILL: {
        k_spinlock_key_t key = k_spin_lock(&lock);
        memset(back, d[2] ? 0xff : 0x00, sizeof(back));
        k_spin_unlock(&lock, key);
        break;
    }
    case PANEL_ROWS: {
        int y = d[2], n = MIN(d[3], 3);
        if (y + n > HOST_PANEL_H || 4 + n * HOST_PANEL_STRIDE > REPORT_SIZE) {
            break;
        }
        k_spinlock_key_t key = k_spin_lock(&lock);
        memcpy(&back[y * HOST_PANEL_STRIDE], &d[4], n * HOST_PANEL_STRIDE);
        k_spin_unlock(&lock, key);
        break;
    }
    case PANEL_PRESENT: {
        k_spinlock_key_t key = k_spin_lock(&lock);
        memcpy(front, back, sizeof(front));
        k_spin_unlock(&lock, key);
        set_alive(true);
        redraw();
        break;
    }
    case PANEL_SCREENS:
        screens_reply(d[2]);
        break;
    case PANEL_RELEASE:
        k_work_cancel_delayable(&timeout_work);
        set_alive(false);
        break;
    }
}

static void art_command(const uint8_t *d) {
    uint8_t r[2 + ART_RELAY_REPORT_SIZE] = {HOST_LINK_ART, 0x80 | d[1]};
#if IS_ENABLED(CONFIG_NICE_VIEW_ART_RELAY)
    int err = 0;
    switch (d[1]) {
    case ART_BEGIN:
        err = art_relay_begin(sys_get_le16(&d[2]), sys_get_le32(&d[4]));
        break;
    case ART_DATA:
        err = art_relay_data(sys_get_le16(&d[2]), &d[5], MIN(d[4], REPORT_SIZE - 5));
        if (err == 0) {
            return;
        }
        break;
    case ART_COMMIT:
        err = art_relay_commit();
        break;
    case ART_SELECT:
        err = art_relay_select((int8_t)d[2]);
        break;
    case ART_STATUS:
        break;
    case ART_SAVE:
        err = art_relay_save(d[2], &d[3]);
        break;
    case ART_ERASE:
        err = art_relay_erase(d[2]);
        break;
    case ART_SLOT:
        if (art_relay_slot(d[2]) == 0) {
            return; // D6 87 comes from the relay
        }
        r[2] = 1; // busy
        r[3] = d[2];
        reply(r, sizeof(r));
        return;
    default:
        return;
    }
    ARG_UNUSED(err); // the error is in the report
    art_relay_report(&r[2]);
#else
    r[2] = ART_LEFT_FAILED;
    r[3] = ART_ERR_NO_LINK;
#endif
    reply(r, sizeof(r));
}

void host_link_art_status(void) {
    if (atomic_get(&host_seen)) {
        const uint8_t d[] = {HOST_LINK_ART, ART_STATUS};
        art_command(d);
    }
}

/* INVOKE: runs on the system work queue, like the keymap does for key presses */

#define INVOKE_MAX_HOLD_MS 10000
// No key sits here; behaviors that track positions (hold-tap, sticky keys) see a key of its own.
#define INVOKE_POSITION UINT32_MAX

static struct zmk_behavior_binding invoke_binding;
static struct zmk_behavior_binding_event invoke_event;
static uint16_t invoke_id; // of the invoke that holds invoke_busy
static uint16_t invoke_hold_ms;
static atomic_t invoke_busy;

static void invoke_reply(uint8_t status, uint16_t id) {
    uint8_t r[5] = {HOST_LINK_SETTINGS, 0x80 | SETTING_INVOKE, status};
    sys_put_le16(id, &r[3]);
    reply(r, sizeof(r));
}

static void invoke_release_cb(struct k_work *work) {
    invoke_event.timestamp = k_uptime_get();
    zmk_behavior_invoke_binding(&invoke_binding, invoke_event, false);
    atomic_set(&invoke_busy, 0);
}
static K_WORK_DELAYABLE_DEFINE(invoke_release_work, invoke_release_cb);

static void invoke_press_cb(struct k_work *work) {
    invoke_event = (struct zmk_behavior_binding_event){
        .layer = zmk_keymap_highest_layer_active(),
        .position = INVOKE_POSITION,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    int ret = zmk_behavior_invoke_binding(&invoke_binding, invoke_event, true);
    invoke_reply(ret < 0 ? INVOKE_FAILED : INVOKE_OK, invoke_id);
    k_work_schedule(&invoke_release_work, K_MSEC(invoke_hold_ms));
}
static K_WORK_DEFINE(invoke_press_work, invoke_press_cb);

// d starts at the local id.
static uint8_t invoke(const uint8_t *d, int len) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
    if (len < 10) {
        return INVOKE_BAD_LENGTH;
    }
    uint16_t id = sys_get_le16(&d[0]);
    const char *name = zmk_behavior_find_behavior_name_from_local_id(id);
    if (name == NULL) {
        return INVOKE_UNKNOWN;
    }
    if (foxglove_behavior_refused(name)) {
        return INVOKE_REFUSED;
    }
    if (!atomic_cas(&invoke_busy, 0, 1)) {
        return INVOKE_BUSY;
    }
    invoke_id = id;
    invoke_binding = (struct zmk_behavior_binding){
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        .local_id = id,
#endif
        .behavior_dev = name,
        .param1 = sys_get_le32(&d[2]),
        .param2 = sys_get_le32(&d[6]),
    };
    invoke_hold_ms = MIN(len >= 12 ? sys_get_le16(&d[10]) : 0, INVOKE_MAX_HOLD_MS);
    k_work_submit(&invoke_press_work); // replies once pressed
    return INVOKE_OK;
#else
    return INVOKE_UNSUPPORTED;
#endif
}

void host_link_bootloader_reply(uint8_t status, uint8_t half) {
    const uint8_t r[] = {HOST_LINK_SETTINGS, 0x80 | SETTING_BOOTLOADER, status, half};
    reply(r, sizeof(r));
}

void host_link_position_reply(uint8_t status, uint8_t pos) {
    const uint8_t r[] = {HOST_LINK_SETTINGS, 0x80 | SETTING_POSITION, status, pos};
    reply(r, sizeof(r));
}

#define CONN_STATE_SIZE 8

static void conn_state(uint8_t r[CONN_STATE_SIZE]) {
    memset(r, 0, CONN_STATE_SIZE);
    r[0] = HOST_LINK_SETTINGS;
    r[1] = 0x80 | SETTING_CONN_STATE;
#if IS_ENABLED(CONFIG_ZMK_BLE)
    r[2] = zmk_ble_active_profile_index();
    r[3] = zmk_ble_active_profile_is_connected() ? 1 : 0;
    r[4] = zmk_ble_active_profile_is_open() ? 0 : 1;
    r[7] = ZMK_BLE_PROFILE_COUNT;
#endif
    r[5] = zmk_endpoints_selected().transport == ZMK_TRANSPORT_BLE ? 1 : 0;
#if IS_ENABLED(CONFIG_ZMK_USB)
    r[6] = zmk_usb_is_hid_ready() ? 1 : 0;
#endif
}

// Unsolicited CONN_STATE: after host traffic, a change of the active BLE profile, its connection
// or bond, the endpoint or USB sends the same reply on its own, once things have settled.
#define CONN_STATE_SETTLE K_MSEC(50)

static uint8_t conn_state_sent[CONN_STATE_SIZE];

static void conn_state_event_cb(struct k_work *work) {
    uint8_t r[CONN_STATE_SIZE];
    conn_state(r);
    if (memcmp(r, conn_state_sent, sizeof(r)) != 0) {
        memcpy(conn_state_sent, r, sizeof(r));
        reply(r, sizeof(r));
    }
}
static K_WORK_DELAYABLE_DEFINE(conn_state_event_work, conn_state_event_cb);

static int conn_state_listener(const zmk_event_t *eh) {
    if (atomic_get(&host_seen)) {
        k_work_reschedule(&conn_state_event_work, CONN_STATE_SETTLE);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(host_link_conn, conn_state_listener);
ZMK_SUBSCRIPTION(host_link_conn, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(host_link_conn, zmk_ble_active_profile_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(host_link_conn, zmk_usb_conn_state_changed);
#endif

#define RGB_STATE_SIZE (2 + 2 * sizeof(struct rgb_idle_wire) + 1)

// The RGB_STATE reply; asks the relay for a fresher right state on the way.
static void rgb_state(uint8_t r[RGB_STATE_SIZE]) {
    struct rgb_idle_wire left = {0}, right = {0};
    uint8_t age = 0xff;
#if IS_ENABLED(CONFIG_NICE_VIEW_RGB_IDLE)
    rgb_idle_state(&left);
#endif
#if IS_ENABLED(CONFIG_NICE_VIEW_ART_RELAY)
    age = art_relay_right_rgb(&right);
#endif
    r[0] = HOST_LINK_SETTINGS;
    r[1] = 0x80 | SETTING_RGB_STATE;
    memcpy(&r[2], &left, sizeof(left));
    r[2 + sizeof(left)] = age;
    memcpy(&r[3 + sizeof(left)], &right, sizeof(right));
}

// Unsolicited RGB_STATE: after host traffic, the left's underglow switching on or off (idle,
// wake, a key, sync) sends the same reply on its own. The right's half of it is the relay's
// freshest read, taken RGB_STATE_SETTLE after the change so the right has followed by then.
#define RGB_STATE_SETTLE K_MSEC(600)

static uint8_t rgb_state_sent = 0xff; // left on | right on << 1 | right known << 2, last sent

static void rgb_state_event_cb(struct k_work *work) {
    uint8_t r[RGB_STATE_SIZE];
    rgb_state(r);
    uint8_t age = r[2 + sizeof(struct rgb_idle_wire)];
    uint8_t now = r[2] | r[3 + sizeof(struct rgb_idle_wire)] << 1 | (age != 0xff) << 2;
    if (now != rgb_state_sent) {
        rgb_state_sent = now;
        reply(r, sizeof(r));
    }
}
static K_WORK_DELAYABLE_DEFINE(rgb_state_event_work, rgb_state_event_cb);

void host_link_rgb_changed(void) {
    if (atomic_get(&host_seen)) {
#if IS_ENABLED(CONFIG_NICE_VIEW_ART_RELAY)
        struct rgb_idle_wire right;
        art_relay_right_rgb(&right); // starts the status read the event will use
#endif
        k_work_reschedule(&rgb_state_event_work, RGB_STATE_SETTLE);
    }
}

#if IS_ENABLED(CONFIG_FOXGLOVE_TEST_HOOKS) && IS_ENABLED(CONFIG_FOXGLOVE_LAMPARRAY)
// D7 0A LAMP_TEST: one lamp lit, so a person can name the key above it (#115).
static uint8_t lamp_test(const uint8_t *d, int len) {
    static uint8_t right_rgb[LAMP_STRIP_LEN][3];
    if (len < 4 || d[2] > 1) {
        return 5;
    }
    uint8_t half = d[2], index = d[3];
    bool on = index != 0xff;
    if (on && (index >= LAMP_STRIP_LEN || len < 7)) {
        return 5;
    }
    if (half == 0) {
        for (int i = 0; i < LAMP_STRIP_LEN; i++) {
            lamp_strip_set(i, 0, 0, 0);
        }
        if (on) {
            lamp_strip_set(index, d[4], d[5], d[6]);
        }
        lamp_strip_host(on);
        lamp_strip_show();
        return 0;
    }
    if (art_relay_link() != 2) {
        return 2;
    }
    memset(right_rgb, 0, sizeof(right_rgb));
    if (on) {
        memcpy(right_rgb[index], &d[4], 3);
    }
    art_relay_lamps(on, right_rgb);
    return 0;
}
#endif

static void settings_command(const uint8_t *d, int len) {
    if (d[1] == SETTING_SCREEN_SNAP || d[1] == SETTING_SCREEN_READ) {
        screen_snap_command(d, len);
        return;
    }
    switch (d[1]) {
    case SETTING_LAMP_TEST: {
        uint8_t r[5] = {HOST_LINK_SETTINGS, 0x80 | SETTING_LAMP_TEST, 4, d[2], d[3]};
#if IS_ENABLED(CONFIG_FOXGLOVE_TEST_HOOKS) && IS_ENABLED(CONFIG_FOXGLOVE_LAMPARRAY)
        r[2] = lamp_test(d, len);
#endif
        reply(r, sizeof(r));
        break;
    }
    case SETTING_GUARD_TEST: {
        uint8_t r[3] = {HOST_LINK_SETTINGS, 0x80 | SETTING_GUARD_TEST, 4};
#if IS_ENABLED(CONFIG_FOXGLOVE_TEST_HOOKS) && IS_ENABLED(CONFIG_NICE_VIEW_ART_RELAY)
        int err = art_relay_guard_test();
        r[2] = err == 0 ? 0 : err == -ENOTCONN ? 2 : 4;
#endif
        reply(r, sizeof(r));
        break;
    }
    case SETTING_BUILD_INFO: {
        uint8_t half = len >= 3 ? d[2] : 0;
        if (half == 0) {
            struct foxglove_build_info build;
            foxglove_build_info(&build);
            host_link_build_info_reply(0, 0, &build);
            break;
        }
#if IS_ENABLED(CONFIG_NICE_VIEW_ART_RELAY)
        if (half == 1) {
            if (art_relay_build_info()) {
                host_link_build_info_reply(1, 1, NULL); // one is on its way
            }
            break;
        }
#endif
        host_link_build_info_reply(half == 1 ? 2 : 5, half, NULL);
        break;
    }
    case SETTING_BOOTLOADER: {
        uint8_t r[7] = {HOST_LINK_SETTINGS, 0x80 | SETTING_BOOTLOADER, 4};
#if IS_ENABLED(CONFIG_FOXGLOVE_TEST_HOOKS)
        if (test_hooks_bootloader(d, len, r)) {
            break; // the reply comes from the relay
        }
#endif
        reply(r, sizeof(r));
        break;
    }
    case SETTING_POSITION: {
        uint8_t status = 4;
#if IS_ENABLED(CONFIG_FOXGLOVE_TEST_HOOKS)
        status = len < 4 ? 5 : test_hooks_press(d[2], d[3]);
#endif
        if (status != 0) {
            host_link_position_reply(status, d[2]);
        }
        break;
    }
    case SETTING_CONN_STATE: {
        uint8_t r[CONN_STATE_SIZE];
        conn_state(r);
        reply(r, sizeof(r));
        break;
    }
    case SETTING_RGB_SYNC: {
        uint8_t r[5] = {HOST_LINK_SETTINGS, 0x80 | SETTING_RGB_SYNC, 0xff, 0, right_link()};
#if IS_ENABLED(CONFIG_NICE_VIEW_RGB_IDLE)
        if (len >= 3 && d[2] <= 1) { // a report cut short is a read
            rgb_idle_set_sync(d[2]);
        }
        bool on = false;
        zmk_rgb_underglow_get_state(&on);
        r[2] = rgb_idle_sync();
        r[3] = on;
#endif
        reply(r, sizeof(r));
        break;
    }
    case SETTING_INVOKE: {
        uint8_t status = invoke(&d[2], len - 2);
        if (status != INVOKE_OK) {
            invoke_reply(status, sys_get_le16(&d[2]));
        }
        break;
    }
    case SETTING_RGB_STATE: {
        uint8_t r[RGB_STATE_SIZE];
        rgb_state(r);
        reply(r, sizeof(r));
        break;
    }
    }
}

static int raw_hid_received_listener(const zmk_event_t *eh) {
    const struct raw_hid_received_event *ev = as_raw_hid_received_event(eh);
    if (ev == NULL || ev->length < 2) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    // Pad short reports so commands can read their fixed fields.
    uint8_t d[REPORT_SIZE] = {0};
    memcpy(d, ev->data, MIN(ev->length, REPORT_SIZE));

    if (d[0] == HOST_LINK_PANEL || d[0] == HOST_LINK_ART || d[0] == HOST_LINK_SETTINGS) {
        atomic_set(&host_seen, 1);
#if IS_ENABLED(CONFIG_FOXGLOVE_TEST_HOOKS)
        test_hooks_host_seen();
#endif
    }
    switch (d[0]) {
    case HOST_LINK_PANEL:
        panel_command(d);
        break;
    case HOST_LINK_ART:
        art_command(d);
        break;
    case HOST_LINK_SETTINGS:
        settings_command(d, MIN(ev->length, REPORT_SIZE));
        break;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(host_link, raw_hid_received_listener);
ZMK_SUBSCRIPTION(host_link, raw_hid_received_event);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
static int peripheral_battery_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (ev != NULL && ev->source == 0 && right_battery != ev->state_of_charge) {
        right_battery = ev->state_of_charge;
        if (atomic_get(&host_seen)) {
            hello_reply();
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(host_link_battery, peripheral_battery_listener);
ZMK_SUBSCRIPTION(host_link_battery, zmk_peripheral_battery_state_changed);
#endif
