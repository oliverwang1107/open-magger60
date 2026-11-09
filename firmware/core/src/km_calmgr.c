#include "km/km_calmgr.h"

#include <string.h>

void km_calmgr_init(km_calmgr_t *m, const km_clog_t *log, km_engine_t *e, void (*after_flash)(void *ctx), void *ctx)
{
    memset(m, 0, sizeof *m);
    m->log = log;
    m->engine = e;
    m->after_flash = after_flash;
    m->ctx = ctx;
    km_cal_policy_init(&m->pol);
    if (log != NULL) {
        (void)km_clog_scan(log, &m->scan0);
        m->rec_valid = km_cal_load(log, &m->rec);
        m->try_restore = m->rec_valid;
    }
}

bool km_calmgr_wants_restore(const km_calmgr_t *m)
{
    return m->try_restore;
}

bool km_calmgr_try_restore(km_calmgr_t *m, const uint16_t live[KM_CAL_SLOTS])
{
    if (!m->try_restore) {
        return false;
    }
    m->try_restore = false;                         /* one attempt per boot */
    unsigned applied = 0;
    const km_cal_restore_t r = km_cal_restore(m->engine, &m->rec, live, &applied);
    m->boot = (uint8_t)(KM_CALMGR_BOOT_RESTORED + (unsigned)r);   /* OK -> 1, DISAGREE -> 2, REFUSED -> 3 */
    if (r == KM_CAL_RESTORE_DISAGREE) {
        m->rec_valid = false;                       /* not this keyboard, or the sensor moved too far: learn again */
        return false;
    }
    if (r != KM_CAL_RESTORE_OK) {
        return false;
    }
    m->applied = (uint16_t)applied;
    km_calmgr_armed(m, false);
    return true;
}

void km_calmgr_armed(km_calmgr_t *m, bool apply_stored)
{
    if (apply_stored && m->rec_valid) {
        m->applied = (uint16_t)km_cal_apply(m->engine, &m->rec);
    }
    km_cal_snapshot(m->engine, m->ref);
    m->due = false;
}

void km_calmgr_keep_learned(km_calmgr_t *m)
{
    km_cal_record_t keep;
    km_cal_capture(m->engine, m->rec_valid ? &m->rec : NULL, &keep);
    if (km_cal_record_valid(&keep)) {
        m->rec = keep;
        m->rec_valid = true;
    }
}

/* One flash operation of the running job; the CPU was stalled by the flash, so the caller's hook repairs the scan chain. */
static km_clog_result_t job_step(km_calmgr_t *m, uint32_t now_s)
{
    const km_clog_result_t r = km_clog_job_step(&m->job);
    if (m->after_flash != NULL) {
        m->after_flash(m->ctx);
    }
    if (r == KM_CLOG_PENDING) {
        return r;
    }
    m->last = (uint8_t)r;
    if (r == KM_CLOG_OK) {
        m->saves_ok++;
        m->rec = m->pending;
        m->rec_valid = true;
        memcpy(m->ref, m->ref_pending, sizeof m->ref);
    } else {
        m->saves_failed++;                          /* the previous record is still the newest valid one */
    }
    km_cal_policy_saved(&m->pol, now_s);            /* a failure waits for the minimum interval too: no retry storm */
    return r;
}

/* Capture the engine state and plan the save. PENDING = a job is running; anything else = nothing left to do. */
static km_clog_result_t job_start(km_calmgr_t *m, uint32_t now_s)
{
    km_cal_capture(m->engine, m->rec_valid ? &m->rec : NULL, &m->pending);
    km_cal_snapshot(m->engine, m->ref_pending);
    m->due = false;
    const km_clog_result_t r = km_cal_job_begin(&m->job, m->log, &m->pending);
    if (r == KM_CLOG_UNCHANGED) {                   /* the flash already holds exactly this */
        m->rec = m->pending;
        m->rec_valid = true;
        memcpy(m->ref, m->ref_pending, sizeof m->ref);
        km_cal_policy_saved(&m->pol, now_s);
    } else if (r != KM_CLOG_PENDING) {
        m->last = (uint8_t)r;
        m->saves_failed++;
        km_cal_policy_saved(&m->pol, now_s);
    }
    return r;
}

void km_calmgr_second(km_calmgr_t *m, uint32_t now_s)
{
    if (m->log != NULL && !km_clog_job_active(&m->job) && m->engine->calibrated &&
        km_cal_policy_due(&m->pol, m->engine, m->ref, now_s)) {
        m->due = true;
    }
}

bool km_calmgr_pass(km_calmgr_t *m, uint32_t now_s, bool idle_now, bool idle_long)
{
    if (m->log == NULL) {
        return false;
    }
    if (km_clog_job_active(&m->job)) {
        if (km_clog_job_must_finish(&m->job) || idle_now) {
            (void)job_step(m, now_s);
            return true;
        }
        return false;
    }
    if (m->due && m->engine->calibrated && idle_long) {
        (void)job_start(m, now_s);                  /* reads only: the first flash operation comes with the next pass */
    }
    return false;
}

km_calmgr_status_t km_calmgr_save_request(km_calmgr_t *m, uint32_t now_s, bool idle_now)
{
    if (m->log == NULL || !m->engine->calibrated || !idle_now) {
        return KM_CALMGR_BUSY;
    }
    if (!km_cal_policy_request(&m->pol, now_s)) {
        return KM_CALMGR_BUSY;                      /* at most one write attempt per KM_CAL_REQUEST_GAP_S: a host cannot wear the flash out */
    }
    km_clog_result_t r = KM_CLOG_PENDING;
    if (!km_clog_job_active(&m->job)) {
        if (!km_cal_changed(m->engine, m->ref)) {
            return KM_CALMGR_OK;                    /* nothing new was learned since the last save */
        }
        r = job_start(m, now_s);
    }
    while (r == KM_CLOG_PENDING) {
        r = job_step(m, now_s);
    }
    return (r == KM_CLOG_OK || r == KM_CLOG_UNCHANGED) ? KM_CALMGR_OK : KM_CALMGR_FAILED;
}

bool km_calmgr_job_active(const km_calmgr_t *m)
{
    return km_clog_job_active(&m->job);
}

bool km_calmgr_changed(const km_calmgr_t *m)
{
    return km_cal_changed(m->engine, m->ref);
}
