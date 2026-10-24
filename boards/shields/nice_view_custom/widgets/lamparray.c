/*
 * HID LampArray (usage page 0x59) on the left's USB, so Windows Dynamic Lighting can drive the
 * underglow of both halves (#88, #115): one LampArray of 42 lamps, the right's over the art
 * relay.
 * SPDX-License-Identifier: MIT
 *
 * All six reports are feature reports (HID Usage Tables, Lighting and Illumination page): on
 * USB the control pipe, over Bluetooth a second HID service (lamparray_hog.c,
 * FOXGLOVE_LAMPARRAY_BLE). While the host is in charge (autonomous mode off), ZMK's underglow and
 * rgb_idle step aside and this file writes the strip; autonomous mode on hands it back.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "art_relay.h"
#include "lamp_strip.h"
#include "lamparray.h"

#define LOGICAL_MAX_U16 0x27, 0xff, 0xff, 0x00, 0x00
#define LOGICAL_MAX_I32 0x27, 0xff, 0xff, 0xff, 0x7f
#define RGBI 0x09, 0x51, 0x09, 0x52, 0x09, 0x53, 0x09, 0x54

const uint8_t lamparray_report_desc[] = {
    0x05, 0x59, // Usage Page (Lighting and Illumination)
    0x09, 0x01, // Usage (LampArray)
    0xa1, 0x01, // Collection (Application)

    0x85, REPORT_ATTRIBUTES, 0x09, 0x02, 0xa1, 0x02, // LampArrayAttributesReport
    0x09, 0x03, 0x15, 0x00, LOGICAL_MAX_U16, 0x75, 0x10, 0x95, 0x01, 0xb1, 0x03, // LampCount
    0x09, 0x04, 0x09, 0x05, 0x09, 0x06, 0x09, 0x07, 0x09, 0x08, // bounding box, kind, interval
    0x15, 0x00, LOGICAL_MAX_I32, 0x75, 0x20, 0x95, 0x05, 0xb1, 0x03, 0xc0,

    0x85, REPORT_ATTR_REQUEST, 0x09, 0x20, 0xa1, 0x02, // LampAttributesRequestReport
    0x09, 0x21, 0x15, 0x00, LOGICAL_MAX_U16, 0x75, 0x10, 0x95, 0x01, 0xb1, 0x02, 0xc0,

    0x85, REPORT_ATTR_RESPONSE, 0x09, 0x22, 0xa1, 0x02, // LampAttributesResponseReport
    0x09, 0x21, 0x15, 0x00, LOGICAL_MAX_U16, 0x75, 0x10, 0x95, 0x01, 0xb1, 0x02, // LampId
    0x09, 0x23, 0x09, 0x24, 0x09, 0x25, 0x09, 0x27, 0x09, 0x26, // x, y, z, latency, purposes
    0x15, 0x00, LOGICAL_MAX_I32, 0x75, 0x20, 0x95, 0x05, 0xb1, 0x02,
    0x09, 0x28, 0x09, 0x29, 0x09, 0x2a, 0x09, 0x2b, 0x09, 0x2c, 0x09, 0x2d, // level counts, ...
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x06, 0xb1, 0x02, 0xc0,

    0x85, REPORT_MULTI_UPDATE, 0x09, 0x50, 0xa1, 0x02, // LampMultiUpdateReport
    0x09, 0x03, 0x09, 0x55, 0x15, 0x00, 0x25, MULTI_MAX, 0x75, 0x08, 0x95, 0x02, 0xb1, 0x02,
    0x09, 0x21, 0x09, 0x21, 0x09, 0x21, 0x09, 0x21, 0x09, 0x21, 0x09, 0x21, 0x09, 0x21, 0x09, 0x21,
    0x15, 0x00, LOGICAL_MAX_U16, 0x75, 0x10, 0x95, MULTI_MAX, 0xb1, 0x02,
    RGBI, RGBI, RGBI, RGBI, RGBI, RGBI, RGBI, RGBI,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 4 * MULTI_MAX, 0xb1, 0x02, 0xc0,

    0x85, REPORT_RANGE_UPDATE, 0x09, 0x60, 0xa1, 0x02, // LampRangeUpdateReport
    0x09, 0x55, 0x15, 0x00, 0x25, 0x08, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x02,
    0x09, 0x61, 0x09, 0x62, 0x15, 0x00, LOGICAL_MAX_U16, 0x75, 0x10, 0x95, 0x02, 0xb1, 0x02,
    RGBI, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0xb1, 0x02, 0xc0,

    0x85, REPORT_CONTROL, 0x09, 0x70, 0xa1, 0x02, // LampArrayControlReport
    0x09, 0x71, 0x15, 0x00, 0x25, 0x01, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x02, 0xc0,

    0xc0,
};

// Lamps 0-20 are the left's strip, 21-41 the right's, which get their colours over the art relay.
// Positions in micrometres, from the physical layout (1 key unit = 19.05 mm; thumbs unrotated),
// assuming each half's LEDs run under its 3x6 keys row by row, then the thumbs. That order is a
// guess until it's checked on the board (#115).
#define HALF_LAMPS LAMP_STRIP_LEN
#define LAMP_COUNT (2 * HALF_LAMPS)
#define BOX_W 333375
#define BOX_H 78486
#define BOX_D 10000
#define LAMP_PURPOSE_ACCENT 0x02
#define LAMP_ARRAY_KIND_KEYBOARD 1
#define UPDATE_INTERVAL_US 33000
// A Bluetooth write takes a connection event and a frame is 6 of them, so hosts pace slower.
#define UPDATE_INTERVAL_BLE_US 100000
// The right gets at most this many frames a second, so the split link keeps room for keys.
#define RIGHT_FRAME K_MSEC(100)

static const struct {
    int32_t x, y;
    uint8_t key; // keymap position under the lamp
} lamps[] = {
    {9525, 16574, 0},
    {28575, 16574, 1},
    {47625, 11811, 2},
    {66675, 9525, 3},
    {85725, 11811, 4},
    {104775, 14097, 5},
    {9525, 35624, 13},
    {28575, 35624, 14},
    {47625, 30861, 15},
    {66675, 28575, 16},
    {85725, 30861, 17},
    {104775, 33147, 18},
    {9525, 54674, 28},
    {28575, 54674, 29},
    {47625, 49911, 30},
    {66675, 47625, 31},
    {85725, 49911, 32},
    {104775, 52197, 33},
    {76200, 68961, 42},
    {95250, 68961, 43},
    {114300, 68961, 44},
    {228600, 14097, 7},
    {247650, 11811, 8},
    {266700, 9525, 9},
    {285750, 11811, 10},
    {304800, 16574, 11},
    {323850, 16574, 12},
    {228600, 33147, 22},
    {247650, 30861, 23},
    {266700, 28575, 24},
    {285750, 30861, 25},
    {304800, 35624, 26},
    {323850, 35624, 27},
    {228600, 52197, 36},
    {247650, 49911, 37},
    {266700, 47625, 38},
    {285750, 49911, 39},
    {304800, 54674, 40},
    {323850, 54674, 41},
    {219075, 68961, 45},
    {238125, 68961, 46},
    {257175, 68961, 47},
};
BUILD_ASSERT(ARRAY_SIZE(lamps) == LAMP_COUNT, "lamp table doesn't match the strips");

const size_t lamparray_report_desc_len = sizeof(lamparray_report_desc);

static uint16_t requested_lamp;
static atomic_t owner; // the transport in charge, 0 = none (autonomous mode)

static uint8_t right_rgb[HALF_LAMPS][3];

#if IS_ENABLED(CONFIG_NICE_VIEW_ART_RELAY)
static void right_frame_cb(struct k_work *work) {
    art_relay_lamps(atomic_get(&owner) != 0, right_rgb);
}
static K_WORK_DELAYABLE_DEFINE(right_frame_work, right_frame_cb);
#endif

static void show(void) {
    lamp_strip_show();
#if IS_ENABLED(CONFIG_NICE_VIEW_ART_RELAY)
    k_work_schedule(&right_frame_work, RIGHT_FRAME); // coalesces: schedule keeps a pending one
#endif
}

static void set_owner(int t) {
    atomic_set(&owner, t);
    lamp_strip_host(t != 0);
    show();
}

static void set_lamp(uint16_t id, const uint8_t *rgbi) {
    // Intensity scales the colour: ws2812 has no separate brightness channel.
    uint8_t r = rgbi[0] * rgbi[3] / 255, g = rgbi[1] * rgbi[3] / 255, b = rgbi[2] * rgbi[3] / 255;
    if (id < HALF_LAMPS) {
        lamp_strip_set(id, r, g, b);
    } else if (id < LAMP_COUNT) {
        right_rgb[id - HALF_LAMPS][0] = r;
        right_rgb[id - HALF_LAMPS][1] = g;
        right_rgb[id - HALF_LAMPS][2] = b;
    }
}

int lamparray_report_len(uint8_t id) {
    switch (id) {
    case REPORT_ATTRIBUTES:
        return 22;
    case REPORT_ATTR_REQUEST:
        return 2;
    case REPORT_ATTR_RESPONSE:
        return 28;
    case REPORT_MULTI_UPDATE:
        return LAMPARRAY_REPORT_MAX;
    case REPORT_RANGE_UPDATE:
        return 9;
    case REPORT_CONTROL:
        return 1;
    default:
        return 0;
    }
}

int lamparray_get_report(enum lamparray_transport t, uint8_t id, uint8_t *p) {
    uint32_t interval = t == LAMPARRAY_BLE ? UPDATE_INTERVAL_BLE_US : UPDATE_INTERVAL_US;
    memset(p, 0, LAMPARRAY_REPORT_MAX);
    switch (id) {
    case REPORT_ATTRIBUTES:
        sys_put_le16(LAMP_COUNT, &p[0]);
        sys_put_le32(BOX_W, &p[2]);
        sys_put_le32(BOX_H, &p[6]);
        sys_put_le32(BOX_D, &p[10]);
        sys_put_le32(LAMP_ARRAY_KIND_KEYBOARD, &p[14]);
        sys_put_le32(interval, &p[18]);
        return 22;
    case REPORT_ATTR_RESPONSE: {
        uint16_t lamp = requested_lamp;
        sys_put_le16(lamp, &p[0]);
        sys_put_le32(lamps[lamp].x, &p[2]);
        sys_put_le32(lamps[lamp].y, &p[6]);
        sys_put_le32(0, &p[10]);
        sys_put_le32(interval, &p[14]);
        sys_put_le32(LAMP_PURPOSE_ACCENT, &p[18]);
        p[22] = p[23] = p[24] = p[25] = 255; // red, green, blue, intensity levels
        p[26] = 1;                           // programmable
        p[27] = 0;                           // no key binding
        requested_lamp = (lamp + 1) % LAMP_COUNT; // hosts walk the lamps by reading again
        return 28;
    }
    default:
        return -ENOTSUP;
    }
}

int lamparray_set_report(enum lamparray_transport t, uint8_t id, const uint8_t *d, int n) {
    switch (id) {
    case REPORT_ATTR_REQUEST:
        if (n >= 2) {
            requested_lamp = MIN(sys_get_le16(&d[0]), LAMP_COUNT - 1);
        }
        return 0;
    case REPORT_MULTI_UPDATE: {
        if (n < LAMPARRAY_REPORT_MAX) {
            return -EINVAL;
        }
        int count = MIN(d[0], MULTI_MAX);
        for (int i = 0; i < count; i++) {
            set_lamp(sys_get_le16(&d[2 + 2 * i]), &d[2 + 2 * MULTI_MAX + 4 * i]);
        }
        if (d[1] & 0x01) { // LampUpdateComplete
            show();
        }
        return 0;
    }
    case REPORT_RANGE_UPDATE: {
        if (n < 9) {
            return -EINVAL;
        }
        uint16_t start = sys_get_le16(&d[1]), end = MIN(sys_get_le16(&d[3]), LAMP_COUNT - 1);
        for (uint16_t lamp = start; lamp <= end; lamp++) {
            set_lamp(lamp, &d[5]);
        }
        if (d[0] & 0x01) {
            show();
        }
        return 0;
    }
    case REPORT_CONTROL:
        if (n >= 1) {
            set_owner(d[0] ? 0 : t); // autonomous mode off = this host draws
        }
        return 0;
    default:
        return -ENOTSUP;
    }
}

void lamparray_release(enum lamparray_transport t) {
    if (atomic_cas(&owner, t, 0)) {
        set_owner(0);
    }
}

/* USB */

static const struct device *hid_dev;
static uint8_t get_buf[1 + LAMPARRAY_REPORT_MAX];

static int get_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    uint8_t id = setup->wValue & 0xff;
    int n = lamparray_get_report(LAMPARRAY_USB, id, &get_buf[1]);
    if (n < 0) {
        return n;
    }
    get_buf[0] = id;
    *len = 1 + n;
    *data = get_buf;
    return 0;
}

static int set_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    if (*len < 1) {
        return -EINVAL;
    }
    return lamparray_set_report(LAMPARRAY_USB, (*data)[0], *data + 1, *len - 1);
}

static const struct hid_ops ops = {
    .get_report = get_report_cb,
    .set_report = set_report_cb,
};

// Registers the interface before ZMK enables USB, like the Raw HID module does; nothing else.
static int lamparray_init(void) {
    hid_dev = device_get_binding(CONFIG_FOXGLOVE_LAMPARRAY_DEVICE);
    if (hid_dev == NULL) {
        LOG_ERR("LampArray: no %s", CONFIG_FOXGLOVE_LAMPARRAY_DEVICE);
        return -ENODEV;
    }
    usb_hid_register_device(hid_dev, lamparray_report_desc, lamparray_report_desc_len, &ops);
    return usb_hid_init(hid_dev);
}

SYS_INIT(lamparray_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

// Unplugged or suspended: nobody draws any more, so both halves get their own lights back.
static int usb_listener(const zmk_event_t *eh) {
    if (!zmk_usb_is_hid_ready()) {
        lamparray_release(LAMPARRAY_USB);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(lamparray_usb, usb_listener);
ZMK_SUBSCRIPTION(lamparray_usb, zmk_usb_conn_state_changed);
