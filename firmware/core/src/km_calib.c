#include "km/km_calib.h"

#include <string.h>

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static unsigned abs_diff(unsigned a, unsigned b)
{
    return a > b ? a - b : b - a;
}

/* ---- record format ---- */

static bool entry_valid(uint16_t base, uint16_t bottom)
{
    if (base == 0u && bottom == 0u) {
        return true;                                       /* nothing known */
    }
    if (base < KM_ENGINE_FAULT_LOW || base > KM_ENGINE_VALUE_MAX || bottom >= base) {
        return false;
    }
    const unsigned span = (unsigned)(base - bottom);
    return span >= KM_ENGINE_MIN_SPAN && span <= KM_ENGINE_MAX_SPAN;
}

bool km_cal_record_valid(const km_cal_record_t *r)
{
    if (r == NULL) {
        return false;
    }
    for (unsigned s = 0; s < KM_CAL_SLOTS; s++) {
        if (!entry_valid(r->base[s], r->bottom[s])) {
            return false;
        }
    }
    return true;
}

unsigned km_cal_encode(const km_cal_record_t *r, uint8_t *buf, unsigned cap)
{
    if (buf == NULL || cap < KM_CAL_PAYLOAD_BYTES || !km_cal_record_valid(r)) {
        return 0;
    }
    buf[0] = (uint8_t)KM_CAL_VERSION;
    buf[1] = (uint8_t)KM_CAL_SLOTS;
    buf[2] = 0;
    buf[3] = 0;
    for (unsigned s = 0; s < KM_CAL_SLOTS; s++) {
        put16(buf + 4u + 4u * s, r->base[s]);
        put16(buf + 6u + 4u * s, r->bottom[s]);
    }
    return KM_CAL_PAYLOAD_BYTES;
}

bool km_cal_decode(km_cal_record_t *r, const uint8_t *buf, unsigned len)
{
    if (r == NULL || buf == NULL || len != KM_CAL_PAYLOAD_BYTES || buf[0] != KM_CAL_VERSION || buf[1] != KM_CAL_SLOTS ||
        buf[2] != 0u || buf[3] != 0u) {
        return false;
    }
    km_cal_record_t t;
    for (unsigned s = 0; s < KM_CAL_SLOTS; s++) {
        t.base[s] = get16(buf + 4u + 4u * s);
        t.bottom[s] = get16(buf + 6u + 4u * s);
    }
    if (!km_cal_record_valid(&t)) {
        return false;
    }
    *r = t;
    return true;
}

/* ---- flash ---- */

bool km_cal_load(const km_clog_t *log, km_cal_record_t *r)
{
    uint8_t buf[KM_CAL_PAYLOAD_BYTES];
    if (log == NULL || log->payload_len != KM_CAL_PAYLOAD_BYTES || !km_clog_read(log, buf, sizeof buf)) {
        return false;
    }
    return km_cal_decode(r, buf, sizeof buf);
}

km_clog_result_t km_cal_job_begin(km_clog_job_t *job, const km_clog_t *log, const km_cal_record_t *r)
{
    uint8_t buf[KM_CAL_PAYLOAD_BYTES];
    if (log == NULL || log->payload_len != KM_CAL_PAYLOAD_BYTES || km_cal_encode(r, buf, sizeof buf) == 0u) {
        return KM_CLOG_ERR_ARG;
    }
    return km_clog_job_begin(job, log, buf);
}

km_clog_result_t km_cal_save(const km_clog_t *log, const km_cal_record_t *r)
{
    km_clog_job_t job;
    km_clog_result_t res = km_cal_job_begin(&job, log, r);
    while (res == KM_CLOG_PENDING) {
        res = km_clog_job_step(&job);
    }
    return res;
}

/* ---- engine glue ---- */

void km_cal_capture(const km_engine_t *e, const km_cal_record_t *prev, km_cal_record_t *out)
{
    for (unsigned s = 0; s < KM_CAL_SLOTS; s++) {
        if (e->calibrated && km_engine_slot_status(e, s) == KM_ENGINE_SLOT_OK) {
            out->base[s] = km_engine_baseline(e, s);
            out->bottom[s] = km_engine_bottom(e, s);
        } else if (prev != NULL) {
            out->base[s] = prev->base[s];
            out->bottom[s] = prev->bottom[s];
        } else {
            out->base[s] = 0;
            out->bottom[s] = 0;
        }
    }
}

void km_cal_snapshot(const km_engine_t *e, uint16_t ref[KM_CAL_SLOTS])
{
    for (unsigned s = 0; s < KM_CAL_SLOTS; s++) {
        ref[s] = km_engine_bottom(e, s);
    }
}

bool km_cal_changed(const km_engine_t *e, const uint16_t ref[KM_CAL_SLOTS])
{
    if (!e->calibrated) {
        return false;
    }
    for (unsigned s = 0; s < KM_CAL_SLOTS; s++) {
        if (km_engine_slot_status(e, s) == KM_ENGINE_SLOT_OK) {
            const uint16_t b = km_engine_bottom(e, s);
            if (ref[s] > b && (unsigned)(ref[s] - b) >= KM_CAL_DELTA_COUNTS) {
                return true;
            }
        }
    }
    return false;
}

uint32_t km_cal_bottom_sum(const km_engine_t *e)
{
    uint32_t sum = 0;
    for (unsigned s = 0; s < KM_CAL_SLOTS; s++) {
        if (km_engine_slot_status(e, s) == KM_ENGINE_SLOT_OK) {
            sum += km_engine_bottom(e, s);
        }
    }
    return sum;
}

unsigned km_cal_apply(km_engine_t *e, const km_cal_record_t *r)
{
    unsigned n = 0;
    for (unsigned s = 0; s < KM_CAL_SLOTS; s++) {
        if (r->base[s] == 0u || km_engine_slot_status(e, s) != KM_ENGINE_SLOT_OK) {
            continue;                               /* nothing stored, or the live baseline of this slot is only a guess */
        }
        const uint16_t live = km_engine_baseline(e, s);
        if (abs_diff(live, r->base[s]) > KM_CAL_AGREE_COUNTS) {
            continue;
        }
        const uint16_t span = (uint16_t)(r->base[s] - r->bottom[s]);
        if (live <= span) {
            continue;
        }
        const uint16_t bottom = (uint16_t)(live - span);
        if (bottom != km_engine_bottom(e, s) && km_engine_set_bottom(e, s, bottom)) {
            n++;
        }
    }
    return n;
}

km_cal_restore_t km_cal_restore(km_engine_t *e, const km_cal_record_t *r, const uint16_t live[KM_CAL_SLOTS], unsigned *applied)
{
    unsigned agree = 0, differ = 0;
    if (applied != NULL) {
        *applied = 0;
    }
    for (unsigned s = 0; s < KM_CAL_SLOTS; s++) {
        if (r->base[s] == 0u || km_engine_slot_status(e, s) == KM_ENGINE_SLOT_NO_KEY) {
            continue;                               /* nothing stored, or no key on this slot: no evidence either way */
        }
        if (live[s] >= KM_ENGINE_FAULT_LOW && live[s] <= KM_ENGINE_VALUE_MAX && abs_diff(live[s], r->base[s]) <= KM_CAL_AGREE_COUNTS) {
            agree++;
        } else {
            differ++;
        }
    }
    if (agree < KM_ENGINE_CAL_MIN_SLOTS || differ > KM_ENGINE_CAL_MAX_BAD) {
        return KM_CAL_RESTORE_DISAGREE;
    }
    km_engine_calibrate_begin(e);
    for (unsigned i = 0; i < KM_ENGINE_CAL_MIN_FRAMES; i++) {
        km_engine_calibrate_idle(e, live);
    }
    if (km_engine_calibrate_finish(e) != KM_ENGINE_CAL_OK) {
        return KM_CAL_RESTORE_REFUSED;
    }
    const unsigned n = km_cal_apply(e, r);
    if (applied != NULL) {
        *applied = n;
    }
    return KM_CAL_RESTORE_OK;
}

/* ---- save policy ---- */

void km_cal_policy_init(km_cal_policy_t *p)
{
    memset(p, 0, sizeof *p);
}

bool km_cal_policy_due(km_cal_policy_t *p, const km_engine_t *e, const uint16_t ref[KM_CAL_SLOTS], uint32_t now_s)
{
    const uint32_t sum = km_cal_bottom_sum(e);
    if (!p->have_sum || sum != p->last_sum) {
        p->have_sum = true;
        p->last_sum = sum;
        p->last_change_s = now_s;                   /* still learning (or just calibrated): the quiet time starts over */
    }
    if (!km_cal_changed(e, ref)) {
        return false;
    }
    if ((uint32_t)(now_s - p->last_change_s) < KM_CAL_QUIET_S) {
        return false;
    }
    return !p->saved_once || (uint32_t)(now_s - p->last_save_s) >= KM_CAL_MIN_INTERVAL_S;
}

void km_cal_policy_saved(km_cal_policy_t *p, uint32_t now_s)
{
    p->saved_once = true;
    p->last_save_s = now_s;
}

bool km_cal_policy_request(km_cal_policy_t *p, uint32_t now_s)
{
    if (p->requested_once && (uint32_t)(now_s - p->last_request_s) < KM_CAL_REQUEST_GAP_S) {
        return false;
    }
    p->requested_once = true;
    p->last_request_s = now_s;
    return true;
}
