/*
 * km_usbfast.h - helpers of the 8 kHz USB work (USB8K_NOTES.md). Pure logic, no hardware.
 *
 *  km_defer_t   : "a key report just went out, so the slow housekeeping (lights, light stream, animation) waits" - with a hard
 *                 upper bound, so housekeeping can never starve (at most one run per `max_cycles` while key events keep coming).
 *  km_ustat_t   : how long a report keeps the NKRO endpoint busy, from "handed to the endpoint" to "the main loop saw the endpoint
 *                 free again". At 1 kHz polling this spreads over 0..1 ms; at 8 kHz it must pile up in the first buckets. The value
 *                 is as seen by the main loop (so it also contains the main loop's own delay in noticing); it is the number that
 *                 matters for "can the next report go out".
 */
#ifndef KM_USBFAST_H
#define KM_USBFAST_H

#include <stdbool.h>
#include <stdint.h>

/* ---- deferral of slow work around key events ---- */
typedef struct {
    bool     active;          /* inside a deferral window */
    uint32_t since;           /* cycles when the window began */
    uint32_t skipped;         /* passes in which the slow work was skipped */
    uint32_t forced;          /* windows that ran out (the work was let through although key events kept coming) */
} km_defer_t;

void km_defer_init(km_defer_t *d);

/* One pass. event = a key report was sent (or is still waiting for the endpoint) in this pass. Returns true when the slow work may run
 * in this pass. A pass without an event always lets it run. During a stretch of event passes it is skipped until `max_cycles` have
 * passed since the first of them, then it runs once and the window restarts. */
bool km_defer_slow_ok(km_defer_t *d, bool event, uint32_t now, uint32_t max_cycles);

/* ---- endpoint busy time ---- */
#define KM_USTAT_BUCKETS 6u       /* <=62 us, <=125, <=250, <=500, <=1000, >1000 (edges in cycles at 240 MHz) */
#define KM_USTAT_UF      6u
#define KM_USTAT_VALUES  8u       /* scope tag 0x0601 */
#define KM_USTAT_SCOPE_TAG 0x0601u

typedef struct {
    bool     busy;
    uint32_t t_sent;
    uint32_t count;           /* completed measurements */
    uint32_t bucket[KM_USTAT_BUCKETS];
    uint32_t max;             /* cycles */
    uint32_t last;
    uint32_t t_sof;           /* micro-frame counter (SOFs seen) at the send */
    uint32_t uf[KM_USTAT_UF]; /* how many micro-frames passed between the send and the endpoint being seen free: 0, 1, 2, 3-4, 5-7, 8+ */
    uint32_t uf_max;
} km_ustat_t;

void km_ustat_init(km_ustat_t *u);
/* a report was handed to the endpoint at `now` */
void km_ustat_sent(km_ustat_t *u, uint32_t now, uint32_t sof);
/* the main loop looked at the endpoint at `now`; `ready` = it is free */
void km_ustat_observe(km_ustat_t *u, uint32_t now, bool ready, uint32_t sof);
/* [0] count (low 16)  [1..6] buckets (saturated 65535)  [7] max in microseconds (saturated) */
void km_ustat_pack(const km_ustat_t *u, uint32_t cycles_per_us, uint16_t out[KM_USTAT_VALUES]);


/* Micro-frame view (scope tag 0x0603): [0..5] counts for 0, 1, 2, 3-4, 5-7, 8+ micro-frames between send and "free", [6] total, [7] largest.
 * A host that polls every 125 us puts nearly everything into 0 and 1; a host that polls every 1 ms spreads it evenly over 0..7 (8 micro-frames). */
#define KM_USTAT_SCOPE_TAG_UF 0x0603u
void km_ustat_pack_uf(const km_ustat_t *u, uint16_t out[KM_USTAT_VALUES]);

/* bInterval of the IN/OUT endpoint `addr` in a configuration descriptor (0 if there is no such endpoint or the descriptor is damaged). */
unsigned km_usb_cfg_binterval(const uint8_t *cfg, unsigned len, uint8_t addr);

#endif
