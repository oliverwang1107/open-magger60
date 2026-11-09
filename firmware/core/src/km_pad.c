/* km_pad.c - gamepad core (M6 package P4). Original work. See km_pad.h and core/PAD_NOTES.md. */
#include "km/km_pad.h"

#include <string.h>

#define WORDS ((KM_PAD_SLOTS + 31u) / 32u)
#define MAXV  KM_PAD_AXIS_MAX
#define SCALE15 32768u    /* 1.0 in Q15 */

/* ---------------------------------------------------------------------------------------------------------------- curves */

void km_pad_default_curve(unsigned index, km_pad_curve_t *out)
{
    /* 0 general stick (0.20..3.20 mm), 1 trigger (0.20..2.00 mm), 2 fine control near the centre, 3 S shape */
    static const km_pad_curve_t def[KM_PAD_CURVES] = {
        { 4u, 64u, KM_PAD_SHAPE_LINEAR, 0u },
        { 4u, 40u, KM_PAD_SHAPE_LINEAR, 0u },
        { 4u, 64u, KM_PAD_SHAPE_EASE_IN, 0u },
        { 4u, 64u, KM_PAD_SHAPE_S, 0u },
    };
    *out = def[index < KM_PAD_CURVES ? index : 0u];
}

bool km_pad_curve_valid(const km_pad_curve_t *c)
{
    return c != NULL && c->start_mm100_5 >= KM_PAD_MIN_START_MM100 / 5u && c->end_mm100_5 >= c->start_mm100_5 + 2u &&
           c->end_mm100_5 <= 68u && c->shape <= KM_PAD_SHAPE_S &&
           (c->flags & (uint8_t)~KM_PAD_CURVE_MIRROR) == 0u;
}

bool km_pad_opts_valid(const km_pad_opts_t *o)
{
    if (o == NULL || o->stick_deadzone > 50u || o->circular > 1u || o->smooth > 3u) {
        return false;
    }
    for (unsigned i = 0; i < sizeof o->rsvd; i++) {
        if (o->rsvd[i] != 0u) {
            return false;
        }
    }
    return true;
}

bool km_pad_entry_valid(uint8_t code, uint8_t arg0, uint8_t arg1)
{
    if (code == KM_PAD_CODE_BTN) {
        return arg0 >= 1u && arg0 <= KM_PAD_BTN_HAT_RIGHT && arg1 <= 68u;
    }
    if (code == KM_PAD_CODE_AXIS) {
        const unsigned axis = arg0 & 7u;
        if ((arg0 & 0xE0u) != 0u || axis >= KM_PAD_AXES || arg1 >= KM_PAD_CURVES) {
            return false;
        }
        return axis <= KM_PAD_AXIS_RY || (arg0 & KM_PAD_AXIS_NEG) == 0u;      /* triggers have one direction only */
    }
    return false;
}

/* Shape on Q15: t in 0..32768 -> 0..32768, each one monotonic and exact at both ends (one floor of an exact expression). */
static uint32_t shape_q15(unsigned shape, uint32_t t)
{
    switch (shape) {
    case KM_PAD_SHAPE_EASE_IN:
        return (t * t) >> 15;
    case KM_PAD_SHAPE_EASE_OUT: {
        const uint32_t u = SCALE15 - t;
        return SCALE15 - ((u * u + (SCALE15 - 1u)) >> 15);
    }
    case KM_PAD_SHAPE_S:   /* 3t^2 - 2t^3 */
        return (uint32_t)(((uint64_t)t * t * (3u * SCALE15 - 2u * t)) >> 30);
    default:
        return t;
    }
}

int32_t km_pad_curve_eval(const km_pad_curve_t *c, uint16_t depth_mm100)
{
    const uint32_t s = (uint32_t)c->start_mm100_5 * 5u;
    const uint32_t e = (uint32_t)c->end_mm100_5 * 5u;
    const uint32_t d = depth_mm100;
    if (d <= s) {
        return 0;
    }
    if (d >= e) {
        return MAXV;
    }
    uint32_t t = ((d - s) << 15) / (e - s);          /* 0..32767 (e > s because d is between them) */
    uint32_t y;
    if ((c->flags & KM_PAD_CURVE_MIRROR) != 0u) {
        y = SCALE15 - shape_q15(c->shape, SCALE15 - t);
    } else {
        y = shape_q15(c->shape, t);
    }
    return (int32_t)((y * (uint32_t)MAXV + (SCALE15 / 2u)) >> 15);
}

/* ---------------------------------------------------------------------------------------------------------------- report */

void km_pad_report_released(uint8_t out[KM_PAD_REPORT_LEN])
{
    memset(out, 0, KM_PAD_REPORT_LEN);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

void km_pad_report_pack(const km_pad_report_t *r, uint8_t out[KM_PAD_REPORT_LEN])
{
    put16(out + 0, r->buttons);
    out[2] = r->hat <= 8u ? r->hat : (uint8_t)0;
    put16(out + 3, (uint16_t)r->lx);
    put16(out + 5, (uint16_t)r->ly);
    put16(out + 7, (uint16_t)r->rx);
    put16(out + 9, (uint16_t)r->ry);
    put16(out + 11, r->lt);
    put16(out + 13, r->rt);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

void km_pad_report_unpack(const uint8_t in[KM_PAD_REPORT_LEN], km_pad_report_t *r)
{
    r->buttons = get16(in + 0);
    r->hat = (uint8_t)(in[2] & 0x0Fu);
    if (r->hat > 8u) {
        r->hat = 0u;
    }
    r->lx = (int16_t)get16(in + 3);
    r->ly = (int16_t)get16(in + 5);
    r->rx = (int16_t)get16(in + 7);
    r->ry = (int16_t)get16(in + 9);
    r->lt = get16(in + 11);
    r->rt = get16(in + 13);
}

bool km_pad_report_equal(const km_pad_report_t *a, const km_pad_report_t *b)
{
    uint8_t x[KM_PAD_REPORT_LEN];
    uint8_t y[KM_PAD_REPORT_LEN];
    km_pad_report_pack(a, x);
    km_pad_report_pack(b, y);
    return memcmp(x, y, KM_PAD_REPORT_LEN) == 0;
}

bool km_pad_report_neutral(const km_pad_report_t *r)
{
    uint8_t x[KM_PAD_REPORT_LEN];
    km_pad_report_pack(r, x);
    for (unsigned i = 0; i < KM_PAD_REPORT_LEN; i++) {
        if (x[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------------------------------------------- state */

static bool bit_get(const uint32_t *w, unsigned i)
{
    return (w[i >> 5] >> (i & 31u)) & 1u;
}

static void bit_set(uint32_t *w, unsigned i)
{
    w[i >> 5] |= 1u << (i & 31u);
}

static void bit_clear(uint32_t *w, unsigned i)
{
    w[i >> 5] &= ~(1u << (i & 31u));
}

static void load_defaults(km_pad_t *p)
{
    for (unsigned i = 0; i < KM_PAD_CURVES; i++) {
        km_pad_default_curve(i, &p->curve[i]);
    }
    memset(&p->opts, 0, sizeof p->opts);
}

void km_pad_reset(km_pad_t *p)
{
    memset(&p->cur, 0, sizeof p->cur);
    memset(p->raw, 0, sizeof p->raw);
    memset(p->blocked, 0, sizeof p->blocked);
    memset(p->btn_on, 0, sizeof p->btn_on);
    p->rearm = true;
    p->resets++;
}

void km_pad_init(km_pad_t *p)
{
    memset(p, 0, sizeof *p);
    load_defaults(p);
    p->rearm = true;
}

bool km_pad_set_config(km_pad_t *p, const km_pad_curve_t *curves, const km_pad_opts_t *opts)
{
    bool all_ok = true;
    for (unsigned i = 0; i < KM_PAD_CURVES; i++) {
        if (curves != NULL && km_pad_curve_valid(&curves[i])) {
            p->curve[i] = curves[i];
        } else {
            km_pad_default_curve(i, &p->curve[i]);
            all_ok = all_ok && curves == NULL;
        }
    }
    if (opts != NULL && km_pad_opts_valid(opts)) {
        p->opts = *opts;
    } else {
        memset(&p->opts, 0, sizeof p->opts);
        all_ok = all_ok && opts == NULL;
    }
    km_pad_reset(p);
    return all_ok;
}

/* floor(sqrt(n)), bit by bit */
static uint32_t isqrt_floor(uint32_t n)
{
    uint32_t r = 0u;
    uint32_t b = 1u << 30;
    while (b > n) {
        b >>= 2;
    }
    while (b != 0u) {
        if (n >= r + b) {
            n -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

static uint32_t isqrt_ceil(uint32_t n)
{
    const uint32_t r = isqrt_floor(n);
    return r * r < n ? r + 1u : r;
}

static int32_t clamp_axis(int32_t v)
{
    return v > MAXV ? MAXV : (v < -MAXV ? -MAXV : v);
}

/* x, y in -32767..32767 -> circle limit, then centre dead zone (radial, rescaled to full), then clamp. */
static void stick(int32_t *x, int32_t *y, unsigned deadzone_pct, bool circular)
{
    uint32_t r2 = (uint32_t)(*x * *x) + (uint32_t)(*y * *y);
    if (r2 == 0u) {
        return;
    }
    const uint32_t max2 = (uint32_t)MAXV * (uint32_t)MAXV;
    if (circular && r2 > max2) {
        const int32_t r = (int32_t)isqrt_ceil(r2);          /* ceil: the result is inside the circle, never a hair outside */
        *x = *x * MAXV / r;                                 /* |x| * 32767 < 2^31: no 64-bit division on the M4 */
        *y = *y * MAXV / r;
        r2 = (uint32_t)(*x * *x) + (uint32_t)(*y * *y);
    }
    if (deadzone_pct != 0u) {
        const uint32_t dz = deadzone_pct * (uint32_t)MAXV / 100u;
        if (r2 <= dz * dz) {
            *x = 0;
            *y = 0;
            return;
        }
        const uint32_t r = isqrt_ceil(r2);
        const uint32_t rn = (r - dz) * (uint32_t)MAXV / ((uint32_t)MAXV - dz);
        *x = clamp_axis(*x * (int32_t)rn / (int32_t)r);     /* |x| * rn <= 32767 * 59913 < 2^31 */
        *y = clamp_axis(*y * (int32_t)rn / (int32_t)r);
    }
}

/* A value follows its input only after moving KM_PAD_OUT_HYST; 0, full scale and a change of sign are taken at once. */
static int32_t out_hyst(int32_t prev, int32_t v)
{
    if (v == 0 || v == MAXV || v == -MAXV || (v < 0) != (prev < 0) || prev == 0) {
        return v;
    }
    const int32_t d = v - prev;
    return (d >= KM_PAD_OUT_HYST || d <= -KM_PAD_OUT_HYST) ? v : prev;
}

static const uint8_t HAT_TABLE[3][3] = {   /* [vertical + 1][horizontal + 1], vertical -1 = up, horizontal -1 = left */
    { 8u, 1u, 2u },
    { 7u, 0u, 3u },
    { 6u, 5u, 4u },
};

static unsigned hat_of(unsigned bits)  /* bit0 up, bit1 down, bit2 left, bit3 right */
{
    const int v = ((bits & 2u) != 0u ? 1 : 0) - ((bits & 1u) != 0u ? 1 : 0);
    const int h = ((bits & 8u) != 0u ? 1 : 0) - ((bits & 4u) != 0u ? 1 : 0);
    return HAT_TABLE[v + 1][h + 1];
}

static uint32_t read_depth(const km_pad_in_t *in, unsigned slot)
{
    return in->depth != NULL ? in->depth(in->ctx, slot) : 0u;
}

static void compute(km_pad_t *p, const km_pad_in_t *in, unsigned n, const bool *use)
{
    uint16_t buttons = 0u;
    unsigned hatbits = 0u;
    int32_t pos[KM_PAD_AXES] = { 0, 0, 0, 0, 0, 0 };
    int32_t neg[4] = { 0, 0, 0, 0 };

    for (unsigned i = 0; i < n; i++) {
        if (!use[i]) {
            continue;
        }
        const km_pad_key_t *k = &in->keys[i];
        if (k->code == KM_PAD_CODE_BTN) {
            bool on = true;
            if (k->arg1 == 0u) {
                bit_clear(p->btn_on, k->slot);
            } else {
                uint32_t act = (uint32_t)k->arg1 * 5u;
                if (act < KM_PAD_BTN_MIN_ACT_MM100) {
                    act = KM_PAD_BTN_MIN_ACT_MM100;
                }
                const uint32_t hyst = act / 2u < KM_PAD_BTN_HYST_MM100 ? act / 2u : KM_PAD_BTN_HYST_MM100;
                const uint32_t d = read_depth(in, k->slot);
                on = bit_get(p->btn_on, k->slot) ? d + hyst >= act : d >= act;
                if (on) {
                    bit_set(p->btn_on, k->slot);
                } else {
                    bit_clear(p->btn_on, k->slot);
                }
            }
            if (on) {
                if (k->arg0 <= KM_PAD_BUTTONS) {
                    buttons = (uint16_t)(buttons | (1u << (k->arg0 - 1u)));
                } else {
                    hatbits |= 1u << (k->arg0 - KM_PAD_BTN_HAT_UP);
                }
            }
        } else {
            const unsigned axis = k->arg0 & 7u;
            const int32_t v = (k->arg0 & KM_PAD_AXIS_DIGITAL) != 0u
                                  ? MAXV
                                  : km_pad_curve_eval(&p->curve[k->arg1], (uint16_t)read_depth(in, k->slot));
            int32_t *dst = ((k->arg0 & KM_PAD_AXIS_NEG) != 0u) ? &neg[axis] : &pos[axis];
            if (v > *dst) {
                *dst = v;
            }
        }
    }

    /* the band acts on the raw axis values, before the circle and the dead zone, so that those two rules always hold exactly */
    int32_t v[4];
    for (unsigned a = 0; a < 4u; a++) {
        p->raw[a] = (int16_t)out_hyst(p->raw[a], pos[a] - neg[a]);
        v[a] = p->raw[a];
    }
    stick(&v[0], &v[1], p->opts.stick_deadzone, p->opts.circular != 0u);
    stick(&v[2], &v[3], p->opts.stick_deadzone, p->opts.circular != 0u);

    km_pad_report_t *r = &p->cur;
    r->buttons = buttons;
    r->hat = (uint8_t)hat_of(hatbits);
    r->lx = (int16_t)v[0];
    r->ly = (int16_t)v[1];
    r->rx = (int16_t)v[2];
    r->ry = (int16_t)v[3];
    r->lt = (uint16_t)out_hyst((int32_t)r->lt, pos[KM_PAD_AXIS_LT]);
    r->rt = (uint16_t)out_hyst((int32_t)r->rt, pos[KM_PAD_AXIS_RT]);
}

bool km_pad_update(km_pad_t *p, const km_pad_in_t *in, km_pad_report_t *out)
{
    if (!in->allowed) {                       /* fault: neutral now and hold every key over until it has been released */
        if (!p->rearm) {
            p->faults++;
        }
        memset(&p->cur, 0, sizeof p->cur);
        memset(p->raw, 0, sizeof p->raw);
        memset(p->btn_on, 0, sizeof p->btn_on);
        p->rearm = true;
        p->epoch = in->epoch;
        p->started = true;
        *out = p->cur;
        return true;
    }
    if (p->started && in->epoch != p->epoch) {
        km_pad_reset(p);
    }
    p->epoch = in->epoch;
    if (p->started && !p->rearm && (uint32_t)(in->now_ms - p->last_ms) < KM_PAD_PERIOD_MS) {
        *out = p->cur;
        return false;
    }
    p->started = true;
    p->last_ms = in->now_ms;
    p->computes++;

    const unsigned n = (in->keys == NULL) ? 0u : (in->n < KM_PAD_MAX_KEYS ? in->n : KM_PAD_MAX_KEYS);
    if (in->keys != NULL && in->n > n) {
        p->ignored_keys += in->n - n;
    }
    bool use[KM_PAD_MAX_KEYS];
    uint32_t listed[WORDS] = { 0u, 0u, 0u };
    for (unsigned i = 0; i < n; i++) {
        const km_pad_key_t *k = &in->keys[i];
        use[i] = k->slot < KM_PAD_SLOTS && km_pad_entry_valid(k->code, k->arg0, k->arg1);
        if (use[i]) {
            bit_set(listed, k->slot);
        } else {
            p->ignored_keys++;
        }
    }

    if (p->rearm) {                           /* keys found down now are held over; nothing is output for them */
        for (unsigned w = 0; w < WORDS; w++) {
            p->blocked[w] = listed[w];
        }
        memset(p->btn_on, 0, sizeof p->btn_on);
        p->rearm = false;                     /* cur and raw are already neutral: every way into re-arm clears them */
        *out = p->cur;
        return true;
    }

    for (unsigned i = 0; i < n; i++) {
        if (use[i] && bit_get(p->blocked, in->keys[i].slot)) {
            use[i] = false;                   /* still held since the reset */
        }
    }
    for (unsigned w = 0; w < WORDS; w++) {    /* a key that is no longer listed is free again */
        p->blocked[w] &= listed[w];
        p->btn_on[w] &= listed[w];
    }
    compute(p, in, n, use);
    *out = p->cur;
    return true;
}
