/*
 * test_layers.c - M5 task T-C: key-map layers, the FN key, media keys, profile keys, consumer report.
 *
 * The expected values are written here by key NAME straight from the USB HID Usage Tables (keyboard page 0x07,
 * consumer page 0x0C), separately from the tables in km_keymap.c, and a small independent model of the latching
 * rule ("a key keeps the layer it was pressed on") checks km_layers_update() over every order of FN and key events.
 */
#include <string.h>

#include "km/km_keymap.h"
#include "km/km_layers.h"
#include "km/km_report.h"
#include "km/km_usb_reports.h"
#include "km_test.h"

/* ---------------------------------------------------------------- helpers */

static int slot_of(const char *name)
{
    for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
        if (strcmp(km_keymap_slot_name(s), name) == 0) {
            return (int)s;
        }
    }
    return -1;
}

typedef enum { L1_KBD, L1_CONSUMER, L1_PROFILE } l1_kind_t;

typedef struct {
    const char *name;
    l1_kind_t   kind;
    unsigned    value;  /* keyboard usage, consumer usage, or profile number 1..4 */
} l1_t;

/* The default layer 1 as documented in core/LAYERS_NOTES.md. Keys not listed here are transparent. */
static const l1_t k_l1[] = {
    { "1", L1_KBD, 0x3A }, { "2", L1_KBD, 0x3B }, { "3", L1_KBD, 0x3C }, { "4", L1_KBD, 0x3D },
    { "5", L1_KBD, 0x3E }, { "6", L1_KBD, 0x3F }, { "7", L1_KBD, 0x40 }, { "8", L1_KBD, 0x41 },
    { "9", L1_KBD, 0x42 }, { "0", L1_KBD, 0x43 }, { "-", L1_KBD, 0x44 }, { "=", L1_KBD, 0x45 },
    { "BACKSPACE", L1_KBD, 0x4C }, { "TAB", L1_KBD, 0x35 },
    { "I", L1_KBD, 0x52 }, { "J", L1_KBD, 0x50 }, { "K", L1_KBD, 0x51 }, { "L", L1_KBD, 0x4F },
    { "U", L1_KBD, 0x4A }, { "O", L1_KBD, 0x4D }, { "P", L1_KBD, 0x4B }, { ";", L1_KBD, 0x4E },
    { "[", L1_CONSUMER, 0xEA }, { "]", L1_CONSUMER, 0xE9 }, { "M", L1_CONSUMER, 0xE2 },
    { ",", L1_CONSUMER, 0xB6 }, { ".", L1_CONSUMER, 0xCD }, { "/", L1_CONSUMER, 0xB5 },
    { "Z", L1_PROFILE, 1 }, { "X", L1_PROFILE, 2 }, { "C", L1_PROFILE, 3 }, { "V", L1_PROFILE, 4 },
};

static const l1_t *l1_of(const char *name)
{
    for (size_t i = 0; i < sizeof k_l1 / sizeof k_l1[0]; i++) {
        if (strcmp(k_l1[i].name, name) == 0) {
            return &k_l1[i];
        }
    }
    return NULL;
}

/* a little keyboard: the pressed slots in press order, fed to km_layers_update() after every event */
static uint8_t  g_down[KM_KEYMAP_SLOTS];
static unsigned g_n;
static km_layers_out_t g_out;

static void rig_reset(void)
{
    km_layers_set_map(NULL);
    km_layers_reset();
    g_n = 0;
    km_layers_update(g_down, 0, &g_out);
}

static void rig_update(void)
{
    km_layers_update(g_down, g_n, &g_out);
}

static void rig_press(unsigned slot)
{
    for (unsigned i = 0; i < g_n; i++) {
        if (g_down[i] == slot) {
            return;
        }
    }
    g_down[g_n++] = (uint8_t)slot;
    rig_update();
}

static void rig_release(unsigned slot)
{
    for (unsigned i = 0; i < g_n; i++) {
        if (g_down[i] == slot) {
            memmove(&g_down[i], &g_down[i + 1], g_n - i - 1u);
            g_n--;
            break;
        }
    }
    rig_update();
}

static bool kbd_is(const unsigned *want, unsigned n)
{
    if (g_out.kbd_n != n) {
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        if (g_out.kbd[i] != want[i]) {
            return false;
        }
    }
    return true;
}

/* a macro so that a failure names the line of the check, not of this helper */
#define expect_kbd1(usage)                                                                 do {                                                                                       const unsigned w1_[1] = { (usage) };                                                   CHECK(kbd_is(w1_, 1));                                                             } while (0)

static void expect_idle(void)
{
    CHECK_EQ(g_out.kbd_n, 0);
    CHECK_EQ(g_out.consumer, 0);
    CHECK_EQ(g_out.profile_request, 0);
}

/* ---------------------------------------------------------------- key map tables */

static void test_tables(void)
{
    uint8_t t[KM_LAYERS][KM_KEYMAP_SLOTS];
    km_layers_set_map(NULL);
    km_keymap_default_table(t);
    CHECK(km_keymap_table_valid(t));

    const int fn = slot_of("FN");
    CHECK_EQ(fn, 14);

    for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
        const char *name = km_keymap_slot_name(s);
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            CHECK_EQ(km_keymap_code(l, s), t[l][s]);  /* the table API and the lookup agree */
            CHECK_EQ(km_keymap_usage(l, s), t[l][s] < 0xF0 ? t[l][s] : 0);
        }
        if (!km_keymap_slot_is_key(s)) {
            for (unsigned l = 0; l < KM_LAYERS; l++) {
                CHECK_EQ(t[l][s], 0);                     /* a slot without a key is "no key" on every layer */
            }
            continue;
        }
        /* layer 0 is the M4 map: usage view unchanged, FN is the only function code */
        if ((int)s == fn) {
            CHECK_EQ(t[0][s], 0xF0);
            CHECK_EQ(km_keymap_usage(0, s), 0);
            CHECK_EQ(t[1][s], 0xFF);
        } else {
            CHECK(t[0][s] >= 0x04 && t[0][s] <= 0xE7);
        }
        /* layers 2 and 3: transparent everywhere by default */
        CHECK_EQ(t[2][s], 0xFF);
        CHECK_EQ(t[3][s], 0xFF);
        /* layer 1 against the by-name expectation */
        const l1_t *e = l1_of(name);
        if (e == NULL) {
            CHECK_EQ(t[1][s], 0xFF);
        } else if (e->kind == L1_KBD) {
            CHECK_EQ(t[1][s], e->value);
        } else if (e->kind == L1_CONSUMER) {
            CHECK(t[1][s] >= 0xF5 && t[1][s] <= 0xFA);
            CHECK_EQ(km_layers_consumer_usage(t[1][s]), e->value);
        } else {
            CHECK_EQ(t[1][s], 0xF0 + e->value);
        }
    }
    /* the legacy view: layer 1 ESC / transparent keys read as usage 0 */
    CHECK_EQ(km_keymap_usage(1, 18), 0);
    CHECK_EQ(km_keymap_usage(4, 18), 0);
    CHECK_EQ(km_keymap_code(0, 76), 0);
    CHECK_EQ(km_keymap_code(0, 100000), 0);

    /* no usage twice on the FN layer among the keyboard usages */
    unsigned seen[256] = { 0 };
    for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
        if (t[1][s] >= 4 && t[1][s] <= 0xE7) {
            seen[t[1][s]]++;
        }
    }
    for (unsigned u = 0; u < 256; u++) {
        CHECK(seen[u] <= 1);
    }
    /* the six media codes map to six distinct consumer usages, other codes to none */
    CHECK_EQ(km_layers_consumer_usage(KM_CODE_VOL_DOWN), 0xEA);
    CHECK_EQ(km_layers_consumer_usage(KM_CODE_VOL_UP), 0xE9);
    CHECK_EQ(km_layers_consumer_usage(KM_CODE_MUTE), 0xE2);
    CHECK_EQ(km_layers_consumer_usage(KM_CODE_PREV_TRACK), 0xB6);
    CHECK_EQ(km_layers_consumer_usage(KM_CODE_PLAY_PAUSE), 0xCD);
    CHECK_EQ(km_layers_consumer_usage(KM_CODE_NEXT_TRACK), 0xB5);
    CHECK_EQ(km_layers_consumer_usage(KM_CODE_HOLD_L1), 0);
    CHECK_EQ(km_layers_consumer_usage(0x04), 0);
    CHECK_EQ(km_layers_consumer_usage(0xFF), 0);
}

static void test_validation_and_loading(void)
{
    /* every possible code byte: accepted exactly when 0, 0x04..0xE7, 0xE8..0xEF (extended, needs arguments; 0xEB..0xEF are the
       M6 codes) or 0xF0..0xFF; the settings format version 2 rule stops at 0xEA */
    for (unsigned v = 0; v < 256; v++) {
        const bool want = v == 0 || (v >= 4 && v <= 0xEF) || v >= 0xF0;
        CHECK_EQ(km_keymap_code_valid((uint8_t)v), want);
        CHECK_EQ(km_keymap_code_valid_v2((uint8_t)v), v == 0 || (v >= 4 && v <= 0xEA) || v >= 0xF0);
    }
    CHECK(!km_keymap_code_valid(0x01));
    CHECK(!km_keymap_code_valid(0x03));
    CHECK(km_keymap_code_valid(0xE8) && km_keymap_code_valid(0xE8 + 3));
    CHECK(km_keymap_code_valid(0xEF) && !km_keymap_code_valid_v2(0xEF));
    CHECK(!km_keymap_entry_valid(0xE8, 0, 0));   /* the extended codes need their arguments, a table of bare codes cannot hold them */

    uint8_t t[KM_LAYERS][KM_KEYMAP_SLOTS];
    km_keymap_default_table(t);

    /* one bad entry anywhere (first and last cell of every layer) rejects the whole table */
    for (unsigned l = 0; l < KM_LAYERS; l++) {
        for (unsigned k = 0; k < 2; k++) {
            const unsigned s = k == 0 ? 0u : KM_KEYMAP_SLOTS - 1u;
            const uint8_t keep = t[l][s];
            t[l][s] = 0x02;
            CHECK(!km_keymap_table_valid(t));
            t[l][s] = 0xEA;
            CHECK(!km_keymap_table_valid(t));
            t[l][s] = keep;
            CHECK(km_keymap_table_valid(t));
        }
    }

    /* load a valid replacement: layer 0 key A (slot 73) becomes B, and the table is copied */
    CHECK_EQ(km_keymap_usage(0, 73), 0x04);
    t[0][73] = 0x05;
    CHECK(km_layers_set_map(t));
    t[0][73] = 0x06;  /* changing the caller's buffer afterwards must not matter */
    CHECK_EQ(km_keymap_usage(0, 73), 0x05);
    CHECK_EQ(km_keymap_code(1, 1), 0x52);

    /* a bad table is refused and the built-in table is back */
    t[0][73] = 0x01;
    CHECK(!km_layers_set_map(t));
    CHECK_EQ(km_keymap_usage(0, 73), 0x04);

    /* NULL restores the built-in table */
    t[0][73] = 0x05;
    CHECK(km_layers_set_map(t));
    CHECK_EQ(km_keymap_usage(0, 73), 0x05);
    CHECK(km_layers_set_map(NULL));
    CHECK_EQ(km_keymap_usage(0, 73), 0x04);

    /* a custom function code is kept raw but hidden from the usage view */
    t[0][73] = KM_CODE_MUTE;
    CHECK(km_layers_set_map(t));
    CHECK_EQ(km_keymap_code(0, 73), KM_CODE_MUTE);
    CHECK_EQ(km_keymap_usage(0, 73), 0);
    km_layers_set_map(NULL);
}

/* ---------------------------------------------------------------- reports */

static void test_reports(void)
{
    uint8_t r[3];
    km_report_build_consumer(0x00CD, r);
    CHECK_EQ(r[0], 2);
    CHECK_EQ(r[1], 0xCD);
    CHECK_EQ(r[2], 0x00);
    km_report_build_consumer(0x0123, r);
    CHECK_EQ(r[1], 0x23);   /* little endian */
    CHECK_EQ(r[2], 0x01);
    km_report_build_consumer(0, r);
    CHECK_EQ(r[0], 2);
    CHECK_EQ(r[1], 0);
    CHECK_EQ(r[2], 0);
    km_report_build_consumer(0x03FF, r);
    CHECK_EQ(r[1], 0xFF);
    CHECK_EQ(r[2], 0x03);
    km_report_build_consumer(0x0400, r);  /* above what the descriptor declares: released, not a wrong key */
    CHECK_EQ(r[1] | r[2], 0);
    km_report_build_consumer(0xFFFF, r);
    CHECK_EQ(r[1] | r[2], 0);
    /* matches the wire format header */
    CHECK_EQ(KM_REPORT_CONSUMER_ID, KM_USB_NKRO_ID_CONSUMER);
    CHECK_EQ(KM_REPORT_CONSUMER_LEN, KM_USB_CONSUMER_REPORT_LEN);
    CHECK_EQ(KM_REPORT_CONSUMER_MAX, KM_USB_CONSUMER_LAST_USAGE);
    CHECK_EQ(KM_REPORT_NKRO_LEN, KM_USB_NKRO_BITMAP_BYTES);
    /* every usage the layers can produce fits the descriptor */
    for (unsigned c = KM_CODE_VOL_DOWN; c <= KM_CODE_NEXT_TRACK; c++) {
        CHECK(km_layers_consumer_usage((uint8_t)c) != 0);
        CHECK(km_layers_consumer_usage((uint8_t)c) <= KM_USB_CONSUMER_LAST_USAGE);
    }

    /* the usage-based builders equal the slot-based ones on layer 0 (letters, modifiers, FN, no-key slots) */
    km_layers_set_map(NULL);
    uint32_t seed = 12345;
    for (unsigned round = 0; round < 400; round++) {
        uint8_t slots[16];
        uint8_t usages[16];
        const unsigned n = (round % 12u) + 1u;
        for (unsigned i = 0; i < n; i++) {
            seed = seed * 1664525u + 1013904223u;
            slots[i] = (uint8_t)((seed >> 16) % 80u);   /* includes no-key slots and slots >= 76 */
            usages[i] = km_keymap_usage(0, slots[i]);
        }
        uint8_t a[8], b[8], c[29], d[29];
        km_report_build_boot(slots, n, a);
        km_report_build_boot_usages(usages, n, b);
        CHECK(memcmp(a, b, 8) == 0);
        km_report_build_nkro(slots, n, c);
        km_report_build_nkro_usages(usages, n, d);
        CHECK(memcmp(c, d, 29) == 0);
    }
    /* usage lists: error codes and out-of-range numbers are ignored, seven keys roll over, modifiers still count */
    const uint8_t bad[] = { 0x01, 0x02, 0x03, 0xE8, 0xFF, 0xF0, 0x00, 0x2C };
    uint8_t b8[8], n29[29];
    km_report_build_boot_usages(bad, sizeof bad, b8);
    CHECK_EQ(b8[0], 0);
    CHECK_EQ(b8[2], 0x2C);
    CHECK_EQ(b8[3], 0);
    km_report_build_nkro_usages(bad, sizeof bad, n29);
    unsigned bits = 0;
    for (unsigned i = 0; i < 29; i++) {
        for (unsigned k = 0; k < 8; k++) {
            bits += (n29[i] >> k) & 1u;
        }
    }
    CHECK_EQ(bits, 1);
    CHECK(((n29[0x2C >> 3] >> (0x2C & 7)) & 1) == 1);
    const uint8_t seven[] = { 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0xE1 };
    km_report_build_boot_usages(seven, sizeof seven, b8);
    CHECK_EQ(b8[0], 0x02);
    for (unsigned i = 2; i < 8; i++) {
        CHECK_EQ(b8[i], 0x01);
    }
}

/* ---------------------------------------------------------------- layer behaviour, by hand */

static void test_basic(void)
{
    const unsigned fn = (unsigned)slot_of("FN"), i = (unsigned)slot_of("I"), a = (unsigned)slot_of("A");
    const unsigned esc = (unsigned)slot_of("ESC"), k1 = (unsigned)slot_of("1");

    rig_reset();
    expect_idle();
    CHECK_EQ(g_out.layer, 0);

    rig_press(i);                     /* without FN: the letter */
    expect_kbd1(0x0C);
    rig_release(i);
    expect_idle();

    rig_press(fn);                    /* FN alone emits nothing */
    expect_idle();
    CHECK_EQ(g_out.layer, 1);
    rig_press(i);                     /* FN + I = Up */
    expect_kbd1(0x52);
    rig_press(esc);                   /* transparent key: still ESC */
    {
        const unsigned w[2] = { 0x52, 0x29 };
        CHECK(kbd_is(w, 2));
    }
    rig_release(esc);
    rig_press(k1);                    /* FN + 1 = F1 */
    {
        const unsigned w[2] = { 0x52, 0x3A };
        CHECK(kbd_is(w, 2));
    }
    rig_release(k1);
    rig_release(i);
    rig_release(fn);
    expect_idle();
    CHECK_EQ(g_out.layer, 0);

    /* "A" held, FN pressed: A stays A (FN does not change keys that are already down) */
    rig_press(a);
    rig_press(fn);
    expect_kbd1(0x04);
    rig_press(i);
    {
        const unsigned w[2] = { 0x04, 0x52 };
        CHECK(kbd_is(w, 2));
    }
    rig_release(a);
    expect_kbd1(0x52);
    rig_release(i);
    rig_release(fn);
    expect_idle();
}

/* The mutation-style checks: each one fails if the layer is looked up when the key is REPORTED instead of when it is
 * PRESSED, or if the release forgets what the press emitted. */
static void test_latching(void)
{
    const unsigned fn = (unsigned)slot_of("FN"), i = (unsigned)slot_of("I"), a = (unsigned)slot_of("A");
    const unsigned dot = (unsigned)slot_of("."), z = (unsigned)slot_of("Z"), bs = (unsigned)slot_of("BACKSPACE");

    /* FN first, key second, FN released first: the key stays Up (not I) until it is released */
    rig_reset();
    rig_press(fn);
    rig_press(i);
    rig_release(fn);
    expect_kbd1(0x52);                /* a non-latching implementation would now report 0x0C ("I") */
    CHECK_EQ(g_out.layer, 0);
    rig_release(i);
    expect_idle();                    /* and would leave 0x52 stuck, or release the wrong usage */

    /* key first, FN second, key released after FN: still the plain letter, no Up appears */
    rig_reset();
    rig_press(i);
    rig_press(fn);
    expect_kbd1(0x0C);                /* a non-latching implementation would switch to 0x52 here */
    rig_release(fn);
    expect_kbd1(0x0C);
    rig_release(i);
    expect_idle();

    /* the same key twice: pressed on layer 1, released, pressed on layer 0 */
    rig_reset();
    rig_press(fn);
    rig_press(bs);
    expect_kbd1(0x4C);                /* Delete */
    rig_release(bs);
    rig_release(fn);
    rig_press(bs);
    expect_kbd1(0x2A);                /* Backspace */
    rig_release(bs);
    expect_idle();

    /* a media key survives FN release, and ends when it is released */
    rig_reset();
    rig_press(fn);
    rig_press(dot);
    CHECK_EQ(g_out.consumer, 0xCD);
    CHECK_EQ(g_out.kbd_n, 0);
    rig_release(fn);
    CHECK_EQ(g_out.consumer, 0xCD);
    rig_release(dot);
    expect_idle();

    /* a layer-0 key held while FN comes and goes keeps its release */
    rig_reset();
    rig_press(a);
    for (unsigned k = 0; k < 5; k++) {
        rig_press(fn);
        expect_kbd1(0x04);
        rig_release(fn);
        expect_kbd1(0x04);
    }
    rig_release(a);
    expect_idle();

    /* profile key: the request comes on RELEASE, even when FN was let go first */
    rig_reset();
    rig_press(fn);
    rig_press(z);
    expect_idle();                    /* nothing while held, and no 'Z' either */
    rig_release(fn);
    expect_idle();
    rig_release(z);
    CHECK_EQ(g_out.profile_request, 1);
    CHECK_EQ(g_out.kbd_n, 0);
    rig_update();                     /* only for that one update */
    CHECK_EQ(g_out.profile_request, 0);

    /* Z pressed on layer 0 and FN pressed afterwards: releasing Z is a plain letter release, no profile switch */
    rig_reset();
    rig_press(z);
    expect_kbd1(0x1D);
    rig_press(fn);
    rig_release(z);
    CHECK_EQ(g_out.profile_request, 0);
    expect_idle();
    rig_release(fn);
    expect_idle();
}

static void test_same_update(void)
{
    const uint8_t fn = (uint8_t)slot_of("FN"), i = (uint8_t)slot_of("I");

    /* FN and I appear in the same update: the list order decides */
    rig_reset();
    {
        const uint8_t order1[2] = { fn, i };
        km_layers_update(order1, 2, &g_out);
        expect_kbd1(0x52);
    }
    rig_reset();
    {
        const uint8_t order2[2] = { i, fn };
        km_layers_update(order2, 2, &g_out);
        expect_kbd1(0x0C);
    }
    /* everything released in one update, also with FN + key vanishing together */
    km_layers_update(g_down, 0, &g_out);
    expect_idle();
    CHECK_EQ(g_out.layer, 0);
    {
        const uint8_t both[2] = { fn, i };
        km_layers_update(both, 2, &g_out);
        km_layers_update(g_down, 0, &g_out);
        expect_idle();
        km_layers_update(g_down, 0, &g_out);
        expect_idle();
    }
    /* repeated and out-of-range entries are ignored */
    rig_reset();
    {
        const uint8_t odd[5] = { i, i, 200, 15, i };   /* 15 = a mirror slot without a key */
        km_layers_update(odd, 5, &g_out);
        expect_kbd1(0x0C);
    }
    km_layers_reset();
    km_layers_update(g_down, 0, &g_out);
    expect_idle();
}

/* ---------------------------------------------------------------- every order of FN and key events */

typedef struct {
    int event[4];   /* 0 FN down, 1 FN up, 2 K down, 3 K up */
    unsigned n;
} seq_t;

static unsigned g_seq_runs;

/* the independent model: what a key emits is decided by whether FN was down when the key was pressed */
static void run_sequence(const char *key_name, const seq_t *q, bool fn0, bool k0)
{
    const unsigned fn = (unsigned)slot_of("FN"), k = (unsigned)slot_of(key_name);
    const l1_t *l1 = l1_of(key_name);
    const unsigned l0_usage = km_keymap_usage(0, k);

    rig_reset();
    bool fn_down = false, k_down = false, k_on_l1 = false;
    if (fn0) {
        rig_press(fn);
        fn_down = true;
    }
    if (k0) {
        k_down = true;
        k_on_l1 = fn_down;
        rig_press(k);
    }
    for (unsigned e = 0; e < q->n; e++) {
        switch (q->event[e]) {
        case 0: rig_press(fn); fn_down = true; break;
        case 1: rig_release(fn); fn_down = false; break;
        case 2: rig_press(k); k_down = true; k_on_l1 = fn_down; break;
        default: rig_release(k); k_down = false; break;
        }
        /* expectation */
        unsigned want_kbd = 0, want_cons = 0, want_prof_now = 0;
        if (k_down) {
            if (k_on_l1 && l1 != NULL) {
                if (l1->kind == L1_KBD) {
                    want_kbd = l1->value;
                } else if (l1->kind == L1_CONSUMER) {
                    want_cons = l1->value;
                }
            } else {
                want_kbd = l0_usage;
            }
        } else if (q->event[e] == 3 && k_on_l1 && l1 != NULL && l1->kind == L1_PROFILE) {
            want_prof_now = l1->value;   /* the release that just happened */
        }
        CHECK_EQ(g_out.kbd_n, want_kbd != 0 ? 1 : 0);
        if (want_kbd != 0 && g_out.kbd_n == 1) {
            CHECK_EQ(g_out.kbd[0], want_kbd);
        }
        CHECK_EQ(g_out.consumer, want_cons);
        CHECK_EQ(g_out.profile_request, want_prof_now);
        CHECK_EQ(g_out.layer, fn_down ? 1 : 0);
    }
    /* finish: release whatever is still down; everything must end idle (a profile key still reports its release) */
    if (k_down) {
        rig_release(k);
        CHECK_EQ(g_out.profile_request, (k_on_l1 && l1 != NULL && l1->kind == L1_PROFILE) ? l1->value : 0);
        g_out.profile_request = 0;
    }
    if (fn_down) {
        rig_release(fn);
    }
    rig_update();   /* a profile request lasts one update only */
    expect_idle();
    CHECK_EQ(g_out.layer, 0);
    g_seq_runs++;
}

/* all interleavings of two ordered chains: chain A (FN) with na events, chain B (key) with nb events */
static void interleave(const char *name, const int *a, unsigned na, const int *b, unsigned nb, bool fn0, bool k0,
                       seq_t *cur, unsigned ia, unsigned ib)
{
    if (ia == na && ib == nb) {
        run_sequence(name, cur, fn0, k0);
        return;
    }
    if (ia < na) {
        cur->event[cur->n++] = a[ia];
        interleave(name, a, na, b, nb, fn0, k0, cur, ia + 1u, ib);
        cur->n--;
    }
    if (ib < nb) {
        cur->event[cur->n++] = b[ib];
        interleave(name, a, na, b, nb, fn0, k0, cur, ia, ib + 1u);
        cur->n--;
    }
}

static void test_all_orders(void)
{
    static const int fn_full[2] = { 0, 1 }, fn_up[1] = { 1 };
    static const int k_full[2] = { 2, 3 }, k_up[1] = { 3 };
    unsigned keys = 0;
    g_seq_runs = 0;
    for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
        const char *name = km_keymap_slot_name(s);
        if (!km_keymap_slot_is_key(s) || strcmp(name, "FN") == 0) {
            continue;
        }
        keys++;
        for (unsigned init = 0; init < 4; init++) {
            const bool fn0 = (init & 1u) != 0, k0 = (init & 2u) != 0;
            seq_t cur = { { 0 }, 0 };
            interleave(name, fn0 ? fn_up : fn_full, fn0 ? 1u : 2u, k0 ? k_up : k_full, k0 ? 1u : 2u, fn0, k0, &cur, 0, 0);
        }
    }
    CHECK_EQ(keys, KM_KEYMAP_KEYS - 1u);   /* every key but FN itself */
    /* per key: initial states (fn0,k0) give 6 + 3 + 3 + 2 = 14 interleavings */
    CHECK_EQ(g_seq_runs, keys * 14u);
}

/* ---------------------------------------------------------------- chatter and random mixes */

static void test_chatter(void)
{
    const unsigned fn = (unsigned)slot_of("FN"), i = (unsigned)slot_of("I"), j = (unsigned)slot_of("J");

    /* FN bouncing while I is held: I never changes; a key pressed during each FN period uses that period's layer */
    rig_reset();
    rig_press(i);
    for (unsigned k = 0; k < 50; k++) {
        rig_press(fn);
        rig_release(fn);
    }
    expect_kbd1(0x0C);
    rig_press(fn);
    rig_press(j);
    {
        const unsigned w[2] = { 0x0C, 0x50 };
        CHECK(kbd_is(w, 2));
    }
    rig_release(fn);
    rig_press(fn);          /* FN again while J (latched Left) is still down */
    rig_release(fn);
    {
        const unsigned w[2] = { 0x0C, 0x50 };
        CHECK(kbd_is(w, 2));
    }
    rig_release(i);
    expect_kbd1(0x50);
    rig_release(j);
    expect_idle();

    /* FN chatter before the key: whatever layer FN has at the instant of the press sticks */
    for (unsigned phase = 0; phase < 2; phase++) {
        rig_reset();
        for (unsigned k = 0; k < 7; k++) {
            rig_press(fn);
            rig_release(fn);
        }
        if (phase == 1) {
            rig_press(fn);
        }
        rig_press(i);
        expect_kbd1(phase == 1 ? 0x52 : 0x0C);
        for (unsigned k = 0; k < 9; k++) {
            rig_release(fn);
            rig_press(fn);
        }
        expect_kbd1(phase == 1 ? 0x52 : 0x0C);
        rig_release(i);
        rig_release(fn);
        expect_idle();
    }
}

/* random mixes of up to 8 keys against a model; after every event the report equals the model's, and releasing
 * everything always ends idle (no key can get stuck) */
static void test_random(void)
{
    uint32_t seed = 0xC0FFEEu;
    unsigned pool[KM_KEYMAP_SLOTS], np = 0;
    for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
        if (km_keymap_slot_is_key(s)) {
            pool[np++] = s;
        }
    }
    const unsigned fn = (unsigned)slot_of("FN");
    for (unsigned round = 0; round < 60; round++) {
        rig_reset();
        bool on_l1[KM_KEYMAP_SLOTS] = { false };
        for (unsigned step = 0; step < 400; step++) {
            seed = seed * 1664525u + 1013904223u;
            unsigned s = pool[(seed >> 16) % np];
            if ((seed >> 8) % 4u == 0) {
                s = fn;                                   /* FN is pressed more often than a typical key */
            }
            bool held = false;
            for (unsigned x = 0; x < g_n; x++) {
                held = held || g_down[x] == s;
            }
            if (held) {
                rig_release(s);
            } else if (g_n < 8) {
                bool fn_held = false;
                for (unsigned x = 0; x < g_n; x++) {
                    fn_held = fn_held || g_down[x] == fn;
                }
                on_l1[s] = fn_held;
                rig_press(s);
            }
            /* model: usages in press order, once each, media = last pressed */
            unsigned want[KM_KEYMAP_SLOTS], nw = 0, cons = 0;
            for (unsigned x = 0; x < g_n; x++) {
                const unsigned sl = g_down[x];
                if (sl == fn) {
                    continue;
                }
                const l1_t *e = on_l1[sl] ? l1_of(km_keymap_slot_name(sl)) : NULL;
                unsigned u = 0;
                if (e == NULL) {
                    u = km_keymap_usage(0, sl);
                } else if (e->kind == L1_KBD) {
                    u = e->value;
                } else if (e->kind == L1_CONSUMER) {
                    cons = e->value;
                }
                bool dup = false;
                for (unsigned y = 0; y < nw; y++) {
                    dup = dup || want[y] == u;
                }
                if (u != 0 && !dup) {
                    want[nw++] = u;
                }
            }
            CHECK(kbd_is(want, nw));
            CHECK_EQ(g_out.consumer, cons);
        }
        while (g_n > 0) {
            rig_release(g_down[0]);
        }
        expect_idle();
    }
}

/* ---------------------------------------------------------------- media keys, many at once */

static void test_media(void)
{
    const unsigned fn = (unsigned)slot_of("FN");
    const unsigned dot = (unsigned)slot_of("."), slash = (unsigned)slot_of("/"), comma = (unsigned)slot_of(",");
    const unsigned lb = (unsigned)slot_of("["), rb = (unsigned)slot_of("]"), m = (unsigned)slot_of("M");
    const unsigned i = (unsigned)slot_of("I");

    /* each one alone */
    static const struct { const char *name; unsigned usage; } one[] = {
        { "[", 0xEA }, { "]", 0xE9 }, { "M", 0xE2 }, { ",", 0xB6 }, { ".", 0xCD }, { "/", 0xB5 },
    };
    for (unsigned t = 0; t < 6; t++) {
        rig_reset();
        rig_press(fn);
        rig_press((unsigned)slot_of(one[t].name));
        CHECK_EQ(g_out.consumer, one[t].usage);
        CHECK_EQ(g_out.kbd_n, 0);
        rig_release((unsigned)slot_of(one[t].name));
        CHECK_EQ(g_out.consumer, 0);
        rig_release(fn);
    }
    /* several: the most recent wins, the older one comes back when the newer is released */
    rig_reset();
    rig_press(fn);
    rig_press(dot);
    CHECK_EQ(g_out.consumer, 0xCD);
    rig_press(slash);
    CHECK_EQ(g_out.consumer, 0xB5);
    rig_press(comma);
    CHECK_EQ(g_out.consumer, 0xB6);
    rig_press(i);                       /* a keyboard key alongside does not disturb the media key */
    CHECK_EQ(g_out.consumer, 0xB6);
    expect_kbd1(0x52);
    rig_release(comma);
    CHECK_EQ(g_out.consumer, 0xB5);
    rig_release(dot);                   /* releasing an older one does not change the newest */
    CHECK_EQ(g_out.consumer, 0xB5);
    rig_release(slash);
    CHECK_EQ(g_out.consumer, 0);
    expect_kbd1(0x52);
    rig_release(i);
    rig_release(fn);
    expect_idle();
    /* volume keys held and the FN released: the volume keys keep going, a new press has no FN layer */
    rig_reset();
    rig_press(fn);
    rig_press(rb);
    rig_release(fn);
    CHECK_EQ(g_out.consumer, 0xE9);
    rig_press(lb);                      /* '[' without FN is the bracket */
    CHECK_EQ(g_out.consumer, 0xE9);
    expect_kbd1(0x2F);
    rig_release(rb);
    CHECK_EQ(g_out.consumer, 0);
    rig_release(lb);
    rig_press(m);
    expect_kbd1(0x10);                  /* plain M */
    rig_release(m);
    expect_idle();
}

/* ---------------------------------------------------------------- loaded tables, no-key slots, extra layers */

static void test_custom_tables(void)
{
    uint8_t t[KM_LAYERS][KM_KEYMAP_SLOTS];

    /* a table that puts a key everywhere on every layer: slots without a key still emit nothing */
    for (unsigned l = 0; l < KM_LAYERS; l++) {
        for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
            t[l][s] = 0x04;
        }
    }
    t[0][slot_of("FN")] = KM_CODE_HOLD_L1;
    CHECK(km_layers_set_map(t));
    rig_reset();
    km_layers_set_map(t);
    unsigned nokey = 0;
    for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
        if (km_keymap_slot_is_key(s)) {
            continue;
        }
        nokey++;
        rig_press(s);
        expect_idle();
        CHECK_EQ(g_out.layer, 0);
        rig_release(s);
    }
    CHECK_EQ(nokey, KM_KEYMAP_SLOTS - KM_KEYMAP_KEYS);
    /* a no-key slot is not even remembered when a holding code is "there" */
    {
        const uint8_t all[3] = { 15, 17, 200 };
        km_layers_update(all, 3, &g_out);
        expect_idle();
    }
    /* the same usage on two keys: reported once, until both are released */
    rig_reset();
    km_layers_set_map(t);
    rig_press(1);
    rig_press(2);
    expect_kbd1(0x04);
    rig_release(1);
    expect_kbd1(0x04);
    rig_release(2);
    expect_idle();

    /* layers 2 and 3: hold codes, highest held layer wins, transparent falls back to layer 0 (not to layer 1) */
    const unsigned fn = (unsigned)slot_of("FN"), a = (unsigned)slot_of("A"), s_ = (unsigned)slot_of("S");
    const unsigned d = (unsigned)slot_of("D"), f = (unsigned)slot_of("F");
    km_keymap_default_table(t);
    t[0][s_] = KM_CODE_HOLD_L2;
    t[0][d] = KM_CODE_HOLD_L3;
    t[2][a] = 0x3A;                      /* A -> F1 on layer 2 */
    t[3][a] = 0x3B;                      /* A -> F2 on layer 3 */
    t[1][f] = 0x3C;                      /* F -> F3 on layer 1 */
    t[2][f] = KM_CODE_TRANSPARENT;       /* ... but transparent on layer 2: layer 0 'F', not layer 1's F3 */
    t[3][f] = 0x00;                      /* 0 is "no key", it does not fall through */
    CHECK(km_layers_set_map(t));
    rig_reset();
    km_layers_set_map(t);
    rig_press(s_);
    CHECK_EQ(g_out.layer, 2);
    expect_idle();
    rig_press(a);
    expect_kbd1(0x3A);
    rig_press(d);                        /* layer 3 held as well: highest wins for keys pressed from now on */
    CHECK_EQ(g_out.layer, 3);
    expect_kbd1(0x3A);                   /* A is latched on layer 2 */
    rig_release(a);
    rig_press(a);
    expect_kbd1(0x3B);
    rig_press(f);                        /* layer 3, entry 0: nothing */
    expect_kbd1(0x3B);
    rig_release(f);
    rig_release(d);
    CHECK_EQ(g_out.layer, 2);
    rig_press(f);                        /* layer 2 transparent -> layer 0 'F' (0x09) */
    {
        const unsigned w[2] = { 0x3B, 0x09 };
        CHECK(kbd_is(w, 2));
    }
    rig_release(f);
    rig_release(s_);
    CHECK_EQ(g_out.layer, 1 - 1);        /* back to layer 0 (FN is not held) */
    rig_press(fn);
    rig_press(f);
    {
        const unsigned w[2] = { 0x3B, 0x3C };   /* A still latched on layer 3; F on layer 1 = F3 */
        CHECK(kbd_is(w, 2));
    }
    rig_release(a);
    expect_kbd1(0x3C);
    rig_release(f);
    rig_release(fn);
    expect_idle();

    /* the table is swapped while a key is down: the key keeps what it was pressed with */
    km_layers_set_map(NULL);
    rig_reset();
    rig_press(a);
    expect_kbd1(0x04);
    km_keymap_default_table(t);
    t[0][a] = 0x05;
    km_layers_set_map(t);
    rig_update();
    expect_kbd1(0x04);
    rig_release(a);
    expect_idle();
    rig_press(a);
    expect_kbd1(0x05);
    rig_release(a);

    km_layers_set_map(NULL);
}

/* ---------------------------------------------------------------- end to end into the reports */

static void test_to_reports(void)
{
    rig_reset();
    const unsigned fn = (unsigned)slot_of("FN");
    rig_press(fn);
    rig_press((unsigned)slot_of("I"));
    rig_press((unsigned)slot_of("LSHIFT"));
    uint8_t b[8], n[29];
    km_report_build_boot_usages(g_out.kbd, g_out.kbd_n, b);
    CHECK_EQ(b[0], 0x02);               /* shift */
    CHECK_EQ(b[2], 0x52);               /* up arrow */
    CHECK_EQ(b[3], 0);
    km_report_build_nkro_usages(g_out.kbd, g_out.kbd_n, n);
    CHECK(((n[0x52 >> 3] >> (0x52 & 7)) & 1) == 1);
    CHECK((n[28] & 0x02) != 0);         /* LShift = usage 0xE1 */

    /* seven FN-layer keys: the boot list rolls over, the bitmap does not */
    rig_reset();
    rig_press(fn);
    static const char *const names[] = { "1", "2", "3", "4", "5", "6", "7" };
    for (unsigned k = 0; k < 7; k++) {
        rig_press((unsigned)slot_of(names[k]));
    }
    CHECK_EQ(g_out.kbd_n, 7);
    km_report_build_boot_usages(g_out.kbd, g_out.kbd_n, b);
    for (unsigned k = 2; k < 8; k++) {
        CHECK_EQ(b[k], 0x01);
    }
    km_report_build_nkro_usages(g_out.kbd, g_out.kbd_n, n);
    for (unsigned k = 0; k < 7; k++) {
        const unsigned u = 0x3Au + k;
        CHECK(((n[u >> 3] >> (u & 7)) & 1) == 1);
    }
    /* releasing FN first and then the keys: the reports go back to empty with no leftovers */
    rig_release(fn);
    km_report_build_nkro_usages(g_out.kbd, g_out.kbd_n, n);
    CHECK_EQ(g_out.kbd_n, 7);
    while (g_n > 0) {
        rig_release(g_down[0]);
    }
    km_report_build_nkro_usages(g_out.kbd, g_out.kbd_n, n);
    for (unsigned k = 0; k < 29; k++) {
        CHECK_EQ(n[k], 0);
    }
    uint8_t c[3];
    km_report_build_consumer(g_out.consumer, c);
    CHECK_EQ(c[1] | c[2], 0);
}

void test_layers(void)
{
    test_tables();
    test_validation_and_loading();
    test_reports();
    test_basic();
    test_latching();
    test_same_update();
    test_all_orders();
    test_chatter();
    test_random();
    test_media();
    test_custom_tables();
    test_to_reports();
    km_layers_set_map(NULL);
    km_layers_reset();
}
