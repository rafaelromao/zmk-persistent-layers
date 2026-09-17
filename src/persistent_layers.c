/*
 * Copyright (c) 2026 Rafael Romão
 *
 * SPDX-License-Identifier: MIT
 *
 * Remember a set of layers per endpoint, across reboots.
 *
 * A mode layer -- an alternate OS, a different base -- is really a property of
 * the host being typed on, not of the keyboard. ZMK keeps neither: layer state
 * is a plain static cleared by every restart, and it is shared by every
 * endpoint, so switching Bluetooth profiles carries the previous host's mode
 * along with it.
 *
 * This node closes both gaps with one table. Every change to an owned layer is
 * recorded against the endpoint that was selected at the time, and the whole
 * table is written to settings; selecting an endpoint replays its row. A reboot
 * is then just an endpoint change that happens to come after a cold start.
 *
 * Two details shape the code:
 *
 *   - The endpoint is ZMK_TRANSPORT_NONE until something connects, and goes
 *     back to it whenever the host drops. That is not a host with an opinion,
 *     so it is neither recorded nor replayed -- the mode simply stays as it is
 *     until a real endpoint returns.
 *
 *   - Replaying a row raises layer_state_changed for each layer it moves. Those
 *     events are this node's own writes, not the user's, so they are ignored
 *     while a replay is in flight. Recording them would be harmless today, but
 *     only because the value being written back is the one already stored.
 */

#define DT_DRV_COMPAT zmk_persistent_layers

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define SAVE_DELAY_MS DT_INST_PROP(0, save_delay_ms)

#define SETTINGS_SUBTREE "persist_layers"
#define SETTINGS_KEY "endpoints"
#define SETTINGS_PATH SETTINGS_SUBTREE "/" SETTINGS_KEY

/* The layers this node owns. Nothing else is read or written. */
static const uint8_t owned[] = DT_INST_PROP(0, layers);
#define N_OWNED ARRAY_SIZE(owned)

BUILD_ASSERT(N_OWNED >= 1, "layers must not be empty");

/*
 * One bitmask of owned layers per endpoint, indexed by
 * zmk_endpoint_instance_to_index(). Index 0 is ZMK_TRANSPORT_NONE and is never
 * used; it costs four bytes to keep the indices identical to ZMK's own.
 */
static uint32_t endpoint_masks[ZMK_ENDPOINT_COUNT];

/* -1 until a real endpoint is selected. */
static int current_index = -1;

/* True while apply() is writing, so its own events are not read back as input. */
static bool applying;

static bool owns(uint8_t layer) {
    for (size_t i = 0; i < N_OWNED; i++) {
        if (owned[i] == layer) {
            return true;
        }
    }
    return false;
}

static uint32_t current_mask(void) {
    uint32_t mask = 0;

    for (size_t i = 0; i < N_OWNED; i++) {
        WRITE_BIT(mask, owned[i], zmk_keymap_layer_active(owned[i]));
    }

    return mask;
}

#if IS_ENABLED(CONFIG_SETTINGS)

static void save_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    int err = settings_save_one(SETTINGS_PATH, endpoint_masks, sizeof(endpoint_masks));
    if (err) {
        LOG_ERR("Failed to save persistent layers (err %d)", err);
    }
}

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_cb);

static void schedule_save(void) { k_work_reschedule(&save_work, K_MSEC(SAVE_DELAY_MS)); }

static int settings_set_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;

    if (!settings_name_steq(name, SETTINGS_KEY, &next) || next) {
        return -ENOENT;
    }

    /*
     * Read only as much as fits. A build whose endpoint count changed since the
     * table was written still loads the rows it can use rather than nothing.
     */
    int read = read_cb(cb_arg, endpoint_masks, MIN(len, sizeof(endpoint_masks)));
    if (read < 0) {
        LOG_ERR("Failed to read persistent layers from settings (err %d)", read);
        return read;
    }

    LOG_DBG("loaded %d bytes of persistent layer state", read);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(persist_layers, SETTINGS_SUBTREE, NULL, settings_set_cb, NULL, NULL);

#else

static void schedule_save(void) {}

#endif /* IS_ENABLED(CONFIG_SETTINGS) */

/*
 * Bring the owned layers in line with an endpoint's stored row. Locking on both
 * sides: these layers are modes, held on by a locking toggle, and a non-locking
 * deactivation of one would be refused by the keymap anyway.
 */
static void apply(int index) {
    uint32_t wanted = endpoint_masks[index];

    applying = true;

    for (size_t i = 0; i < N_OWNED; i++) {
        uint8_t layer = owned[i];
        bool on = (wanted & BIT(layer)) != 0;

        if (on && !zmk_keymap_layer_active(layer)) {
            zmk_keymap_layer_activate(layer, true);
        } else if (!on && zmk_keymap_layer_active(layer)) {
            zmk_keymap_layer_deactivate(layer, true);
        }
    }

    applying = false;

    LOG_DBG("applied layer mask 0x%08x for endpoint %d", wanted, index);
}

static int persistent_layers_listener(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *ep_ev = as_zmk_endpoint_changed(eh);

    if (ep_ev != NULL) {
        if (ep_ev->endpoint.transport == ZMK_TRANSPORT_NONE) {
            /* Nothing is listening; hold the current state and record nothing. */
            current_index = -1;
            return ZMK_EV_EVENT_BUBBLE;
        }

        int index = zmk_endpoint_instance_to_index(ep_ev->endpoint);
        if (index < 0 || index >= ZMK_ENDPOINT_COUNT) {
            LOG_WRN("endpoint index %d out of range; ignoring", index);
            return ZMK_EV_EVENT_BUBBLE;
        }

        current_index = index;
        apply(index);
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_layer_state_changed *layer_ev = as_zmk_layer_state_changed(eh);

    if (layer_ev != NULL && !applying && current_index >= 0 && owns(layer_ev->layer)) {
        uint32_t mask = current_mask();

        if (mask != endpoint_masks[current_index]) {
            endpoint_masks[current_index] = mask;
            LOG_DBG("endpoint %d layer mask -> 0x%08x", current_index, mask);
            schedule_save();
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(persistent_layers, persistent_layers_listener);
ZMK_SUBSCRIPTION(persistent_layers, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(persistent_layers, zmk_layer_state_changed);

static int persistent_layers_init(void) {
    /*
     * Seed from whatever is already selected. At this point that is almost
     * always ZMK_TRANSPORT_NONE -- settings_load() has not run yet either -- so
     * the real work happens on the first endpoint_changed. Seeding anyway keeps
     * a unibody board plugged in at boot from missing its first recording.
     */
    struct zmk_endpoint_instance selected = zmk_endpoint_get_selected();

    if (selected.transport != ZMK_TRANSPORT_NONE) {
        current_index = zmk_endpoint_instance_to_index(selected);
    }

    LOG_DBG("initialised for %zu layers over %d endpoints", N_OWNED, ZMK_ENDPOINT_COUNT);
    return 0;
}

SYS_INIT(persistent_layers_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
