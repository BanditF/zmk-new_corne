/*
 * PC link: the host panel on the left screen and art pushes to the right half.
 * SPDX-License-Identifier: MIT
 *
 * Raw HID reports are 32 bytes; byte 0 picks the protocol, byte 1 the command. Ours don't collide
 * with KeyPeek (0xFF, 0xF1), VIA (0x01-0x14, 0xFE) or zzeneg's hid host (0xAA-0xB1, 0xCC-0xCD).
 * 0xD7-0xDF are reserved for host-link protocols (0xD7 is settings; 0xD8-0xDF unassigned).
 * Replies come back as input reports with the same byte 0 and byte 1 = command | 0x80.
 *
 * 0xD5 host panel, host -> keyboard:
 *   01 HELLO   ver                  reply: ver, panel w, h, x, y, right link, right battery %,
 *                                   relay discovery result (int8), discovery tries,
 *                                   10 relay debug bytes, capabilities u32 LE (report bytes 21-24),
 *                                   panel timeout seconds u16 LE (bytes 25-26; 0 = never),
 *                                   the left's firmware version major, minor, patch, profile
 *                                   (bytes 27-30; profile 0 dev, 1 GA),
 *                                   split links the #122 guard dropped since boot (byte 31;
 *                                   0 also when CONFIG_FOXGLOVE_SPLIT_GUARD is off)
 *      Capabilities: bit 0 panel, 1 art relay, 2 settings/RGB, 3 unsolicited art STATUS,
 *                    4 settings CONN_STATE, 5 POSITION (only when test hooks are built),
 *                    6 screen snapshots (D7 06/07), 7 INVOKE refuses &bt/&out/&bootloader/
 *                    &sys_reset/&soft_off with status 6 (firmware without it may press them),
 *                    8 unsolicited CONN_STATE (D7 84) on connection changes,
 *                    9 BOOTLOADER (D7 08; only when test hooks are built),
 *                    10 flash art slots (D6 06/07/08), 11 unsolicited RGB_STATE (D7 83),
 *                    12 BUILD_INFO (D7 09; HELLO bytes 27-30), 13 SCREENS (D5 06).
 *      Old firmware pads appended fields with zero; version and prior fields stay unchanged.
 *      After host traffic, a right battery change sends an unsolicited HELLO with the same payload.
 *   02 FILL    colour (0 or 1)      fill the back buffer
 *   03 ROWS    y, n, n rows         write rows of the panel's stride, MSB first, 1 = ink; n up to
 *                                   the panel's rows per report (SCREENS; 3 on the Corne)
 *   04 PRESENT                      show the back buffer
 *   05 RELEASE                      give the area back to the WPM chart now
 *   06 SCREENS index u8             reply D5 86: status (0 ok, 5 index >= count), count, index,
 *                                   descriptor (24 B, #160): id, kind (0 display, 1 panel, 2 art),
 *                                   half, parent id (0xff for a display), width u16, height u16 (as
 *                                   the PC addresses it), x u16, y u16 (on the parent, as seen),
 *                                   rotation (quarter turns cw), format (0 = 1 bit), ops u16 (bit 0 D5
 *                                   rows, 1 D7 06/07 readback, 2 D6 art, 3 D6 slots), stride u16,
 *                                   rows per report, readback half (0xff none), version (1), 0 0 0
 *      The Corne: 0 left display 160x68 rot 1; 1 panel 64x52 at (2,106) on 0 (D5 02-05 address it);
 *      2 right display; 3 right art area 68x140 at (0,20) on 2.
 * The panel takes the area at the first PRESENT and keeps the last presented frame until RELEASE.
 * With CONFIG_NICE_VIEW_HOST_PANEL_TIMEOUT_S > 0, the WPM chart also comes back after that many
 * seconds without a 0xD5 report.
 *
 * 0xD6 right-hand art (see art_relay.h for the blob), host -> keyboard:
 *   01 BEGIN   len u16, crc32 u32   start receiving an art blob on the left
 *   02 DATA    offset u16, n, n bytes (n <= 27)
 *   03 COMMIT                       check the CRC, then push the blob to the right half
 *   04 SELECT  set (int8, -1 = random built-in)  show a built-in set again
 *   05 STATUS
 *   06 SAVE    slot u8, name[16]    save the pushed set now on the right into flash slot n
 *   07 SLOT    slot u8              reply D6 87: status, slot, used u8, writes u32, len u16,
 *                                   name[16] (status 0 ok, 1 busy, 2 no link, 3 BLE error,
 *                                   4 right without slots); comes once the right has answered
 *   08 ERASE   slot u8              empty flash slot n
 *   SAVE and ERASE reply like SELECT; the write then runs on the right, and STATUS shows the
 *   right's state SAVING (6), then SAVED (7) or SAVE_FAILED (8). SELECT set_count + n plays
 *   flash slot n. The right status (version 3) ends with flash_slots u8, flash_used u8 (bits).
 * Every command but DATA replies with the relay status (art_relay_central.c); DATA replies only
 * on error. Once a host has sent any host-link report since boot, relay discovery completion
 * and loss of a ready relay also send 0xD6 0x85, with exactly the STATUS reply payload.
 * These best-effort events use the same reply queue; hosts may still request STATUS to resync.
 *
 * 0xD7 keyboard settings, host -> keyboard; saved on the keyboard, so the PC needn't resend them:
 *   01 RGB_SYNC  0 = off, 1 = on, 0xff = just read     reply: sync (0/1, 0xff = not built in),
 *                                                     left underglow on, right link
 *      On: the right half's underglow follows the left's on/off (default). Off: each half turns
 *      its underglow off on its own idle.
 *   02 INVOKE    behavior local id u16, param1 u32, param2 u32, hold ms u16 (0 = tap)
 *                                                     reply: status, local id u16
 *      Presses and releases the binding as a key would, on the left; allowed global behaviors
 *      (&rgb_ug, ...) reach the right half too. The ids are ZMK Studio's local ids.
 *      hold ms keeps it pressed that long. Status:
 *      0 ok, 1 unknown id, 2 busy (a held invoke is still running), 3 behavior failed,
 *      4 not built in (no local ids), 5 bad length, 6 refused.
 *      Refused before scheduling: &bt (BT_SEL/BT_CLR/BT_CLR_ALL/BT_NXT/BT_PRV/BT_DISC),
 *      &out (all commands), &bootloader, &sys_reset and &soft_off. Matches resolved device
 *      names of enabled zmk,behavior-{bluetooth,outputs,reset,soft-off} nodes, including
 *      renamed instances; all Bluetooth parameters are refused.
 *   03 RGB_STATE                                      reply: left struct rgb_idle_wire (7 B),
 *                                                     right age s (0xff = none), right (7 B)
 *      After host traffic, D7 83 also comes unsolicited ~600 ms after the left's underglow turns
 *      on or off (idle, wake, a key, sync), if left on / right on / right known changed since the
 *      last unsolicited one (capability bit 11).
 *      The right's state comes from the last relay status read (each request asks for a new one),
 *      so poll it; age says how old it is.
 *   04 CONN_STATE  no payload (request D7 04)          reply: D7 84, active BLE profile index u8,
 *                                                     connected (0/1), bonded (0/1),
 *                                                     selected endpoint (0 USB, 1 BLE),
 *                                                     USB HID ready (0/1), BLE profile count u8
 *      Bonded means the active profile is not open. Endpoint is the currently selected transport,
 *      not the preferred transport. Without BLE, index/connected/bonded/count are zero; without
 *      USB, ready is zero. No boot work. After host traffic, a change of the active profile (or
 *      its connection or bond), the endpoint or USB also sends D7 84 unsolicited, ~50 ms after it
 *      settles and only if the payload differs from the last one sent that way (capability bit 8).
 *   05 POSITION  byte 0 D7, byte 1 05, byte 2 pos u8, byte 3 state u8 (1 down, 0 up)
 *                reply: byte 0 D7, byte 1 85, byte 2 status u8, byte 3 pos u8
 *      Positions are keymap indices 0..ZMK_KEYMAP_LEN-1 (48). Remaining report bytes are padding.
 *      Status: 0 ok, 2 busy/no relay link, 3 relay timeout (3 s), 4 not built in,
 *      5 short payload/invalid position/state, 6 refused (same devices as INVOKE).
 *      Active layers resolve top-down through &trans, stopping at the default layer.
 *      Left events run on the system queue; right events go over ART_RELAY_PRESS then
 *      return through ZMK's normal split link. Duplicate downs and unowned ups are no-op ok.
 *      Owned downs release after 10 s or after 10 s without a host-link report.
 *      CONFIG_FOXGLOVE_TEST_HOOKS=n omits the handler and returns status 4.
 *      Replies use the existing nonblocking queue and are zero-padded to the report size.
 *   06 SCREEN_SNAP half u8 (0 left, 1 right), request bytes 0..2 = D7 06 half.
 *      Reply bytes: 0 D7, 1 86, 2 status, 3 half, 4..5 frame u16, 6 w (160), 7 h (68),
 *                   8..9 bytes u16 (1360), 10..13 crc32 u32. All multibyte fields are LE.
 *      Metadata is zero on error. Frame increments per captured image (wraps at 65536).
 *      CRC is crc32_ieee over the 1360 image bytes. Right reply is asynchronous, after relay
 *      transfer and CRC verification; USB never waits for the other half.
 *   07 SCREEN_READ half u8, offset u16 LE, request bytes 0..4 = D7 07 half offset.
 *      Reply: 0 D7, 1 87, 2 status, 3 half, 4..5 echoed offset u16 LE, 6 n u8, 7.. data[n].
 *      n = min(1360-offset, report size-7); offset=1360 succeeds with n=0; larger is invalid.
 *      Reads the frozen local cache, including for half 1. Error replies have n=0.
 *      Status (both): 0 ok, 1 busy (retry), 2 no right relay/screen link, 3 right timeout
 *      (3 seconds including queue wait), 4 not built in, 5 bad request, 6 no snapshot yet (READ).
 *      Capture/transfer makes READ busy. A failed partial transfer invalidates the cache;
 *      a request rejected before copying pixels preserves the previous snapshot.
 *      Image: native 160x68 panel coordinates, row-major, 20 bytes/row, MSB first, 1=black ink,
 *      0=white, after widget inversion. Native top is y=0. On both portrait-mounted halves,
 *      rotate the returned image 90 degrees counterclockwise for the person's 68x160 view:
 *      portrait (x,y) = (native y,159-native x). Battery/status is at the portrait top.
 *      Left joins the three already rotated TRUE_COLOR canvases (including the displayed host
 *      panel). Right overlays the INDEXED_1BIT art, using its palette, on the status canvas.
 *      Both captures run on zmk_display_work_q(), without touching LVGL from USB/BT RX.
 *   09 BUILD_INFO  half u8 (0 left, 1 right)    reply: D7 89, status, half, major u8, minor u8,
 *                                                patch u8, profile u8 (0 dev, 1 GA), sha[8]
 *      sha is the short git SHA in ASCII, zero-padded. Status: 0 ok, 1 busy, 2 no relay link,
 *      3 relay error/timeout (3 s), 4 right firmware without it, 5 bad half. The right's comes
 *      with an offset read of the relay status value (after the slot table); the reply waits.
 *      The version is FIRMWARE_VERSION (SemVer); release tags are fw-vX.Y.Z.
 *   0A LAMP_TEST  half u8, index u8 (0xff = end), r, g, b    reply: D7 8A, status, half, index
 *      Test hooks + LampArray builds only (#115): lights lamp index of that half alone, so a person
 *      can name the key above it; index 0xff gives the half its own lights back. The right's goes
 *      over the art relay. Status: 0 ok, 2 no relay link, 4 not built in, 5 bad request.
 *   08 BOOTLOADER (test hooks only), two steps so one stray report can't enter a bootloader:
 *      ARM      request D7 08 00                  reply D7 88, status, nonce u32 LE
 *      TRIGGER  request D7 08 01 half nonce u32   reply D7 88, status, half
 *      ARM makes a fresh random nonce (never 0), valid for 5 s and for one TRIGGER: any TRIGGER,
 *      good or bad, uses it up. half 0 = left: the reply goes out, then the left reboots into its
 *      UF2 bootloader ~300 ms later. half 1 = right: the left relays art op BOOT; the reply comes
 *      once the right has answered, and the right reboots into its bootloader ~300 ms after.
 *      Status: 0 ok, 2 no relay link (right), 3 relay timeout (3 s), 4 not built in
 *      (CONFIG_FOXGLOVE_TEST_HOOKS=n, GA), 5 bad request, 6 bad or expired nonce.
 *   0B GUARD_TEST (test hooks + split guard only)        reply: D7 8B, status
 *      The #122 split guard treats the current split link as stuck once, 2 s from now: it drops
 *      it, ZMK reconnects, HELLO byte 31 counts one more. Status: 0 ok, 2 no split link,
 *      4 not built in (GA, or no guard).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HOST_LINK_PANEL 0xD5
#define HOST_LINK_ART 0xD6
#define HOST_LINK_SETTINGS 0xD7
#define HOST_LINK_VERSION 1

#define HOST_LINK_CAP_PANEL (1U << 0)
#define HOST_LINK_CAP_ART_RELAY (1U << 1)
#define HOST_LINK_CAP_SETTINGS_RGB (1U << 2)
#define HOST_LINK_CAP_ART_STATUS_EVENT (1U << 3)
#define HOST_LINK_CAP_CONN_STATE (1U << 4)
#define HOST_LINK_CAP_POSITION (1U << 5)
#define HOST_LINK_CAP_SCREEN (1U << 6) // set by the screen readback build
#define HOST_LINK_CAP_INVOKE_REFUSES (1U << 7)
#define HOST_LINK_CAP_CONN_STATE_EVENT (1U << 8)
#define HOST_LINK_CAP_BOOTLOADER (1U << 9) // D7 08, test hooks only
#define HOST_LINK_CAP_ART_SLOTS (1U << 10)  // D6 06/07/08 flash slots on the right
#define HOST_LINK_CAP_RGB_STATE_EVENT (1U << 11)
#define HOST_LINK_CAP_BUILD_INFO (1U << 12)
#define HOST_LINK_CAP_SCREENS (1U << 13)

// Nonblocking POSITION response; also used by the test-hooks worker.
void host_link_position_reply(uint8_t status, uint8_t pos);
// BOOTLOADER TRIGGER reply for the right half, once the relay has answered.
void host_link_bootloader_reply(uint8_t status, uint8_t half);

// Queues a report to the PC (nonblocking; dropped if the queue is full).
void host_link_send(const uint8_t *data, int len);

struct foxglove_build_info;
// D7 89 BUILD_INFO reply; build may be NULL on errors.
void host_link_build_info_reply(uint8_t status, uint8_t half,
                                const struct foxglove_build_info *build);

// rgb_idle.c, on the left: the underglow went on or off.
void host_link_rgb_changed(void);

// Called by relay connection/discovery callbacks; queues a report only after host traffic.
void host_link_art_status(void);

// The panel covers the WPM block of the portrait layout (status.c), inside the frame.
// A board with a panel of its own sets CONFIG_FOXGLOVE_PANEL_* (foxglove_link shield).
#if defined(CONFIG_FOXGLOVE_PANEL_W)
#define HOST_PANEL_X CONFIG_FOXGLOVE_PANEL_X
#define HOST_PANEL_Y CONFIG_FOXGLOVE_PANEL_Y
#define HOST_PANEL_W CONFIG_FOXGLOVE_PANEL_W
#define HOST_PANEL_H CONFIG_FOXGLOVE_PANEL_H
#else
#define HOST_PANEL_X 2
#define HOST_PANEL_Y 106
#define HOST_PANEL_W 64
#define HOST_PANEL_H 52
#endif
#define HOST_PANEL_STRIDE (HOST_PANEL_W / 8)
#define HOST_PANEL_BYTES (HOST_PANEL_STRIDE * HOST_PANEL_H)

/* D5 06 SCREENS (#160): each board's build supplies foxglove_screens[] (screens_corne.c,
 * foxglove_link/screens_*.c). Layout in the 0xD5 comment above. */
enum { SCREEN_DISPLAY, SCREEN_PANEL, SCREEN_ART };
enum { SCREEN_OP_ROWS = 1 << 0, SCREEN_OP_READBACK = 1 << 1, SCREEN_OP_ART = 1 << 2,
       SCREEN_OP_SLOTS = 1 << 3 };
#define SCREEN_DESCRIPTOR_VERSION 1
#define SCREEN_UNSET 0xff

struct screen_descriptor {
    uint8_t id, kind, half, parent;
    uint16_t width, height; // as the PC addresses the region
    uint16_t x, y;          // on the parent display, as seen; 0 for a display
    uint8_t rotation;       // region -> parent as seen (displays: native -> as seen), quarter turns
    uint8_t format;         // 0 = 1 bit, MSB first, 1 = ink as seen
    uint16_t ops;
    uint16_t stride;
    uint8_t rows_per_report;
    uint8_t readback_half; // D7 06's half for a display, 0xff otherwise
    uint8_t version;
    uint8_t reserved[3];
} __attribute__((packed));

extern const struct screen_descriptor foxglove_screens[];
extern const uint8_t foxglove_screen_count;

// Copies the shown panel into buf and returns true while the panel holds the area.
bool host_panel_get(uint8_t buf[HOST_PANEL_BYTES]);

// Redraws the left screen; status.c, called on the display work queue.
void host_panel_redraw(void);
