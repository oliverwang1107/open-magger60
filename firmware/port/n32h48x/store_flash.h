/*
 * store_flash.h - the real N32H48x flash operations under km_store (M5 task T-D): erase an 8 KB sector, program an
 * 8-byte unit, read. Register protocol modelled on flash_boot.c (unlock keys, status clear, PER+ADDR+STRT erase,
 * PG + two word writes), which is the only flash code that had run on the keyboard before this one. See M5_PLAN.md / docs/HARDWARE_LOG.md for
 * how far the store itself has been exercised on the real chip.
 *
 * Safety rules, all enforced here and tested on the host against a register-level simulation:
 *   - every erase, program and read address is checked against km_flash_guard.h (km_flash_store_allowed): only
 *     settings A 0x08030000, settings B 0x08032000, the calibration sector 0x08034000, the light configuration
 *     sector 0x08036000 and the two light scene sectors 0x08038000 / 0x0803A000 can be touched;
 *   - every wait is bounded and returns false instead of hanging; the IWDG is fed while waiting;
 *   - every program unit is read back and compared before the call returns true;
 *   - the flash is unlocked once (a second unlock sequence on this kind of controller can lock it until reset);
 *   - the caller decides WHEN to save (no keys down, USB idle); the CPU may stall while the flash is busy.
 */
#ifndef STORE_FLASH_H
#define STORE_FLASH_H

#include "km/km_calstore.h"
#include "km/km_store.h"

/* The ops struct for km_store_init(). */
const km_store_ops_t *km_store_flash_ops(void);

/* What the last failed flash operation saw, for the scope: out[0] stage (1 erase pre-wait, 2 erase wait, 3 blank check,
 * 4 program pre-wait, 5 program wait, 6 program read-back; 0 = nothing failed yet), out[1] flash status low 16 bits,
 * out[2] status high 16 bits, out[3] control register low 16 bits, out[4] address offset within 0x08000000 (bits 0..15). */
void km_store_flash_diag(uint16_t out[5]);

/* The store over settings A and B on the real flash (initialised on first use). */
const km_store_t *km_store_flash(void);

/* The light scene (km_lightscene.h) on the two sectors 0x08038000 / 0x0803A000, written alternately (initialised on first use). Use it with
 * km_lightscene_load() / km_lightscene_save(); a save erases one sector and programs the record, synchronously, with the driver rules above. */
const km_store_t *km_lightscene_store_flash(void);

/* The calibration log (km_calstore.h / km_calib.h) over sector 0x08034000 on the real flash (initialised on first use). One
 * record = KM_CAL_PAYLOAD_BYTES. Same driver rules as above; one erase per 24 records, one 8-byte program per job step. */
const km_clog_t *km_calstore_flash(void);

/* The light configuration log (km_ledstore.h) over sector 0x08036000 on the real flash (initialised on first use). One record =
 * KM_LEDSTORE_PAYLOAD_BYTES, 256 records per sector, one erase per 256 saves. */
const km_clog_t *km_ledstore_flash(void);
/* The same sector read and written as the per-profile table (km_ledprof.h, 64-byte payload). km_ledstore_flash() stays as the reader of
 * version-1 records (boot migration). */
const km_clog_t *km_ledprof_flash(void);

#endif /* STORE_FLASH_H */
