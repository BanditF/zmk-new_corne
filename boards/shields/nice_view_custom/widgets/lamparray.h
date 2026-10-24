/* SPDX-License-Identifier: MIT */
// The LampArray's reports without their transport (lamparray.c), so USB (lamparray.c) and
// Bluetooth (lamparray_hog.c, FOXGLOVE_LAMPARRAY_BLE) share one LampArray (#88).
#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    REPORT_ATTRIBUTES = 1,
    REPORT_ATTR_REQUEST,
    REPORT_ATTR_RESPONSE,
    REPORT_MULTI_UPDATE,
    REPORT_RANGE_UPDATE,
    REPORT_CONTROL,
};
#define LAMPARRAY_REPORTS REPORT_CONTROL

#define MULTI_MAX 8
// The largest report payload (without its report ID): LampMultiUpdate.
#define LAMPARRAY_REPORT_MAX (2 + 2 * MULTI_MAX + 4 * MULTI_MAX)

enum lamparray_transport { LAMPARRAY_USB = 1, LAMPARRAY_BLE = 2 };

extern const uint8_t lamparray_report_desc[];
extern const size_t lamparray_report_desc_len;

// A report's payload size without its ID, 0 for an unknown ID.
int lamparray_report_len(uint8_t id);

// Fills out (LAMPARRAY_REPORT_MAX bytes) with a get-feature report's payload: its length, or
// -ENOTSUP. Reading the attributes response moves on to the next lamp.
int lamparray_get_report(enum lamparray_transport t, uint8_t id, uint8_t *out);

// A set-feature report's payload: 0, -EINVAL (too short) or -ENOTSUP. Turning autonomous mode
// off makes t the transport in charge.
int lamparray_set_report(enum lamparray_transport t, uint8_t id, const uint8_t *d, int n);

// The transport went away (unplugged, disconnected, another profile): if it was in charge, both
// halves get their own lights back.
void lamparray_release(enum lamparray_transport t);
