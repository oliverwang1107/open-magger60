/*
 * km_report.h - keyboard input report builders (M4 task T-B). Original work.
 *
 * Input: the pressed slots in press order (oldest first), as kept by the key engine
 * (km_engine_down_slots()). Each slot is looked up on layer 0 of km_keymap; slots whose usage is 0
 * (Fn, no-key slots, out-of-range numbers) are ignored. With layers, km_layers_update() (km_layers.h) turns the
 * pressed slots into a list of usages instead, and the *_usages builders below take that list; they apply the same
 * rules (press order, six-key limit, ErrorRollOver, duplicates once). Usages outside 0x04..0xE7 are ignored.
 *
 * ---- Boot keyboard report, KM_REPORT_BOOT_LEN = 8 bytes (HID 1.11 appendix B.1) ----
 *   byte 0      modifier bits: bit n = usage 0xE0 + n (LCtrl, LShift, LAlt, LGUI, RCtrl, RShift, RAlt, RGUI)
 *   byte 1      reserved, 0
 *   bytes 2..7  up to six non-modifier usages, in PRESS ORDER (oldest pressed key first). A key that
 *               stays down keeps its position, a newly pressed key goes behind the older ones, so the
 *               list is stable and does not reshuffle while keys are held. Unused entries are 0.
 *   Rollover: with more than six distinct non-modifier keys down, all six entries are 0x01
 *   (ErrorRollOver); the modifier byte is still reported. A usage held on two slots shows up once.
 *
 * ---- N-key rollover report, KM_REPORT_NKRO_LEN = 29 bytes ----
 *   A plain bitmap, no report id byte, no modifier byte: bit (usage & 7) of byte (usage >> 3) is 1
 *   while that usage is down. 29 bytes * 8 = 232 bits = usages 0..231 (0xE7 = RightGUI is the
 *   last one, so the modifiers are bits 0..7 of byte 28). Bits for usages 0..3 are always 0
 *   (reserved / error codes), bits for the usages of the keymap that do not exist stay 0.
 *   HID descriptor that matches: Usage Page (Keyboard) ; Usage Minimum (0) ; Usage Maximum (231) ;
 *   Logical Minimum (0) ; Logical Maximum (1) ; Report Size (1) ; Report Count (232) ;
 *   Input (Data, Variable, Absolute).  No rollover limit, no ErrorRollOver.
 *
 * ---- Consumer control report, KM_REPORT_CONSUMER_LEN = 3 bytes (IF2, report id 2; km_usb_reports.h) ----
 *   byte 0 = report id 2, bytes 1..2 = one 16-bit usage of the Consumer page 0x0C, little endian; 0 = nothing
 *   pressed. Only ONE media key is reported at a time: when several are held, km_layers reports the most
 *   recently pressed one (and falls back to the older one when that is released).
 */
#ifndef KM_REPORT_H
#define KM_REPORT_H

#include <stdint.h>

#define KM_REPORT_BOOT_LEN   8u
#define KM_REPORT_BOOT_KEYS  6u
#define KM_REPORT_NKRO_LEN   29u
#define KM_REPORT_NKRO_BITS  232u   /* usages 0..231 */
#define KM_REPORT_ROLLOVER   0x01u  /* ErrorRollOver */
#define KM_REPORT_CONSUMER_ID   2u
#define KM_REPORT_CONSUMER_LEN  3u
#define KM_REPORT_CONSUMER_MAX  0x03FFu  /* highest usage the report descriptor declares */

/* Build the 8-byte boot report from n pressed slots (down_slots[0] is the oldest press). */
void km_report_build_boot(const uint8_t *down_slots, unsigned n, uint8_t report[KM_REPORT_BOOT_LEN]);

/* Build the 29-byte NKRO bitmap from n pressed slots. */
void km_report_build_nkro(const uint8_t *down_slots, unsigned n, uint8_t report[KM_REPORT_NKRO_LEN]);

/* Same, from a list of keyboard usages (as produced by km_layers_update()) instead of slots. */
void km_report_build_boot_usages(const uint8_t *usages, unsigned n, uint8_t report[KM_REPORT_BOOT_LEN]);
void km_report_build_nkro_usages(const uint8_t *usages, unsigned n, uint8_t report[KM_REPORT_NKRO_LEN]);

/* Build the 3-byte consumer report: id 2, usage little endian. usage 0 = released; usages above
 * KM_REPORT_CONSUMER_MAX are treated as 0. */
void km_report_build_consumer(uint16_t usage, uint8_t report[KM_REPORT_CONSUMER_LEN]);

#endif /* KM_REPORT_H */
