/*
 * usb_hs.h - USB HS device driver for the N32H48x (DWC2-compatible core at 0x40040000), polled.
 *
 * The core is the common DWC2 layout; the Nations-specific clock/PHY bits have no public field description, so the
 * values used are the smallest set that made the keyboard enumerate (USB_BRINGUP_NOTES.md section 2; where they
 * were first learned is stated in DISCLAIMER.md section 5). Enumerates on the keyboard since the D2 test.
 */
#ifndef USB_HS_H
#define USB_HS_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_usb.h"

/* Bring up clocks, PHY and the core, then connect (D+ pull-up). Feeds the IWDG while it waits. */
void km_usbhs_init(void);

/* Same, presenting the descriptor set `variant` (km_usb.h). km_usbhs_init() is the PLAIN variant: the keyboard as it always was.
 * KM_USB_VARIANT_PAD adds the gamepad interface IF3 / endpoint EP4 and arms the fall-back: call km_usbhs_set_clock() FIRST; if
 * no clock was given, PAD silently becomes PLAIN (check km_usbhs_variant()). */
void km_usbhs_init_variant(km_usb_variant_t variant);
/* Millisecond clock (monotonic, wrapping is fine) used by the fall-back. It must be callable from km_usbhs_poll(). */
void km_usbhs_set_clock(uint32_t (*now_ms)(void));
/* The descriptor set presented right now (PLAIN after a fall-back), whether the fall-back happened, and whether its clock runs. */
km_usb_variant_t km_usbhs_variant(void);
bool km_usbhs_fell_back(void);
bool km_usbhs_fallback_clock_running(void);
/* HARDWARE DRILL ONLY (usb_diag "u5c"): refuse SET_CONFIGURATION(1) while the PAD set is presented, so the host never configures
 * and the fall-back must happen. Never call it in a real image. */
void km_usbhs_rehearsal_refuse_configuration(bool on);
/* GHWCFG1..4 of the USB core, raw (decode with km_usb_hwcfg_decode()). Read-only; valid after km_usbhs_init*(). */
void km_usbhs_read_hwcfg(uint32_t out[4]);

/* Handle pending bus events and control transfers. Call it from the main loop as often as possible
 * (a SETUP packet must be answered within a few milliseconds). */
void km_usbhs_poll(void);

bool km_usbhs_configured(void);

/* Drop the D+ pull-up so the host sees the device disconnect (before a reset). */
void km_usbhs_disconnect(void);

/* Vendor channel (IF1: EP10 OUT / EP2 IN, 64-byte reports). The handler gets each received report and fills `out`
 * when a reply must be sent (return true). It runs in the main loop, never in an interrupt. */
typedef bool (*km_usbhs_vendor_fn)(const uint8_t in[64], uint8_t out[64]);
void km_usbhs_set_vendor_handler(km_usbhs_vendor_fn fn);

/* Key reports. All of them are non-blocking and return false when the device is not configured or the previous report on
 * that endpoint has not been taken by the host yet (the transfer-complete is noticed inside km_usbhs_poll()). Retry with
 * the CURRENT key state on the next pass; never queue stale ones.
 *   IF0 / EP1: boot keyboard, exactly 8 bytes (km_usb_reports.h).
 *   IF2 / EP3: 30 bytes starting 0x01 (NKRO bitmap) or 3 bytes starting 0x02 (consumer control), anything else is refused. */
bool km_usbhs_kbd_ready(void);
bool km_usbhs_send_kbd(const uint8_t report[8]);
bool km_usbhs_nkro_ready(void);
bool km_usbhs_send_nkro(const uint8_t *report, unsigned len);
/* Gamepad (PAD variant only, IF3 / EP4): exactly KM_USB_PAD_REPORT_LEN bytes (km_usb_reports.h; km_usb_pad_pack() writes them). Same
 * rules as the key reports: non-blocking, false when not configured, in the PLAIN variant, or while the previous report is still
 * waiting for the host. The gamepad endpoint is independent: a host that never polls it delays nothing else. */
bool km_usbhs_pad_ready(void);
bool km_usbhs_send_pad(const uint8_t *report, unsigned len);
/* XInput-style gamepad (XINPUT variant only, same endpoint EP4 as the HID gamepad): exactly KM_USB_XI_REPORT_LEN bytes
 * (km_usb_xinput_pack() writes them). Same rules as km_usbhs_send_pad(). The host's rumble message is only recorded. */
bool km_usbhs_xinput_ready(void);
bool km_usbhs_send_xinput(const uint8_t *report, unsigned len);
void km_usbhs_xinput_rumble(uint8_t *large, uint8_t *small);
/* The host selected the boot protocol on IF0 (BIOS / bootloader): use IF0 reports then. HID idle repeats are handled inside
 * the driver (SOF counted in km_usbhs_poll()). */
bool km_usbhs_kbd_boot_protocol(void);

/* ---- M5 T-E: bus events and endpoint recovery (no change to enumeration) ---- */

typedef struct {
    uint32_t resets;      /* USB bus resets handled (each one drops the configuration) */
    uint32_t suspends;    /* suspend-detected events (GINTSTS.USBSUSP) */
    uint32_t resumes;     /* resume / wake-up detected events (GINTSTS.WKUPINT, polled as a status bit like the others) */
} km_usbhs_events_t;
/* Totals since power-up; the main loop compares them with its last copy. */
void km_usbhs_get_events(km_usbhs_events_t *out);
/* The keyboard LEDs the host last asked for (SET_REPORT on IF0 or IF2): KM_USB_LED_NUM_LOCK | CAPS_LOCK | SCROLL_LOCK of
 * km_usb_reports.h. 0 after power-up and after every bus reset (the host sends its state again once it has configured the
 * device), so 0 means "off or not told yet". km_usbhs_led_report_count() counts the reports accepted since power-up: it tells a
 * host that never sends any (nothing changes, count stays 0) from one that sends them (count rises on every Caps Lock). */
uint8_t km_usbhs_host_leds(void);
uint32_t km_usbhs_led_report_count(void);
/* true while the core reports the bus as suspended (DSTS.SUSPSTS, a level, so a missed edge cannot leave it wrong). */
bool km_usbhs_suspended(void);
/* A key-report IN endpoint (EP1, EP3) that stays busy for ever: disable it, flush its transmit FIFO and mark it free again.
 * Bounded waits only. UNVERIFIED on hardware: the DWC2 NAK/disable sequence is written from the programming guide, not tested. */
void km_usbhs_in_recover(void);
/* ---- 8 kHz diagnostics (read-only) ---- */
uint32_t km_usbhs_sof_count(void);     /* SOF interrupts seen by km_usbhs_poll() since power-up (8000 per second at high speed if the poll is fast enough) */
uint8_t km_usbhs_enum_speed(void);     /* DSTS.ENUMSPD at the end of the last enumeration: 0 high speed, 1 or 3 full speed, 0xFF not yet */
uint32_t km_usbhs_dsts(void);          /* DSTS raw */
/* Same for the gamepad endpoint (EP4): a report the host never took is dropped and the endpoint is free again. No effect in the PLAIN variant. */
void km_usbhs_pad_recover(void);

#endif
