/*
 * Right-hand art pushed from the PC: PC -> Raw HID -> left -> BLE split link -> right.
 * SPDX-License-Identifier: MIT
 *
 * The right half adds its own GATT service to the split connection:
 *   data   (write, write without response)  op byte + payload, below
 *   screen (encrypted read) frame u16 LE, crc32 u32 LE, 1360 native screen bytes
 *      SNAP (op 7, one byte, write request) queues capture; screen reads return ATT unlikely
 *      until ready. Long reads use offsets into the frozen image; CRC excludes the 6-byte header.
 *   status (read)                            struct art_relay_status
 * Ops: BEGIN len u16, crc32 u32 | DATA offset u16, bytes | COMMIT | SELECT int8 set |
 *      RGB on u8, follow u8 [, more] | PRESS position u8, state u8
 * PRESS is exactly three bytes: 06, global keymap position, state (1 down, 0 up).
 * It uses a write request on relay_q with a job_conn() reference and a 3 s timeout.
 * The peripheral queues an event on the system queue; acceptance is the ATT reply.
 * Application ATT errors: 82 busy, 84 hooks not built in, 85 invalid length/position/state.
 * These map to POSITION statuses 2, 4, 5; ATT success maps to 0 and timeout to 3.
 * RGB (write request, so the left knows it arrived) is the left's underglow on/off; with follow = 1 the right shows
 * the same, with 0 it goes by its own activity (rgb_idle.c). Bytes after follow are reserved for
 * colour and effect; the right ignores bytes it doesn't know, so the op can grow.
 * The right half receives into a spare slot and keeps playing what it shows until a blob passes
 * its CRC and header checks; a failed or abandoned push leaves the screen alone.
 *
 * Blob, all little endian (scripts/flipper_art.py writes it for OUT.bin):
 *   "NVA1", w u8 (140), h u8 (68), fps u8, 0, unique u16, frames u16
 *   order u8[frames]       play order, as indices into the deltas
 *   offset u16[unique]     where each delta starts, from the start of the blob
 *   deltas                 RLE-packed XOR deltas, as in custom_art.c
 */

#pragma once

#include <stdint.h>

#include <zephyr/bluetooth/uuid.h>
#include <zephyr/toolchain.h>

#include "rgb_idle.h"

#define ART_RELAY_SERVICE_UUID BT_UUID_128_ENCODE(0x6e766172, 0x7400, 0x4c6b, 0x9a10, 0x5d1e0bfa7a00)
#define ART_RELAY_DATA_UUID BT_UUID_128_ENCODE(0x6e766172, 0x7401, 0x4c6b, 0x9a10, 0x5d1e0bfa7a00)
#define ART_RELAY_SCREEN_UUID \
    BT_UUID_128_ENCODE(0x6e766172, 0x7403, 0x4c6b, 0x9a10, 0x5d1e0bfa7a00)
#define ART_RELAY_STATUS_UUID BT_UUID_128_ENCODE(0x6e766172, 0x7402, 0x4c6b, 0x9a10, 0x5d1e0bfa7a00)

enum art_relay_op {
    ART_RELAY_BEGIN = 1,
    ART_RELAY_DATA,
    ART_RELAY_COMMIT,
    ART_RELAY_SELECT,
    ART_RELAY_RGB,
    ART_RELAY_PRESS, // 6: keymap position u8, state u8 (1 down, 0 up)
    ART_RELAY_SNAP = 7, // one byte, write request: take a screen snapshot
    ART_RELAY_SAVE = 9,  // slot u8, name[16]: save the shown pushed set to flash (write request)
    ART_RELAY_ERASE = 10, // slot u8: empty a flash slot (write request)
    ART_RELAY_LAMPS = 11, // start u8, n u8, n x (r, g, b): host colours (write without response)
    ART_RELAY_LAMP_HOST = 12, // on u8: the PC drives the right's LEDs (write request)
    ART_RELAY_BOOT = 8, // "BOOT", write request: reboot into the bootloader (test hooks only)
};

#define ART_RELAY_HEADER_SIZE 12
#define ART_RELAY_MAGIC "NVA1"

enum art_relay_right_state {
    ART_RIGHT_IDLE,      // showing a built-in set
    ART_RIGHT_RECEIVING,
    ART_RIGHT_PLAYING,   // showing the pushed set
    ART_RIGHT_BAD_CRC,
    ART_RIGHT_BAD_BLOB,
    ART_RIGHT_TOO_BIG,
    ART_RIGHT_SAVING,      // a flash slot is being written or erased
    ART_RIGHT_SAVED,
    ART_RIGHT_SAVE_FAILED,
};

struct art_relay_status {
    uint8_t version;
    uint8_t state;     // enum art_relay_right_state
    uint16_t received; // bytes of the current blob received so far
    uint16_t capacity; // CONFIG_NICE_VIEW_ART_RELAY_MAX_SIZE
    int8_t set;        // built-in set shown, or -1 for the pushed one
    uint8_t set_count; // built-in sets
    // version >= 2
    struct rgb_idle_wire rgb; // the right's underglow
    // version >= 3: flash slots (art_slots.c); set = set_count + n while slot n is shown
    uint8_t flash_slots;
    uint8_t flash_used; // bit n: slot n holds a set
    // version >= 4: struct foxglove_build_info comes after the slot table (D7 09 for half 1)
} __packed;

// The status characteristic carries ART_RELAY_FLASH_SLOTS of these after struct art_relay_status;
// the left reads one with an offset read (D6 07 SLOT). Normal status reads stop after the first
// chunk, so they cost no more than before.
#define ART_RELAY_FLASH_SLOTS 8
struct art_relay_slot_info {
    uint8_t used;
    uint32_t writes; // erase/write cycles so far
    uint16_t len;    // blob bytes
    char name[16];   // not NUL-terminated when 16 long
} __packed;

// Left half (art_relay_central.c). The blob is collected on the left, then pushed in one go.
// A receive that sees no BEGIN or DATA for ART_RELAY_RECEIVE_TIMEOUT goes back to idle.
#define ART_RELAY_RECEIVE_TIMEOUT K_SECONDS(10)
enum art_relay_left_state {
    ART_LEFT_IDLE,
    ART_LEFT_RECEIVING, // from the PC
    ART_LEFT_PUSHING,   // to the right half
    ART_LEFT_DONE,
    ART_LEFT_FAILED,
};

enum art_relay_error {
    ART_ERR_NONE,
    ART_ERR_NO_LINK,   // right half not connected, or its firmware has no relay service
    ART_ERR_TOO_BIG,
    ART_ERR_BUSY,      // a push is still running
    ART_ERR_RANGE,     // DATA outside the announced length
    ART_ERR_BAD_CRC,   // what the PC sent doesn't match its CRC
    ART_ERR_BLE,       // a write to the right half failed
    ART_ERR_RIGHT,     // the right half rejected the blob; see its status
    ART_ERR_TIMEOUT,   // the PC stopped sending mid-blob; the partial blob was dropped
};

int art_relay_begin(uint16_t len, uint32_t crc);
int art_relay_data(uint16_t offset, const uint8_t *data, uint8_t len);
int art_relay_commit(void);
int art_relay_select(int8_t set);
// How finding the right half's relay service went; negative = a bt_gatt_discover error.
enum art_relay_discovery {
    ART_DISCOVERY_IDLE,
    ART_DISCOVERY_RUNNING,
    ART_DISCOVERY_NO_SERVICE,
    ART_DISCOVERY_NO_CHRC,
    ART_DISCOVERY_DONE,
};
void art_relay_discovery(int8_t *result, uint8_t *tries);
// Counters for debugging the link: chrcs seen, discoveries finished, last request return
// value, last ATT error, timeouts, callbacks, request in flight, security level, MTU u16.
#define ART_RELAY_DEBUG_SIZE 10
void art_relay_debug(uint8_t out[ART_RELAY_DEBUG_SIZE]);
// 0 = right half not connected, 1 = connected, 2 = connected and has the relay service.
uint8_t art_relay_link(void);
// Sends the left's underglow state and sync setting to the right soon, if they changed.
void art_relay_rgb_kick(void);
// The right's underglow from the last status read; returns its age in seconds (capped at 254), or
// 0xff if there is none (no link yet, or right firmware without it). Asks for a fresher read.
uint8_t art_relay_right_rgb(struct rgb_idle_wire *out);
// Fills out with: state, error, length u16, pushed u16, link, struct art_relay_status.
#define ART_RELAY_REPORT_SIZE (7 + sizeof(struct art_relay_status))
void art_relay_report(uint8_t out[ART_RELAY_REPORT_SIZE]);
// Flash slots on the right (D6 06/07/08). save/erase report like select; slot replies D6 87 later.
int art_relay_save(uint8_t slot, const uint8_t name[16]);
int art_relay_erase(uint8_t slot);
int art_relay_slot(uint8_t slot);
// D7 09 for the right: its build info, by an offset read; replies D7 89 later.
int art_relay_build_info(void);

#if IS_ENABLED(CONFIG_FOXGLOVE_TEST_HOOKS)
void art_relay_press(uint8_t pos, uint8_t state);
// Asks the right half to reboot into its bootloader; answers test_hooks_boot_relay_done().
void art_relay_boot(void);
// LampArray: hands the right's strip to the PC (or back) and sends its colours, if the link is
// up. Copies rgb; runs on the relay queue.
void art_relay_lamps(bool host, const uint8_t rgb[][3]);
#endif
