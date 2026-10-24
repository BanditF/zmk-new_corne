/*
 * Underglow idle auto-off that survives a reboot, and the right half following the left's.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

// One half's underglow, as the PC sees it (0xD7 03) and as the right reports it (art_relay.h).
struct rgb_idle_wire {
    uint8_t on;
    uint16_t hue;       // 0-359
    uint8_t saturation; // 0-100
    uint8_t brightness; // 0-100
    uint8_t effect;     // 0 solid, 1 breathe, 2 spectrum, 3 swirl
    uint8_t speed;      // 1-5, as last saved (ZMK has no getter; lags a minute after a change)
} __packed;

// This half's underglow. Any thread.
void rgb_idle_state(struct rgb_idle_wire *out);

// Left: whether the right half's underglow follows the left's (persisted, default on).
bool rgb_idle_sync(void);
void rgb_idle_set_sync(bool sync);

// Right: the left's underglow state, from the art relay link. follow = false hands the right its
// own idle auto-off back.
void rgb_idle_remote(bool on, bool follow);
// Left: the PC drives the LEDs (HID LampArray); idle and sync step aside until it hands back.
void rgb_idle_host_control(bool on);

// Right: the split link dropped; go by the right's own activity until the left is back.
void rgb_idle_remote_lost(void);
