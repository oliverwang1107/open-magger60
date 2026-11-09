/*
 * km_report_sync.h - report reconciliation (M5 task T-E, rule 2). Original work, pure logic, no hardware.
 *
 * The host only knows what the keyboard told it last. If a report is lost, refused or never leaves the endpoint, the host keeps
 * a key "down" that is up (a stuck key: the observed 50 s space bar). This module keeps one copy of the last state the driver
 * ACCEPTED and decides, on every pass of the main loop, whether to send something:
 *
 *   - the target differs from what the host last got                        -> send the target
 *   - the target is all-released because keys are not allowed (fault)       -> send all-released, and keep it that way
 *   - a send failed                                                         -> try again on the next pass, counted in `retries`,
 *                                                                              without limit: a release must get through
 *   - any key is down and nothing was sent for KM_SYNC_HEARTBEAT_CYCLES    -> send the same state again (the host ignores
 *                                                                              repeats; it heals a report that was lost silently)
 *   - the link came back (enumeration, bus reset, resume from suspend)     -> the host's state is unknown, send the target
 *                                                                              even if it equals the old state (`resyncs`)
 *   - all keys were just released                                          -> the release is sent once more after the heartbeat
 *                                                                              time (the "echo"; heals a release that was accepted
 *                                                                              by the driver but never reached the host)
 *   - the endpoint stays busy for KM_SYNC_BUSY_LIMIT_CYCLES with a report waiting -> ask the driver to recover the endpoint
 *                                                                              (`recoveries`), again after every further limit
 *   - nothing is accepted for KM_SYNC_DEAD_CYCLES with the link up          -> km_report_sync_stalled(): the caller lets the
 *                                                                              watchdog reset the keyboard
 *
 * The target is the desired report while keys are allowed, and the all-released report otherwise. The synchronizer never queues:
 * it always sends the CURRENT target.
 */
#ifndef KM_REPORT_SYNC_H
#define KM_REPORT_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#define KM_SYNC_MAX_LEN            32u
#define KM_SYNC_HEARTBEAT_CYCLES   (500u * 240000u)     /* 500 ms at 240 MHz */
#define KM_SYNC_BUSY_LIMIT_CYCLES  (100u * 240000u)     /* 100 ms; the host polls the endpoint every 1 ms */
#define KM_SYNC_DEAD_CYCLES        (1000u * 240000u)    /* 1 s without delivering what is due = the USB side is dead */
#define KM_SYNC_LATE_CYCLES        30000u               /* 125 us at 240 MHz: one 8 kHz micro-frame. A report accepted later than this after it
                                                           became due is counted as `late` (USB8K_NOTES.md) */

typedef struct {
    uint8_t  released[KM_SYNC_MAX_LEN];   /* the all-released report */
    uint8_t  last[KM_SYNC_MAX_LEN];       /* last report the driver accepted */
    uint8_t  len;
    bool     force;                       /* host state unknown: send the target even if it equals `last` */
    bool     link_was_up;
    bool     busy_timing;
    uint32_t busy_since;
    uint32_t last_tx;                     /* cycle counter at the last accepted send */
    bool     undelivered;                 /* something is due and has not been accepted yet (link up) */
    uint32_t undelivered_since;
    bool     echo_pending;                /* a release was sent: repeat it once after the heartbeat time */
    bool     had_down;                    /* `last` has a key down */
    bool     blocked_noted;               /* this undelivered episode was already counted in `deferred` */
    uint8_t  pend[KM_SYNC_MAX_LEN];       /* the target of the previous pass while undelivered (to notice a change inside the wait) */
    /* counters */
    uint32_t sent, retries, resyncs, heartbeats, recoveries, forced_releases;
    /* latency counters (8 kHz work): only about reports that were due because the KEYS changed or a resync, not heartbeats */
    uint32_t deferred;                    /* episodes where something was due but the endpoint was still busy with the previous report */
    uint32_t coalesced;                   /* the target changed again before the due one was accepted: the in-between state never reached the host */
    uint32_t late;                        /* due -> accepted took longer than KM_SYNC_LATE_CYCLES */
    uint32_t wait_last, wait_max;         /* cycles from due to accepted: last / largest */
} km_report_sync_t;

/* The link to the driver. All callbacks get `ctx`. */
typedef struct {
    void *ctx;
    bool (*link_up)(void *ctx);                                          /* configured and not suspended */
    bool (*tx_ready)(void *ctx);                                         /* the previous report has left the endpoint */
    bool (*send)(void *ctx, const uint8_t *report, unsigned len);        /* true = the driver accepted it */
    void (*recover)(void *ctx);                                          /* flush a stuck endpoint, make tx_ready true again */
} km_sync_io_t;

/* `released` is the all-released report (len <= KM_SYNC_MAX_LEN bytes, including a report-id byte if there is one). */
void km_report_sync_init(km_report_sync_t *s, const uint8_t *released, unsigned len, uint32_t now_cycles);

/* The host's state is unknown (e.g. protocol switch): the next pass sends the target. */
void km_report_sync_resync(km_report_sync_t *s);

/* One pass. `desired` is the report for the current key state; it is replaced by the all-released report when !keys_allowed. */
void km_report_sync_pump(km_report_sync_t *s, const km_sync_io_t *io, const uint8_t *desired, bool keys_allowed, uint32_t now_cycles);

/* true when something has been due for KM_SYNC_DEAD_CYCLES with the link up and the driver has not taken it: the endpoint is
 * dead. The main loop then stops reporting USB progress to km_health, so the watchdog resets the keyboard (a reset drops the
 * USB connection, which releases every key on the host). Never true while the link is down (suspend, not configured). */
bool km_report_sync_stalled(const km_report_sync_t *s, uint32_t now_cycles);

#endif /* KM_REPORT_SYNC_H */
