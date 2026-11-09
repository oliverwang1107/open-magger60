/*
 * km_flash_map.h - Magger60 (N32H48x) flash layout. The settings sectors keep the stock firmware's
 * layout, so the bootloader and a restore of the stock image keep working (CUSTOM_FIRMWARE_PLAN.md
 * decision D1). The bootloader range and the app window were measured on the keyboard (BOOTLOADER_ANALYSIS.md).
 *
 * Sectors our own firmware may write (km_flash_guard.h): 0x08030000 and 0x08032000 settings A / B, 0x08034000 calibration log,
 * 0x08036000 light configuration log, 0x08038000 and 0x0803A000 light scene A / B, and the boot-flag byte in 0x0802C000. All other sectors of the table below are only ever read
 * (and are rewritten with the backup contents whenever a firmware image is flashed).
 *
 * WARNING: the bootloader area (0x08000000-0x08007FFF) must never be written by firmware
 * we ship. The driver's raw flash-write command (0xE1) can reach it; we do not implement that command.
 */
#ifndef KM_FLASH_MAP_H
#define KM_FLASH_MAP_H

#define KM_FLASH_BASE        0x08000000u
#define KM_FLASH_SIZE        0x00080000u  /* 512 KB */
#define KM_FLASH_SECTOR      0x00002000u  /* 8 KB erase unit */

#define KM_BOOTLOADER_BASE   0x08000000u
#define KM_BOOTLOADER_END    0x08008000u  /* exclusive; bootloader is not in the update image */

#define KM_APP_BASE          0x08008000u  /* vector table; update image file offset 0 */
#define KM_APP_LIMIT         0x08028000u  /* exclusive: 128 KB for code and constants */

#define KM_CFG_ID_BLOCK      0x08028000u  /* VID/PID/version, 14 bytes: left exactly as the keyboard already holds it */
#define KM_CFG_BASE_INFO     0x0802A000u  /* current profile, profile count, AA BB magic */
#define KM_CFG_PROFILE       0x0802C000u  /* 4 x 64 B; byte 0 bit 0 = enter-bootloader flag */
#define KM_CFG_CALIBRATION   0x0802E000u  /* 128 x 8 B per-key calibration */
#define KM_CFG_TRIGGER       0x08030000u  /* 4 x 1 KB, 8 B per key */
#define KM_CFG_KEYMAP        0x08032000u  /* 4 profiles x 4 layers x 512 B, 3 B per key */
#define KM_CFG_COLORS        0x08034000u  /* 4 x 512 B */
#define KM_CFG_MACROS        0x08036000u  /* 8 KB. Stock macros; our firmware has no macros and keeps its light configuration here (km_ledstore.h) */
#define KM_CFG_DKS           0x08038000u  /* 768 B per profile, 24 B per key. Stock DKS; our firmware keeps light scene A here (km_lightscene.h) */
#define KM_CFG_MT            0x0803A000u  /* 256 B per profile, 6 B per key. Stock MT; our firmware keeps light scene B here */
#define KM_CFG_TGL           0x0803C000u  /* 128 B per profile, 3 B per key */
#define KM_CFG_APP_DATA      0x0803E000u  /* 8 KB */
#define KM_CFG_END           0x08040000u  /* end of the settings sectors (exclusive) */

/* Bit 0 of the first byte of profile record 0: set, then reset, to enter the bootloader. */
#define KM_BOOT_FLAG_ADDR    KM_CFG_PROFILE
#define KM_BOOT_FLAG_MASK    0x01u

#endif /* KM_FLASH_MAP_H */
