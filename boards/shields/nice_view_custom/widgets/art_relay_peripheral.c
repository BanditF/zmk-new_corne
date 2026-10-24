/*
 * Right half: takes an art set pushed over the split link (art_relay.h) and plays it from RAM.
 * SPDX-License-Identifier: MIT
 *
 * Two slots: one holds the pushed set on screen (if any), the other takes the next push. The
 * screen only changes once a blob has fully arrived and passed its CRC and header checks, so a
 * push that fails, stops halfway or never commits leaves whatever is playing alone.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#if IS_ENABLED(CONFIG_NICE_VIEW_RGB_IDLE)
#include <zephyr/bluetooth/conn.h>

#include "rgb_idle.h"
#endif

#include "art_relay.h"
#if IS_ENABLED(CONFIG_FOXGLOVE_LAMP_STRIP)
#include "lamp_strip.h"
#endif
#include "screen_snap.h"
#if IS_ENABLED(CONFIG_FOXGLOVE_TEST_HOOKS)
#include "test_hooks.h"
#endif
#include "art_slots.h"
#include "half_marker.h"
#include "custom_art.h"
#include "peripheral_status.h"

static uint8_t slots[2][CONFIG_NICE_VIEW_ART_RELAY_MAX_SIZE];
static int shown = -1; // slot playing, or -1 for a built-in set
static int rx;         // slot being received into; never the one shown
static uint16_t rx_len;
static uint16_t slot_len[2]; // blob length in each RAM slot, once committed
static int flash_shown = -1;  // flash slot playing, or -1
static uint32_t rx_crc;

static struct art_relay_status status = {
    .version = 4,
    .capacity = CONFIG_NICE_VIEW_ART_RELAY_MAX_SIZE,
    .set = -1,
};

static const uint8_t *ram_deltas[2][256];
static struct custom_art_set ram_sets[2] = {
    {.name = "pushed", .deltas = ram_deltas[0]},
    {.name = "pushed", .deltas = ram_deltas[1]},
};
static const uint8_t *flash_deltas[256];
static struct custom_art_set flash_set = {.name = "saved", .deltas = flash_deltas};

// Fills set (and its deltas) from blob b; false, with set untouched, if it doesn't hold together.
static bool parse(const uint8_t *b, uint16_t slot_len, const uint8_t **deltas,
                  struct custom_art_set *set) {
    if (slot_len < ART_RELAY_HEADER_SIZE || memcmp(b, ART_RELAY_MAGIC, 4) != 0 ||
        b[4] != CUSTOM_ART_W || b[5] != CUSTOM_ART_H) {
        return false;
    }
    int unique = sys_get_le16(&b[8]);
    int frames = sys_get_le16(&b[10]);
    int deltas_at = ART_RELAY_HEADER_SIZE + frames + 2 * unique;
    if (unique < 1 || unique > ARRAY_SIZE(flash_deltas) || frames < 1 || deltas_at >= slot_len) {
        return false;
    }
    const uint8_t *order = &b[ART_RELAY_HEADER_SIZE];
    for (int i = 0; i < frames; i++) {
        if (order[i] >= unique) {
            return false;
        }
    }
    for (int i = 0; i < unique; i++) {
        int at = sys_get_le16(&b[ART_RELAY_HEADER_SIZE + frames + 2 * i]);
        if (at < deltas_at || at >= slot_len) {
            return false;
        }
    }
    for (int i = 0; i < unique; i++) { // checked above; the set may be on screen, so fill last
        deltas[i] = &b[sys_get_le16(&b[ART_RELAY_HEADER_SIZE + frames + 2 * i])];
    }
    set->order = order;
    set->frame_count = frames;
    set->fps = b[6];
    return true;
}

// Fills ram_sets[rx] from the blob in slots[rx].
static bool parse_blob(void) { return parse(slots[rx], rx_len, ram_deltas[rx], &ram_sets[rx]); }

static int8_t select_set;

static void play_cb(struct k_work *work) {
    if (flash_shown >= 0) {
        art_play(&flash_set);
    } else if (shown >= 0) {
        art_play(&ram_sets[shown]);
    } else {
        status.set = art_builtin_index(select_set);
        art_play(&custom_art_sets[status.set]);
    }
}
static K_WORK_DEFINE(play_work, play_cb);

static void play(void) {
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &play_work);
    }
}

#if IS_ENABLED(CONFIG_NICE_VIEW_RGB_IDLE)
static void rgb_disconnected(struct bt_conn *conn, uint8_t reason) {
    rgb_idle_remote_lost();
#if IS_ENABLED(CONFIG_FOXGLOVE_LAMP_STRIP)
    lamp_strip_host(false); // no PC to draw any more: the right's own lights come back
#endif
}

BT_CONN_CB_DEFINE(art_relay_rgb_conn_cb) = {
    .disconnected = rgb_disconnected,
};
#endif

// From art_slots.c's queue when a SAVE or ERASE has finished.
static void save_done(int err) { status.state = err ? ART_RIGHT_SAVE_FAILED : ART_RIGHT_SAVED; }

// Runs in the Bluetooth RX thread; keep it to copying and checking.
static ssize_t data_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                          uint16_t len, uint16_t offset, uint8_t flags) {
    const uint8_t *d = buf;
    if (offset != 0 || len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    switch (d[0]) {
    case ART_RELAY_LAMPS:
#if IS_ENABLED(CONFIG_FOXGLOVE_LAMP_STRIP)
        if (len < 3 || len != 3 + 3 * d[2]) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        for (int i = 0; i < d[2]; i++) {
            lamp_strip_set(d[1] + i, d[3 + 3 * i], d[4 + 3 * i], d[5 + 3 * i]);
        }
        lamp_strip_show();
#endif
        break;

    case ART_RELAY_LAMP_HOST:
        if (len != 2) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
#if IS_ENABLED(CONFIG_FOXGLOVE_LAMP_STRIP)
        lamp_strip_host(d[1] != 0);
#else
        return BT_GATT_ERR(0x84);
#endif
        break;

    case ART_RELAY_BOOT:
#if IS_ENABLED(CONFIG_FOXGLOVE_TEST_HOOKS)
        if (len != 5 || memcmp(&d[1], "BOOT", 4) != 0) {
            return BT_GATT_ERR(0x85);
        }
        test_hooks_reboot_soon(); // after this write's response has gone out
        break;
#else
        return BT_GATT_ERR(0x84);
#endif
    case ART_RELAY_PRESS:
#if IS_ENABLED(CONFIG_FOXGLOVE_TEST_HOOKS)
        if (len != 3) {
            return BT_GATT_ERR(0x85);
        }
        uint8_t result = test_hooks_press(d[1], d[2]);
        if (result) {
            return BT_GATT_ERR(0x80 + result);
        }
        break;
#else
        return BT_GATT_ERR(0x84);
#endif
    case ART_RELAY_BEGIN:
        if (len < 7) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        if (status.state == ART_RIGHT_SAVING) {
            return BT_GATT_ERR(0x82); // the shown slot is being copied to flash
        }
        // The screen keeps playing; this only fills the slot that isn't shown.
        rx = shown == 0 ? 1 : 0;
        rx_len = sys_get_le16(&d[1]);
        rx_crc = sys_get_le32(&d[3]);
        status.received = 0;
        status.state = rx_len <= sizeof(slots[0]) ? ART_RIGHT_RECEIVING : ART_RIGHT_TOO_BIG;
        break;

    case ART_RELAY_DATA: {
        if (status.state != ART_RIGHT_RECEIVING || len < 3) {
            break;
        }
        uint16_t at = sys_get_le16(&d[1]);
        uint16_t n = len - 3;
        if (at + n <= rx_len) {
            memcpy(&slots[rx][at], &d[3], n);
            status.received += n;
        }
        break;
    }

    case ART_RELAY_COMMIT:
        if (status.state != ART_RIGHT_RECEIVING) {
            break;
        }
        if (status.received != rx_len || crc32_ieee(slots[rx], rx_len) != rx_crc) {
            status.state = ART_RIGHT_BAD_CRC;
        } else if (!parse_blob()) {
            status.state = ART_RIGHT_BAD_BLOB;
        } else {
            // Freeze the old set until play_cb switches over, so the next push can take its slot.
            art_hold();
            shown = rx;
            slot_len[rx] = rx_len;
            flash_shown = -1;
            status.state = ART_RIGHT_PLAYING;
            status.set = -1;
            LOG_INF("art relay: playing pushed set, %d frames", ram_sets[shown].frame_count);
            play();
        }
        break;

    case ART_RELAY_SELECT:
        if (len < 2) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        if (status.state == ART_RIGHT_SAVING) {
            return BT_GATT_ERR(0x82);
        }
        select_set = (int8_t)d[1];
        if (select_set >= custom_art_set_count &&
            select_set < custom_art_set_count + art_slots_count()) {
            uint16_t blob_len;
            const uint8_t *blob = art_slots_blob(select_set - custom_art_set_count, &blob_len);
            art_hold(); // flash_set may be on screen while parse fills it
            if (blob == NULL || !parse(blob, blob_len, flash_deltas, &flash_set)) {
                status.state = ART_RIGHT_BAD_BLOB;
                play(); // carry on with whatever was showing
                break;
            }
            flash_shown = select_set - custom_art_set_count;
            shown = -1;
            status.set = select_set;
            status.state = ART_RIGHT_IDLE;
            play();
            break;
        }
        art_hold(); // as for COMMIT: a push may reuse the shown slot before play_cb runs
        shown = -1;
        flash_shown = -1;
        status.state = ART_RIGHT_IDLE;
        play();
        break;

    case ART_RELAY_SAVE:
        if (len != 18) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        if (status.state == ART_RIGHT_SAVING) {
            return BT_GATT_ERR(0x82);
        }
        if (shown < 0 || d[1] >= art_slots_count()) {
            return BT_GATT_ERR(0x85); // nothing pushed to save, or no such slot
        }
        status.state = ART_RIGHT_SAVING;
        if (art_slots_save(d[1], slots[shown], slot_len[shown], (const char *)&d[2], save_done)) {
            status.state = ART_RIGHT_SAVE_FAILED;
        }
        break;

    case ART_RELAY_ERASE:
        if (len != 2) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        if (status.state == ART_RIGHT_SAVING) {
            return BT_GATT_ERR(0x82);
        }
        if (d[1] >= art_slots_count()) {
            return BT_GATT_ERR(0x85);
        }
        if (flash_shown == d[1]) { // don't play from pages about to be erased
            art_hold();
            flash_shown = -1;
            select_set = -1;
            play();
        }
        status.state = ART_RIGHT_SAVING;
        if (art_slots_erase(d[1], save_done)) {
            status.state = ART_RIGHT_SAVE_FAILED;
        }
        break;

    case ART_RELAY_SNAP:
        if (len != 1) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        if (screen_snap_local() != SCREEN_OK) {
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }
        break;

    case ART_RELAY_RGB:
        if (len < 2) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
#if IS_ENABLED(CONFIG_NICE_VIEW_RGB_IDLE)
        rgb_idle_remote(d[1] != 0, len < 3 || d[2] != 0);
#endif
        break;
    }
    return len;
}

static ssize_t status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                           uint16_t len, uint16_t offset) {
    status.set_count = custom_art_set_count;
#if IS_ENABLED(CONFIG_NICE_VIEW_RGB_IDLE)
    rgb_idle_state(&status.rgb);
#endif
    status.flash_slots = art_slots_count();
    status.flash_used = art_slots_used();
    // Status, then the slot table (only built when a read reaches past the status).
    // Status, then the slot table, then the build info.
    static uint8_t value[sizeof(status) + ART_RELAY_FLASH_SLOTS * sizeof(struct art_relay_slot_info) +
                         sizeof(struct foxglove_build_info)];
    memcpy(value, &status, sizeof(status));
    if (offset + len > sizeof(status)) {
        for (int i = 0; i < ART_RELAY_FLASH_SLOTS; i++) {
            struct art_relay_slot_info info;
            art_slots_info(i, &info);
            memcpy(&value[sizeof(status) + i * sizeof(info)], &info, sizeof(info));
        }
        struct foxglove_build_info build;
        foxglove_build_info(&build);
        memcpy(&value[sizeof(value) - sizeof(build)], &build, sizeof(build));
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static ssize_t screen_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                           uint16_t len, uint16_t offset) {
    if (offset > SCREEN_SNAP_WIRE) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    int n = screen_snap_wire_read(buf, len, offset);
    return n < 0 ? BT_GATT_ERR(BT_ATT_ERR_UNLIKELY) : n;
}

BT_GATT_SERVICE_DEFINE(
    art_relay_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ART_RELAY_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ART_RELAY_DATA_UUID),
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE_ENCRYPT, NULL, data_write, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ART_RELAY_STATUS_UUID), BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT, status_read, NULL, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ART_RELAY_SCREEN_UUID), BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT, screen_read, NULL, NULL), );
