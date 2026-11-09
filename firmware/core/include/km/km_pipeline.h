/*
 * km_pipeline.h - the M6 key pipeline (package P10): the order in which the engine's pressed list passes the M6 modules, as pure
 * logic so that the host tests run the SAME code the firmware runs (port/n32h48x/m6_main.c only supplies the callbacks and the
 * report paths). Original work, no hardware, no allocation, no waiting.
 *
 *     engine pressed list (oldest first, physical keys)
 *       -> km_socd_filter     opposing keys: the losing key disappears from the list (the layers never see it)
 *       -> km_layers_update   layers, tap-hold, one-shot, toggle keys, Mod-Morph, media keys, profile keys  -> kbd usages
 *       -> for the keys that are down, ask km_layers_locked_entry() what each key is LOCKED to:
 *            KM_CODE_DKS      -> km_dks_update   extra keyboard usages, merged into the keyboard list
 *            KM_CODE_PAD_*    -> km_pad_update   one gamepad state (report of its own)
 *            KM_CODE_LIGHT    -> light actions   reported once per press (the caller changes the light settings)
 *
 * Invariants (tests/test_pipeline_m6.c checks them over random key sequences, random valid tables and injected faults):
 *   - every physical key up and no toggle key on  =>  the keyboard list is empty, the gamepad state is neutral, DKS has nothing down
 *     (after at most KM_DKS_MAX_SETTLE_MS of passes); nothing sticks because of a layer change, a profile change or a fault;
 *   - a fault (allowed == false), km_pipeline_reset() or a table change empties every module at once; keys that are still down
 *     then count as "held over" for DKS, the gamepad and the light actions until released once (no output comes back by itself);
 *   - the work of one pass is bounded: SOCD (4 pairs), at most KM_ENGINE_MAX_DOWN locked-entry lookups, at most KM_DKS_KEYS DKS keys,
 *     at most KM_PAD_MAX_KEYS pad keys, at most KM_PIPE_MAX_LIGHT light actions;
 *   - a module the profile does not use is not called at all: with no DKS / gamepad / light-action key in the tables a pass costs
 *     what km_socd_filter + km_layers_update_cycles cost (the lookups are skipped).
 */
#ifndef KM_PIPELINE_H
#define KM_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_dks.h"
#include "km/km_engine.h"
#include "km/km_keymap.h"
#include "km/km_layers.h"
#include "km/km_pad.h"
#include "km/km_settings.h"
#include "km/km_socd.h"

#define KM_PIPE_MAX_LIGHT   4u    /* light actions reported per pass (a key press each); more at once are dropped, the keys still count as pressed */

typedef uint16_t (*km_pipe_depth_fn)(void *ctx, unsigned slot);     /* depth of a slot in 0.01 mm (km_engine_depth) */

typedef struct {
    km_socd_t  socd;
    km_dks_t   dks;
    km_pad_t   pad;
    uint8_t    tmp[KM_ENGINE_MAX_DOWN];      /* the list after SOCD */
    uint32_t   holdover[3];                  /* slots (bit s of word s / 32) that DKS and the light actions ignore until they are released */
    uint32_t   light_down[3];                /* slots whose light action has been reported and that are still down */
    uint32_t   epoch;                        /* changes with every table change (gamepad epoch) */
    uint32_t   ms;                           /* own millisecond clock */
    uint32_t   clk_prev, clk_acc;
    bool       clk_init;
    uint8_t    prev_layer;
    bool       has_dks, has_pad, has_light;  /* the active tables contain such a code */
    bool       blocked;                      /* the last pass was refused (allowed == false) */
    bool       pad_poke;                     /* the gamepad was reset: its next update must see the keys that are down now (they are held over) */
    /* counters (diagnostics) */
    uint32_t   passes, blocked_passes, resets, lookups, dks_keys_max, pad_keys_max, light_actions, light_dropped;
} km_pipeline_t;

typedef struct {
    const uint8_t *down;          /* the engine's pressed slots, oldest press first */
    unsigned       n;
    bool           allowed;       /* false = a fault: everything is dropped and nothing is reported */
    uint32_t       now_cycles;    /* free-running CPU cycle counter */
    uint32_t       cycles_per_ms; /* 0 = no clock (every pass counts as one millisecond) */
    km_pipe_depth_fn depth;       /* NULL = every depth counts as 0 */
    void          *ctx;
} km_pipe_in_t;

typedef struct {
    km_layers_out_t lo;           /* kbd[] already includes the DKS usages */
    km_pad_report_t pad;          /* the gamepad state (neutral when no pad key is down) */
    bool            pad_recomputed;
    uint8_t         light_n;      /* light actions (KM_LIGHT_ACT_*) requested by keys that went down in this pass */
    uint8_t         light[KM_PIPE_MAX_LIGHT];
    uint8_t         n_after_socd; /* pressed keys the layer logic saw */
} km_pipe_out_t;

void km_pipeline_init(km_pipeline_t *p);

/* Everything to rest: layers (toggle keys, tap-hold, one-shot, latched codes), SOCD, DKS, gamepad, light-action memory. The tables stay.
 * Call it on a fail-safe release (the pipeline does it itself when a pass is refused), a USB reset and at start-up. */
void km_pipeline_reset(km_pipeline_t *p);

/* Make `prof` the active profile: tables, Mod-Morph records, SOCD pairs, DKS records and gamepad curves are loaded and the running state
 * is reset. `down`/`n` = the keys that are down right now (they are held over: DKS, the gamepad and the light actions ignore them until
 * they have been released once). A set the module rejects as a whole (km_socd_set_pairs / km_dks_set_records) leaves that module off. */
void km_pipeline_set_profile(km_pipeline_t *p, const km_profile_t *prof, const uint8_t *down, unsigned n);

/* Mark the keys that are down now as held over (DKS, the gamepad and the light actions ignore them until released once). Call it right
 * after km_pipeline_reset() when the reset happens with keys physically down (USB bus reset), so that they do not start again by themselves. */
void km_pipeline_hold_over(km_pipeline_t *p, const uint8_t *down, unsigned n);

/* The key engine settings the active profile needs for one slot: its own key config, except that a slot that carries a DKS entry on any
 * layer is FIXED with press = t_act and release = t_act - 10, and a slot that carries a gamepad entry is FIXED with press 0.20 mm and
 * release 0.10 mm (KM_PAD_ENGINE_*), because the engine's pressed/released is event 1/4 of DKS and "pressed a little" for the gamepad.
 * The first such layer entry (layer 0 first) decides. Returns true when the config was overridden. */
bool km_pipeline_slot_cfg(const km_profile_t *prof, unsigned slot, km_engine_key_cfg_t *out);

/* One pass of the main loop. */
void km_pipeline_run(km_pipeline_t *p, const km_pipe_in_t *in, km_pipe_out_t *out);

#endif /* KM_PIPELINE_H */
