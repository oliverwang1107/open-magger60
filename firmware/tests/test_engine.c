/*
 * test_engine.c - host tests for the M4 task T-B code: key engine, default key map, report builders.
 *
 * Signals are synthetic (ramps, steps, noise from a fixed-seed generator) or the real endpoints from
 * core/data/slot_ranges_s2.json / slotmap_measured.json (our own measurements), played back as ramps.
 * Report bytes are written by hand from the USB HID spec and the HID Usage Tables.
 *
 * The checks run through the real depth model (km_depth_model.c) and are written against depth in mm,
 * not against fixed counts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <km/km_depth_model.h>
#include <km/km_engine.h>
#include <km/km_keymap.h>
#include <km/km_report.h>

#include "km_test.h"


/* ---------------------------------------------------------------- measured data (our own JSON) */

typedef struct {
    int      ok;
    uint16_t released[KM_ENGINE_SLOTS];  /* key slots: measured released level */
    uint16_t bottom[KM_ENGINE_SLOTS];    /* key slots: measured bottomed-out level */
    uint16_t idle[KM_ENGINE_SLOTS];      /* every slot: idle level (keys: released, others: no_key_slots idle) */
    int      is_key[KM_ENGINE_SLOTS];
    char     name[KM_ENGINE_SLOTS][12];
} data_t;

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1u);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* __FILE__ is the path the compiler was given (absolute in tools/run_tests.py): ../core/data/<name> */
static char *slurp_data(const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s", __FILE__);
    char *slash = strrchr(path, '/');
    char *bslash = strrchr(path, '\\');
    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        slash = bslash;
    }
    if (slash == NULL) {
        return NULL;
    }
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "../core/data/%s", name);
    return slurp(path);
}

/* the slot number of the object that contains position q: the last  "<digits>": {  before it */
static int slot_before(const char *begin, const char *q)
{
    for (const char *p = q; p > begin + 4; p--) {
        if (p[0] == '{' && p[-1] == ' ' && p[-2] == ':' && p[-3] == '"') {
            const char *e = p - 3;
            const char *s = e;
            while (s > begin && s[-1] >= '0' && s[-1] <= '9') {
                s--;
            }
            if (s != e && s[-1] == '"') {
                return atoi(s);
            }
        }
    }
    return -1;
}

static const data_t *data(void)
{
    static data_t d;
    static int loaded;
    if (loaded) {
        return &d;
    }
    loaded = 1;
    char *ranges = slurp_data("slot_ranges_s2.json");
    char *map = slurp_data("slotmap_measured.json");
    if (ranges == NULL || map == NULL) {
        free(ranges);
        free(map);
        return &d;
    }
    int keys = 0;
    const char *start = strstr(ranges, "\"keys\"");
    for (const char *p = start; p != NULL && (p = strstr(p, "\"released\":")) != NULL; p += 5) {
        const int s = slot_before(start, p);
        const char *b = strstr(p, "\"bottom\":");
        if (s < 0 || s >= (int)KM_ENGINE_SLOTS || b == NULL) {
            free(ranges);
            free(map);
            return &d;
        }
        d.released[s] = (uint16_t)atoi(p + 11);
        d.bottom[s] = (uint16_t)atoi(b + 9);
        d.idle[s] = d.released[s];
        d.is_key[s] = 1;
        keys++;
    }
    /* key names live in slotmap_measured.json: "slots": { "N": "NAME", ... } */
    const char *sl = strstr(map, "\"slots\"");
    int names = 0;
    if (sl != NULL) {
        const char *p = strchr(sl, '{') + 1;
        while (*p != '}') {
            if (*p == '"' && p[1] >= '0' && p[1] <= '9') {
                const int s = atoi(p + 1);
                const char *v = strstr(p, ": \"") + 3;
                size_t n = 0;
                while (*v != '"' && n < sizeof d.name[0] - 1u) {
                    if (*v == '\\') {
                        v++; /* JSON escape: the backslash key is written \\ */
                    }
                    d.name[s][n++] = *v++;
                }
                p = v;
                names++;
            }
            p++;
        }
    }
    const char *nk = strstr(map, "\"no_key_slots\"");
    for (const char *p = nk; p != NULL && (p = strstr(p, "\"idle\":")) != NULL; p += 5) {
        const int s = slot_before(nk, p);
        if (s >= 0 && s < (int)KM_ENGINE_SLOTS) {
            d.idle[s] = (uint16_t)atoi(p + 8);
        }
    }
    d.ok = (keys == 61 && names == 61);
    free(ranges);
    free(map);
    return &d;
}

/* ---------------------------------------------------------------- key map */

typedef struct {
    const char *name;
    uint8_t     usage;
} name_usage_t;

/* written separately from the table in km_keymap.c, straight from the HID Usage Tables (page 0x07) */
static const name_usage_t k_expect[] = {
    { "A", 0x04 }, { "B", 0x05 }, { "C", 0x06 }, { "D", 0x07 }, { "E", 0x08 }, { "F", 0x09 }, { "G", 0x0A },
    { "H", 0x0B }, { "I", 0x0C }, { "J", 0x0D }, { "K", 0x0E }, { "L", 0x0F }, { "M", 0x10 }, { "N", 0x11 },
    { "O", 0x12 }, { "P", 0x13 }, { "Q", 0x14 }, { "R", 0x15 }, { "S", 0x16 }, { "T", 0x17 }, { "U", 0x18 },
    { "V", 0x19 }, { "W", 0x1A }, { "X", 0x1B }, { "Y", 0x1C }, { "Z", 0x1D },
    { "1", 0x1E }, { "2", 0x1F }, { "3", 0x20 }, { "4", 0x21 }, { "5", 0x22 }, { "6", 0x23 }, { "7", 0x24 },
    { "8", 0x25 }, { "9", 0x26 }, { "0", 0x27 },
    { "ENTER", 0x28 }, { "ESC", 0x29 }, { "BACKSPACE", 0x2A }, { "TAB", 0x2B }, { "SPACE", 0x2C },
    { "-", 0x2D }, { "=", 0x2E }, { "[", 0x2F }, { "]", 0x30 }, { "\\", 0x31 }, { ";", 0x33 }, { "'", 0x34 },
    { ",", 0x36 }, { ".", 0x37 }, { "/", 0x38 }, { "CAPS", 0x39 },
    { "LCTRL", 0xE0 }, { "LSHIFT", 0xE1 }, { "LALT", 0xE2 }, { "LWIN", 0xE3 },
    { "RCTRL", 0xE4 }, { "RSHIFT", 0xE5 }, { "RALT", 0xE6 }, { "RWIN", 0xE7 },
    { "FN", 0x00 },
};

static int expect_usage(const char *name)
{
    for (size_t i = 0; i < sizeof k_expect / sizeof k_expect[0]; i++) {
        if (strcmp(k_expect[i].name, name) == 0) {
            return k_expect[i].usage;
        }
    }
    return -1;
}

static int slot_of(const char *name)
{
    for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
        if (strcmp(km_keymap_slot_name(s), name) == 0) {
            return (int)s;
        }
    }
    return -1;
}

static void test_keymap(void)
{
    const data_t *d = data();
    CHECK(d->ok);

    unsigned keys = 0;
    unsigned seen[256] = { 0 };
    for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
        const char *n = km_keymap_slot_name(s);
        if (d->is_key[s]) {
            keys++;
            CHECK(km_keymap_slot_is_key(s));
            CHECK(strcmp(n, d->name[s]) == 0);                  /* our measured slot map */
            CHECK_EQ(km_keymap_usage(0, s), expect_usage(n));   /* usage from the spec table */
            if (km_keymap_usage(0, s) != 0) {
                seen[km_keymap_usage(0, s)]++;
            }
        } else {
            CHECK(!km_keymap_slot_is_key(s));
            CHECK_EQ(km_keymap_usage(0, s), 0);
            CHECK(n[0] == '\0');
        }
    }
    CHECK_EQ(keys, KM_KEYMAP_KEYS);
    for (unsigned u = 0; u < 256; u++) {
        CHECK(seen[u] <= 1u); /* no usage twice on layer 0 */
    }
    /* Fn: a physical key with no function in this stage */
    CHECK_EQ(slot_of("FN"), 14);
    CHECK(km_keymap_slot_is_key(14));
    CHECK_EQ(km_keymap_usage(0, 14), 0);
    /* spot checks by hand: WASD, Esc, Space */
    CHECK_EQ(km_keymap_usage(0, 75), 0x1A); /* W */
    CHECK_EQ(km_keymap_usage(0, 73), 0x04); /* A */
    CHECK_EQ(km_keymap_usage(0, 72), 0x16); /* S */
    CHECK_EQ(km_keymap_usage(0, 74), 0x07); /* D */
    CHECK_EQ(km_keymap_usage(0, 18), 0x29); /* Esc */
    CHECK_EQ(km_keymap_usage(0, 63), 0x2C); /* Space */
    /* the mirror and floating slots of slotmap_measured.json */
    static const unsigned nokey[] = { 15, 17, 20, 24, 33, 35, 40, 41, 46, 50, 51, 53, 58, 69, 71 };
    for (unsigned i = 0; i < sizeof nokey / sizeof nokey[0]; i++) {
        CHECK(!km_keymap_slot_is_key(nokey[i]));
        CHECK_EQ(km_keymap_usage(0, nokey[i]), 0);
    }
    /* out of range */
    CHECK_EQ(km_keymap_usage(1, 18), 0);
    CHECK_EQ(km_keymap_usage(0, 76), 0);
    CHECK_EQ(km_keymap_usage(0, 100000), 0);
    CHECK(!km_keymap_slot_is_key(76));
    CHECK(km_keymap_slot_name(76)[0] == '\0');
    /* modifier helper */
    CHECK(!km_usage_is_modifier(0xDF));
    CHECK(km_usage_is_modifier(0xE0));
    CHECK(km_usage_is_modifier(0xE7));
    CHECK(!km_usage_is_modifier(0xE8));
}

/* ---------------------------------------------------------------- reports (golden bytes) */

static void boot_is(const uint8_t *slots, unsigned n, const char *hex)
{
    uint8_t want[8];
    uint8_t got[8];
    memset(got, 0xAA, sizeof got); /* the builder must write every byte */
    CHECK_EQ(km_hex(hex, want, sizeof want), 8);
    km_report_build_boot(slots, n, got);
    if (memcmp(got, want, 8) != 0) {
        km_test_failures++;
        printf("  FAIL boot report: got %02X %02X %02X %02X %02X %02X %02X %02X, want %s\n", got[0], got[1], got[2],
               got[3], got[4], got[5], got[6], got[7], hex);
    }
    km_test_checks++;
}

static void test_report_boot(void)
{
    CHECK_EQ(KM_REPORT_BOOT_LEN, 8);
    boot_is(NULL, 0, "0000000000000000");
    { const uint8_t s[] = { 73 };            boot_is(s, 1, "0000040000000000"); }       /* A */
    { const uint8_t s[] = { 18 };            boot_is(s, 1, "0000290000000000"); }       /* Esc */
    { const uint8_t s[] = { 63 };            boot_is(s, 1, "00002C0000000000"); }       /* Space */
    { const uint8_t s[] = { 0, 73 };         boot_is(s, 2, "0200040000000000"); }       /* LShift + A */
    { const uint8_t s[] = { 5, 18 };         boot_is(s, 2, "0100290000000000"); }       /* LCtrl + Esc */
    /* press order, not slot order: A S D vs S A */
    { const uint8_t s[] = { 73, 72, 74 };    boot_is(s, 3, "0000041607000000"); }
    { const uint8_t s[] = { 72, 73 };        boot_is(s, 2, "0000160400000000"); }
    /* a modifier in the middle of the list does not take a key position */
    { const uint8_t s[] = { 73, 0, 72 };     boot_is(s, 3, "0200041600000000"); }
    /* all eight modifiers: LCtrl(5) LShift(0) LAlt(23) LGui(59) RCtrl(68) RShift(28) RAlt(65) RGui(66) */
    { const uint8_t s[] = { 5, 0, 23, 59, 68, 28, 65, 66 }; boot_is(s, 8, "FF00000000000000"); }
    { const uint8_t s[] = { 28, 66 };        boot_is(s, 2, "A000000000000000"); }       /* RShift bit5 + RGui bit7 */
    /* exactly six keys: no rollover. Q W E R T Y = 14 1A 08 15 17 1C */
    { const uint8_t s[] = { 2, 75, 6, 42, 13, 16 }; boot_is(s, 6, "0000141A0815171C"); }
    /* six keys + two modifiers still fine */
    { const uint8_t s[] = { 2, 75, 6, 42, 13, 16, 5, 23 }; boot_is(s, 8, "0500141A0815171C"); }
    /* seven keys: ErrorRollOver in all six places, modifiers kept (also one that comes after the 7th key) */
    { const uint8_t s[] = { 2, 75, 6, 42, 13, 16, 1 };     boot_is(s, 7, "0000010101010101"); }
    { const uint8_t s[] = { 5, 2, 75, 6, 42, 13, 16, 1 };  boot_is(s, 8, "0100010101010101"); }
    { const uint8_t s[] = { 2, 75, 6, 42, 13, 16, 1, 0 };  boot_is(s, 8, "0200010101010101"); }
    /* slots without a function are ignored: Fn (14), mirror (15), floating (20), out of range (200) */
    { const uint8_t s[] = { 14, 15, 20, 200 };              boot_is(s, 4, "0000000000000000"); }
    { const uint8_t s[] = { 14, 73, 15, 76 };               boot_is(s, 4, "0000040000000000"); }
    /* ignored slots do not count towards the six-key limit */
    { const uint8_t s[] = { 14, 2, 15, 75, 6, 42, 13, 16 }; boot_is(s, 8, "0000141A0815171C"); }
    /* the same slot twice is one key */
    { const uint8_t s[] = { 73, 73 };                       boot_is(s, 2, "0000040000000000"); }
}

static void nkro_is(const uint8_t *slots, unsigned n, const char *hex)
{
    uint8_t want[29];
    uint8_t got[29];
    memset(got, 0xAA, sizeof got);
    CHECK_EQ(km_hex(hex, want, sizeof want), 29);
    km_report_build_nkro(slots, n, got);
    int same = memcmp(got, want, 29) == 0;
    if (!same) {
        printf("  FAIL nkro report:");
        for (int i = 0; i < 29; i++) {
            printf(" %02X", got[i]);
        }
        printf("\n   want %s\n", hex);
    }
    CHECK(same);
}

static void test_report_nkro(void)
{
    CHECK_EQ(KM_REPORT_NKRO_LEN, 29);
    CHECK_EQ(KM_REPORT_NKRO_BITS, 232);
    /* bit (usage & 7) of byte (usage >> 3) */
    nkro_is(NULL, 0, "00000000000000000000000000000000000000000000000000000000" "00");
    /* A = 0x04: byte 0, bit 4 -> 0x10 */
    { const uint8_t s[] = { 73 }; nkro_is(s, 1, "10" "00000000000000000000000000000000000000000000000000000000"); }
    /* Esc = 0x29: byte 5, bit 1 -> 0x02 */
    { const uint8_t s[] = { 18 }; nkro_is(s, 1, "0000000000" "02" "00000000000000000000000000000000000000000000000000"); }
    /* Space = 0x2C: byte 5, bit 4 -> 0x10 */
    { const uint8_t s[] = { 63 }; nkro_is(s, 1, "0000000000" "10" "00000000000000000000000000000000000000000000000000"); }
    /* Z = 0x1D: byte 3, bit 5 -> 0x20 */
    { const uint8_t s[] = { 38 }; nkro_is(s, 1, "000000" "20" "00000000000000000000000000000000000000000000000000000"); }
    /* LShift = 0xE1: byte 28, bit 1; RGui = 0xE7: byte 28, bit 7 */
    { const uint8_t s[] = { 0 };       nkro_is(s, 1, "00000000000000000000000000000000000000000000000000000000" "02"); }
    { const uint8_t s[] = { 66 };      nkro_is(s, 1, "00000000000000000000000000000000000000000000000000000000" "80"); }
    { const uint8_t s[] = { 5, 0, 23, 59, 68, 28, 65, 66 };
      nkro_is(s, 8, "00000000000000000000000000000000000000000000000000000000" "FF"); }
    /* A + S + D + Esc + LCtrl: byte 0 = bit4 (A) | bit 7 (0x07 D) = 0x10|0x80; S = 0x16: byte 2 bit 6; Esc byte 5 bit 1; LCtrl byte 28 bit 0 */
    { const uint8_t s[] = { 73, 72, 74, 18, 5 };
      nkro_is(s, 5, "90" "00" "40" "00" "00" "02" "00000000000000000000000000000000000000000000" "01"); }
    /* order does not matter, ignored slots do nothing */
    { const uint8_t s[] = { 14, 74, 15, 18, 72, 73, 5, 99 };
      nkro_is(s, 8, "90" "00" "40" "00" "00" "02" "00000000000000000000000000000000000000000000" "01"); }
    /* every key of the keyboard at once: 60 usages (Fn has none), no rollover limit */
    {
        uint8_t all[KM_KEYMAP_SLOTS];
        unsigned n = 0;
        for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
            if (km_keymap_slot_is_key(s)) {
                all[n++] = (uint8_t)s;
            }
        }
        CHECK_EQ(n, 61);
        uint8_t r[29];
        km_report_build_nkro(all, n, r);
        unsigned bits = 0;
        for (unsigned i = 0; i < 29; i++) {
            for (unsigned b = 0; b < 8; b++) {
                bits += (r[i] >> b) & 1u;
            }
        }
        CHECK_EQ(bits, 60);
        CHECK_EQ(r[0] & 0x0F, 0); /* usages 0..3 are never set */
        CHECK_EQ(r[28], 0xFF);
    }
}

/* ---------------------------------------------------------------- engine test rig */

#define NOISE 15

typedef struct {
    km_engine_t e;
    uint16_t    level[KM_ENGINE_SLOTS];  /* true noise-free reading of every slot */
    int         noise;                   /* +- counts of uniform noise */
    uint32_t    rng;
    uint32_t    frames;
    uint8_t     was_down[KM_ENGINE_SLOTS];
    unsigned    press_ev[KM_ENGINE_SLOTS];
    unsigned    release_ev[KM_ENGINE_SLOTS];
    uint16_t    press_depth[KM_ENGINE_SLOTS];    /* true depth (true endpoints, model) at the last press event */
    uint16_t    release_depth[KM_ENGINE_SLOTS];  /* ... at the last release event */
    uint16_t    max_down;                        /* most keys down at once */
} rig_t;

static uint32_t rnd(rig_t *r)
{
    r->rng ^= r->rng << 13;
    r->rng ^= r->rng >> 17;
    r->rng ^= r->rng << 5;
    return r->rng;
}

static km_depth_cal_t true_cal(unsigned slot)
{
    const data_t *d = data();
    km_depth_cal_t c = { d->released[slot], d->bottom[slot] };
    return c;
}

/* a reading (true endpoints of the slot) that sits at the given depth: largest v with depth(v) >= depth */
static uint16_t level_at(unsigned slot, uint16_t depth_mm100)
{
    km_depth_cal_t c = true_cal(slot);
    int lo = c.bottom;
    int hi = c.released;
    if (depth_mm100 == 0) {
        return c.released;
    }
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (km_depth_model_mm100(&c, (uint16_t)mid) >= depth_mm100) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return (uint16_t)lo;
}

static uint16_t true_depth(const rig_t *r, unsigned slot)
{
    if (!data()->is_key[slot]) {
        return 0;
    }
    km_depth_cal_t c = true_cal(slot);
    return km_depth_model_mm100(&c, r->level[slot]);
}

static void rig_frame(rig_t *r, uint16_t out[KM_ENGINE_SLOTS])
{
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        int v = r->level[s];
        if (r->noise > 0) {
            v += (int)(rnd(r) % (2u * (unsigned)r->noise + 1u)) - r->noise;
        }
        out[s] = (uint16_t)(v < 0 ? 0 : v > 4095 ? 4095 : v);
    }
}

static void rig_events(rig_t *r)
{
    unsigned down = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        const int now = km_engine_slot_down(&r->e, s);
        down += (unsigned)now;
        if (now && !r->was_down[s]) {
            r->press_ev[s]++;
            r->press_depth[s] = true_depth(r, s);
        } else if (!now && r->was_down[s]) {
            r->release_ev[s]++;
            r->release_depth[s] = true_depth(r, s);
        }
        r->was_down[s] = (uint8_t)now;
    }
    if (down > r->max_down) {
        r->max_down = (uint16_t)down;
    }
}

/* The rest of the suite lives in two include files (one translation unit, shared rig above). */
#include "test_engine_calib.inc"
#include "test_engine_rapid.inc"
#include "test_engine_budget.inc"
#include "test_engine_run.inc"
