/*
 * km_lightscene (M6 package P6b): the KMLS blob (encode / strict decode) and its storage on the two scene sectors 0x08038000 / 0x0803A000,
 * on a simulated flash that can lose power after any single erase/program step (the flash model of test_store.c, here with every access
 * also checked against km_flash_guard.h).
 *
 * Properties proven here:
 *   - encode(decode(x)) == x for every blob the encoder makes, and the size is what the documentation says (150 + 8 per rule, 214 at most);
 *   - the decoder is strict: with the checksum repaired after the change, EVERY single-byte change of a valid blob (all 255 other values at
 *     every position) is either refused or leaves a blob that is exactly what the encoder writes for what was decoded (canonical form);
 *     every shorter prefix and every blob with trailing bytes is refused; ordering, flags, unknown required sections, reserved bytes, ranges
 *     are each refused by name; optional unknown sections are skipped; missing sections mean defaults; a refused blob leaves the output alone;
 *   - after a power cut at EVERY erase/program step of a save (5 variants for the erase, 4 for a program), under 7 start states, the store
 *     loads the old scene or the new one, never anything else (first save: or none), and the next save works;
 *   - an error reported by the flash and a silent bit error never become the current scene.
 */
#include <stddef.h>

#include "km/km_flash_guard.h"
#include "km/km_lightscene.h"
#include "km/km_settings.h"
#include "km/km_store.h"
#include "km_test.h"

#define BASE KM_FLASH_SCENE_A
#define SECT 0x2000u

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

/* ---------------------------------------------------------------- scenes */

static void rand_scene(km_lightscene_t *s, uint32_t seed)
{
    uint32_t x = mix(seed * 2654435761u + 1u);
#define NEXT() (x = mix(x + 0x9E3779B9u))
    km_lightscene_default(s);
    s->n_rules = (uint8_t)(NEXT() % (KM_LS_MAX_RULES + 1u));
    for (unsigned i = 0; i < KM_LS_MAX_RULES; i++) {                 /* all eight are valid, so a test may raise n_rules afterwards */
        km_ls_rule_t *r = &s->rule[i];
        r->source = (uint8_t)(1u + NEXT() % KM_LS_SRC_LAST);
        r->param = (r->source == KM_LS_SRC_LAYER || r->source == KM_LS_SRC_PROFILE) ? (uint8_t)(NEXT() % 4u) : 0u;
        r->led = (uint8_t)(NEXT() % KM_LED_COUNT);
        const unsigned off = NEXT() % 3u;
        r->flags = (uint8_t)(off | ((NEXT() & 1u) ? KM_LS_FLAG_BLINK : 0u));
        r->on.r = (uint8_t)NEXT();
        r->on.g = (uint8_t)NEXT();
        r->on.b = (uint8_t)NEXT();
        r->dim = off == KM_LS_OFF_DIM ? (uint8_t)NEXT() : 0u;
    }
    for (unsigned i = 0; i < KM_LS_PALETTE; i++) {
        s->palette[i].r = (uint8_t)NEXT();
        s->palette[i].g = (uint8_t)NEXT();
        s->palette[i].b = (uint8_t)NEXT();
    }
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        s->key_idx[i] = (uint8_t)(NEXT() % KM_LS_PALETTE);
    }
    s->anim_flags = (uint8_t)(NEXT() & KM_LS_ANIM_MASK);
    s->anim_clip = (uint8_t)NEXT();
    s->anim_offset_ms = (int32_t)(NEXT() % (2u * KM_LS_ANIM_OFFSET_MAX + 1u)) - KM_LS_ANIM_OFFSET_MAX;
#undef NEXT
}

static void seal(uint8_t *b, unsigned len)       /* repair the checksum after a change */
{
    const uint32_t c = km_crc32(0, b, len - 4u);
    for (unsigned i = 0; i < 4; i++) {
        b[len - 4u + i] = (uint8_t)(c >> (8 * i));
    }
}

static void le16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* ---------------------------------------------------------------- the blob */

static void test_blob_basics(void)
{
    km_lightscene_t d, back;
    uint8_t buf[KM_LS_MAX_ENCODED + 8];

    km_lightscene_default(&d);
    CHECK(km_lightscene_valid(&d));
    CHECK_EQ(d.n_rules, 0);
    CHECK_EQ(d.palette[0].r, 0);
    CHECK_EQ(d.palette[0].g, 150);
    CHECK_EQ(d.palette[0].b, 255);
    CHECK_EQ(km_lightscene_encode(&d, buf, sizeof buf), 150);                 /* header 16 + 5 + 113 + 12 + 4 */
    CHECK(memcmp(buf, "KMLS", 4) == 0);
    CHECK_EQ(buf[4] | (buf[5] << 8), 1);
    CHECK_EQ(buf[6] | (buf[7] << 8), 150);
    CHECK_EQ(buf[8], 3);
    CHECK_EQ(buf[16], KM_LS_SEC_RULES);
    CHECK_EQ(buf[16 + 5], KM_LS_SEC_KEYS);
    CHECK_EQ(buf[16 + 5 + 4 + 109], KM_LS_SEC_ANIM);
    memset(&back, 0x5A, sizeof back);
    CHECK(km_lightscene_decode(&back, buf, 150));
    CHECK_EQ(back.n_rules, 0);
    CHECK(memcmp(back.palette, d.palette, sizeof d.palette) == 0);
    CHECK(memcmp(back.key_idx, d.key_idx, sizeof d.key_idx) == 0);

    /* a hand-made byte-exact reference of the default blob, independent of the encoder: the layout in km_lightscene.h */
    uint8_t ref[150];
    memset(ref, 0, sizeof ref);
    memcpy(ref, "KMLS", 4);
    ref[4] = 1; ref[6] = 150; ref[8] = 3;
    ref[16] = 1; ref[18] = 1;                                                  /* RULES: len 1 (the count), count 0 */
    ref[21] = 2; ref[23] = 109;                                                /* KEYS at 16+5: len 109 */
    ref[25 + 0] = 0; ref[25 + 1] = 150; ref[25 + 2] = 255;                     /* palette 0 */
    ref[16 + 5 + 4 + 109] = 3;                                                 /* ANIM header */
    le16(ref + 16 + 5 + 4 + 109 + 2, 8);
    seal(ref, 150);
    uint8_t enc[KM_LS_MAX_ENCODED];
    CHECK_EQ(km_lightscene_encode(&d, enc, sizeof enc), 150);
    CHECK(memcmp(enc, ref, 150) == 0);

    /* sizes and the cap */
    for (unsigned n = 0; n <= KM_LS_MAX_RULES; n++) {
        km_lightscene_t s;
        rand_scene(&s, 100u + n);
        s.n_rules = (uint8_t)n;
        CHECK(km_lightscene_valid(&s));
        const unsigned want = 150u + 8u * n;
        CHECK_EQ(km_lightscene_encode(&s, enc, sizeof enc), want);
        CHECK_EQ(km_lightscene_encode(&s, enc, want - 1u), 0);               /* buffer one too small */
        CHECK_EQ(km_lightscene_encode(&s, enc, want), want);
        CHECK(km_lightscene_decode(&back, enc, want));
        uint8_t again[KM_LS_MAX_ENCODED];
        CHECK_EQ(km_lightscene_encode(&back, again, sizeof again), want);
        CHECK(memcmp(enc, again, want) == 0);
    }
    CHECK_EQ(150u + 8u * KM_LS_MAX_RULES, KM_LS_MAX_ENCODED);
    CHECK_EQ(km_lightscene_encode(NULL, enc, sizeof enc), 0);
    CHECK_EQ(km_lightscene_encode(&d, NULL, 200), 0);

    /* random scenes round-trip exactly */
    for (unsigned k = 0; k < 400; k++) {
        km_lightscene_t s, t;
        rand_scene(&s, 1000u + k);
        CHECK(km_lightscene_valid(&s));
        const unsigned n = km_lightscene_encode(&s, enc, sizeof enc);
        CHECK(n >= 150u && n <= KM_LS_MAX_ENCODED);
        km_lightscene_default(&t);
        CHECK(km_lightscene_decode(&t, enc, n));
        CHECK_EQ(t.n_rules, s.n_rules);
        for (unsigned i = 0; i < s.n_rules; i++) {
            CHECK(memcmp(&t.rule[i], &s.rule[i], sizeof s.rule[i]) == 0);
        }
        CHECK(memcmp(t.palette, s.palette, sizeof s.palette) == 0);
        CHECK(memcmp(t.key_idx, s.key_idx, sizeof s.key_idx) == 0);
        CHECK_EQ(t.anim_flags, s.anim_flags);
        CHECK_EQ(t.anim_clip, s.anim_clip);
        CHECK_EQ(t.anim_offset_ms, s.anim_offset_ms);
    }
}

static void test_scene_validity(void)
{
    km_lightscene_t s;
    uint8_t buf[KM_LS_MAX_ENCODED];
    rand_scene(&s, 7);
    s.n_rules = 2;
    s.rule[0] = (km_ls_rule_t){ KM_LS_SRC_HOST_CAPS, 0, 28, KM_LS_OFF_DIM, { 255, 0, 0 }, 40 };
    s.rule[1] = (km_ls_rule_t){ KM_LS_SRC_LAYER, 3, 0, KM_LS_OFF_BLACK | KM_LS_FLAG_BLINK, { 0, 255, 0 }, 0 };
    CHECK(km_lightscene_valid(&s));
    CHECK(km_lightscene_encode(&s, buf, sizeof buf) != 0);

#define BAD(stmt)                                                                  \
    do {                                                                           \
        km_lightscene_t b = s;                                                     \
        stmt;                                                                      \
        CHECK(!km_lightscene_valid(&b));                                           \
        CHECK_EQ(km_lightscene_encode(&b, buf, sizeof buf), 0);                    \
    } while (0)
    {   /* a ninth rule, made valid on purpose: it overlays the first palette bytes (the rule array is followed by the palette) */
        _Static_assert(offsetof(km_lightscene_t, palette) == offsetof(km_lightscene_t, rule) + KM_LS_MAX_RULES * sizeof(km_ls_rule_t), "layout");
        km_lightscene_t b = s;
        memset(b.palette, 0, sizeof b.palette);
        b.palette[0].r = KM_LS_SRC_HOST_NUM;
        b.n_rules = KM_LS_MAX_RULES;
        CHECK(km_lightscene_valid(&b));
        b.n_rules = KM_LS_MAX_RULES + 1u;
        CHECK(!km_lightscene_valid(&b));
        CHECK_EQ(km_lightscene_encode(&b, buf, sizeof buf), 0);
    }
    BAD(b.rule[0].source = 0);
    BAD(b.rule[0].source = KM_LS_SRC_LAST + 1u);
    BAD(b.rule[0].param = 1);                                  /* Caps Lock has no parameter */
    BAD(b.rule[1].param = 4);                                  /* only layers 0..3 */
    BAD(b.rule[0].led = KM_LED_COUNT);
    BAD(b.rule[0].flags = KM_LS_OFF_DIM + 1u);                 /* off-behaviour 3 */
    BAD(b.rule[1].flags |= KM_LS_FLAG_OFF_MASK);               /* off-behaviour 3 without a dim value */
    BAD(b.rule[0].flags |= 0x08u);
    BAD(b.rule[0].flags |= 0x80u);
    BAD(b.rule[1].dim = 1);                                    /* dim only with the DIM behaviour */
    BAD(b.key_idx[0] = KM_LS_PALETTE);
    BAD(b.key_idx[KM_LED_COUNT - 1u] = 255);
    BAD(b.anim_flags = 4);
    BAD(b.anim_offset_ms = KM_LS_ANIM_OFFSET_MAX + 1);
    BAD(b.anim_offset_ms = -KM_LS_ANIM_OFFSET_MAX - 1);
#undef BAD
    {   /* the profile source takes 0..3, the other parameter-less sources only 0 */
        km_ls_rule_t r = { KM_LS_SRC_PROFILE, 3, 0, 0, { 1, 2, 3 }, 0 };
        CHECK(km_ls_rule_valid(&r));
        r.param = 4;
        CHECK(!km_ls_rule_valid(&r));
        for (unsigned src = KM_LS_SRC_HOST_NUM; src <= KM_LS_SRC_LAST; src++) {
            r = (km_ls_rule_t){ (uint8_t)src, 0, 60, 0, { 0, 0, 0 }, 0 };
            CHECK(km_ls_rule_valid(&r));
            r.led = 61;
            CHECK(!km_ls_rule_valid(&r));
        }
    }
    CHECK_EQ(s.anim_offset_ms >= -KM_LS_ANIM_OFFSET_MAX, 1);
    s.anim_offset_ms = KM_LS_ANIM_OFFSET_MAX;
    CHECK(km_lightscene_valid(&s));
    s.anim_offset_ms = -KM_LS_ANIM_OFFSET_MAX;
    CHECK(km_lightscene_valid(&s));
}

/* Append one section to a blob under construction (n sections so far in b[8]); returns the new length without the checksum. */
static unsigned add_section(uint8_t *b, unsigned len, unsigned id, unsigned flags, const uint8_t *payload, unsigned plen)
{
    b[len] = (uint8_t)id;
    b[len + 1] = (uint8_t)flags;
    le16(b + len + 2, plen);
    if (plen != 0) {
        memcpy(b + len + 4, payload, plen);
    }
    b[8]++;
    return len + 4u + plen;
}

static unsigned start_blob(uint8_t *b)
{
    memset(b, 0, 1100);
    memcpy(b, "KMLS", 4);
    b[4] = 1;
    return KM_LS_HEADER_BYTES;
}

static unsigned finish_blob(uint8_t *b, unsigned len)
{
    le16(b + 6, len + 4u);
    seal(b, len + 4u);
    return len + 4u;
}

static void test_decode_strict(void)
{
    km_lightscene_t s, out, sentinel;
    uint8_t good[KM_LS_MAX_ENCODED], b[1100];
    rand_scene(&s, 31);
    s.n_rules = 5;
    const unsigned n = km_lightscene_encode(&s, good, sizeof good);
    CHECK_EQ(n, 190);
    km_lightscene_default(&sentinel);
    sentinel.n_rules = 1;
    sentinel.rule[0] = (km_ls_rule_t){ KM_LS_SRC_PAD, 0, 5, 0, { 1, 1, 1 }, 0 };

    /* every shorter prefix and every longer blob is refused, and a refusal leaves the output alone */
    for (unsigned len = 0; len < n; len++) {
        out = sentinel;
        CHECK(!km_lightscene_decode(&out, good, len));
        CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
    }
    memcpy(b, good, n);
    b[n] = 0;
    out = sentinel;
    CHECK(!km_lightscene_decode(&out, b, n + 1));                      /* the length field says n */
    le16(b + 6, n + 1);
    seal(b, n + 1);
    CHECK(!km_lightscene_decode(&out, b, n + 1));                      /* consistent length, but a stray byte between the sections and the checksum */
    CHECK(!km_lightscene_decode(NULL, good, n));
    CHECK(!km_lightscene_decode(&out, NULL, n));

    /* every single-byte change, checksum repaired: refused, or exactly canonical */
    unsigned accepted_changes = 0, skipped_sections = 0;
    for (unsigned pos = 0; pos < n - 4u; pos++) {
        for (unsigned v = 0; v < 256; v++) {
            if (v == good[pos]) {
                continue;
            }
            memcpy(b, good, n);
            b[pos] = (uint8_t)v;
            seal(b, n);
            out = sentinel;
            if (km_lightscene_decode(&out, b, n)) {
                uint8_t re[KM_LS_MAX_ENCODED];
                if (v >= 0x80u && pos == 16u + 5u + 8u * 5u + 4u + 109u) {
                    skipped_sections++;                        /* the last section's id turned into an unknown OPTIONAL one: skipped, not canonical */
                    continue;
                }
                accepted_changes++;
                CHECK_EQ(km_lightscene_encode(&out, re, sizeof re), n);       /* what was accepted is what the encoder would write */
                CHECK(memcmp(re, b, n) == 0);
            } else {
                CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
            }
        }
    }
    CHECK_EQ(skipped_sections, 128);                               /* ... all 128 ids 0x80..0xFF (earlier sections can not: the ids would no longer increase) */
    CHECK(accepted_changes > 400);        /* colours, indices, clip, offset ... really are free data: the sweep reached them */
    for (unsigned pos = n - 4u; pos < n; pos++) {                  /* a wrong checksum byte */
        memcpy(b, good, n);
        b[pos] ^= 0x01;
        CHECK(!km_lightscene_decode(&out, b, n));
    }
    memcpy(b, good, n);                                              /* any other byte changed WITHOUT repairing the checksum */
    for (unsigned pos = 0; pos < n - 4u; pos++) {
        b[pos] ^= 0x10;
        CHECK(!km_lightscene_decode(&out, b, n));
        b[pos] ^= 0x10;
    }

    /* section structure, built by hand */
    uint8_t cnt0[1] = { 0 };
    uint8_t keys[KM_LS_KEYS_BYTES];
    uint8_t anim[8] = { 3, 7, 0, 0, 0, 0, 0, 0 };
    memset(keys, 0, sizeof keys);
    keys[0] = 10; keys[1] = 20; keys[2] = 30;                        /* palette 0 */
    keys[48] = 15;                                                   /* light 0 uses palette 15 */
    unsigned len;
    unsigned total;

    len = start_blob(b);                                             /* no sections at all = defaults */
    total = finish_blob(b, len);
    km_lightscene_default(&sentinel);
    out = sentinel;
    out.n_rules = 3;
    CHECK(km_lightscene_decode(&out, b, total));
    km_lightscene_default(&sentinel);
    CHECK(memcmp(&out, &sentinel, sizeof out) == 0);                 /* the decoder starts from the defaults, not from the old value */
    km_lightscene_default(&out);

    len = start_blob(b);                                             /* only KEYS: rules and animation stay default */
    len = add_section(b, len, KM_LS_SEC_KEYS, 0, keys, sizeof keys);
    total = finish_blob(b, len);
    CHECK(km_lightscene_decode(&out, b, total));
    CHECK_EQ(out.n_rules, 0);
    CHECK_EQ(out.palette[0].r, 10);
    CHECK_EQ(out.key_idx[0], 15);
    CHECK_EQ(out.anim_clip, 0);

    len = start_blob(b);                                             /* only ANIM */
    len = add_section(b, len, KM_LS_SEC_ANIM, 0, anim, sizeof anim);
    total = finish_blob(b, len);
    CHECK(km_lightscene_decode(&out, b, total));
    CHECK_EQ(out.anim_flags, 3);
    CHECK_EQ(out.anim_clip, 7);

    len = start_blob(b);                                             /* an optional unknown section (id >= 0x80) is skipped; ids stay increasing, so it comes after the known ones */
    len = add_section(b, len, KM_LS_SEC_RULES, 0, cnt0, 1);
    len = add_section(b, len, KM_LS_SEC_ANIM, 0, anim, sizeof anim);
    len = add_section(b, len, 0x80, 0, (const uint8_t *)"abc", 3);
    len = add_section(b, len, 0xF0, 0, NULL, 0);
    total = finish_blob(b, len);
    CHECK(km_lightscene_decode(&out, b, total));
    CHECK_EQ(out.anim_clip, 7);

    len = start_blob(b);                                             /* an unknown REQUIRED section refuses the blob */
    len = add_section(b, len, 0x04, 0, cnt0, 1);
    total = finish_blob(b, len);
    CHECK(!km_lightscene_decode(&out, b, total));
    len = start_blob(b);
    len = add_section(b, len, 0x7F, 0, NULL, 0);
    total = finish_blob(b, len);
    CHECK(!km_lightscene_decode(&out, b, total));

    len = start_blob(b);                                             /* id 0 */
    len = add_section(b, len, 0x00, 0, NULL, 0);
    total = finish_blob(b, len);
    CHECK(!km_lightscene_decode(&out, b, total));

    len = start_blob(b);                                             /* a section twice */
    len = add_section(b, len, KM_LS_SEC_ANIM, 0, anim, sizeof anim);
    len = add_section(b, len, KM_LS_SEC_ANIM, 0, anim, sizeof anim);
    total = finish_blob(b, len);
    CHECK(!km_lightscene_decode(&out, b, total));

    len = start_blob(b);                                             /* an optional section in front of a known one: ids must increase */
    len = add_section(b, len, 0x80, 0, NULL, 0);
    len = add_section(b, len, KM_LS_SEC_ANIM, 0, anim, sizeof anim);
    total = finish_blob(b, len);
    CHECK(!km_lightscene_decode(&out, b, total));

    len = start_blob(b);                                             /* wrong order */
    len = add_section(b, len, KM_LS_SEC_ANIM, 0, anim, sizeof anim);
    len = add_section(b, len, KM_LS_SEC_KEYS, 0, keys, sizeof keys);
    total = finish_blob(b, len);
    CHECK(!km_lightscene_decode(&out, b, total));

    len = start_blob(b);                                             /* section flags must be 0 */
    len = add_section(b, len, KM_LS_SEC_ANIM, 1, anim, sizeof anim);
    total = finish_blob(b, len);
    CHECK(!km_lightscene_decode(&out, b, total));

    len = start_blob(b);                                             /* section longer than the blob */
    len = add_section(b, len, KM_LS_SEC_ANIM, 0, anim, sizeof anim);
    le16(b + KM_LS_HEADER_BYTES + 2, 9);
    total = finish_blob(b, len);
    CHECK(!km_lightscene_decode(&out, b, total));

    len = start_blob(b);                                             /* wrong section sizes */
    len = add_section(b, len, KM_LS_SEC_KEYS, 0, keys, sizeof keys - 1u);
    total = finish_blob(b, len);
    CHECK(!km_lightscene_decode(&out, b, total));
    len = start_blob(b);
    len = add_section(b, len, KM_LS_SEC_ANIM, 0, anim, 7);
    total = finish_blob(b, len);
    CHECK(!km_lightscene_decode(&out, b, total));
    uint8_t cnt1[2] = { 1, 0 };                                     /* count says 1 rule, bytes for none */
    len = start_blob(b);
    len = add_section(b, len, KM_LS_SEC_RULES, 0, cnt1, 1);
    total = finish_blob(b, len);
    CHECK(!km_lightscene_decode(&out, b, total));
    uint8_t cnt9[1 + 9 * 8];
    memset(cnt9, 0, sizeof cnt9);
    cnt9[0] = 9;                                                    /* more rules than allowed, every one of them valid */
    for (unsigned i = 0; i < 9; i++) {
        cnt9[1 + 8 * i] = KM_LS_SRC_HOST_NUM;
    }
    len = start_blob(b);
    len = add_section(b, len, KM_LS_SEC_RULES, 0, cnt9, sizeof cnt9);
    total = finish_blob(b, len);
    CHECK(!km_lightscene_decode(&out, b, total));

    len = start_blob(b);                                             /* header: count 9, reserved byte, version 0/2, magic */
    total = finish_blob(b, len);
    b[8] = 9;
    seal(b, total);
    CHECK(!km_lightscene_decode(&out, b, total));
    len = start_blob(b);
    total = finish_blob(b, len);
    b[12] = 1;
    seal(b, total);
    CHECK(!km_lightscene_decode(&out, b, total));
    len = start_blob(b);
    total = finish_blob(b, len);
    b[4] = 2;
    seal(b, total);
    CHECK(!km_lightscene_decode(&out, b, total));
    len = start_blob(b);
    total = finish_blob(b, len);
    CHECK(km_lightscene_decode(&out, b, total));                    /* the plain header is fine */
    b[0] = 'X';
    seal(b, total);
    CHECK(!km_lightscene_decode(&out, b, total));

    /* the section count in the header must match what is there (more sections announced than present, fewer, ...) */
    len = start_blob(b);
    len = add_section(b, len, KM_LS_SEC_ANIM, 0, anim, sizeof anim);
    total = finish_blob(b, len);
    CHECK(km_lightscene_decode(&out, b, total));
    b[8] = 2;
    seal(b, total);
    CHECK(!km_lightscene_decode(&out, b, total));                    /* announces a second section that is not there */
    b[8] = 0;
    seal(b, total);
    CHECK(!km_lightscene_decode(&out, b, total));                    /* announces none, but bytes follow */

    /* section sizes that are one too long (the exact size is part of the format) */
    {
        uint8_t k110[KM_LS_KEYS_BYTES + 1], a9[9];
        memset(k110, 0, sizeof k110);
        memset(a9, 0, sizeof a9);
        len = start_blob(b);
        len = add_section(b, len, KM_LS_SEC_KEYS, 0, k110, sizeof k110);
        total = finish_blob(b, len);
        CHECK(!km_lightscene_decode(&out, b, total));
        len = start_blob(b);
        len = add_section(b, len, KM_LS_SEC_ANIM, 0, a9, sizeof a9);
        total = finish_blob(b, len);
        CHECK(!km_lightscene_decode(&out, b, total));
    }

    /* more than 8 sections (nine optional ones are otherwise harmless) and the 1024-byte limit */
    {
        uint8_t big[900];
        memset(big, 0x55, sizeof big);
        len = start_blob(b);
        for (unsigned i = 0; i < 8; i++) {
            len = add_section(b, len, 0x80u + i, 0, NULL, 0);
        }
        total = finish_blob(b, len);
        CHECK(km_lightscene_decode(&out, b, total));                 /* eight optional sections: fine */
        len = start_blob(b);
        for (unsigned i = 0; i < 9; i++) {
            len = add_section(b, len, 0x80u + i, 0, NULL, 0);
        }
        total = finish_blob(b, len);
        CHECK(!km_lightscene_decode(&out, b, total));                /* nine: refused */
        len = start_blob(b);
        len = add_section(b, len, 0x80, 0, big, sizeof big);         /* 16 + 4 + 900 + 4 = 924 bytes */
        total = finish_blob(b, len);
        CHECK_EQ(total, 924);
        CHECK(km_lightscene_decode(&out, b, total));
        len = start_blob(b);
        len = add_section(b, len, 0x80, 0, big, sizeof big);
        len = add_section(b, len, 0x81, 0, big, 90);
        total = finish_blob(b, len);
        CHECK_EQ(total, 1018);
        CHECK(km_lightscene_decode(&out, b, total));
        len = start_blob(b);
        len = add_section(b, len, 0x80, 0, big, sizeof big);
        len = add_section(b, len, 0x81, 0, big, 100);
        total = finish_blob(b, len);
        CHECK_EQ(total, 1028);
        CHECK(!km_lightscene_decode(&out, b, total));                /* over KM_LS_MAX_DECODE although well formed */
    }

    /* a blob over the decode limit */
    memset(b, 0, sizeof b);
    CHECK(!km_lightscene_decode(&out, b, KM_LS_MAX_DECODE + 1u));
}

/* ---------------------------------------------------------------- storage on the simulated flash */

typedef struct {
    uint8_t  mem[2 * SECT];
    bool     dead;
    int      ev;
    int      cut_ev, cut_var;
    int      fail_ev, fail_var;
    int      corrupt_ev;
    unsigned violations;
    unsigned erases[2], programs;
} sim_t;

static sim_t g_sim, g_start;
static km_store_t g_store;

static bool in_range(uint32_t addr, uint32_t len)
{
    return addr >= BASE && addr + len <= BASE + 2 * SECT && addr + len >= addr;
}

static bool sim_read(void *c, uint32_t addr, uint8_t *dst, uint32_t len)
{
    sim_t *s = c;
    if (!in_range(addr, len) || !km_flash_store_allowed(addr, len)) {
        s->violations++;
        return false;
    }
    memcpy(dst, &s->mem[addr - BASE], len);
    return true;
}

static void half_program(sim_t *s, uint32_t off, const uint8_t d[8], int variant)
{
    for (unsigned w = 0; w < 2; w++) {
        uint32_t old = 0, want = 0;
        memcpy(&old, &s->mem[off + 4 * w], 4);
        memcpy(&want, d + 4 * w, 4);
        const uint32_t clear = old & ~want;
        uint32_t done = clear;
        if (variant == 0 || (variant == 1 && w == 1) || (variant == 3 && w == 1)) {
            done = 0;
        } else if ((variant == 2 && w == 1) || (variant == 3 && w == 0)) {
            done = clear & 0x55555555u;
        }
        old &= ~done;
        memcpy(&s->mem[off + 4 * w], &old, 4);
    }
}

static bool sim_program(void *c, uint32_t addr, const uint8_t d[8])
{
    sim_t *s = c;
    if (s->dead) {
        return false;
    }
    if ((addr & 7u) != 0 || !in_range(addr, 8) || !km_flash_store_allowed(addr, 8)) {
        s->violations++;
        return false;
    }
    const uint32_t off = addr - BASE;
    for (unsigned i = 0; i < 8; i++) {
        if (s->mem[off + i] != 0xFF) {
            s->violations++;                         /* a unit is only programmed while erased */
        }
    }
    if ((addr & 8u) == 0) {                          /* the hardware refuses a unit below one already programmed in its 16-byte line */
        for (unsigned i = 0; i < 8; i++) {
            if (s->mem[off + 8 + i] != 0xFF) {
                s->violations++;
                break;
            }
        }
    }
    const int ev = s->ev++;
    s->programs++;
    if (ev == s->cut_ev) {
        half_program(s, off, d, s->cut_var);
        s->dead = true;
        return false;
    }
    if (ev == s->fail_ev) {
        half_program(s, off, d, s->fail_var);
        return false;
    }
    memcpy(&s->mem[off], d, 8);
    if (ev == s->corrupt_ev) {
        unsigned i = 0;
        while (i < 8 && d[i] == 0) {
            i++;
        }
        if (i < 8) {
            s->mem[off + i] = (uint8_t)(d[i] & (d[i] - 1u));
        } else {
            s->mem[off] = 1;
        }
    }
    return true;
}

static bool sim_erase(void *c, uint32_t addr)
{
    sim_t *s = c;
    if (s->dead) {
        return false;
    }
    if ((addr & (SECT - 1u)) != 0 || !in_range(addr, SECT) || !km_flash_store_allowed(addr, SECT)) {
        s->violations++;
        return false;
    }
    const uint32_t off = addr - BASE;
    const int ev = s->ev++;
    if (ev == s->cut_ev || ev == s->fail_ev) {
        const int v = (ev == s->cut_ev) ? s->cut_var : s->fail_var;
        for (uint32_t i = 0; i < SECT; i++) {
            const bool first = i < SECT / 2;
            switch (v) {
            case 1: if (first) s->mem[off + i] = 0xFF; break;
            case 2: s->mem[off + i] = (uint8_t)mix(addr + i); break;
            case 3: if (!first) s->mem[off + i] = 0xFF; break;
            case 4: s->mem[off + i] = first ? 0xFF : (uint8_t)mix(addr + i); break;
            default: break;
            }
        }
        if (ev == s->cut_ev) {
            s->dead = true;
        }
        return false;
    }
    memset(&s->mem[off], 0xFF, SECT);
    s->erases[(addr - BASE) / SECT]++;
    return true;
}

static void sim_blank(void)
{
    memset(&g_sim, 0, sizeof g_sim);
    memset(g_sim.mem, 0xFF, sizeof g_sim.mem);
    g_sim.cut_ev = g_sim.fail_ev = g_sim.corrupt_ev = -1;
    static const km_store_ops_t ops = { NULL, sim_read, sim_erase, sim_program };
    km_store_ops_t o = ops;
    o.ctx = &g_sim;
    km_store_init(&g_store, &o, KM_FLASH_SCENE_A, KM_FLASH_SCENE_B);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* A finished store record written straight into the memory from the documented layout (independent of km_store.c). */
static void forge(unsigned idx, uint32_t seq, const uint8_t *data, uint32_t len)
{
    uint8_t *r = &g_sim.mem[idx * SECT];
    memset(r, 0xFF, SECT);
    put32(r + 0, 0x53524D4Bu);
    put32(r + 8, seq);
    put32(r + 12, len);
    memcpy(r + 16, data, len);
    put32(r + 4, km_crc32(0, r + 8, 8 + len));
}

static void fill_junk(unsigned idx, uint32_t seed)
{
    for (uint32_t i = 0; i < SECT; i++) {
        g_sim.mem[idx * SECT + i] = (uint8_t)mix(seed + i);
    }
}

static km_lightscene_t sc[4];
static uint8_t blob[4][KM_LS_MAX_ENCODED];
static unsigned blen[4];

static void make_scenes(void)
{
    for (unsigned i = 0; i < 4; i++) {
        rand_scene(&sc[i], 900u + i * 13u);
        sc[i].n_rules = (uint8_t)(3u + i);               /* different sizes: 174 .. 198 bytes */
        blen[i] = km_lightscene_encode(&sc[i], blob[i], sizeof blob[i]);
        CHECK(blen[i] >= 174u && blen[i] <= KM_LS_MAX_ENCODED);
    }
}

typedef enum { NONE, IS_OLD, IS_NEW, IS_OTHER } which_t;

static which_t classify(const uint8_t *old, unsigned olen, const uint8_t *nw, unsigned nlen)
{
    km_lightscene_t s;
    uint8_t re[KM_LS_MAX_ENCODED];
    if (!km_lightscene_load(&g_store, &s)) {
        return NONE;
    }
    const unsigned n = km_lightscene_encode(&s, re, sizeof re);
    if (nw != NULL && n == nlen && memcmp(re, nw, n) == 0) {
        return IS_NEW;
    }
    if (old != NULL && n == olen && memcmp(re, old, n) == 0) {
        return IS_OLD;
    }
    return IS_OTHER;
}

static unsigned g_cut_points, g_fail_points;

static void cut_everywhere(const char *name, int old_i, int new_i, int third_i)
{
    const uint8_t *old = old_i < 0 ? NULL : blob[old_i];
    const unsigned olen = old_i < 0 ? 0 : blen[old_i];
    memcpy(&g_sim, &g_start, sizeof g_sim);
    CHECK(km_lightscene_save(&g_store, &sc[new_i]) == KM_STORE_OK);
    CHECK_EQ(g_sim.violations, 0);
    CHECK(classify(old, olen, blob[new_i], blen[new_i]) == IS_NEW);
    const int steps = g_sim.ev;
    CHECK(steps > 20 && steps < 60);                   /* one erase + ~25 record units + the header unit */

    unsigned bad = 0, seen_old = 0;
    for (int ev = 0; ev < steps; ev++) {
        const int nvar = (ev == 0) ? 5 : 4;
        for (int v = 0; v < nvar; v++) {
            memcpy(&g_sim, &g_start, sizeof g_sim);
            g_sim.cut_ev = ev;
            g_sim.cut_var = v;
            const km_store_result_t r = km_lightscene_save(&g_store, &sc[new_i]);
            CHECK(r != KM_STORE_OK && r != KM_STORE_UNCHANGED);
            CHECK(g_sim.dead);
            CHECK_EQ(g_sim.violations, 0);
            g_cut_points++;
            g_sim.dead = false;
            g_sim.cut_ev = -1;
            const which_t w = classify(old, olen, blob[new_i], blen[new_i]);
            if (!((w == IS_OLD) || (w == IS_NEW) || (w == NONE && old == NULL))) {
                bad++;
                printf("  FAIL %s: cut at step %d variant %d loads %d\n", name, ev, v, (int)w);
            }
            seen_old += (w == IS_OLD);
            if (km_lightscene_save(&g_store, &sc[third_i]) != KM_STORE_OK ||
                classify(NULL, 0, blob[third_i], blen[third_i]) != IS_NEW || g_sim.violations != 0) {
                bad++;
                printf("  FAIL %s: follow-up save after cut at step %d variant %d\n", name, ev, v);
            }
        }
    }
    CHECK_EQ(bad, 0);
    CHECK(seen_old > 0 || old == NULL);
}

static void fail_everywhere(const char *name, int old_i, int new_i)
{
    const uint8_t *old = old_i < 0 ? NULL : blob[old_i];
    const unsigned olen = old_i < 0 ? 0 : blen[old_i];
    memcpy(&g_sim, &g_start, sizeof g_sim);
    CHECK(km_lightscene_save(&g_store, &sc[new_i]) == KM_STORE_OK);
    const int steps = g_sim.ev;
    unsigned bad = 0;
    for (int ev = 0; ev < steps; ev++) {
        for (int mode = 0; mode < 3; mode++) {       /* 0 clean error, 1 error after half a unit, 2 silent corruption */
            memcpy(&g_sim, &g_start, sizeof g_sim);
            if (mode == 2) {
                if (ev == 0) {
                    continue;
                }
                g_sim.corrupt_ev = ev;
            } else {
                g_sim.fail_ev = ev;
                g_sim.fail_var = (mode == 0) ? 0 : (ev == 0 ? 4 : 2);
            }
            const km_store_result_t r = km_lightscene_save(&g_store, &sc[new_i]);
            const km_store_result_t want = (mode == 2) ? KM_STORE_ERR_VERIFY : (ev == 0 ? KM_STORE_ERR_ERASE : KM_STORE_ERR_PROGRAM);
            if (r != want) {
                bad++;
                printf("  FAIL %s: step %d mode %d returned %d, want %d\n", name, ev, mode, (int)r, (int)want);
            }
            g_sim.fail_ev = g_sim.corrupt_ev = -1;
            const which_t w = classify(old, olen, blob[new_i], blen[new_i]);
            if (!((w == IS_OLD) || (w == NONE && old == NULL)) || g_sim.violations != 0) {
                bad++;
                printf("  FAIL %s: step %d mode %d left %d\n", name, ev, mode, (int)w);
            }
            g_fail_points++;
        }
    }
    CHECK_EQ(bad, 0);
}

static void scenario_states(unsigned which)
{
    sim_blank();
    switch (which) {
    case 0: break;
    case 1: fill_junk(0, 11); fill_junk(1, 22); break;               /* the stock firmware's DKS / MT data */
    case 2: forge(0, 1, blob[0], blen[0]); break;
    case 3: forge(0, 7, blob[0], blen[0]); forge(1, 8, blob[1], blen[1]); break;
    case 4: forge(1, 3, blob[0], blen[0]); forge(0, 4, blob[1], blen[1]); break;
    case 5: forge(0, 0xFFFFFFFFu, blob[0], blen[0]); forge(1, 0xFFFFFFFEu, blob[1], blen[1]); break;
    case 6: forge(1, 0xFFFFFFFFu, blob[0], blen[0]); forge(0, 0xFFFFFFFEu, blob[1], blen[1]); fill_junk(0, 5); break;
    default: break;
    }
    memcpy(&g_start, &g_sim, sizeof g_sim);
}

static int scenario_old(unsigned which)
{
    static const int old[7] = { -1, -1, 0, 1, 1, 0, 0 };
    return old[which];
}

static void test_store_basics(void)
{
    km_lightscene_t s;
    uint8_t re[KM_LS_MAX_ENCODED];

    sim_blank();
    CHECK(!km_lightscene_load(&g_store, &s));                          /* erased: no scene */
    km_lightscene_default(&s);
    s.n_rules = 1;
    CHECK(!km_lightscene_load(&g_store, &s) && s.n_rules == 1);        /* untouched when nothing is there */
    for (unsigned i = 0; i < 40; i++) {                                /* alternation and wear */
        km_lightscene_t t = sc[i & 3u];
        t.anim_clip = (uint8_t)i;
        CHECK(km_lightscene_save(&g_store, &t) == KM_STORE_OK);
        CHECK(km_lightscene_load(&g_store, &s));
        uint8_t want[KM_LS_MAX_ENCODED];
        const unsigned nw = km_lightscene_encode(&t, want, sizeof want);
        CHECK_EQ(km_lightscene_encode(&s, re, sizeof re), nw);
        CHECK(memcmp(re, want, nw) == 0);
        CHECK_EQ(s.anim_clip, i);
    }
    CHECK_EQ(g_sim.erases[0], 20);
    CHECK_EQ(g_sim.erases[1], 20);
    CHECK_EQ(g_sim.violations, 0);
    const int ev = g_sim.ev;
    CHECK(km_lightscene_save(&g_store, &s) == KM_STORE_UNCHANGED);     /* the same scene again writes nothing */
    CHECK_EQ(g_sim.ev, ev);

    km_lightscene_t bad = s;
    bad.key_idx[3] = 16;                                               /* an invalid scene never reaches the flash */
    CHECK(km_lightscene_save(&g_store, &bad) == KM_STORE_ERR_ARG);
    CHECK_EQ(g_sim.ev, ev);
    CHECK(km_lightscene_save(NULL, &s) == KM_STORE_ERR_ARG);
    CHECK(km_lightscene_save(&g_store, NULL) == KM_STORE_ERR_ARG);
    CHECK(!km_lightscene_load(NULL, &s));
    CHECK(!km_lightscene_load(&g_store, NULL));

    /* a store record that is valid for the store but is not a scene: no scene (the lights keep the defaults) */
    sim_blank();
    uint8_t other[200];
    for (unsigned i = 0; i < sizeof other; i++) {
        other[i] = (uint8_t)(i * 3u);
    }
    forge(0, 1, other, sizeof other);
    CHECK(!km_lightscene_load(&g_store, &s));
    uint8_t bl[KM_LS_MAX_ENCODED];
    memcpy(bl, blob[0], blen[0]);
    bl[40] ^= 1;                                                       /* a scene with a wrong inner checksum inside a good record */
    forge(0, 2, bl, blen[0]);
    CHECK(!km_lightscene_load(&g_store, &s));
    forge(1, 3, blob[1], blen[1]);                                     /* the newest valid record wins, the damaged older one is not consulted */
    CHECK(km_lightscene_load(&g_store, &s));
    CHECK_EQ(s.rule[0].led, sc[1].rule[0].led);
    uint8_t huge[KM_STORE_MAX_PAYLOAD];                                 /* a record bigger than any scene */
    memset(huge, 0, sizeof huge);
    forge(0, 9, huge, sizeof huge);
    CHECK(!km_lightscene_load(&g_store, &s));
    CHECK_EQ(g_sim.violations, 0);

    /* wear: the first scene save over the stock data erases exactly one sector */
    scenario_states(1);
    memcpy(&g_sim, &g_start, sizeof g_sim);
    CHECK(km_lightscene_save(&g_store, &sc[2]) == KM_STORE_OK);
    CHECK_EQ(g_sim.erases[0] + g_sim.erases[1], 1);
}

void test_lightscene(void)
{
    make_scenes();
    test_blob_basics();
    test_scene_validity();
    test_decode_strict();
    test_store_basics();

    static const char *names[] = { "first save on erased flash", "first save over foreign data", "second save, record in A",
                                   "both valid, B newer", "both valid, A newer", "sequence wrap", "older record damaged" };
    g_cut_points = 0;
    g_fail_points = 0;
    for (unsigned k = 0; k < 7; k++) {
        scenario_states(k);
        cut_everywhere(names[k], scenario_old(k), 2, 3);
        scenario_states(k);
        fail_everywhere(names[k], scenario_old(k), 2);
    }
    printf("  lightscene: %u power-cut points and %u error points (every erase/program step x variants), 7 start states\n",
           g_cut_points, g_fail_points);
    CHECK(g_cut_points > 700);
}
