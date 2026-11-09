/*
 * km_layers.h - layers, the FN key, media keys, profile-switch keys, toggle layers, tap-hold keys, one-shot
 * modifiers, toggle keys and Mod-Morph keys (M5 task T-C, task L4, M6 task P2). Original work.
 *
 * km_layers_update_ms() turns the pressed slots (press order, oldest first, as kept by the key engine) and the time
 * into
 *   - the keyboard usages to report (feed them to km_report_build_boot_usages / km_report_build_nkro_usages),
 *   - ONE consumer-page usage to report (km_report_build_consumer),
 *   - a profile-switch request.
 * Call it on every pass of the main loop (it must see the time pass: a tap-hold key is decided by a timer); it is cheap,
 * a few passes over the 76 slots.
 *
 * The table entries it understands are described in km_keymap.h (0x04..0xE7 keyboard usage, 0xE8..0xEA extended codes with
 * two argument bytes, 0xF0..0xFF function codes). Rules:
 *
 *  - Layer latching (the stuck-key rule). A key uses the layer that is active at the moment its press is HANDLED, and
 *    keeps the code it got then until it is RELEASED. The module remembers, per slot, what that key emitted, so
 *      * releasing FN before the other key does not change the other key (no stuck key, no wrong release);
 *      * pressing FN while other keys are already down does not change those keys;
 *      * a key pressed while FN is held stays "the FN-layer key" after FN is let go, until it is released.
 *    If FN and another key first appear in the same update, the order of the slot list decides: keys listed after
 *    FN see the FN layer, keys listed before it do not.
 *  - The active layer: 0, or the highest layer that is held (KM_CODE_HOLD_L1 / _L2 / _L3, or the hold of a tap-hold key)
 *    or toggled on (KM_CODE_TOGGLE). The entry of a slot on that layer is used; KM_CODE_TRANSPARENT (and only that) falls
 *    back to the layer-0 entry of the slot (code and arguments). A 0 entry is "no key" and does NOT fall through. On layer
 *    0, transparent means no key.
 *  - Slots that carry no key (mirror / floating slots) never emit anything, whatever a loaded table says.
 *  - Profile keys (KM_CODE_PROFILE_1..4) do nothing while down; when released, out->profile_request is set to
 *    1..4 for exactly that one update (the caller switches profile, then it may call km_layers_reset()).
 *  - Media keys (KM_CODE_VOL_DOWN .. KM_CODE_NEXT_TRACK): only the most recently pressed one of the keys that are
 *    down is reported (press order); when it is released the older one that is still down is reported again.
 *    out->consumer is 0 when none is down.
 *  - Keyboard usages come out in press order, each usage once (the same usage on two keys is reported once, and
 *    stays reported until both are released).
 *
 * Toggle layer (KM_CODE_TOGGLE, arg0 = layer 1..3). Pressing the key switches that layer on if it is off and off if it is
 * on; releasing does nothing. A toggled layer stays on until it is toggled off again, until another table is loaded
 * (km_layers_set_table / km_layers_set_map: this is what a profile switch does) or until km_layers_reset(). Interaction:
 *    * a toggled layer and a held layer are independent: the active layer is the highest of all of them, so releasing a
 *      momentary hold never turns a toggled layer off, and toggling a layer that is also held leaves it on after the
 *      hold ends;
 *    * keys that are down keep the code they were pressed with (latching), so toggling a layer while keys are down
 *      changes none of them and every release still matches its press: nothing can get stuck.
 *    * the key that toggles a layer off must be reachable on the active layer (the configurator warns about it); the
 *      escape hatches are the same toggle key, a profile switch, and a reset.
 *
 * Tap-hold (KM_CODE_TAP_HOLD, arg0 = tap action, arg1 = hold action). Pressing the key does not decide anything yet
 * (the key is "undecided"). With tap_term_ms from the layer options and the policy chosen there:
 *    * released before tap_term_ms (measured from the press, using the time the events were seen) -> a TAP: the tap
 *      action is sent as a short press-and-release (KM_LAYERS_STEP_MS long, so the host cannot miss it);
 *    * held until tap_term_ms -> a HOLD: the hold action starts (layer held / modifier down) and lasts until release;
 *    * policy KM_TAP_POLICY_OTHER_KEY: a HOLD as soon as another key is pressed; KM_TAP_POLICY_PERMISSIVE: a HOLD when
 *      another key is pressed AND released before this one is released and before the term ends.
 *    Keys pressed while a tap-hold key is undecided are NOT lost and NOT reordered: their presses wait in a small queue
 *    and are replayed, in order, after the decision, so they see the layer / modifier of a hold and come after the key of
 *    a tap. Releases of keys that were already down before the tap-hold key was pressed are not delayed. Replayed events
 *    are KM_LAYERS_STEP_MS apart, so every press is visible for at least that long. If the queue (KM_LAYERS_EVENTS) ever
 *    fills up, the undecided key becomes a hold at once and everything queued is processed (nothing is dropped).
 *    The hold lasts exactly as long as the key is down.
 *    The hold action (format 3) may also be an ordinary key: it is then that key, down until the tap-hold key is released
 *    (a modifier or a layer hold is as before). An ordinary key takes an armed one-shot modifier with it, a modifier does not.
 *
 * One-shot modifier (KM_CODE_ONESHOT_MOD, arg0 = modifier usage). Pressing it sends the modifier at once, so holding it
 * and typing is an ordinary modifier. If another key was pressed while it was held, or it was held for tap_term_ms or
 * longer, it is just that; if not (a tap), the
 * modifier stays on ("armed") and is applied to the NEXT key that is pressed: it is sent with that key and ends when that
 * key is released. A plain modifier key (0xE0..0xE7), layer keys, toggle keys, profile keys and media keys do not use it
 * up. Armed modifiers also end after oneshot_ms of the layer options (0 = never), on a table change and on a reset.
 * Several one-shot modifiers can be armed together.
 *
 * Toggle key (KM_CODE_TOGGLE_KEY, arg0 = keyboard usage 0x04..0xE7, a modifier too). Pressing the key switches that usage on if
 * it is off and off if it is on; releasing does nothing. While it is on the usage is in out->kbd as if a key held it down.
 *    * The state belongs to the usage, not to the slot: two toggle keys with the same usage switch the same thing.
 *    * Up to KM_LAYERS_TOGGLE_KEYS usages can be on at once; a press that would switch on one more is ignored (it stays off).
 *    * The state lives in RAM only and ends: when the usage is toggled again, on km_layers_reset() (boot, USB reset, fail-safe
 *      release, so the caller must call it there) and when another table is loaded (profile switch, settings commit).
 *    * The key counts as "another key was pressed" for tap-hold and one-shot decisions, like a layer toggle key, but does not
 *      use up an armed one-shot modifier.
 *    * out->toggle_keys is how many are on. The usages come after the keys that are held, in the order they were switched on.
 *
 * Mod-Morph (KM_CODE_MOD_MORPH, arg0 = the usage sent normally, arg1 = record index into km_layers_set_morph()). When the key's
 * press is HANDLED the module looks at the modifiers that are being sent at that moment (held modifier keys, modifiers carried
 * by other keys, an armed one-shot modifier, a toggled modifier key; NOT a modifier whose key is still an undecided tap-hold
 * key). If one of them is in the record's mod_mask the key is the record's usage instead, otherwise arg0. The choice is
 * LOCKED until the key is released (the layer latching rule): releasing or pressing the modifier later changes nothing, so the
 * release always matches the press. With the record's suppress flag the modifiers that triggered the morph (the bits of mod_mask
 * that were being sent) are left out of the report while the key is down, and come back when it is released. A suppressed
 * modifier that is released in the meantime is simply gone (no phantom modifier when the morph key goes up); pressing a
 * modifier again while the morph key is still down is NOT suppressed. The key's own output is never suppressed. An index with no
 * record, or a record that breaks the rules of km_settings_morph_valid(), makes the key a plain arg0 key.
 *
 * Time. now_ms is a free-running millisecond counter (32 bit, wrap-safe: only differences are used). The tap-hold
 * deadline and the one-shot timeout are in that unit.
 */
#ifndef KM_LAYERS_H
#define KM_LAYERS_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_keymap.h"

#define KM_LAYERS_EVENTS   64u   /* events that can wait behind an undecided tap-hold key */
#define KM_LAYERS_STEP_MS  10u   /* a tap pulse is this long; events that had to wait are replayed this far apart */
#define KM_LAYERS_TOGGLE_KEYS 16u /* toggle keys (KM_CODE_TOGGLE_KEY usages) that can be on at the same time */

typedef struct {
    uint8_t  kbd[KM_KEYMAP_SLOTS];  /* keyboard-page usages 0x04..0xE7, press order, no duplicates */
    uint8_t  kbd_n;
    uint16_t consumer;              /* consumer-page usage (page 0x0C), 0 = none */
    uint8_t  profile_request;       /* 0 = none, 1..4 = switch to that profile now */
    uint8_t  layer;                 /* the layer that a key pressed now would use (0..KM_LAYERS-1) */
    uint8_t  toggled;               /* bit l = layer l is toggled on */
    uint8_t  armed;                 /* one-shot modifiers waiting for the next key (bit i = usage 0xE0 + i) */
    uint8_t  toggle_keys;           /* how many toggle keys (KM_CODE_TOGGLE_KEY) are on */
    uint8_t  suppressed;            /* modifiers (bit i = usage 0xE0 + i) that Mod-Morph keys are keeping out of kbd[] now */
    bool     pending;               /* a tap-hold key is undecided */
} km_layers_out_t;

/* Forget everything (all keys count as released, no layer toggled, no toggle key on, nothing armed). Call at start-up, after a
 * USB reset, after a fail-safe release. It keeps the Mod-Morph records. */
void km_layers_reset(void);

/* The Mod-Morph records of the active profile (km_profile_t.morph / morph_n), copied. Call it whenever a profile is activated,
 * together with km_layers_set_table(). NULL or n = 0 removes them (every Mod-Morph key is then a plain key). Keys that are down
 * keep what they were pressed with. Records beyond KM_MORPH_MAX are ignored. */
void km_layers_set_morph(const km_morph_rec_t *rec, unsigned n);

/* Process the pressed slots at time now_ms. Entries >= KM_KEYMAP_SLOTS and repeated entries are ignored. */
void km_layers_update_ms(const uint8_t *down_slots, unsigned n, uint32_t now_ms, km_layers_out_t *out);

/* The same with a CPU cycle counter (any 32-bit free-running counter): the module turns it into milliseconds itself, so a
 * counter that wraps every few seconds is fine. */
void km_layers_update_cycles(const uint8_t *down_slots, unsigned n, uint32_t now_cycles, uint32_t cycles_per_ms,
                             km_layers_out_t *out);

/* Without a time: the clock stands still (tables without tap-hold and one-shot keys do not need one). */
void km_layers_update(const uint8_t *down_slots, unsigned n, km_layers_out_t *out);

/* The table entry a pressed key is LOCKED to (M6): what the key resolved to when its press was handled (layer latching, see
 * above), kept until the key is released. The modules that own the M6 codes read the pressed keys through this:
 *     for each slot in the pressed list:
 *         km_layers_entry_t e;
 *         if (km_layers_locked_entry(slot, &e) && e.code == KM_CODE_DKS) { ... e.arg0 = the DKS record index ... }
 * Returns false (and leaves *out alone) when the resolver has no pressed record for the slot: the key is not down, its press
 * is still waiting in the queue (behind an undecided tap-hold key), the key is an undecided tap-hold key itself, or it was a tap
 * (a tap is a pulse, there is nothing to lock). For a tap-hold key that was decided as a HOLD the entry is the tap-hold entry
 * (code KM_CODE_TAP_HOLD, tap action, hold action), exactly as stored in the table.
 * The codes KM_CODE_DKS .. KM_CODE_PAD_AXIS and KM_CODE_LIGHT produce NO keyboard output from this module: the key is down,
 * counts as "another key was pressed" for tap-hold and one-shot decisions, and is otherwise silent. Toggle keys and Mod-Morph
 * keys are reported with their table entry (the morph choice is not visible here). A slot is only reported while update has
 * seen it down, so the accessor reflects the last km_layers_update*() call. */
typedef struct {
    uint8_t code;   /* the table code (KM_CODE_*, or a keyboard usage 0x04..0xE7, or 0 for "no key") */
    uint8_t arg0;
    uint8_t arg1;
} km_layers_entry_t;
bool km_layers_locked_entry(unsigned slot, km_layers_entry_t *out);

/* The consumer-page usage of a media function code KM_CODE_VOL_DOWN..KM_CODE_NEXT_TRACK; 0 for any other code. */
uint16_t km_layers_consumer_usage(uint8_t code);

#endif /* KM_LAYERS_H */
