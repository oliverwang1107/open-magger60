#include "km/km_ledstore.h"

#include <string.h>

void km_ledstore_rec_default(km_ledstore_rec_t *r)
{
    km_led_cfg_default(&r->cfg);
    r->idle_s = KM_LED_IDLE_DEFAULT_S;
}

bool km_ledstore_rec_valid(const km_ledstore_rec_t *r)
{
    return r != NULL && km_led_cfg_valid(&r->cfg);
}

unsigned km_ledstore_encode(const km_ledstore_rec_t *r, uint8_t *buf, unsigned cap)
{
    if (buf == NULL || cap < KM_LEDSTORE_PAYLOAD_BYTES || !km_ledstore_rec_valid(r)) {
        return 0;
    }
    memset(buf, 0, KM_LEDSTORE_PAYLOAD_BYTES);
    buf[0] = (uint8_t)KM_LEDSTORE_VERSION;
    buf[1] = r->cfg.effect;
    buf[2] = r->cfg.brightness;
    buf[3] = r->cfg.speed;
    buf[4] = r->cfg.color.r;
    buf[5] = r->cfg.color.g;
    buf[6] = r->cfg.color.b;
    buf[8] = (uint8_t)(r->idle_s & 0xFFu);
    buf[9] = (uint8_t)(r->idle_s >> 8);
    return KM_LEDSTORE_PAYLOAD_BYTES;
}

bool km_ledstore_decode(km_ledstore_rec_t *r, const uint8_t *buf, unsigned len)
{
    if (r == NULL || buf == NULL || len != KM_LEDSTORE_PAYLOAD_BYTES || buf[0] != KM_LEDSTORE_VERSION || buf[7] != 0u) {
        return false;
    }
    for (unsigned i = 10; i < KM_LEDSTORE_PAYLOAD_BYTES; i++) {
        if (buf[i] != 0u) {
            return false;
        }
    }
    km_ledstore_rec_t t;
    t.cfg.effect = buf[1];
    t.cfg.brightness = buf[2];
    t.cfg.speed = buf[3];
    t.cfg.color.r = buf[4];
    t.cfg.color.g = buf[5];
    t.cfg.color.b = buf[6];
    t.idle_s = (uint16_t)(buf[8] | ((unsigned)buf[9] << 8));
    if (!km_ledstore_rec_valid(&t)) {
        return false;
    }
    *r = t;
    return true;
}

bool km_ledstore_load(const km_clog_t *log, km_ledstore_rec_t *r)
{
    uint8_t buf[KM_LEDSTORE_PAYLOAD_BYTES];
    if (log == NULL || log->payload_len != KM_LEDSTORE_PAYLOAD_BYTES || !km_clog_read(log, buf, sizeof buf)) {
        return false;
    }
    return km_ledstore_decode(r, buf, sizeof buf);
}

void km_ledstore_init(km_ledstore_t *m, const km_clog_t *log, void (*after_flash)(void *ctx), void *ctx)
{
    memset(m, 0, sizeof *m);
    m->log = log;
    m->after_flash = after_flash;
    m->ctx = ctx;
}

bool km_ledstore_boot(km_ledstore_t *m, km_ledstore_rec_t *r)
{
    m->loaded = km_ledstore_load(m->log, r);
    return m->loaded;
}

static void request_commit(km_ledstore_t *m)
{
    m->req_seq++;
    m->req_fresh = true;                              /* the settle time is counted from the next pass: the stamp of the last one may be stale */
    m->tries = 0;
    m->gave_up = false;
}

bool km_ledstore_request(km_ledstore_t *m, const km_ledstore_rec_t *r)
{
    if (m->log == NULL || m->log->payload_len != KM_LEDSTORE_PAYLOAD_BYTES || km_ledstore_encode(r, m->want, sizeof m->want) == 0u) {
        return false;
    }
    request_commit(m);
    return true;
}

bool km_ledstore_request_raw(km_ledstore_t *m, const uint8_t *payload, unsigned len)
{
    if (m->log == NULL || payload == NULL || len == 0u || len != m->log->payload_len || len > sizeof m->want) {
        return false;
    }
    memcpy(m->want, payload, len);
    request_commit(m);
    return true;
}

bool km_ledstore_job_active(const km_ledstore_t *m)
{
    return km_clog_job_active(&m->job);
}

bool km_ledstore_must_finish(const km_ledstore_t *m)
{
    return km_clog_job_active(&m->job) && km_clog_job_must_finish(&m->job);
}

bool km_ledstore_pending(const km_ledstore_t *m)
{
    return m->log != NULL && !m->gave_up && m->req_seq != m->saved_seq;      /* a job in flight carries an unsaved request, so this covers it */
}

/* One flash operation of the running job; the CPU was stalled by the flash, so the caller's hook repairs the scan chain. */
static void job_step(km_ledstore_t *m)
{
    const km_clog_result_t r = km_clog_job_step(&m->job);
    if (m->after_flash != NULL) {
        m->after_flash(m->ctx);
    }
    if (r == KM_CLOG_PENDING) {
        return;
    }
    m->last = (uint8_t)r;
    if (r == KM_CLOG_OK) {
        m->saves_ok++;
        m->saved_seq = m->job_seq;
        m->tries = 0;
    } else {
        m->saves_failed++;                            /* the previous record is still the newest valid one */
        if (++m->tries >= KM_LEDSTORE_MAX_TRIES) {
            m->gave_up = true;
        }
    }
}

bool km_ledstore_pass(km_ledstore_t *m, uint32_t now_s, bool idle_now, bool idle_long)
{
    m->now_s = now_s;
    if (m->req_fresh) {
        m->req_fresh = false;
        m->last_req_s = now_s;
    }
    if (m->log == NULL) {
        return false;
    }
    if (km_clog_job_active(&m->job)) {
        if (km_clog_job_must_finish(&m->job) || idle_now) {
            job_step(m);
            return true;
        }
        return false;
    }
    if (m->gave_up || m->req_seq == m->saved_seq || !idle_long) {
        return false;
    }
    if ((uint32_t)(now_s - m->last_req_s) < KM_LEDSTORE_SETTLE_S) {
        return false;                                 /* still being changed: coalesce */
    }
    if (m->started_once && (uint32_t)(now_s - m->last_start_s) < KM_LEDSTORE_MIN_GAP_S) {
        return false;
    }
    m->started_once = true;
    m->last_start_s = now_s;
    m->job_seq = m->req_seq;
    const km_clog_result_t r = km_clog_job_begin(&m->job, m->log, m->want);     /* reads only: the first flash operation comes with the next pass */
    if (r == KM_CLOG_UNCHANGED) {                     /* the flash already holds exactly this */
        m->saved_seq = m->job_seq;
        m->tries = 0;
        m->last = (uint8_t)KM_CLOG_UNCHANGED;
    } else if (r != KM_CLOG_PENDING) {
        m->last = (uint8_t)r;
        m->saves_failed++;
        if (++m->tries >= KM_LEDSTORE_MAX_TRIES) {
            m->gave_up = true;
        }
    }
    return false;
}
