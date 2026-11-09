/*
 * test_socd.c - host tests for the M6 package P1 code: the SOCD filter (km_socd.[hc]).
 *
 *   1. configuration: validity of a pair, rejected sets (a slot twice, too many, bad flags / mode), mode-0 entries
 *   2. hand-written truth tables of the four modes (the documented behaviour, one scenario per sentence of km_socd.h)
 *   3. EXHAUSTIVE: every event sequence of up to 8 presses / releases of the two keys, for the three order-based modes,
 *      against a model that only knows time stamps (it never sees a list)
 *   4. "deeper wins": hysteresis boundary, start of a conflict, depth callback absent, noise on the depths
 *   5. random sequences with 4 pairs, other keys, layers, depths and fail-safe calls: structural invariants,
 *      "released everything means nothing is reported", the order-based modes keep no state, in-place == separate buffer
 *   6. the real key engine: frames of synthetic sensor readings through km_engine -> km_socd_filter
 *   7. edge cases: slots >= 76, repeated entries, empty list, 64 entries, counters
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "km/km_depth_model.h"
#include "km/km_engine.h"
#include "km/km_keymap.h"
#include "km/km_socd.h"
#include "km_test.h"

void test_socd(void);

#define SS 72u   /* the slots of the W A S D keys on this board (core/data/slotmap_measured.json) */
#define SA 73u
#define SD 74u
#define SW 75u

/* ---------------------------------------------------------------- helpers */

static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* three keys that are not W A S D, found from the key map */
static unsigned other_key(unsigned i)
{
    unsigned seen = 0;
    for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
        if (km_keymap_slot_is_key(s) && (s < SS || s > SW)) {
            if (seen++ == i) {
                return s;
            }
        }
    }
    return 0;
}

static unsigned letter(char c)
{
    switch (c) {
    case 'A': return SA;
    case 'D': return SD;
    case 'W': return SW;
    case 'S': return SS;
    case 'x': return other_key(0);
    case 'y': return other_key(1);
    default:  return other_key(2);   /* 'z' */
    }
}

typedef struct {
    uint8_t  l[KM_ENGINE_MAX_DOWN];
    unsigned n;
} plist_t;

static bool p_has(const plist_t *p, unsigned slot)
{
    for (unsigned i = 0; i < p->n; i++) {
        if (p->l[i] == slot) {
            return true;
        }
    }
    return false;
}

static void p_press(plist_t *p, unsigned slot)
{
    if (!p_has(p, slot) && p->n < sizeof p->l) {
        p->l[p->n++] = (uint8_t)slot;
    }
}

static void p_release(plist_t *p, unsigned slot)
{
    for (unsigned i = 0; i < p->n; i++) {
        if (p->l[i] == slot) {
            memmove(&p->l[i], &p->l[i + 1], p->n - i - 1u);
            p->n--;
            return;
        }
    }
}

static uint16_t cb_depth(void *ctx, unsigned slot)
{
    return ((const uint16_t *)ctx)[slot];
}

/* the running state and the counters are equal (the struct padding is not compared) */
static bool same_state(const km_socd_t *a, const km_socd_t *b)
{
    return memcmp(a->owner, b->owner, sizeof a->owner) == 0 && memcmp(a->supp, b->supp, sizeof a->supp) == 0 &&
           a->suppress_events == b->suppress_events && a->calls_in_conflict == b->calls_in_conflict;
}

static unsigned run(km_socd_t *s, const plist_t *in, unsigned layer, uint16_t *depth, uint8_t *out)
{
    return km_socd_filter(s, in->l, in->n, layer, depth != NULL ? cb_depth : NULL, depth, out);
}

/* the output equals the letters, in that order */
static bool match(const uint8_t *out, unsigned n, const char *want)
{
    if (strlen(want) != n) {
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        if (out[i] != letter(want[i])) {
            return false;
        }
    }
    return true;
}

static km_socd_pair_t pair(unsigned a, unsigned b, unsigned mode, unsigned flags)
{
    km_socd_pair_t p = { (uint8_t)a, (uint8_t)b, (uint8_t)mode, (uint8_t)flags };
    return p;
}

static void setup1(km_socd_t *s, unsigned mode, unsigned flags)
{
    const km_socd_pair_t p = pair(SA, SD, mode, flags);
    km_socd_init(s);
    CHECK(km_socd_set_pairs(s, &p, 1));
}

/* one step of a scenario: press (+) or release (-) the letters, filter, compare */
static bool step(km_socd_t *s, plist_t *l, unsigned layer, uint16_t *depth, const char *events, const char *want)
{
    for (const char *e = events; *e != '\0'; e += 2) {
        if (e[0] == '+') {
            p_press(l, letter(e[1]));
        } else {
            p_release(l, letter(e[1]));
        }
    }
    uint8_t out[KM_ENGINE_MAX_DOWN];
    const unsigned n = run(s, l, layer, depth, out);
    return match(out, n, want);
}

/* ---------------------------------------------------------------- 1. configuration */

static void socd_config(void)
{
    km_socd_pair_t p = pair(SA, SD, KM_SOCD_LAST, 0);
    CHECK(km_socd_pair_valid(&p));
    CHECK(!km_socd_pair_valid(NULL));
    for (unsigned mode = 0; mode <= 4; mode++) {
        p = pair(SA, SD, mode, 0);
        CHECK(km_socd_pair_valid(&p));
    }
    p = pair(SA, SD, 5, 0);
    CHECK(!km_socd_pair_valid(&p));
    p = pair(SA, SD, 255, 0);
    CHECK(!km_socd_pair_valid(&p));
    p = pair(SA, SD, KM_SOCD_LAST, 1);
    CHECK(km_socd_pair_valid(&p));
    p = pair(SA, SD, KM_SOCD_LAST, 2);
    CHECK(!km_socd_pair_valid(&p));
    p = pair(SA, SD, KM_SOCD_OFF, 2);
    CHECK(!km_socd_pair_valid(&p));                       /* reserved bits must be 0 even for an unused entry */
    p = pair(SA, SA, KM_SOCD_LAST, 0);
    CHECK(!km_socd_pair_valid(&p));                       /* a key against itself */
    p = pair(SA, 15, KM_SOCD_LAST, 0);
    CHECK(!km_keymap_slot_is_key(15) && !km_socd_pair_valid(&p));   /* a mirror slot is no key */
    p = pair(15, SA, KM_SOCD_LAST, 0);
    CHECK(!km_socd_pair_valid(&p));
    p = pair(SA, 76, KM_SOCD_LAST, 0);
    CHECK(!km_socd_pair_valid(&p));
    p = pair(SA, 255, KM_SOCD_LAST, 0);
    CHECK(!km_socd_pair_valid(&p));
    p = pair(0, 0, KM_SOCD_OFF, 0);
    CHECK(km_socd_pair_valid(&p));                        /* an unused entry: slots do not matter */
    p = pair(200, 201, KM_SOCD_OFF, 0);
    CHECK(km_socd_pair_valid(&p));

    km_socd_t s;
    km_socd_init(&s);
    CHECK_EQ(s.n_used, 0);
    km_socd_pair_t four[6] = { pair(SA, SD, 1, 0), pair(SW, SS, 2, 0), pair(letter('x'), letter('y'), 3, 1), pair(0, 0, 0, 0) };
    CHECK(km_socd_set_pairs(&s, four, 4));
    CHECK_EQ(s.n_used, 3);
    CHECK(km_socd_set_pairs(&s, four, 0));                /* n = 0: all off, fine */
    CHECK_EQ(s.n_used, 0);
    CHECK(km_socd_set_pairs(&s, NULL, 0));
    CHECK(!km_socd_set_pairs(&s, NULL, 1));
    CHECK(!km_socd_set_pairs(&s, four, 5));               /* more than KM_SOCD_MAX */
    CHECK_EQ(s.n_used, 0);

    /* a slot in two pairs: the whole set is refused and nothing is used */
    km_socd_pair_t dup[2] = { pair(SA, SD, 1, 0), pair(SD, SW, 1, 0) };
    CHECK(km_socd_set_pairs(&s, four, 4));
    CHECK(!km_socd_set_pairs(&s, dup, 2));
    CHECK_EQ(s.n_used, 0);
    for (unsigned i = 0; i < KM_SOCD_SLOTS; i++) {
        CHECK_EQ(s.role[i], 0);
    }
    dup[1] = pair(SW, SA, 1, 0);
    CHECK(!km_socd_set_pairs(&s, dup, 2));
    /* the same slot in an UNUSED entry does not clash */
    dup[1] = pair(SD, SW, 0, 0);
    CHECK(km_socd_set_pairs(&s, dup, 2));
    CHECK_EQ(s.n_used, 1);
    /* one invalid pair among valid ones: nothing is used (not even the valid ones) */
    km_socd_pair_t bad[2] = { pair(SA, SD, 1, 0), pair(SW, SS, 9, 0) };
    CHECK(!km_socd_set_pairs(&s, bad, 2));
    CHECK_EQ(s.n_used, 0);
    uint8_t in[2] = { SA, SD }, out[2];
    CHECK_EQ(km_socd_filter(&s, in, 2, 0, NULL, NULL, out), 2);   /* so a refused set filters nothing */

    /* a refused set after a good one turns the old pairs off as well */
    CHECK(km_socd_set_pairs(&s, four, 1));
    CHECK_EQ(km_socd_filter(&s, in, 2, 0, NULL, NULL, out), 1);
    CHECK(!km_socd_set_pairs(&s, bad, 2));
    CHECK_EQ(km_socd_filter(&s, in, 2, 0, NULL, NULL, out), 2);

    /* default: everything off, every list passes untouched */
    km_socd_init(&s);
    CHECK_EQ(km_socd_filter(&s, in, 2, 0, NULL, NULL, out), 2);
    CHECK(out[0] == SA && out[1] == SD);
    /* mode-0 pair: nothing happens even with both keys down */
    km_socd_pair_t off = pair(SA, SD, 0, 0);
    CHECK(km_socd_set_pairs(&s, &off, 1));
    CHECK_EQ(km_socd_filter(&s, in, 2, 0, NULL, NULL, out), 2);

    /* an unused entry in front of / between used ones: the used pairs still work (they are packed) */
    km_socd_pair_t gap[3] = { pair(0, 0, 0, 0), pair(SA, SD, KM_SOCD_LAST, 0), pair(0, 0, 0, 0) };
    CHECK(km_socd_set_pairs(&s, gap, 3));
    CHECK_EQ(s.n_used, 1);
    CHECK_EQ(km_socd_filter(&s, in, 2, 0, NULL, NULL, out), 1);
    CHECK_EQ(out[0], SD);
    km_socd_pair_t gap2[4] = { pair(SA, SD, KM_SOCD_FIRST, 0), pair(0, 0, 0, 0), pair(SW, SS, KM_SOCD_NEUTRAL, 0), pair(0, 0, 0, 0) };
    CHECK(km_socd_set_pairs(&s, gap2, 4));
    CHECK_EQ(s.n_used, 2);
    const uint8_t wasd[4] = { SW, SA, SS, SD };
    uint8_t out4[4];
    CHECK_EQ(km_socd_filter(&s, wasd, 4, 0, NULL, NULL, out4), 1);
    CHECK_EQ(out4[0], SA);
}

/* ---------------------------------------------------------------- 2. hand-written truth tables */

static void socd_last(void)
{
    km_socd_t s;
    plist_t l = { { 0 }, 0 };
    setup1(&s, KM_SOCD_LAST, 0);
    CHECK(step(&s, &l, 0, NULL, "", ""));
    CHECK(step(&s, &l, 0, NULL, "+A", "A"));
    CHECK(step(&s, &l, 0, NULL, "+D", "D"));        /* D came later: D wins, A is held back */
    CHECK(step(&s, &l, 0, NULL, "-D", "A"));        /* D released: A, still held, comes back */
    CHECK(step(&s, &l, 0, NULL, "+D", "D"));        /* D pressed again: later again */
    CHECK(step(&s, &l, 0, NULL, "-A", "D"));        /* the held-back key is released: no change for D */
    CHECK(step(&s, &l, 0, NULL, "-D", ""));

    /* the other way round */
    CHECK(step(&s, &l, 0, NULL, "+D", "D"));
    CHECK(step(&s, &l, 0, NULL, "+A", "A"));
    CHECK(step(&s, &l, 0, NULL, "-D", "A"));
    CHECK(step(&s, &l, 0, NULL, "-A", ""));

    /* A, D, then release A first, then D: D stays all the time */
    CHECK(step(&s, &l, 0, NULL, "+A", "A"));
    CHECK(step(&s, &l, 0, NULL, "+D", "D"));
    CHECK(step(&s, &l, 0, NULL, "-A", "D"));
    CHECK(step(&s, &l, 0, NULL, "-D", ""));

    /* a rolling hand: A D A D ... each new press takes over, A must always win when it is the latest */
    CHECK(step(&s, &l, 0, NULL, "+A", "A"));
    CHECK(step(&s, &l, 0, NULL, "+D", "D"));
    CHECK(step(&s, &l, 0, NULL, "-A+A", "A"));      /* A released and pressed again within one update: it is the latest */
    CHECK(step(&s, &l, 0, NULL, "-A", "D"));
    CHECK(step(&s, &l, 0, NULL, "-D", ""));

    /* other keys keep their place in the list */
    CHECK(step(&s, &l, 0, NULL, "+x+A+y+D+z", "xyDz"));
    CHECK(step(&s, &l, 0, NULL, "-x", "yDz"));
    CHECK(step(&s, &l, 0, NULL, "-D", "Ayz"));      /* A comes back at ITS place in the list: it was pressed before y */
    CHECK(step(&s, &l, 0, NULL, "-A-y-z", ""));

    /* same frame: both appear in one update, ascending slot order in the list, so the higher slot (D = 74) is "later" */
    plist_t same = { { (uint8_t)SA, (uint8_t)SD }, 2 };
    uint8_t out[8];
    CHECK(match(out, run(&s, &same, 0, NULL, out), "D"));
}

static void socd_first(void)
{
    km_socd_t s;
    plist_t l = { { 0 }, 0 };
    setup1(&s, KM_SOCD_FIRST, 0);
    CHECK(step(&s, &l, 0, NULL, "+A", "A"));
    CHECK(step(&s, &l, 0, NULL, "+D", "A"));        /* A was first: it wins, D is ignored */
    CHECK(step(&s, &l, 0, NULL, "-A", "D"));        /* A released: D, still held, is reported */
    CHECK(step(&s, &l, 0, NULL, "+A", "D"));        /* A is now the later key: D keeps winning */
    CHECK(step(&s, &l, 0, NULL, "-D", "A"));
    CHECK(step(&s, &l, 0, NULL, "-A", ""));
    CHECK(step(&s, &l, 0, NULL, "+D", "D"));
    CHECK(step(&s, &l, 0, NULL, "+A", "D"));
    CHECK(step(&s, &l, 0, NULL, "-A", "D"));        /* the ignored key is released: nothing changes */
    CHECK(step(&s, &l, 0, NULL, "-D", ""));
    CHECK(step(&s, &l, 0, NULL, "+x+D+y+A", "xDy"));
    CHECK(step(&s, &l, 0, NULL, "-D", "xyA"));
    CHECK(step(&s, &l, 0, NULL, "-x-y-A", ""));
    plist_t same = { { (uint8_t)SA, (uint8_t)SD }, 2 };
    uint8_t out[8];
    CHECK(match(out, run(&s, &same, 0, NULL, out), "A"));
}

static void socd_neutral(void)
{
    km_socd_t s;
    plist_t l = { { 0 }, 0 };
    setup1(&s, KM_SOCD_NEUTRAL, 0);
    CHECK(step(&s, &l, 0, NULL, "+A", "A"));
    CHECK(step(&s, &l, 0, NULL, "+D", ""));         /* both down: neither */
    CHECK(step(&s, &l, 0, NULL, "-A", "D"));        /* one released: the other acts */
    CHECK(step(&s, &l, 0, NULL, "+A", ""));
    CHECK(step(&s, &l, 0, NULL, "-D", "A"));
    CHECK(step(&s, &l, 0, NULL, "-A", ""));
    CHECK(step(&s, &l, 0, NULL, "+x+A+y+D+z", "xyz"));
    CHECK(step(&s, &l, 0, NULL, "-A", "xyDz"));
    CHECK(step(&s, &l, 0, NULL, "-x-y-z-D", ""));
    plist_t same = { { (uint8_t)SA, (uint8_t)SD }, 2 };
    uint8_t out[8];
    CHECK_EQ(run(&s, &same, 0, NULL, out), 0);
}

static void socd_pairs_independent(void)
{
    /* W/S and A/D together, in different modes: pairs do not influence each other */
    km_socd_t s;
    km_socd_pair_t p[2] = { pair(SA, SD, KM_SOCD_LAST, 0), pair(SW, SS, KM_SOCD_NEUTRAL, 0) };
    km_socd_init(&s);
    CHECK(km_socd_set_pairs(&s, p, 2));
    plist_t l = { { 0 }, 0 };
    CHECK(step(&s, &l, 0, NULL, "+W+A+S+D", "D"));
    CHECK(step(&s, &l, 0, NULL, "-S", "WD"));
    CHECK(step(&s, &l, 0, NULL, "-D", "WA"));
    CHECK(step(&s, &l, 0, NULL, "+S+D", "D"));
    CHECK(step(&s, &l, 0, NULL, "-A-W-S-D", ""));
}

static void socd_layer_flag(void)
{
    km_socd_t s;
    plist_t l = { { 0 }, 0 };
    setup1(&s, KM_SOCD_LAST, KM_SOCD_FLAG_LAYER0);
    CHECK(step(&s, &l, 0, NULL, "+A+D", "D"));      /* layer 0: active */
    CHECK(step(&s, &l, 1, NULL, "", "AD"));         /* the layer changes with both held: the filter lets both through ... */
    CHECK(!km_socd_suppressed(&s, SA));
    CHECK(step(&s, &l, 0, NULL, "", "D"));          /* ... and takes over again on layer 0 */
    CHECK(km_socd_suppressed(&s, SA));
    CHECK(step(&s, &l, 3, NULL, "-D", "A"));
    CHECK(step(&s, &l, 3, NULL, "+D", "AD"));
    CHECK(step(&s, &l, 0, NULL, "-A-D", ""));
    /* DEEPER with the flag: the winner is forgotten while the pair sleeps, the conflict starts afresh on layer 0 */
    uint16_t d[KM_SOCD_SLOTS];
    memset(d, 0, sizeof d);
    setup1(&s, KM_SOCD_DEEPER, KM_SOCD_FLAG_LAYER0);
    l.n = 0;
    d[SA] = 200;
    d[SD] = 150;
    CHECK(step(&s, &l, 0, d, "+A+D", "A"));
    d[SD] = 240;
    CHECK(step(&s, &l, 0, d, "", "D"));             /* D took over */
    CHECK(step(&s, &l, 1, d, "", "AD"));
    d[SD] = 190;                                    /* back on layer 0: A is only 10 deeper, the earlier key (A) starts */
    CHECK(step(&s, &l, 0, d, "", "A"));
    /* without the flag the layer does not matter */
    setup1(&s, KM_SOCD_LAST, 0);
    l.n = 0;
    CHECK(step(&s, &l, 2, NULL, "+A+D", "D"));
}

/* ---------------------------------------------------------------- 3. exhaustive event sequences */

/* the model: only time stamps (event number + 1 when a key went down, 0 while up) */
static void model_both(unsigned mode, unsigned ta, unsigned tb, bool *out_a, bool *out_b)
{
    *out_a = ta != 0u;
    *out_b = tb != 0u;
    if (ta != 0u && tb != 0u) {
        switch (mode) {
        case KM_SOCD_LAST:    *out_a = ta > tb; *out_b = tb > ta; break;
        case KM_SOCD_FIRST:   *out_a = ta < tb; *out_b = tb < ta; break;
        default:              *out_a = false;   *out_b = false;   break;   /* NEUTRAL */
        }
    }
}

static void socd_exhaustive(void)
{
    enum { LEN = 8 };
    for (unsigned mode = KM_SOCD_LAST; mode <= KM_SOCD_FIRST; mode++) {
        for (unsigned code = 0; code < (1u << (2u * LEN)); code++) {
            km_socd_t s;
            plist_t l = { { 0 }, 0 };
            setup1(&s, mode, 0);
            unsigned ta = 0, tb = 0;
            unsigned c = code;
            for (unsigned i = 0; i < LEN; i++, c >>= 2) {
                switch (c & 3u) {
                case 0: if (ta == 0u) { ta = i + 1u; p_press(&l, SA); } break;
                case 1: ta = 0; p_release(&l, SA); break;
                case 2: if (tb == 0u) { tb = i + 1u; p_press(&l, SD); } break;
                default: tb = 0; p_release(&l, SD); break;
                }
                uint8_t out[KM_ENGINE_MAX_DOWN];
                const unsigned n = run(&s, &l, 0, NULL, out);
                bool ea, eb;
                model_both(mode, ta, tb, &ea, &eb);
                const unsigned want = (ea ? 1u : 0u) + (eb ? 1u : 0u);
                bool ok = n == want;
                for (unsigned k = 0; k < n; k++) {
                    ok = ok && ((out[k] == SA && ea) || (out[k] == SD && eb));
                }
                if (!ok) {                                   /* count one failure per broken sequence, not per step */
                    CHECK(ok);
                    break;
                }
            }
        }
    }
    CHECK(1);
}

/* ---------------------------------------------------------------- 4. deeper wins */

static void socd_deeper(void)
{
    km_socd_t s;
    plist_t l = { { 0 }, 0 };
    uint16_t d[KM_SOCD_SLOTS];
    memset(d, 0, sizeof d);
    setup1(&s, KM_SOCD_DEEPER, 0);

    d[SA] = 200;
    CHECK(step(&s, &l, 0, d, "+A", "A"));
    d[SD] = 150;
    CHECK(step(&s, &l, 0, d, "+D", "A"));           /* conflict starts: A was reported first, and it is deeper */
    d[SD] = 225;
    CHECK(step(&s, &l, 0, d, "", "A"));             /* 25 deeper: inside the hysteresis, A stays */
    d[SD] = 229;
    CHECK(step(&s, &l, 0, d, "", "A"));             /* 29: still A */
    d[SD] = 230;
    CHECK(step(&s, &l, 0, d, "", "D"));             /* exactly 30 deeper: D takes over */
    d[SD] = 215;
    CHECK(step(&s, &l, 0, d, "", "D"));             /* D is only 15 deeper than A now: D stays (no flicker back) */
    d[SD] = 171;
    CHECK(step(&s, &l, 0, d, "", "D"));             /* D is shallower by 29: D stays */
    d[SD] = 170;
    CHECK(step(&s, &l, 0, d, "", "A"));             /* A is 30 deeper: A is back */
    d[SA] = 100;
    d[SD] = 100;
    CHECK(step(&s, &l, 0, d, "", "A"));             /* equal: no change */
    CHECK(step(&s, &l, 0, d, "-A", "D"));           /* partner released: D reported whatever the depth */
    CHECK(step(&s, &l, 0, d, "-D", ""));
    CHECK_EQ(s.owner[0], 0);                        /* no conflict: the winner is forgotten */

    /* conflict starts with the new key already clearly deeper */
    d[SA] = 130;
    CHECK(step(&s, &l, 0, d, "+A", "A"));
    d[SD] = 200;
    CHECK(step(&s, &l, 0, d, "+D", "D"));
    d[SD] = 150;
    CHECK(step(&s, &l, 0, d, "", "D"));             /* difference 20: D stays, although A was reported first */
    CHECK(step(&s, &l, 0, d, "-D-A", ""));

    /* the earlier key (D this time) is the starting winner when the depths are close */
    d[SA] = 150;
    d[SD] = 150;
    CHECK(step(&s, &l, 0, d, "+D", "D"));
    CHECK(step(&s, &l, 0, d, "+A", "D"));
    CHECK(step(&s, &l, 0, d, "-D", "A"));
    CHECK(step(&s, &l, 0, d, "+D", "A"));           /* D released and pressed again: A is now the earlier key */
    CHECK(step(&s, &l, 0, d, "-A-D", ""));

    /* same frame, equal depths: the first entry of the list (lower slot) wins to begin with */
    plist_t same = { { (uint8_t)SA, (uint8_t)SD }, 2 };
    uint8_t out[8];
    CHECK(match(out, run(&s, &same, 0, d, out), "A"));
    CHECK(match(out, run(&s, &same, 0, d, out), "A"));   /* and keeps it */

    /* no depth callback: every depth is equal, so the first key keeps winning (documented) */
    km_socd_init(&s);
    km_socd_pair_t p = pair(SA, SD, KM_SOCD_DEEPER, 0);
    CHECK(km_socd_set_pairs(&s, &p, 1));
    l.n = 0;
    CHECK(step(&s, &l, 0, NULL, "+D+A", "D"));
    CHECK(step(&s, &l, 0, NULL, "-D", "A"));

    /* noise: depths wobbling by +-12 around equal values never change the winner */
    km_socd_init(&s);
    CHECK(km_socd_set_pairs(&s, &p, 1));
    l.n = 0;
    d[SA] = 180;
    d[SD] = 180;
    CHECK(step(&s, &l, 0, d, "+A+D", "A"));
    unsigned flips = 0;
    for (unsigned i = 0; i < 20000; i++) {
        d[SA] = (uint16_t)(180u + rnd() % 25u - 12u);
        d[SD] = (uint16_t)(180u + rnd() % 25u - 12u);
        uint8_t o[8];
        const unsigned n = run(&s, &l, 0, d, o);
        flips += (n != 1u || o[0] != SA);
    }
    CHECK_EQ(flips, 0);
    /* a slow drift of D from 150 to 250 against A at 200: exactly one change, at 230 (30 deeper than A) */
    d[SA] = 200;
    unsigned changes = 0;
    unsigned last = SA;
    unsigned at = 0;
    for (unsigned dd = 150; dd <= 250; dd++) {
        d[SD] = (uint16_t)dd;
        uint8_t o[8];
        const unsigned n = run(&s, &l, 0, d, o);
        CHECK_EQ(n, 1);
        if (o[0] != last) {
            changes++;
            at = dd;
            last = o[0];
        }
    }
    CHECK_EQ(changes, 1);
    CHECK_EQ(at, 230);
    for (unsigned dd = 250; dd >= 150; dd--) {      /* and back: one change, at 170 (A 30 deeper than D) */
        d[SD] = (uint16_t)dd;
        uint8_t o[8];
        run(&s, &l, 0, d, o);
        if (o[0] != last) {
            changes++;
            at = dd;
            last = o[0];
        }
    }
    CHECK_EQ(changes, 2);
    CHECK_EQ(at, 170);
}

/* ---------------------------------------------------------------- 5. random sequences */

typedef struct {
    km_socd_pair_t pr[KM_SOCD_MAX];
    unsigned n;
} cfg_t;

/* four disjoint random pairs of key slots */
static void random_cfg(cfg_t *c, bool allow_deeper)
{
    unsigned used[KM_SOCD_SLOTS] = { 0 };
    c->n = KM_SOCD_MAX;
    for (unsigned i = 0; i < c->n; i++) {
        unsigned a, b;
        do { a = rnd() % KM_SOCD_SLOTS; } while (!km_keymap_slot_is_key(a) || used[a]);
        used[a] = 1;
        do { b = rnd() % KM_SOCD_SLOTS; } while (!km_keymap_slot_is_key(b) || used[b]);
        used[b] = 1;
        c->pr[i] = pair(a, b, rnd() % (allow_deeper ? 5u : 4u), rnd() % 2u);
        if (!allow_deeper && c->pr[i].mode == 4u) {
            c->pr[i].mode = 1u;
        }
    }
}

static bool in_list(const uint8_t *l, unsigned n, unsigned slot)
{
    for (unsigned i = 0; i < n; i++) {
        if (l[i] == slot) {
            return true;
        }
    }
    return false;
}

static unsigned pos_of(const uint8_t *l, unsigned n, unsigned slot)
{
    for (unsigned i = 0; i < n; i++) {
        if (l[i] == slot) {
            return i;
        }
    }
    return 0xFFFFu;
}

/* every structural rule of km_socd.h for one call; returns the number of broken rules */
static unsigned check_rules(const cfg_t *c, const plist_t *in, const uint8_t *out, unsigned n_out, unsigned layer, const uint16_t *depth)
{
    unsigned bad = 0;
    /* subsequence of the input, nothing added or repeated */
    unsigned j = 0;
    for (unsigned i = 0; i < in->n && j < n_out; i++) {
        if (in->l[i] == out[j]) {
            j++;
        }
    }
    bad += j != n_out;
    for (unsigned i = 0; i < n_out; i++) {
        for (unsigned k = i + 1; k < n_out; k++) {
            bad += out[i] == out[k];
        }
    }
    /* keys of no active pair are reported; keys in conflict follow the mode */
    for (unsigned i = 0; i < in->n; i++) {
        const unsigned slot = in->l[i];
        bool in_conflict = false;
        for (unsigned p = 0; p < c->n; p++) {
            const km_socd_pair_t *q = &c->pr[p];
            if (q->mode == 0u || (slot != q->slot_a && slot != q->slot_b)) {
                continue;
            }
            const unsigned partner = slot == q->slot_a ? q->slot_b : q->slot_a;
            if (p_has(in, partner) && !((q->flags & 1u) && layer != 0u)) {
                in_conflict = true;
            }
        }
        if (!in_conflict) {
            bad += !in_list(out, n_out, slot);
        }
    }
    for (unsigned p = 0; p < c->n; p++) {
        const km_socd_pair_t *q = &c->pr[p];
        if (q->mode == 0u || !p_has(in, q->slot_a) || !p_has(in, q->slot_b) || ((q->flags & 1u) && layer != 0u)) {
            continue;
        }
        const bool oa = in_list(out, n_out, q->slot_a);
        const bool ob = in_list(out, n_out, q->slot_b);
        if (q->mode == KM_SOCD_NEUTRAL) {
            bad += oa || ob;
        } else {
            bad += oa == ob;                                  /* exactly one */
            const unsigned pa = pos_of(in->l, in->n, q->slot_a);
            const unsigned pb = pos_of(in->l, in->n, q->slot_b);
            if (q->mode == KM_SOCD_LAST && oa != ob) {
                bad += oa != (pa > pb);
            } else if (q->mode == KM_SOCD_FIRST && oa != ob) {
                bad += oa != (pa < pb);
            } else if (q->mode == KM_SOCD_DEEPER && oa != ob && depth != NULL) {
                const unsigned dw = oa ? depth[q->slot_a] : depth[q->slot_b];
                const unsigned dl = oa ? depth[q->slot_b] : depth[q->slot_a];
                bad += dl >= dw + KM_SOCD_DEEPER_HYST_MM100;   /* never behind a deeper key by the hysteresis or more */
            }
        }
    }
    return bad;
}

static void socd_random(unsigned rounds, bool allow_deeper)
{
    for (unsigned round = 0; round < rounds; round++) {
        cfg_t c;
        random_cfg(&c, allow_deeper);
        km_socd_t s, fresh, twin;
        km_socd_init(&s);
        CHECK(km_socd_set_pairs(&s, c.pr, c.n));
        twin = s;                                             /* runs in place on a copy of the list */
        km_socd_init(&fresh);
        CHECK(km_socd_set_pairs(&fresh, c.pr, c.n));

        /* a pool of keys: all pair slots plus some others */
        uint8_t pool[16];
        unsigned np = 0;
        for (unsigned p = 0; p < c.n && np < 8; p++) {
            pool[np++] = c.pr[p].slot_a;
            pool[np++] = c.pr[p].slot_b;
        }
        while (np < 12) {
            const unsigned k = rnd() % KM_SOCD_SLOTS;
            if (km_keymap_slot_is_key(k)) {
                pool[np++] = (uint8_t)k;
            }
        }
        pool[np++] = 100;                                     /* two entries that are no slots at all: they must pass through */
        pool[np++] = 200;
        plist_t l = { { 0 }, 0 };
        uint16_t depth[256];
        memset(depth, 0, sizeof depth);
        unsigned layer = 0;
        unsigned bad = 0, bad_state = 0, bad_inplace = 0, bad_fresh = 0;
        for (unsigned step_no = 0; step_no < 400; step_no++) {
            const unsigned ev = rnd() % 16u;
            const unsigned k = pool[rnd() % np];
            if (ev < 7u) {
                p_press(&l, k);
            } else if (ev < 13u) {
                p_release(&l, k);
            } else if (ev == 13u) {
                layer = rnd() % 4u;
            } else if (ev == 14u && l.n > 0u) {              /* two presses in the same frame, ascending slots, as the engine lists them */
                const unsigned k2 = pool[rnd() % np];
                if (!p_has(&l, k) && !p_has(&l, k2) && k != k2) {
                    p_press(&l, k < k2 ? k : k2);
                    p_press(&l, k < k2 ? k2 : k);
                }
            } else {                                          /* a fail-safe release: an empty list and the state is gone */
                uint8_t o[KM_ENGINE_MAX_DOWN];
                CHECK_EQ(km_socd_filter(&s, NULL, 0, layer, NULL, NULL, o), 0);
                for (unsigned i = 0; i < 3; i++) {
                    bad_state += s.supp[i] != 0u;
                }
                for (unsigned i = 0; i < KM_SOCD_MAX; i++) {
                    bad_state += s.owner[i] != 0u;
                }
                km_socd_reset(&twin);
                km_socd_reset(&fresh);
            }
            for (unsigned i = 0; i < np; i++) {              /* depths wander a bit; held keys sit between 100 and 340 */
                depth[pool[i]] = p_has(&l, pool[i]) ? (uint16_t)(100u + rnd() % 241u) : (uint16_t)0;
            }
            uint8_t out[KM_ENGINE_MAX_DOWN], o2[KM_ENGINE_MAX_DOWN], o3[KM_ENGINE_MAX_DOWN];
            const unsigned n = run(&s, &l, layer, depth, out);
            bad += check_rules(&c, &l, out, n, layer, depth);

            /* the same call in place on a copy of the list: same result, same state */
            memcpy(o2, l.l, l.n);
            const unsigned n2 = km_socd_filter(&twin, o2, l.n, layer, cb_depth, depth, o2);
            bad_inplace += n2 != n || memcmp(o2, out, n) != 0 || !same_state(&twin, &s);

            /* the order-based modes keep no state: a filter that was just reset gives the same list */
            if (!allow_deeper) {
                km_socd_reset(&fresh);
                const unsigned n3 = run(&fresh, &l, layer, depth, o3);
                bad_fresh += n3 != n || memcmp(o3, out, n) != 0;
            }
        }
        /* release everything: nothing is reported and nothing is remembered */
        while (l.n > 0u) {
            p_release(&l, l.l[rnd() % l.n]);
            uint8_t out[KM_ENGINE_MAX_DOWN];
            const unsigned n = run(&s, &l, layer, depth, out);
            bad += check_rules(&c, &l, out, n, layer, NULL);
        }
        uint8_t out[KM_ENGINE_MAX_DOWN];
        const unsigned n = run(&s, &l, layer, depth, out);
        bad += n != 0u;
        for (unsigned i = 0; i < 3; i++) {
            bad_state += s.supp[i] != 0u;
        }
        for (unsigned i = 0; i < KM_SOCD_MAX; i++) {
            bad_state += s.owner[i] != 0u;
        }
        CHECK_EQ(bad, 0);
        CHECK_EQ(bad_state, 0);
        CHECK_EQ(bad_inplace, 0);
        CHECK_EQ(bad_fresh, 0);
    }
}

/* every key of every pair, every mode: a key whose partner is up is reported, whatever happened before (no key stays suppressed) */
static void socd_no_stuck_suppression(void)
{
    for (unsigned mode = KM_SOCD_LAST; mode <= KM_SOCD_DEEPER; mode++) {
        km_socd_t s;
        setup1(&s, mode, 0);
        uint16_t d[KM_SOCD_SLOTS];
        memset(d, 0, sizeof d);
        for (unsigned round = 0; round < 500; round++) {
            plist_t l = { { 0 }, 0 };
            uint8_t out[KM_ENGINE_MAX_DOWN];
            for (unsigned i = 0; i < 12; i++) {
                const unsigned k = (rnd() & 1u) ? SA : SD;
                if (rnd() & 1u) { p_press(&l, k); } else { p_release(&l, k); }
                d[SA] = (uint16_t)(rnd() % 341u);
                d[SD] = (uint16_t)(rnd() % 341u);
                run(&s, &l, 0, d, out);
            }
            p_release(&l, SA);                                /* A up: if D is down it must be reported */
            unsigned n = run(&s, &l, 0, d, out);
            CHECK_EQ(n, p_has(&l, SD) ? 1u : 0u);
            if (n == 1u) {
                CHECK_EQ(out[0], SD);
            }
            p_release(&l, SD);
            CHECK_EQ(run(&s, &l, 0, d, out), 0);
        }
    }
}

/* ---------------------------------------------------------------- 6. the real engine */

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
    km_engine_t e;
    km_socd_t   s;
} rig_t;

static void rig_init(rig_t *r, unsigned mode)
{
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
    setup1(&r->s, mode, 0);
}

/* hold the two keys at the given depths (mm100) for 60 frames, then return what the filter reports */
static unsigned rig_hold(rig_t *r, unsigned depth_a, unsigned depth_d, uint8_t *out, unsigned *n_engine)
{
    uint16_t sums[KM_ENGINE_SLOTS];
    unsigned n = 0;
    for (unsigned f = 0; f < 60; f++) {
        for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
            sums[i] = 3300;
        }
        sums[SA] = counts_for_depth(depth_a);
        sums[SD] = counts_for_depth(depth_d);
        km_engine_update(&r->e, sums);
        unsigned ne;
        const uint8_t *down = km_engine_down_slots(&r->e, &ne);
        n = km_socd_filter(&r->s, down, ne, 0, eng_depth, &r->e, out);
        if (n_engine != NULL) {
            *n_engine = ne;
        }
    }
    return n;
}

static void socd_engine_rig(void)
{
    rig_t r;
    uint8_t out[KM_ENGINE_MAX_DOWN];
    unsigned ne = 0;

    rig_init(&r, KM_SOCD_LAST);
    CHECK(match(out, rig_hold(&r, 250, 0, out, &ne), "A"));
    CHECK(match(out, rig_hold(&r, 250, 250, out, &ne), "D"));
    CHECK_EQ(ne, 2);                                          /* the engine itself still sees both keys */
    CHECK(match(out, rig_hold(&r, 250, 0, out, &ne), "A"));
    CHECK(match(out, rig_hold(&r, 0, 0, out, &ne), ""));
    CHECK(match(out, rig_hold(&r, 0, 250, out, &ne), "D"));
    CHECK(match(out, rig_hold(&r, 250, 250, out, &ne), "A"));
    CHECK(match(out, rig_hold(&r, 0, 250, out, &ne), "D"));

    rig_init(&r, KM_SOCD_FIRST);
    CHECK(match(out, rig_hold(&r, 250, 0, out, &ne), "A"));
    CHECK(match(out, rig_hold(&r, 250, 250, out, &ne), "A"));
    CHECK(match(out, rig_hold(&r, 0, 250, out, &ne), "D"));

    rig_init(&r, KM_SOCD_NEUTRAL);
    CHECK(match(out, rig_hold(&r, 250, 0, out, &ne), "A"));
    CHECK(match(out, rig_hold(&r, 250, 250, out, &ne), ""));
    CHECK(match(out, rig_hold(&r, 0, 250, out, &ne), "D"));

    /* deeper wins: the depths come from the engine's own filtered reading (a few 0.01 mm of rounding, so the test keeps
     * well away from the 30 boundary) */
    rig_init(&r, KM_SOCD_DEEPER);
    CHECK(match(out, rig_hold(&r, 200, 0, out, &ne), "A"));
    CHECK(match(out, rig_hold(&r, 200, 150, out, &ne), "A"));     /* both down, A deeper by 50 */
    CHECK(match(out, rig_hold(&r, 200, 220, out, &ne), "A"));     /* D deeper by 20: inside the hysteresis */
    CHECK(match(out, rig_hold(&r, 200, 245, out, &ne), "D"));     /* by 45: D wins */
    CHECK(match(out, rig_hold(&r, 200, 215, out, &ne), "D"));     /* D deeper by only 15: stays */
    CHECK(match(out, rig_hold(&r, 250, 215, out, &ne), "A"));     /* A deeper by 35 */
    CHECK(match(out, rig_hold(&r, 250, 0, out, &ne), "A"));
    CHECK(match(out, rig_hold(&r, 0, 0, out, &ne), ""));
    CHECK_EQ(r.s.owner[0], 0);
}

/* ---------------------------------------------------------------- 7. edge cases */

static void socd_edges(void)
{
    km_socd_t s;
    setup1(&s, KM_SOCD_LAST, 0);

    /* slots >= 76 pass through, the layer logic ignores them */
    const uint8_t odd[6] = { 200, SA, 76, SD, 255, 3 };
    uint8_t out[8];
    unsigned n = km_socd_filter(&s, odd, 6, 0, NULL, NULL, out);
    CHECK_EQ(n, 5);
    CHECK(out[0] == 200 && out[1] == 76 && out[2] == SD && out[3] == 255 && out[4] == 3);

    /* slot numbers >= 76 are no keys: they never take part, whatever else is in the table (76 sits right behind the role table) */
    const uint8_t lead[3] = { 76, SD, SA };
    n = km_socd_filter(&s, lead, 3, 0, NULL, NULL, out);
    CHECK_EQ(n, 2);
    CHECK(out[0] == 76 && out[1] == SA);
    const uint8_t lead2[3] = { 255, SD, SA };
    n = km_socd_filter(&s, lead2, 3, 0, NULL, NULL, out);
    CHECK_EQ(n, 2);
    CHECK(out[0] == 255 && out[1] == SA);

    /* repeated entries: suppression applies to every copy, order of the rest unchanged */
    const uint8_t rep[5] = { SA, 3, SA, SD, 3 };
    n = km_socd_filter(&s, rep, 5, 0, NULL, NULL, out);
    CHECK_EQ(n, 3);
    CHECK(out[0] == 3 && out[1] == SD && out[2] == 3);

    /* an entry listed twice counts at its FIRST place: A D A is "A pressed first" (LAST: D wins) */
    const uint8_t aba[3] = { SA, SD, SA };
    n = km_socd_filter(&s, aba, 3, 0, NULL, NULL, out);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0], SD);

    /* empty list */
    CHECK_EQ(km_socd_filter(&s, NULL, 0, 0, NULL, NULL, out), 0);
    CHECK(!km_socd_suppressed(&s, SA));

    /* the whole engine list, 64 entries, with the pair at the very end */
    uint8_t big[KM_ENGINE_MAX_DOWN], big_out[KM_ENGINE_MAX_DOWN];
    unsigned nb = 0;
    for (unsigned k = 0; k < KM_SOCD_SLOTS && nb < KM_ENGINE_MAX_DOWN - 2u; k++) {
        if (km_keymap_slot_is_key(k) && k != SA && k != SD) {
            big[nb++] = (uint8_t)k;
        }
    }
    big[nb++] = (uint8_t)SA;
    big[nb++] = (uint8_t)SD;
    CHECK_EQ(nb, 61);
    n = km_socd_filter(&s, big, nb, 0, NULL, NULL, big_out);
    CHECK_EQ(n, 60);
    CHECK(big_out[59] == SD && !in_list(big_out, n, SA));
    /* a list longer than a byte can index (the engine never makes one): entries past position 254 are passed on unseen */
    uint8_t huge[300], huge_out[300];
    memset(huge, (int)other_key(0), sizeof huge);
    huge[0] = (uint8_t)SA;
    huge[256] = (uint8_t)SD;                              /* 256 would wrap to 0 in a byte: it must not look like "pressed with A" */
    n = km_socd_filter(&s, huge, 300, 0, NULL, NULL, huge_out);
    CHECK_EQ(n, 300);
    CHECK(huge_out[0] == SA && huge_out[256] == SD);

    /* out of range query */
    CHECK(!km_socd_suppressed(&s, 76));
    CHECK(!km_socd_suppressed(&s, 1000));

    /* counters: a key counts when it becomes suppressed, not on every call */
    km_socd_t c;
    setup1(&c, KM_SOCD_LAST, 0);
    plist_t l = { { 0 }, 0 };
    p_press(&l, SA);
    run(&c, &l, 0, NULL, out);
    CHECK_EQ(c.suppress_events, 0);
    p_press(&l, SD);
    for (unsigned i = 0; i < 10; i++) {
        run(&c, &l, 0, NULL, out);
    }
    CHECK_EQ(c.suppress_events, 1);
    CHECK_EQ(c.calls_in_conflict, 10);
    p_release(&l, SD);
    run(&c, &l, 0, NULL, out);
    p_press(&l, SD);
    run(&c, &l, 0, NULL, out);
    CHECK_EQ(c.suppress_events, 2);
    setup1(&c, KM_SOCD_NEUTRAL, 0);                       /* set_pairs keeps the counters, init clears them */
    CHECK_EQ(c.suppress_events, 0);
    km_socd_pair_t np = pair(SA, SD, KM_SOCD_NEUTRAL, 0);
    CHECK(km_socd_set_pairs(&c, &np, 1));
    run(&c, &l, 0, NULL, out);
    CHECK_EQ(c.suppress_events, 2);                       /* neutral suppresses both keys */
}

void test_socd(void)
{
    socd_config();
    socd_last();
    socd_first();
    socd_neutral();
    socd_pairs_independent();
    socd_layer_flag();
    socd_exhaustive();
    socd_deeper();
    socd_random(300, false);
    socd_random(300, true);
    socd_no_stuck_suppression();
    socd_engine_rig();
    socd_edges();
}
