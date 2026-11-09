/*
 * km_dks.h - Dynamic Keystroke (DKS), M6 package P3. Original work.
 *
 * What it is (plain words): one key, up to four actions, triggered by HOW DEEP the key is pressed. Example: a shallow press
 * (past the start point) presses A; pressing all the way down presses S as well; letting the key up a little releases S;
 * letting go completely releases A. A Hall-effect keyboard knows the depth continuously, so this is real depth, not a
 * second contact.
 *
 * The four events of one press (km_dks_rec_t.act[i].ev holds 2 bits per event, bit0-1 = event 1 ... bit6-7 = event 4):
 *   1  the key goes down past t_act          (the engine's press: it is configured with press = t_act)
 *   2  the key goes down past t_bot          (bottom point; t_bot = 0 in the record means "travel - 30" = 3.10 mm)
 *   3  the key comes back up past t_bot - 20 (a hysteresis of 0.20 mm: the noise is about +-0.15 mm)
 *   4  the key is released                   (the engine's release, configured at t_act - 10)
 * Every action has, for every event, one of: 0 nothing, 1 press the usage, 2 release the usage, 3 tap (press now, release
 * KM_DKS_TAP_MS later; on an action that is already down it changes nothing, except that it restarts a tap pulse).
 * The decision when the key is down or up stays with the key engine (debounce, glitch and fault guards, drift tracking are
 * all the engine's): this module only READS the depth of keys that the engine says are down.
 *
 * Rules (each one is a test in tests/test_dks.c):
 *  - Fast press: a key whose depth is already past t_bot when the engine reports it gets event 1 and event 2 in that order.
 *  - Fast release: a key released while it is still counted as "bottomed" gets event 3 and then event 4.
 *  - Event 4 releases EVERYTHING of the key that is still down (except a tap pulse that is still running, which ends by
 *    itself after KM_DKS_TAP_MS): a table that presses at event 1 and never releases can not leave a key stuck.
 *  - flags bit0 (retrigger) = 0: events 2 and 3 happen at most once per press (between event 1 and event 4);
 *    bit0 = 1: every crossing of the bottom point fires them again (the key need not be released first).
 *  - Visibility: two events of the same key are applied at least KM_DKS_STEP_MS apart (they wait in a small queue), so an
 *    action that is pressed by one event and released by the next one is on the wire for at least that long - a quick slam
 *    can not make a press disappear. The first event of an idle key is applied at once (no added latency).
 *  - The union of everything that is down is the output: the same usage held by two actions or two keys is listed once and
 *    stays down until the last of them is released.
 *  - km_dks_reset() (use it for a fault, a USB reset / suspend, a profile or table change) drops every action at once: the
 *    next output is empty. After the last DKS key is released the output is empty within KM_DKS_MAX_SETTLE_MS.
 *  - A key whose record index is out of range or unusable is ignored (no output, nothing remembered).
 *
 * How it is wired (P10, see core/DKS_NOTES.md): the layer logic says which pressed slots carry a DKS table entry (KM_CODE_DKS,
 * arg0 = record index; latched like every other entry until the key is released); the caller passes exactly those as
 * km_dks_press_t, plus the depth callback (km_engine_depth). The returned usages are merged into the keyboard report with
 * km_dks_merge(). apply_profile() sets the engine's trigger for a DKS key to FIXED with the press and release points from
 * km_dks_engine_points().
 *
 * No allocation, no hardware access, no division in the per-pass path (km_dks_update_cycles divides once per pass to get
 * milliseconds, exactly like km_layers_update_cycles); cost: core/DKS_NOTES.md. With nothing pressed and nothing to finish
 * a pass costs a few dozen cycles.
 */
#ifndef KM_DKS_H
#define KM_DKS_H

#include <stdbool.h>
#include <stdint.h>

/* The shared M6 types (settings format v3, package P0) come from km_m6_types.h once it exists; until then this header carries
 * the same definitions, so P3 builds and tests on its own. Both must stay identical (plan section 2.2). */
#if defined(__has_include)
#if __has_include("km/km_m6_types.h")
#define KM_DKS_HAVE_M6_TYPES 1
#include "km/km_m6_types.h"
#endif
#endif
#ifndef KM_DKS_HAVE_M6_TYPES
#define KM_DKS_MAX 8u                       /* records per profile */
typedef struct {                            /* one DKS record, 16 bytes in the settings blob */
    uint16_t t_act_mm100;                   /* start point (default 120): event 1 going down, event 4 going up */
    uint16_t t_bot_mm100;                   /* bottom point (0 = travel - 30): event 2 going down, event 3 going up */
    uint8_t  flags;                         /* bit0 = events 2 and 3 may fire again without a full release */
    uint8_t  rsvd[3];                       /* must be 0 */
    struct { uint8_t usage; uint8_t ev; } act[4];   /* usage 0x04..0xE7 (0 = action unused); ev = 4 x 2 bits */
} km_dks_rec_t;
#endif

#define KM_DKS_ACTIONS        4u
#define KM_DKS_FLAG_RETRIGGER 0x01u

#define KM_DKS_EV_NONE    0u
#define KM_DKS_EV_PRESS   1u
#define KM_DKS_EV_RELEASE 2u
#define KM_DKS_EV_TAP     3u

#define KM_DKS_ACT_MIN_MM100      40u    /* t_act at least this (below it the sensor noise makes the point unstable) */
#define KM_DKS_GAP_MIN_MM100      40u    /* t_bot - t_act at least this */
#define KM_DKS_TRAVEL_MM100       340u   /* same as KM_TRAVEL_MM100 */
#define KM_DKS_BOT_DEFAULT_MM100  310u   /* t_bot = 0 in a record: travel - 0.30 mm */
#define KM_DKS_HYST_MM100         20u    /* event 3 when the depth is this far above t_bot */
#define KM_DKS_RELEASE_BELOW_MM100 10u   /* the engine releases at t_act - 10 */

#define KM_DKS_KEYS       8u             /* DKS keys that can be tracked at the same time (held or still letting go) */
#define KM_DKS_OUT_MAX    (KM_DKS_KEYS * KM_DKS_ACTIONS)
#define KM_DKS_QUEUE      6u             /* events that can wait per key */
#define KM_DKS_STEP_MS    4u             /* least time between two applied events of one key */
#define KM_DKS_TAP_MS     10u            /* length of a tap pulse (same as KM_LAYERS_STEP_MS) */
#define KM_DKS_MAX_SETTLE_MS ((KM_DKS_QUEUE + 2u) * KM_DKS_STEP_MS + KM_DKS_TAP_MS)   /* last key up -> output empty (passes at least every 2 ms) */
#define KM_DKS_SLOTS      76u

/* A pressed slot whose latched table entry is KM_CODE_DKS with this record index. */
typedef struct {
    uint8_t slot;
    uint8_t rec;
} km_dks_press_t;

/* Depth of a slot in 0.01 mm (0 = unknown). In the firmware: km_engine_depth(&engine, slot). */
typedef uint16_t (*km_dks_depth_fn)(void *ctx, unsigned slot);

/* The usages that are down because of DKS, each once, in the order key table / action number. */
typedef struct {
    uint8_t usage[KM_DKS_OUT_MAX];
    uint8_t n;
} km_dks_out_t;

/* Private per-key state (public only so that the owner can allocate everything statically). */
typedef struct {
    uint32_t next_ms;                       /* earliest time the next queued event may be applied */
    uint32_t tap_due[KM_DKS_ACTIONS];       /* end of a running tap pulse */
    uint8_t  slot;                          /* 0xFF = this entry is free */
    uint8_t  rec;                           /* record of the press that is open now (events carry their own record) */
    uint8_t  phase;                         /* generator: bit0 = a press is open, bit1 = bottomed, bit2 = bottom used once */
    uint8_t  held;                          /* bit a = action a is down */
    uint8_t  tap;                           /* bit a = the down state of action a is a tap pulse */
    uint8_t  seen;                          /* in the list of this pass */
    uint8_t  q_n;
    uint8_t  q[KM_DKS_QUEUE];               /* queued events: (record << 2) | (event - 1) */
    uint8_t  usage[KM_DKS_ACTIONS];         /* the usage action a holds while it is down (taken when it went down) */
} km_dks_key_t;

typedef struct {
    km_dks_rec_t rec[KM_DKS_MAX];
    uint8_t      ok[KM_DKS_MAX];            /* 1 = record usable */
    uint8_t      ev_mask[KM_DKS_MAX];       /* bit e-1 = event e has at least one action */
    km_dks_key_t key[KM_DKS_KEYS];
    uint8_t      where[KM_DKS_SLOTS];       /* slot -> 1 + index in key[], 0 = not tracked */
    uint8_t      n_tracked;                 /* entries of key[] in use */
    bool         dirty;                     /* an action went down or up since cur was built */
    km_dks_out_t cur;                       /* the output of the last pass (rebuilt only when something changed) */
    uint32_t     ms;                       /* own millisecond clock (km_dks_update_cycles) */
    uint32_t     clk_prev;
    bool         clk_init;
    uint32_t     events_applied;            /* diagnostics */
    uint32_t     queue_flushes;             /* a queue was full: its events were applied at once */
    uint32_t     overflow_presses;          /* a DKS key could not be tracked (all KM_DKS_KEYS entries busy) */
    uint32_t     resets;
} km_dks_t;

/* No records, nothing tracked, counters 0. */
void km_dks_init(km_dks_t *d);

/* Drop everything now: every action is up, the queues are empty, no key is tracked. The records stay. */
void km_dks_reset(km_dks_t *d);

/* The effective bottom point of a record (the default for 0). */
uint16_t km_dks_t_bot(const km_dks_rec_t *r);

/* A record is usable: t_act >= 40; (effective) t_bot - t_act >= 40 and t_bot <= 340; flags 0 or 1; rsvd 0; every usage is 0
 * or a keyboard usage 0x04..0xE7. (An action with usage 0 is unused; its ev bits are ignored.) */
bool km_dks_rec_valid(const km_dks_rec_t *r);

/* Load the records of a profile (n <= KM_DKS_MAX; NULL or 0 = none). true when every record is valid; otherwise NO record is
 * used (all DKS off) and false is returned. Always resets the running state (the actions of the old records are dropped). */
bool km_dks_set_records(km_dks_t *d, const km_dks_rec_t *recs, unsigned n);

/* The engine trigger points for a DKS key with this record: press at t_act, release at t_act - 10 (km_engine FIXED mode).
 * false (nothing written) for a record that is not valid. */
bool km_dks_engine_points(const km_dks_rec_t *r, uint16_t *press_mm100, uint16_t *release_mm100);

/*
 * One pass of the main loop. down[0..n) = the pressed DKS keys (any order, a slot listed twice counts once); now_ms = free-
 * running millisecond counter (wrap safe); depth / ctx = depth callback (read only for tracked keys whose record has an
 * action on event 2 or 3; may be NULL: then there are no events 2 and 3). The usages that are down because of DKS go to out
 * (out->n of them); returns out->n. At most KM_DKS_KEYS keys are tracked, so the work per pass is bounded; entries beyond
 * that are only counted (overflow_presses).
 */
unsigned km_dks_update_ms(km_dks_t *d, const km_dks_press_t *down, unsigned n, uint32_t now_ms, km_dks_depth_fn depth, void *ctx,
                          km_dks_out_t *out);

/* The same with a CPU cycle counter (the module turns it into milliseconds, wrap safe, like km_layers_update_cycles).
 * cycles_per_ms = 0 means "no clock": every pass then counts as KM_DKS_STEP_MS, so queued events still drain (nothing can
 * get stuck), but all timings are meaningless. */
unsigned km_dks_update_cycles(km_dks_t *d, const km_dks_press_t *down, unsigned n, uint32_t now_cycles, uint32_t cycles_per_ms,
                              km_dks_depth_fn depth, void *ctx, km_dks_out_t *out);

/* Add the DKS usages to a keyboard-usage list (press order, no duplicates): those already in kbd[0..n_kbd) are skipped, the
 * rest is appended while there is room (cap entries in total). Returns the new length. */
unsigned km_dks_merge(uint8_t *kbd, unsigned n_kbd, unsigned cap, const km_dks_out_t *dks);

/* True while any key is tracked or any action is down (the module has something to say / to finish). */
bool km_dks_busy(const km_dks_t *d);

#endif /* KM_DKS_H */
