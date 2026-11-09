#include "km/km_capture.h"

#include <string.h>

void km_capture_init(km_capture_t *c, uint16_t cpu_mhz)
{
    memset(c, 0, sizeof *c);
    for (unsigned i = 0; i < KM_CAPTURE_MAX_SEL; i++) {
        c->sel[i] = KM_CAPTURE_NO_SLOT;
    }
    c->cpu_mhz = cpu_mhz;
}

static void recount(km_capture_t *c)
{
    unsigned n = 0;
    while (n < KM_CAPTURE_MAX_SEL && c->sel[n] != KM_CAPTURE_NO_SLOT) {
        n++;
    }
    c->n_sel = (uint8_t)n;
}

/* a changed selection makes the old recording unreadable: its columns would mean something else */
static void forget(km_capture_t *c)
{
    c->state = KM_CAPTURE_IDLE;
    c->frames = 0;
    c->cond_frames = 0;
    c->t_first = 0;
    c->t_last = 0;
}

bool km_capture_select(km_capture_t *c, unsigned idx, unsigned slot)
{
    if (c->state == KM_CAPTURE_RUNNING || idx >= KM_CAPTURE_MAX_SEL ||
        (slot >= KM_CAPTURE_NUM_SLOTS && slot != KM_CAPTURE_NO_SLOT)) {
        return false;
    }
    c->sel[idx] = (uint8_t)slot;
    recount(c);
    forget(c);
    return true;
}

bool km_capture_set_slots(km_capture_t *c, const uint8_t *slots, unsigned n)
{
    if (c->state == KM_CAPTURE_RUNNING || n > KM_CAPTURE_MAX_SEL || (n != 0 && slots == NULL)) {
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        if (slots[i] >= KM_CAPTURE_NUM_SLOTS) {
            return false;
        }
    }
    for (unsigned i = 0; i < KM_CAPTURE_MAX_SEL; i++) {
        c->sel[i] = i < n ? slots[i] : (uint8_t)KM_CAPTURE_NO_SLOT;
    }
    recount(c);
    forget(c);
    return true;
}

bool km_capture_control(km_capture_t *c, unsigned ctrl)
{
    switch (ctrl) {
    case KM_CAPTURE_CTRL_STOP:
        if (c->state == KM_CAPTURE_RUNNING) {
            c->state = c->frames != 0 ? KM_CAPTURE_DONE : KM_CAPTURE_IDLE;
        }
        return true;
    case KM_CAPTURE_CTRL_ONESHOT:
    case KM_CAPTURE_CTRL_RING:
        if (c->n_sel == 0) {
            return false;
        }
        c->ring = ctrl == KM_CAPTURE_CTRL_RING ? 1u : 0u;
        c->frames = 0;
        c->cond_frames = 0;
        c->t_first = 0;
        c->t_last = 0;
        c->run = (uint16_t)(c->run + 1u);
        if (c->run == 0) {
            c->run = 1;                         /* 0 means "never armed" */
        }
        c->state = KM_CAPTURE_RUNNING;
        return true;
    default:
        return false;
    }
}

unsigned km_capture_capacity_frames(const km_capture_t *c)
{
    return c->n_sel == 0 ? 0u : KM_CAPTURE_SAMPLES / c->n_sel;
}

bool km_capture_push_frame(km_capture_t *c, const uint32_t *phase_words, const uint32_t inj[2], uint32_t now, bool cond)
{
    if (c->state != KM_CAPTURE_RUNNING) {
        return false;
    }
    const unsigned n = c->n_sel;
    const unsigned cap = KM_CAPTURE_SAMPLES / n;        /* n >= 1: arming needs a slot */
    const unsigned f = (unsigned)(c->frames % cap);
    uint16_t *dst = &c->buf[f * n];
    for (unsigned i = 0; i < n; i++) {
        const unsigned slot = c->sel[i];
        const unsigned word = slot >> 1;
        const uint32_t w = word < 36u ? phase_words[word] : inj[word - 36u];
        const uint32_t sample = ((slot & 1u) != 0u ? w >> 16 : w) & KM_CAPTURE_VALUE_MASK;
        dst[i] = (uint16_t)(sample | (cond ? KM_CAPTURE_FLAG : 0u));
    }
    if (c->frames == 0) {
        c->t_first = now;
    }
    c->t_last = now;
    c->frames++;
    if (cond) {
        c->cond_frames++;
    }
    if (c->ring == 0 && c->frames >= cap) {
        c->state = KM_CAPTURE_DONE;
    }
    return true;
}

static unsigned avail_frames(const km_capture_t *c)
{
    const unsigned cap = km_capture_capacity_frames(c);
    return c->frames < cap ? (unsigned)c->frames : cap;
}

unsigned km_capture_value_count(const km_capture_t *c)
{
    if (c->state == KM_CAPTURE_RUNNING) {
        return 0;
    }
    return avail_frames(c) * c->n_sel;
}

unsigned km_capture_read(const km_capture_t *c, unsigned first, unsigned n, uint16_t *out)
{
    const unsigned total = km_capture_value_count(c);
    if (first >= total || out == NULL) {
        return 0;
    }
    if (n > total - first) {
        n = total - first;
    }
    const unsigned cols = c->n_sel;
    const unsigned cap = km_capture_capacity_frames(c);
    /* a ring run that wrapped starts at the oldest frame, which is the one the next frame would overwrite */
    const unsigned oldest = c->frames > cap ? (unsigned)(c->frames % cap) : 0u;
    for (unsigned k = 0; k < n; k++) {
        const unsigned v = first + k;
        const unsigned f = (oldest + v / cols) % cap;
        out[k] = c->buf[f * cols + v % cols];
    }
    return n;
}

void km_capture_status(const km_capture_t *c, uint16_t out[KM_CAPTURE_STATUS_WORDS])
{
    memset(out, 0, KM_CAPTURE_STATUS_WORDS * sizeof out[0]);
    out[KM_CAP_ST_VERSION] = KM_CAPTURE_STATUS_VERSION;
    out[KM_CAP_ST_STATE] = c->state;
    out[KM_CAP_ST_NSEL] = c->n_sel;
    for (unsigned i = 0; i < KM_CAPTURE_MAX_SEL; i++) {
        out[KM_CAP_ST_SLOT0 + i] = c->sel[i];
    }
    out[KM_CAP_ST_RUN] = c->run;
    out[KM_CAP_ST_CAP_FRAMES] = (uint16_t)km_capture_capacity_frames(c);
    out[KM_CAP_ST_FRAMES_LO] = (uint16_t)c->frames;
    out[KM_CAP_ST_FRAMES_HI] = (uint16_t)(c->frames >> 16);
    out[KM_CAP_ST_AVAIL] = (uint16_t)avail_frames(c);
    out[KM_CAP_ST_T_FIRST_LO] = (uint16_t)c->t_first;
    out[KM_CAP_ST_T_FIRST_HI] = (uint16_t)(c->t_first >> 16);
    out[KM_CAP_ST_T_LAST_LO] = (uint16_t)c->t_last;
    out[KM_CAP_ST_T_LAST_HI] = (uint16_t)(c->t_last >> 16);
    out[KM_CAP_ST_COND_LO] = (uint16_t)c->cond_frames;
    out[KM_CAP_ST_COND_HI] = (uint16_t)(c->cond_frames >> 16);
    out[KM_CAP_ST_RING] = c->ring;
    out[KM_CAP_ST_CPU_MHZ] = c->cpu_mhz;
}
