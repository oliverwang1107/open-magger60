/*
 * km_usb.h - hardware-independent USB device logic of the Magger60 open firmware: descriptors, answers to the EP0
 * control requests, and the bookkeeping of the key-report IN endpoints. Nothing here touches a register; the DWC2
 * driver in port/n32h48x/usb_hs.c feeds it SETUP packets and carries out the results.
 *
 * The descriptors are our own design, written from the USB 2.0 specification, the HID 1.11 specification and the
 * HID Usage Tables (M4_PLAN.md task T-C). The device is one composite device with three HID interfaces:
 *     IF0  boot keyboard (EP1 IN, 8 B)           IF1  vendor channel (EP2 IN + EP10 OUT, 64 B)
 *     IF2  NKRO keyboard bitmap + media keys (EP3 IN, report IDs 1 and 2)
 * and, in the PAD variant only (M6 P5b, chosen when the firmware starts, see km_usb_variant_t):
 *     IF3  gamepad (EP4 IN, 15 B, no report ID)
 * The identifiers and strings are in km_usb_ids.h, the report formats in km_usb_reports.h.
 */
#ifndef KM_USB_H
#define KM_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "km/km_usb_ids.h"
#include "km/km_usb_reports.h"

#define KM_USB_NUM_IF        3u   /* interfaces of the PLAIN variant: the keyboard as it was before the gamepad */
#define KM_USB_IF_KBD        0u
#define KM_USB_IF_VND        1u
#define KM_USB_IF_NKRO       2u
#define KM_USB_IF_PAD        3u   /* PAD variant only */
#define KM_USB_NUM_IF_PAD    4u   /* interfaces of the PAD variant */
#define KM_USB_NUM_IF_MAX    4u   /* size of the per-interface state arrays */
#define KM_USB_EP0_MPS       64u

/* The two descriptor sets. PLAIN is byte for byte what the keyboard presented before M6 (bcdDevice 0x0001); PAD adds
 * interface IF3 / endpoint EP4 (bcdDevice 0x0002). The variant is fixed while the firmware runs, except for the one-way
 * automatic fall-back from PAD to PLAIN (km_usb_guard_t below). */
typedef enum {
    KM_USB_VARIANT_PLAIN = 0,
    KM_USB_VARIANT_PAD = 1,
    KM_USB_VARIANT_XINPUT = 2    /* M6 P11: IF3 is an XInput-style vendor interface (EP4 IN + EP4 OUT) instead of the HID gamepad (bcdDevice 0x0003) */
} km_usb_variant_t;
/* True for the variants that present IF3 / EP4 (PAD and XINPUT): they share the endpoint, its FIFO and the fall-back guard. */
#define KM_USB_VARIANT_HAS_IF3(v) ((v) == KM_USB_VARIANT_PAD || (v) == KM_USB_VARIANT_XINPUT)

/* Endpoint addresses of the configuration descriptor. */
#define KM_USB_EP_KBD_IN     0x81u   /* IF0, 8 bytes */
#define KM_USB_EP_VND_IN     0x82u   /* IF1, 64 bytes: vendor channel */
#define KM_USB_EP_VND_OUT    0x0Au   /* IF1, 64 bytes */
#define KM_USB_EP_NKRO_IN    0x83u   /* IF2, 64 bytes max (reports are 30 and 3 bytes) */
#define KM_USB_EP_PAD_IN     0x84u   /* IF3 (PAD variant), 16 bytes max (the report is KM_USB_PAD_REPORT_LEN = 15) */
#define KM_USB_PAD_EP_MPS    16u
#define KM_USB_EP_XI_IN      0x84u   /* IF3 (XINPUT variant): the same IN endpoint as the HID gamepad, 20 B reports */
#define KM_USB_EP_XI_OUT     0x04u   /* IF3 (XINPUT variant): rumble / LED messages from the host */
#define KM_USB_XI_EP_MPS     32u

/* Longest string descriptor answer (2 header bytes + up to 31 UTF-16 characters). */
#define KM_USB_STRING_MAX    64u

typedef struct {
    uint8_t  bm_request_type;
    uint8_t  b_request;
    uint16_t w_value;
    uint16_t w_index;
    uint16_t w_length;
} km_usb_setup_t;

void km_usb_setup_parse(const uint8_t raw[8], km_usb_setup_t *out);

/* ---- descriptors ----
 * The *_for() forms take the variant; the short names are the PLAIN variant (what the keyboard always presented). */
unsigned km_usb_variant_num_if(km_usb_variant_t v);                   /* 3 or 4 */
const uint8_t *km_usb_device_desc_for(km_usb_variant_t v, size_t *len);
const uint8_t *km_usb_config_desc_for(km_usb_variant_t v, size_t *len);
const uint8_t *km_usb_hid_desc_for(km_usb_variant_t v, unsigned iface, size_t *len);
const uint8_t *km_usb_report_desc_for(km_usb_variant_t v, unsigned iface, size_t *len);
const uint8_t *km_usb_device_desc(size_t *len);
const uint8_t *km_usb_config_desc(size_t *len);
const uint8_t *km_usb_qualifier_desc(size_t *len);                    /* the same for both variants */
const uint8_t *km_usb_hid_desc(unsigned iface, size_t *len);          /* 9 bytes inside the config descriptor */
const uint8_t *km_usb_report_desc(unsigned iface, size_t *len);
/* String descriptor 0..3 into buf (KM_USB_STRING_MAX bytes); returns the length or 0 for an unknown index.
 * Index 0 = language IDs, 1 = manufacturer, 2 = product, 3 = serial (km_usb_ids.h). */
size_t km_usb_string_desc(unsigned index, const uint32_t uid[3], uint8_t buf[KM_USB_STRING_MAX]);

/* ---- control requests ---- */
typedef enum {
    KM_CTRL_STALL = 0,   /* answer with a STALL */
    KM_CTRL_ACK,         /* no data stage: send the zero-length status packet */
    KM_CTRL_IN,          /* send data[0..len) to the host (len already limited to wLength), then expect status */
    KM_CTRL_OUT          /* take wLength bytes from the host: pass every packet to km_usb_ctrl_out_push(), call
                          * km_usb_ctrl_out_end() when the data stage is over, then send the status packet */
} km_ctrl_kind_t;

typedef enum {
    KM_CTRL_APPLY_NONE = 0,
    KM_CTRL_APPLY_ADDRESS,        /* program the device address `value` before the status stage */
    KM_CTRL_APPLY_CONFIGURATION   /* (de)activate the endpoints for configuration `value` */
} km_ctrl_apply_t;

typedef struct {
    km_ctrl_kind_t  kind;
    km_ctrl_apply_t apply;
    uint8_t         value;
    const uint8_t  *data;
    uint16_t        len;
    bool            zlp;         /* IN: the data is shorter than wLength and a multiple of 64: end with a zero-length packet */
} km_ctrl_result_t;

typedef struct {
    km_usb_variant_t variant;    /* the descriptor set presented; survives a bus reset (set by the driver, not by the host) */
    bool     test_refuse_configuration;   /* hardware drill only: refuse SET_CONFIGURATION(1) while the PAD variant is presented */
    uint8_t  configuration;      /* 0 or 1 */
    uint8_t  idle[KM_USB_NUM_IF_MAX];       /* HID idle rate in 4 ms units, 0 = only on change (IF0 and IF2 only) */
    uint8_t  protocol[KM_USB_NUM_IF_MAX];   /* HID protocol, 1 = report (default); 0 = boot is only ever set on IF0 */
    uint32_t uid[3];             /* chip unique ID words (0x1FFFC740, +4, +8) for the serial string */
    uint8_t  scratch[KM_USB_STRING_MAX];   /* backing store for small answers; valid until the next request */

    /* IN endpoint bookkeeping (km_usb_in.c). `last_*` is the report most recently sent: the GET_REPORT answer and the
     * HID idle repeat. All zero (a "nothing pressed" report with its report ID) after a reset. */
    uint8_t  last_kbd[KM_USB_BOOT_REPORT_LEN];
    uint8_t  last_nkro[KM_USB_NKRO_REPORT_LEN];
    uint8_t  last_consumer[KM_USB_CONSUMER_REPORT_LEN];
    uint8_t  last_pad[KM_USB_PAD_REPORT_LEN];   /* IF3: the report most recently queued (GET_REPORT); all zero = centred */
    uint8_t  last_xi[KM_USB_XI_REPORT_LEN];     /* IF3 in the XINPUT variant: the report most recently queued */
    uint8_t  xi_rumble_large, xi_rumble_small;  /* last rumble message of the host (XINPUT variant, 0 after a bus reset) */
    uint8_t  xi_led;                            /* last LED pattern of the host */
    uint32_t xi_out_messages;                   /* rumble / LED messages accepted since power-up (diagnostic) */
    bool     busy_kbd;           /* a report is queued on EP1 and the host has not taken it yet */
    bool     busy_nkro;          /* same for EP3 */
    bool     busy_pad;           /* same for EP4; never affects the two above */
    bool     sent_kbd;           /* at least one report was sent since the configuration was set (idle repeat) */
    bool     sent_nkro;
    uint32_t idle_age[KM_USB_NUM_IF_MAX];   /* micro-frames since the last report on that interface (idle repeat) */

    /* Host keyboard LEDs (km_usb_ctrl.c): the last LED output report the host sent on IF0 or IF2, masked to
     * KM_USB_LED_MASK (bit0 Num Lock, bit1 Caps Lock, bit2 Scroll Lock). 0 after a bus reset = "unknown", all off. */
    uint8_t  host_leds;
    uint32_t led_reports;        /* LED output reports accepted since power-up (diagnostic; survives bus resets) */

    /* EP0 OUT data stage in progress (km_usb_ctrl_out_*): the SETUP that started it and the bytes received so far. */
    km_usb_setup_t out_setup;
    uint8_t  out_buf[KM_USB_EP0_MPS];
    uint16_t out_len;
    bool     out_active;         /* a KM_CTRL_OUT request was accepted and its data stage is not finished */
    bool     out_bad;            /* more data arrived than the request announced, or more than the buffer holds */
} km_usb_dev_t;

/* True when a data stage of `have` bytes answering a request for `want` bytes must end with a zero-length packet. */
bool km_usb_ctrl_needs_zlp(size_t have, uint16_t want);

void km_usb_dev_reset(km_usb_dev_t *dev);   /* bus reset: configuration 0, HID defaults, IN state cleared (uid, variant and the drill flag are kept) */
void km_usb_ctrl_handle(km_usb_dev_t *dev, const km_usb_setup_t *setup, km_ctrl_result_t *out);

/* ---- EP0 OUT data stage (HID SET_REPORT) ----
 * After km_usb_ctrl_handle() returned KM_CTRL_OUT the driver receives the data stage and calls
 *   km_usb_ctrl_out_push() once per received data packet (any size, in order; zero-length packets may be skipped), then
 *   km_usb_ctrl_out_end() when the data stage has ended (short packet or wLength bytes), BEFORE the status stage.
 * Only a request that delivered exactly the wLength bytes it announced is acted upon; a short, long, repeated or stray
 * data stage changes nothing. A new SETUP packet (any km_usb_ctrl_handle call) abandons a data stage in progress.
 * km_usb_ctrl_out_end() returns true when the data was a valid LED output report and host_leds was updated. */
void km_usb_ctrl_out_push(km_usb_dev_t *dev, const uint8_t *data, unsigned len);
bool km_usb_ctrl_out_end(km_usb_dev_t *dev);

/* ---- key-report IN endpoints (km_usb_in.c) ----
 * The driver supplies `start`: it queues `len` bytes on IN endpoint `ep` (1 or 3) and returns at once; the host takes
 * them at its next poll and the driver then calls km_usb_in_complete(). Nothing here blocks. */
typedef void (*km_usb_in_start_fn)(unsigned ep, const uint8_t *data, unsigned len);

void km_usb_in_reset(km_usb_dev_t *dev);                                  /* configuration changed or bus reset */
bool km_usb_in_kbd_ready(const km_usb_dev_t *dev);                        /* configured and EP1 idle */
bool km_usb_in_nkro_ready(const km_usb_dev_t *dev);                       /* configured and EP3 idle */
/* Queue an 8-byte boot report on EP1. False (and nothing queued) when not configured or the previous one is in flight. */
bool km_usb_in_send_kbd(km_usb_dev_t *dev, const uint8_t report[KM_USB_BOOT_REPORT_LEN], km_usb_in_start_fn start);
/* Queue an NKRO report on EP3: 30 bytes starting 0x01 (keys) or 3 bytes starting 0x02 (consumer). Any other length or ID is
 * refused. False also when not configured or in flight. */
bool km_usb_in_send_nkro(km_usb_dev_t *dev, const uint8_t *report, unsigned len, km_usb_in_start_fn start);
void km_usb_in_complete(km_usb_dev_t *dev, unsigned ep);                  /* the host took the report queued on `ep` (1, 3 or 4) */
/* Time passes: `uframes` micro-frames (125 us) since the last call. Repeats the last report when the HID idle rate asks for it. */
void km_usb_in_tick(km_usb_dev_t *dev, uint32_t uframes, km_usb_in_start_fn start);

/* ---- gamepad endpoint (IF3 / EP4, PAD variant) ----
 * Independent of the keyboard endpoints: its own busy flag, its own FIFO. If the host never polls it, EP4 stays busy and
 * km_usb_in_send_pad() keeps returning false; nothing else is affected. The caller sends the CURRENT state again whenever the
 * endpoint is ready and the state differs from the last one queued (reports are states, not events). */
typedef struct {
    uint16_t buttons;            /* bit n = button n+1 */
    uint8_t  hat;                /* 0 centred, 1 up ... 8 up-left */
    int16_t  lx, ly, rx, ry;     /* -32767..32767 */
    uint16_t lt, rt;             /* 0..32767 */
} km_usb_pad_state_t;

/* The one place that writes and reads the 15-byte layout of km_usb_reports.h. pack() clamps nothing: it returns false (and
 * writes nothing) when a field is outside its range (hat > 8, a stick at -32768, a trigger above 32767). unpack() is the
 * exact inverse and rejects the same. */
bool km_usb_pad_pack(const km_usb_pad_state_t *s, uint8_t out[KM_USB_PAD_REPORT_LEN]);
bool km_usb_pad_unpack(const uint8_t in[KM_USB_PAD_REPORT_LEN], km_usb_pad_state_t *s);
/* The synthetic state the hardware test plays `t_ms` after it started: every axis a triangle wave with its own period
 * (KM_USB_PAD_SWEEP_*_MS), one button and one hat direction per second. */
void km_usb_pad_sweep(uint32_t t_ms, km_usb_pad_state_t *s);

/* ---- XInput-style interface (XINPUT variant, M6 P11) ----
 * km_usb_xinput_pack() writes the 20-byte report of km_usb_reports.h from a pad state (sticks: +Y is up, so LY / RY are negated;
 * triggers 0..32767 -> 0..255, rounded). Returns false and writes nothing for an out-of-range state (same rules as km_usb_pad_pack()).
 * Button mapping from the HID pad button numbers (the numbers the gamepad page shows): 1 A, 2 B, 3 X, 4 Y, 5 LB, 6 RB, 7 Back,
 * 8 Start, 9 left stick click, 10 right stick click, 11 Guide; 12..16 have no XInput equivalent and are dropped. The hat becomes the
 * four pad bits (diagonals set two). */
bool km_usb_xinput_pack(const km_usb_pad_state_t *s, uint8_t out[KM_USB_XI_REPORT_LEN]);
/* The same from the 15-byte report of km_pad / km_usb_pad_pack(); false (nothing written) if the input is invalid. */
bool km_usb_xinput_from_pad(const uint8_t pad[KM_USB_PAD_REPORT_LEN], uint8_t out[KM_USB_XI_REPORT_LEN]);
/* The host's OUT message on EP4 (rumble 8 bytes / LED 3 bytes). Returns true when it was one of those and was recorded. */
bool km_usb_xinput_out(km_usb_dev_t *dev, const uint8_t *data, unsigned len);
bool km_usb_in_xinput_ready(const km_usb_dev_t *dev);                     /* XINPUT variant, configured and EP4 idle */
bool km_usb_in_send_xinput(km_usb_dev_t *dev, const uint8_t *report, unsigned len, km_usb_in_start_fn start);   /* exactly 20 bytes */

bool km_usb_in_pad_ready(const km_usb_dev_t *dev);                        /* PAD variant, configured and EP4 idle */
/* Queue a gamepad report on EP4. Exactly KM_USB_PAD_REPORT_LEN bytes; false (nothing queued) for any other length, in the
 * PLAIN variant, when not configured, or when the previous report has not been taken. */
bool km_usb_in_send_pad(km_usb_dev_t *dev, const uint8_t *report, unsigned len, km_usb_in_start_fn start);

/* ---- fall-back from PAD to PLAIN ----
 * Pure bookkeeping, driven by the driver with a millisecond clock. The clock starts at the FIRST bus reset the host makes
 * (a host that is not there never resets the bus, so there is nothing to fall back from) and stops for good when the host
 * sets configuration 1. If KM_USB_PAD_FALLBACK_MS pass without that, km_usb_guard_poll() returns true ONCE: the driver
 * must drop off the bus and come back with the PLAIN variant. After that the guard is finished until the next power-up. */
#define KM_USB_PAD_FALLBACK_MS 6000u
typedef struct {
    km_usb_variant_t requested;  /* what the firmware was started with */
    km_usb_variant_t active;     /* what is presented now: requested, or PLAIN after the fall-back */
    bool     running;            /* the clock runs */
    bool     fell_back;
    bool     accepted;           /* the host configured the device: the PAD variant works */
    uint32_t started_ms;
} km_usb_guard_t;

void km_usb_guard_init(km_usb_guard_t *g, km_usb_variant_t requested);
void km_usb_guard_on_reset(km_usb_guard_t *g, uint32_t now_ms);
void km_usb_guard_on_configured(km_usb_guard_t *g);
bool km_usb_guard_poll(km_usb_guard_t *g, uint32_t now_ms);

/* ---- what the USB core can do (GHWCFG1..4) and where the FIFOs go ----
 * The decoding follows the common DWC2 core; for this chip the field positions are UNVERIFIED (the diagnostic image prints the
 * raw words so they can be checked against the reference manual). km_usb_hwcfg_check() answers "does this variant fit". */
typedef struct {
    uint32_t raw[4];             /* GHWCFG1..4 as read */
    uint8_t  ep_dir[16];         /* GHWCFG1: 0 bidirectional, 1 IN only, 2 OUT only, 3 reserved */
    uint8_t  num_dev_eps;        /* GHWCFG2[13:10]: device endpoints besides EP0 */
    uint8_t  num_in_eps;         /* GHWCFG4[29:26] + 1: IN endpoints including EP0 */
    bool     ded_fifo;           /* GHWCFG4[25]: one TX FIFO per IN endpoint */
    uint16_t fifo_depth;         /* GHWCFG3[31:16]: total FIFO RAM in 32-bit words */
} km_usb_hwcfg_t;

#define KM_USB_HW_NO_DED_FIFO   0x01u   /* check() bits: the dedicated-FIFO scheme of usb_hs.c is not available */
#define KM_USB_HW_TOO_FEW_IN    0x02u   /* fewer IN endpoints than the highest IN endpoint number the variant uses needs */
#define KM_USB_HW_IN_DIR        0x04u   /* a used IN endpoint is declared OUT only (or reserved) */
#define KM_USB_HW_OUT_DIR       0x08u   /* the vendor OUT endpoint is declared IN only (or reserved) */
#define KM_USB_HW_TOO_FEW_EPS   0x10u   /* fewer device endpoints than the highest endpoint number used */
#define KM_USB_HW_FIFO_SPACE    0x20u   /* the FIFO plan does not fit in the FIFO RAM */

void km_usb_hwcfg_decode(const uint32_t raw[4], km_usb_hwcfg_t *out);
uint32_t km_usb_hwcfg_check(const km_usb_hwcfg_t *hw, km_usb_variant_t v);    /* 0 = it fits */

/* FIFO layout in 32-bit words, in register order: the receive FIFO first, then the transmit FIFO of EP0, EP1, EP2, EP3 and (PAD
 * variant) EP4, back to back from 0. The driver programs exactly this; the values for the PLAIN variant are the ones the
 * keyboard has always used. The whole plan stays inside KM_USB_FIFO_TOTAL_WORDS, the RAM size the stock bootloader assumes. */
#define KM_USB_FIFO_TOTAL_WORDS 0x100u
#define KM_USB_FIFO_RX          0xFFu    /* `ep` of the receive FIFO entry */
#define KM_USB_FIFO_MAX         6u
typedef struct { uint8_t ep; uint16_t start; uint16_t depth; } km_usb_fifo_t;
unsigned km_usb_fifo_plan(km_usb_variant_t v, km_usb_fifo_t out[KM_USB_FIFO_MAX]);   /* returns the number of entries */

#endif /* KM_USB_H */
