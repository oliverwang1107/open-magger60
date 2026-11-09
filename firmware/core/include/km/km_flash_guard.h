/*
 * km_flash_guard.h - the ONE rule for writing flash from our firmware (M2-D4 of M2_PLAN.md).
 *
 * Without an SWD probe a corrupted bootloader (0x08000000-0x08007FFF) cannot be repaired, so firmware we ship
 * may write flash only in the profile sector (0x0802C000-0x0802DFFF), and only to set the enter-bootloader
 * flag (km_flash_write_allowed), plus, since M5, our own settings sectors (km_flash_store_allowed, see below).
 * The low-level erase and program routines in port/n32h48x/flash_boot.c and store_flash.c each refuse every
 * address outside their own rule; this header holds the rules so the host tests can pin them down.
 */
#ifndef KM_FLASH_GUARD_H
#define KM_FLASH_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_flash_map.h"

/*
 * M5 (T-D) adds our own settings storage as ADDITIONAL writable sectors, checked by a separate rule so that the
 * boot-flag path (km_flash_write_allowed, used by flash_boot.c) can still reach only the profile sector and the
 * settings path (km_flash_store_allowed, used by store_flash.c) can still reach only these six sectors:
 *   0x08030000  settings A    0x08032000  settings B (written alternately)    0x08034000  calibration
 *   0x08036000  light configuration (v1: one record, km_ledstore.h; v2: per-profile table, km_ledprof.h)
 *   0x08038000  light scene A    0x0803A000  light scene B (written alternately; indicator rules, per-key colours; km_lightscene.h)
 * Everything else stays forbidden: bootloader 0x08000000-0x08007FFF, app 0x08008000-0x08027FFF, the app-ID
 * block 0x08028000, 0x0802A000, 0x0802C000 (boot flag) and every sector above 0x0803BFFF (0x0803C000 and 0x0803E000 stay reserved).
 * Why 0x08038000 / 0x0803A000 for the scene: the next two sectors of the same block (stock DKS and MT tables, which our firmware does not
 * implement: no code of ours reads or writes them), the boot decision (BOOTLOADER_ANALYSIS.md section 2, inferred from the bootloader, not hardware-proven for these sectors) reads only the app-ID, the boot flag, the rescue keys and the vector table, and an image flash
 * restores them from the backup like every other settings sector. They are one contiguous block with the five sectors we already own.
 * Why 0x08036000 for the lights: it is the next sector of the block we already own, the stock firmware used it for macros, which our
 * firmware does not implement (no code of ours reads or writes macros), and the sectors after it (DKS, MT, TGL, app data) stay untouched.
 */
#define KM_FLASH_SETTINGS_A     0x08030000u
#define KM_FLASH_SETTINGS_B     0x08032000u
#define KM_FLASH_CALIBRATION    0x08034000u
#define KM_FLASH_LED_CONFIG     0x08036000u
#define KM_FLASH_SCENE_A        0x08038000u
#define KM_FLASH_SCENE_B        0x0803A000u
#define KM_FLASH_STORE_SECTORS  6u

#define KM_FLASH_WRITABLE_BASE KM_CFG_PROFILE
#define KM_FLASH_WRITABLE_END  (KM_CFG_PROFILE + KM_FLASH_SECTOR)   /* exclusive */

/* True only when [addr, addr + len) lies completely inside the profile sector. Overflow-safe. */
static inline bool km_flash_write_allowed(uint32_t addr, uint32_t len)
{
    return len <= KM_FLASH_SECTOR && addr >= KM_FLASH_WRITABLE_BASE && addr <= KM_FLASH_WRITABLE_END - len;
}

/* True only when [addr, addr + len) lies completely inside ONE of the six sectors above. Overflow-safe. */
static inline bool km_flash_store_allowed(uint32_t addr, uint32_t len)
{
    if (len > KM_FLASH_SECTOR) {
        return false;
    }
    const uint32_t sectors[KM_FLASH_STORE_SECTORS] = { KM_FLASH_SETTINGS_A, KM_FLASH_SETTINGS_B, KM_FLASH_CALIBRATION, KM_FLASH_LED_CONFIG,
                                                      KM_FLASH_SCENE_A, KM_FLASH_SCENE_B };
    for (unsigned i = 0; i < KM_FLASH_STORE_SECTORS; i++) {
        if (addr >= sectors[i] && addr <= sectors[i] + KM_FLASH_SECTOR - len) {
            return true;
        }
    }
    return false;
}

#endif
