/*
 * M6 P4: km_pad (gamepad core). Layout of the report byte by byte, curves (monotonic, bounded, exact ends), bindings, opposing and
 * diagonal keys, circle and dead zone, fault / epoch / reset release, the 1 ms cadence, and random sequences against the
 * invariants. Entry point test_pad(); registration in test_main.c / km_test.h is the integrator's job.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "km/km_engine.h"
#include "km/km_pad.h"
#include "km_test.h"

static uint16_t g_depth[KM_PAD_SLOTS];
static unsigned g_reads;

static uint16_t depth_of(void *ctx, unsigned slot)
{
    (void)ctx;
    g_reads++;
    return g_depth[slot];
}

static uint16_t depth_engine(void *ctx, unsigned slot)
{
    return km_engine_depth((const km_engine_t *)ctx, slot);   /* the callback shape the real glue uses */
}

#define AX(axis, flags, curve) KM_PAD_CODE_AXIS, (uint8_t)((axis) | (flags)), (curve)
#define BTN(n, act) KM_PAD_CODE_BTN, (n), (act)

static km_pad_key_t K(uint8_t slot, uint8_t code, uint8_t a0, uint8_t a1)
{
    km_pad_key_t k = { slot, code, a0, a1 };
    return k;
}

/* One-pad rig: a clock that advances 1 ms per step so that every step recomputes. */
typedef struct {
    km_pad_t pad;
    uint32_t ms;
    uint32_t epoch;
    bool allowed;
    km_pad_report_t out;
} rig_t;

static void rig_init(rig_t *r)
{
    memset(r, 0, sizeof *r);
    km_pad_init(&r->pad);
    r->allowed = true;
    memset(g_depth, 0, sizeof g_depth);
    km_pad_in_t in = { NULL, 0, true, 0, 0, depth_of, NULL };   /* first update after init = the re-arm pass */
    km_pad_update(&r->pad, &in, &r->out);
    r->ms = 1;
}

static void step(rig_t *r, const km_pad_key_t *keys, unsigned n)
{
    km_pad_in_t in = { keys, n, r->allowed, r->epoch, r->ms, depth_of, NULL };
    r->ms++;
    km_pad_update(&r->pad, &in, &r->out);
}

/* ------------------------------------------------------------------------------------------------ layout */

static void test_layout(void)
{
    uint8_t b[KM_PAD_REPORT_LEN];
    km_pad_report_t r;
    memset(&r, 0, sizeof r);
    km_pad_report_pack(&r, b);
    for (unsigned i = 0; i < KM_PAD_REPORT_LEN; i++) {
        CHECK_EQ(b[i], 0);
    }
    km_pad_report_released(b);
    CHECK(memcmp(b, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", KM_PAD_REPORT_LEN) == 0);
    CHECK_EQ(KM_PAD_REPORT_LEN, 15);

    /* every field in its own bytes, little endian */
    r.buttons = 0xA5C3u;
    r.hat = 6;
    r.lx = 0x1234;
    r.ly = -2;
    r.rx = 32767;
    r.ry = -32767;
    r.lt = 0x0102;
    r.rt = 32767;
    km_pad_report_pack(&r, b);
    static const uint8_t want[KM_PAD_REPORT_LEN] = { 0xC3, 0xA5, 0x06, 0x34, 0x12, 0xFE, 0xFF, 0xFF, 0x7F, 0x01, 0x80, 0x02, 0x01, 0xFF, 0x7F };
    CHECK(memcmp(b, want, KM_PAD_REPORT_LEN) == 0);

    km_pad_report_t u;
    km_pad_report_unpack(b, &u);
    CHECK(km_pad_report_equal(&r, &u));
    CHECK_EQ(u.buttons, 0xA5C3);
    CHECK_EQ(u.hat, 6);
    CHECK_EQ(u.ry, -32767);

    /* equality compares exactly the bytes; the high hat nibble is not data, an out-of-range hat packs as neutral */
    km_pad_report_t a = r, c = r;
    CHECK(km_pad_report_equal(&a, &c));
    c.rt = 32766;
    CHECK(!km_pad_report_equal(&a, &c));
    c.rt = 0x7EFF;                         /* differs in the very last byte only */
    CHECK(!km_pad_report_equal(&a, &c));
    c = r;
    c.buttons ^= 0x0001u;
    CHECK(!km_pad_report_equal(&a, &c));
    c = r;
    c.hat = 7;
    CHECK(!km_pad_report_equal(&a, &c));
    c.hat = 200;
    km_pad_report_pack(&c, b);
    CHECK_EQ(b[2], 0);
    b[2] = 0xF3;
    km_pad_report_unpack(b, &u);
    CHECK_EQ(u.hat, 3);
    b[2] = 0x0E;
    km_pad_report_unpack(b, &u);
    CHECK_EQ(u.hat, 0);
    memset(&u, 0, sizeof u);
    CHECK(km_pad_report_neutral(&u));
    u.lt = 1;
    CHECK(!km_pad_report_neutral(&u));
    memset(&u, 0, sizeof u);
    u.rt = 0x4000;                         /* last byte only */
    CHECK(!km_pad_report_neutral(&u));
}

/* ------------------------------------------------------------------------------------------------ curves */

static void test_curve_validity(void)
{
    km_pad_curve_t c = { 4, 6, KM_PAD_SHAPE_LINEAR, 0 };
    CHECK(km_pad_curve_valid(&c));
    c.start_mm100_5 = 3;
    CHECK(!km_pad_curve_valid(&c));       /* below 0.20 mm: inside the resting noise */
    c.start_mm100_5 = 66;
    c.end_mm100_5 = 68;
    CHECK(km_pad_curve_valid(&c));
    c.start_mm100_5 = 67;
    c.end_mm100_5 = 69;
    CHECK(!km_pad_curve_valid(&c));
    c = (km_pad_curve_t){ 10, 11, 0, 0 };
    CHECK(!km_pad_curve_valid(&c));       /* narrower than 0.10 mm */
    c.end_mm100_5 = 12;
    CHECK(km_pad_curve_valid(&c));
    c.end_mm100_5 = 69;
    CHECK(!km_pad_curve_valid(&c));       /* beyond the travel */
    c.end_mm100_5 = 40;
    c.shape = 4;
    CHECK(!km_pad_curve_valid(&c));
    c.shape = 3;
    c.flags = 1;
    CHECK(km_pad_curve_valid(&c));
    c.flags = 2;
    CHECK(!km_pad_curve_valid(&c));
    CHECK(!km_pad_curve_valid(NULL));
    memset(&c, 0, sizeof c);
    CHECK(!km_pad_curve_valid(&c));       /* an all-zero (unset) curve is not usable */
    for (unsigned i = 0; i < KM_PAD_CURVES; i++) {
        km_pad_default_curve(i, &c);
        CHECK(km_pad_curve_valid(&c));
    }
    km_pad_default_curve(99, &c);
    km_pad_curve_t d0;
    km_pad_default_curve(0, &d0);
    CHECK(memcmp(&c, &d0, sizeof c) == 0);

    km_pad_opts_t o;
    memset(&o, 0, sizeof o);
    CHECK(km_pad_opts_valid(&o));
    o.stick_deadzone = 50;
    o.circular = 1;
    o.smooth = 3;
    CHECK(km_pad_opts_valid(&o));
    o.stick_deadzone = 51;
    CHECK(!km_pad_opts_valid(&o));
    o.stick_deadzone = 0;
    o.circular = 2;
    CHECK(!km_pad_opts_valid(&o));
    o.circular = 0;
    o.smooth = 4;
    CHECK(!km_pad_opts_valid(&o));
    o.smooth = 0;
    o.rsvd[4] = 1;
    CHECK(!km_pad_opts_valid(&o));
    CHECK(!km_pad_opts_valid(NULL));
}

static void test_config_replaces_bad_input(void)
{
    km_pad_t p;
    km_pad_init(&p);
    km_pad_curve_t cv[4], def;
    for (unsigned i = 0; i < 4; i++) {
        km_pad_default_curve(i, &cv[i]);
    }
    cv[1] = (km_pad_curve_t){ 30, 31, 0, 0 };      /* too narrow */
    cv[2] = (km_pad_curve_t){ 8, 40, KM_PAD_SHAPE_S, 1 };
    CHECK(!km_pad_set_config(&p, cv, NULL));
    km_pad_default_curve(1, &def);
    CHECK(memcmp(&p.curve[1], &def, sizeof def) == 0);   /* replaced by the default of its own index */
    CHECK(memcmp(&p.curve[2], &cv[2], sizeof def) == 0); /* the good one is kept */
    km_pad_opts_t o;
    memset(&o, 0, sizeof o);
    o.stick_deadzone = 60;
    CHECK(!km_pad_set_config(&p, NULL, &o));
    CHECK_EQ(p.opts.stick_deadzone, 0);
    o.stick_deadzone = 30;
    o.circular = 1;
    CHECK(km_pad_set_config(&p, NULL, &o));
    CHECK_EQ(p.opts.stick_deadzone, 30);
    CHECK(km_pad_set_config(&p, NULL, NULL));
    CHECK_EQ(p.opts.stick_deadzone, 0);
    CHECK_EQ(p.opts.circular, 0);
    km_pad_default_curve(2, &def);
    CHECK(memcmp(&p.curve[2], &def, sizeof def) == 0);
}

static void test_entry_validity(void)
{
    for (unsigned a0 = 0; a0 < 256; a0++) {
        const bool want = a0 >= 1 && a0 <= 20;
        CHECK_EQ(km_pad_entry_valid(KM_PAD_CODE_BTN, (uint8_t)a0, 0), want);
    }
    CHECK(km_pad_entry_valid(KM_PAD_CODE_BTN, 3, 68));
    CHECK(!km_pad_entry_valid(KM_PAD_CODE_BTN, 3, 69));
    CHECK(km_pad_entry_valid(KM_PAD_CODE_BTN, 3, 1));
    for (unsigned a0 = 0; a0 < 256; a0++) {
        for (unsigned a1 = 0; a1 < 6; a1++) {
            const unsigned axis = a0 & 7u;
            bool want = (a0 & 0xE0u) == 0 && axis < 6 && a1 < 4;
            if (axis >= 4 && (a0 & 8u)) {
                want = false;
            }
            CHECK_EQ(km_pad_entry_valid(KM_PAD_CODE_AXIS, (uint8_t)a0, (uint8_t)a1), want);
        }
    }
    CHECK(!km_pad_entry_valid(0xED, 1, 0));
    CHECK(!km_pad_entry_valid(0x04, 0, 0));
    CHECK(!km_pad_entry_valid(0xFF, 1, 0));
}

static void test_curve_shape(void)
{
    for (unsigned shape = 0; shape <= 3; shape++) {
        for (unsigned flags = 0; flags <= 1; flags++) {
            for (unsigned s5 = 4; s5 <= 60; s5 += 7) {
                for (unsigned e5 = s5 + 2; e5 <= 68; e5 += 5) {
                    const km_pad_curve_t c = { (uint8_t)s5, (uint8_t)e5, (uint8_t)shape, (uint8_t)flags };
                    CHECK(km_pad_curve_valid(&c));
                    int32_t prev = 0;
                    for (unsigned d = 0; d <= 340; d++) {
                        const int32_t v = km_pad_curve_eval(&c, (uint16_t)d);
                        if (v < prev || v < 0 || v > 32767) {
                            CHECK(!"curve monotonic and bounded");
                        }
                        if (d <= s5 * 5u && v != 0) {
                            CHECK(!"zero up to the start");
                        }
                        if (d >= e5 * 5u && v != 32767) {
                            CHECK(!"full scale from the end");
                        }
                        prev = v;
                    }
                    CHECK_EQ(km_pad_curve_eval(&c, (uint16_t)(s5 * 5u)), 0);
                    CHECK_EQ(km_pad_curve_eval(&c, (uint16_t)(e5 * 5u)), 32767);
                    CHECK_EQ(km_pad_curve_eval(&c, 340), 32767);
                    CHECK_EQ(km_pad_curve_eval(&c, 0xFFFF), 32767);
                    CHECK_EQ(km_pad_curve_eval(&c, 0), 0);
                }
            }
        }
    }
}

static void test_curve_values(void)
{
    /* 0.20 .. 3.20 mm: width 300 */
    km_pad_curve_t lin = { 4, 64, KM_PAD_SHAPE_LINEAR, 0 };
    CHECK_EQ(km_pad_curve_eval(&lin, 20), 0);
    CHECK_EQ(km_pad_curve_eval(&lin, 21), 109);        /* 32767/300 = 109.2 */
    CHECK_EQ(km_pad_curve_eval(&lin, 170), 16384);     /* the middle */
    CHECK_EQ(km_pad_curve_eval(&lin, 319), 32657);
    CHECK_EQ(km_pad_curve_eval(&lin, 320), 32767);

    km_pad_curve_t in = lin, out = lin, s = lin, min = lin;
    in.shape = KM_PAD_SHAPE_EASE_IN;
    out.shape = KM_PAD_SHAPE_EASE_OUT;
    s.shape = KM_PAD_SHAPE_S;
    min.shape = KM_PAD_SHAPE_EASE_IN;
    min.flags = KM_PAD_CURVE_MIRROR;
    for (unsigned d = 21; d < 320; d++) {
        const int32_t l = km_pad_curve_eval(&lin, (uint16_t)d);
        CHECK(km_pad_curve_eval(&in, (uint16_t)d) <= l);      /* slow then fast: below the line */
        CHECK(km_pad_curve_eval(&out, (uint16_t)d) >= l);     /* fast then slow: above the line */
        /* mirror of "slow then fast" is "fast then slow" (same curve up to one rounding step) */
        const int32_t dm = km_pad_curve_eval(&min, (uint16_t)d) - km_pad_curve_eval(&out, (uint16_t)d);
        CHECK(dm >= -2 && dm <= 2);
        /* S is point-symmetric about the middle: v(d) + v(mirror d) = full scale, within four steps */
        const int32_t sym = km_pad_curve_eval(&s, (uint16_t)d) + km_pad_curve_eval(&s, (uint16_t)(340 - d));
        CHECK(sym >= 32767 - 4 && sym <= 32767 + 4);
    }
    CHECK_EQ(km_pad_curve_eval(&in, 170), 8192);           /* t = 1/2 -> 1/4 */
    CHECK_EQ(km_pad_curve_eval(&out, 170), 24575);         /* t = 1/2 -> 3/4 */
    CHECK_EQ(km_pad_curve_eval(&s, 170), 16384);
    CHECK_EQ(km_pad_curve_eval(&s, 95), 5120);             /* t = 1/4: 3/16 - 2/64 = 0.15625 */
    CHECK_EQ(km_pad_curve_eval(&s, 245), 27647);           /* t = 3/4: 1 - 0.15625 */
}

/* ------------------------------------------------------------------------------------------------ bindings */

#define W 5u
#define A_ 6u
#define S_ 7u
#define D_ 8u

static void test_stick_basic(void)
{
    rig_t r;
    rig_init(&r);
    /* W = LY negative (up), the engine key list holds the held pad keys */
    const km_pad_key_t w = K(W, AX(KM_PAD_AXIS_LY, KM_PAD_AXIS_NEG, 0));
    step(&r, NULL, 0);
    CHECK_EQ(r.out.ly, 0);
    CHECK(km_pad_report_neutral(&r.out));
    int16_t prev = 0;
    for (unsigned d = 0; d <= 340; d += 2) {
        g_depth[W] = (uint16_t)d;
        step(&r, &w, 1);
        CHECK(r.out.ly <= prev + 0);            /* moves towards negative only */
        CHECK(r.out.ly >= -32767);
        CHECK_EQ(r.out.lx, 0);
        CHECK_EQ(r.out.ry, 0);
        CHECK_EQ(r.out.buttons, 0);
        prev = r.out.ly;
    }
    CHECK_EQ(r.out.ly, -32767);                  /* full travel = full scale, exactly */
    g_depth[W] = 0;
    step(&r, NULL, 0);                            /* the key list is empty: released = exactly 0 */
    CHECK_EQ(r.out.ly, 0);
    CHECK(km_pad_report_neutral(&r.out));

    /* the curve index of the entry picks the curve: curve 1 is 0.20 .. 2.00 mm, curve 0 is 0.20 .. 3.20 mm */
    {
        const km_pad_key_t t1 = K(D_, AX(KM_PAD_AXIS_LT, 0, 1));
        const km_pad_key_t t0 = K(D_, AX(KM_PAD_AXIS_LT, 0, 0));
        km_pad_curve_t c0, c1;
        km_pad_default_curve(0, &c0);
        km_pad_default_curve(1, &c1);
        g_depth[D_] = 110;
        step(&r, &t1, 1);
        CHECK_EQ(r.out.lt, km_pad_curve_eval(&c1, 110));
        step(&r, &t0, 1);
        CHECK_EQ(r.out.lt, km_pad_curve_eval(&c0, 110));
        CHECK(km_pad_curve_eval(&c1, 110) != km_pad_curve_eval(&c0, 110));
        step(&r, NULL, 0);
    }
    /* the positive direction of the other axes, exact full scale, nothing else moves */
    for (unsigned axis = 0; axis < 6; axis++) {
        const km_pad_key_t k = K(D_, AX(axis, 0, 0));
        g_depth[D_] = 340;
        step(&r, &k, 1);
        step(&r, &k, 1);
        km_pad_report_t want;
        memset(&want, 0, sizeof want);
        int16_t *f[4] = { &want.lx, &want.ly, &want.rx, &want.ry };
        if (axis < 4) {
            *f[axis] = 32767;
        } else if (axis == 4) {
            want.lt = 32767;
        } else {
            want.rt = 32767;
        }
        CHECK(km_pad_report_equal(&r.out, &want));
        step(&r, NULL, 0);
        CHECK(km_pad_report_neutral(&r.out));
    }
}

static void test_opposing_keys(void)
{
    rig_t r;
    rig_init(&r);
    const km_pad_key_t k[2] = { K(A_, AX(KM_PAD_AXIS_LX, KM_PAD_AXIS_NEG, 0)), K(D_, AX(KM_PAD_AXIS_LX, 0, 0)) };
    g_depth[A_] = 340;
    g_depth[D_] = 340;
    step(&r, k, 2);
    CHECK_EQ(r.out.lx, 0);                       /* equally deep: they cancel */
    g_depth[D_] = 180;                           /* A deeper: A wins by the difference, same with the order swapped */
    step(&r, k, 2);
    const int16_t ab = r.out.lx;
    CHECK(ab < 0);
    CHECK_EQ(ab, -(32767 - km_pad_curve_eval(&(km_pad_curve_t){ 4, 64, 0, 0 }, 180)));
    const km_pad_key_t k2[2] = { k[1], k[0] };
    step(&r, k2, 2);
    CHECK_EQ(r.out.lx, ab);
    g_depth[D_] = 340;
    g_depth[A_] = 180;
    step(&r, k2, 2);
    CHECK(r.out.lx > 0);
    CHECK_EQ(r.out.lx, -ab);
    /* digital keys: both full = 0, one alone = full */
    const km_pad_key_t dk[2] = { K(A_, AX(KM_PAD_AXIS_LX, KM_PAD_AXIS_NEG | KM_PAD_AXIS_DIGITAL, 0)),
                                 K(D_, AX(KM_PAD_AXIS_LX, KM_PAD_AXIS_DIGITAL, 0)) };
    g_depth[A_] = g_depth[D_] = 0;               /* the depth is not looked at at all */
    g_reads = 0;
    step(&r, dk, 2);
    CHECK_EQ(r.out.lx, 0);
    step(&r, dk, 1);
    CHECK_EQ(r.out.lx, -32767);
    step(&r, dk + 1, 1);
    CHECK_EQ(r.out.lx, 32767);
    CHECK_EQ(g_reads, 0);
    /* two keys on the same direction: the deeper one counts */
    const km_pad_key_t sd[2] = { K(A_, AX(KM_PAD_AXIS_RT, 0, 0)), K(D_, AX(KM_PAD_AXIS_RT, 0, 0)) };
    g_depth[A_] = 100;
    g_depth[D_] = 250;
    step(&r, sd, 2);
    const int32_t lo = km_pad_curve_eval(&(km_pad_curve_t){ 4, 64, 0, 0 }, 100);
    const int32_t hi = km_pad_curve_eval(&(km_pad_curve_t){ 4, 64, 0, 0 }, 250);
    CHECK(lo < hi);
    CHECK_EQ(r.out.rt, hi);
}

static void test_diagonal_and_circle(void)
{
    const km_pad_key_t k[2] = { K(W, AX(KM_PAD_AXIS_LY, KM_PAD_AXIS_NEG, 0)), K(D_, AX(KM_PAD_AXIS_LX, 0, 0)) };
    rig_t r;
    rig_init(&r);
    g_depth[W] = g_depth[D_] = 340;
    step(&r, k, 2);
    CHECK_EQ(r.out.lx, 32767);                   /* square: both corners full */
    CHECK_EQ(r.out.ly, -32767);

    km_pad_opts_t o;
    memset(&o, 0, sizeof o);
    o.circular = 1;
    CHECK(km_pad_set_config(&r.pad, NULL, &o));
    step(&r, k, 2);                               /* the configuration change released everything; the keys are held over */
    CHECK(km_pad_report_neutral(&r.out));
    step(&r, NULL, 0);
    step(&r, k, 2);
    const int32_t x = r.out.lx, y = r.out.ly;
    CHECK(x * x + y * y <= 32767 * 32767);       /* inside the unit circle, exactly */
    CHECK(x > 23000 && x < 23200);               /* about 32767 / sqrt(2) */
    CHECK_EQ(x, -y);
    /* one key alone is not touched */
    step(&r, k, 1);
    CHECK_EQ(r.out.ly, -32767);
    CHECK_EQ(r.out.lx, 0);
    /* and a short deflection is not stretched to the rim */
    g_depth[W] = 170;
    step(&r, k, 1);
    CHECK_EQ(r.out.ly, -16384);
    g_depth[D_] = 170;
    step(&r, k, 2);
    CHECK_EQ(r.out.lx, 16384);                         /* (0.5, 0.5) is inside the circle: left alone */
    CHECK_EQ(r.out.ly, -16384);
    g_depth[W] = g_depth[D_] = 340;
    /* every direction of the circle on a coarse grid of depths, never outside */
    for (unsigned dw = 0; dw <= 340; dw += 17) {
        for (unsigned dd = 0; dd <= 340; dd += 17) {
            g_depth[W] = (uint16_t)dw;
            g_depth[D_] = (uint16_t)dd;
            step(&r, k, 2);
            const int32_t xx = r.out.lx, yy = r.out.ly;
            CHECK(xx * xx + yy * yy <= 32767 * 32767);
        }
    }
    /* right stick independent of the left one */
    const km_pad_key_t rk[2] = { K(W, AX(KM_PAD_AXIS_RY, KM_PAD_AXIS_NEG, 0)), K(D_, AX(KM_PAD_AXIS_RX, 0, 0)) };
    g_depth[W] = g_depth[D_] = 340;
    step(&r, rk, 2);
    CHECK(r.out.rx > 23000 && r.out.ry < -23000);
    CHECK_EQ(r.out.lx, 0);
    CHECK_EQ(r.out.ly, 0);
}

static void test_deadzone(void)
{
    rig_t r;
    rig_init(&r);
    km_pad_opts_t o;
    memset(&o, 0, sizeof o);
    o.stick_deadzone = 20;
    km_pad_curve_t cv[4];
    for (unsigned i = 0; i < 4; i++) {
        km_pad_default_curve(i, &cv[i]);
    }
    CHECK(km_pad_set_config(&r.pad, cv, &o));
    step(&r, NULL, 0);
    const km_pad_key_t k = K(D_, AX(KM_PAD_AXIS_LX, 0, 0));
    int32_t prev = 0;
    bool left_zero = false;
    for (unsigned d = 0; d <= 340; d++) {
        g_depth[D_] = (uint16_t)d;
        step(&r, &k, 1);
        CHECK(r.out.lx >= 0);
        if (r.out.lx == 0) {
            left_zero = true;
        }
        CHECK(r.out.lx >= prev);
        prev = r.out.lx;
    }
    CHECK(left_zero);
    CHECK_EQ(r.out.lx, 32767);                   /* the rest of the range is rescaled to full */
    /* inside the dead zone (20 % of 32767 = 6553): exactly 0 */
    g_depth[D_] = 20 + 300 * 15 / 100;           /* 15 % of the curve */
    step(&r, &k, 1);
    CHECK_EQ(r.out.lx, 0);
    /* the dead zone is radial: a diagonal that is short in both axes is 0 as well */
    const km_pad_key_t k2[2] = { K(W, AX(KM_PAD_AXIS_LY, KM_PAD_AXIS_NEG, 0)), K(D_, AX(KM_PAD_AXIS_LX, 0, 0)) };
    g_depth[W] = g_depth[D_] = (uint16_t)(20 + 300 * 14 / 100);
    step(&r, k2, 2);
    CHECK_EQ(r.out.lx, 0);
    CHECK_EQ(r.out.ly, 0);
    /* between the edge and full scale the value is rescaled: (16384 - 6553) / (32767 - 6553) of full scale */
    g_depth[D_] = 170;
    step(&r, &k, 1);
    CHECK_EQ(r.out.lx, 12288);
    /* maximum dead zone still reaches full scale */
    o.stick_deadzone = 50;
    CHECK(km_pad_set_config(&r.pad, cv, &o));
    step(&r, NULL, 0);
    g_depth[D_] = 340;
    step(&r, &k, 1);
    CHECK_EQ(r.out.lx, 32767);
}

static void test_buttons_and_hat(void)
{
    rig_t r;
    rig_init(&r);
    for (unsigned b = 1; b <= 16; b++) {
        const km_pad_key_t k = K(W, BTN((uint8_t)b, 0));
        step(&r, &k, 1);
        CHECK_EQ(r.out.buttons, 1u << (b - 1u));
        CHECK_EQ(r.out.hat, 0);
        step(&r, NULL, 0);
        CHECK_EQ(r.out.buttons, 0);
    }
    /* several at once */
    const km_pad_key_t m[3] = { K(1, BTN(1, 0)), K(2, BTN(16, 0)), K(3, BTN(9, 0)) };
    step(&r, m, 3);
    CHECK_EQ(r.out.buttons, 0x8101);
    /* the same button on two keys */
    const km_pad_key_t dup[2] = { K(1, BTN(5, 0)), K(2, BTN(5, 0)) };
    step(&r, dup, 2);
    CHECK_EQ(r.out.buttons, 0x0010);
    step(&r, dup, 1);
    CHECK_EQ(r.out.buttons, 0x0010);
    /* hat: all 16 combinations of up/down/left/right */
    static const uint8_t want[16] = {
        /* bit0 up, bit1 down, bit2 left, bit3 right */
        0, 1, 5, 0,       /* none, up, down, up+down */
        7, 8, 6, 7,       /* left, up+left, down+left, up+down+left */
        3, 2, 4, 3,       /* right, up+right, down+right, up+down+right */
        0, 1, 5, 0        /* left+right, + up, + down, all four */
    };
    for (unsigned bits = 0; bits < 16; bits++) {
        km_pad_key_t k[4];
        unsigned n = 0;
        for (unsigned i = 0; i < 4; i++) {
            if (bits & (1u << i)) {
                k[n] = K((uint8_t)(10 + i), BTN((uint8_t)(KM_PAD_BTN_HAT_UP + i), 0));
                n++;
            }
        }
        step(&r, k, n);
        CHECK_EQ(r.out.hat, want[bits]);
        CHECK_EQ(r.out.buttons, 0);
    }
}

static void test_depth_button(void)
{
    rig_t r;
    rig_init(&r);
    const km_pad_key_t k = K(W, BTN(2, 40));      /* from 2.00 mm */
    g_depth[W] = 199;
    step(&r, &k, 1);
    CHECK_EQ(r.out.buttons, 0);
    g_depth[W] = 200;
    step(&r, &k, 1);
    CHECK_EQ(r.out.buttons, 2);
    g_depth[W] = 181;                             /* inside the hysteresis band (20): stays on */
    step(&r, &k, 1);
    CHECK_EQ(r.out.buttons, 2);
    g_depth[W] = 180;
    step(&r, &k, 1);
    CHECK_EQ(r.out.buttons, 2);
    g_depth[W] = 179;
    step(&r, &k, 1);
    CHECK_EQ(r.out.buttons, 0);
    g_depth[W] = 199;                             /* and does not come back below the activation point */
    step(&r, &k, 1);
    CHECK_EQ(r.out.buttons, 0);
    g_depth[W] = 340;
    step(&r, &k, 1);
    CHECK_EQ(r.out.buttons, 2);
    step(&r, NULL, 0);                            /* key released: off at once, and the memory is gone */
    CHECK_EQ(r.out.buttons, 0);
    g_depth[W] = 190;
    step(&r, &k, 1);
    CHECK_EQ(r.out.buttons, 0);
    /* an activation point below 0.20 mm is raised to 0.20 mm (noise) */
    const km_pad_key_t lo = K(W, BTN(3, 1));
    g_depth[W] = 19;
    step(&r, &lo, 1);
    CHECK_EQ(r.out.buttons, 0);
    g_depth[W] = 20;
    step(&r, &lo, 1);
    CHECK_EQ(r.out.buttons, 4);
    g_depth[W] = 10;                              /* hysteresis 10 for a 20 activation */
    step(&r, &lo, 1);
    CHECK_EQ(r.out.buttons, 4);
    g_depth[W] = 9;
    step(&r, &lo, 1);
    CHECK_EQ(r.out.buttons, 0);
    /* a depth-activated hat direction */
    const km_pad_key_t hk = K(W, BTN(KM_PAD_BTN_HAT_UP, 60));
    g_depth[W] = 299;
    step(&r, &hk, 1);
    CHECK_EQ(r.out.hat, 0);
    g_depth[W] = 300;
    step(&r, &hk, 1);
    CHECK_EQ(r.out.hat, 1);
    /* engine key list without a depth callback: depth reads as 0, nothing activates, nothing crashes */
    km_pad_in_t in = { &k, 1, true, 0, r.ms + 5, NULL, NULL };
    km_pad_update(&r.pad, &in, &r.out);
    CHECK_EQ(r.out.buttons, 0);
}

/* ------------------------------------------------------------------------------------------------ release rules */

static void test_fault_epoch_reset(void)
{
    rig_t r;
    rig_init(&r);
    const km_pad_key_t k[3] = { K(W, AX(KM_PAD_AXIS_LY, KM_PAD_AXIS_NEG, 0)), K(D_, AX(KM_PAD_AXIS_RT, 0, 1)), K(1, BTN(1, 0)) };
    g_depth[W] = g_depth[D_] = 340;
    step(&r, k, 3);
    CHECK_EQ(r.out.ly, -32767);
    CHECK_EQ(r.out.rt, 32767);
    CHECK_EQ(r.out.buttons, 1);

    /* a fault: everything neutral now, the keys stay neutral when the fault ends while they are still held */
    r.allowed = false;
    step(&r, k, 3);
    CHECK(km_pad_report_neutral(&r.out));
    step(&r, k, 3);
    CHECK(km_pad_report_neutral(&r.out));
    r.allowed = true;
    step(&r, k, 3);
    CHECK(km_pad_report_neutral(&r.out));        /* held over */
    step(&r, k, 3);
    CHECK(km_pad_report_neutral(&r.out));
    km_pad_key_t only_d[1] = { k[1] };           /* W and the button are let go, D is still held: only they come back */
    step(&r, only_d, 1);
    CHECK(km_pad_report_neutral(&r.out));
    step(&r, k, 3);                               /* pressed again: W and the button work, D has not been released */
    CHECK_EQ(r.out.ly, -32767);
    CHECK_EQ(r.out.buttons, 1);
    CHECK_EQ(r.out.rt, 0);
    step(&r, k, 2);                               /* D released and pressed again: it works */
    step(&r, k, 1);
    step(&r, k, 2);
    CHECK_EQ(r.out.rt, 32767);
    CHECK_EQ(r.pad.faults, 1);

    /* an epoch change (table / profile / layer) does the same */
    r.epoch = 7;
    step(&r, k, 3);
    CHECK(km_pad_report_neutral(&r.out));
    step(&r, k, 3);
    CHECK(km_pad_report_neutral(&r.out));
    step(&r, NULL, 0);
    step(&r, k, 3);
    CHECK_EQ(r.out.ly, -32767);
    CHECK_EQ(r.out.buttons, 1);

    /* km_pad_reset() and set_config() */
    km_pad_reset(&r.pad);
    CHECK(km_pad_report_neutral(&r.pad.cur));            /* neutral at once, not only at the next pass */
    step(&r, k, 3);
    CHECK(km_pad_report_neutral(&r.out));
    step(&r, NULL, 0);
    step(&r, k, 3);
    CHECK_EQ(r.out.ly, -32767);
    CHECK(km_pad_set_config(&r.pad, NULL, NULL));
    step(&r, k, 3);
    CHECK(km_pad_report_neutral(&r.out));

    /* a fault with an empty key list is also fine and counts once per fault */
    const uint32_t f = r.pad.faults;
    r.allowed = false;
    step(&r, NULL, 0);
    step(&r, NULL, 0);
    CHECK_EQ(r.pad.faults, f + 1);
    r.allowed = true;
    step(&r, NULL, 0);
    step(&r, k, 3);                               /* nothing was held across the fault: works at once */
    CHECK_EQ(r.out.ly, -32767);
}

static void test_start_up(void)
{
    /* after init a key that is already down at the first pass is held over (power-up with a key down, USB re-plug) */
    km_pad_t p;
    km_pad_init(&p);
    km_pad_report_t out;
    memset(g_depth, 0, sizeof g_depth);
    g_depth[W] = 340;
    const km_pad_key_t k = K(W, AX(KM_PAD_AXIS_RX, 0, 0));
    km_pad_in_t in = { &k, 1, true, 0, 10, depth_of, NULL };
    km_pad_update(&p, &in, &out);
    CHECK(km_pad_report_neutral(&out));
    in.now_ms = 11;
    km_pad_update(&p, &in, &out);
    CHECK(km_pad_report_neutral(&out));
    in.n = 0;
    in.now_ms = 12;
    km_pad_update(&p, &in, &out);
    in.n = 1;
    in.now_ms = 13;
    km_pad_update(&p, &in, &out);
    CHECK_EQ(out.rx, 32767);
}

static void test_cadence(void)
{
    km_pad_t p;
    km_pad_init(&p);
    km_pad_report_t out;
    const km_pad_key_t k = K(W, AX(KM_PAD_AXIS_RX, 0, 0));
    memset(g_depth, 0, sizeof g_depth);
    km_pad_in_t in = { NULL, 0, true, 0, 1000, depth_of, NULL };
    CHECK(km_pad_update(&p, &in, &out));         /* the first pass after init arms and returns */
    in.keys = &k;
    in.n = 1;
    in.now_ms = 1001;
    g_depth[W] = 200;
    CHECK(km_pad_update(&p, &in, &out));
    const int16_t v1 = out.rx;
    CHECK(v1 > 0);
    g_depth[W] = 330;
    g_reads = 0;
    in.now_ms = 1001;                             /* same millisecond: nothing is recomputed */
    CHECK(!km_pad_update(&p, &in, &out));
    CHECK_EQ(out.rx, v1);
    CHECK_EQ(g_reads, 0);
    in.now_ms = 1002;
    CHECK(km_pad_update(&p, &in, &out));
    CHECK_EQ(out.rx, 32767);
    /* a fault is never delayed, the counter wraps */
    in.now_ms = 1002;
    in.allowed = false;
    CHECK(km_pad_update(&p, &in, &out));
    CHECK(km_pad_report_neutral(&out));
    in.allowed = true;
    in.keys = NULL;
    in.n = 0;
    in.now_ms = 0xFFFFFFFFu;
    CHECK(km_pad_update(&p, &in, &out));         /* the pass that arms after the fault */
    in.keys = &k;
    in.n = 1;
    in.now_ms = 0;                                /* wrapped: one millisecond later */
    CHECK(km_pad_update(&p, &in, &out));
    CHECK_EQ(out.rx, 32767);
    in.now_ms = 0;
    CHECK(!km_pad_update(&p, &in, &out));
    /* a pass that follows a fault is never skipped by the cadence, even within the same millisecond */
    in.now_ms = 50;
    CHECK(km_pad_update(&p, &in, &out));
    in.allowed = false;
    CHECK(km_pad_update(&p, &in, &out));
    in.allowed = true;
    CHECK(km_pad_update(&p, &in, &out));              /* the arming pass, same millisecond */
}

/* State that must not survive a fault / a table change: the output band and the depth-activated button memory. */
static void test_stale_memory(void)
{
    rig_t r;
    rig_init(&r);
    const km_pad_key_t a = K(A_, AX(KM_PAD_AXIS_LX, 0, 0));
    const km_pad_key_t b = K(D_, AX(KM_PAD_AXIS_LX, 0, 0));
    km_pad_curve_t c0;
    km_pad_default_curve(0, &c0);
    g_depth[A_] = 150;
    step(&r, &a, 1);
    const int32_t v1 = km_pad_curve_eval(&c0, 150);
    CHECK_EQ(r.out.lx, v1);
    r.allowed = false;
    step(&r, &a, 1);
    r.allowed = true;
    step(&r, NULL, 0);                                   /* arming pass; the key was let go */
    g_depth[D_] = 150;
    step(&r, &b, 1);
    g_depth[D_] = 151;                                   /* 109 units further: well past the band */
    step(&r, &b, 1);
    CHECK_EQ(r.out.lx, km_pad_curve_eval(&c0, 151));
    /* a fresh key right after a fault whose value is within the band of the old one: reported exactly, not the old value */
    rig_init(&r);
    km_pad_curve_t cv[4];
    for (unsigned i = 0; i < 4; i++) {
        km_pad_default_curve(i, &cv[i]);
    }
    cv[0].shape = KM_PAD_SHAPE_EASE_IN;
    CHECK(km_pad_set_config(&r.pad, cv, NULL));
    step(&r, NULL, 0);
    memset(g_depth, 0, sizeof g_depth);
    g_depth[A_] = 60;
    step(&r, &a, 1);                                     /* ease-in around 0.40 mm: a value in the hundreds */
    const int32_t old_v = r.out.lx;
    CHECK(old_v > 100);
    r.allowed = false;
    step(&r, &a, 1);
    r.allowed = true;
    step(&r, &a, 1);                                     /* arming pass, a is held over */
    g_depth[D_] = 61;                                    /* step of a few tens of units against the old value */
    step(&r, &b, 2 - 1);
    CHECK_EQ(r.out.lx, km_pad_curve_eval(&cv[0], 61));
    CHECK(r.out.lx != old_v);

    /* depth-activated button: the on/off memory is dropped while the entry is a plain engine-state button */
    rig_init(&r);
    const km_pad_key_t deep = K(W, BTN(2, 40));
    const km_pad_key_t plain = K(W, BTN(2, 0));
    g_depth[W] = 340;
    step(&r, &deep, 1);
    CHECK_EQ(r.out.buttons, 2);
    step(&r, &plain, 1);
    CHECK_EQ(r.out.buttons, 2);
    g_depth[W] = 190;                                    /* between the release point (180) and the activation point (200) */
    step(&r, &deep, 1);
    CHECK_EQ(r.out.buttons, 0);                          /* a stale "on" would keep it on */
}

static void test_ignored_and_limits(void)
{
    rig_t r;
    rig_init(&r);
    g_depth[W] = 340;
    const km_pad_key_t bad[6] = {
        K(W, AX(6, 0, 0)),                        /* axis 6 */
        K(W, AX(KM_PAD_AXIS_LT, KM_PAD_AXIS_NEG, 0)), /* a negative trigger */
        K(W, AX(KM_PAD_AXIS_LX, 0, 4)),           /* curve 4 */
        K(W, BTN(0, 0)),                          /* button 0 */
        K(KM_PAD_SLOTS, BTN(1, 0)),               /* slot 76 */
        K(W, 0x04, 0, 0),                         /* not a pad code */
    };
    step(&r, bad, 6);
    CHECK(km_pad_report_neutral(&r.out));
    CHECK_EQ(r.pad.ignored_keys, 6);
    /* more than KM_PAD_MAX_KEYS entries: the extra ones are ignored, not read out of bounds */
    km_pad_key_t many[KM_PAD_MAX_KEYS + 8];
    for (unsigned i = 0; i < KM_PAD_MAX_KEYS + 8; i++) {
        many[i] = K((uint8_t)i, BTN((uint8_t)(1 + i % 16), 0));
    }
    step(&r, many, KM_PAD_MAX_KEYS + 8);
    CHECK_EQ(r.out.buttons, 0xFFFF);
    CHECK_EQ(r.pad.ignored_keys, 6 + 8);
    /* a key listed twice with different meaning: both count (OR / deeper), nothing breaks */
    const km_pad_key_t twice[2] = { K(W, BTN(1, 0)), K(W, BTN(2, 0)) };
    step(&r, NULL, 0);
    step(&r, twice, 2);
    CHECK_EQ(r.out.buttons, 3);
}

static void test_output_hysteresis(void)
{
    rig_t r;
    rig_init(&r);
    const km_pad_key_t k = K(D_, AX(KM_PAD_AXIS_LX, 0, 0));
    g_depth[D_] = 150;
    step(&r, &k, 1);
    const int16_t base = r.out.lx;
    CHECK(base > 0);
    /* a 0.01 mm step is about 100 output units, above the band: every move comes through */
    int changes = 0;
    int16_t last = r.out.lx;
    for (unsigned d = 150; d < 160; d++) {
        g_depth[D_] = (uint16_t)d;
        step(&r, &k, 1);
        if (r.out.lx != last) {
            changes++;
            CHECK(r.out.lx - last >= KM_PAD_OUT_HYST);
        }
        last = r.out.lx;
    }
    CHECK(changes > 0);
    /* a curve whose slope is small enough that single steps stay inside the band */
    km_pad_curve_t cv[4];
    for (unsigned i = 0; i < 4; i++) {
        km_pad_default_curve(i, &cv[i]);
    }
    cv[0].shape = KM_PAD_SHAPE_EASE_IN;           /* flat near the start: tiny output steps */
    CHECK(km_pad_set_config(&r.pad, cv, NULL));
    step(&r, NULL, 0);
    g_depth[D_] = 22;
    step(&r, &k, 1);
    const int16_t held = r.out.lx;
    int distinct = 1;
    int32_t last_raw = km_pad_curve_eval(&cv[0], 22);
    for (unsigned d = 22; d < 33; d++) {                       /* the curve itself moves (about 5 units per step) ... */
        g_depth[D_] = (uint16_t)d;
        step(&r, &k, 1);
        const int32_t raw = km_pad_curve_eval(&cv[0], (uint16_t)d);
        if (raw != last_raw) {
            distinct++;
        }
        last_raw = raw;
        CHECK_EQ(r.out.lx, held);                                  /* ... the report does not */
    }
    CHECK(distinct > 5);
    CHECK(last_raw < held + KM_PAD_OUT_HYST);
    g_depth[D_] = 40;                                          /* far enough: follows */
    step(&r, &k, 1);
    CHECK(r.out.lx > held);
    /* zero and full scale are never held back */
    g_depth[D_] = 340;
    step(&r, &k, 1);
    CHECK_EQ(r.out.lx, 32767);
    step(&r, NULL, 0);
    CHECK_EQ(r.out.lx, 0);
}

static void test_band_edges(void)
{
    km_pad_curve_t cv[4];
    for (unsigned i = 0; i < 4; i++) {
        km_pad_default_curve(i, &cv[i]);
    }
    /* find two depths whose curve values differ by exactly 63 / exactly 64 units */
    unsigned d63a = 0, d63b = 0, d64a = 0, d64b = 0;
    for (unsigned d1 = 20; d1 < 200 && !(d63b && d64b); d1++) {
        for (unsigned d2 = d1 + 1; d2 < d1 + 5; d2++) {
            const int32_t diff = km_pad_curve_eval(&cv[2], (uint16_t)d2) - km_pad_curve_eval(&cv[2], (uint16_t)d1);
            if (km_pad_curve_eval(&cv[2], (uint16_t)d1) > 0 && diff == KM_PAD_OUT_HYST - 1 && !d63b) {
                d63a = d1;
                d63b = d2;
            }
            if (km_pad_curve_eval(&cv[2], (uint16_t)d1) > 0 && diff == KM_PAD_OUT_HYST && !d64b) {
                d64a = d1;
                d64b = d2;
            }
        }
    }
    CHECK(d63b != 0 && d64b != 0);
    rig_t r;
    rig_init(&r);
    const km_pad_key_t k = K(D_, AX(KM_PAD_AXIS_RT, 0, 2));
    for (unsigned pass = 0; pass < 2; pass++) {
        const unsigned da = pass ? d64a : d63a, db = pass ? d64b : d63b;
        step(&r, NULL, 0);
        g_depth[D_] = (uint16_t)da;
        step(&r, &k, 1);
        const int32_t first = r.out.rt;
        CHECK_EQ(first, km_pad_curve_eval(&cv[2], (uint16_t)da));
        g_depth[D_] = (uint16_t)db;
        step(&r, &k, 1);
        if (pass == 0) {
            CHECK_EQ(r.out.rt, first);                         /* 63 units: held back (also for triggers) */
        } else {
            CHECK_EQ(r.out.rt, km_pad_curve_eval(&cv[2], (uint16_t)db));   /* 64 units: followed */
        }
    }

    /* the left trigger has the same band */
    rig_init(&r);
    const km_pad_key_t lk = K(D_, AX(KM_PAD_AXIS_LT, 0, 2));
    g_depth[D_] = (uint16_t)d63a;
    step(&r, &lk, 1);
    const int32_t lt_first = r.out.lt;
    CHECK_EQ(lt_first, km_pad_curve_eval(&cv[2], (uint16_t)d63a));
    g_depth[D_] = (uint16_t)d63b;
    step(&r, &lk, 1);
    CHECK_EQ(r.out.lt, lt_first);
    g_depth[D_] = (uint16_t)d64b;
    step(&r, &lk, 1);
    CHECK_EQ(r.out.lt, km_pad_curve_eval(&cv[2], (uint16_t)d64b));

    /* a change of sign is taken at once even when it is smaller than the band */
    rig_init(&r);
    const km_pad_key_t ka = K(A_, AX(KM_PAD_AXIS_LX, KM_PAD_AXIS_NEG, 2));
    const km_pad_key_t kd = K(D_, AX(KM_PAD_AXIS_LX, 0, 2));
    memset(g_depth, 0, sizeof g_depth);
    g_depth[D_] = 28;
    g_depth[A_] = 28;
    step(&r, &kd, 1);
    const int32_t pos = r.out.lx;
    CHECK(pos > 0 && pos < 32);
    step(&r, &ka, 1);
    CHECK_EQ(r.out.lx, -pos);
}

static void test_engine_link(void)
{
    /* the numbers the integrator gives a pad key in the key engine are a valid engine configuration */
    km_engine_key_cfg_t cfg;
    km_engine_key_cfg_default(&cfg);
    cfg.mode = KM_ENGINE_TRIG_FIXED;
    cfg.press_mm100 = KM_PAD_ENGINE_PRESS_MM100;
    cfg.release_mm100 = KM_PAD_ENGINE_RELEASE_MM100;
    CHECK(km_engine_cfg_valid(&cfg));
    CHECK_EQ(KM_PAD_SLOTS, KM_ENGINE_SLOTS);
    CHECK(KM_PAD_MIN_START_MM100 >= KM_ENGINE_RT_MIN_MM100);   /* same noise floor as every rapid-trigger distance */

    /* the real callback shape: an engine that has not been calibrated reads 0 everywhere, so the pad is neutral */
    km_engine_t e;
    km_engine_init(&e);
    km_pad_t p;
    km_pad_init(&p);
    km_pad_report_t out;
    const km_pad_key_t k = K(W, AX(KM_PAD_AXIS_LX, 0, 0));
    km_pad_in_t in = { &k, 1, true, 0, 1, depth_engine, &e };
    km_pad_update(&p, &in, &out);
    in.now_ms = 2;
    km_pad_update(&p, &in, &out);
    CHECK(km_pad_report_neutral(&out));
}

/* ------------------------------------------------------------------------------------------------ random sequences */

static uint32_t rng_state;

static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void gen_keys(km_pad_key_t *keys, unsigned *n_out)
{
    static const uint8_t arg0_axis[] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 16, 17, 24, 25, 18, 19, 20, 21 };
    const unsigned n = rnd() % 14;
    for (unsigned i = 0; i < n; i++) {
        const uint8_t slot = (uint8_t)(rnd() % 40);
        if (rnd() % 3 == 0) {
            keys[i] = K(slot, BTN((uint8_t)(1 + rnd() % 20), (uint8_t)((rnd() % 3 == 0) ? 1 + rnd() % 68 : 0)));
        } else {
            keys[i] = K(slot, AX(arg0_axis[rnd() % sizeof arg0_axis], 0, (uint8_t)(rnd() % 4)));
        }
    }
    *n_out = n;
}

static void random_run(uint32_t seed, km_pad_report_t *trace, unsigned steps)
{
    rng_state = seed;
    rig_t r;
    rig_init(&r);
    km_pad_curve_t cv[4];
    km_pad_opts_t o;
    memset(&o, 0, sizeof o);
    for (unsigned i = 0; i < 4; i++) {
        km_pad_default_curve(i, &cv[i]);
    }
    for (unsigned t = 0; t < steps; t++) {
        const unsigned ev = rnd() % 64;
        if (ev == 0) {
            o.stick_deadzone = (uint8_t)(rnd() % 51);
            o.circular = (uint8_t)(rnd() % 2);
            for (unsigned i = 0; i < 4; i++) {
                cv[i].start_mm100_5 = (uint8_t)(4 + rnd() % 30);
                cv[i].end_mm100_5 = (uint8_t)(cv[i].start_mm100_5 + 2 + rnd() % (67 - cv[i].start_mm100_5 - 1));
                cv[i].shape = (uint8_t)(rnd() % 4);
                cv[i].flags = (uint8_t)(rnd() % 2);
            }
            CHECK(km_pad_set_config(&r.pad, cv, &o));
        } else if (ev == 1) {
            r.allowed = false;
        } else if (ev == 2) {
            r.allowed = true;
        } else if (ev == 3) {
            r.epoch++;
        } else if (ev == 4) {
            km_pad_reset(&r.pad);
        }
        for (unsigned s = 0; s < 40; s++) {
            if (rnd() % 4 == 0) {
                g_depth[s] = (uint16_t)(rnd() % 350);
            }
        }
        km_pad_key_t keys[40];
        unsigned n;
        gen_keys(keys, &n);
        const bool was_allowed = r.allowed;
        step(&r, keys, n);
        const km_pad_report_t *o2 = &r.out;
        trace[t] = r.out;
        /* bounds */
        CHECK(o2->lx >= -32767 && o2->ly >= -32767 && o2->rx >= -32767 && o2->ry >= -32767);
        CHECK(o2->lt <= 32767 && o2->rt <= 32767);
        CHECK(o2->hat <= 8);
        if (o.circular) {
            const int64_t a = (int64_t)o2->lx * o2->lx + (int64_t)o2->ly * o2->ly;
            const int64_t b = (int64_t)o2->rx * o2->rx + (int64_t)o2->ry * o2->ry;
            CHECK(a <= (int64_t)32767 * 32767);
            CHECK(b <= (int64_t)32767 * 32767);
        }
        /* the invariant of the pipeline: no key down (or keys not allowed) = a neutral pad */
        if (n == 0 || !was_allowed) {
            CHECK(km_pad_report_neutral(&r.out));
        }
        /* the packed form round-trips */
        uint8_t b[KM_PAD_REPORT_LEN];
        km_pad_report_t u;
        km_pad_report_pack(&r.out, b);
        km_pad_report_unpack(b, &u);
        CHECK(km_pad_report_equal(&u, &r.out));
        CHECK_EQ(b[2] & 0xF0, 0);
    }
}

static void test_random(void)
{
    enum { N = 4000 };
    static km_pad_report_t a[N], b[N];
    for (uint32_t seed = 1; seed <= 12; seed++) {
        random_run(seed * 7919u, a, N);
        random_run(seed * 7919u, b, N);                       /* deterministic: the same inputs, the same reports */
        CHECK(memcmp(a, b, sizeof a) == 0);
    }
}

/* A single analog key swept up and down: the value never goes down while the key goes down, never up while it comes up. */
static void test_sweep_monotonic(void)
{
    for (unsigned shape = 0; shape < 4; shape++) {
        for (unsigned axis = 0; axis < 6; axis++) {
            rig_t r;
            rig_init(&r);
            km_pad_curve_t cv[4];
            for (unsigned i = 0; i < 4; i++) {
                km_pad_default_curve(i, &cv[i]);
            }
            cv[2].shape = (uint8_t)shape;
            cv[2].flags = (uint8_t)(shape & 1u);
            CHECK(km_pad_set_config(&r.pad, cv, NULL));
            step(&r, NULL, 0);
            const km_pad_key_t k = K(W, AX(axis, 0, 2));
            int32_t prev = 0;
            for (unsigned d = 0; d <= 340; d++) {
                g_depth[W] = (uint16_t)d;
                step(&r, &k, 1);
                const int32_t v = axis == 0 ? r.out.lx : axis == 1 ? r.out.ly : axis == 2 ? r.out.rx : axis == 3 ? r.out.ry : axis == 4 ? r.out.lt : r.out.rt;
                CHECK(v >= prev);
                prev = v;
            }
            CHECK_EQ(prev, 32767);
            for (unsigned d = 340; d > 0; d--) {
                g_depth[W] = (uint16_t)d;
                step(&r, &k, 1);
                const int32_t v = axis == 0 ? r.out.lx : axis == 1 ? r.out.ly : axis == 2 ? r.out.rx : axis == 3 ? r.out.ry : axis == 4 ? r.out.lt : r.out.rt;
                CHECK(v <= prev);
                prev = v;
            }
            step(&r, NULL, 0);
            CHECK(km_pad_report_neutral(&r.out));
        }
    }
}

void test_pad(void)
{
    test_layout();
    test_curve_validity();
    test_config_replaces_bad_input();
    test_entry_validity();
    test_curve_shape();
    test_curve_values();
    test_stick_basic();
    test_opposing_keys();
    test_diagonal_and_circle();
    test_deadzone();
    test_buttons_and_hat();
    test_depth_button();
    test_fault_epoch_reset();
    test_start_up();
    test_cadence();
    test_stale_memory();
    test_band_edges();
    test_ignored_and_limits();
    test_output_hysteresis();
    test_engine_link();
    test_sweep_monotonic();
    test_random();
}
