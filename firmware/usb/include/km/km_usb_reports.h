/*
 * km_usb_reports.h - the wire format of every report the keyboard sends or receives (what the report descriptors in
 * km_usb_desc.c promise to the host). Hardware independent.
 *
 *  Interface 0  boot keyboard      EP1 IN 8 B   (HID boot subclass, keyboard protocol; no report IDs)
 *      IN  8 bytes   [0] modifier bits: bit0 LCtrl, 1 LShift, 2 LAlt, 3 LGui, 4 RCtrl, 5 RShift, 6 RAlt, 7 RGui
 *                    [1] reserved, always 0
 *                    [2..7] up to six key usages (keyboard page 0x07); all 0 = no key; 0x01 in all six = ErrorRollOver
 *      OUT 1 byte    LED bits: bit0 NumLock, 1 CapsLock, 2 ScrollLock, 3 Compose, 4 Kana (set over EP0, SET_REPORT; bits 0..2 kept as host_leds)
 *
 *  Interface 1  vendor channel     EP2 IN + EP10 OUT, 64 B each, no report IDs (cmd 3, scope 0xC5, enter-bootloader)
 *
 *  Interface 3  gamepad (PAD variant only, see km_usb_desc.c)   EP4 IN, 15 B, no report ID, HID usage Generic Desktop / Game Pad
 *      IN  15 bytes  [0..1] 16 buttons, bit n = button n+1 (little endian)
 *                    [2]    hat switch: 0 = centred, 1 = up, 2 = up-right, 3 = right, 4 = down-right, 5 = down, 6 = down-left,
 *                           7 = left, 8 = up-left (the other 4 bits of the byte are padding, 0)
 *                    [3..4] LX  [5..6] LY  [7..8] RX  [9..10] RY   signed 16 bit, -32767..32767 (X, Y, Rx, Ry of the Generic Desktop page)
 *                    [11..12] left trigger  [13..14] right trigger   unsigned 16 bit, 0..32767 (Z and Rz)
 *      No output or feature reports. The length is KM_USB_PAD_REPORT_LEN; the byte layout is the one thing the gamepad core
 *      (km_pad) and this interface must agree on, and km_usb_pad_pack() below is the only place that writes it.
 *
 *  Interface 3  XInput-style gamepad (XINPUT variant only, M6 P11; replaces the HID gamepad, never both)
 *      Vendor-specific class 0xFF / subclass 0x5D / protocol 0x01, one vendor class descriptor (0x21, 17 bytes) between the
 *      interface and its endpoints, EP4 IN (32 B max) and EP4 OUT (32 B max). Written from the publicly documented behaviour of
 *      wired Xbox-360-compatible controllers (Linux xpad, open XInput clones); NOT checked against any Microsoft document.
 *      IN  20 bytes  [0] 0x00 message type, [1] 0x14 length
 *                    [2] bit0 pad up, 1 pad down, 2 pad left, 3 pad right, 4 Start, 5 Back, 6 left stick click, 7 right stick click
 *                    [3] bit0 LB, 1 RB, 2 Guide, 3 unused (0), 4 A, 5 B, 6 X, 7 Y
 *                    [4] left trigger 0..255   [5] right trigger 0..255
 *                    [6..7] LX  [8..9] LY  [10..11] RX  [12..13] RY   signed 16 bit little endian, +Y = UP (the opposite of the HID
 *                    gamepad above), -32768..32767 (we use -32767..32767)
 *                    [14..19] 0
 *      OUT 8 bytes rumble  [0]=0x00 [1]=0x08 [2]=0 [3]=large motor [4]=small motor [5..7]=0;  3 bytes LED  [0]=0x01 [1]=0x03 [2]=pattern.
 *      Both are only recorded (km_usb_xinput_rumble()); there is no motor. Anything else on OUT is dropped.
 *      The 15-byte pad report of km_pad is converted by km_usb_xinput_from_pad(); the button mapping is documented there.
 *
 *  Interface 2  NKRO + media       EP3 IN, max 64 B per report, report IDs are the first byte of every report
 *      id 1  keyboard bitmap  IN  30 bytes = [0] 0x01, [1..29] bitmap of 232 bits: bit n of the bitmap (byte 1 + n/8, bit n%8)
 *                                 is 1 while keyboard-page usage n is pressed, n = 0x00..0xE7 (so the modifiers 0xE0..0xE7
 *                                 are the last byte). Bits 0..3 (no-event, ErrorRollOver, POSTFail, ErrorUndefined) are not
 *                                 keys: they are padding in the descriptor and always 0.
 *                         OUT  2 bytes = [0] 0x01, [1] LED bits as above (SET_REPORT; bits 0..2 kept as host_leds)
 *      id 2  consumer control IN   3 bytes = [0] 0x02, [1..2] one 16-bit usage of the consumer page 0x0C, little endian,
 *                                 0 = released (play/pause 0x00CD, mute 0x00E2, volume up/down 0x00E9/0x00EA, next/previous
 *                                 track 0x00B5/0x00B6, ...). Usages up to 0x03FF.
 *
 * PROVISIONAL (M4_PLAN.md T-B/T-C): km_report.h of the key engine (T-B) builds the NKRO bitmap; this header is the
 * layout it must produce. At integration the two are reconciled; if T-B chose another bit layout, the report
 * descriptor for id 1 is the only thing that changes.
 */
#ifndef KM_USB_REPORTS_H
#define KM_USB_REPORTS_H

#define KM_USB_BOOT_REPORT_LEN     8u
#define KM_USB_BOOT_LED_LEN        1u
#define KM_USB_VND_REPORT_LEN      64u

#define KM_USB_NKRO_ID_KEYS        1u
#define KM_USB_NKRO_ID_CONSUMER    2u
#define KM_USB_NKRO_FIRST_USAGE    0x00u
#define KM_USB_NKRO_LAST_USAGE     0xE7u
#define KM_USB_NKRO_BITMAP_BYTES   29u                                   /* ceil(232 / 8) */
#define KM_USB_NKRO_REPORT_LEN     (1u + KM_USB_NKRO_BITMAP_BYTES)       /* 30, with the report ID */
#define KM_USB_CONSUMER_REPORT_LEN 3u                                    /* 1 + 2, with the report ID */
#define KM_USB_NKRO_LED_LEN        2u                                    /* report ID + LED bits */
#define KM_USB_CONSUMER_LAST_USAGE 0x03FFu

/* Host keyboard LED bits (HID LED page, boot keyboard output report): the ones the firmware keeps. Compose (bit3) and
 * Kana (bit4) are declared in the descriptors but have no use here, the top three bits are padding. */
#define KM_USB_LED_NUM_LOCK        0x01u
#define KM_USB_LED_CAPS_LOCK       0x02u
#define KM_USB_LED_SCROLL_LOCK     0x04u
#define KM_USB_LED_MASK            0x07u

/* ---- gamepad report (IF3) ---- */
#define KM_USB_PAD_BUTTONS         16u
#define KM_USB_PAD_AXES            6u
#define KM_USB_PAD_REPORT_LEN      ((KM_USB_PAD_BUTTONS + 8u + KM_USB_PAD_AXES * 16u) / 8u)   /* 15 */
#define KM_USB_PAD_STICK_MAX       32767
#define KM_USB_PAD_HAT_MAX         8u
/* ---- XInput-style report (IF3 in the XINPUT variant) ---- */
#define KM_USB_XI_REPORT_LEN       20u
#define KM_USB_XI_OUT_MAX          8u
#define KM_USB_XI_TYPE_INPUT       0x00u
#define KM_USB_XI_LEN_BYTE         0x14u
/* byte 2 */
#define KM_USB_XI_DPAD_UP          0x01u
#define KM_USB_XI_DPAD_DOWN        0x02u
#define KM_USB_XI_DPAD_LEFT        0x04u
#define KM_USB_XI_DPAD_RIGHT       0x08u
#define KM_USB_XI_START            0x10u
#define KM_USB_XI_BACK             0x20u
#define KM_USB_XI_L3               0x40u
#define KM_USB_XI_R3               0x80u
/* byte 3 */
#define KM_USB_XI_LB               0x01u
#define KM_USB_XI_RB               0x02u
#define KM_USB_XI_GUIDE            0x04u
#define KM_USB_XI_A                0x10u
#define KM_USB_XI_B                0x20u
#define KM_USB_XI_X                0x40u
#define KM_USB_XI_Y                0x80u

/* Periods (ms) of the synthetic sweep km_usb_pad_sweep() plays for the hardware test; tools/magger60_padtest.py reads these. */
#define KM_USB_PAD_SWEEP_LX_MS     4000u
#define KM_USB_PAD_SWEEP_LY_MS     5000u
#define KM_USB_PAD_SWEEP_RX_MS     6000u
#define KM_USB_PAD_SWEEP_RY_MS     7000u
#define KM_USB_PAD_SWEEP_LT_MS     8000u
#define KM_USB_PAD_SWEEP_RT_MS     9000u

#endif /* KM_USB_REPORTS_H */
