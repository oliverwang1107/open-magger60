/*
 * km_socd.h - SOCD (simultaneous opposing cardinal directions) filter, M6 package P1. Original work.
 *
 * What it is (plain words): when two opposite keys (A and D, W and S) are down at the same time, a game normally cancels
 * both and the character stops. This filter lets the keyboard decide instead, per pair of keys. It sits between the key
 * engine and the layer logic: it takes the engine's list of pressed slots and returns the same list with the "losing" key
 * of each pair removed, so everything downstream (layers, tap-hold, reports) sees a clean list. A suppressed key is, for
 * the layer logic, simply not pressed (no half-decided tap-hold is left behind).
 *
 * NOTE for users (also in the configurator and core/SOCD_NOTES.md): some games and tournament rules forbid SOCD handling.
 * It is OFF by default (mode 0 = off), it never switches itself on, and using it is the user's responsibility.
 *
 * Modes (km_socd_pair_t.mode), applied only while BOTH keys of a pair are down; with one key down it is reported as usual:
 *   KM_SOCD_OFF     0  pair not used
 *   KM_SOCD_LAST    1  last input wins: the key pressed later wins; when it is released while the other is still held, the
 *                      other one is reported again (it was never released)
 *   KM_SOCD_NEUTRAL 2  neutral: neither key is reported while both are down; when one is released the other is reported
 *   KM_SOCD_FIRST   3  first input wins: the key pressed first wins; when it is released while the other is still held,
 *                      the other one is reported (it is physically held, so the player expects it to act)
 *   KM_SOCD_DEEPER  4  deeper key wins (analogue; only a Hall-effect keyboard can do this): the key that is pressed
 *                      deeper wins. The winner changes only when the other key is deeper by KM_SOCD_DEEPER_HYST_MM100 or
 *                      more (0.30 mm), otherwise the current winner stays (so noise cannot make it flicker). When the
 *                      conflict starts the key that was already reported is the winner, unless the new key is already
 *                      deeper by the hysteresis. Two keys that go down in the very same frame: the one earlier in the
 *                      engine's list (lower slot number) is the starting winner. Without a depth callback every depth
 *                      counts as equal, which makes this mode behave like KM_SOCD_FIRST.
 * "Pressed earlier / later" is the position in the engine's pressed list (oldest press first), so first/last need no
 * stored state at all: a key that is released and pressed again is "later" again. Keys that go down in the same frame
 * are listed in ascending slot order, so there the higher slot counts as "later" (deterministic, and a 1-frame difference
 * is not something a finger can mean).
 *
 * Flags (km_socd_pair_t.flags): bit0 = the pair works only while the active layer is 0 (the layer a key pressed now would
 * use, km_layers_out_t.layer of the previous update). When the active layer changes while a pair is in conflict the
 * suppression starts / stops at once: a key then appears to or disappears from the layer logic exactly like a press or a
 * release, nothing can stay stuck (the layer logic latches what each key emitted).
 *
 * Validity (km_socd_pair_valid): mode 0..4; flags 0 or 1; for a used pair (mode != 0) two different slots that both carry a
 * key (km_keymap_slot_is_key). A mode-0 pair is ignored whatever its slots are. km_socd_set_pairs() additionally rejects a
 * set in which a slot is used by two pairs. A rejected set leaves ALL pairs off (a bad configuration never half-works).
 *
 * Properties (checked by tests/test_socd.c over random sequences):
 *   - the output is the input with entries removed: same order, nothing added, nothing duplicated;
 *   - an empty input gives an empty output and clears every bit of state (a fail-safe release or a layer reset can never
 *     leave a key suppressed or reported);
 *   - a key whose partner is up (or that is not in any pair) is always reported;
 *   - the only state is the "current winner" of the pairs in DEEPER mode; it is cleared whenever the pair is not in conflict.
 *
 * No allocation, no hardware access, no division; cost: see core/SOCD_NOTES.md.
 */
#ifndef KM_SOCD_H
#define KM_SOCD_H

#include <stdbool.h>
#include <stdint.h>

/* The shared M6 types (settings format v3, package P0) come from km_m6_types.h once it exists; until then this header
 * carries the same definitions, so P1 builds and tests on its own. Both definitions must stay identical (plan section 2.2). */
#if defined(__has_include)
#if __has_include("km/km_m6_types.h")
#define KM_SOCD_HAVE_M6_TYPES 1
#include "km/km_m6_types.h"
#endif
#endif
#ifndef KM_SOCD_HAVE_M6_TYPES
#define KM_SOCD_MAX 4u              /* pairs per profile */
typedef struct {
    uint8_t slot_a, slot_b;         /* the two opposing keys, sensor slots */
    uint8_t mode;                   /* KM_SOCD_* (0 = off) */
    uint8_t flags;                  /* bit0: only while the active layer is 0 */
} km_socd_pair_t;
#endif

#define KM_SOCD_OFF      0u
#define KM_SOCD_LAST     1u
#define KM_SOCD_NEUTRAL  2u
#define KM_SOCD_FIRST    3u
#define KM_SOCD_DEEPER   4u
#define KM_SOCD_MODE_LAST_VALID 4u  /* highest mode number */
#define KM_SOCD_FLAG_LAYER0 0x01u

#define KM_SOCD_DEEPER_HYST_MM100 30u   /* "deeper wins" changes the winner only when the other key is this much deeper */
#define KM_SOCD_SLOTS 76u               /* same as KM_KEYMAP_SLOTS; slot numbers >= this are never keys */

/* Depth of a slot in 0.01 mm (0 = not pressed / unknown). In the firmware: km_engine_depth(&engine, slot). */
typedef uint16_t (*km_socd_depth_fn)(void *ctx, unsigned slot);

typedef struct {
    uint8_t  slot_a[KM_SOCD_MAX];       /* copies of the active pairs; index = pair number */
    uint8_t  slot_b[KM_SOCD_MAX];
    uint8_t  mode[KM_SOCD_MAX];         /* 0 for an unused pair */
    uint8_t  flags[KM_SOCD_MAX];
    uint8_t  owner[KM_SOCD_MAX];        /* DEEPER: 0 = no conflict, 1 = key A wins, 2 = key B wins */
    uint8_t  role[KM_SOCD_SLOTS];       /* 0 = slot is in no pair, else 1 + 2 * pair + (0 for A, 1 for B) */
    uint8_t  n_used;                    /* pairs with mode != 0 */
    uint32_t supp[3];                   /* slots suppressed by the last call (bit s of word s / 32) */
    uint32_t suppress_events;           /* diagnostics: how many keys were newly suppressed so far */
    uint32_t calls_in_conflict;         /* diagnostics: calls in which at least one pair was in conflict */
} km_socd_t;

/* All pairs off, nothing suppressed, counters 0. */
void km_socd_init(km_socd_t *s);

/* Forget the running state (the DEEPER winners and the last suppression set); the configured pairs stay. Call after a
 * fail-safe release, a USB reset, a profile change. (A call with an empty list does the same.) */
void km_socd_reset(km_socd_t *s);

/* True when one pair is acceptable (see above). */
bool km_socd_pair_valid(const km_socd_pair_t *p);

/* Load the pairs of a profile (n <= KM_SOCD_MAX; pairs == NULL or n == 0: all off). Returns true when every pair is valid
 * and no slot is used twice; otherwise nothing is used (all off) and false is returned. Resets the running state. */
bool km_socd_set_pairs(km_socd_t *s, const km_socd_pair_t *pairs, unsigned n);

/*
 * Filter one pressed list. down_in = the engine's pressed slots (oldest press first), n_in entries; layer = the active
 * layer (see flags above); depth / ctx = depth callback, only called while a DEEPER pair is in conflict (may be NULL).
 * down_out must have room for n_in entries (km_engine: KM_ENGINE_MAX_DOWN); it may be the same buffer as down_in.
 * Entries >= KM_SOCD_SLOTS pass through untouched (the layer logic ignores them). Returns the output length.
 */
unsigned km_socd_filter(km_socd_t *s, const uint8_t *down_in, unsigned n_in, unsigned layer, km_socd_depth_fn depth,
                        void *ctx, uint8_t *down_out);

/* True when the last call suppressed this slot (for LEDs / diagnostics). */
bool km_socd_suppressed(const km_socd_t *s, unsigned slot);

#endif /* KM_SOCD_H */
