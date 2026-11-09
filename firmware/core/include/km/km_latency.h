/*
 * km_latency.h - the press-to-report probe of the M6 latency work (LATENCY_NOTES.md, section 1).
 *
 * Pure logic, no hardware: the main loop feeds it the pressed-slot list after every engine update and the moment a keyboard
 * report was handed to the USB endpoint; it keeps the time from the first "new press seen by the engine" to the next report
 * that goes out, as CPU cycles (240 MHz). The probe only counts: it never changes what is sent or when.
 *
 * What it measures: engine-visible press -> report accepted by the endpoint (the rest of the main loop, the report building
 * and the sync logic). What it CANNOT see: the physical travel before the engine (scan frames, debounce) and the USB poll
 * wait after the endpoint accepted the report (up to 1 ms at bInterval 4). Those are measured from outside (LATENCY_NOTES.md).
 *
 * Built into images only with KM_M6_LAT_PROBE = 1 (km_m6p); the default images do not contain it.
 */
#ifndef KM_LATENCY_H
#define KM_LATENCY_H

#include <stdint.h>

#define KM_LAT_RING       8u       /* presses waiting for a report; a press that finds the ring full is counted as dropped */
#define KM_LAT_SLOT_WORDS 3u       /* 3 x 32 bits = slots 0..95 (the keyboard has 76 slots) */
#define KM_LAT_VALUES     8u       /* values of the scope tag 0x0600 */
#define KM_LAT_SCOPE_TAG  0x0600u

typedef struct {
    uint32_t press[KM_LAT_RING];           /* cycles of new presses not yet followed by a report, oldest first */
    uint8_t  pending;                      /* how many entries of press[] are in use */
    uint32_t prev[KM_LAT_SLOT_WORDS];      /* slots that were down at the last observe */
    uint32_t samples;                      /* reports that answered at least one press */
    uint32_t last, min, max;               /* cycles from the oldest waiting press to the report (last / smallest / largest) */
    uint32_t presses;                      /* new down edges seen by the engine */
    uint32_t dropped;                      /* presses that found the ring full */
    uint32_t sends;                        /* reports handed to the endpoint (all, including heartbeats) */
} km_lat_t;

void km_lat_init(km_lat_t *l);

/* One pass with the pressed-slot list the engine reports (slots 0..95 are kept; others ignored). Every slot that is down now and
 * was up at the previous call is a new press at time `now`. */
void km_lat_observe(km_lat_t *l, uint32_t now, const uint8_t *down, unsigned n);

/* A keyboard report went to the endpoint at `now`: every waiting press is answered by it. */
void km_lat_sent(km_lat_t *l, uint32_t now);

/* Scope values (tag KM_LAT_SCOPE_TAG), microseconds with cycles_per_us cycles per microsecond, saturated at 65535:
 *   [0] samples  [1] last  [2] min  [3] max  [4] presses  [5] dropped  [6] sends (low 16 bits)  [7] pending now */
void km_lat_pack(const km_lat_t *l, uint32_t cycles_per_us, uint16_t out[KM_LAT_VALUES]);

#endif
