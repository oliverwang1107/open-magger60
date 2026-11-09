/*
 * test_layers_m6.c - M6 task P2: toggle keys (KM_CODE_TOGGLE_KEY), Mod-Morph (KM_CODE_MOD_MORPH) and tap-hold keys whose hold
 * action is an ordinary key, in km_layers; plus the regression test of the queued-release bug found by task P0.
 *
 * Same rig as test_layers_ext.c: km_layers_update_ms() is fed once per simulated millisecond and every change of the output is
 * recorded with its time. Expected values come from the rules written in km_layers.h, not from the code. The last tests are
 * randomised runs over tables full of every kind of key (plain, layer hold / toggle, tap-hold, one-shot, toggle key, Mod-Morph),
 * with faults (fail-safe reset, table reload, long gaps between updates, clock wrap), that check at every millisecond that
 * every reported usage has a possible cause and that after everything is released nothing is left over but the toggle keys that
 * a model of the rules says must be on.
 */
#include <string.h>

#include "km/km_keymap.h"
#include "km/km_layers.h"
#include "km_test.h"

enum { A = 73, S = 72, D = 74, F = 49, Q = 2, W = 75, E = 6, R = 42, I = 1, SPACE = 63, LSHIFT = 0, RSHIFT = 28, LCTRL = 5, FN = 14 };
#define U_A 0x04
#define U_B 0x05
#define U_D 0x07
#define U_S 0x16
#define U_X 0x1B
#define U_Z 0x1D
#define U_SPACE 0x2C
#define U_BKSP 0x2A
#define U_DEL 0x4C
#define U_GRAVE 0x35
#define U_LCTRL 0xE0
#define U_LSHIFT 0xE1
#define U_LALT 0xE2
#define U_RSHIFT 0xE5
#define M_LCTRL 0x01
#define M_LSHIFT 0x02
#define M_RSHIFT 0x20
#define M_SHIFTS (M_LSHIFT | M_RSHIFT)
#define STEP KM_LAYERS_STEP_MS
#define TERM 200

/* ---------------------------------------------------------------- the rig */

typedef struct {
    uint32_t t;
    uint8_t  kbd[16];
    uint8_t  n;
} frame_t;

static uint32_t g_now, g_start;
static uint8_t  g_down[KM_KEYMAP_SLOTS];
static unsigned g_n;
static km_layers_out_t g_out;
static frame_t  g_tr[8192];
static unsigned g_trn;

static uint8_t g_code[KM_LAYERS][KM_KEYMAP_SLOTS];
static uint8_t g_arg[KM_LAYERS][KM_KEYMAP_SLOTS][KM_KEYMAP_ARGS];
static km_layer_opts_t g_opts;
static km_morph_rec_t g_rec[KM_MORPH_MAX];
static unsigned g_rec_n;

static void record(void)
{
    frame_t f;
    memset(&f, 0, sizeof f);
    f.t = g_now - g_start;
    f.n = g_out.kbd_n < sizeof f.kbd ? g_out.kbd_n : (uint8_t)sizeof f.kbd;
    memcpy(f.kbd, g_out.kbd, f.n);
    if (g_trn > 0 && g_tr[g_trn - 1].n == f.n && memcmp(g_tr[g_trn - 1].kbd, f.kbd, f.n) == 0) {
        return;
    }
    if (g_trn < sizeof g_tr / sizeof g_tr[0]) {
        g_tr[g_trn++] = f;
    }
}

static void upd(void)
{
    km_layers_update_ms(g_down, g_n, g_now, &g_out);
    record();
}

static void adv(unsigned ms)
{
    while (ms--) {
        g_now++;
        upd();
    }
}

static void press(unsigned slot)
{
    g_down[g_n++] = (uint8_t)slot;
    upd();
}

static void rel(unsigned slot)
{
    for (unsigned i = 0; i < g_n; i++) {
        if (g_down[i] == slot) {
            memmove(&g_down[i], &g_down[i + 1u], g_n - i - 1u);
            g_n--;
            break;
        }
    }
    upd();
}

static void tbl_set(unsigned layer, unsigned slot, uint8_t code, uint8_t a0, uint8_t a1)
{
    g_code[layer][slot] = code;
    g_arg[layer][slot][0] = a0;
    g_arg[layer][slot][1] = a1;
}

static void tbl_load(void)
{
    CHECK(km_layers_set_table(g_code, g_arg, &g_opts));
}

/* the default table with the slots used here set to something definite (so the tests do not depend on the default map): the
   slots of this file are nothing on layer 0 and transparent above, then the plain keys are put back */
static void tbl_begin(unsigned term, unsigned policy, unsigned oneshot)
{
    km_keymap_default_table(g_code);
    memset(g_arg, 0, sizeof g_arg);
    g_opts.tap_term_ms = (uint16_t)term;
    g_opts.tap_policy = (uint8_t)policy;
    g_opts.oneshot_ms = (uint16_t)oneshot;
    const unsigned used[] = { A, S, D, F, Q, W, E, R, I, SPACE, LSHIFT, RSHIFT, LCTRL };
    for (unsigned i = 0; i < sizeof used / sizeof used[0]; i++) {
        tbl_set(0, used[i], 0, 0, 0);
        for (unsigned l = 1; l < KM_LAYERS; l++) {
            tbl_set(l, used[i], KM_CODE_TRANSPARENT, 0, 0);
        }
    }
    tbl_set(0, A, U_A, 0, 0);
    tbl_set(0, S, U_S, 0, 0);
    tbl_set(0, D, U_D, 0, 0);
    tbl_set(0, F, 0x09, 0, 0);
    tbl_set(0, LSHIFT, U_LSHIFT, 0, 0);
    tbl_set(0, RSHIFT, U_RSHIFT, 0, 0);
    tbl_set(0, LCTRL, U_LCTRL, 0, 0);
    tbl_set(0, SPACE, U_SPACE, 0, 0);
    tbl_set(0, Q, U_BKSP, 0, 0);
    g_rec_n = 0;
    memset(g_rec, 0, sizeof g_rec);
}

static void morph_rec(unsigned i, uint8_t mask, uint8_t usage, uint8_t flags)
{
    g_rec[i].mod_mask = mask;
    g_rec[i].usage = usage;
    g_rec[i].flags = flags;
    g_rec[i].rsvd = 0;
    if (i >= g_rec_n) {
        g_rec_n = i + 1u;
    }
}

/* load the table and the Mod-Morph records, start the clock */
static void rig_start(uint32_t t0)
{
    tbl_load();
    km_layers_set_morph(g_rec, g_rec_n);
    km_layers_reset();
    g_n = 0;
    g_now = t0;
    g_start = t0;
    g_trn = 0;
    upd();
}

static bool has(uint8_t u)
{
    for (unsigned i = 0; i < g_out.kbd_n; i++) {
        if (g_out.kbd[i] == u) {
            return true;
        }
    }
    return false;
}

/* the output is exactly this set of usages (order does not matter) */
static bool out_is(const uint8_t *want, unsigned n)
{
    if (g_out.kbd_n != n) {
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        if (!has(want[i])) {
            return false;
        }
    }
    return true;
}

#define OUT_IS(...)                                                             \
    do {                                                                        \
        const uint8_t w_[] = { __VA_ARGS__ };                                   \
        CHECK(out_is(w_, sizeof w_ / sizeof w_[0]));                            \
    } while (0)
#define OUT_NONE() CHECK_EQ(g_out.kbd_n, 0)

/* times (relative to the start) at which usage u was first reported, -1 = never; and how many times it appeared */
static int first_time(uint8_t u)
{
    for (unsigned i = 0; i < g_trn; i++) {
        for (unsigned j = 0; j < g_tr[i].n; j++) {
            if (g_tr[i].kbd[j] == u) {
                return (int)g_tr[i].t;
            }
        }
    }
    return -1;
}

static unsigned appearances(uint8_t u)
{
    unsigned c = 0;
    bool was = false;
    for (unsigned i = 0; i < g_trn; i++) {
        bool is = false;
        for (unsigned j = 0; j < g_tr[i].n; j++) {
            is |= g_tr[i].kbd[j] == u;
        }
        c += (is && !was) ? 1u : 0u;
        was = is;
    }
    return c;
}

static bool locked_is(unsigned slot, uint8_t code, uint8_t a0, uint8_t a1)
{
    km_layers_entry_t e = { 0xAA, 0xAA, 0xAA };
    return km_layers_locked_entry(slot, &e) && e.code == code && e.arg0 == a0 && e.arg1 == a1;
}

static bool is_locked(unsigned slot)
{
    km_layers_entry_t e;
    return km_layers_locked_entry(slot, &e);
}

static void expect_idle(void)
{
    CHECK_EQ(g_out.kbd_n, 0);
    CHECK_EQ(g_out.consumer, 0);
    CHECK(!g_out.pending);
    CHECK_EQ(g_out.armed, 0);
    CHECK_EQ(g_out.toggle_keys, 0);
    CHECK_EQ(g_out.suppressed, 0);
}

/* ---------------------------------------------------------------- toggle keys */

static void test_toggle_key_basic(void)
{
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, W, KM_CODE_TOGGLE_KEY, U_Z, 0);
    rig_start(1000);
    OUT_NONE();
    press(W);
    OUT_IS(U_Z);                                            /* on at the press, no waiting */
    CHECK_EQ(g_out.toggle_keys, 1);
    CHECK(locked_is(W, KM_CODE_TOGGLE_KEY, U_Z, 0));
    adv(50);
    rel(W);
    adv(50);
    OUT_IS(U_Z);                                            /* releasing the key does nothing */
    CHECK_EQ(g_out.toggle_keys, 1);
    CHECK(!is_locked(W));
    press(A);                                               /* other keys come and go, the toggled usage stays */
    OUT_IS(U_Z, U_A);
    CHECK(g_out.kbd[0] == U_A && g_out.kbd[1] == U_Z);      /* the keys that are held first, then the toggled usages */
    rel(A);
    OUT_IS(U_Z);
    adv(5000);
    OUT_IS(U_Z);                                            /* it does not time out */
    press(W);
    OUT_NONE();                                             /* the second press switches it off */
    CHECK_EQ(g_out.toggle_keys, 0);
    adv(20);
    rel(W);
    adv(20);
    expect_idle();
    press(W);                                               /* and on again */
    OUT_IS(U_Z);
    rel(W);
    press(W);
    OUT_NONE();
    rel(W);
    adv(STEP);
    expect_idle();
    CHECK_EQ(appearances(U_Z), 2);
}

static void test_toggle_key_sharing(void)
{
    /* two toggle keys with different usages come out in the order they were switched on, after the keys that are held */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, W, KM_CODE_TOGGLE_KEY, U_Z, 0);
    tbl_set(0, E, KM_CODE_TOGGLE_KEY, U_X, 0);
    tbl_set(0, R, KM_CODE_TOGGLE_KEY, U_Z, 0);               /* the same usage as W */
    tbl_set(0, D, U_Z, 0, 0);                                /* a plain key with that usage */
    rig_start(1000);
    press(A);
    press(E);
    press(W);
    CHECK_EQ(g_out.kbd_n, 3);
    CHECK(g_out.kbd[0] == U_A && g_out.kbd[1] == U_X && g_out.kbd[2] == U_Z);
    CHECK_EQ(g_out.toggle_keys, 2);
    rel(E);
    rel(W);
    rel(A);
    adv(STEP);
    CHECK(g_out.kbd_n == 2 && g_out.kbd[0] == U_X && g_out.kbd[1] == U_Z);
    press(E);                                                /* X off, Z stays, the order of the rest does not change */
    OUT_IS(U_Z);
    rel(E);
    /* the state belongs to the usage: the other key with that usage switches it off */
    press(R);
    OUT_NONE();
    rel(R);
    press(R);
    OUT_IS(U_Z);
    rel(R);
    /* a plain key with the same usage: reported once, and the toggle keeps it on after the plain key is let go */
    press(D);
    CHECK_EQ(g_out.kbd_n, 1);
    OUT_IS(U_Z);
    rel(D);
    OUT_IS(U_Z);
    press(D);
    press(W);                                                /* off while D is held: D still reports it */
    OUT_IS(U_Z);
    CHECK_EQ(g_out.toggle_keys, 0);
    rel(W);
    rel(D);
    adv(STEP);
    expect_idle();
}

static void test_toggle_key_modifier(void)
{
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, W, KM_CODE_TOGGLE_KEY, U_LSHIFT, 0);
    rig_start(1000);
    press(W);
    OUT_IS(U_LSHIFT);
    rel(W);
    press(A);
    OUT_IS(U_A, U_LSHIFT);
    press(LSHIFT);                                           /* the real Shift key as well: reported once */
    OUT_IS(U_A, U_LSHIFT);
    rel(LSHIFT);
    OUT_IS(U_A, U_LSHIFT);
    rel(A);
    press(W);
    OUT_NONE();
    rel(W);
    adv(STEP);
    expect_idle();
}

static void test_toggle_key_cap(void)
{
    /* more toggle keys than KM_LAYERS_TOGGLE_KEYS: the extra one stays off (nothing sticks), the others still switch off */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    unsigned slot[KM_LAYERS_TOGGLE_KEYS + 1u], n = 0;
    for (unsigned s = 0; s < KM_KEYMAP_SLOTS && n < KM_LAYERS_TOGGLE_KEYS + 1u; s++) {
        if (km_keymap_slot_is_key(s) && s != FN) {
            slot[n] = s;
            tbl_set(0, s, KM_CODE_TOGGLE_KEY, (uint8_t)(0x04u + n), 0);
            for (unsigned l = 1; l < KM_LAYERS; l++) {
                tbl_set(l, s, KM_CODE_TRANSPARENT, 0, 0);
            }
            n++;
        }
    }
    CHECK_EQ(n, KM_LAYERS_TOGGLE_KEYS + 1u);
    rig_start(1000);
    for (unsigned i = 0; i < KM_LAYERS_TOGGLE_KEYS; i++) {
        press(slot[i]);
        rel(slot[i]);
    }
    CHECK_EQ(g_out.toggle_keys, KM_LAYERS_TOGGLE_KEYS);
    CHECK_EQ(g_out.kbd_n, KM_LAYERS_TOGGLE_KEYS);
    press(slot[KM_LAYERS_TOGGLE_KEYS]);                      /* the 17th */
    rel(slot[KM_LAYERS_TOGGLE_KEYS]);
    CHECK_EQ(g_out.toggle_keys, KM_LAYERS_TOGGLE_KEYS);
    CHECK(!has((uint8_t)(0x04u + KM_LAYERS_TOGGLE_KEYS)));
    press(slot[3]);                                          /* one off ... */
    rel(slot[3]);
    CHECK_EQ(g_out.toggle_keys, KM_LAYERS_TOGGLE_KEYS - 1u);
    press(slot[KM_LAYERS_TOGGLE_KEYS]);                      /* ... and the 17th fits now */
    rel(slot[KM_LAYERS_TOGGLE_KEYS]);
    CHECK_EQ(g_out.toggle_keys, KM_LAYERS_TOGGLE_KEYS);
    CHECK(has((uint8_t)(0x04u + KM_LAYERS_TOGGLE_KEYS)) && !has(0x07));
    for (unsigned i = 0; i < KM_LAYERS_TOGGLE_KEYS + 1u; i++) {
        if (i != 3) {
            press(slot[i]);
            rel(slot[i]);
        }
    }
    adv(STEP);
    expect_idle();
}

static void test_toggle_key_ends(void)
{
    /* a table change (profile switch, settings commit) switches the toggled usages off, even while the toggle key is down */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, W, KM_CODE_TOGGLE_KEY, U_Z, 0);
    tbl_set(0, E, KM_CODE_TOGGLE_KEY, U_X, 0);
    rig_start(1000);
    press(W);
    rel(W);
    press(E);
    OUT_IS(U_Z, U_X);
    tbl_load();
    adv(1);
    OUT_NONE();
    CHECK_EQ(g_out.toggle_keys, 0);
    CHECK(locked_is(E, KM_CODE_TOGGLE_KEY, U_X, 0));         /* the key under the finger keeps its entry */
    rel(E);
    adv(STEP);
    expect_idle();                                           /* and its release does not toggle anything */
    /* km_layers_reset (boot, USB reset, fail-safe release) */
    press(W);
    OUT_IS(U_Z);
    km_layers_reset();
    g_n = 0;
    adv(1);
    expect_idle();
    /* the Mod-Morph records are NOT part of the state a reset forgets */
    morph_rec(0, M_SHIFTS, U_DEL, 0);
    tbl_set(0, Q, KM_CODE_MOD_MORPH, U_BKSP, 0);
    rig_start(1000);
    km_layers_reset();
    press(LSHIFT);
    press(Q);
    OUT_IS(U_LSHIFT, U_DEL);
    rel(Q);
    rel(LSHIFT);
    km_layers_set_morph(NULL, 0);
}

static void test_toggle_key_with_osm(void)
{
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, LSHIFT, KM_CODE_ONESHOT_MOD, U_LSHIFT, 0);
    tbl_set(0, W, KM_CODE_TOGGLE_KEY, U_Z, 0);
    rig_start(1000);
    /* a toggle key does not use up an armed one-shot modifier ... */
    press(LSHIFT);
    adv(50);
    rel(LSHIFT);
    CHECK_EQ(g_out.armed, M_LSHIFT);
    press(W);
    CHECK_EQ(g_out.armed, M_LSHIFT);
    OUT_IS(U_Z, U_LSHIFT);
    rel(W);
    press(A);                                                /* ... the next ordinary key does */
    OUT_IS(U_Z, U_A, U_LSHIFT);
    rel(A);
    OUT_IS(U_Z);
    CHECK_EQ(g_out.armed, 0);
    /* ... but it counts as 'another key' for a one-shot modifier that is held: no arming when it is let go */
    tbl_set(0, LSHIFT, KM_CODE_ONESHOT_MOD, U_LSHIFT, 0);
    press(W);                                                /* Z off */
    rel(W);
    press(LSHIFT);
    press(W);
    OUT_IS(U_LSHIFT, U_Z);
    rel(W);
    adv(50);
    rel(LSHIFT);
    CHECK_EQ(g_out.armed, 0);
    OUT_IS(U_Z);
    press(W);
    rel(W);
    adv(STEP);
    expect_idle();
}

static void test_toggle_key_with_tap_hold(void)
{
    /* a toggle key pressed behind an undecided tap-hold key waits for the decision and uses the layer it gives */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, SPACE, KM_CODE_TAP_HOLD, U_SPACE, KM_CODE_HOLD_L1);
    tbl_set(0, W, KM_CODE_TOGGLE_KEY, U_Z, 0);
    tbl_set(1, W, KM_CODE_TOGGLE_KEY, U_X, 0);
    rig_start(1000);
    press(SPACE);
    adv(10);
    press(W);
    adv(TERM / 2);
    OUT_NONE();                                              /* nothing yet: the press waits */
    CHECK(g_out.pending);
    adv(TERM);                                               /* the hold is decided; the toggle key replays on layer 1 */
    adv(3 * STEP);
    CHECK(has(U_X) && !has(U_Z));
    rel(W);
    rel(SPACE);
    adv(3 * STEP);
    OUT_IS(U_X);
    press(W);                                                /* layer 0 again: Z */
    OUT_IS(U_X, U_Z);
    rel(W);
    press(W);
    OUT_IS(U_X);
    rel(W);
    /* a tap: the tap action first, then the toggle */
    km_layers_reset();
    g_n = 0;
    rig_start(5000);
    press(SPACE);
    adv(10);
    press(W);
    adv(10);
    rel(SPACE);
    adv(TERM);
    CHECK(first_time(U_SPACE) >= 0 && first_time(U_Z) > first_time(U_SPACE));
    CHECK(has(U_Z));
    rel(W);
    press(W);
    rel(W);
    adv(STEP);
    expect_idle();
}

static void test_toggle_key_on_layer(void)
{
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(1, S, KM_CODE_TOGGLE_KEY, U_X, 0);
    rig_start(1000);
    press(FN);
    press(S);
    OUT_IS(U_X);
    rel(FN);                                                 /* the layer ends under the finger: nothing changes */
    adv(30);
    OUT_IS(U_X);
    rel(S);
    adv(30);
    OUT_IS(U_X);
    press(S);                                                /* layer 0: the plain S, the toggle stays */
    OUT_IS(U_X, U_S);
    rel(S);
    press(FN);
    press(S);
    OUT_NONE();
    rel(S);
    rel(FN);
    adv(STEP);
    expect_idle();
}

/* ---------------------------------------------------------------- Mod-Morph */

static void morph_table(void)
{
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, Q, KM_CODE_MOD_MORPH, U_BKSP, 0);
    morph_rec(0, M_SHIFTS, U_DEL, 0);
}

static void test_morph_basic(void)
{
    morph_table();
    rig_start(1000);
    press(Q);
    OUT_IS(U_BKSP);
    CHECK(locked_is(Q, KM_CODE_MOD_MORPH, U_BKSP, 0));
    rel(Q);
    OUT_NONE();
    press(LSHIFT);
    press(Q);
    OUT_IS(U_LSHIFT, U_DEL);                                 /* without the suppress flag the Shift goes along */
    CHECK_EQ(g_out.suppressed, 0);
    rel(Q);
    OUT_IS(U_LSHIFT);
    rel(LSHIFT);
    OUT_NONE();
    press(RSHIFT);                                           /* left and right Shift are separate bits of the mask */
    press(Q);
    OUT_IS(U_RSHIFT, U_DEL);
    rel(Q);
    rel(RSHIFT);
    press(LCTRL);                                            /* a modifier that is not in the mask does not morph */
    press(Q);
    OUT_IS(U_LCTRL, U_BKSP);
    rel(Q);
    rel(LCTRL);
    adv(STEP);
    expect_idle();
}

static void test_morph_lock(void)
{
    morph_table();
    rig_start(1000);
    /* the choice is made when the key goes down and kept until it goes up */
    press(Q);
    adv(30);
    press(LSHIFT);                                           /* Shift arrives later: the key stays Backspace */
    OUT_IS(U_BKSP, U_LSHIFT);
    rel(Q);
    OUT_IS(U_LSHIFT);                                        /* the release is the release of Backspace */
    rel(LSHIFT);
    OUT_NONE();
    press(LSHIFT);
    press(Q);
    OUT_IS(U_LSHIFT, U_DEL);
    rel(LSHIFT);                                             /* Shift goes first: the key stays Delete, nothing sticks */
    OUT_IS(U_DEL);
    adv(100);
    OUT_IS(U_DEL);
    rel(Q);
    OUT_NONE();
    /* pressed again without Shift: the key is plain again */
    press(Q);
    OUT_IS(U_BKSP);
    rel(Q);
    adv(STEP);
    expect_idle();
}

static void test_morph_suppress(void)
{
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, Q, KM_CODE_MOD_MORPH, U_BKSP, 0);
    tbl_set(0, R, KM_CODE_MOD_MORPH, U_BKSP, 1);
    morph_rec(0, M_SHIFTS, U_DEL, KM_MORPH_FLAG_SUPPRESS);
    morph_rec(1, M_LSHIFT, U_GRAVE, KM_MORPH_FLAG_SUPPRESS);
    rig_start(1000);
    /* Shift is hidden while the morph key is down and comes back when it is released */
    press(LSHIFT);
    OUT_IS(U_LSHIFT);
    press(Q);
    OUT_IS(U_DEL);
    CHECK_EQ(g_out.suppressed, M_LSHIFT);
    adv(100);
    OUT_IS(U_DEL);
    rel(Q);
    OUT_IS(U_LSHIFT);
    CHECK_EQ(g_out.suppressed, 0);
    rel(LSHIFT);
    OUT_NONE();
    /* Shift is let go while the morph key is down: no Shift appears when the morph key goes up */
    press(LSHIFT);
    press(Q);
    OUT_IS(U_DEL);
    rel(LSHIFT);
    OUT_IS(U_DEL);
    CHECK_EQ(g_out.suppressed, 0);
    rel(Q);
    OUT_NONE();
    /* Shift is pressed again while the morph key is down: that is a new press and it is seen */
    press(LSHIFT);
    press(Q);
    rel(LSHIFT);
    adv(20);
    press(LSHIFT);
    OUT_IS(U_DEL, U_LSHIFT);
    rel(Q);
    OUT_IS(U_LSHIFT);
    rel(LSHIFT);
    OUT_NONE();
    /* only the modifiers of the mask that were sent are hidden: Ctrl stays, a right Shift that is not in the mask stays */
    press(LCTRL);
    press(LSHIFT);
    press(Q);
    OUT_IS(U_LCTRL, U_DEL);
    rel(Q);
    OUT_IS(U_LCTRL, U_LSHIFT);
    rel(LSHIFT);
    rel(LCTRL);
    press(RSHIFT);
    press(R);                                                /* record 1: only the LEFT Shift triggers: plain Backspace, Shift stays */
    OUT_IS(U_RSHIFT, U_BKSP);
    rel(R);
    rel(RSHIFT);
    /* both Shifts down: both hidden */
    press(LSHIFT);
    press(RSHIFT);
    press(Q);
    OUT_IS(U_DEL);
    CHECK_EQ(g_out.suppressed, M_SHIFTS);
    rel(RSHIFT);
    OUT_IS(U_DEL);
    CHECK_EQ(g_out.suppressed, M_LSHIFT);
    rel(Q);
    OUT_IS(U_LSHIFT);
    rel(LSHIFT);
    /* two morph keys: Shift stays hidden until both are up */
    press(LSHIFT);
    press(Q);
    press(R);
    OUT_IS(U_DEL, U_GRAVE);
    rel(Q);
    OUT_IS(U_GRAVE);
    rel(R);
    OUT_IS(U_LSHIFT);
    rel(LSHIFT);
    adv(STEP);
    expect_idle();
}

static void test_morph_sources_of_modifiers(void)
{
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, Q, KM_CODE_MOD_MORPH, U_BKSP, 0);
    tbl_set(0, R, KM_CODE_MOD_MORPH, U_BKSP, 1);
    tbl_set(0, E, KM_CODE_TOGGLE_KEY, U_LSHIFT, 0);
    tbl_set(0, F, KM_CODE_ONESHOT_MOD, U_LSHIFT, 0);
    morph_rec(0, M_SHIFTS, U_DEL, KM_MORPH_FLAG_SUPPRESS);
    morph_rec(1, M_SHIFTS, U_DEL, 0);
    rig_start(1000);
    /* a toggled modifier key counts, and is hidden like a held one */
    press(E);
    rel(E);
    OUT_IS(U_LSHIFT);
    press(Q);
    OUT_IS(U_DEL);
    rel(Q);
    OUT_IS(U_LSHIFT);
    press(E);                                                /* off */
    rel(E);
    OUT_NONE();
    /* an armed one-shot modifier counts, rides on the morph key, and is used up */
    press(F);
    adv(40);
    rel(F);
    CHECK_EQ(g_out.armed, M_LSHIFT);
    press(R);                                                /* no suppress: Shift + Delete */
    OUT_IS(U_DEL, U_LSHIFT);
    CHECK_EQ(g_out.armed, 0);
    rel(R);
    OUT_NONE();
    press(F);
    adv(40);
    rel(F);
    press(Q);                                                /* suppress: Delete alone, and the one-shot is gone */
    OUT_IS(U_DEL);
    CHECK_EQ(g_out.armed, 0);
    rel(Q);
    OUT_NONE();
    adv(STEP);
    expect_idle();
    /* a held one-shot modifier key is a held modifier */
    press(F);
    press(Q);
    OUT_IS(U_DEL);
    rel(Q);
    OUT_IS(U_LSHIFT);
    rel(F);
    adv(STEP);
    expect_idle();
}

static void test_morph_edges(void)
{
    morph_table();
    morph_rec(1, M_LSHIFT, U_LCTRL, KM_MORPH_FLAG_SUPPRESS);         /* a morph that produces a modifier */
    morph_rec(2, M_LSHIFT, U_LSHIFT, KM_MORPH_FLAG_SUPPRESS);        /* ... its own trigger: never hidden */
    tbl_set(0, R, KM_CODE_MOD_MORPH, U_BKSP, 1);
    tbl_set(0, E, KM_CODE_MOD_MORPH, U_BKSP, 2);
    tbl_set(0, I, KM_CODE_MOD_MORPH, U_BKSP, 6);                     /* no such record */
    rig_start(1000);
    press(LSHIFT);
    press(R);
    OUT_IS(U_LCTRL);                                                 /* Shift hidden, Ctrl sent */
    rel(R);
    OUT_IS(U_LSHIFT);
    press(E);
    OUT_IS(U_LSHIFT);
    CHECK_EQ(g_out.suppressed, 0);
    rel(LSHIFT);                                                     /* the morph key sends Shift itself now */
    OUT_IS(U_LSHIFT);
    rel(E);
    OUT_NONE();
    press(LSHIFT);
    press(I);                                                        /* record 6 does not exist: plain key */
    OUT_IS(U_LSHIFT, U_BKSP);
    rel(I);
    rel(LSHIFT);
    /* records that break the rules are ignored: mask 0, usage 0x03, an unknown flag, a reserved byte, usage 0xE8; the rest work.
       More than KM_MORPH_MAX records are cut (the pointer is only read up to the maximum) */
    km_morph_rec_t bad[KM_MORPH_MAX];
    for (unsigned i = 0; i < KM_MORPH_MAX; i++) {
        bad[i].mod_mask = M_LSHIFT;
        bad[i].usage = U_DEL;
        bad[i].flags = 0;
        bad[i].rsvd = 0;
    }
    bad[0].mod_mask = 0;
    bad[1].usage = 0x03;
    bad[2].flags = 0x02;
    bad[3].rsvd = 1;
    bad[4].usage = 0xE8;
    const unsigned slots[] = { S, D, F, W, A, E, R, I };
    for (unsigned i = 0; i < 8u; i++) {
        tbl_set(0, slots[i], KM_CODE_MOD_MORPH, U_B, (uint8_t)i);
    }
    tbl_load();
    km_layers_set_morph(bad, KM_MORPH_MAX + 100u);
    km_layers_reset();
    g_n = 0;
    press(LSHIFT);
    for (unsigned i = 0; i < 8u; i++) {
        press(slots[i]);
        if (i < 5u) {
            CHECK(has(U_B) && !has(U_DEL));
        } else {
            CHECK(has(U_DEL) && !has(U_B));
        }
        rel(slots[i]);
    }
    rel(LSHIFT);
    /* no records at all: every Mod-Morph key is its plain key */
    km_layers_set_morph(NULL, 5);
    press(LSHIFT);
    press(S);
    OUT_IS(U_LSHIFT, U_B);
    rel(S);
    rel(LSHIFT);
    km_layers_set_morph(bad, 0);
    press(LSHIFT);
    press(E);
    OUT_IS(U_LSHIFT, U_B);
    rel(E);
    rel(LSHIFT);
    /* the records can change while a morph key is down: the key keeps what it was pressed with, release matches */
    morph_rec(0, M_LSHIFT, U_DEL, KM_MORPH_FLAG_SUPPRESS);
    tbl_set(0, Q, KM_CODE_MOD_MORPH, U_BKSP, 0);
    tbl_load();
    km_layers_set_morph(g_rec, g_rec_n);
    press(LSHIFT);
    press(Q);
    OUT_IS(U_DEL);
    km_layers_set_morph(NULL, 0);
    adv(10);
    OUT_IS(U_DEL);
    rel(LSHIFT);
    OUT_IS(U_DEL);
    rel(Q);
    adv(STEP);
    expect_idle();
}

static void test_morph_more(void)
{
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, Q, KM_CODE_MOD_MORPH, U_BKSP, 0);
    tbl_set(0, R, KM_CODE_MOD_MORPH, U_BKSP, 1);
    tbl_set(0, E, KM_CODE_MOD_MORPH, U_BKSP, 2);
    tbl_set(0, F, KM_CODE_ONESHOT_MOD, U_LSHIFT, 0);
    morph_rec(0, M_SHIFTS, U_DEL, KM_MORPH_FLAG_SUPPRESS);
    morph_rec(1, M_LSHIFT, U_GRAVE, KM_MORPH_FLAG_SUPPRESS);
    morph_rec(2, M_LSHIFT, U_LCTRL, 0);                      /* a morph into a modifier */
    rig_start(1000);
    /* a morph key whose output is a modifier does not use up an armed one-shot modifier (a plain modifier never does) */
    press(F);
    adv(40);
    rel(F);
    CHECK_EQ(g_out.armed, M_LSHIFT);
    press(E);
    OUT_IS(U_LCTRL, U_LSHIFT);
    CHECK_EQ(g_out.armed, M_LSHIFT);
    press(A);                                                /* the next ordinary key takes it */
    OUT_IS(U_LCTRL, U_LSHIFT, U_A);
    CHECK_EQ(g_out.armed, 0);
    rel(A);
    rel(E);
    adv(STEP);
    expect_idle();
    /* only the modifiers that were being sent when the morph key went down are hidden: a Shift that goes down in the same
       update, after the morph key in the list, is not (record 0 has both Shifts in its mask) */
    press(RSHIFT);
    g_down[g_n++] = Q;
    g_down[g_n++] = LSHIFT;
    upd();
    OUT_IS(U_DEL, U_LSHIFT);
    CHECK_EQ(g_out.suppressed, M_RSHIFT);
    rel(Q);
    rel(LSHIFT);
    rel(RSHIFT);
    adv(STEP);
    expect_idle();
    /* two morph keys that hide different modifiers: both are hidden, each comes back with its own key */
    tbl_set(0, R, KM_CODE_MOD_MORPH, U_BKSP, 3);
    morph_rec(3, M_RSHIFT, U_GRAVE, KM_MORPH_FLAG_SUPPRESS);
    tbl_set(0, Q, KM_CODE_MOD_MORPH, U_BKSP, 1);
    morph_rec(1, M_LSHIFT, U_DEL, KM_MORPH_FLAG_SUPPRESS);
    rig_start(1000);
    press(LSHIFT);
    press(RSHIFT);
    press(Q);
    OUT_IS(U_RSHIFT, U_DEL);
    press(R);
    OUT_IS(U_DEL, U_GRAVE);
    CHECK_EQ(g_out.suppressed, M_SHIFTS);
    rel(Q);
    OUT_IS(U_LSHIFT, U_GRAVE);
    rel(R);
    OUT_IS(U_LSHIFT, U_RSHIFT);
    rel(LSHIFT);
    rel(RSHIFT);
    adv(STEP);
    expect_idle();
}

static void test_morph_with_tap_hold(void)
{
    /* a modifier that comes from a tap-hold key counts once it is decided, not before */
    tbl_begin(TERM, KM_TAP_POLICY_OTHER_KEY, 2000);
    tbl_set(0, SPACE, KM_CODE_TAP_HOLD, U_SPACE, U_LSHIFT);
    tbl_set(0, Q, KM_CODE_MOD_MORPH, U_BKSP, 0);
    morph_rec(0, M_SHIFTS, U_DEL, 0);
    rig_start(1000);
    press(SPACE);
    adv(5);
    press(Q);                                                /* another key: the hold is decided, then the morph key is handled */
    adv(60);
    OUT_IS(U_LSHIFT, U_DEL);
    CHECK(first_time(U_DEL) - first_time(U_LSHIFT) >= (int)STEP);   /* the Shift is seen first */
    rel(Q);
    rel(SPACE);
    adv(STEP);
    expect_idle();

    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);            /* a tap of the tap-hold key: no Shift, the morph key is plain */
    tbl_set(0, SPACE, KM_CODE_TAP_HOLD, U_SPACE, U_LSHIFT);
    tbl_set(0, Q, KM_CODE_MOD_MORPH, U_BKSP, 0);
    morph_rec(0, M_SHIFTS, U_DEL, 0);
    rig_start(1000);
    press(SPACE);
    adv(5);
    press(Q);
    adv(5);
    rel(SPACE);
    adv(60);
    CHECK(has(U_BKSP) && !has(U_DEL) && !has(U_LSHIFT));
    CHECK(first_time(U_SPACE) >= 0 && first_time(U_BKSP) > first_time(U_SPACE));
    rel(Q);
    adv(STEP);
    expect_idle();

    rig_start(1000);                                         /* the hold comes by the timeout */
    press(SPACE);
    adv(10);
    press(Q);
    adv(TERM + 50);
    OUT_IS(U_LSHIFT, U_DEL);
    rel(SPACE);
    adv(STEP);
    OUT_IS(U_DEL);                                           /* the Shift goes, the morphed key stays what it is until released */
    rel(Q);
    adv(STEP);
    expect_idle();
}

/* ---------------------------------------------------------------- tap-hold with an ordinary key as the hold action */

static void test_tap_hold_key_hold(void)
{
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, SPACE, KM_CODE_TAP_HOLD, U_SPACE, U_B);
    rig_start(1000);
    press(SPACE);                                            /* tap: the tap action for STEP ms, never the hold key */
    adv(50);
    rel(SPACE);
    adv(40);
    CHECK_EQ(appearances(U_SPACE), 1);
    CHECK_EQ(appearances(U_B), 0);
    expect_idle();
    press(SPACE);                                            /* hold */
    adv(TERM - 2);
    OUT_NONE();
    CHECK(g_out.pending);
    adv(3);
    OUT_IS(U_B);
    CHECK(!g_out.pending);
    CHECK(locked_is(SPACE, KM_CODE_TAP_HOLD, U_SPACE, U_B));
    adv(1000);
    OUT_IS(U_B);
    rel(SPACE);
    OUT_NONE();
    CHECK(!is_locked(SPACE));
    /* the hold usage is also on a plain key: reported once, and held by whichever is still down */
    tbl_set(0, S, U_B, 0, 0);
    rig_start(1000);
    press(S);
    press(SPACE);
    adv(TERM + 5);
    CHECK_EQ(g_out.kbd_n, 1);
    rel(S);
    OUT_IS(U_B);
    rel(SPACE);
    OUT_NONE();
    /* policies: another key decides, and the hold key is seen before it */
    tbl_begin(TERM, KM_TAP_POLICY_OTHER_KEY, 2000);
    tbl_set(0, SPACE, KM_CODE_TAP_HOLD, U_SPACE, U_B);
    rig_start(1000);
    press(SPACE);
    adv(3);
    press(D);
    adv(60);
    OUT_IS(U_B, U_D);
    CHECK(first_time(U_D) - first_time(U_B) >= (int)STEP);
    rel(D);
    rel(SPACE);
    adv(STEP);
    expect_idle();
    tbl_begin(TERM, KM_TAP_POLICY_PERMISSIVE, 2000);
    tbl_set(0, SPACE, KM_CODE_TAP_HOLD, U_SPACE, U_B);
    rig_start(1000);
    press(SPACE);
    adv(3);
    press(D);
    adv(3);
    rel(D);                                                  /* pressed and released inside the tap-hold key: a hold */
    adv(60);
    CHECK(has(U_B));
    CHECK_EQ(appearances(U_D), 1);
    rel(SPACE);
    adv(STEP);
    expect_idle();
    /* an armed one-shot modifier goes with the hold key like with any ordinary key, and ends with it; with a modifier as the
       hold action it is left alone (as before) */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, SPACE, KM_CODE_TAP_HOLD, U_SPACE, U_B);
    tbl_set(0, F, KM_CODE_ONESHOT_MOD, U_LSHIFT, 0);
    tbl_set(0, E, KM_CODE_TAP_HOLD, U_SPACE, U_LCTRL);
    rig_start(1000);
    press(F);
    adv(40);
    rel(F);
    CHECK_EQ(g_out.armed, M_LSHIFT);
    press(SPACE);
    adv(TERM + 5);
    OUT_IS(U_B, U_LSHIFT);
    CHECK_EQ(g_out.armed, 0);
    rel(SPACE);
    OUT_NONE();
    press(F);
    adv(40);
    rel(F);
    press(E);
    adv(TERM + 5 + STEP);                                    /* (a decided modifier hold paces what follows it by STEP) */
    OUT_IS(U_LCTRL, U_LSHIFT);                               /* the armed Shift is sent anyway, and still armed */
    CHECK_EQ(g_out.armed, M_LSHIFT);
    press(A);
    OUT_IS(U_LCTRL, U_LSHIFT, U_A);
    rel(A);
    rel(E);
    adv(STEP);
    expect_idle();
}

/* ---------------------------------------------------------------- the queued-release bug (found by task P0) */

static void test_release_behind_queue(void)
{
    /* a key that is pressed, let go and pressed again while its first press still waits behind an undecided tap-hold key,
       and is then released as soon as the first press is handled: its second release must wait in the queue behind the
       second press, or the second press is left without a release (the key stays down until a reset) */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, SPACE, KM_CODE_TAP_HOLD, U_SPACE, KM_CODE_HOLD_L1);
    rig_start(1000);
    press(SPACE);
    adv(10);
    press(A);
    adv(10);
    rel(A);
    adv(10);
    press(A);
    adv(10);
    rel(SPACE);                                              /* a tap: the queue is released ... */
    unsigned guard = 0;
    while (!has(U_A) && guard++ < 200u) {
        adv(1);
    }
    CHECK(has(U_A));
    rel(A);                                                  /* ... and the second release comes while the first press is shown */
    adv(500);
    expect_idle();
    CHECK_EQ(appearances(U_A), 2);
    CHECK(!is_locked(A));
    CHECK(!is_locked(SPACE));
}

/* ---------------------------------------------------------------- randomised runs with a model */

static uint32_t g_rs;
static uint32_t rnd(void)
{
    g_rs = g_rs * 1664525u + 1013904223u;
    return g_rs >> 8;
}

enum { R_PLAIN, R_TAPHOLD, R_LAYER_TOGGLE, R_OSM, R_LAYER_HOLD, R_TOGGLE_KEY, R_MORPH, R_MODIFIER, R_ROLES };

#define NK 12u
typedef struct {
    unsigned slot;
    int      role;
    uint8_t  use[KM_LAYERS];     /* plain: the usage on layers 0..3 */
    uint8_t  a0, a1;             /* the layer-0 entry's arguments */
    uint8_t  poss[32];           /* bitmap of the usages this key can cause */
    bool     raw;
    uint32_t t_release;
    bool     ever;
} fkey_t;

static fkey_t   fk[NK];
static uint8_t  f_osm_mods[32];   /* bitmap: modifiers a one-shot key of this table can arm */
static bool     f_have_osm;
static uint8_t  f_tk_on[256];     /* model: toggle key usages that must be on */
static uint8_t  f_layer_par;      /* model: layers toggled on (bit l) */
static unsigned g_random_failures;

#define RCHECK(cond, seed, what)                                                         \
    do {                                                                                 \
        km_test_checks++;                                                                \
        if (!(cond)) {                                                                   \
            km_test_failures++;                                                          \
            if (g_random_failures++ < 20) {                                              \
                printf("  FAIL random seed %u: %s (%s) t=%u\n", (unsigned)(seed), what, #cond, (unsigned)(g_now - g_start)); \
            }                                                                            \
        }                                                                                \
    } while (0)

static void bm_set(uint8_t *bm, unsigned u)
{
    bm[u >> 3] = (uint8_t)(bm[u >> 3] | (1u << (u & 7u)));
}

static bool bm_get(const uint8_t *bm, unsigned u)
{
    return (bm[u >> 3] & (1u << (u & 7u))) != 0;
}

static void f_model_clear(void)
{
    memset(f_tk_on, 0, sizeof f_tk_on);
    f_layer_par = 0;
}

static void f_morph_random(void)
{
    km_morph_rec_t r[KM_MORPH_MAX];
    for (unsigned i = 0; i < KM_MORPH_MAX; i++) {
        r[i].mod_mask = (uint8_t)(1u + rnd() % 255u);
        r[i].usage = (uint8_t)(0x04u + rnd() % 26u);
        r[i].flags = (uint8_t)(rnd() % 2u);
        r[i].rsvd = 0;
    }
    km_layers_set_morph(r, rnd() % (KM_MORPH_MAX + 1u));
    memcpy(g_rec, r, sizeof r);
}

static void f_config(void)
{
    static const unsigned slots[NK] = { A, S, D, F, Q, W, E, R, I, SPACE, LSHIFT, RSHIFT };
    static const uint8_t tk_usages[] = { U_Z, U_X, U_LSHIFT, U_LALT };
    memset(f_osm_mods, 0, sizeof f_osm_mods);
    f_have_osm = false;
    tbl_begin(40u + rnd() % 300u, rnd() % 3u, 100u + rnd() % 300u);
    f_morph_random();
    for (unsigned i = 0; i < NK; i++) {
        fkey_t *k = &fk[i];
        memset(k, 0, sizeof *k);
        k->slot = slots[i];
        k->role = (int)(rnd() % R_ROLES);
        if (i < 6) {
            static const int forced[6] = { R_PLAIN, R_TAPHOLD, R_TOGGLE_KEY, R_MORPH, R_MODIFIER, R_TOGGLE_KEY };
            k->role = forced[i];
        }
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            tbl_set(l, k->slot, l == 0 ? (uint8_t)0 : (uint8_t)KM_CODE_TRANSPARENT, 0, 0);
        }
        switch (k->role) {
        case R_PLAIN:
            for (unsigned l = 0; l < KM_LAYERS; l++) {
                k->use[l] = (uint8_t)(l == 0 ? 0x04u + i : 0x3Au + i * 3u + l);
                tbl_set(l, k->slot, k->use[l], 0, 0);
                bm_set(k->poss, k->use[l]);
            }
            break;
        case R_TAPHOLD: {
            static const uint8_t holds[] = { KM_CODE_HOLD_L1, KM_CODE_HOLD_L2, KM_CODE_HOLD_L3, 0xE0, 0xE1, 0xE5, 0x30 };
            k->a0 = (uint8_t)(0x20u + i);
            k->a1 = holds[rnd() % 7u];
            tbl_set(0, k->slot, KM_CODE_TAP_HOLD, k->a0, k->a1);
            bm_set(k->poss, k->a0);
            if (k->a1 >= 0x04 && k->a1 <= 0xE7) {
                bm_set(k->poss, k->a1);
            }
            break;
        }
        case R_LAYER_TOGGLE:
            k->a0 = (uint8_t)(1u + rnd() % 3u);
            tbl_set(0, k->slot, KM_CODE_TOGGLE, k->a0, 0);
            break;
        case R_OSM:
            k->a0 = (uint8_t)(0xE0u + rnd() % 8u);
            tbl_set(0, k->slot, KM_CODE_ONESHOT_MOD, k->a0, 0);
            bm_set(k->poss, k->a0);
            bm_set(f_osm_mods, k->a0);
            f_have_osm = true;
            break;
        case R_LAYER_HOLD: {
            static const uint8_t lh[] = { KM_CODE_HOLD_L1, KM_CODE_HOLD_L2, KM_CODE_HOLD_L3 };
            tbl_set(0, k->slot, lh[rnd() % 3u], 0, 0);
            break;
        }
        case R_TOGGLE_KEY:
            k->a0 = tk_usages[rnd() % 4u];
            tbl_set(0, k->slot, KM_CODE_TOGGLE_KEY, k->a0, 0);
            break;
        case R_MORPH:
            k->a0 = (uint8_t)(0x2Au + (i & 3u));
            k->a1 = (uint8_t)(rnd() % KM_MORPH_MAX);
            tbl_set(0, k->slot, KM_CODE_MOD_MORPH, k->a0, k->a1);
            bm_set(k->poss, k->a0);
            for (unsigned r = 0; r < KM_MORPH_MAX; r++) {
                bm_set(k->poss, g_rec[r].usage);            /* any record may come and go (set_morph is called at random) */
            }
            break;
        default:   /* R_MODIFIER */
            k->a0 = (uint8_t)(0xE0u + rnd() % 8u);
            tbl_set(0, k->slot, k->a0, 0, 0);
            bm_set(k->poss, k->a0);
            break;
        }
    }
    tbl_load();
}

static void f_set_raw(fkey_t *k, bool down)
{
    if (down == k->raw) {
        return;
    }
    k->raw = down;
    if (down) {
        k->ever = true;
        if (k->role == R_TOGGLE_KEY) {
            f_tk_on[k->a0] ^= 1u;
        } else if (k->role == R_LAYER_TOGGLE) {
            f_layer_par ^= (uint8_t)(1u << k->a0);
        }
        press(k->slot);
    } else {
        k->t_release = g_now;
        rel(k->slot);
    }
}

static unsigned f_down_count(void)
{
    unsigned c = 0;
    for (unsigned i = 0; i < NK; i++) {
        c += fk[i].raw ? 1u : 0u;
    }
    return c;
}

/* the checks that hold at every instant */
static void f_sample(uint32_t seed)
{
    bool seen[256] = { false };
    RCHECK(g_out.kbd_n <= KM_KEYMAP_SLOTS, seed, "too many usages");
    const bool any_down = f_down_count() != 0;
    for (unsigned i = 0; i < g_out.kbd_n; i++) {
        const unsigned u = g_out.kbd[i];
        RCHECK(u >= 0x04u && u <= 0xE7u, seed, "usage out of range");
        RCHECK(!seen[u], seed, "duplicate usage");
        seen[u] = true;
        if (u >= 0xE0u) {
            RCHECK((g_out.suppressed & (1u << (u - 0xE0u))) == 0, seed, "a suppressed modifier is reported");
        }
        /* every reported usage has a possible cause: a toggle key of the table, or a key that is down or was let go in the last
           1.5 s and can produce it, or (a modifier) a one-shot modifier that can be carried by whatever key is down */
        bool explained = false;
        for (unsigned j = 0; j < NK && !explained; j++) {
            if (fk[j].role == R_TOGGLE_KEY && fk[j].a0 == u) {
                explained = true;
            } else if (fk[j].ever && bm_get(fk[j].poss, u) && (fk[j].raw || g_now - fk[j].t_release <= 1500u)) {
                explained = true;
            }
        }
        if (!explained && f_have_osm && bm_get(f_osm_mods, u)) {
            /* an armed one-shot modifier waits up to oneshot_ms for the next key, and rides on any key that is down */
            explained = any_down;
            for (unsigned j = 0; j < NK; j++) {
                if (fk[j].ever && !fk[j].raw && g_now - fk[j].t_release <= 1500u) {
                    explained = true;
                }
            }
        }
        RCHECK(explained, seed, "a usage is reported that nothing can have caused");
    }
}

static void f_step(uint32_t seed)
{
    adv(1);
    f_sample(seed);
}

/* everything is released and the pipeline reset, like the fail-safe of the main loop */
static void f_failsafe(void)
{
    for (unsigned i = 0; i < NK; i++) {
        if (fk[i].raw) {
            fk[i].raw = false;
            fk[i].t_release = g_now;
        }
    }
    km_layers_reset();
    g_n = 0;
    f_model_clear();
    upd();
}

static void f_run(uint32_t seed, bool exact)
{
    g_rs = seed * 2654435761u + 12345u;
    f_model_clear();
    f_config();
    uint32_t t0 = rnd();
    if ((seed & 3u) == 0) {
        t0 = 0xFFFFFFFFu - rnd() % 3000u;                    /* the millisecond counter wraps during the run */
    }
    g_n = 0;
    km_layers_reset();
    g_now = t0;
    g_start = t0;
    g_trn = 0;
    upd();
    uint32_t last_event = g_now;
    const unsigned actions = 60u + rnd() % 100u;
    for (unsigned a = 0; a < actions; a++) {
        const unsigned dice = rnd() % 100u;
        if (dice < 3) {                                      /* fail-safe release */
            f_failsafe();
            last_event = g_now;
        } else if (dice < 6) {                               /* the engine reports nothing is down (no reset) */
            for (unsigned i = 0; i < NK; i++) {
                if (fk[i].raw) {
                    f_set_raw(&fk[i], false);
                }
            }
            last_event = g_now;
        } else if (dice < 8) {                               /* the main loop stalls for a while */
            g_now += rnd() % 900u;
            upd();
            f_sample(seed);
        } else if (dice < 10) {                              /* another set of Mod-Morph records (a profile switch loads these) */
            f_morph_random();
            for (unsigned i = 0; i < NK; i++) {
                if (fk[i].role == R_MORPH) {
                    for (unsigned r = 0; r < KM_MORPH_MAX; r++) {
                        bm_set(fk[i].poss, g_rec[r].usage);
                    }
                }
            }
        } else if (dice < 13 && (!exact || (f_down_count() == 0 && g_now - last_event > 1500u))) {
            tbl_load();                                      /* the table is loaded again: toggled layers and usages end */
            f_model_clear();
            if (!exact) {
                last_event = g_now;
            }
        } else if (dice < 18) {                              /* a burst of keys at the same millisecond */
            const unsigned cnt = 4u + rnd() % 8u;
            for (unsigned c = 0; c < cnt; c++) {
                fkey_t *k = &fk[rnd() % NK];
                f_set_raw(k, !k->raw);
            }
            f_sample(seed);
            last_event = g_now;
        } else {
            fkey_t *k = &fk[rnd() % NK];
            f_set_raw(k, !k->raw);
            f_sample(seed);
            last_event = g_now;
        }
        const unsigned gap = (rnd() % 4u == 0) ? rnd() % 3u : 1u + rnd() % 250u;
        for (unsigned ms = 0; ms < gap; ms++) {
            f_step(seed);
        }
    }
    for (unsigned i = 0; i < NK; i++) {                      /* let go of everything */
        if (fk[i].raw) {
            f_set_raw(&fk[i], false);
            f_sample(seed);
        }
    }
    for (unsigned ms = 0; ms < 4000; ms++) {
        f_step(seed);
    }
    /* nothing is left over: no key, no undecided tap-hold key, no armed or suppressed modifier, no locked key */
    RCHECK(!g_out.pending, seed, "a tap-hold key is still undecided");
    RCHECK(g_out.armed == 0, seed, "an armed one-shot modifier did not expire");
    RCHECK(g_out.suppressed == 0, seed, "a modifier is still suppressed");
    RCHECK(g_out.consumer == 0, seed, "consumer still reported");
    for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
        RCHECK(!is_locked(s), seed, "a released key is still locked");
    }
    if (exact) {
        unsigned want_n = 0;
        for (unsigned u = 0; u < 256u; u++) {
            if (f_tk_on[u] != 0) {
                want_n++;
                RCHECK(has((uint8_t)u), seed, "a toggle key that is on is not reported");
            }
        }
        RCHECK(g_out.kbd_n == want_n, seed, "a usage other than the toggle keys is still reported");
        RCHECK(g_out.toggle_keys == want_n, seed, "toggle key count is wrong (a press was lost or doubled)");
        RCHECK(g_out.toggled == f_layer_par, seed, "toggle layer parity is wrong");
    } else {
        for (unsigned i = 0; i < g_out.kbd_n; i++) {          /* whatever is on must be a toggle key of the table */
            bool tk = false;
            for (unsigned j = 0; j < NK; j++) {
                tk |= fk[j].role == R_TOGGLE_KEY && fk[j].a0 == g_out.kbd[i];
            }
            RCHECK(tk, seed, "a usage other than the toggle keys is still reported");
        }
    }
    unsigned top = 0;
    for (unsigned l = 1; l < KM_LAYERS; l++) {
        if ((g_out.toggled & (1u << l)) != 0) {
            top = l;
        }
    }
    RCHECK(g_out.layer == top, seed, "a held layer is stuck");
    /* the way out of a toggled state always exists: loading a table (profile switch) ends every toggle */
    tbl_load();
    adv(2);
    RCHECK(g_out.kbd_n == 0 && g_out.toggle_keys == 0 && g_out.toggled == 0 && g_out.layer == 0, seed, "a table load does not clear the toggles");
}

static void test_random_exact(void)
{
    for (uint32_t seed = 1; seed <= 500; seed++) {
        f_run(seed, true);
    }
}

static void test_random_faults(void)
{
    for (uint32_t seed = 1001; seed <= 1500; seed++) {
        f_run(seed, false);
    }
}

void test_layers_m6(void)
{
    test_toggle_key_basic();
    test_toggle_key_sharing();
    test_toggle_key_modifier();
    test_toggle_key_cap();
    test_toggle_key_ends();
    test_toggle_key_with_osm();
    test_toggle_key_with_tap_hold();
    test_toggle_key_on_layer();
    test_morph_basic();
    test_morph_lock();
    test_morph_suppress();
    test_morph_sources_of_modifiers();
    test_morph_edges();
    test_morph_more();
    test_morph_with_tap_hold();
    test_tap_hold_key_hold();
    test_release_behind_queue();
    test_random_exact();
    test_random_faults();
    km_layers_set_morph(NULL, 0);
    km_layers_set_map(NULL);
    km_layers_reset();
}
