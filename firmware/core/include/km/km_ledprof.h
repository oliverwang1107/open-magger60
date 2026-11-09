/*
 * km_ledprof.h - the light configuration per key profile (LIGHT_PROFILE_NOTES.md). Original work, hardware-free.
 *
 * One table holds the lights of the four key profiles plus a "global" fallback entry. It lives in the SAME flash sector as the older
 * single-record light configuration (km_ledstore.h, 0x08036000, km_flash_guard.h), as a km_clog log with a 64-byte payload (stride 80,
 * 102 saves per sector erase). Every save writes the whole table, so one valid record always holds all five entries; the erase that
 * clears a full sector loses nothing.
 *
 * Meaning:
 *   entry 0     global: what a profile without an own entry uses. Boot migration: a sector that holds only version-1 records
 *               (km_ledstore.h, 16-byte payload) is read as the global entry, so an old keyboard keeps its lights.
 *   entry 1..4  profile 0..3 (index + 1)
 *   mask bit i  entry i is set; a cleared bit means "use the global entry" (profiles) / "built-in defaults" (global)
 *
 * Payload (64 bytes; strict on read, any violation = no table = global defaults, never a crash):
 *   0      u8   format version, 2
 *   1      u8   mask, bits 0..4 (bits 5..7 must be 0)
 *   2..3   u8   0
 *   4+12*i      entry i, 12 bytes: effect, brightness, speed, r, g, b, idle-off seconds (u16 LE, 0 = never),
 *               scene reference (must be 0: reserved, the key-colour scene stays global), 3 bytes 0.
 *               An entry whose mask bit is clear must be all zero.
 *
 * Saving reuses km_ledstore_t (coalescing, idle-only, one flash operation per pass) through km_ledstore_request_raw().
 */
#ifndef KM_LEDPROF_H
#define KM_LEDPROF_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_calstore.h"
#include "km/km_ledstore.h"

#define KM_LEDPROF_PROFILES        4u
#define KM_LEDPROF_ENTRIES         5u       /* global + 4 profiles */
#define KM_LEDPROF_VERSION         2u
#define KM_LEDPROF_ENTRY_BYTES     12u
#define KM_LEDPROF_HEADER_BYTES    4u
#define KM_LEDPROF_PAYLOAD_BYTES   (KM_LEDPROF_HEADER_BYTES + KM_LEDPROF_ENTRIES * KM_LEDPROF_ENTRY_BYTES)    /* 64 */

typedef struct {
    uint8_t             mask;                           /* bit 0 global, bit 1+p profile p */
    km_ledstore_rec_t   rec[KM_LEDPROF_ENTRIES];
} km_ledprof_t;

/* Empty table: mask 0, every record the built-in default. */
void km_ledprof_default(km_ledprof_t *t);
/* The lights a profile (0..3) shows: its own entry, else the global entry. false = neither is set (the caller keeps what it has). */
bool km_ledprof_effective(const km_ledprof_t *t, unsigned profile, km_ledstore_rec_t *r);
bool km_ledprof_has_own(const km_ledprof_t *t, unsigned profile);
/* Set profile p's own entry (false = bad profile or invalid record, table untouched). */
bool km_ledprof_set(km_ledprof_t *t, unsigned profile, const km_ledstore_rec_t *r);
/* Set the global entry. */
bool km_ledprof_set_global(km_ledprof_t *t, const km_ledstore_rec_t *r);
/* Remove profile p's own entry (it follows the global entry again). */
bool km_ledprof_clear(km_ledprof_t *t, unsigned profile);

unsigned km_ledprof_encode(const km_ledprof_t *t, uint8_t *buf, unsigned cap);       /* payload length, 0 = invalid / cap too small */
bool km_ledprof_decode(km_ledprof_t *t, const uint8_t *buf, unsigned len);           /* strict; false = *t untouched */
/* Boot: the newest valid version-2 record of `log`; else the newest version-1 record of `legacy` (the same sector read as the 16-byte
 * log) becomes the global entry. false = neither (*t untouched). Either log may be NULL. */
bool km_ledprof_load(const km_clog_t *log, const km_clog_t *legacy, km_ledprof_t *t);

#endif /* KM_LEDPROF_H */
