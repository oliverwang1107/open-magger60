/*
 * km_calstore.h - power-fail-safe append-only record log inside ONE 8 KB flash sector (M5 task T-F).
 * Original work. Hardware-free: all flash access goes through km_store_ops_t (km_store.h), so the host tests run
 * the same code against a simulated flash that loses power at every single erase/program step.
 *
 * Why a log and not km_store's two-sector scheme: the calibration lives in a single sector (0x08034000). A
 * two-slot ping-pong inside one sector would need an erase for EVERY save, and an erase destroys the only
 * copy. Here a save normally erases nothing: it programs one more record behind the previous ones, and the
 * sector is erased only when it is full (one erase per `slots` saves, 24 for the calibration record), which also divides the wear by that number.
 *
 * Layout: the sector is cut into slots of a fixed size (stride), slot 0 at the sector start. All slots have the
 * same payload length, fixed when the log is created; the tail that does not fit a whole slot stays erased.
 *
 *   slot offset  size  field                                              programmed
 *   0            4     magic "KMCL" (4B 4D 43 4C)                          3. LAST  (one 8-byte unit with the CRC)
 *   4            4     CRC32 over bytes 8 .. 16+payload_len-1               3. LAST
 *   8            4     sequence number                                      2. second to last
 *   12           4     payload length in bytes (the log's fixed length)     2.
 *   16           n     payload, padded with 0xFF to a multiple of 8         1. first, in address order
 *
 * Classification of a slot when the log is scanned:
 *   VALID  magic, length and CRC are right. Only a slot whose commit unit was programmed completely can be.
 *   BLANK  every byte is 0xFF: untouched since the erase, may be programmed.
 *   DIRTY  anything else (a write that lost power, a failed program, foreign data): never programmed again,
 *          skipped until the next erase.
 * The newest record = the VALID slot with the newest sequence number (compared modulo 2^32). The next slot to
 * write = the one after the LAST non-blank slot (valid or dirty), so a half written slot is never reused and
 * a unit is never programmed twice between erases. When there is no such slot the sector is erased first.
 *
 * Power cut at any step (checked by tests/test_calstore.c at every erase/program step):
 *   - during a record write: the slot stays DIRTY or BLANK, the previous newest record is still there, intact;
 *   - between the start of the erase that clears a full sector and the programming of the new record's commit
 *     unit there is NO valid record (or, for a cut inside the erase itself, an older one): the load finds none.
 *     That is harmless for the calibration record (derived data: with none, the keyboard calibrates normally
 *     and re-learns) and it needs the cut to hit the one save in `slots` that has to erase, inside the erase time
 *     plus about 40 unit programs. The window is kept short on purpose: a job that has erased must be finished
 *     without pausing (km_clog_job_must_finish).
 * A save is verified by reading the slot back; a failed save leaves the previous record the current one.
 *
 * Incremental writing: km_clog_job_begin() plus km_clog_job_step() do the same as km_clog_append() but one flash
 * operation (one erase OR one 8-byte program OR the read-back) per step, so the main loop can keep scanning keys
 * between the steps and pause the job while a key is down.
 */
#ifndef KM_CALSTORE_H
#define KM_CALSTORE_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_store.h"     /* km_store_ops_t */

#define KM_CLOG_UNIT            8u
#define KM_CLOG_LINE            16u     /* flash line: two units that must be programmed back to back, lower first */
#define KM_CLOG_HEADER_BYTES    16u
#define KM_CLOG_SECTOR_BYTES    0x2000u
#define KM_CLOG_MAGIC           0x4C434D4Bu     /* bytes 4B 4D 43 4C "KMCL" as a little-endian u32 */
#define KM_CLOG_MAX_PAYLOAD     512u
#define KM_CLOG_MAX_STRIDE      (KM_CLOG_HEADER_BYTES + KM_CLOG_MAX_PAYLOAD)

typedef struct {
    km_store_ops_t ops;
    uint32_t sector;        /* base address of the sector, 8 KB aligned */
    uint32_t payload_len;   /* fixed payload length of every record */
    uint32_t stride;        /* slot size: header + payload rounded up to a multiple of 16 (a flash line) */
    uint32_t slots;         /* how many slots fit in the sector */
} km_clog_t;

typedef enum {
    KM_CLOG_OK = 0,         /* finished: written and verified (job_step), or loaded */
    KM_CLOG_UNCHANGED,      /* the newest record already equals the new payload: nothing written */
    KM_CLOG_PENDING,        /* job_begin started a job / job_step has more steps to do */
    KM_CLOG_ERR_ARG,
    KM_CLOG_ERR_READ,
    KM_CLOG_ERR_ERASE,
    KM_CLOG_ERR_PROGRAM,
    KM_CLOG_ERR_VERIFY      /* read-back differs: the previous record is still the newest valid one */
} km_clog_result_t;

typedef struct {
    bool     valid;         /* at least one valid record */
    uint32_t index;         /* slot of the newest valid record */
    uint32_t seq;           /* its sequence number */
    uint32_t next;          /* first slot that may be programmed; == slots: the sector must be erased first */
    uint32_t used;          /* valid records in the sector (older ones count too) */
    uint32_t dirty;         /* slots with unusable content */
} km_clog_scan_t;

/* false = the arguments do not make a log (payload_len 0 or above KM_CLOG_MAX_PAYLOAD, unaligned sector, no slot fits) */
bool km_clog_init(km_clog_t *log, const km_store_ops_t *ops, uint32_t sector, uint32_t payload_len);

/* Walk the sector (reads only). false = a read failed. */
bool km_clog_scan(const km_clog_t *log, km_clog_scan_t *out);
/* Copy the payload of the newest valid record into buf (cap >= payload_len). false = no record, cap too small or read error. */
bool km_clog_read(const km_clog_t *log, uint8_t *buf, uint32_t cap);

typedef struct {
    const km_clog_t *log;
    uint8_t  image[KM_CLOG_MAX_STRIDE];     /* the slot as it has to look when finished */
    uint32_t slot;
    uint32_t pos;           /* next byte offset inside the slot to program */
    uint32_t seq;
    uint8_t  phase;         /* private: idle / erase / payload / seq unit / commit unit / verify */
    bool     erased;        /* private: the sector was erased by this job; no valid record exists until the commit unit is in */
} km_clog_job_t;

/*
 * Plan a save of data (log->payload_len bytes). PENDING = a job was set up and needs km_clog_job_step() calls;
 * UNCHANGED = same as the newest record, nothing to do (the job stays idle); an error = nothing was written.
 */
km_clog_result_t km_clog_job_begin(km_clog_job_t *job, const km_clog_t *log, const uint8_t *data);
/* Do ONE flash operation. PENDING = call again; OK = the record is written and verified; an error ends the job. */
km_clog_result_t km_clog_job_step(km_clog_job_t *job);
bool km_clog_job_active(const km_clog_job_t *job);
/* true while the next step is the sector erase (the slow one: the caller may want to wait for a better moment) */
bool km_clog_job_next_is_erase(const km_clog_job_t *job);
/* true once this job has erased the sector: from then until the commit unit is programmed NO record exists, so the caller should
 * run the remaining steps back to back instead of pausing the job (about 40 program operations). */
bool km_clog_job_must_finish(const km_clog_job_t *job);
void km_clog_job_abort(km_clog_job_t *job);

/* begin + all steps in a row. OK, UNCHANGED or an error. */
km_clog_result_t km_clog_append(const km_clog_t *log, const uint8_t *data);

#endif /* KM_CALSTORE_H */
