/*
 * km_ledstore.h - persistence of the light configuration (L5 follow-up, LED_NOTES.md section 15): effect, brightness, speed, colour
 * and the idle-off time, kept in flash and restored at boot. Original work, hardware-free: the flash is a km_clog_t (km_calstore.h),
 * so the host tests run this code against the simulated flash that loses power at every erase / program step.
 *
 * Storage: an append-only km_clog log in its own sector (0x08036000, km_flash_guard.h). One record = KM_LEDSTORE_PAYLOAD_BYTES = 16 bytes,
 * one slot = 32 bytes, so a sector holds 256 saves and the erase happens once in 256 saves. A save normally programs ONE slot (four
 * 8-byte units) and erases nothing.
 *
 * Payload (all fields validated when it is read back; anything else = no record = built-in defaults, never a crash):
 *   0      u8   format version, 1
 *   1      u8   effect (km_led_fx_t, < KM_LED_FX_COUNT)
 *   2      u8   brightness
 *   3      u8   speed (1..255)
 *   4..6   u8   red, green, blue
 *   7      u8   0
 *   8..9   u16  idle-off time in seconds, little endian, 0 = never
 *   10..15 u8   0 (reserved; a record with anything else here is rejected, so a future version can not be misread by this one)
 *
 * When it is written (km_ledstore_pass, called once per main-loop pass by the integration):
 *   - only after km_ledstore_request() (the host's LED_SET with the save flag); the live LED_SET without it never touches flash;
 *   - requests are COALESCED: the record to write is always the latest request, and nothing starts before KM_LEDSTORE_SETTLE_S seconds
 *     have passed since the last request (a slider that is being dragged sends a request per step; one record is written after it stops);
 *   - a job starts only when the keyboard has been idle for a while (idle_long) and never within KM_LEDSTORE_MIN_GAP_S seconds of the
 *     previous start, so a host can not wear the flash out; a record equal to the newest one in flash writes nothing at all;
 *   - the job is a km_clog job: ONE flash operation per pass (one 8-byte program, or the sector erase once in 256 saves), paused while a
 *     key is down or a fault is active, except that a job that has already erased the sector runs on (until the last unit is in, no
 *     record exists);
 *   - a failed save is retried at most KM_LEDSTORE_MAX_TRIES times in all, spaced by the gap; then it gives up until the next request.
 * The lights keep running during all of it: this module does not touch the LED pipeline.
 */
#ifndef KM_LEDSTORE_H
#define KM_LEDSTORE_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_calstore.h"
#include "km/km_led.h"

#define KM_LEDSTORE_PAYLOAD_BYTES  16u
#define KM_LEDSTORE_VERSION        1u
#define KM_LEDSTORE_SETTLE_S       3u     /* quiet time after the last request before a save may start (a whole-second counter: 2..3 s) */
#define KM_LEDSTORE_MIN_GAP_S      10u    /* least time between two save starts */
#define KM_LEDSTORE_MAX_TRIES      3u
#define KM_LEDSTORE_WANT_MAX       64u    /* the largest payload the scheduler can carry: the per-profile table of km_ledprof.h */

typedef struct {
    km_led_cfg_t cfg;
    uint16_t     idle_s;      /* 0 = never */
} km_ledstore_rec_t;

/* The built-in defaults: km_led_cfg_default() and KM_LED_IDLE_DEFAULT_S. */
void km_ledstore_rec_default(km_ledstore_rec_t *r);
/* effect and speed valid (km_led_cfg_valid); any brightness, colour and idle time is fine. */
bool km_ledstore_rec_valid(const km_ledstore_rec_t *r);
/* Serialise into buf (cap >= KM_LEDSTORE_PAYLOAD_BYTES); returns the payload length, 0 = invalid record or cap too small. */
unsigned km_ledstore_encode(const km_ledstore_rec_t *r, uint8_t *buf, unsigned cap);
/* Parse a payload strictly (length, version, ranges, zero bytes). false = *r untouched. */
bool km_ledstore_decode(km_ledstore_rec_t *r, const uint8_t *buf, unsigned len);
/* Newest valid record of the log. false = none or unusable (the caller keeps the defaults); *r untouched then. */
bool km_ledstore_load(const km_clog_t *log, km_ledstore_rec_t *r);

typedef struct {
    const km_clog_t *log;                 /* NULL = no persistence */
    void           (*after_flash)(void *ctx);    /* called after every flash operation (the CPU was stalled): restart the scan chain */
    void            *ctx;
    km_clog_job_t    job;
    uint8_t          want[KM_LEDSTORE_WANT_MAX];    /* the latest request, encoded (the first log->payload_len bytes are used) */
    uint32_t         req_seq;             /* counts requests */
    uint32_t         saved_seq;           /* the request the newest saved / confirmed record corresponds to */
    uint32_t         job_seq;             /* the request the job in flight carries */
    uint32_t         now_s;               /* the time of the latest pass */
    uint32_t         last_req_s;          /* second of the first pass after the latest request */
    uint32_t         last_start_s;
    bool             started_once;
    bool             req_fresh;           /* a request arrived since the last pass: the next pass stamps last_req_s */
    bool             gave_up;
    bool             loaded;              /* a valid record was found at boot */
    uint8_t          tries;
    uint8_t          last;                /* km_clog_result_t of the latest save attempt (0 = ok) */
    uint16_t         saves_ok, saves_failed;
} km_ledstore_t;

void km_ledstore_init(km_ledstore_t *m, const km_clog_t *log, void (*after_flash)(void *ctx), void *ctx);
/* Read the newest record: true = *r was filled (the caller applies it). Call once at boot, before the first pass. */
bool km_ledstore_boot(km_ledstore_t *m, km_ledstore_rec_t *r);
/* The host wants this configuration kept. false = invalid record or no persistence (nothing changes). Cheap: RAM only. */
bool km_ledstore_request(km_ledstore_t *m, const km_ledstore_rec_t *r);
/* The same for a log whose payload is not the 16-byte record (km_ledprof.h: the 64-byte profile table): len must equal the log's payload length. */
bool km_ledstore_request_raw(km_ledstore_t *m, const uint8_t *payload, unsigned len);
/* Once per main-loop pass. idle_now: no key down, no fault. idle_long: idle_now plus a quiet second. true = one flash operation was done. */
bool km_ledstore_pass(km_ledstore_t *m, uint32_t now_s, bool idle_now, bool idle_long);
bool km_ledstore_job_active(const km_ledstore_t *m);
/* true while a job that has already erased the sector is running: it must be given the next pass (no valid record exists meanwhile) */
bool km_ledstore_must_finish(const km_ledstore_t *m);
/* true while a requested record is not yet in flash (waiting, or being written) and the module has not given up. A request that arrives
 * while a save is running stays pending after that save ends: the job carries the request it was started with. */
bool km_ledstore_pending(const km_ledstore_t *m);

#endif /* KM_LEDSTORE_H */
