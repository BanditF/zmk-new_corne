/*
 * This half's underglow strip under the PC's control (HID LampArray, #88), on either half.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>

#define LAMP_STRIP_LEN DT_PROP(DT_CHOSEN(zmk_underglow), chain_length)

// on: ZMK's animation and rgb_idle step aside and lamp_strip_show() draws; off hands back.
void lamp_strip_host(bool on);
void lamp_strip_set(int index, uint8_t r, uint8_t g, uint8_t b);
// Writes the strip soon (ZMK's low-priority queue). Any thread.
void lamp_strip_show(void);
