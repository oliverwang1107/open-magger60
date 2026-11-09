/*
 * km_calmgr.h - the calibration persistence manager (M5 task T-F): boot restore, background save, host command.
 * Original work, hardware-free: the engine, the flash log and the "something stalled the CPU" hook are passed in, time is
 * passed in as seconds, so the whole behaviour (including the power-loss sweeps) is tested on the host. m5_main.c (km_l33) owns
 * one of these and only supplies the facts it knows: whether keys are down, whether a fault is active, the clock.
 *
 * Lifecycle
 *   init            find the log, load the newest record (g_rec). A record that decodes arms the boot restore.
 *   boot            when the engine is not calibrated and a live frame is available: km_calmgr_try_restore(). true = the engine is
 *                   calibrated from that one frame (the 64-frame wait is skipped); false = calibrate normally and call
 *                   km_calmgr_armed(true) when that succeeded (it puts the stored strokes back where the sensor agrees).
 *   recalibration   km_calmgr_keep_learned() BEFORE the engine is uncalibrated, km_calmgr_armed(true) after: the learned strokes survive.
 *   running         km_calmgr_second() once per second (the policy of km_calib.h), km_calmgr_pass() once per main loop pass.
 *                   The save is a km_calstore job: one flash line (two units) per pass, started only when idle_long, continued only when
 *                   idle_now, except that a job that has already erased the sector runs on (see km_calstore.h).
 *   host            km_calmgr_save_request() = the protocol's save_calibration ("SCAL"): to the end, now, BUSY when not idle_now or
 *                   when the previous attempt was less than KM_CAL_REQUEST_GAP_S ago; OK also when nothing new was learned.
 */
#ifndef KM_CALMGR_H
#define KM_CALMGR_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_calib.h"

typedef enum {
    KM_CALMGR_OK = 0,
    KM_CALMGR_BUSY,         /* not now (key down, fault, not calibrated, or asked again too soon): the host may retry */
    KM_CALMGR_FAILED        /* the flash refused or the read-back differed; the previous record is still the newest valid one */
} km_calmgr_status_t;

/* boot result, km_calmgr_t.boot */
#define KM_CALMGR_BOOT_NONE      0u    /* no usable record */
#define KM_CALMGR_BOOT_RESTORED  1u
#define KM_CALMGR_BOOT_DISAGREED 2u    /* the record did not match the live sensor: dropped */
#define KM_CALMGR_BOOT_REFUSED   3u    /* the engine refused the live frame (keys held, railed inputs) */

typedef struct {
    const km_clog_t *log;
    km_engine_t     *engine;
    void           (*after_flash)(void *ctx);   /* called after every flash operation (the CPU was stalled): restart the scan chain */
    void            *ctx;

    km_cal_record_t rec;            /* the learned state: loaded from flash, updated at every save and every recalibration */
    bool            rec_valid;
    km_cal_record_t pending;        /* the record of the job in flight */
    uint16_t        ref[KM_CAL_SLOTS];          /* bottoms at the last save / calibration */
    uint16_t        ref_pending[KM_CAL_SLOTS];
    km_cal_policy_t pol;
    km_clog_job_t   job;
    bool            try_restore;
    bool            due;            /* the policy wants a save; it starts at the next idle moment */
    uint8_t         boot;           /* KM_CALMGR_BOOT_* */
    uint8_t         last;           /* last km_clog_result_t of a save */
    uint16_t        applied;        /* strokes the boot restore applied */
    uint32_t        saves_ok, saves_failed;
    km_clog_scan_t  scan0;          /* the log as found at boot */
} km_calmgr_t;

void km_calmgr_init(km_calmgr_t *m, const km_clog_t *log, km_engine_t *e, void (*after_flash)(void *ctx), void *ctx);

bool km_calmgr_wants_restore(const km_calmgr_t *m);
bool km_calmgr_try_restore(km_calmgr_t *m, const uint16_t live[KM_CAL_SLOTS]);
void km_calmgr_armed(km_calmgr_t *m, bool apply_stored);
void km_calmgr_keep_learned(km_calmgr_t *m);

void km_calmgr_second(km_calmgr_t *m, uint32_t now_s);
/* true = a flash operation was done in this pass. idle_now: no key down, no fault. idle_long: idle_now plus a quiet second, no report waiting. */
bool km_calmgr_pass(km_calmgr_t *m, uint32_t now_s, bool idle_now, bool idle_long);

km_calmgr_status_t km_calmgr_save_request(km_calmgr_t *m, uint32_t now_s, bool idle_now);

bool km_calmgr_job_active(const km_calmgr_t *m);
/* the policy's "changed noticeably" right now (diagnostics) */
bool km_calmgr_changed(const km_calmgr_t *m);

#endif /* KM_CALMGR_H */
