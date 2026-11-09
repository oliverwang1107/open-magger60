/*
 * km_store.h - power-fail-safe record store on two alternating flash sectors (M5 task T-D).
 * Original work. Hardware-free: all flash access goes through a small ops struct, so the host tests run the
 * same code against a simulated flash that loses power at every single program step.
 *
 * Sector layout (one record per sector, at offset 0; the rest of the sector stays erased):
 *
 *   offset  size  field                                            written
 *   0       4     magic "KMRS" (4B 4D 52 53)                        6. LAST  (together with the CRC, one 8-byte unit)
 *   4       4     CRC32 over bytes 8 .. 16+len-1 (seq, len, payload) 6. LAST
 *   8       4     sequence number                                    5. before the commit unit
 *   12      4     payload length in bytes
 *   16      len   payload                                            4. first (in address order)
 *
 * A record is valid iff the magic matches AND the CRC matches AND len fits the sector. Programming happens in
 * 8-byte units (the N32H48x flash programs a double word at a time and each unit is written exactly once after an
 * erase); the commit unit is programmed last, so a power cut at any earlier point leaves a record that fails
 * the check, and a cut inside the commit unit leaves magic or CRC incomplete, which fails the check too.
 *
 * Load: the valid record with the newest sequence number wins (sequence numbers compare modulo 2^32, so the
 * counter may wrap). Save: the sector that does NOT hold the newest record is erased, written, read back and
 * compared; only then is the new record "current". The old record is untouched until the next save, so one
 * complete valid record exists at every instant after the first save. A save whose payload equals the current
 * record's payload writes nothing at all (wear).
 */
#ifndef KM_STORE_H
#define KM_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_settings.h"

#define KM_STORE_UNIT          8u          /* program unit; programmed addresses are multiples of this */
#define KM_STORE_HEADER_BYTES  16u
#define KM_STORE_SECTOR_BYTES  0x2000u     /* same as KM_FLASH_SECTOR */
#define KM_STORE_MAX_PAYLOAD   (KM_STORE_SECTOR_BYTES - KM_STORE_HEADER_BYTES)
#define KM_STORE_MAGIC         0x53524D4Bu /* bytes 4B 4D 52 53 "KMRS" as a little-endian u32 */

typedef struct {
    void *ctx;
    /* Copy len bytes of flash at addr into dst. false = the read failed. */
    bool (*read)(void *ctx, uint32_t addr, uint8_t *dst, uint32_t len);
    /* Erase the whole sector at sector_addr (8 KB aligned). false = failed or timed out. */
    bool (*erase)(void *ctx, uint32_t sector_addr);
    /* Program one 8-byte unit at addr (8-byte aligned, currently erased). false = failed or timed out. */
    bool (*program)(void *ctx, uint32_t addr, const uint8_t data[8]);
} km_store_ops_t;

typedef struct {
    km_store_ops_t ops;
    uint32_t sector[2];     /* base addresses of the two sectors */
} km_store_t;

typedef enum {
    KM_STORE_OK = 0,        /* saved and verified */
    KM_STORE_UNCHANGED,     /* identical to the current record: nothing was written */
    KM_STORE_ERR_ARG,       /* bad pointer, empty or oversized payload, invalid settings */
    KM_STORE_ERR_READ,
    KM_STORE_ERR_ERASE,
    KM_STORE_ERR_PROGRAM,
    KM_STORE_ERR_VERIFY     /* read-back differs: the old record is still the current one */
} km_store_result_t;

typedef struct {
    bool     valid;
    uint8_t  index;         /* which sector (0 or 1) holds it */
    uint32_t seq;
    uint32_t len;
} km_store_info_t;

void km_store_init(km_store_t *st, const km_store_ops_t *ops, uint32_t sector_a, uint32_t sector_b);

/* Validate the record in sector idx (0/1) without keeping its payload. */
bool km_store_check_sector(const km_store_t *st, unsigned idx, km_store_info_t *info);
/* Find the newest valid record. false = none (blank, foreign or damaged flash). */
bool km_store_find(const km_store_t *st, km_store_info_t *info);
/* Copy the newest valid payload into buf (cap bytes). false = no record or cap too small; *len = payload length. */
bool km_store_read(const km_store_t *st, uint8_t *buf, uint32_t cap, uint32_t *len);
/* Write a new record as described above. */
km_store_result_t km_store_write(const km_store_t *st, const uint8_t *data, uint32_t len);

/* ---- settings on top of the store ----
 * scratch must hold KM_SETTINGS_ENCODED_SIZE bytes. load: false = no valid record or it does not decode (use defaults). */
bool km_store_load_settings(const km_store_t *st, km_settings_t *s, uint8_t *scratch, unsigned cap);
km_store_result_t km_store_save_settings(const km_store_t *st, const km_settings_t *s, uint8_t *scratch, unsigned cap);

/* ---- the plan's argument-less API, on a store the firmware binds once at startup ---- */
void km_store_bind(const km_store_t *st);
bool km_store_load(km_settings_t *s);          /* false = nothing usable, the caller uses km_settings_default() */
bool km_store_save(const km_settings_t *s);    /* true = stored or already identical */
km_store_result_t km_store_last_result(void);   /* result code of the most recent km_store_save (diagnostics) */

#endif /* KM_STORE_H */
