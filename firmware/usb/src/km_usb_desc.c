/*
 * USB descriptors of the Magger60 open firmware. Our own design, written from
 *   - the USB 2.0 specification (chapter 9: device, configuration, interface, endpoint, qualifier and string descriptors),
 *   - the HID 1.11 specification (HID descriptor, boot keyboard report descriptor of appendix B.1, item encoding),
 *   - the HID Usage Tables (generic desktop, keyboard/keypad, LED and consumer pages).
 * What each report means on the wire is documented in km_usb_reports.h; names and numbers that may change in a
 * release are in km_usb_ids.h. tests/test_usb.c walks every descriptor below with a small independent parser.
 */
#include <string.h>

#include "km/km_usb.h"

/* ---- HID report descriptor items (HID 1.11 section 6.2.2): prefix byte = tag << 4 | type << 2 | size ---- */
#define LO(v) ((v) & 0xFFu)
#define HI(v) (((v) >> 8) & 0xFFu)

#define H_USAGE_PAGE(p)        0x05u, (p)
#define H_USAGE_PAGE16(p)      0x06u, LO(p), HI(p)
#define H_USAGE(u)             0x09u, (u)
#define H_USAGE_MIN(u)         0x19u, (u)
#define H_USAGE_MAX(u)         0x29u, (u)
#define H_USAGE_MAX16(u)       0x2Au, LO(u), HI(u)
#define H_LOGICAL_MIN(v)       0x15u, (v)
#define H_LOGICAL_MIN16(v)     0x16u, LO(v), HI(v)
#define H_LOGICAL_MAX(v)       0x25u, (v)
#define H_LOGICAL_MAX16(v)     0x26u, LO(v), HI(v)
#define H_PHYSICAL_MIN(v)      0x35u, (v)
#define H_PHYSICAL_MAX(v)      0x45u, (v)
#define H_PHYSICAL_MAX16(v)    0x46u, LO(v), HI(v)
#define H_UNIT(v)              0x65u, (v)
#define H_REPORT_SIZE(n)       0x75u, (n)
#define H_REPORT_COUNT(n)      0x95u, (n)
#define H_REPORT_COUNT16(n)    0x96u, LO(n), HI(n)
#define H_REPORT_ID(n)         0x85u, (n)
#define H_INPUT(f)             0x81u, (f)
#define H_OUTPUT(f)            0x91u, (f)
#define H_COLLECTION(t)        0xA1u, (t)
#define H_END_COLLECTION       0xC0u

#define F_DATA_VAR_ABS         0x02u   /* Data, Variable, Absolute */
#define F_DATA_ARRAY_ABS       0x00u   /* Data, Array, Absolute */
#define F_CONST                0x01u   /* Constant (padding) */
#define F_DATA_VAR_ABS_NULL    0x42u   /* Data, Variable, Absolute, Null state (a value outside the logical range = not pressed) */
#define UNIT_DEGREES           0x14u   /* English rotation, degrees */
#define COLL_APPLICATION       0x01u

_Static_assert(KM_USB_VND_TOP_USAGE_PAGE <= 0xFFu && KM_USB_VND_TOP_USAGE <= 0xFFu, "one-byte items below");

/* IF0: boot keyboard. Shape fixed by HID 1.11 appendix B.1 (modifier byte, reserved byte, 5 LEDs + 3 padding bits, 6 key
 * codes); the key array holds the 101 usages 0..0x65 of that appendix; the full 0..0xE7 set is IF2's job (a 64-byte descriptor
 * would also need a zero-length packet to end the EP0 answer, which this layout avoids). */
static const uint8_t k_report_boot[] = {
    H_USAGE_PAGE(0x01), H_USAGE(0x06), H_COLLECTION(COLL_APPLICATION),
        H_USAGE_PAGE(0x07), H_USAGE_MIN(0xE0), H_USAGE_MAX(0xE7),         /* modifiers: 8 bits */
        H_LOGICAL_MIN(0), H_LOGICAL_MAX(1), H_REPORT_SIZE(1), H_REPORT_COUNT(8), H_INPUT(F_DATA_VAR_ABS),
        H_REPORT_COUNT(1), H_REPORT_SIZE(8), H_INPUT(F_CONST),            /* reserved byte */
        H_USAGE_PAGE(0x08), H_USAGE_MIN(1), H_USAGE_MAX(5),               /* LEDs: 5 bits out + 3 padding */
        H_REPORT_COUNT(5), H_REPORT_SIZE(1), H_OUTPUT(F_DATA_VAR_ABS),
        H_REPORT_COUNT(1), H_REPORT_SIZE(3), H_OUTPUT(F_CONST),
        H_USAGE_PAGE(0x07), H_USAGE_MIN(0), H_USAGE_MAX(0x65),            /* 6 key codes, usages 0..0x65 as in appendix B.1 */
        H_LOGICAL_MIN(0), H_LOGICAL_MAX(0x65), H_REPORT_COUNT(6), H_REPORT_SIZE(8), H_INPUT(F_DATA_ARRAY_ABS),
    H_END_COLLECTION,
};

/* IF1: the vendor channel, 64 bytes each way, no report IDs. The collection itself sits where the host tools look for
 * it (km_usb_ids.h); the payload bytes are declared in the vendor-defined page 0xFF4D. */
static const uint8_t k_report_vendor[] = {
    H_USAGE_PAGE(KM_USB_VND_TOP_USAGE_PAGE), H_USAGE(KM_USB_VND_TOP_USAGE), H_COLLECTION(COLL_APPLICATION),
        H_USAGE_PAGE16(KM_USB_VND_DATA_USAGE_PAGE),
        H_LOGICAL_MIN(0), H_LOGICAL_MAX16(0xFF), H_REPORT_SIZE(8), H_REPORT_COUNT(KM_USB_VND_REPORT_LEN),
        H_USAGE(0x01), H_INPUT(F_DATA_VAR_ABS),
        H_USAGE(0x02), H_OUTPUT(F_DATA_VAR_ABS),
    H_END_COLLECTION,
};

/* IF2: report ID 1 = keyboard as a bitmap of 232 bits, bit n = keyboard-page usage n. Bits 0..3 (no event, error codes)
 * are constant padding, bits 4..0xE7 are the real keys including the modifiers 0xE0..0xE7; plus the LED output report.
 * Report ID 2 = consumer control (media keys), one 16-bit usage, 0 = none. */
static const uint8_t k_report_nkro[] = {
    H_USAGE_PAGE(0x01), H_USAGE(0x06), H_COLLECTION(COLL_APPLICATION),
        H_REPORT_ID(KM_USB_NKRO_ID_KEYS),
        H_LOGICAL_MIN(0), H_LOGICAL_MAX(1), H_REPORT_SIZE(1),
        H_REPORT_COUNT(4), H_INPUT(F_CONST),                              /* usages 0..3: never set */
        H_USAGE_PAGE(0x07), H_USAGE_MIN(0x04), H_USAGE_MAX(KM_USB_NKRO_LAST_USAGE),
        H_REPORT_COUNT16(KM_USB_NKRO_LAST_USAGE - 3u), H_INPUT(F_DATA_VAR_ABS),   /* usages 4..0xE7: 228 bits */
        H_USAGE_PAGE(0x08), H_USAGE_MIN(1), H_USAGE_MAX(5),
        H_REPORT_COUNT(5), H_OUTPUT(F_DATA_VAR_ABS),                      /* LEDs */
        H_REPORT_COUNT(3), H_OUTPUT(F_CONST),
    H_END_COLLECTION,
    H_USAGE_PAGE(0x0C), H_USAGE(0x01), H_COLLECTION(COLL_APPLICATION),
        H_REPORT_ID(KM_USB_NKRO_ID_CONSUMER),
        H_LOGICAL_MIN(0), H_LOGICAL_MAX16(KM_USB_CONSUMER_LAST_USAGE),
        H_USAGE_MIN(0), H_USAGE_MAX16(KM_USB_CONSUMER_LAST_USAGE),
        H_REPORT_SIZE(16), H_REPORT_COUNT(1), H_INPUT(F_DATA_ARRAY_ABS),
    H_END_COLLECTION,
};

/* IF3 (PAD variant): a plain HID gamepad, no report IDs, the layout of km_usb_reports.h: 16 buttons, a hat (4 bits, 0 = centred)
 * and 4 bits of padding, then X Y Rx Ry as signed 16-bit sticks and Z Rz as unsigned 16-bit triggers = 15 bytes. The hat
 * carries a physical range and a unit (degrees) the way hosts expect a point-of-view switch; they are reset to 0 right after it
 * so they do not leak onto the axes. Windows binds its generic game-controller driver to this; no vendor driver. */
static const uint8_t k_report_pad[] = {
    H_USAGE_PAGE(0x01), H_USAGE(0x05), H_COLLECTION(COLL_APPLICATION),                  /* Generic Desktop / Game Pad */
        H_USAGE_PAGE(0x09), H_USAGE_MIN(1), H_USAGE_MAX(KM_USB_PAD_BUTTONS),             /* buttons 1..16 */
        H_LOGICAL_MIN(0), H_LOGICAL_MAX(1), H_REPORT_SIZE(1), H_REPORT_COUNT(KM_USB_PAD_BUTTONS), H_INPUT(F_DATA_VAR_ABS),
        H_USAGE_PAGE(0x01), H_USAGE(0x39),                                                /* hat switch */
        H_LOGICAL_MIN(1), H_LOGICAL_MAX(KM_USB_PAD_HAT_MAX), H_PHYSICAL_MIN(0), H_PHYSICAL_MAX16(315), H_UNIT(UNIT_DEGREES),
        H_REPORT_SIZE(4), H_REPORT_COUNT(1), H_INPUT(F_DATA_VAR_ABS_NULL),
        H_PHYSICAL_MIN(0), H_PHYSICAL_MAX(0), H_UNIT(0),                                  /* back to "same as logical" */
        H_REPORT_SIZE(4), H_REPORT_COUNT(1), H_INPUT(F_CONST),                            /* pad the hat to a byte */
        H_USAGE(0x30), H_USAGE(0x31), H_USAGE(0x33), H_USAGE(0x34),                       /* X Y Rx Ry = LX LY RX RY */
        H_LOGICAL_MIN16(0x8001), H_LOGICAL_MAX16(0x7FFF), H_REPORT_SIZE(16), H_REPORT_COUNT(4), H_INPUT(F_DATA_VAR_ABS),
        H_USAGE(0x32), H_USAGE(0x35),                                                     /* Z Rz = left and right trigger */
        H_LOGICAL_MIN(0), H_LOGICAL_MAX16(0x7FFF), H_REPORT_COUNT(2), H_INPUT(F_DATA_VAR_ABS),
    H_END_COLLECTION,
};

#define DEVICE_DESC(bcd)                                                                                           \
    18, 0x01,                       /* bLength, DEVICE */                                                          \
    0x00, 0x02,                     /* bcdUSB 2.00 */                                                              \
    0x00, 0x00, 0x00,               /* class, subclass, protocol: defined per interface */                         \
    KM_USB_EP0_MPS,                 /* bMaxPacketSize0 */                                                          \
    LO(KM_USB_VID), HI(KM_USB_VID), LO(KM_USB_PID), HI(KM_USB_PID),                                                \
    LO(bcd), HI(bcd),                                                                                              \
    1, 2, 3,                        /* iManufacturer, iProduct, iSerialNumber */                                   \
    1                               /* bNumConfigurations */

static const uint8_t k_device[] = { DEVICE_DESC(KM_USB_BCD_DEVICE_PLAIN) };
static const uint8_t k_device_pad[] = { DEVICE_DESC(KM_USB_BCD_DEVICE_PAD) };
static const uint8_t k_device_xi[] = { DEVICE_DESC(KM_USB_BCD_DEVICE_XINPUT) };

static const uint8_t k_qualifier[] = {
    10, 0x06,                       /* bLength, DEVICE_QUALIFIER */
    0x00, 0x02, 0x00, 0x00, 0x00,   /* bcdUSB, class, subclass, protocol */
    KM_USB_EP0_MPS, 1, 0,           /* bMaxPacketSize0, bNumConfigurations, reserved */
};

#define CFG_LEN        9u
#define IF_LEN         9u
#define HID_LEN        9u
#define EP_LEN         7u
#define IF0_LEN        (IF_LEN + HID_LEN + 1u * EP_LEN)
#define IF1_LEN        (IF_LEN + HID_LEN + 2u * EP_LEN)
#define IF2_LEN        (IF_LEN + HID_LEN + 1u * EP_LEN)
#define IF3_LEN        (IF_LEN + HID_LEN + 1u * EP_LEN)
#define CFG_TOTAL      (CFG_LEN + IF0_LEN + IF1_LEN + IF2_LEN)
#define CFG_TOTAL_PAD  (CFG_TOTAL + IF3_LEN)
#define XI_CLASS_LEN   17u                                       /* the vendor class descriptor of an XInput interface */
#define IF3_XI_LEN     (IF_LEN + XI_CLASS_LEN + 2u * EP_LEN)
#define CFG_TOTAL_XI   (CFG_TOTAL + IF3_XI_LEN)
#define HID_OFFSET_0   (CFG_LEN + IF_LEN)
#define HID_OFFSET_1   (CFG_LEN + IF0_LEN + IF_LEN)
#define HID_OFFSET_2   (CFG_LEN + IF0_LEN + IF1_LEN + IF_LEN)
#define HID_OFFSET_3   (CFG_LEN + IF0_LEN + IF1_LEN + IF2_LEN + IF_LEN)

#define IFACE(num, neps, sub, proto) IF_LEN, 0x04, (num), 0, (neps), 0x03, (sub), (proto), 0
#define HID(rd) HID_LEN, 0x21, 0x11, 0x01, 0x00, 1, 0x22, LO(sizeof(rd)), HI(sizeof(rd))
#define EP(addr, mps, ival) EP_LEN, 0x05, (addr), 0x03, LO(mps), HI(mps), (ival)

#define CFG_HEADER(total, nif)                                                                                     \
    CFG_LEN, 0x02, LO(total), HI(total),                                                                           \
    (nif), 1, 0,                    /* bNumInterfaces, bConfigurationValue, iConfiguration */                      \
    0x80,                           /* bus powered, no remote wakeup */                                            \
    KM_USB_MAX_POWER_MA / 2u

/* The three interfaces both variants share; the PLAIN variant is exactly this and nothing else. */
#define CFG_IF012                                                                                                  \
    IFACE(KM_USB_IF_KBD, 1, 0x01, 0x01),            /* HID, boot interface subclass, keyboard protocol */          \
    HID(k_report_boot),                                                                                            \
    EP(KM_USB_EP_KBD_IN, KM_USB_BOOT_REPORT_LEN, KM_USB_BINTERVAL_KBD),                                            \
                                                                                                                   \
    IFACE(KM_USB_IF_VND, 2, 0x00, 0x00),                                                                           \
    HID(k_report_vendor),                                                                                          \
    EP(KM_USB_EP_VND_IN, KM_USB_VND_REPORT_LEN, KM_USB_BINTERVAL_VND),                                             \
    EP(KM_USB_EP_VND_OUT, KM_USB_VND_REPORT_LEN, KM_USB_BINTERVAL_VND),                                            \
                                                                                                                   \
    IFACE(KM_USB_IF_NKRO, 1, 0x00, 0x00),                                                                          \
    HID(k_report_nkro),                                                                                            \
    EP(KM_USB_EP_NKRO_IN, 64, KM_USB_BINTERVAL_NKRO)

static const uint8_t k_config[] = {
    CFG_HEADER(CFG_TOTAL, KM_USB_NUM_IF),
    CFG_IF012,
};

static const uint8_t k_config_pad[] = {
    CFG_HEADER(CFG_TOTAL_PAD, KM_USB_NUM_IF_PAD),
    CFG_IF012,

    IFACE(KM_USB_IF_PAD, 1, 0x00, 0x00),            /* HID, no boot subclass */
    HID(k_report_pad),
    EP(KM_USB_EP_PAD_IN, KM_USB_PAD_EP_MPS, KM_USB_BINTERVAL_PAD),
};

/* IF3 of the XINPUT variant (M6 P11): vendor class 0xFF, subclass 0x5D, protocol 0x01 - the triple the Windows XInput driver
 * (xusb) is documented to bind to - then one class-specific descriptor of the layout wired Xbox-360-compatible controllers are seen
 * to send (bcdXInput 1.00, then 0x25 + the IN endpoint + its 20-byte report size, and 0x13 + the OUT endpoint + its 8-byte message
 * size; the rest is zero), then the IN and OUT endpoint descriptors. The endpoint addresses in the blob are ours (0x84 / 0x04);
 * whether the host cares about them is unverified. No HID descriptor, no report descriptor: GET_DESCRIPTOR(HID/report) for IF3 stalls. */
#define XI_CLASS_DESC                                                                                              \
    XI_CLASS_LEN, 0x21, 0x00, 0x01, 0x01, 0x25, KM_USB_EP_XI_IN, KM_USB_XI_REPORT_LEN, 0x00, 0x00, 0x00, 0x00,      \
    0x13, KM_USB_EP_XI_OUT, KM_USB_XI_OUT_MAX, 0x00, 0x00

static const uint8_t k_config_xi[] = {
    CFG_HEADER(CFG_TOTAL_XI, KM_USB_NUM_IF_PAD),
    CFG_IF012,

    IF_LEN, 0x04, KM_USB_IF_PAD, 0, 2, 0xFF, 0x5D, 0x01, 0,
    XI_CLASS_DESC,
    EP(KM_USB_EP_XI_IN, KM_USB_XI_EP_MPS, KM_USB_BINTERVAL_XINPUT),
    EP(KM_USB_EP_XI_OUT, KM_USB_XI_EP_MPS, KM_USB_BINTERVAL_XINPUT),
};

_Static_assert(sizeof k_config_xi == CFG_TOTAL_XI, "XINPUT configuration descriptor length");
_Static_assert(KM_USB_XI_REPORT_LEN <= KM_USB_XI_EP_MPS, "an XInput report fits one packet of EP4");
_Static_assert(sizeof k_config == CFG_TOTAL, "configuration descriptor length");
_Static_assert(sizeof k_config_pad == CFG_TOTAL_PAD, "PAD configuration descriptor length");
_Static_assert(sizeof k_device == sizeof k_device_pad && sizeof k_device == sizeof k_device_xi, "device descriptors have the same length");
_Static_assert(KM_USB_PAD_REPORT_LEN <= KM_USB_PAD_EP_MPS, "a gamepad report fits one packet of EP4");

unsigned km_usb_variant_num_if(km_usb_variant_t v) { return KM_USB_VARIANT_HAS_IF3(v) ? KM_USB_NUM_IF_PAD : KM_USB_NUM_IF; }

const uint8_t *km_usb_device_desc_for(km_usb_variant_t v, size_t *len)
{
    const uint8_t *p = v == KM_USB_VARIANT_PAD ? k_device_pad : v == KM_USB_VARIANT_XINPUT ? k_device_xi : k_device;
    if (len) { *len = sizeof k_device; }   /* all three are 18 bytes */
    return p;
}

const uint8_t *km_usb_config_desc_for(km_usb_variant_t v, size_t *len)
{
    if (v == KM_USB_VARIANT_XINPUT) {
        if (len) { *len = sizeof k_config_xi; }
        return k_config_xi;
    }
    if (len) { *len = v == KM_USB_VARIANT_PAD ? sizeof k_config_pad : sizeof k_config; }
    return v == KM_USB_VARIANT_PAD ? k_config_pad : k_config;
}

const uint8_t *km_usb_qualifier_desc(size_t *len) { if (len) { *len = sizeof k_qualifier; } return k_qualifier; }

const uint8_t *km_usb_hid_desc_for(km_usb_variant_t v, unsigned iface, size_t *len)
{
    static const size_t off[KM_USB_NUM_IF_PAD] = { HID_OFFSET_0, HID_OFFSET_1, HID_OFFSET_2, HID_OFFSET_3 };
    if (iface >= km_usb_variant_num_if(v) || (v == KM_USB_VARIANT_XINPUT && iface == KM_USB_IF_PAD)) {   /* IF3 of XINPUT is not a HID interface */
        return NULL;
    }
    if (len) {
        *len = HID_LEN;
    }
    return km_usb_config_desc_for(v, NULL) + off[iface];
}

const uint8_t *km_usb_report_desc_for(km_usb_variant_t v, unsigned iface, size_t *len)
{
    const uint8_t *p;
    size_t n;
    if (iface >= km_usb_variant_num_if(v) || (v == KM_USB_VARIANT_XINPUT && iface == KM_USB_IF_PAD)) {
        return NULL;
    }
    switch (iface) {
    case KM_USB_IF_KBD:  p = k_report_boot;   n = sizeof k_report_boot;   break;
    case KM_USB_IF_VND:  p = k_report_vendor; n = sizeof k_report_vendor; break;
    case KM_USB_IF_NKRO: p = k_report_nkro;   n = sizeof k_report_nkro;   break;
    case KM_USB_IF_PAD:  p = k_report_pad;    n = sizeof k_report_pad;    break;
    default:             return NULL;
    }
    if (len) {
        *len = n;
    }
    return p;
}

/* The short names are the PLAIN variant: what the keyboard always presented. */
const uint8_t *km_usb_device_desc(size_t *len) { return km_usb_device_desc_for(KM_USB_VARIANT_PLAIN, len); }
const uint8_t *km_usb_config_desc(size_t *len) { return km_usb_config_desc_for(KM_USB_VARIANT_PLAIN, len); }
const uint8_t *km_usb_hid_desc(unsigned iface, size_t *len) { return km_usb_hid_desc_for(KM_USB_VARIANT_PLAIN, iface, len); }
const uint8_t *km_usb_report_desc(unsigned iface, size_t *len) { return km_usb_report_desc_for(KM_USB_VARIANT_PLAIN, iface, len); }

/* ---- strings ---- */
static size_t ascii_desc(const char *s, uint8_t buf[KM_USB_STRING_MAX])
{
    size_t n = strlen(s);
    if (n > (KM_USB_STRING_MAX - 2u) / 2u) {
        n = (KM_USB_STRING_MAX - 2u) / 2u;
    }
    buf[0] = (uint8_t)(2u + 2u * n);
    buf[1] = 0x03;
    for (size_t i = 0; i < n; i++) {
        buf[2 + 2 * i] = (uint8_t)s[i];
        buf[3 + 2 * i] = 0;
    }
    return 2u + 2u * n;
}

static void put_char(uint8_t *out, char c)
{
    out[0] = (uint8_t)c;
    out[1] = 0;
}

size_t km_usb_string_desc(unsigned index, const uint32_t uid[3], uint8_t buf[KM_USB_STRING_MAX])
{
    switch (index) {
    case 0:
        buf[0] = 4;
        buf[1] = 0x03;
        buf[2] = LO(KM_USB_LANGID);
        buf[3] = HI(KM_USB_LANGID);
        return 4;
    case 1:
        return ascii_desc(KM_USB_STR_MANUFACTURER, buf);
    case 2:
        return ascii_desc(KM_USB_STR_PRODUCT, buf);
    case 3: {
        static const char hex[] = "0123456789ABCDEF";
        const size_t prefix = sizeof KM_USB_SERIAL_PREFIX - 1u;
        const size_t chars = prefix + 8u * KM_USB_SERIAL_UID_WORDS;
        _Static_assert(2u + 2u * (sizeof KM_USB_SERIAL_PREFIX - 1u + 8u * KM_USB_SERIAL_UID_WORDS) <= KM_USB_STRING_MAX,
                       "serial number fits the string buffer");
        buf[0] = (uint8_t)(2u + 2u * chars);
        buf[1] = 0x03;
        uint8_t *o = buf + 2;
        for (size_t i = 0; i < prefix; i++, o += 2) {
            put_char(o, KM_USB_SERIAL_PREFIX[i]);
        }
        for (unsigned w = 0; w < KM_USB_SERIAL_UID_WORDS; w++) {
            for (int sh = 28; sh >= 0; sh -= 4, o += 2) {
                put_char(o, hex[(uid[w] >> sh) & 0xFu]);
            }
        }
        return chars * 2u + 2u;
    }
    default:
        return 0;
    }
}
