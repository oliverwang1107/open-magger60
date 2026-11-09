/*
 * km_usb_ids.h - every identifier and every piece of branding the USB device presents, in ONE place.
 * Change something about how the keyboard introduces itself here and nowhere else.
 *
 * OPEN QUESTION (owner decision before anything is published): the vendor/product ID below is the one the
 * keyboard shipped with. Keeping it means the stock tools, the OS driver caches and our own host tools
 * all keep recognising the device, which is what the K1-K3 hardware ladder needs. A VID is not ours to
 * assign: a USB-IF vendor ID costs money, and reusing someone else's in a published product is at best a
 * grey area. For a public release the options are (a) a free sponsored PID such as pid.codes / a PID from the
 * chip vendor, (b) a clearly "private use" test ID, (c) keep the stock pair because the product is an
 * alternative firmware for that very hardware. Whatever is chosen, only this header changes (the host
 * tools and ui/js/device/ids.js hold their own copies of VID/PID and must follow).
 */
#ifndef KM_USB_IDS_H
#define KM_USB_IDS_H

/* ---- device descriptor ---- */
#define KM_USB_VID              0x19F5u
#define KM_USB_PID              0xFB49u
/*
 * bcdDevice, binary coded decimal "major.minor". Bumped whenever the descriptors change in a way an OS could
 * trip over: Windows keeps a per-device (VID/PID/serial) cache and a new bcdDevice together with the new serial
 * format below makes it treat the keyboard as a new device. 0x0001 = first own descriptor set ("K1").
 * The descriptor variants (km_usb_variant_t) each have their own value, so Windows never reuses the cached descriptors of
 * one for the other: PLAIN (3 interfaces, the keyboard as it always was) keeps 0x0001, PAD (the gamepad interface IF3 added)
 * is 0x0002. A later variant takes the next number; never reuse one for a different descriptor set.
 */
/*
 * KM_USB_HS_8KHZ = 1 (image km_m6h, LATENCY_NOTES.md section 4): the keyboard endpoints (IF0 boot keyboard, IF2 NKRO) are polled every
 * micro-frame (bInterval 1 = 125 us = 8 kHz). That is a different descriptor set, so it gets its own bcdDevice values (never reused).
 * Default 0: the descriptors are exactly those of km_l51 / km_m6a.
 */
#ifndef KM_USB_HS_8KHZ
#define KM_USB_HS_8KHZ 0
#endif
#if KM_USB_HS_8KHZ
#define KM_USB_BCD_DEVICE_PLAIN 0x0003u
#define KM_USB_BCD_DEVICE_PAD   0x0004u
#else
#define KM_USB_BCD_DEVICE_PLAIN 0x0001u
#define KM_USB_BCD_DEVICE_PAD   0x0002u
#endif
#define KM_USB_BCD_DEVICE       KM_USB_BCD_DEVICE_PLAIN   /* kept for the existing name */
#define KM_USB_BCD_DEVICE_XINPUT 0x0005u         /* XINPUT variant (M6 P11): the Xbox-360-style vendor interface instead of the HID gamepad (0x0003/0x0004 are the 8 kHz sets, never reuse) */

/* ---- strings (US English only; ASCII text, converted to UTF-16LE on the fly) ---- */
#define KM_USB_LANGID           0x0409u
#define KM_USB_STR_MANUFACTURER "Open Magger Firmware"
#define KM_USB_STR_PRODUCT      "Magger60 Open Keyboard"
/*
 * Serial number: this prefix, then the three 32-bit words of the chip's unique ID as 24 upper-case hex digits
 * (word 0 first, most significant digit first). Total 28 characters. The stock firmware used a different, shorter
 * scheme, so the OS sees a different "serial" and starts from a clean device instance (see K1_NOTES.md).
 */
#define KM_USB_SERIAL_PREFIX    "KM60"
#define KM_USB_SERIAL_UID_WORDS 3u

/* ---- configuration descriptor ---- */
#define KM_USB_MAX_POWER_MA     300u     /* bMaxPower is in units of 2 mA */
/*
 * bInterval values are for HIGH speed: the period is 2^(n-1) micro-frames of 125 us, so 4 = 1 ms, 1 = 125 us.
 * Everything polls once per millisecond for now; 1 on the NKRO endpoint is the later option for lowest latency.
 */
#define KM_USB_BINTERVAL_1MS    4u
#define KM_USB_BINTERVAL_125US  1u
#if KM_USB_HS_8KHZ
#define KM_USB_BINTERVAL_KBD    KM_USB_BINTERVAL_125US   /* keys: every micro-frame */
#define KM_USB_BINTERVAL_NKRO   KM_USB_BINTERVAL_125US
#else
#define KM_USB_BINTERVAL_KBD    KM_USB_BINTERVAL_1MS
#define KM_USB_BINTERVAL_NKRO   KM_USB_BINTERVAL_1MS
#endif
#define KM_USB_BINTERVAL_VND    KM_USB_BINTERVAL_1MS     /* the host tools' channel stays at 1 kHz */
#define KM_USB_BINTERVAL_PAD    KM_USB_BINTERVAL_1MS     /* the gamepad endpoint polls once per millisecond as well */
#define KM_USB_BINTERVAL_XINPUT 4u               /* XINPUT variant: 1 ms as well */

/*
 * Usage of the vendor channel's top-level collection (IF1). The host tools (tools/magger60_backup.py
 * pick_interface(), tools/magger60_scope.py, ui/js/device/ids.js) look the channel up as "usage page 0x01 (Generic
 * Desktop), usage 0x00 (Undefined)" and so does everything built on them. That pair is therefore kept for K1; the
 * 64 data bytes themselves sit in the vendor-defined page 0xFF4D (see km_usb_desc.c). To move the top-level
 * collection to a vendor page as well, change the four bytes in the IF1 report descriptor AND those three host
 * lookups together; tests/test_usb.c pins both numbers so a change cannot happen by accident.
 */
#define KM_USB_VND_TOP_USAGE_PAGE 0x01u
#define KM_USB_VND_TOP_USAGE      0x00u
#define KM_USB_VND_DATA_USAGE_PAGE 0xFF4Du

#endif /* KM_USB_IDS_H */
