/*
 * flash_boot.h - the only flash write our firmware performs: set the enter-bootloader flag (bit 0 of the byte at
 * 0x0802C000) and reset. That is the flag the stock firmware's own "enter the bootloader" command sets, and the
 * bootloader reacts to it as described in BOOTLOADER_ANALYSIS.md section 2.
 *
 * Run on the keyboard in the D4 test (M2_PLAN.md): the bootloader accepted the flag and the sector read back
 * byte-identical afterwards. Every erase and program call is checked against km_flash_guard.h, so
 * nothing outside the profile sector 0x0802C000-0x0802DFFF can be touched, in particular not the bootloader.
 */
#ifndef FLASH_BOOT_H
#define FLASH_BOOT_H

/* Read the profile sector into RAM, set the flag bit, erase and rewrite the sector, verify it, then reset the MCU.
 * Does not return. The bootloader sees the flag, clears it again (restoring the original byte) and stays in update mode. */
void km_flash_request_bootloader(void) __attribute__((noreturn));

#endif
