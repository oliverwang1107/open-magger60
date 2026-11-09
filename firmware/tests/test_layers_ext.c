/*
 * test_layers_ext.c - task L4: toggle layers, tap-hold keys, one-shot modifiers (km_layers with a clock).
 *
 * The rig feeds km_layers_update_ms() once per simulated millisecond (like the main loop does) and records every change of
 * the output with its time, so a test can say "the space bar was visible from 60 ms to 70 ms, then the A from 70 to 80".
 * Expected values come from the rules written in km_layers.h, not from the code. The last test is a randomised run of
 * keys, layers and tap-hold / one-shot / toggle keys that checks the properties that matter on a real keyboard: no key is
 * stuck after everything is released, no press is lost, toggle parity is exact.
 */
#include <string.h>

#include "km/km_keymap.h"
#include "km/km_layers.h"
#include "km_test.h"

enum { A = 73, S = 72, D = 74, F = 49, Q = 2, W = 75, E = 6, R = 42, I = 1, SPACE = 63, LSHIFT = 0, RSHIFT = 28, LCTRL = 5, ESC = 18, FN = 14 };
#define U_A 0x04
#define U_I 0x0C
#define U_UP 0x52
#define U_SPACE 0x2C
#define U_ESC 0x29
#define U_LSHIFT 0xE1
#define U_LCTRL 0xE0
#define U_RSHIFT 0xE5
#define STEP KM_LAYERS_STEP_MS
#define TERM 200

/* ---------------------------------------------------------------- the rig */

typedef struct {
    uint32_t t;
    uint8_t  kbd[12];
    uint8_t  n;
    uint16_t consumer;
} frame_t;

static uint32_t g_now, g_start;
static uint8_t  g_down[KM_KEYMAP_SLOTS];
static unsigned g_n;
static km_layers_out_t g_out;
static frame_t  g_tr[4096];
static unsigned g_trn;

static uint8_t g_code[KM_LAYERS][KM_KEYMAP_SLOTS];
static uint8_t g_arg[KM_LAYERS][KM_KEYMAP_SLOTS][KM_KEYMAP_ARGS];
static km_layer_opts_t g_opts;

static void record(void)
{
    frame_t f;
    memset(&f, 0, sizeof f);
    f.t = g_now - g_start;
    f.n = g_out.kbd_n < sizeof f.kbd ? g_out.kbd_n : (uint8_t)sizeof f.kbd;
    memcpy(f.kbd, g_out.kbd, f.n);
    f.consumer = g_out.consumer;
    if (g_trn > 0 && g_tr[g_trn - 1].n == f.n && g_tr[g_trn - 1].consumer == f.consumer &&
        memcmp(g_tr[g_trn - 1].kbd, f.kbd, f.n) == 0) {
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

/* default table + options; the caller adds entries and calls tbl_load() */
static void tbl_begin(unsigned term, unsigned policy, unsigned oneshot)
{
    km_keymap_default_table(g_code);
    memset(g_arg, 0, sizeof g_arg);
    g_opts.tap_term_ms = (uint16_t)term;
    g_opts.tap_policy = (uint8_t)policy;
    g_opts.oneshot_ms = (uint16_t)oneshot;
}

static void rig_start(uint32_t t0)
{
    km_layers_reset();
    g_n = 0;
    g_trn = 0;
    g_now = g_start = t0;
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

static bool frame_has(const frame_t *f, uint8_t u)
{
    for (unsigned i = 0; i < f->n; i++) {
        if (f->kbd[i] == u) {
            return true;
        }
    }
    return false;
}

/* time the usage first appeared (relative to the start), -1 = never */
static int first_time(uint8_t u)
{
    for (unsigned i = 0; i < g_trn; i++) {
        if (frame_has(&g_tr[i], u)) {
            return (int)g_tr[i].t;
        }
    }
    return -1;
}

static unsigned appearances(uint8_t u)
{
    unsigned c = 0;
    bool prev = false;
    for (unsigned i = 0; i < g_trn; i++) {
        const bool h = frame_has(&g_tr[i], u);
        c += (h && !prev);
        prev = h;
    }
    return c;
}

/* how long the usage stayed visible the first time it appeared, -1 = never appeared or still visible */
static int visible_for(uint8_t u)
{
    for (unsigned i = 0; i < g_trn; i++) {
        if (frame_has(&g_tr[i], u)) {
            for (unsigned j = i + 1u; j < g_trn; j++) {
                if (!frame_has(&g_tr[j], u)) {
                    return (int)(g_tr[j].t - g_tr[i].t);
                }
            }
            return -1;
        }
    }
    return -1;
}

static unsigned consumer_appearances(uint16_t c)
{
    unsigned k = 0;
    uint16_t prev = 0;
    for (unsigned i = 0; i < g_trn; i++) {
        k += (g_tr[i].consumer == c && prev != c);
        prev = g_tr[i].consumer;
    }
    return k;
}

static void expect_idle(void)
{
    CHECK_EQ(g_out.kbd_n, 0);
    CHECK_EQ(g_out.consumer, 0);
    CHECK(!g_out.pending);
    CHECK_EQ(g_out.armed, 0);
}

/* ---------------------------------------------------------------- tables with arguments */

static void test_tables_v2(void)
{
    /* the entry rule */
    CHECK(km_keymap_entry_valid(KM_CODE_TOGGLE, 1, 0) && km_keymap_entry_valid(KM_CODE_TOGGLE, 3, 0));
    CHECK(!km_keymap_entry_valid(KM_CODE_TOGGLE, 0, 0) && !km_keymap_entry_valid(KM_CODE_TOGGLE, 4, 0) && !km_keymap_entry_valid(KM_CODE_TOGGLE, 1, 1));
    CHECK(km_keymap_entry_valid(KM_CODE_TAP_HOLD, U_SPACE, KM_CODE_HOLD_L1) && km_keymap_entry_valid(KM_CODE_TAP_HOLD, KM_CODE_MUTE, 0xE7));
    CHECK(!km_keymap_entry_valid(KM_CODE_TAP_HOLD, 0, KM_CODE_HOLD_L1) && !km_keymap_entry_valid(KM_CODE_TAP_HOLD, U_SPACE, 0));
    CHECK(!km_keymap_entry_valid(KM_CODE_TAP_HOLD, KM_CODE_HOLD_L1, 0xE0) && !km_keymap_entry_valid(KM_CODE_TAP_HOLD, U_SPACE, U_SPACE));
    CHECK(km_keymap_entry_valid(KM_CODE_ONESHOT_MOD, 0xE0, 0) && !km_keymap_entry_valid(KM_CODE_ONESHOT_MOD, 0x04, 0));
    CHECK(km_keymap_entry_valid(0x04, 0, 0) && !km_keymap_entry_valid(0x04, 1, 0) && !km_keymap_entry_valid(0xEB, 0, 0));
    CHECK(km_keymap_entry_valid(KM_CODE_TRANSPARENT, 0, 0) && !km_keymap_entry_valid(KM_CODE_TRANSPARENT, 0, 1));
    CHECK(!km_keymap_table_valid((const uint8_t (*)[KM_KEYMAP_SLOTS])(const uint8_t[KM_LAYERS][KM_KEYMAP_SLOTS]){ { KM_CODE_TOGGLE } }));

    /* a loaded v2 table: codes, arguments and options come back; the usage view hides the extended codes */
    tbl_begin(150, KM_TAP_POLICY_PERMISSIVE, 777);
    tbl_set(0, A, KM_CODE_TAP_HOLD, U_SPACE, KM_CODE_HOLD_L2);
    tbl_set(1, A, KM_CODE_TOGGLE, 3, 0);
    tbl_load();
    CHECK_EQ(km_keymap_code(0, A), KM_CODE_TAP_HOLD);
    CHECK_EQ(km_keymap_arg(0, A, 0), U_SPACE);
    CHECK_EQ(km_keymap_arg(0, A, 1), KM_CODE_HOLD_L2);
    CHECK_EQ(km_keymap_arg(1, A, 0), 3);
    CHECK_EQ(km_keymap_arg(0, A, 2), 0);                     /* no third argument */
    CHECK_EQ(km_keymap_arg(KM_LAYERS, A, 0), 0);
    CHECK_EQ(km_keymap_arg(0, KM_KEYMAP_SLOTS, 0), 0);
    CHECK_EQ(km_keymap_usage(0, A), 0);
    CHECK_EQ(km_keymap_usage(1, A), 0);
    CHECK_EQ(km_keymap_opts()->tap_term_ms, 150);
    CHECK_EQ(km_keymap_opts()->tap_policy, KM_TAP_POLICY_PERMISSIVE);
    CHECK_EQ(km_keymap_opts()->oneshot_ms, 777);
    const uint32_t gen = km_keymap_generation();

    /* refused tables / options restore the built-in table and the default options (and count as a load) */
    tbl_set(1, A, KM_CODE_TOGGLE, 9, 0);
    CHECK(!km_layers_set_table(g_code, g_arg, &g_opts));
    CHECK(km_keymap_generation() != gen);
    CHECK_EQ(km_keymap_code(0, A), 0x04);
    CHECK_EQ(km_keymap_arg(0, A, 0), 0);
    CHECK_EQ(km_keymap_opts()->tap_term_ms, KM_LAYER_TAP_TERM_DEFAULT);
    tbl_set(1, A, KM_CODE_TOGGLE, 3, 0);
    g_opts.tap_term_ms = 29;
    CHECK(!km_layers_set_table(g_code, g_arg, &g_opts));
    g_opts.tap_term_ms = 200;
    g_opts.tap_policy = 3;
    CHECK(!km_layers_set_table(g_code, g_arg, &g_opts));
    g_opts.tap_policy = 0;
    g_opts.oneshot_ms = 99;
    CHECK(!km_layers_set_table(g_code, g_arg, &g_opts));
    g_opts.oneshot_ms = 0;
    CHECK(km_layers_set_table(g_code, g_arg, &g_opts));
    /* arguments or options may be left out */
    tbl_begin(TERM, 0, 1000);
    CHECK(km_layers_set_table(g_code, NULL, NULL));
    CHECK_EQ(km_keymap_opts()->tap_term_ms, KM_LAYER_TAP_TERM_DEFAULT);
    tbl_set(0, A, KM_CODE_TOGGLE, 1, 0);
    CHECK(!km_layers_set_table(g_code, NULL, &g_opts));      /* an extended code without arguments is refused */
    CHECK(km_layers_set_table(NULL, NULL, NULL));            /* NULL = the built-in table */
    CHECK_EQ(km_keymap_code(0, A), 0x04);
    const km_layer_opts_t *o = km_keymap_opts();
    CHECK(o->tap_term_ms == KM_LAYER_TAP_TERM_DEFAULT && o->oneshot_ms == KM_LAYER_ONESHOT_DEFAULT && o->tap_policy == KM_TAP_POLICY_TIMEOUT);
    km_layer_opts_t d;
    km_layer_opts_default(&d);
    CHECK(km_layer_opts_valid(&d) && !km_layer_opts_valid(NULL));
}

/* ---------------------------------------------------------------- toggle layers */

static void test_toggle(void)
{
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 1000);
    tbl_set(0, ESC, KM_CODE_TOGGLE, 2, 0);
    tbl_set(2, ESC, KM_CODE_TOGGLE, 2, 0);          /* the way back, on the layer itself */
    tbl_set(2, A, 0x05, 0, 0);                      /* layer 2: A -> B */
    tbl_load();
    rig_start(1000);

    /* press and release: the layer comes on and stays */
    press(ESC);
    CHECK_EQ(g_out.layer, 2);                       /* on already while the key is down */
    CHECK_EQ(g_out.toggled, 4);
    adv(30);
    rel(ESC);
    CHECK_EQ(g_out.layer, 2);
    CHECK_EQ(g_out.toggled, 4);
    expect_idle();
    adv(500);
    CHECK_EQ(g_out.layer, 2);                       /* no timer ends it */
    press(A);
    CHECK(has(0x05) && !has(U_A));
    adv(20);
    rel(A);
    CHECK_EQ(g_out.kbd_n, 0);

    /* press again: off */
    press(ESC);
    rel(ESC);
    CHECK_EQ(g_out.layer, 0);
    CHECK_EQ(g_out.toggled, 0);
    press(A);
    CHECK(has(U_A) && !has(0x05));
    rel(A);

    /* latching: a key pressed on the toggled layer keeps its code when the layer is toggled off under it */
    press(ESC); rel(ESC);                           /* on */
    adv(20);
    press(A);
    CHECK(has(0x05));
    adv(20);
    press(ESC); rel(ESC);                           /* off, A still down */
    CHECK_EQ(g_out.layer, 0);
    CHECK(has(0x05) && !has(U_A));                  /* still B: the release must match the press */
    adv(20);
    rel(A);
    CHECK_EQ(g_out.kbd_n, 0);
    press(A);                                       /* a new press uses layer 0 again */
    CHECK(has(U_A));
    rel(A);

    /* a key pressed on layer 0 keeps layer 0's code when the layer comes on under it */
    press(A);
    adv(10);
    press(ESC); rel(ESC);                           /* on, A still down */
    CHECK_EQ(g_out.layer, 2);
    CHECK(has(U_A) && !has(0x05));
    rel(A);
    CHECK_EQ(g_out.kbd_n, 0);
    press(ESC); rel(ESC);                           /* off */
    CHECK_EQ(g_out.toggled, 0);

    /* holding the toggle key does not change anything but the first press */
    press(ESC);
    adv(500);
    CHECK_EQ(g_out.toggled, 4);
    press(A);
    CHECK(has(0x05));
    rel(A);
    rel(ESC);
    CHECK_EQ(g_out.toggled, 4);
    press(ESC); rel(ESC);                           /* off */

    /* a loaded table (= a profile switch) ends a toggled layer; so does a reset */
    press(ESC); rel(ESC);
    CHECK_EQ(g_out.toggled, 4);
    tbl_load();
    adv(1);
    CHECK_EQ(g_out.toggled, 0);
    CHECK_EQ(g_out.layer, 0);
    press(ESC); rel(ESC);
    CHECK_EQ(g_out.toggled, 4);
    km_layers_reset();
    g_n = 0;
    upd();
    CHECK_EQ(g_out.toggled, 0);
    CHECK_EQ(g_out.layer, 0);
    km_layers_set_map(NULL);                        /* even loading the built-in table counts */
    press(FN);                                      /* (FN holds layer 1 in the built-in table) */
    rel(FN);
    CHECK_EQ(g_out.toggled, 0);
}

static void test_toggle_and_holds(void)
{
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 1000);
    tbl_set(0, ESC, KM_CODE_TOGGLE, 1, 0);          /* toggles layer 1 (the default FN layer: I = Up) */
    tbl_set(0, Q, KM_CODE_TOGGLE, 2, 0);
    tbl_set(0, W, KM_CODE_HOLD_L3, 0, 0);
    tbl_set(2, I, 0x3B, 0, 0);                      /* layer 2: I -> F2 */
    tbl_set(3, I, 0x3C, 0, 0);                      /* layer 3: I -> F3 */
    tbl_load();
    rig_start(0);

    press(ESC); rel(ESC);                           /* layer 1 toggled on */
    press(I);
    CHECK(has(U_UP));
    rel(I);

    /* hold FN (layer 1) and let go: the toggled layer stays on */
    press(FN);
    adv(20);
    CHECK_EQ(g_out.layer, 1);
    rel(FN);
    CHECK_EQ(g_out.layer, 1);
    CHECK_EQ(g_out.toggled, 2);
    press(I);
    CHECK(has(U_UP));
    rel(I);

    /* a higher toggled layer wins over a lower held one; releasing the hold changes nothing */
    press(Q); rel(Q);                               /* layer 2 on as well */
    CHECK_EQ(g_out.toggled, 6);
    CHECK_EQ(g_out.layer, 2);
    press(FN);
    CHECK_EQ(g_out.layer, 2);
    press(I);
    CHECK(has(0x3B));
    rel(I);
    rel(FN);
    CHECK_EQ(g_out.layer, 2);

    /* a held layer above the toggled ones wins while held, and falls back to the toggled one */
    press(W);
    CHECK_EQ(g_out.layer, 3);
    press(I);
    CHECK(has(0x3C));
    rel(I);
    rel(W);
    CHECK_EQ(g_out.layer, 2);

    /* toggling a layer that is currently held: it stays on after the hold ends */
    press(FN);                                      /* hold layer 1 ... */
    press(ESC); rel(ESC);                           /* ... the toggle key sits on layer 0 / transparent: layer 1 toggles OFF (it was on) */
    CHECK_EQ(g_out.toggled, 4);
    CHECK_EQ(g_out.layer, 2);
    press(ESC); rel(ESC);                           /* on again while FN is still held */
    CHECK_EQ(g_out.toggled, 6);
    rel(FN);
    CHECK_EQ(g_out.layer, 2);
    CHECK_EQ(g_out.toggled, 6);

    /* everything off */
    press(ESC); rel(ESC);
    press(Q); rel(Q);
    CHECK_EQ(g_out.toggled, 0);
    CHECK_EQ(g_out.layer, 0);
    expect_idle();
}

/* ---------------------------------------------------------------- tap-hold */

static void tbl_taphold_basic(unsigned term, unsigned policy)
{
    tbl_begin(term, policy, 1000);
    tbl_set(0, SPACE, KM_CODE_TAP_HOLD, U_SPACE, KM_CODE_HOLD_L1);    /* layer-tap */
    tbl_set(0, LCTRL, KM_CODE_TAP_HOLD, U_ESC, U_LCTRL);              /* mod-tap */
    tbl_load();
}

static void test_tap(void)
{
    tbl_taphold_basic(TERM, KM_TAP_POLICY_TIMEOUT);
    rig_start(5000);
    press(SPACE);
    CHECK(g_out.pending);
    CHECK_EQ(g_out.kbd_n, 0);                       /* nothing is sent before the decision */
    adv(50);
    CHECK_EQ(g_out.kbd_n, 0);
    rel(SPACE);                                     /* t = 50: a tap */
    CHECK(has(U_SPACE));
    CHECK(!g_out.pending);
    CHECK_EQ(g_out.layer, 0);
    adv(STEP - 1);
    CHECK(has(U_SPACE));                            /* visible for STEP ms ... */
    adv(1);
    CHECK(!has(U_SPACE));                           /* ... then released */
    adv(500);
    expect_idle();
    CHECK_EQ(first_time(U_SPACE), 50);
    CHECK_EQ(visible_for(U_SPACE), (int)STEP);
    CHECK_EQ(appearances(U_SPACE), 1);

    /* the timeout boundary: released one ms before the term is a tap, at the term it is a hold (nothing visible) */
    rig_start(100);
    press(SPACE);
    adv(TERM - 1);
    rel(SPACE);
    CHECK_EQ(appearances(U_SPACE), 1);
    adv(100);
    rig_start(100);
    press(SPACE);
    adv(TERM);
    CHECK_EQ(g_out.layer, 1);                       /* decided: hold */
    rel(SPACE);
    adv(100);
    CHECK_EQ(appearances(U_SPACE), 0);
    CHECK_EQ(g_out.layer, 0);
    expect_idle();

    /* a media key as the tap action */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 1000);
    tbl_set(0, SPACE, KM_CODE_TAP_HOLD, KM_CODE_PLAY_PAUSE, KM_CODE_HOLD_L2);
    tbl_load();
    rig_start(0);
    press(SPACE);
    adv(30);
    rel(SPACE);
    CHECK_EQ(g_out.consumer, 0x00CD);
    adv(STEP);
    CHECK_EQ(g_out.consumer, 0);
    CHECK_EQ(consumer_appearances(0x00CD), 1);
    CHECK_EQ(g_out.kbd_n, 0);
}

static void test_hold(void)
{
    tbl_taphold_basic(TERM, KM_TAP_POLICY_TIMEOUT);
    rig_start(0);
    /* layer-tap held past the term: layer 1 for the keys pressed after it */
    press(SPACE);
    adv(TERM - 1);
    CHECK(g_out.pending);
    CHECK_EQ(g_out.layer, 0);
    adv(1);
    CHECK(!g_out.pending);
    CHECK_EQ(g_out.layer, 1);
    CHECK_EQ(g_out.kbd_n, 0);
    adv(50);
    press(I);
    CHECK(has(U_UP) && !has(U_I));
    adv(30);
    rel(I);
    adv(100);
    rel(SPACE);
    CHECK_EQ(g_out.layer, 0);
    expect_idle();
    CHECK_EQ(appearances(U_SPACE), 0);              /* a hold is never also a tap */

    /* mod-tap held past the term: the modifier is down while the key is down, nothing else is sent */
    rig_start(0);
    press(LCTRL);
    adv(TERM);
    CHECK(has(U_LCTRL));
    CHECK_EQ(g_out.kbd_n, 1);
    adv(100);
    press(A);
    adv(STEP);
    CHECK(has(U_LCTRL) && has(U_A));
    rel(A);
    CHECK(has(U_LCTRL));
    adv(20);
    rel(LCTRL);
    CHECK_EQ(g_out.kbd_n, 0);
    CHECK_EQ(appearances(U_ESC), 0);
}

static void test_long_gap_between_updates(void)
{
    /* the main loop was busy for longer than the term (a flash write): events are stamped when they are SEEN, and a release
       that is seen after the deadline never counts as a tap, even though nothing was decided in between */
    tbl_taphold_basic(TERM, KM_TAP_POLICY_TIMEOUT);
    rig_start(0);
    press(SPACE);
    g_now += TERM + 100;                            /* no update during this time */
    rel(SPACE);
    adv(100);
    CHECK_EQ(appearances(U_SPACE), 0);
    expect_idle();
    CHECK_EQ(g_out.layer, 0);

    /* and a key that was pressed during the gap is not reordered or lost: it follows the hold on layer 1 */
    rig_start(0);
    press(SPACE);
    g_now += 50;
    press(I);
    g_now += TERM;                                  /* both are older than the deadline when the loop runs again */
    rel(I);
    rel(SPACE);
    adv(3 * STEP);
    CHECK_EQ(appearances(U_UP) + appearances(U_I), 1);
    expect_idle();
}

static void test_other_key_timeout_policy(void)
{
    tbl_taphold_basic(TERM, KM_TAP_POLICY_TIMEOUT);

    /* another key is tapped inside a quick tap: the tap comes first, then the key (never reordered, never lost) */
    rig_start(0);
    press(SPACE);
    adv(20);
    press(A);
    adv(20);
    rel(A);
    CHECK_EQ(g_out.kbd_n, 0);                       /* A waits for the decision */
    adv(20);
    rel(SPACE);                                     /* t = 60 */
    CHECK(has(U_SPACE) && !has(U_A));
    adv(STEP);                                      /* t = 70: the tap ends, A starts */
    CHECK(!has(U_SPACE) && has(U_A));
    adv(STEP);                                      /* t = 80 */
    CHECK_EQ(g_out.kbd_n, 0);
    CHECK_EQ(first_time(U_SPACE), 60);
    CHECK_EQ(first_time(U_A), 70);
    CHECK_EQ(visible_for(U_SPACE), (int)STEP);
    CHECK_EQ(visible_for(U_A), (int)STEP);          /* a replayed press is visible for a full step, never lost */
    CHECK_EQ(g_out.layer, 0);
    expect_idle();

    /* the other key is still held when the tap-hold key is released: tap, then A (A stays down) */
    rig_start(0);
    press(SPACE);
    adv(20);
    press(A);
    adv(20);
    rel(SPACE);
    CHECK(has(U_SPACE) && !has(U_A));
    adv(STEP);
    CHECK(!has(U_SPACE) && has(U_A));
    adv(300);
    CHECK(has(U_A));
    rel(A);
    CHECK_EQ(g_out.kbd_n, 0);

    /* held to the term with another key pressed and released meanwhile: hold, and the key is replayed ON the layer */
    rig_start(0);
    press(SPACE);
    adv(20);
    press(I);
    adv(20);
    rel(I);
    adv(TERM - 40);                                 /* t = 200: the term ends */
    CHECK_EQ(g_out.layer, 1);
    CHECK(has(U_UP));                               /* I replayed as the layer-1 key (Up), not as I */
    adv(STEP);
    CHECK(!has(U_UP));
    CHECK_EQ(appearances(U_I), 0);
    CHECK_EQ(appearances(U_UP), 1);
    rel(SPACE);
    CHECK_EQ(g_out.layer, 0);
    adv(100);
    expect_idle();

    /* the other key is still down when the term ends: it takes the layer */
    rig_start(0);
    press(SPACE);
    adv(20);
    press(I);
    adv(TERM);
    CHECK(has(U_UP) && !has(U_I));
    rel(SPACE);                                     /* releasing the layer key first does not change I (latching) */
    CHECK(has(U_UP));
    adv(40);
    rel(I);
    CHECK_EQ(g_out.kbd_n, 0);

    /* a key that was already down is not held up, and keeps what it was pressed with */
    rig_start(0);
    press(A);
    adv(10);
    CHECK(has(U_A));
    press(SPACE);
    adv(10);
    rel(A);                                         /* released at once, although the decision is open */
    CHECK_EQ(g_out.kbd_n, 0);
    CHECK(g_out.pending);
    adv(10);
    rel(SPACE);
    CHECK(has(U_SPACE));
    adv(50);
    expect_idle();
}

static void test_other_key_policies(void)
{
    /* hold as soon as another key is pressed */
    tbl_taphold_basic(TERM, KM_TAP_POLICY_OTHER_KEY);
    rig_start(0);
    press(SPACE);
    adv(20);
    press(I);
    CHECK_EQ(g_out.layer, 1);
    CHECK(has(U_UP));                               /* at once, no waiting */
    adv(100);
    rel(I);
    rel(SPACE);
    expect_idle();
    CHECK_EQ(appearances(U_SPACE), 0);
    /* but a quick tap alone is still a tap */
    rig_start(0);
    press(SPACE);
    adv(40);
    rel(SPACE);
    CHECK(has(U_SPACE));
    adv(50);
    expect_idle();
    /* with a modifier hold, the modifier is seen on its own for a step before the key that triggered it */
    rig_start(0);
    press(LCTRL);
    adv(20);
    press(A);
    CHECK(has(U_LCTRL) && !has(U_A));
    adv(STEP);
    CHECK(has(U_LCTRL) && has(U_A));
    rel(A);
    rel(LCTRL);
    adv(20);
    expect_idle();

    /* permissive hold: another key pressed AND released inside the hold key makes it a hold */
    tbl_taphold_basic(TERM, KM_TAP_POLICY_PERMISSIVE);
    rig_start(0);
    press(SPACE);
    adv(20);
    press(I);
    adv(20);
    CHECK_EQ(g_out.kbd_n, 0);                       /* still waiting: I has not been released yet */
    rel(I);                                         /* t = 40 */
    CHECK_EQ(g_out.layer, 1);
    CHECK(has(U_UP));
    adv(STEP);
    CHECK(!has(U_UP));
    adv(100);
    rel(SPACE);
    expect_idle();
    CHECK_EQ(appearances(U_SPACE), 0);
    CHECK_EQ(appearances(U_I), 0);

    /* ... but if the tap-hold key is released first, it was a tap */
    rig_start(0);
    press(SPACE);
    adv(20);
    press(I);
    adv(40);
    rel(SPACE);                                     /* t = 60, I still down */
    CHECK(has(U_SPACE) && !has(U_I));
    adv(STEP);
    CHECK(has(U_I) && !has(U_SPACE));
    adv(100);
    rel(I);
    expect_idle();
    CHECK_EQ(appearances(U_UP), 0);
}

static void test_two_tap_holds(void)
{
    tbl_taphold_basic(TERM, KM_TAP_POLICY_TIMEOUT);

    /* both pending: the first decides first, the second keeps ITS OWN press time for its decision */
    rig_start(0);
    press(SPACE);
    adv(10);
    press(LCTRL);
    adv(40);
    rel(SPACE);                                     /* t = 50: space is a tap */
    CHECK(has(U_SPACE));
    adv(30);
    rel(LCTRL);                                     /* t = 80: LCTRL released 70 ms after ITS press: a tap too */
    adv(STEP);
    CHECK_EQ(appearances(U_SPACE), 1);
    CHECK_EQ(appearances(U_ESC), 1);
    CHECK(first_time(U_SPACE) < first_time(U_ESC));
    CHECK_EQ(visible_for(U_SPACE), (int)STEP);
    CHECK_EQ(visible_for(U_ESC), (int)STEP);
    adv(300);
    expect_idle();

    /* the second one is held past ITS term after the first one became a tap */
    rig_start(0);
    press(SPACE);
    adv(10);
    press(LCTRL);
    adv(40);
    rel(SPACE);
    adv(TERM);                                      /* t = 250: LCTRL has been down for 240 ms */
    CHECK(has(U_LCTRL));
    adv(20);
    rel(LCTRL);
    adv(100);
    expect_idle();
    CHECK_EQ(appearances(U_ESC), 0);
    CHECK_EQ(appearances(U_SPACE), 1);

    /* with hold-on-other-key: the second tap-hold key being pressed makes the first one a hold */
    tbl_taphold_basic(TERM, KM_TAP_POLICY_OTHER_KEY);
    rig_start(0);
    press(SPACE);
    adv(10);
    press(LCTRL);
    CHECK_EQ(g_out.layer, 1);                       /* SPACE is a hold now */
    adv(20);
    rel(LCTRL);                                     /* LCTRL was a tap (20 ms) */
    adv(STEP + 5);
    CHECK_EQ(appearances(U_ESC), 1);
    rel(SPACE);
    adv(100);
    expect_idle();
}

static void test_tap_hold_on_a_layer(void)
{
    /* the entry can live on a layer; transparent entries use the layer 0 entry (code and arguments) */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 1000);
    tbl_set(0, A, KM_CODE_TAP_HOLD, U_A, U_LSHIFT);
    tbl_set(1, S, KM_CODE_TAP_HOLD, 0x16, U_LCTRL);   /* on layer 1 only */
    tbl_load();
    rig_start(0);
    press(A);
    adv(TERM);
    CHECK(has(U_LSHIFT));
    rel(A);
    adv(50);
    press(S);                                       /* layer 0: plain S */
    CHECK(has(0x16));
    rel(S);
    press(FN);                                      /* layer 1 */
    press(S);
    CHECK(g_out.pending);
    adv(30);
    rel(S);
    CHECK(has(0x16));
    adv(STEP);
    rel(FN);
    adv(50);
    expect_idle();
    /* A on layer 1 is transparent: the layer 0 tap-hold entry is used, with its arguments */
    press(FN);
    press(A);
    adv(TERM);
    CHECK(has(U_LSHIFT));
    rel(A);
    rel(FN);
}

static void test_events_not_delayed_after_decision(void)
{
    /* nothing waits longer than it must: after a hold decision the queued keys flow at one step each */
    tbl_taphold_basic(TERM, KM_TAP_POLICY_TIMEOUT);
    rig_start(0);
    press(SPACE);
    adv(10);
    press(A);
    adv(5);
    rel(A);
    adv(5);
    press(S);
    adv(5);
    rel(S);
    adv(TERM);                                      /* hold decided at 200 */
    adv(5 * STEP);
    CHECK_EQ(appearances(U_A), 1);                  /* layer 1 has no entry for A and S: both come out as themselves, in order */
    CHECK_EQ(appearances(0x16), 1);
    CHECK(first_time(U_A) < first_time(0x16));
    CHECK_EQ(visible_for(U_A), (int)STEP);
    CHECK_EQ(visible_for(0x16), (int)STEP);
    rel(SPACE);
    adv(50);
    expect_idle();
}

static void test_queue_overflow(void)
{
    /* more events than the queue holds while a key is undecided: the key becomes a hold, nothing stays down */
    tbl_taphold_basic(1000, KM_TAP_POLICY_TIMEOUT);
    rig_start(0);
    press(SPACE);
    static const uint8_t keys[] = { A, S, D, F, Q, W, E, R, I, 1 /* I again below */ };
    unsigned pressed = 0;
    for (unsigned round = 0; round < 8; round++) {
        for (unsigned k = 0; k < 9; k++) {
            press(keys[k]);
            adv(1);
            rel(keys[k]);
            adv(1);
            pressed++;
        }
    }
    CHECK(pressed > KM_LAYERS_EVENTS / 2u + 5u);
    CHECK(!g_out.pending);                          /* decided by the overflow */
    rel(SPACE);
    adv(3000);
    expect_idle();
    CHECK_EQ(g_out.layer, 0);
}

/* ---------------------------------------------------------------- one-shot modifiers */

static void tbl_oneshot(unsigned oneshot)
{
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, oneshot);
    tbl_set(0, LSHIFT, KM_CODE_ONESHOT_MOD, U_LSHIFT, 0);
    tbl_set(0, LCTRL, KM_CODE_ONESHOT_MOD, U_LCTRL, 0);
    tbl_set(0, SPACE, KM_CODE_TAP_HOLD, U_SPACE, KM_CODE_HOLD_L2);
    tbl_load();
}

static void test_oneshot(void)
{
    tbl_oneshot(1000);

    /* tap, then a key: the modifier goes with that key only */
    rig_start(0);
    press(LSHIFT);
    CHECK(has(U_LSHIFT));                           /* sent at once */
    adv(30);
    rel(LSHIFT);
    CHECK(has(U_LSHIFT));                           /* ... and stays: armed */
    CHECK_EQ(g_out.armed, 1u << 1);
    adv(200);
    CHECK(has(U_LSHIFT));
    press(A);
    CHECK(has(U_A) && has(U_LSHIFT));
    CHECK_EQ(g_out.armed, 0);
    adv(40);
    CHECK(has(U_LSHIFT));                           /* as long as A is down */
    rel(A);
    CHECK_EQ(g_out.kbd_n, 0);                       /* then gone */
    press(A);
    CHECK(has(U_A) && !has(U_LSHIFT));              /* the next key is a plain key again */
    rel(A);

    /* the timeout */
    rig_start(0);
    press(LSHIFT);
    adv(30);
    rel(LSHIFT);                                    /* armed at 30 */
    adv(999);                                       /* 999 ms after it was armed: still there */
    CHECK(has(U_LSHIFT));
    adv(1);                                         /* 1000 ms: over */
    CHECK(!has(U_LSHIFT));
    CHECK_EQ(g_out.armed, 0);
    expect_idle();

    /* held as an ordinary modifier: gone when released, nothing armed */
    rig_start(0);
    press(LSHIFT);
    adv(50);
    press(A);
    adv(20);
    rel(A);
    CHECK(has(U_LSHIFT));                           /* the modifier is still held */
    adv(300);
    rel(LSHIFT);
    CHECK_EQ(g_out.kbd_n, 0);
    CHECK_EQ(g_out.armed, 0);

    /* held for the term or longer without any other key: also just a hold, nothing armed */
    rig_start(0);
    press(LSHIFT);
    adv(TERM);
    rel(LSHIFT);
    CHECK_EQ(g_out.kbd_n, 0);
    CHECK_EQ(g_out.armed, 0);
    rig_start(0);
    press(LSHIFT);
    adv(TERM - 1);
    rel(LSHIFT);
    CHECK_EQ(g_out.armed, 1u << 1);                 /* one ms shorter it is a tap */
    press(A); rel(A);

    /* two one-shot modifiers at once */
    rig_start(0);
    press(LSHIFT); adv(20); rel(LSHIFT);
    press(LCTRL); adv(20); rel(LCTRL);
    CHECK(has(U_LSHIFT) && has(U_LCTRL));
    CHECK_EQ(g_out.armed, (1u << 1) | (1u << 0));
    press(A);
    CHECK(has(U_A) && has(U_LSHIFT) && has(U_LCTRL));
    rel(A);
    CHECK_EQ(g_out.kbd_n, 0);

    /* things that do not use the modifier up: a plain modifier key, a layer key, a media key, a toggle key */
    tbl_oneshot(1000);
    tbl_set(0, ESC, KM_CODE_TOGGLE, 3, 0);
    tbl_set(1, Q, KM_CODE_PLAY_PAUSE, 0, 0);
    tbl_load();
    rig_start(0);
    press(LSHIFT); adv(20); rel(LSHIFT);
    press(RSHIFT);
    CHECK(has(U_LSHIFT) && has(U_RSHIFT));
    rel(RSHIFT);
    CHECK(has(U_LSHIFT));                           /* still armed */
    press(FN);
    CHECK(has(U_LSHIFT));
    press(Q);                                       /* layer 1: Q is not a key there (transparent -> Q); use a media key instead */
    rel(Q);
    rel(FN);
    press(ESC); rel(ESC);                           /* toggle layer 3 on */
    press(ESC); rel(ESC);                           /* and off */
    CHECK(has(U_LSHIFT));
    CHECK_EQ(g_out.armed, 1u << 1);
    press(A);
    CHECK(has(U_A) && has(U_LSHIFT));
    rel(A);
    expect_idle();

    /* a tap of a tap-hold key uses it up and carries it on the pulse */
    rig_start(0);
    press(LSHIFT); adv(20); rel(LSHIFT);
    press(SPACE);
    adv(30);
    rel(SPACE);
    CHECK(has(U_SPACE) && has(U_LSHIFT));
    adv(STEP);
    CHECK_EQ(g_out.kbd_n, 0);
    CHECK_EQ(g_out.armed, 0);

    /* a table change ends an armed modifier (a profile switch) */
    rig_start(0);
    press(LSHIFT); adv(20); rel(LSHIFT);
    CHECK_EQ(g_out.armed, 1u << 1);
    tbl_load();
    adv(1);
    CHECK_EQ(g_out.armed, 0);
    CHECK_EQ(g_out.kbd_n, 0);

    /* two keys carry their own modifier; releasing one does not drop the other's */
    rig_start(0);
    press(LSHIFT); adv(20); rel(LSHIFT);
    press(A);
    adv(20);
    press(LCTRL); adv(20); rel(LCTRL);              /* arm ctrl while A (with shift) is down */
    press(S);
    CHECK(has(U_A) && has(0x16) && has(U_LSHIFT) && has(U_LCTRL));
    rel(A);
    CHECK(has(0x16) && has(U_LCTRL) && !has(U_LSHIFT));
    rel(S);
    CHECK_EQ(g_out.kbd_n, 0);

    /* timeout 0: waits for the next key for ever */
    tbl_oneshot(0);
    rig_start(0);
    press(LSHIFT); adv(20); rel(LSHIFT);
    adv(20000);
    CHECK(has(U_LSHIFT));
    press(A);
    CHECK(has(U_A) && has(U_LSHIFT));
    rel(A);
    expect_idle();
}

/* ---------------------------------------------------------------- the clocks */

static void test_clocks(void)
{
    /* the millisecond counter may wrap in the middle of a decision */
    tbl_taphold_basic(TERM, KM_TAP_POLICY_TIMEOUT);
    rig_start(0xFFFFFF00u);                         /* wraps after 255 ms */
    press(SPACE);
    adv(TERM - 1);
    CHECK(g_out.pending);
    adv(1);
    CHECK_EQ(g_out.layer, 1);
    rel(SPACE);
    adv(30);
    rig_start(0xFFFFFFF0u);
    press(SPACE);
    adv(30);
    rel(SPACE);                                     /* a tap across the wrap */
    CHECK(has(U_SPACE));
    adv(STEP);
    CHECK(!has(U_SPACE));
    CHECK_EQ(visible_for(U_SPACE), (int)STEP);
    tbl_oneshot(500);
    rig_start(0xFFFFFFF0u);
    press(LSHIFT); adv(20); rel(LSHIFT);
    adv(400);
    CHECK(has(U_LSHIFT));
    adv(100);
    CHECK(!has(U_LSHIFT));

    /* a CPU cycle counter: 240000 cycles per ms, 32 bit, wrapping every ~17.9 s; the module turns it into milliseconds */
    tbl_taphold_basic(TERM, KM_TAP_POLICY_TIMEOUT);
    km_layers_reset();
    uint32_t cyc = 0xFFF00000u;
    const uint32_t cpm = 240000u;
    uint8_t d[1] = { SPACE };
    km_layers_out_t o;
    km_layers_update_cycles(d, 1, cyc, cpm, &o);
    CHECK(o.pending);
    for (unsigned ms = 1; ms <= TERM + 5u; ms++) {
        cyc += cpm + (ms % 3u == 0 ? 17u : 0u);         /* the loop is never exactly on a millisecond */
        km_layers_update_cycles(d, 1, cyc, cpm, &o);
        if (ms < TERM - 3u) {
            CHECK(o.pending);
        }
    }
    CHECK(!o.pending);
    CHECK_EQ(o.layer, 1);
    km_layers_update_cycles(d, 0, cyc, cpm, &o);
    CHECK_EQ(o.layer, 0);
    /* several seconds pass between two calls: nothing breaks */
    cyc += 5u * 1000u * cpm;
    km_layers_update_cycles(d, 0, cyc, cpm, &o);
    CHECK_EQ(o.kbd_n, 0);
    km_layers_reset();
}

/* ---------------------------------------------------------------- randomised */

static uint32_t g_rs;
static uint32_t rnd(void)
{
    g_rs = g_rs * 1664525u + 1013904223u;
    return g_rs >> 8;
}

enum { ROLE_PLAIN, ROLE_TAPHOLD, ROLE_TOGGLE, ROLE_OSM, ROLE_HOLD };

typedef struct {
    unsigned slot;
    int      role;
    uint8_t  use[KM_LAYERS];   /* plain: the usage on layers 0..3; tap-hold: use[0] = tap usage */
    uint8_t  toggle_layer;
    bool     raw;
    uint32_t t_press, t_release;
    uint32_t presses, appearances, taps_expected;
    bool     isolated;         /* tap-hold: no other key event between its press and its release */
    bool     present;
} rkey_t;

static bool key_present(const rkey_t *k)
{
    const unsigned nl = k->role == ROLE_PLAIN ? KM_LAYERS : 1u;
    for (unsigned l = 0; l < nl; l++) {
        if (has(k->use[l])) {
            return true;
        }
    }
    return false;
}

static unsigned g_random_failures;
#define RCHECK(cond, seed, what)                                                       \
    do {                                                                               \
        km_test_checks++;                                                              \
        if (!(cond)) {                                                                 \
            km_test_failures++;                                                        \
            if (g_random_failures++ < 20) {                                            \
                printf("  FAIL random seed %u: %s (%s)\n", (unsigned)(seed), what, #cond); \
            }                                                                          \
        }                                                                              \
    } while (0)

/* looks at the current output; checks everything that must hold at every instant */
static void rsample(rkey_t *k, unsigned nk, uint32_t seed)
{
    bool seen[256] = { false };
    RCHECK(g_out.kbd_n <= KM_KEYMAP_SLOTS, seed, "too many usages");
    for (unsigned i = 0; i < g_out.kbd_n; i++) {
        RCHECK(g_out.kbd[i] >= 0x04 && g_out.kbd[i] <= 0xE7, seed, "usage out of range");
        RCHECK(!seen[g_out.kbd[i]], seed, "duplicate usage");
        seen[g_out.kbd[i]] = true;
    }
    for (unsigned i = 0; i < nk; i++) {
        const bool p = key_present(&k[i]);
        if (p && !k[i].present) {
            k[i].appearances++;
        }
        k[i].present = p;
        /* no stuck key: a plain key that has been up for a long time cannot still be reported */
        if ((k[i].role == ROLE_PLAIN || k[i].role == ROLE_TAPHOLD) && !k[i].raw && p && g_now - k[i].t_release > 1500u) {
            RCHECK(false, seed, "a released key is still reported");
        }
    }
}

static void rstep(rkey_t *k, unsigned nk, uint32_t seed)
{
    adv(1);
    rsample(k, nk, seed);
}

static void random_run(uint32_t seed)
{
    static const unsigned slots[] = { 73, 72, 74, 49, 2, 75, 6, 42 };
    enum { NK = 8 };
    rkey_t k[NK];
    g_rs = seed * 2654435761u + 12345u;
    const unsigned term = 40u + rnd() % 300u;
    const unsigned policy = rnd() % 3u;
    const unsigned oneshot = 100u + rnd() % 300u;
    tbl_begin(term, policy, oneshot);
    for (unsigned i = 0; i < NK; i++) {
        memset(&k[i], 0, sizeof k[i]);
        k[i].slot = slots[i];
        k[i].role = (int)(rnd() % 5u);
        if (i < 2) {
            k[i].role = i == 0 ? ROLE_PLAIN : ROLE_TAPHOLD;   /* always some of the interesting kinds */
        }
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            k[i].use[l] = (uint8_t)(0x04u + i);               /* the letter itself ... */
            if (l > 0 && k[i].role == ROLE_PLAIN) {
                k[i].use[l] = (uint8_t)(0x3Au + i * 3u + l);  /* ... and its layer variants (F-keys): all different */
            }
        }
        for (unsigned l = 1; l < KM_LAYERS; l++) {
            tbl_set(l, slots[i], k[i].role == ROLE_PLAIN ? k[i].use[l] : (uint8_t)KM_CODE_TRANSPARENT, 0, 0);
        }
        switch (k[i].role) {
        case ROLE_PLAIN:
            tbl_set(0, slots[i], k[i].use[0], 0, 0);
            break;
        case ROLE_TAPHOLD: {
            static const uint8_t holds[] = { KM_CODE_HOLD_L1, KM_CODE_HOLD_L2, KM_CODE_HOLD_L3, 0xE0, 0xE1, 0xE2, 0xE3 };
            k[i].use[0] = (uint8_t)(0x20u + i);
            tbl_set(0, slots[i], KM_CODE_TAP_HOLD, k[i].use[0], holds[rnd() % 7u]);
            break;
        }
        case ROLE_TOGGLE:
            k[i].toggle_layer = (uint8_t)(1u + rnd() % 3u);
            tbl_set(0, slots[i], KM_CODE_TOGGLE, k[i].toggle_layer, 0);
            break;
        case ROLE_OSM:
            tbl_set(0, slots[i], KM_CODE_ONESHOT_MOD, (uint8_t)(0xE0u + rnd() % 8u), 0);
            break;
        default: {
            static const uint8_t layer_holds[] = { KM_CODE_HOLD_L1, KM_CODE_HOLD_L2, KM_CODE_HOLD_L3 };
            tbl_set(0, slots[i], layer_holds[rnd() % 3u], 0, 0);
            break;
        }
        }
    }
    tbl_load();
    rig_start(rnd());
    g_trn = 0;
    uint8_t parity = 0;
    const unsigned actions = 60u + rnd() % 80u;
    for (unsigned a = 0; a < actions; a++) {
        const unsigned i = rnd() % NK;
        rkey_t *key = &k[i];
        for (unsigned j = 0; j < NK; j++) {
            if (j != i) {
                k[j].isolated = false;                  /* this event falls inside every other key's press */
            }
        }
        if (!key->raw) {
            key->raw = true;
            key->t_press = g_now;
            key->presses++;
            key->isolated = true;
            if (key->role == ROLE_TOGGLE) {
                parity ^= (uint8_t)(1u << key->toggle_layer);
            }
            press(key->slot);
            rsample(k, NK, seed);
        } else {
            key->raw = false;
            key->t_release = g_now;
            rel(key->slot);
            rsample(k, NK, seed);
            if (key->role == ROLE_TAPHOLD && key->isolated && g_now - key->t_press < term) {
                key->taps_expected++;                   /* alone and quick: it must come out as its tap */
            }
        }
        const unsigned gap = (rnd() % 4u == 0) ? rnd() % 3u : 1u + rnd() % 250u;
        for (unsigned ms = 0; ms < gap; ms++) {
            rstep(k, NK, seed);
        }
    }
    for (unsigned i = 0; i < NK; i++) {                 /* let go of everything */
        if (k[i].raw) {
            k[i].raw = false;
            k[i].t_release = g_now;
            rel(k[i].slot);
            rsample(k, NK, seed);
            for (unsigned j = 0; j < NK; j++) {
                k[j].isolated = false;
            }
        }
    }
    for (unsigned ms = 0; ms < 4000; ms++) {
        rstep(k, NK, seed);
    }
    RCHECK(g_out.kbd_n == 0, seed, "keys still reported after everything was released");
    RCHECK(g_out.consumer == 0, seed, "consumer still reported");
    RCHECK(!g_out.pending, seed, "a tap-hold key is still undecided");
    RCHECK(g_out.armed == 0, seed, "an armed one-shot modifier did not expire");
    RCHECK(g_out.toggled == parity, seed, "toggle parity is wrong (a press was lost or doubled)");
    unsigned top = 0;
    for (unsigned l = 1; l < KM_LAYERS; l++) {
        if (parity & (1u << l)) {
            top = l;
        }
    }
    RCHECK(g_out.layer == top, seed, "a held layer is stuck");
    for (unsigned i = 0; i < NK; i++) {
        if (k[i].role == ROLE_PLAIN) {
            RCHECK(k[i].appearances == k[i].presses, seed, "a plain key press was lost or doubled");
        } else if (k[i].role == ROLE_TAPHOLD) {
            RCHECK(k[i].appearances >= k[i].taps_expected, seed, "a quick, isolated tap was lost");
        }
    }
}

/* ---------------------------------------------------------------- M6: new codes and the locked-entry accessor (task P0) */

static bool locked(unsigned slot, uint8_t code, uint8_t a0, uint8_t a1)
{
    km_layers_entry_t e = { 0xAA, 0xAA, 0xAA };
    if (!km_layers_locked_entry(slot, &e)) {
        return false;
    }
    return e.code == code && e.arg0 == a0 && e.arg1 == a1;
}

static bool not_locked(unsigned slot)
{
    km_layers_entry_t e = { 0x55, 0x66, 0x77 };
    const bool r = km_layers_locked_entry(slot, &e);
    return !r && e.code == 0x55 && e.arg0 == 0x66 && e.arg1 == 0x77;      /* *out is left alone on false */
}

static void test_m6_codes(void)
{
    /* the codes that other modules own are silent in the keyboard output, and the key that carries one is reported with its
       exact entry (task P2 gave toggle keys and Mod-Morph keys an output of their own: tests/test_layers_m6.c) */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, A, KM_CODE_DKS, 2, 0);
    tbl_set(0, S, KM_CODE_PAD_AXIS, KM_PAD_AXIS_ARG(KM_PAD_AXIS_LX, true, false), 3);
    tbl_set(0, D, KM_CODE_PAD_BTN, 7, 20);
    tbl_set(0, E, KM_CODE_LIGHT, KM_LIGHT_ACT_BRIGHT_UP, 0);
    tbl_load();
    rig_start(1000);
    CHECK(not_locked(A));
    press(F);
    adv(STEP);
    CHECK(has(0x09));                                  /* an ordinary key still works */
    CHECK(locked(F, 0x09, 0, 0));
    const struct { unsigned slot; uint8_t code, a0, a1; } want[] = {
        { A, KM_CODE_DKS, 2, 0 },
        { S, KM_CODE_PAD_AXIS, (uint8_t)KM_PAD_AXIS_ARG(KM_PAD_AXIS_LX, true, false), 3 },
        { D, KM_CODE_PAD_BTN, 7, 20 },
        { E, KM_CODE_LIGHT, KM_LIGHT_ACT_BRIGHT_UP, 0 },
    };
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
        press(want[i].slot);
        adv(STEP);
        CHECK(locked(want[i].slot, want[i].code, want[i].a0, want[i].a1));
        CHECK_EQ(g_out.kbd_n, 1);                      /* only the F key; the M6 keys add nothing (not even their argument usage) */
        CHECK_EQ(g_out.consumer, 0);
        CHECK_EQ(g_out.profile_request, 0);
    }
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
        rel(want[i].slot);
        adv(STEP);
        CHECK(not_locked(want[i].slot));
    }
    CHECK(locked(F, 0x09, 0, 0));
    rel(F);
    adv(STEP);
    expect_idle();
    CHECK(not_locked(F));

    /* arguments of the accessor: a slot out of range, a NULL pointer, a slot without a key */
    km_layers_entry_t e;
    CHECK(!km_layers_locked_entry(KM_KEYMAP_SLOTS, &e) && !km_layers_locked_entry(255, &e));
    CHECK(!km_layers_locked_entry(A, NULL));
    CHECK(!km_layers_locked_entry(15, &e));            /* slot 15 carries no key */

    /* latching: the entry is the one the key had when it was pressed, until it is released */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, I, KM_CODE_DKS, 5, 0);
    tbl_set(1, I, KM_CODE_DKS, 1, 0);
    tbl_set(1, S, KM_CODE_PAD_BTN, 4, 0);
    tbl_load();
    rig_start(1000);
    press(FN);
    adv(STEP);
    press(I);
    adv(STEP);
    CHECK(locked(I, KM_CODE_DKS, 1, 0));
    rel(FN);
    adv(STEP);
    CHECK(locked(I, KM_CODE_DKS, 1, 0));               /* FN is up, the key keeps its layer-1 entry */
    press(S);
    adv(STEP);
    CHECK(locked(S, 0x16, 0, 0));                      /* S was pressed after FN went up: layer 0, a plain key */
    rel(I);
    adv(STEP);
    CHECK(not_locked(I));
    press(I);
    adv(STEP);
    CHECK(locked(I, KM_CODE_DKS, 5, 0));               /* pressed again on layer 0 */
    rel(I);
    rel(S);
    adv(STEP);
    expect_idle();

    /* a transparent layer-1 entry reports the layer-0 entry (code and arguments) */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, I, KM_CODE_DKS, 6, 0);
    tbl_set(1, I, KM_CODE_TRANSPARENT, 0, 0);
    tbl_load();
    rig_start(1000);
    press(FN);
    adv(STEP);
    press(I);
    adv(STEP);
    CHECK(locked(I, KM_CODE_DKS, 6, 0));
    rel(I);
    rel(FN);

    /* a tap-hold key: nothing to report while it is undecided or after a tap; as a hold it reports its tap-hold entry, and
       the hold action may be an ordinary key (format 3) */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, SPACE, KM_CODE_TAP_HOLD, U_SPACE, 0x05);
    tbl_load();
    rig_start(1000);
    press(SPACE);
    adv(TERM / 2);
    CHECK(g_out.pending && not_locked(SPACE));
    rel(SPACE);
    adv(3 * STEP);
    CHECK(not_locked(SPACE));                          /* a tap is a pulse */
    CHECK_EQ(appearances(U_SPACE), 1);
    rig_start(1000);
    press(SPACE);
    adv(TERM);
    CHECK(!g_out.pending && has(0x05));                /* the hold action is the 'b' key */
    CHECK(locked(SPACE, KM_CODE_TAP_HOLD, U_SPACE, 0x05));
    rel(SPACE);
    adv(STEP);
    CHECK(not_locked(SPACE));
    expect_idle();

    /* a press that waits behind an undecided tap-hold key is not locked until it is handled, then it is, with the layer the
       tap-hold decision gave it */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, SPACE, KM_CODE_TAP_HOLD, U_SPACE, KM_CODE_HOLD_L1);
    tbl_set(0, I, KM_CODE_DKS, 5, 0);
    tbl_set(1, I, KM_CODE_DKS, 1, 0);
    tbl_load();
    rig_start(1000);
    press(SPACE);
    adv(10);
    press(I);
    adv(TERM / 2);
    CHECK(g_out.pending && not_locked(I));             /* queued behind the undecided key */
    adv(TERM);
    CHECK(!g_out.pending);
    adv(2 * STEP);
    CHECK(locked(I, KM_CODE_DKS, 1, 0));               /* replayed after the hold decision: layer 1 */
    rel(I);
    rel(SPACE);
    adv(STEP);
    expect_idle();

    /* reset forgets everything */
    tbl_begin(TERM, KM_TAP_POLICY_TIMEOUT, 2000);
    tbl_set(0, A, KM_CODE_PAD_BTN, 1, 0);
    tbl_load();
    rig_start(1000);
    press(A);
    adv(STEP);
    CHECK(locked(A, KM_CODE_PAD_BTN, 1, 0));
    km_layers_reset();
    CHECK(not_locked(A));

    /* randomised: tables full of the new codes; a key that is up is never reported (except while a decision is pending), a
       reported entry is always a valid table entry, and nothing is reported or output once everything is released */
    uint32_t rng = 99u;
    for (unsigned run = 0; run < 150u; run++) {
        tbl_begin(TERM, run % 3u, 2000);
        const unsigned slots[] = { A, S, D, F, Q, W, E, R, I, SPACE };
        for (unsigned i = 0; i < sizeof slots / sizeof slots[0]; i++) {
            rng = rng * 1664525u + 1013904223u;
            switch ((rng >> 16) % 8u) {
            case 0: tbl_set(0, slots[i], KM_CODE_DKS, (uint8_t)((rng >> 8) % KM_DKS_MAX), 0); break;
            case 1: tbl_set(0, slots[i], KM_CODE_PAD_BTN, (uint8_t)(1u + (rng >> 8) % 20u), 0); break;
            case 2: tbl_set(0, slots[i], KM_CODE_PAD_AXIS, (uint8_t)KM_PAD_AXIS_ARG((rng >> 8) % 4u, (rng >> 12) & 1u, false), (uint8_t)((rng >> 4) % 4u)); break;
            case 3: tbl_set(0, slots[i], KM_CODE_TAP_HOLD, U_SPACE, (uint8_t)(0x04u + (rng >> 8) % 20u)); break;
            case 4: tbl_set(0, slots[i], KM_CODE_LIGHT, KM_LIGHT_ACT_NEXT_FX, 0); break;   /* (was a toggle key before task P2: it now stays on after release) */
            case 5: tbl_set(1, slots[i], KM_CODE_DKS, 1, 0); break;
            default: break;
            }
        }
        tbl_load();
        rig_start(5000);
        for (unsigned step = 0; step < 120u; step++) {
            rng = rng * 1664525u + 1013904223u;
            const unsigned slot = slots[(rng >> 16) % (sizeof slots / sizeof slots[0])];
            bool down = false;
            for (unsigned i = 0; i < g_n; i++) {
                down |= g_down[i] == slot;
            }
            if (down) {
                rel(slot);
            } else if (g_n < 8u) {
                /* a key may be pressed again at once (task P0 had to wait half a second after a release: a second tap inside the
                   replay window of an undecided tap-hold key could leave a press without its release; fixed in task P2) */
                press(slot);
            }
            adv((rng >> 4) % 40u);
            for (unsigned i = 0; i < sizeof slots / sizeof slots[0]; i++) {
                km_layers_entry_t le;
                bool isdown = false;
                for (unsigned k = 0; k < g_n; k++) {
                    isdown |= g_down[k] == slots[i];
                }
                if (isdown && km_layers_locked_entry(slots[i], &le)) {
                    CHECK(km_keymap_entry_valid(le.code, le.arg0, le.arg1) || le.code == 0);
                }
            }
        }
        while (g_n > 0) {
            rel(g_down[g_n - 1]);
        }
        adv(3 * TERM);
        for (unsigned i = 0; i < sizeof slots / sizeof slots[0]; i++) {
            CHECK(!km_layers_locked_entry(slots[i], &e));
        }
        expect_idle();
    }
}

static void test_random(void)
{
    for (uint32_t seed = 1; seed <= 600; seed++) {
        random_run(seed);
    }
}

void test_layers_ext(void)
{
    test_tables_v2();
    test_toggle();
    test_toggle_and_holds();
    test_tap();
    test_hold();
    test_long_gap_between_updates();
    test_other_key_timeout_policy();
    test_other_key_policies();
    test_two_tap_holds();
    test_tap_hold_on_a_layer();
    test_events_not_delayed_after_decision();
    test_queue_overflow();
    test_oneshot();
    test_clocks();
    test_m6_codes();
    test_random();
    km_layers_set_map(NULL);
    km_layers_reset();
}
