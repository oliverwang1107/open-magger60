/*
 * test_dks.c - host tests for the M6 package P3 code: Dynamic Keystroke (km_dks.[hc]).
 *
 *   1. records: validity boundaries, "one bad record rejects the whole set", engine trigger points
 *   2. depth sequences through a small model of the engine's press / release (press at t_act, release at t_act - 10):
 *      slow press and release, fast press, fast release, partial press, bottom and back up and down again (retrigger off / on),
 *      noise around the bottom point, taps, "event 4 releases everything", the same usage on two actions / two keys,
 *      an action that is pressed while another one is down, a record change while the key is down, a time-counter wrap
 *   3. the REAL key engine and depth model: synthetic sensor frames (with noise) -> km_engine_update -> km_dks_update_cycles
 *   4. limits: more DKS keys than the table holds, a full event queue (flush), unusable record indices, empty list
 *   5. random sequences (random records, 10 keys, depth walks and jumps, resets, skipped time): output rules at every pass
 *      and "everything is released" after the last key goes up or after a reset
 *   6. merge into the keyboard list, cycle-counter wrapper
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "km/km_depth_model.h"
#include "km/km_dks.h"
#include "km/km_engine.h"
#include "km_test.h"

void test_dks(void);

#define U_A 0x04u
#define U_S 0x16u
#define U_D 0x07u
#define U_T 0x17u
#define U_W 0x1Au

/* the keys used by the tests (slots of the W A S D keys of this board) */
#define K1 73u
#define K2 74u
#define K3 75u
#define K4 72u

/* ev byte: events 1..4 = values e1, e2, e3, e4 (0 none, 1 press, 2 release, 3 tap) */
static uint8_t ev(unsigned e1, unsigned e2, unsigned e3, unsigned e4)
{
    return (uint8_t)(e1 | (e2 << 2) | (e3 << 4) | (e4 << 6));
}

static km_dks_rec_t rec_make(unsigned t_act, unsigned t_bot, unsigned flags)
{
    km_dks_rec_t r;
    memset(&r, 0, sizeof r);
    r.t_act_mm100 = (uint16_t)t_act;
    r.t_bot_mm100 = (uint16_t)t_bot;
    r.flags = (uint8_t)flags;
    return r;
}

static void act_set(km_dks_rec_t *r, unsigned i, unsigned usage, uint8_t evb)
{
    r->act[i].usage = (uint8_t)usage;
    r->act[i].ev = evb;
}

/* the example of the plan: shallow press = A down; bottom = S down; a little up = S up; released = A up */
static km_dks_rec_t rec_classic(unsigned flags)
{
    km_dks_rec_t r = rec_make(120, 0, flags);
    act_set(&r, 0, U_A, ev(KM_DKS_EV_PRESS, 0, 0, KM_DKS_EV_RELEASE));
    act_set(&r, 1, U_S, ev(0, KM_DKS_EV_PRESS, KM_DKS_EV_RELEASE, 0));
    return r;
}

/* ---------------------------------------------------------------- the rig: engine model + depth + history */

#define MAXCH 128

typedef struct {
    uint32_t t;
    uint8_t  n;
    uint8_t  u[8];
} change_t;

typedef struct {
    km_dks_t    d;
    uint32_t    now;
    uint16_t    depth[KM_DKS_SLOTS];
    uint8_t     rec_of[KM_DKS_SLOTS];     /* 0xFF = not a DKS key */
    bool        down[KM_DKS_SLOTS];       /* the engine model's state */
    km_dks_out_t out;
    change_t    ch[MAXCH];
    unsigned    n_ch;
    uint16_t    t_act[KM_DKS_MAX];        /* for the engine model */
} rig_t;

static uint16_t cb_depth(void *ctx, unsigned slot)
{
    return ((const rig_t *)ctx)->depth[slot];
}

static void rig_init(rig_t *r, const km_dks_rec_t *recs, unsigned n, uint32_t t0)
{
    memset(r, 0, sizeof *r);
    km_dks_init(&r->d);
    CHECK(km_dks_set_records(&r->d, recs, n));
    memset(r->rec_of, 0xFF, sizeof r->rec_of);
    for (unsigned i = 0; i < n; i++) {
        r->t_act[i] = recs[i].t_act_mm100;
    }
    r->now = t0;
}

/* one pass of the main loop at the current time, with the engine model deciding which keys are down */
static void rig_pass(rig_t *r)
{
    km_dks_press_t list[KM_DKS_SLOTS];
    unsigned n = 0;
    for (unsigned s = 0; s < KM_DKS_SLOTS; s++) {
        if (r->rec_of[s] == 0xFFu) {
            continue;
        }
        const unsigned ta = r->t_act[r->rec_of[s]];
        if (!r->down[s] && r->depth[s] >= ta) {
            r->down[s] = true;
        } else if (r->down[s] && r->depth[s] + KM_DKS_RELEASE_BELOW_MM100 < ta) {
            r->down[s] = false;
        }
        if (r->down[s]) {
            list[n].slot = (uint8_t)s;
            list[n].rec = r->rec_of[s];
            n++;
        }
    }
    km_dks_update_ms(&r->d, list, n, r->now, cb_depth, r, &r->out);
    if (r->n_ch == 0u || r->ch[r->n_ch - 1u].n != r->out.n || memcmp(r->ch[r->n_ch - 1u].u, r->out.usage, r->out.n) != 0) {
        if (r->n_ch < MAXCH) {
            change_t *c = &r->ch[r->n_ch++];
            c->t = r->now;
            c->n = r->out.n;
            memcpy(c->u, r->out.usage, r->out.n < 8u ? r->out.n : 8u);
        }
    }
}

/* run ms milliseconds, one pass per millisecond */
static void rig_run(rig_t *r, unsigned ms)
{
    for (unsigned i = 0; i < ms; i++) {
        rig_pass(r);
        r->now++;
    }
}

/* move one key linearly to a depth over ms milliseconds, a pass per millisecond */
static void rig_ramp(rig_t *r, unsigned slot, unsigned to, unsigned ms)
{
    const int from = r->depth[slot];
    for (unsigned i = 1; i <= ms; i++) {
        r->depth[slot] = (uint16_t)(from + ((int)to - from) * (int)i / (int)ms);
        rig_pass(r);
        r->now++;
    }
}

static bool out_is(const rig_t *r, const char *want)
{
    if (strlen(want) != r->out.n) {
        return false;
    }
    for (unsigned i = 0; i < r->out.n; i++) {
        unsigned u = 0;
        switch (want[i]) {
        case 'A': u = U_A; break;
        case 'S': u = U_S; break;
        case 'D': u = U_D; break;
        case 'T': u = U_T; break;
        default:  u = U_W; break;
        }
        if (r->out.usage[i] != u) {
            return false;
        }
    }
    return true;
}

/* the history, as a list of strings: "" "A" "AS" ...; changes of the output only */
static bool hist_is(const rig_t *r, const char *const *want, unsigned n)
{
    if (r->n_ch != n) {
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        const change_t *c = &r->ch[i];
        if (strlen(want[i]) != c->n) {
            return false;
        }
        for (unsigned k = 0; k < c->n; k++) {
            const unsigned u = want[i][k] == 'A' ? U_A : want[i][k] == 'S' ? U_S : want[i][k] == 'D' ? U_D : want[i][k] == 'T' ? U_T : U_W;
            if (c->u[k] != u) {
                return false;
            }
        }
    }
    return true;
}

/* ---------------------------------------------------------------- 1. records */

static void dks_records(void)
{
    km_dks_rec_t r = rec_classic(0);
    CHECK(km_dks_rec_valid(&r));
    CHECK(!km_dks_rec_valid(NULL));
    CHECK_EQ(km_dks_t_bot(&r), 310);
    r.t_bot_mm100 = 250;
    CHECK_EQ(km_dks_t_bot(&r), 250);

    /* t_act */
    r = rec_make(40, 0, 0);
    CHECK(km_dks_rec_valid(&r));
    r = rec_make(39, 0, 0);
    CHECK(!km_dks_rec_valid(&r));
    r = rec_make(0, 0, 0);
    CHECK(!km_dks_rec_valid(&r));
    /* the gap: t_bot - t_act >= 40, t_bot <= 340 */
    r = rec_make(120, 160, 0);
    CHECK(km_dks_rec_valid(&r));
    r = rec_make(120, 159, 0);
    CHECK(!km_dks_rec_valid(&r));
    r = rec_make(120, 340, 0);
    CHECK(km_dks_rec_valid(&r));
    r = rec_make(120, 341, 0);
    CHECK(!km_dks_rec_valid(&r));
    r = rec_make(120, 100, 0);
    CHECK(!km_dks_rec_valid(&r));
    r = rec_make(280, 0, 0);                              /* default bottom 310: gap 30 */
    CHECK(!km_dks_rec_valid(&r));
    r = rec_make(270, 0, 0);
    CHECK(km_dks_rec_valid(&r));
    r = rec_make(300, 340, 0);
    CHECK(km_dks_rec_valid(&r));
    r = rec_make(301, 340, 0);
    CHECK(!km_dks_rec_valid(&r));
    r = rec_make(65535, 0, 0);
    CHECK(!km_dks_rec_valid(&r));
    /* flags and reserved bytes */
    r = rec_make(120, 0, 1);
    CHECK(km_dks_rec_valid(&r));
    r = rec_make(120, 0, 2);
    CHECK(!km_dks_rec_valid(&r));
    r = rec_make(120, 0, 0);
    r.rsvd[0] = 1;
    CHECK(!km_dks_rec_valid(&r));
    r.rsvd[0] = 0;
    r.rsvd[1] = 1;
    CHECK(!km_dks_rec_valid(&r));
    r.rsvd[1] = 0;
    r.rsvd[2] = 1;
    CHECK(!km_dks_rec_valid(&r));
    r.rsvd[2] = 0;
    CHECK(km_dks_rec_valid(&r));
    /* usages: 0 (unused), 0x04..0xE7 */
    for (unsigned u = 0; u < 256; u++) {
        for (unsigned a = 0; a < 4; a++) {
            r = rec_make(120, 0, 0);
            act_set(&r, a, u, 0x55);
            CHECK_EQ(km_dks_rec_valid(&r), u == 0u || (u >= 4u && u <= 0xE7u));
        }
    }
    r = rec_make(120, 0, 0);
    act_set(&r, 2, 0, 0xFF);                              /* an unused action keeps whatever ev bits it has */
    CHECK(km_dks_rec_valid(&r));

    /* engine points */
    uint16_t press = 0, release = 0;
    r = rec_make(120, 0, 0);
    CHECK(km_dks_engine_points(&r, &press, &release));
    CHECK_EQ(press, 120);
    CHECK_EQ(release, 110);
    r = rec_make(40, 0, 0);
    CHECK(km_dks_engine_points(&r, &press, &release));
    CHECK_EQ(press, 40);
    CHECK_EQ(release, 30);
    press = 7;
    r = rec_make(10, 0, 0);
    CHECK(!km_dks_engine_points(&r, &press, &release));
    CHECK_EQ(press, 7);
    r = rec_make(120, 0, 0);
    CHECK(!km_dks_engine_points(&r, NULL, &release));
    CHECK(!km_dks_engine_points(&r, &press, NULL));

    /* sets */
    km_dks_t d;
    km_dks_init(&d);
    km_dks_rec_t set[9];
    for (unsigned i = 0; i < 9; i++) {
        set[i] = rec_classic(0);
    }
    CHECK(km_dks_set_records(&d, set, 8));
    for (unsigned i = 0; i < 8; i++) {
        CHECK_EQ(d.ok[i], 1);
    }
    CHECK(!km_dks_set_records(&d, set, 9));
    for (unsigned i = 0; i < 8; i++) {
        CHECK_EQ(d.ok[i], 0);
    }
    CHECK(km_dks_set_records(&d, NULL, 0));
    CHECK(!km_dks_set_records(&d, NULL, 1));
    set[3].t_act_mm100 = 10;                              /* one bad record: none is used */
    CHECK(!km_dks_set_records(&d, set, 5));
    for (unsigned i = 0; i < 8; i++) {
        CHECK_EQ(d.ok[i], 0);
    }
    CHECK(km_dks_set_records(&d, set, 3));                /* the bad one is beyond n: not looked at */
    CHECK_EQ(d.ok[2], 1);
    CHECK_EQ(d.ok[3], 0);
    /* the event mask: which events have any action (an action with usage 0 does not count) */
    km_dks_rec_t m = rec_make(120, 0, 0);
    act_set(&m, 0, U_A, ev(KM_DKS_EV_PRESS, 0, 0, 0));
    act_set(&m, 1, 0, ev(0, 3, 3, 3));
    CHECK(km_dks_set_records(&d, &m, 1));
    CHECK_EQ(d.ev_mask[0], 1);
    act_set(&m, 2, U_S, ev(0, 0, KM_DKS_EV_TAP, 0));
    CHECK(km_dks_set_records(&d, &m, 1));
    CHECK_EQ(d.ev_mask[0], 5);
    /* setting records drops what was running */
    rig_t r1;
    km_dks_rec_t c = rec_classic(0);
    rig_init(&r1, &c, 1, 100);
    r1.rec_of[K1] = 0;
    r1.depth[K1] = 200;
    rig_run(&r1, 3);
    CHECK(out_is(&r1, "A"));
    CHECK(km_dks_set_records(&r1.d, &c, 1));
    CHECK(!km_dks_busy(&r1.d));
}

/* ---------------------------------------------------------------- 2. depth sequences */

static void dks_slow(uint32_t t0)
{
    rig_t r;
    km_dks_rec_t c = rec_classic(0);
    rig_init(&r, &c, 1, t0);
    r.rec_of[K1] = 0;
    rig_run(&r, 5);
    CHECK_EQ(r.out.n, 0);
    /* 0 -> 340 in 300 ms, hold, back in 300 ms */
    rig_ramp(&r, K1, 340, 300);
    rig_run(&r, 50);
    CHECK(out_is(&r, "AS"));
    rig_ramp(&r, K1, 0, 300);
    rig_run(&r, 30);
    CHECK(out_is(&r, ""));
    CHECK(!km_dks_busy(&r.d));
    const char *want[] = { "", "A", "AS", "A", "" };
    CHECK(hist_is(&r, want, 5));
    /* when did they happen? A at 1.20 mm: ms 106 of the ramp (120/340*300), S at 3.10 mm, S off at 2.90 mm, A off below 1.10 mm */
    CHECK(r.ch[1].t - (t0 + 5u) >= 105u && r.ch[1].t - (t0 + 5u) <= 108u);
    CHECK(r.ch[2].t - (t0 + 5u) >= 272u && r.ch[2].t - (t0 + 5u) <= 276u);
    CHECK_EQ(r.d.events_applied, 4);
    CHECK_EQ(r.d.queue_flushes, 0);
}

static void dks_fast(uint32_t t0)
{
    rig_t r;
    km_dks_rec_t c = rec_classic(0);
    rig_init(&r, &c, 1, t0);
    r.rec_of[K1] = 0;
    rig_run(&r, 5);
    /* fast press: the key is already at the bottom when it is first seen: events 1 and 2, in that order, not together */
    r.depth[K1] = 340;
    rig_run(&r, 30);
    const char *want1[] = { "", "A", "AS" };
    CHECK(hist_is(&r, want1, 3));
    CHECK(r.ch[2].t - r.ch[1].t >= KM_DKS_STEP_MS);       /* A was on the wire before S joined it */
    CHECK_EQ(r.ch[1].t, t0 + 5u);                         /* and the first event had no delay */
    /* fast release: events 3 and 4 in that order, a step apart */
    const uint32_t t_rel = r.now;
    r.depth[K1] = 0;
    rig_run(&r, 30);
    const char *want2[] = { "", "A", "AS", "A", "" };
    CHECK(hist_is(&r, want2, 5));
    CHECK_EQ(r.ch[3].t, t_rel);
    CHECK(r.ch[4].t - r.ch[3].t >= KM_DKS_STEP_MS);
    CHECK(!km_dks_busy(&r.d));
}

static void dks_partial(void)
{
    rig_t r;
    km_dks_rec_t c = rec_classic(0);
    rig_init(&r, &c, 1, 5000);
    r.rec_of[K1] = 0;
    /* partial press: past the start point, never to the bottom */
    rig_ramp(&r, K1, 250, 100);
    rig_run(&r, 20);
    CHECK(out_is(&r, "A"));
    rig_ramp(&r, K1, 0, 100);
    rig_run(&r, 30);
    CHECK(out_is(&r, ""));
    const char *want[] = { "", "A", "" };
    CHECK(hist_is(&r, want, 3));
    /* not even to the start point: nothing at all */
    rig_t q;
    rig_init(&q, &c, 1, 5000);
    q.rec_of[K1] = 0;
    rig_ramp(&q, K1, 119, 100);
    rig_ramp(&q, K1, 0, 100);
    CHECK_EQ(q.n_ch, 1);
    CHECK_EQ(q.d.events_applied, 0);
    /* press, stop between the start point and the release point (hysteresis of the engine model): A stays */
    rig_ramp(&q, K1, 125, 10);
    rig_ramp(&q, K1, 115, 10);
    CHECK(out_is(&q, "A"));
    rig_ramp(&q, K1, 105, 10);
    rig_run(&q, 20);
    CHECK(out_is(&q, ""));
}

static void dks_bottom_again(unsigned flags)
{
    rig_t r;
    km_dks_rec_t c = rec_classic(flags);
    rig_init(&r, &c, 1, 777);
    r.rec_of[K1] = 0;
    rig_ramp(&r, K1, 340, 100);
    rig_run(&r, 20);
    CHECK(out_is(&r, "AS"));
    rig_ramp(&r, K1, 250, 50);                            /* up past t_bot - 20: S released, key still down */
    rig_run(&r, 20);
    CHECK(out_is(&r, "A"));
    rig_ramp(&r, K1, 340, 50);                            /* down to the bottom again without a release */
    rig_run(&r, 20);
    CHECK(out_is(&r, flags != 0u ? "AS" : "A"));          /* only with the retrigger flag */
    rig_ramp(&r, K1, 250, 50);
    rig_run(&r, 20);
    CHECK(out_is(&r, "A"));
    rig_ramp(&r, K1, 0, 100);
    rig_run(&r, 30);
    CHECK(out_is(&r, ""));
    CHECK(!km_dks_busy(&r.d));
    /* a new press starts afresh: the bottom works again, also without the flag */
    rig_ramp(&r, K1, 340, 100);
    rig_run(&r, 20);
    CHECK(out_is(&r, "AS"));
    rig_ramp(&r, K1, 0, 100);
    rig_run(&r, 30);
    CHECK(out_is(&r, ""));
}

static void dks_hysteresis(void)
{
    rig_t r;
    km_dks_rec_t c = rec_classic(1);                      /* retrigger on: the hysteresis alone must stop the flicker */
    rig_init(&r, &c, 1, 100);
    r.rec_of[K1] = 0;
    rig_ramp(&r, K1, 305, 50);
    rig_run(&r, 10);
    CHECK(out_is(&r, "A"));
    /* hover around the bottom point with +-19 (the release point is 20 below): S goes down once and stays */
    uint32_t seed = 12345;
    for (unsigned i = 0; i < 20000; i++) {
        seed = seed * 1664525u + 1013904223u;
        r.depth[K1] = (uint16_t)(310 + (int)((seed >> 16) % 39u) - 19);
        rig_pass(&r);
        r.now++;
    }
    CHECK(out_is(&r, "AS"));
    CHECK(r.n_ch <= 4);
    /* just not enough to release (291 > 290), then 290: released */
    r.depth[K1] = 291;
    rig_run(&r, 10);
    CHECK(out_is(&r, "AS"));
    r.depth[K1] = 290;
    rig_run(&r, 10);
    CHECK(out_is(&r, "A"));
    /* and the bottom point itself: 309 does not press S, 310 does (with the flag: again) */
    r.depth[K1] = 309;
    rig_run(&r, 10);
    CHECK(out_is(&r, "A"));
    r.depth[K1] = 310;
    rig_run(&r, 10);
    CHECK(out_is(&r, "AS"));
    r.depth[K1] = 0;
    rig_run(&r, 40);
    CHECK(out_is(&r, ""));
}

static void dks_taps(void)
{
    km_dks_rec_t c = rec_make(120, 0, 0);
    act_set(&c, 0, U_A, ev(KM_DKS_EV_TAP, 0, 0, 0));                    /* tap A when the key goes down */
    act_set(&c, 1, U_S, ev(0, KM_DKS_EV_TAP, 0, KM_DKS_EV_TAP));        /* tap S at the bottom and at the release */
    rig_t r;
    rig_init(&r, &c, 1, 9000);
    r.rec_of[K1] = 0;
    r.depth[K1] = 150;
    rig_run(&r, 3);
    CHECK(out_is(&r, "A"));
    rig_run(&r, 6);
    CHECK(out_is(&r, "A"));                               /* a tap lasts KM_DKS_TAP_MS */
    rig_run(&r, 2);
    CHECK(out_is(&r, ""));
    CHECK_EQ(r.ch[0].t, 9000u);                           /* the first pass already sees the key */
    CHECK_EQ(r.ch[1].t, 9000u + KM_DKS_TAP_MS);           /* and the pulse ends exactly KM_DKS_TAP_MS later */
    /* the key is still down: nothing else happens */
    rig_run(&r, 50);
    CHECK_EQ(r.out.n, 0);
    /* release at once after a tap: the pulse still lasts its time and is not cut by event 4 */
    r.depth[K1] = 0;
    rig_run(&r, 3);
    CHECK(out_is(&r, "S"));
    rig_run(&r, 20);
    CHECK(out_is(&r, ""));
    CHECK(!km_dks_busy(&r.d));
    /* a quick slam: down and up within a few milliseconds, the tap still appears for its whole length */
    rig_t q;
    rig_init(&q, &c, 1, 100);
    q.rec_of[K1] = 0;
    q.depth[K1] = 340;
    rig_run(&q, 2);
    q.depth[K1] = 0;
    rig_run(&q, 60);
    CHECK(!km_dks_busy(&q.d));
    unsigned a_ms = 0, s_ms = 0;
    for (unsigned i = 0; i + 1 < q.n_ch; i++) {
        if (q.ch[i].n > 0u && q.ch[i].u[0] == U_A) {
            a_ms += q.ch[i + 1].t - q.ch[i].t;
        }
        for (unsigned k = 0; k < q.ch[i].n; k++) {
            if (q.ch[i].u[k] == U_S) {
                s_ms += q.ch[i + 1].t - q.ch[i].t;
            }
        }
    }
    CHECK(a_ms >= KM_DKS_TAP_MS);
    CHECK(s_ms >= KM_DKS_TAP_MS);
    /* tap on an action that is held down: nothing changes, the action stays down until released */
    km_dks_rec_t h = rec_make(120, 0, 0);
    act_set(&h, 0, U_A, ev(KM_DKS_EV_PRESS, KM_DKS_EV_TAP, 0, KM_DKS_EV_RELEASE));
    rig_t p;
    rig_init(&p, &h, 1, 100);
    p.rec_of[K1] = 0;
    p.depth[K1] = 340;
    rig_run(&p, 60);
    CHECK(out_is(&p, "A"));
    p.depth[K1] = 0;
    rig_run(&p, 20);
    CHECK(out_is(&p, ""));
    const char *want[] = { "A", "" };                     /* (the first pass already sees the key: no initial empty entry) */
    CHECK(hist_is(&p, want, 2));

    /* a press after a tap on the same action: the action stays down (the pulse no longer ends it) */
    km_dks_rec_t tp = rec_make(120, 0, 0);
    act_set(&tp, 0, U_A, ev(KM_DKS_EV_TAP, KM_DKS_EV_PRESS, 0, KM_DKS_EV_RELEASE));
    rig_t s;
    rig_init(&s, &tp, 1, 100);
    s.rec_of[K1] = 0;
    s.depth[K1] = 340;
    rig_run(&s, 60);
    CHECK(out_is(&s, "A"));
    s.depth[K1] = 0;
    rig_run(&s, 20);
    CHECK(out_is(&s, ""));

    /* a second tap on a running pulse starts it over: tap at 100, tap again at 104, so A is down until 114 */
    km_dks_rec_t t2 = rec_make(120, 0, 0);
    act_set(&t2, 0, U_A, ev(KM_DKS_EV_TAP, KM_DKS_EV_TAP, 0, 0));
    rig_t u;
    rig_init(&u, &t2, 1, 100);
    u.rec_of[K1] = 0;
    u.depth[K1] = 340;
    rig_run(&u, 40);
    CHECK_EQ(u.n_ch, 2);
    CHECK_EQ(u.ch[0].t, 100u);
    CHECK_EQ(u.ch[1].t, 100u + KM_DKS_STEP_MS + KM_DKS_TAP_MS);

    /* events nobody asked for are not applied: only events 1, 3 and 4 have actions here, so event 2 is not even queued */
    km_dks_rec_t no2 = rec_make(120, 0, 0);
    act_set(&no2, 0, U_A, ev(KM_DKS_EV_PRESS, 0, 0, KM_DKS_EV_RELEASE));
    act_set(&no2, 1, U_S, ev(0, 0, KM_DKS_EV_TAP, 0));
    rig_t v;
    rig_init(&v, &no2, 1, 100);
    v.rec_of[K1] = 0;
    v.depth[K1] = 340;
    rig_run(&v, 30);
    v.depth[K1] = 0;
    rig_run(&v, 40);
    CHECK_EQ(v.d.events_applied, 3);
    CHECK(!km_dks_busy(&v.d));
}

static void dks_event4_releases_all(void)
{
    /* a table that presses at event 1 and 2 and releases nothing: the key up releases them all */
    km_dks_rec_t c = rec_make(120, 0, 0);
    act_set(&c, 0, U_A, ev(KM_DKS_EV_PRESS, 0, 0, 0));
    act_set(&c, 1, U_S, ev(0, KM_DKS_EV_PRESS, 0, 0));
    act_set(&c, 2, U_D, ev(KM_DKS_EV_PRESS, 0, 0, 0));
    rig_t r;
    rig_init(&r, &c, 1, 100);
    r.rec_of[K1] = 0;
    rig_ramp(&r, K1, 340, 100);
    rig_run(&r, 20);
    CHECK(out_is(&r, "ASD"));
    rig_ramp(&r, K1, 250, 50);
    rig_run(&r, 20);
    CHECK(out_is(&r, "ASD"));                             /* nothing releases them on the way up */
    rig_ramp(&r, K1, 0, 100);
    rig_run(&r, 30);
    CHECK(out_is(&r, ""));
    CHECK(!km_dks_busy(&r.d));
    /* the same with the key released from the bottom in one step */
    r.depth[K1] = 340;
    rig_run(&r, 40);
    CHECK(out_is(&r, "ASD"));
    r.depth[K1] = 0;
    rig_run(&r, 30);
    CHECK(out_is(&r, ""));
    /* a key that only releases (no press anywhere): never any output */
    km_dks_rec_t e = rec_make(120, 0, 0);
    act_set(&e, 0, U_A, ev(KM_DKS_EV_RELEASE, KM_DKS_EV_RELEASE, KM_DKS_EV_RELEASE, KM_DKS_EV_RELEASE));
    rig_t q;
    rig_init(&q, &e, 1, 100);
    q.rec_of[K1] = 0;
    rig_ramp(&q, K1, 340, 50);
    rig_ramp(&q, K1, 0, 50);
    CHECK_EQ(q.n_ch, 1);
    /* a record without any action: the key is tracked and freed, nothing is output */
    km_dks_rec_t n = rec_make(120, 0, 0);
    rig_t z;
    rig_init(&z, &n, 1, 100);
    z.rec_of[K1] = 0;
    rig_ramp(&z, K1, 340, 50);
    CHECK(!km_dks_busy(&z.d) || z.out.n == 0u);
    rig_ramp(&z, K1, 0, 50);
    rig_run(&z, 20);
    CHECK(!km_dks_busy(&z.d));
}

static void dks_union(void)
{
    /* two actions of one key with the same usage: down until the last one is released */
    km_dks_rec_t c = rec_make(120, 0, 0);
    act_set(&c, 0, U_A, ev(KM_DKS_EV_PRESS, 0, 0, KM_DKS_EV_RELEASE));
    act_set(&c, 1, U_A, ev(0, KM_DKS_EV_PRESS, KM_DKS_EV_RELEASE, 0));
    rig_t r;
    rig_init(&r, &c, 1, 100);
    r.rec_of[K1] = 0;
    rig_ramp(&r, K1, 340, 100);
    rig_run(&r, 20);
    CHECK(out_is(&r, "A"));                               /* listed once */
    rig_ramp(&r, K1, 250, 50);
    rig_run(&r, 20);
    CHECK(out_is(&r, "A"));                               /* action 1 let go, action 0 still holds A */
    rig_ramp(&r, K1, 0, 100);
    rig_run(&r, 30);
    CHECK(out_is(&r, ""));

    /* two keys with the same usage: A stays down while either key holds it ("press while another action is held") */
    km_dks_rec_t two[2];
    two[0] = rec_classic(0);
    two[1] = rec_make(150, 0, 0);
    act_set(&two[1], 0, U_A, ev(KM_DKS_EV_PRESS, 0, 0, KM_DKS_EV_RELEASE));
    act_set(&two[1], 1, U_D, ev(0, KM_DKS_EV_PRESS, KM_DKS_EV_RELEASE, 0));
    rig_t q;
    rig_init(&q, two, 2, 100);
    q.rec_of[K1] = 0;
    q.rec_of[K2] = 1;
    rig_ramp(&q, K1, 200, 50);
    rig_run(&q, 10);
    CHECK(out_is(&q, "A"));
    rig_ramp(&q, K2, 200, 50);
    rig_run(&q, 10);
    CHECK(out_is(&q, "A"));                               /* key 2 pressed A as well: still one A */
    rig_ramp(&q, K2, 340, 50);                            /* key 2 bottoms out: D joins */
    rig_run(&q, 10);
    CHECK(out_is(&q, "AD"));
    rig_ramp(&q, K1, 340, 50);                            /* key 1 bottoms out: S joins (order: key table, then action) */
    rig_run(&q, 10);
    CHECK_EQ(q.out.n, 3);
    CHECK(q.out.usage[0] != q.out.usage[1] && q.out.usage[1] != q.out.usage[2] && q.out.usage[0] != q.out.usage[2]);
    rig_ramp(&q, K1, 0, 50);                              /* key 1 up: A stays (key 2), S gone */
    rig_run(&q, 20);
    CHECK(out_is(&q, "AD"));
    rig_ramp(&q, K2, 0, 50);
    rig_run(&q, 30);
    CHECK(out_is(&q, ""));
    CHECK(!km_dks_busy(&q.d));

    /* a key that is released and pressed again while its last events are still waiting in the queue: no lost release,
     * no stuck key */
    rig_t w;
    rig_init(&w, &c, 1, 100);
    w.rec_of[K1] = 0;
    for (unsigned i = 0; i < 40; i++) {
        w.depth[K1] = 340;
        rig_run(&w, 1 + i % 3u);
        w.depth[K1] = 0;
        rig_run(&w, 1 + i % 2u);
    }
    rig_run(&w, KM_DKS_MAX_SETTLE_MS + 2u);
    CHECK_EQ(w.out.n, 0);
    CHECK(!km_dks_busy(&w.d));
}

static void dks_two_records(void)
{
    /* two keys with different records and different usages: each key uses its own record from the very first pass */
    km_dks_rec_t two[2];
    two[0] = rec_classic(0);
    two[1] = rec_make(150, 0, 0);
    act_set(&two[1], 0, U_W, ev(KM_DKS_EV_PRESS, 0, 0, KM_DKS_EV_RELEASE));
    act_set(&two[1], 1, U_D, ev(0, KM_DKS_EV_PRESS, KM_DKS_EV_RELEASE, 0));
    rig_t r;
    rig_init(&r, two, 2, 100);
    r.rec_of[K1] = 0;
    r.rec_of[K2] = 1;
    r.depth[K2] = 200;
    rig_run(&r, 3);
    CHECK(out_is(&r, "W"));
    r.depth[K1] = 200;
    rig_run(&r, 3);
    CHECK(out_is(&r, "WA"));                            /* key table order: K2 was tracked first */
    r.depth[K2] = 340;
    rig_run(&r, 10);
    CHECK(out_is(&r, "WDA"));
    r.depth[K1] = 340;
    rig_run(&r, 10);
    CHECK_EQ(r.out.n, 4);
    r.depth[K1] = 0;
    rig_run(&r, 20);
    CHECK(out_is(&r, "WD"));
    r.depth[K2] = 0;
    rig_run(&r, 30);
    CHECK(out_is(&r, ""));
    CHECK(!km_dks_busy(&r.d));
}

static void dks_rec_change(void)
{
    /* the same slot pressed with a different record while the old key's events are still running */
    km_dks_rec_t two[2];
    two[0] = rec_classic(0);
    two[1] = rec_make(120, 0, 0);
    act_set(&two[1], 0, U_D, ev(KM_DKS_EV_PRESS, 0, 0, KM_DKS_EV_RELEASE));
    rig_t r;
    rig_init(&r, two, 2, 100);
    r.rec_of[K1] = 0;
    r.depth[K1] = 340;
    rig_run(&r, 30);
    CHECK(out_is(&r, "AS"));
    r.rec_of[K1] = 1;                                     /* the layer logic hands out another record for the same slot */
    rig_run(&r, 40);
    CHECK(out_is(&r, "D"));                               /* the old press was closed (everything of it released), the new one started */
    r.depth[K1] = 0;
    rig_run(&r, 30);
    CHECK(out_is(&r, ""));
    CHECK(!km_dks_busy(&r.d));
}

static void dks_wrap(void)
{
    /* the whole slow scenario with the millisecond counter wrapping in the middle of it */
    dks_slow(0xFFFFFFFFu - 150u);
    dks_slow(0xFFFFFFFFu - 400u);
    dks_fast(0xFFFFFFFFu - 3u);
    dks_fast(0xFFFFFFF0u);
}

static void dks_run_all_depth_cases(void)
{
    dks_slow(1000);
    dks_slow(0);
    dks_fast(1000);
    dks_fast(0);
    dks_partial();
    dks_bottom_again(0);
    dks_bottom_again(1);
    dks_hysteresis();
    dks_taps();
    dks_event4_releases_all();
    dks_union();
    dks_two_records();
    dks_rec_change();
    dks_wrap();
}

/* ---------------------------------------------------------------- 3. the real engine */

static uint16_t counts_for_depth(unsigned mm100)
{
    static uint16_t table[KM_TRAVEL_MM100 + 1u];
    static bool built;
    if (!built) {
        const km_depth_cal_t cal = { 3300, 2300 };
        for (unsigned d = 0; d <= KM_TRAVEL_MM100; d++) {
            unsigned c = 3300;
            while (c > 2300 && km_depth_model_mm100(&cal, (uint16_t)c) < d) {
                c--;
            }
            table[d] = (uint16_t)c;
        }
        built = true;
    }
    return table[mm100 > KM_TRAVEL_MM100 ? KM_TRAVEL_MM100 : mm100];
}

static uint16_t eng_depth(void *ctx, unsigned slot)
{
    return km_engine_depth((const km_engine_t *)ctx, slot);
}

typedef struct {
    km_engine_t  e;
    km_dks_t     d;
    km_dks_out_t out;
    uint32_t     frame;
    uint32_t     seed;
    uint32_t     cyc;
    unsigned     target[KM_DKS_SLOTS];     /* wanted depth per key (mm100), moves a little per frame towards it */
    unsigned     cur_q4[KM_DKS_SLOTS];     /* the depth the sensor really has, mm100 << 4 */
    unsigned     step_q4;                  /* max movement per frame, mm100 << 4 */
    unsigned     noise;                    /* +- counts of noise */
    uint8_t      rec_of[KM_DKS_SLOTS];
    char         hist[32][8];             /* the output after each change, as letters (the first entry is the first frame) */
    unsigned     hist_n;
    uint32_t     hist_t[32];
} erig_t;

static void erig_init(erig_t *r, const km_dks_rec_t *recs, unsigned n)
{
    memset(r, 0, sizeof *r);
    uint16_t sums[KM_ENGINE_SLOTS];
    for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
        sums[i] = 3300;
    }
    km_engine_init(&r->e);
    km_engine_calibrate_begin(&r->e);
    for (unsigned i = 0; i < 32; i++) {
        km_engine_calibrate_idle(&r->e, sums);
    }
    CHECK_EQ(km_engine_calibrate_finish(&r->e), KM_ENGINE_CAL_OK);
    km_dks_init(&r->d);
    CHECK(km_dks_set_records(&r->d, recs, n));
    memset(r->rec_of, 0xFF, sizeof r->rec_of);
    r->seed = 99;
    r->cyc = 5000;
    r->step_q4 = 16u * 400u;
}

static void erig_assign(erig_t *r, unsigned slot, unsigned rec)
{
    km_engine_key_cfg_t c;
    uint16_t press, release;
    CHECK(km_dks_engine_points(&r->d.rec[rec], &press, &release));
    km_engine_key_cfg_default(&c);
    c.mode = KM_ENGINE_TRIG_FIXED;
    c.press_mm100 = press;
    c.release_mm100 = release;
    CHECK(km_engine_set_key_cfg(&r->e, slot, &c));
    r->rec_of[slot] = (uint8_t)rec;
}

static void erig_frames(erig_t *r, unsigned frames)
{
    for (unsigned f = 0; f < frames; f++) {
        uint16_t sums[KM_ENGINE_SLOTS];
        for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
            sums[i] = 3300;
        }
        for (unsigned s = 0; s < KM_DKS_SLOTS; s++) {
            if (r->rec_of[s] == 0xFFu) {
                continue;
            }
            const unsigned tgt = r->target[s] << 4;
            if (r->cur_q4[s] + r->step_q4 < tgt) {
                r->cur_q4[s] += r->step_q4;
            } else if (r->cur_q4[s] > tgt + r->step_q4) {
                r->cur_q4[s] -= r->step_q4;
            } else {
                r->cur_q4[s] = tgt;
            }
            int c = (int)counts_for_depth(r->cur_q4[s] >> 4);
            r->seed = r->seed * 1664525u + 1013904223u;
            if (r->noise != 0u) {
                c += (int)((r->seed >> 16) % (2u * r->noise + 1u)) - (int)r->noise;
            }
            sums[s] = (uint16_t)c;
        }
        km_engine_update(&r->e, sums);
        unsigned ne;
        const uint8_t *down = km_engine_down_slots(&r->e, &ne);
        km_dks_press_t list[KM_ENGINE_MAX_DOWN];
        unsigned n = 0;
        for (unsigned i = 0; i < ne; i++) {
            if (r->rec_of[down[i]] != 0xFFu && n < KM_ENGINE_MAX_DOWN) {
                list[n].slot = down[i];
                list[n].rec = r->rec_of[down[i]];
                n++;
            }
        }
        r->cyc += 10400u;                                /* one 43.3 us frame at 240 MHz */
        km_dks_update_cycles(&r->d, list, n, r->cyc, 240000u, eng_depth, &r->e, &r->out);
        char now[16] = { 0 };
        for (unsigned i = 0; i < r->out.n && i < 8u; i++) {
            now[i] = r->out.usage[i] == U_A ? 'A' : r->out.usage[i] == U_S ? 'S' : r->out.usage[i] == U_D ? 'D' : r->out.usage[i] == U_T ? 'T' : '?';
        }
        if (r->hist_n == 0u || strncmp(r->hist[r->hist_n - 1u], now, 8) != 0) {
            if (r->hist_n < 32u) {
                memset(r->hist[r->hist_n], 0, 8);
                memcpy(r->hist[r->hist_n], now, strlen(now) < 8u ? strlen(now) : 7u);
                r->hist_t[r->hist_n] = f;
                r->hist_n++;
            }
        }
    }
}

static bool ehist_is(const erig_t *r, const char *const *want, unsigned n)
{
    if (r->hist_n != n) {
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        if (strncmp(r->hist[i], want[i], 8) != 0) {
            return false;
        }
    }
    return true;
}

static void dks_engine_rig(void)
{
    km_dks_rec_t c = rec_classic(0);
    const char *want[] = { "", "A", "AS", "A", "" };
    const char *want_partial[] = { "", "A", "" };
    const char *want_fast[] = { "", "A", "AS", "A", "" };

    /* slow press and release with 1.2 counts of noise... of +-14 counts: the real engine, filter, debounce and depth model */
    erig_t r;
    erig_init(&r, &c, 1);
    erig_assign(&r, K1, 0);
    r.noise = 14;
    r.step_q4 = 16u * 2u / 3u;                            /* 0.0067 mm per frame = about 0.15 mm per ms: a slow finger */
    r.target[K1] = 340;
    erig_frames(&r, 3000);
    CHECK_EQ(r.out.n, 2);
    r.target[K1] = 0;
    erig_frames(&r, 3000);
    CHECK_EQ(r.out.n, 0);
    CHECK(ehist_is(&r, want, 5));
    CHECK(!km_dks_busy(&r.d));

    /* fast press and release (a slam: 3.4 mm in 0.4 ms steps of 1 mm per frame) */
    erig_t q;
    erig_init(&q, &c, 1);
    erig_assign(&q, K1, 0);
    q.noise = 14;
    q.step_q4 = 16u * 100u;
    q.target[K1] = 340;
    erig_frames(&q, 2000);
    q.target[K1] = 0;
    erig_frames(&q, 2000);
    CHECK(ehist_is(&q, want_fast, 5));
    CHECK(!km_dks_busy(&q.d));

    /* partial press with noise: only A */
    erig_t p;
    erig_init(&p, &c, 1);
    erig_assign(&p, K1, 0);
    p.noise = 14;
    p.step_q4 = 16u * 3u;
    p.target[K1] = 250;
    erig_frames(&p, 2000);
    p.target[K1] = 0;
    erig_frames(&p, 2000);
    CHECK(ehist_is(&p, want_partial, 3));

    /* resting at the bottom point with noise (+-14 counts is about +-0.12 mm near the bottom): S goes down once */
    erig_t h;
    km_dks_rec_t rr = rec_classic(1);
    erig_init(&h, &rr, 1);
    erig_assign(&h, K1, 0);
    h.noise = 14;
    h.step_q4 = 16u * 3u;
    h.target[K1] = 312;
    erig_frames(&h, 20000);
    CHECK(h.hist_n <= 3u);
    CHECK_EQ(h.out.n, 2);
    h.target[K1] = 0;
    erig_frames(&h, 2000);
    CHECK_EQ(h.out.n, 0);

    /* two DKS keys (two records) and a normal key's neighbour: independent outputs */
    km_dks_rec_t two[2];
    two[0] = rec_classic(0);
    two[1] = rec_make(200, 280, 0);
    act_set(&two[1], 0, U_D, ev(KM_DKS_EV_PRESS, 0, 0, KM_DKS_EV_RELEASE));
    act_set(&two[1], 1, U_T, ev(0, KM_DKS_EV_TAP, 0, 0));
    erig_t t;
    erig_init(&t, two, 2);
    erig_assign(&t, K1, 0);
    erig_assign(&t, K2, 1);
    t.noise = 10;
    t.step_q4 = 16u * 2u;
    t.target[K1] = 340;
    t.target[K2] = 300;
    erig_frames(&t, 4000);
    CHECK(t.out.n >= 2u && t.out.n <= 4u);
    t.target[K1] = 0;
    t.target[K2] = 0;
    erig_frames(&t, 4000);
    CHECK_EQ(t.out.n, 0);
    CHECK(!km_dks_busy(&t.d));

    /* engine fault: a key that reads below the fault level is forced up by the engine, the DKS key is released */
    erig_t f;
    erig_init(&f, &c, 1);
    erig_assign(&f, K1, 0);
    f.step_q4 = 16u * 40u;
    f.target[K1] = 340;
    erig_frames(&f, 600);
    CHECK_EQ(f.out.n, 2);
    km_engine_set_slot_enabled(&f.e, K1, false);          /* "disabled by the caller": forced up */
    erig_frames(&f, 400);
    CHECK_EQ(f.out.n, 0);
    CHECK(!km_dks_busy(&f.d));
}

/* ---------------------------------------------------------------- 4. limits */

static void dks_limits(void)
{
    /* more DKS keys than the table holds: the extra one is ignored, counted, never stuck */
    km_dks_rec_t c = rec_classic(0);
    rig_t r;
    rig_init(&r, &c, 1, 100);
    unsigned keys[9];
    unsigned nk = 0;
    for (unsigned s = 0; s < KM_DKS_SLOTS && nk < 9u; s++) {
        keys[nk++] = s;
    }
    for (unsigned i = 0; i < 9; i++) {
        r.rec_of[keys[i]] = 0;
        r.depth[keys[i]] = 200;
    }
    rig_run(&r, 30);
    CHECK_EQ(r.out.n, 1);                                 /* all of them hold A: once */
    CHECK(r.d.overflow_presses >= 1u);
    for (unsigned i = 0; i < 9; i++) {
        r.depth[keys[i]] = 0;
    }
    rig_run(&r, 40);
    CHECK_EQ(r.out.n, 0);
    CHECK(!km_dks_busy(&r.d));
    /* the 9th key, once a slot is free, works */
    r.depth[keys[8]] = 200;
    rig_run(&r, 10);
    CHECK(out_is(&r, "A"));
    r.depth[keys[8]] = 0;
    rig_run(&r, 40);
    CHECK_EQ(r.out.n, 0);

    /* unusable record indices and slots are ignored */
    km_dks_t d;
    km_dks_init(&d);
    CHECK(km_dks_set_records(&d, &c, 1));
    km_dks_press_t bad[6] = { { K1, 1 }, { K1, 8 }, { K1, 255 }, { 200, 0 }, { 76, 0 }, { 255, 0 } };
    km_dks_out_t out;
    CHECK_EQ(km_dks_update_ms(&d, bad, 6, 100, NULL, NULL, &out), 0);
    CHECK(!km_dks_busy(&d));
    /* no records at all: nothing works */
    km_dks_t e;
    km_dks_init(&e);
    km_dks_press_t one = { K1, 0 };
    CHECK_EQ(km_dks_update_ms(&e, &one, 1, 100, NULL, NULL, &out), 0);
    CHECK(!km_dks_busy(&e));

    /* a key listed twice counts once */
    km_dks_press_t twice[2] = { { K1, 0 }, { K1, 0 } };
    CHECK_EQ(km_dks_update_ms(&d, twice, 2, 100, NULL, NULL, &out), 1);
    CHECK_EQ(out.usage[0], U_A);
    CHECK_EQ(km_dks_update_ms(&d, NULL, 0, 101, NULL, NULL, &out), 1);   /* the release waits no longer than a step */
    CHECK_EQ(km_dks_update_ms(&d, NULL, 0, 110, NULL, NULL, &out), 0);
    CHECK(!km_dks_busy(&d));

    /* no depth callback: no events 2 and 3, the rest works */
    km_dks_press_t one2 = { K1, 0 };
    CHECK_EQ(km_dks_update_ms(&d, &one2, 1, 200, NULL, NULL, &out), 1);
    CHECK_EQ(km_dks_update_ms(&d, NULL, 0, 210, NULL, NULL, &out), 0);

    /* a full event queue is flushed, nothing is lost: depth jumps across the bottom point on every pass, retrigger on */
    km_dks_rec_t fl = rec_classic(1);
    rig_t q;
    rig_init(&q, &fl, 1, 100);
    q.rec_of[K1] = 0;
    for (unsigned i = 0; i < 400; i++) {
        q.depth[K1] = (i & 1u) ? 340u : 200u;
        rig_pass(&q);
        q.now += 1;
        if (i % 4u == 3u) {
            q.now += 0;
        }
    }
    q.depth[K1] = 0;
    rig_run(&q, 60);
    CHECK_EQ(q.out.n, 0);
    CHECK(!km_dks_busy(&q.d));
    CHECK(q.d.events_applied >= 100u);
    /* the same at 0.25 ms per pass: the queue is really full at times (flush) */
    rig_t w;
    rig_init(&w, &fl, 1, 100);
    w.rec_of[K1] = 0;
    uint32_t seen_flushes = 0;
    for (unsigned i = 0; i < 4000; i++) {
        w.depth[K1] = (i & 1u) ? 340u : 200u;
        rig_pass(&w);
        if (w.d.queue_flushes != seen_flushes) {          /* a flush applies everything that waited: at most the new event is left */
            seen_flushes = w.d.queue_flushes;
            for (unsigned k = 0; k < KM_DKS_KEYS; k++) {
                CHECK(w.d.key[k].q_n <= 1u);
            }
        }
        if (i % 4u == 3u) {
            w.now++;
        }
    }
    CHECK(w.d.queue_flushes > 0u);
    w.depth[K1] = 0;
    rig_run(&w, 80);
    CHECK_EQ(w.out.n, 0);
    CHECK(!km_dks_busy(&w.d));

    /* a very long time with nothing to do and then a press: no wait for an old clock */
    rig_t lg;
    rig_init(&lg, &c, 1, 100);
    lg.rec_of[K1] = 0;
    lg.depth[K1] = 340;
    rig_run(&lg, 5);
    lg.now += 0xA0000000u;                                /* the key was held for 33 days */
    rig_run(&lg, 2);
    lg.depth[K1] = 0;
    rig_run(&lg, 50);
    CHECK_EQ(lg.out.n, 0);
    CHECK(!km_dks_busy(&lg.d));

    /* an unknown depth (0) while the key is bottomed does not count as "came back up" */
    rig_t u;
    rig_init(&u, &c, 1, 100);
    u.rec_of[K1] = 0;
    u.depth[K1] = 340;
    rig_run(&u, 30);
    CHECK(out_is(&u, "AS"));
    u.depth[K1] = 0;                                      /* the callback says 0 but the key is still listed as down */
    for (unsigned i = 0; i < 20; i++) {
        km_dks_press_t pr = { K1, 0 };
        km_dks_update_ms(&u.d, &pr, 1, u.now++, cb_depth, &u, &u.out);
    }
    CHECK(out_is(&u, "AS"));

    /* reset: everything drops at once, also taps */
    km_dks_rec_t tp = rec_make(120, 0, 0);
    act_set(&tp, 0, U_A, ev(KM_DKS_EV_PRESS, KM_DKS_EV_TAP, 0, 0));
    act_set(&tp, 1, U_S, ev(KM_DKS_EV_TAP, 0, 0, 0));
    rig_t z;
    rig_init(&z, &tp, 1, 100);
    z.rec_of[K1] = 0;
    z.depth[K1] = 340;
    rig_run(&z, 6);
    CHECK_EQ(z.out.n, 2);
    km_dks_reset(&z.d);
    CHECK(!km_dks_busy(&z.d));
    km_dks_update_ms(&z.d, NULL, 0, z.now, NULL, NULL, &z.out);
    CHECK_EQ(z.out.n, 0);
    CHECK_EQ(z.d.resets, 1);
    /* after a reset a key that is still down starts a new press (events from 1 on) */
    z.down[K1] = true;
    z.now += 1;
    rig_pass(&z);
    CHECK(z.out.n >= 1u);
    z.depth[K1] = 0;
    rig_run(&z, 60);
    CHECK_EQ(z.out.n, 0);
}

/* ---------------------------------------------------------------- 5. random sequences */

static uint32_t g_rng = 0x1234567u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static bool usage_possible(const km_dks_rec_t *recs, unsigned n, unsigned u)
{
    for (unsigned i = 0; i < n; i++) {
        for (unsigned a = 0; a < KM_DKS_ACTIONS; a++) {
            if (recs[i].act[a].usage == u) {
                return true;
            }
        }
    }
    return false;
}

/* the internal books agree: slot map, count, and the output is the union of what the keys hold */
static unsigned dks_books_bad(const km_dks_t *d, const km_dks_out_t *out)
{
    unsigned bad = 0, used = 0;
    uint8_t want[KM_DKS_OUT_MAX];
    unsigned nw = 0;
    for (unsigned i = 0; i < KM_DKS_KEYS; i++) {
        const km_dks_key_t *k = &d->key[i];
        if (k->slot == 0xFFu) {
            bad += k->held != 0u || k->q_n != 0u;
            continue;
        }
        used++;
        bad += k->slot >= KM_DKS_SLOTS || d->where[k->slot] != i + 1u;
        for (unsigned a = 0; a < KM_DKS_ACTIONS; a++) {
            if ((k->held & (1u << a)) != 0u) {
                bool dup = false;
                for (unsigned j = 0; j < nw; j++) {
                    dup = dup || want[j] == k->usage[a];
                }
                if (!dup && nw < sizeof want) {
                    want[nw++] = k->usage[a];
                }
            }
        }
    }
    bad += used != d->n_tracked;
    unsigned mapped = 0;
    for (unsigned s = 0; s < KM_DKS_SLOTS; s++) {
        mapped += d->where[s] != 0u;
    }
    bad += mapped != used;
    bad += out->n != nw || memcmp(out->usage, want, nw) != 0;
    return bad;
}

static void dks_random(unsigned rounds)
{
    static const uint8_t pool[6] = { U_A, U_S, U_D, U_T, U_W, 0xE1 };   /* few usages, so that actions overlap; one modifier */
    for (unsigned round = 0; round < rounds; round++) {
        km_dks_rec_t recs[KM_DKS_MAX];
        const unsigned nrec = 1u + rnd() % KM_DKS_MAX;
        for (unsigned i = 0; i < nrec; i++) {
            const unsigned ta = 40u + rnd() % 200u;
            const unsigned tb = (rnd() % 4u == 0u) ? 0u : ta + 40u + rnd() % (340u - ta - 40u + 1u);
            recs[i] = rec_make(ta, tb, rnd() & 1u);
            if (tb == 0u && ta + 40u > 310u) {
                recs[i].t_act_mm100 = 100;
            }
            for (unsigned a = 0; a < 4u; a++) {
                if (rnd() % 5u != 0u) {
                    act_set(&recs[i], a, pool[rnd() % 6u], (uint8_t)rnd());
                }
            }
        }
        rig_t r;
        rig_init(&r, recs, nrec, rnd());
        unsigned keys[10];
        for (unsigned i = 0; i < 10u; i++) {
            keys[i] = 20u + i;
            r.rec_of[keys[i]] = (uint8_t)(rnd() % nrec);
        }
        unsigned bad = 0;
        uint32_t last_list_ms = r.now;
        bool had_list = false;
        for (unsigned pass = 0; pass < 3000u; pass++) {
            const unsigned what = rnd() % 64u;
            if (what < 12u) {                              /* a key jumps somewhere */
                r.depth[keys[rnd() % 10u]] = (uint16_t)(rnd() % 341u);
            } else if (what < 40u) {                       /* a key moves a little */
                const unsigned k = keys[rnd() % 10u];
                const int dd = (int)(rnd() % 41u) - 20;
                int v = (int)r.depth[k] + dd;
                v = v < 0 ? 0 : v > 340 ? 340 : v;
                r.depth[k] = (uint16_t)v;
            } else if (what < 43u) {                       /* all keys up */
                for (unsigned i = 0; i < 10u; i++) {
                    r.depth[keys[i]] = 0;
                }
            } else if (what == 43u) {                      /* a fault, a USB reset, a profile change: reset */
                km_dks_reset(&r.d);
                km_dks_update_ms(&r.d, NULL, 0, r.now, NULL, NULL, &r.out);
                bad += r.out.n != 0u || km_dks_busy(&r.d);
                for (unsigned i = 0; i < KM_DKS_SLOTS; i++) {
                    r.down[i] = false;                     /* the engine model drops the keys as well */
                }
                for (unsigned i = 0; i < 10u; i++) {
                    r.depth[keys[i]] = 0;
                }
            }
            rig_pass(&r);
            /* every pass: usages unique and possible; no more than the table can hold */
            for (unsigned i = 0; i < r.out.n; i++) {
                bad += !usage_possible(recs, nrec, r.out.usage[i]);
                for (unsigned j = i + 1u; j < r.out.n; j++) {
                    bad += r.out.usage[i] == r.out.usage[j];
                }
            }
            bad += r.out.n > KM_DKS_OUT_MAX;
            bad += dks_books_bad(&r.d, &r.out);
            unsigned any_down = 0;
            for (unsigned i = 0; i < 10u; i++) {
                any_down += r.down[keys[i]] ? 1u : 0u;
            }
            if (any_down != 0u) {
                last_list_ms = r.now;
                had_list = true;
            } else if (had_list && (uint32_t)(r.now - last_list_ms) > KM_DKS_MAX_SETTLE_MS) {
                bad += r.out.n != 0u || km_dks_busy(&r.d);   /* nothing down for the settle time: all released */
            }
            r.now += 1u + (rnd() % 8u == 0u ? 1u : 0u);
        }
        /* end: release everything, run on: output empty within the bound */
        for (unsigned i = 0; i < 10u; i++) {
            r.depth[keys[i]] = 0;
        }
        for (unsigned i = 0; i < KM_DKS_MAX_SETTLE_MS + 4u; i++) {
            rig_pass(&r);
            r.now++;
        }
        bad += r.out.n != 0u || km_dks_busy(&r.d);
        CHECK_EQ(bad, 0);
    }
}

/* ---------------------------------------------------------------- 6. merge, cycles */

static void dks_merge_cycles(void)
{
    km_dks_out_t o;
    o.n = 3;
    o.usage[0] = U_A;
    o.usage[1] = U_S;
    o.usage[2] = U_D;
    uint8_t kbd[8] = { U_W, U_S, 0, 0, 0, 0, 0, 0 };
    CHECK_EQ(km_dks_merge(kbd, 2, 8, &o), 4);
    CHECK(kbd[0] == U_W && kbd[1] == U_S && kbd[2] == U_A && kbd[3] == U_D);
    CHECK_EQ(km_dks_merge(kbd, 4, 8, &o), 4);             /* idempotent */
    uint8_t small[3] = { U_W, 0, 0 };
    CHECK_EQ(km_dks_merge(small, 1, 3, &o), 3);           /* capacity respected */
    CHECK(small[1] == U_A && small[2] == U_S);
    o.n = 0;
    CHECK_EQ(km_dks_merge(kbd, 4, 8, &o), 4);
    uint8_t none[1] = { 0 };
    o.n = 1;
    CHECK_EQ(km_dks_merge(none, 0, 0, &o), 0);

    /* the cycle wrapper: the same history as milliseconds; a counter that wraps; cycles_per_ms = 0 stands still */
    km_dks_rec_t c = rec_classic(0);
    km_dks_t d;
    km_dks_init(&d);
    CHECK(km_dks_set_records(&d, &c, 1));
    km_dks_press_t one = { K1, 0 };
    km_dks_out_t out;
    uint32_t cyc = 0xFFFFF000u;
    km_dks_update_cycles(&d, &one, 1, cyc, 240000u, NULL, NULL, &out);
    CHECK_EQ(out.n, 1);
    CHECK_EQ(d.ms, 0);
    for (unsigned i = 0; i < 50; i++) {                   /* 5 ms in 0.1 ms passes: the millisecond clock reads 5, not more */
        cyc += 24000u;
        km_dks_update_cycles(&d, &one, 1, cyc, 240000u, NULL, NULL, &out);
    }
    CHECK_EQ(d.ms, 5);
    cyc += 100u;                                          /* a pass less than a millisecond later: no tick */
    km_dks_update_cycles(&d, &one, 1, cyc, 240000u, NULL, NULL, &out);
    CHECK_EQ(d.ms, 5);
    for (unsigned i = 0; i < 600; i++) {                  /* 60 ms of released key in 0.1 ms passes */
        cyc += 24000u;
        km_dks_update_cycles(&d, NULL, 0, cyc, 240000u, NULL, NULL, &out);
    }
    CHECK_EQ(out.n, 0);
    km_dks_t s;
    km_dks_init(&s);
    CHECK(km_dks_set_records(&s, &c, 1));
    CHECK_EQ(km_dks_update_cycles(&s, &one, 1, 1000, 0, NULL, NULL, &out), 1);
    for (unsigned i = 0; i < 10; i++) {                   /* no clock: every pass counts as one step, so nothing can get stuck */
        km_dks_update_cycles(&s, NULL, 0, 2000, 0, NULL, NULL, &out);
    }
    CHECK_EQ(out.n, 0);
    CHECK(!km_dks_busy(&s));
}

void test_dks(void)
{
    dks_records();
    dks_run_all_depth_cases();
    dks_engine_rig();
    dks_limits();
    dks_random(400);
    dks_merge_cycles();
}
