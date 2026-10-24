/*
 * This half's underglow strip under the PC's control (lamp_strip.h).
 * SPDX-License-Identifier: MIT
 *
 * Strip writes run on ZMK's low-priority queue, the one ZMK's own underglow off work uses, so a
 * hand-over (ZMK's off zeroes the strip) always lands before the first host frame.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>

#include <drivers/ext_power.h>
#include <zmk/rgb_underglow.h>
#include <zmk/workqueue.h>

#include "lamp_strip.h"
#include "rgb_idle.h"

static const struct device *const strip = DEVICE_DT_GET(DT_CHOSEN(zmk_underglow));
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));

static struct led_rgb pixels[LAMP_STRIP_LEN];
static atomic_t want_host;
static bool host; // only touched on the low-priority queue

static void strip_cb(struct k_work *work) {
    bool want = atomic_get(&want_host);
    if (want != host) {
        host = want;
        rgb_idle_host_control(want); // off: rgb_idle re-applies the user's on/off
        if (want) {
            zmk_rgb_underglow_off(); // stops ZMK's animation; the LEDs need their power back
            if (device_is_ready(ext_power)) {
                ext_power_enable(ext_power);
            }
        }
    }
    if (host) {
        led_strip_update_rgb(strip, pixels, LAMP_STRIP_LEN);
    }
}
static K_WORK_DEFINE(strip_work, strip_cb);

void lamp_strip_show(void) { k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &strip_work); }

void lamp_strip_host(bool on) {
    if (atomic_set(&want_host, on) != on) {
        lamp_strip_show();
    }
}

void lamp_strip_set(int index, uint8_t r, uint8_t g, uint8_t b) {
    if (index >= 0 && index < LAMP_STRIP_LEN) {
        pixels[index] = (struct led_rgb){.r = r, .g = g, .b = b};
    }
}
