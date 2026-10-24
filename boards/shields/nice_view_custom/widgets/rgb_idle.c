/*
 * Underglow idle auto-off, in place of ZMK's CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE.
 * SPDX-License-Identifier: MIT
 *
 * ZMK's auto-off turns the underglow off with zmk_rgb_underglow_off(), which saves "off" to flash
 * as if the user had chosen it. The right half runs on battery and deep-sleeps after an hour of no
 * key presses of its own (gaming: right hand on the mouse); a key press then reboots it, it loads
 * "off" and stays dark. Here the user's choice (wanted) is kept apart from the idle state and
 * saved on its own, so the underglow comes back after idle, sleep or a reboot.
 *
 * On a split keyboard the left (central) sees every key press, so its activity is the keyboard's.
 * With sync on (rgb_idle_set_sync, from the PC), the left sends its underglow state to the right
 * over the art relay link and the right follows it; with sync off, or while the link is down, the
 * right goes by its own activity.
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/rgb_underglow.h>

#include "rgb_idle.h"
#include "art_relay.h"
#include "host_link.h"

BUILD_ASSERT(!IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE),
             "NICE_VIEW_RGB_IDLE replaces ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE; turn that off");

#define CENTRAL (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
#define RELAY_CENTRAL (CENTRAL && IS_ENABLED(CONFIG_NICE_VIEW_ART_RELAY))
#define HOST_CENTRAL (CENTRAL && IS_ENABLED(CONFIG_NICE_VIEW_HOST_LINK))

// Picks up on/off changes made with the rgb_ug behaviours while active.
#define POLL K_SECONDS(2)
// Saving waits a little, so a few quick toggles make one flash write.
#define SAVE_DELAY K_SECONDS(10)

static bool wanted;      // the user's on/off, saved
static bool loaded;      // wanted came from the settings
static bool sync = true; // left: the right follows the left, saved
static bool following;   // right: the left's state is in charge
static bool host_drawn;  // left: the PC drives the LEDs (lamparray.c)
static bool remote_on;   // right: the left's state

static bool active(void) { return zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE; }

/* State for the PC */

static uint8_t speed = CONFIG_ZMK_RGB_UNDERGLOW_SPD_START;

// ZMK keeps the animation speed to itself, so it comes from what ZMK last saved: struct
// rgb_underglow_state in rgb_underglow.c (v0.3): h u16, s u8, b u8, speed u8, effect u8, ...
//
// Reading the settings is slow and takes the settings lock, which main holds for the whole boot
// time settings_load. Doing it on the system work queue from the settings commit is the likely
// cause of #45's boot hang (the left didn't even enumerate on USB). So it only happens when the PC
// asks, well after boot, at most every SPEED_EVERY_MS, on a thread of its own.
#define SPEED_AFTER_BOOT K_SECONDS(10)
#define SPEED_EVERY_MS 5000
#define SPEED_STACK_SIZE 2048

K_THREAD_STACK_DEFINE(speed_stack, SPEED_STACK_SIZE);
static struct k_work_q speed_q;
static atomic_t speed_ready;
static int64_t speed_read_at;
static int speed_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
                    void *param) {
    uint8_t buf[16];
    if (len >= 6 && len <= sizeof(buf) && read_cb(cb_arg, buf, len) == len && buf[4] >= 1 &&
        buf[4] <= 5) {
        speed = buf[4];
    }
    return 0;
}

static void speed_work_cb(struct k_work *work) {
    settings_load_subtree_direct("rgb/underglow/state", speed_cb, NULL);
}
static K_WORK_DEFINE(speed_work, speed_work_cb);

static void speed_ready_cb(struct k_work *work) {
    if (atomic_get(&speed_ready)) {
        return;
    }
    k_work_queue_start(&speed_q, speed_stack, K_THREAD_STACK_SIZEOF(speed_stack),
                       K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
    atomic_set(&speed_ready, 1);
}
static K_WORK_DELAYABLE_DEFINE(speed_ready_work, speed_ready_cb);

void rgb_idle_state(struct rgb_idle_wire *out) {
    bool on = false;
    zmk_rgb_underglow_get_state(&on);
    struct zmk_led_hsb hsb = zmk_rgb_underglow_calc_hue(0);
    *out = (struct rgb_idle_wire){
        .on = on,
        .hue = hsb.h,
        .saturation = hsb.s,
        .brightness = hsb.b,
        .effect = zmk_rgb_underglow_calc_effect(0),
        .speed = speed,
    };
    int64_t now = k_uptime_get();
    if (atomic_get(&speed_ready) && now - speed_read_at >= SPEED_EVERY_MS) {
        speed_read_at = now;
        k_work_submit_to_queue(&speed_q, &speed_work); // fresher for next time
    }
}

static void save_cb(struct k_work *work) {
    settings_save_one("nvc_rgb/wanted", &wanted, sizeof(wanted));
#if CENTRAL
    settings_save_one("nvc_rgb/sync", &sync, sizeof(sync));
#endif
}
static K_WORK_DELAYABLE_DEFINE(save_work, save_cb);

static void save(void) { k_work_reschedule(&save_work, SAVE_DELAY); }

// The work items below all run on the system work queue, one at a time.

static void apply_cb(struct k_work *work) {
    if (host_drawn) {
        return;
    }
    bool on;
    bool target = following ? remote_on : wanted && active();
    if (zmk_rgb_underglow_get_state(&on) == 0 && on != target) {
        if (target) {
            zmk_rgb_underglow_on();
        } else {
            zmk_rgb_underglow_off();
        }
#if HOST_CENTRAL
        host_link_rgb_changed();
#endif
    }
#if RELAY_CENTRAL
    art_relay_rgb_kick();
#endif
}
static K_WORK_DEFINE(apply_work, apply_cb);

static void poll_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(poll_work, poll_cb);

static void poll_cb(struct k_work *work) {
    if (!active()) {
        return; // the activity listener starts polling again
    }
    bool on;
    if (!following && !host_drawn && zmk_rgb_underglow_get_state(&on) == 0 && on != wanted) {
        wanted = on; // toggled with a key
        save();
#if HOST_CENTRAL
        host_link_rgb_changed();
#endif
#if RELAY_CENTRAL
        art_relay_rgb_kick();
#endif
    }
    k_work_schedule(&poll_work, POLL);
}

static int activity_listener(const zmk_event_t *eh) {
    k_work_submit(&apply_work);
    if (active()) {
        k_work_reschedule(&poll_work, POLL);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rgb_idle, activity_listener);
ZMK_SUBSCRIPTION(rgb_idle, zmk_activity_state_changed);

bool rgb_idle_sync(void) { return sync; }

void rgb_idle_set_sync(bool on) {
    if (sync != on) {
        sync = on;
        save();
    }
#if RELAY_CENTRAL
    art_relay_rgb_kick();
#endif
}

void rgb_idle_remote(bool on, bool follow) {
    remote_on = on;
    following = follow;
    k_work_submit(&apply_work);
}

void rgb_idle_host_control(bool on) {
    host_drawn = on;
    k_work_submit(&apply_work);
}

void rgb_idle_remote_lost(void) {
    following = false;
    k_work_submit(&apply_work);
}

static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (settings_name_steq(name, "wanted", NULL) && len == sizeof(wanted)) {
        loaded = read_cb(cb_arg, &wanted, sizeof(wanted)) == sizeof(wanted);
    } else if (settings_name_steq(name, "sync", NULL) && len == sizeof(sync)) {
        read_cb(cb_arg, &sync, sizeof(sync));
    }
    return 0;
}

// After all settings are loaded (ZMK's own underglow state too).
static int rgb_settings_commit(void) {
    if (!loaded) {
        // First boot with this firmware. ZMK's saved state can't tell the user's "off" from its
        // idle auto-off's, and after any idle it says off, so start lit; a user who wants it off
        // turns it off again and that sticks.
        wanted = true;
        loaded = true;
        save();
    }
    k_work_submit(&apply_work);
    k_work_reschedule(&poll_work, POLL);
    k_work_schedule(&speed_ready_work, SPEED_AFTER_BOOT); // never during settings_load
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(nvc_rgb, "nvc_rgb", NULL, rgb_settings_set, rgb_settings_commit, NULL);
