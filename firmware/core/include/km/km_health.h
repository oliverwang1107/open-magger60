/*
 * km_health.h - health monitoring and fail-safe policy (M5 task T-E). Original work, pure logic: no hardware, time is passed in
 * as a free-running 32-bit CPU cycle counter (DWT CYCCNT, 240 MHz, wraps every 17.9 s), so everything is testable on the host.
 *
 * The rules this module implements (M5_PLAN.md section 2A):
 *   1. Any failure path first makes km_health_keys_allowed() false. The main loop then sends an all-released report and
 *      keeps sending it (km_report_sync.h) until the flag is cleared again. A stopped keyboard is acceptable, a stuck key is not.
 *   5. Progress-gated watchdog feed. The 2 s IWDG is fed only while the scan chain AND the USB poll have both advanced within the
 *      last KM_HEALTH_FEED_WINDOW (500 ms). A stalled scan or a dead poll therefore ends in a watchdog reset 2.0 .. 2.5 s after the
 *      stall began (the last feed happens at most one window after the last progress). The bootloader that runs after the reset
 *      restarts the IWDG itself and jumps back to the app, so this is a restart, not a brick.
 *   7. Everything is counted in km_health_t and can be published (km_health_pack_scope(), scope tag 0x0300).
 *
 * Fault flags (km_health_t.fault_flags), a keyboard may report keys only while NONE is set:
 *   SCAN_STALL      no ADC frame for the scan timeout; cleared when frames flow again and the hold-off has passed
 *   DMA_ERROR       the scan reported a DMA transfer error; cleared after the hold-off with clean frames
 *   CAL_LOST        no valid calibration (boot, or the engine lost it); cleared by km_health_on_calibration(true)
 *   USB_SUSPENDED   the bus is suspended (host asleep / hub off); cleared on resume or bus reset
 *   ENGINE_OVERRUN  one engine update took longer than KM_HEALTH_ENGINE_LIMIT_CYCLES; cleared after the hold-off
 * The hold-off (KM_HEALTH_HOLDOFF_CYCLES, 30 ms) keeps keys released while the 16-point sums and the filters flush out
 * whatever a bad frame left in them (16 frames = 0.7 ms, filter and debounce a few frames more).
 */
#ifndef KM_HEALTH_H
#define KM_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#define KM_CPU_HZ        240000000u
#define KM_MS_CYCLES     (KM_CPU_HZ / 1000u)

#define KM_HEALTH_FEED_WINDOW_CYCLES   (500u * KM_MS_CYCLES)
#define KM_HEALTH_HOLDOFF_CYCLES       (30u * KM_MS_CYCLES)
#define KM_HEALTH_ENGINE_BUDGET_CYCLES 10400u    /* one frame at 23 kHz; above it the update is only counted */
#define KM_HEALTH_ENGINE_LIMIT_CYCLES  60000u    /* 250 us = about 6 frames: not scheduling noise, a fault */

/* what km_health_progress() reports */
#define KM_PROG_SCAN    0x01u    /* a new ADC frame was taken */
#define KM_PROG_USB     0x02u    /* km_usbhs_poll() ran */
#define KM_PROG_ENGINE  0x04u    /* the engine was updated (informational, not required for the feed) */

#define KM_FAULT_SCAN_STALL     0x01u
#define KM_FAULT_DMA_ERROR      0x02u
#define KM_FAULT_CAL_LOST       0x04u
#define KM_FAULT_USB_SUSPENDED  0x08u
#define KM_FAULT_ENGINE_OVERRUN 0x10u

/* reset_cause: the top byte of the reset/status register as read at boot (UNVERIFIED layout, see reset_cause.c), decoded by
 * km_health_reset_cause_name() under the assumption that the layout is the usual one. 0 = no flag set / not readable. */
#define KM_RESET_RMV     0x01u    /* bit 24 - the clear request, reads back as 0 normally */
#define KM_RESET_PIN     0x04u    /* bit 26 */
#define KM_RESET_POR     0x08u    /* bit 27 */
#define KM_RESET_SOFT    0x10u    /* bit 28 */
#define KM_RESET_IWDG    0x20u    /* bit 29 */
#define KM_RESET_WWDG    0x40u    /* bit 30 */
#define KM_RESET_LPWR    0x80u    /* bit 31 */

typedef struct {
    uint32_t frames, scan_stalls, dma_errors, scan_restarts;   /* scan chain */
    uint32_t engine_max_cycles, engine_over_budget;            /* engine time budget */
    uint32_t usb_resets, usb_suspends, report_retries, report_resyncs;
    uint32_t glitches, calibrations, calibration_refused;
    uint8_t  reset_cause;                                      /* this boot's reset cause (RCC flags, top byte) */
    uint8_t  fault_flags;                                      /* faults active now */

    /* ---- below: bookkeeping, treat as private ---- */
    uint32_t usb_resumes;
    uint32_t engine_overruns;       /* updates above KM_HEALTH_ENGINE_LIMIT_CYCLES */
    uint32_t scan_ts, usb_ts, engine_ts;   /* cycle counter at the last progress of each part */
    uint32_t hold_until;            /* transient faults stay set until this time */
    uint8_t  prog_pending;          /* progress reported since the last km_health_should_feed() */
    uint8_t  require;               /* parts that must have progressed within the window (default SCAN | USB) */
    uint8_t  transient;             /* fault bits that clear by themselves after the hold-off */
    bool     frame_seen_since_stall;
    bool     usb_susp;              /* suspend state as last told (km_health_set_usb_suspended / km_health_on_usb) */
} km_health_t;

/* Start state: CAL_LOST set (no calibration yet), the feed window starts now. */
void km_health_init(km_health_t *h, uint32_t now_cycles, uint8_t reset_cause);

/* ---- progress-gated watchdog feed ---- */
void km_health_progress(km_health_t *h, unsigned what);        /* KM_PROG_* bits, call from the main loop */
/* Call once per loop pass. true = every required part has progressed within KM_HEALTH_FEED_WINDOW_CYCLES of `now`:
 * the caller may feed the IWDG. */
bool km_health_should_feed(km_health_t *h, uint32_t now_cycles);

/* Failure safe state: true only while no fault flag is set. */
bool km_health_keys_allowed(const km_health_t *h);

/* Call once per loop pass: clears transient faults whose hold-off has passed. */
void km_health_tick(km_health_t *h, uint32_t now_cycles);

/* ---- events ---- */
void km_health_on_frame(km_health_t *h);                                  /* an ADC frame was taken (counts + KM_PROG_SCAN) */
/* No frame for the scan timeout: raise SCAN_STALL, count it. Returns true = restart the scan chain (counted as a restart). */
bool km_health_on_scan_timeout(km_health_t *h, uint32_t now_cycles);
void km_health_on_dma_error(km_health_t *h, uint32_t now_cycles, uint32_t new_errors);
/* One engine update took `cycles`. Tracks the maximum, counts budget overruns, raises ENGINE_OVERRUN above the limit. */
void km_health_on_engine(km_health_t *h, uint32_t now_cycles, uint32_t cycles);
void km_health_on_calibration(km_health_t *h, bool ok);                   /* a calibration attempt finished */
void km_health_cal_lost(km_health_t *h);                                  /* the engine is no longer calibrated */
void km_health_set_glitches(km_health_t *h, uint32_t engine_glitch_count);

typedef enum { KM_USB_EV_RESET = 1, KM_USB_EV_SUSPEND, KM_USB_EV_RESUME } km_usb_event_t;
void km_health_on_usb(km_health_t *h, km_usb_event_t ev);
/* The bus suspend state as a level (the driver's DSTS.SUSPSTS); call every pass. Only changes raise suspend / resume events. */
void km_health_set_usb_suspended(km_health_t *h, bool suspended);
/* Totals of the report synchronizer (km_report_sync.h), copied in for the scope. */
void km_health_set_report_stats(km_health_t *h, uint32_t retries, uint32_t resyncs);

/* ---- reset cause ---- */
/* Top byte of the reset/status register -> KM_RESET_* flags (bit 24 is the clear request and is dropped). */
uint8_t km_health_reset_cause_from_reg(uint32_t reg);
const char *km_health_reset_cause_name(uint8_t cause);    /* "iwdg", "por", ... "none" */

/* ---- scope output ---- */
#define KM_SCOPE_TAG_HEALTH  0x0300u   /* 24 values: km_health_pack_scope() */
#define KM_SCOPE_TAG_KEYMASK 0x0301u   /* 8 values: km_health_pack_keymask() */
#define KM_HEALTH_SCOPE_VALUES  24u
#define KM_KEYMASK_SCOPE_VALUES 8u

/* tag 0x0300, 24 values (u16, counters saturate at 0xFFFF; "lo/hi" = 32-bit value split):
 *   [0] fault_flags | reset_cause << 8   [1] frames lo  [2] frames hi  [3] scan_stalls  [4] dma_errors  [5] scan_restarts
 *   [6] engine_max_cycles  [7] engine_over_budget  [8] engine_overruns  [9] usb_resets  [10] usb_suspends  [11] usb_resumes
 *   [12] report_retries  [13] report_resyncs  [14] glitches  [15] calibrations  [16] calibration_refused
 *   [17..23] extra[0..6] as given by the caller (the m5_stab image: reports sent (lo 16 bits), heartbeats, endpoint recoveries,
 *            previous run's fault flags (breadcrumb, 0xFFFF = none), boot count, uptime in seconds, longest engine update
 *            INCLUDING the scan interrupts that ran during it; engine_max_cycles [6] is the engine's own time) */
void km_health_pack_scope(const km_health_t *h, const uint16_t extra[7], uint16_t out[KM_HEALTH_SCOPE_VALUES]);

/* tag 0x0301, 8 values: [0..4] bit i of the 80-bit mask (value k bit b = slot 16k + b) = slot i is in the key state the keyboard is
 * currently trying to report (all zero while keys are not allowed);  [5] sequence number (+1 at every publish, wraps)
 * [6] fault_flags  [7] 1 = the last report has not been delivered yet. Slots >= 76 are ignored. */
void km_health_pack_keymask(const uint8_t *down_slots, unsigned n, uint16_t seq, uint8_t fault_flags, bool undelivered,
                            uint16_t out[KM_KEYMASK_SCOPE_VALUES]);

#endif /* KM_HEALTH_H */
