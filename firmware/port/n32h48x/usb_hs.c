/*
 * USB HS device driver, polled. See usb_hs.h and USB_BRINGUP_NOTES.md.
 *
 * Structure follows the classic DWC2 slave-mode (no DMA) programming model. The constants for the
 * Nations-specific parts (RCC_USBHSCTRL1/2 bits, FIFO sizes, TRDT) are the set that made the keyboard enumerate
 * (USB_BRINGUP_NOTES.md section 2); the meaning of the individual PHY bits is not documented anywhere we have.
 */
#include "usb_hs.h"

#include <string.h>

#include "km/km_usb.h"
#include "n32h48x_regs.h"

/* ---- registers ---- */
#define RCC_BASE         0x40021000u
#define RCC_REG(off)     KM_REG32(RCC_BASE + (off))
#define GPIOB_BASE       0x4002C400u
#define AFIO_RMP_CFG     KM_REG32(0x40010800u)
#define UID_BASE         0x1FFFC740u

#define USB_BASE         0x40040000u
#define U(off)           KM_REG32(USB_BASE + (off))
#define GAHBCFG          U(0x008)
#define GUSBCFG          U(0x00C)
#define GRSTCTL          U(0x010)
#define GINTSTS          U(0x014)
#define GINTMSK          U(0x018)
#define GRXSTSP          U(0x020)
#define GRXFSIZ          U(0x024)
#define GNPTXFSIZ        U(0x028)     /* = DIEPTXF0 */
#define DIEPTXF(n)       U(0x100 + 4u * ((n) - 1u))
#define DCFG             U(0x800)
#define DCTL             U(0x804)
#define DSTS             U(0x808)
#define DIEPMSK          U(0x810)
#define DOEPMSK          U(0x814)
#define DAINT            U(0x818)
#define DAINTMSK         U(0x81C)
#define DIEPCTL(n)       U(0x900 + 0x20u * (n))
#define DIEPINT(n)       U(0x908 + 0x20u * (n))
#define DIEPTSIZ(n)      U(0x910 + 0x20u * (n))
#define DOEPCTL(n)       U(0xB00 + 0x20u * (n))
#define DOEPINT(n)       U(0xB08 + 0x20u * (n))
#define DOEPTSIZ(n)      U(0xB10 + 0x20u * (n))
#define GHWCFG(n)        U(0x40 + 4u * (n))   /* GHWCFG1..4, read-only hardware configuration */
#define PCGCCTL          U(0xE00)
#define FIFO(n)          U(0x1000 + 0x1000u * (n))

/* GRSTCTL */
#define GRSTCTL_CSRST        (1u << 0)
#define GRSTCTL_RXFFLSH      (1u << 4)
#define GRSTCTL_TXFFLSH      (1u << 5)
#define GRSTCTL_CSRSTDONE    (1u << 29)
#define GRSTCTL_AHBIDL       (1u << 31)
/* GINTSTS */
#define GINT_WKUPINT         (1u << 31)   /* resume / remote wake-up detected (status bit, polled) */
#define GINT_SOF             (1u << 3)    /* status bit, set at every start of (micro)frame even though it is not unmasked */
#define GINT_RXFLVL          (1u << 4)
#define GINT_USBSUSP         (1u << 11)
#define GINT_USBRST          (1u << 12)
#define GINT_ENUMDNE         (1u << 13)
#define GINT_IEPINT          (1u << 18)
#define GINT_OEPINT          (1u << 19)
/* DCTL */
#define DCTL_RWUSIG          (1u << 0)
#define DCTL_SDIS            (1u << 1)
#define DCTL_CGINAK          (1u << 10)
/* endpoint control */
#define EPCTL_USBAEP         (1u << 15)
#define EPCTL_STALL          (1u << 21)
#define EPCTL_CNAK           (1u << 26)
#define EPCTL_SNAK           (1u << 27)
#define EPCTL_SD0PID         (1u << 28)
#define EPCTL_EPDIS          (1u << 30)
#define EPCTL_EPENA          (1u << 31)
/* endpoint interrupts */
#define EPINT_XFRC           (1u << 0)
#define EPINT_EPDISD         (1u << 1)
#define EPINT_STUP           (1u << 3)    /* OUT endpoints: SETUP phase done */

#define EP_TYPE_INTERRUPT    3u
#define NUM_EP               16u
#define VENDOR_OUT_EP        10u
#define PAD_EP               (KM_USB_EP_PAD_IN & 0xFu)
#define XI_OUT_EP            (KM_USB_EP_XI_OUT & 0xFu)   /* XINPUT variant: rumble / LED messages */
#define RECONNECT_GAP_MS     300u     /* D+ pull-up off for this long: hosts debounce a disconnect for 100 ms or more */

/* ---- timing helpers (the IWDG is fed while waiting; the loop count assumes the 240 MHz the bootloader sets up) ---- */
static void delay_ms(uint32_t ms)
{
    while (ms--) {
        km_iwdg_feed();
        for (volatile uint32_t i = 0; i < 60000u; i++) {
        }
    }
}

static void delay_us(uint32_t us)
{
    for (volatile uint32_t i = 0; i < us * 60u; i++) {
    }
}

/* ---- driver state ---- */
typedef enum { EP0_IDLE, EP0_DATA_IN, EP0_STATUS_IN, EP0_DATA_OUT, EP0_STATUS_OUT } ep0_state_t;

static km_usb_dev_t g_dev;
static uint8_t g_setup[8];
static ep0_state_t g_ep0_state;
static const uint8_t *g_tx_ptr;
static uint16_t g_tx_left;
static bool g_tx_zlp;
static uint8_t g_vendor_rx[64];
static uint8_t g_vendor_rx_len;
static bool g_vendor_rx_ready;     /* a complete 64-byte report waits in g_vendor_rx */
static bool g_vendor_tx_busy;      /* the reply is queued in the EP2 FIFO and not yet taken by the host */
static km_usbhs_vendor_fn g_vendor_fn;
static km_usbhs_events_t g_events;
static km_usb_guard_t g_guard;     /* the PAD variant's fall-back (needs a clock: km_usbhs_set_clock) */
static uint32_t (*g_clock)(void);
static uint32_t g_uframes_seen;    /* SOF events counted by km_usbhs_poll() and not yet handed to km_usb_in_tick() */

void km_usbhs_set_vendor_handler(km_usbhs_vendor_fn fn)
{
    g_vendor_fn = fn;
}

bool km_usbhs_configured(void)
{
    return g_dev.configuration != 0;
}

void km_usbhs_set_clock(uint32_t (*now_ms)(void))
{
    g_clock = now_ms;
}

km_usb_variant_t km_usbhs_variant(void)
{
    return g_dev.variant;
}

bool km_usbhs_fell_back(void)
{
    return g_guard.fell_back;
}

bool km_usbhs_fallback_clock_running(void)
{
    return g_guard.running;
}

void km_usbhs_rehearsal_refuse_configuration(bool on)
{
    g_dev.test_refuse_configuration = on;
}

void km_usbhs_read_hwcfg(uint32_t out[4])
{
    for (unsigned i = 0; i < 4; i++) {
        out[i] = GHWCFG(i + 1u);
    }
}

void km_usbhs_get_events(km_usbhs_events_t *out)
{
    *out = g_events;
}

uint8_t km_usbhs_host_leds(void)
{
    return g_dev.host_leds;
}

uint32_t km_usbhs_led_report_count(void)
{
    return g_dev.led_reports;
}

bool km_usbhs_suspended(void)
{
    return (DSTS & 1u) != 0;                     /* SUSPSTS */
}

static void fifo_write(unsigned ep, const uint8_t *data, unsigned len);

/* diagnostics for the 8 kHz work: SOFs seen by the poll (a micro-frame each at high speed, a frame each at full speed) and the speed the
 * core reported at the end of enumeration */
static uint32_t g_sof_total;
static uint8_t g_enum_speed = 0xFFu;
uint32_t km_usbhs_sof_count(void) { return g_sof_total; }
uint8_t km_usbhs_enum_speed(void) { return g_enum_speed; }
uint32_t km_usbhs_dsts(void) { return DSTS; }

/* ---- key reports (IF0 boot keyboard on EP1, IF2 NKRO + consumer on EP3); the policy is in km_usb_in.c ---- */
static void in_start(unsigned ep, const uint8_t *data, unsigned len)
{
    DIEPTSIZ(ep) = (1u << 19) | len;                 /* one packet; the endpoints' packet sizes are >= the report sizes */
    DIEPCTL(ep) |= EPCTL_EPENA | EPCTL_CNAK;
    fifo_write(ep, data, len);
}

bool km_usbhs_kbd_ready(void)
{
    return km_usb_in_kbd_ready(&g_dev);
}

bool km_usbhs_nkro_ready(void)
{
    return km_usb_in_nkro_ready(&g_dev);
}

bool km_usbhs_send_kbd(const uint8_t report[8])
{
    return km_usb_in_send_kbd(&g_dev, report, in_start);
}

bool km_usbhs_send_nkro(const uint8_t *report, unsigned len)
{
    return km_usb_in_send_nkro(&g_dev, report, len, in_start);
}

bool km_usbhs_pad_ready(void)
{
    return km_usb_in_pad_ready(&g_dev);
}

bool km_usbhs_send_pad(const uint8_t *report, unsigned len)
{
    return km_usb_in_send_pad(&g_dev, report, len, in_start);
}

bool km_usbhs_xinput_ready(void)
{
    return km_usb_in_xinput_ready(&g_dev);
}

bool km_usbhs_send_xinput(const uint8_t *report, unsigned len)
{
    return km_usb_in_send_xinput(&g_dev, report, len, in_start);
}

void km_usbhs_xinput_rumble(uint8_t *large, uint8_t *small)
{
    *large = g_dev.xi_rumble_large;
    *small = g_dev.xi_rumble_small;
}

bool km_usbhs_kbd_boot_protocol(void)
{
    return g_dev.protocol[KM_USB_IF_KBD] == 0;
}

void km_usbhs_disconnect(void)
{
    DCTL |= DCTL_SDIS;
    delay_ms(30);
}

/* ---- FIFO access ---- */
static void fifo_write(unsigned ep, const uint8_t *data, unsigned len)
{
    for (unsigned i = 0; i < len; i += 4) {
        uint32_t w = 0;
        for (unsigned b = 0; b < 4 && i + b < len; b++) {
            w |= (uint32_t)data[i + b] << (8 * b);
        }
        FIFO(ep) = w;
    }
}

static void fifo_read(uint8_t *dst, unsigned len)
{
    for (unsigned i = 0; i < len; i += 4) {
        const uint32_t w = FIFO(0);
        for (unsigned b = 0; b < 4 && i + b < len; b++) {
            if (dst != NULL) {
                dst[i + b] = (uint8_t)(w >> (8 * b));
            }
        }
    }
}

static void flush_tx_all(void)
{
    GRSTCTL = (0x10u << 6) | GRSTCTL_TXFFLSH;
    for (unsigned t = 0; t < 200000u && (GRSTCTL & GRSTCTL_TXFFLSH); t++) {
    }
}

static void flush_rx(void)
{
    GRSTCTL = GRSTCTL_RXFFLSH;
    for (unsigned t = 0; t < 200000u && (GRSTCTL & GRSTCTL_RXFFLSH); t++) {
    }
}

/* ---- endpoint 0 ---- */
static void ep0_out_arm(void)
{
    DOEPTSIZ(0) = (3u << 29) | (1u << 19) | (3u * 8u);    /* 3 back-to-back SETUP packets, one OUT packet */
    DOEPCTL(0) |= EPCTL_EPENA | EPCTL_CNAK;
}

static void ep0_send(const uint8_t *data, unsigned len)
{
    DIEPTSIZ(0) = (1u << 19) | len;
    DIEPCTL(0) |= EPCTL_EPENA | EPCTL_CNAK;
    if (len != 0) {
        fifo_write(0, data, len);
    }
}

static void ep0_send_next_chunk(void)
{
    const unsigned n = g_tx_left > KM_USB_EP0_MPS ? KM_USB_EP0_MPS : g_tx_left;
    ep0_send(g_tx_ptr, n);
    g_tx_ptr += n;
    g_tx_left = (uint16_t)(g_tx_left - n);
}

static void ep0_stall(void)
{
    DIEPCTL(0) |= EPCTL_STALL;
    DOEPCTL(0) |= EPCTL_STALL;
}

/* ---- data endpoints ---- */
static void vendor_out_arm(void)
{
    DOEPTSIZ(VENDOR_OUT_EP) = (1u << 19) | 64u;
    DOEPCTL(VENDOR_OUT_EP) |= EPCTL_EPENA | EPCTL_CNAK;
}

static void xi_out_arm(void)
{
    DOEPTSIZ(XI_OUT_EP) = (1u << 19) | KM_USB_XI_EP_MPS;
    DOEPCTL(XI_OUT_EP) |= EPCTL_EPENA | EPCTL_CNAK;
}

static void activate_in(unsigned ep, unsigned mps)
{
    DIEPCTL(ep) |= mps | (EP_TYPE_INTERRUPT << 18) | (ep << 22) | EPCTL_USBAEP | EPCTL_SD0PID | EPCTL_SNAK;
    DAINTMSK |= 1u << ep;
}

static void set_configuration(unsigned value)
{
    flush_tx_all();                  /* nothing queued by the previous configuration may reach the host */
    if (value == 0) {
        for (unsigned ep = 1; ep <= PAD_EP; ep++) {
            DIEPCTL(ep) &= ~EPCTL_USBAEP;
        }
        DOEPCTL(VENDOR_OUT_EP) &= ~EPCTL_USBAEP;
        DOEPCTL(XI_OUT_EP) &= ~EPCTL_USBAEP;
        DAINTMSK &= 0x00010001u;
        return;
    }
    activate_in(KM_USB_EP_KBD_IN & 0xFu, 8);
    activate_in(KM_USB_EP_VND_IN & 0xFu, 64);
    activate_in(KM_USB_EP_NKRO_IN & 0xFu, 64);
    if (g_dev.variant == KM_USB_VARIANT_PAD) {
        activate_in(PAD_EP, KM_USB_PAD_EP_MPS);
    } else if (g_dev.variant == KM_USB_VARIANT_XINPUT) {
        activate_in(PAD_EP, KM_USB_XI_EP_MPS);
        DOEPCTL(XI_OUT_EP) |= KM_USB_XI_EP_MPS | (EP_TYPE_INTERRUPT << 18) | EPCTL_USBAEP | EPCTL_SD0PID;
        DAINTMSK |= 1u << (16 + XI_OUT_EP);
        xi_out_arm();
    }
    DOEPCTL(VENDOR_OUT_EP) |= 64u | (EP_TYPE_INTERRUPT << 18) | EPCTL_USBAEP | EPCTL_SD0PID;
    DAINTMSK |= 1u << (16 + VENDOR_OUT_EP);
    g_vendor_rx_len = 0;
    g_vendor_rx_ready = false;
    g_vendor_tx_busy = false;
    vendor_out_arm();
}

static void handle_setup(void)
{
    km_usb_setup_t s;
    km_ctrl_result_t r;
    km_usb_setup_parse(g_setup, &s);
    km_usb_ctrl_handle(&g_dev, &s, &r);
    g_ep0_state = EP0_IDLE;

    switch (r.kind) {
    case KM_CTRL_IN:
        g_tx_ptr = r.data;
        g_tx_left = r.len;
        g_tx_zlp = r.zlp;
        g_ep0_state = EP0_DATA_IN;
        ep0_out_arm();                  /* the status stage is an OUT packet */
        ep0_send_next_chunk();
        break;
    case KM_CTRL_ACK:
        if (r.apply == KM_CTRL_APPLY_ADDRESS) {
            DCFG = (DCFG & ~(0x7Fu << 4)) | ((uint32_t)r.value << 4);
        } else if (r.apply == KM_CTRL_APPLY_CONFIGURATION) {
            set_configuration(r.value);
            if (r.value == 1) {
                km_usb_guard_on_configured(&g_guard);     /* the host accepted this descriptor set: no fall-back needed */
            }
        }
        g_ep0_state = EP0_STATUS_IN;
        ep0_out_arm();
        ep0_send(NULL, 0);
        break;
    case KM_CTRL_OUT:
        g_ep0_state = EP0_DATA_OUT;
        DOEPTSIZ(0) = (3u << 29) | (1u << 19) | r.len;
        DOEPCTL(0) |= EPCTL_EPENA | EPCTL_CNAK;
        break;
    case KM_CTRL_STALL:
    default:
        ep0_stall();
        ep0_out_arm();
        break;
    }
}

/* ---- bus events ---- */
static void on_reset(void)
{
    GINTSTS = GINT_USBRST;
    g_events.resets++;
    DCTL &= ~DCTL_RWUSIG;
    flush_tx_all();
    flush_rx();
    for (unsigned n = 0; n < NUM_EP; n++) {
        DIEPINT(n) = 0xFB7Fu;
        DIEPCTL(n) &= ~EPCTL_STALL;
        DIEPCTL(n) |= EPCTL_SNAK;
        DOEPINT(n) = 0xFB7Fu;
        DOEPCTL(n) &= ~EPCTL_STALL;
        DOEPCTL(n) |= EPCTL_SNAK;
    }
    DAINTMSK = 0x00010001u;                      /* EP0 IN and OUT */
    DOEPMSK = EPINT_XFRC | EPINT_EPDISD | EPINT_STUP;
    DIEPMSK = EPINT_XFRC | EPINT_EPDISD | (1u << 3);
    DCFG &= ~(0x7Fu << 4);                       /* address 0 */
    km_usb_dev_reset(&g_dev);
    g_ep0_state = EP0_IDLE;
    g_vendor_rx_len = 0;
    g_vendor_rx_ready = false;
    g_vendor_tx_busy = false;
    ep0_out_arm();
    if (g_clock != NULL) {
        km_usb_guard_on_reset(&g_guard, g_clock());       /* the first reset of the host starts the fall-back clock */
    }
}

static void on_enum_done(void)
{
    GINTSTS = GINT_ENUMDNE;
    g_enum_speed = (uint8_t)((DSTS >> 1) & 3u);   /* DSTS.ENUMSPD: 0 = high speed (30/60 MHz PHY clock), 1 = full speed (PHY at 30/60 MHz), 3 = full speed (48 MHz) */
    DCTL |= DCTL_CGINAK;
    DIEPCTL(0) &= ~3u;                           /* EP0 maximum packet size 64 */
    GUSBCFG = (GUSBCFG & ~(0xFu << 10)) | (9u << 10);   /* USB turnaround time for the HS PHY (value that works with the HS PHY on this chip) */
}

static void on_rxflvl(void)
{
    while (GINTSTS & GINT_RXFLVL) {
        const uint32_t v = GRXSTSP;
        const unsigned ep = v & 0xFu;
        const unsigned bytes = (v >> 4) & 0x7FFu;
        const unsigned status = (v >> 17) & 0xFu;
        if (status == 6u) {                       /* SETUP packet received */
            uint8_t raw[8];
            fifo_read(raw, 8);
            memcpy(g_setup, raw, 8);
        } else if (status == 2u && bytes != 0) {  /* OUT data */
            if (ep == VENDOR_OUT_EP) {
                const unsigned n = bytes > sizeof g_vendor_rx ? (unsigned)sizeof g_vendor_rx : bytes;
                fifo_read(g_vendor_rx, n);
                if (bytes > n) {
                    fifo_read(NULL, bytes - n);
                }
                g_vendor_rx_len = (uint8_t)n;
            } else if (ep == XI_OUT_EP && g_dev.variant == KM_USB_VARIANT_XINPUT) {   /* rumble / LED message: recorded, no motor */
                uint8_t msg[KM_USB_XI_EP_MPS];
                const unsigned n = bytes > sizeof msg ? (unsigned)sizeof msg : bytes;
                fifo_read(msg, n);
                if (bytes > n) {
                    fifo_read(NULL, bytes - n);
                }
                (void)km_usb_xinput_out(&g_dev, msg, n);
            } else if (ep == 0 && g_ep0_state == EP0_DATA_OUT) {   /* data stage of SET_REPORT: km_usb_ctrl_out_*() judges it */
                for (unsigned left = bytes; left != 0;) {
                    uint8_t chunk[KM_USB_EP0_MPS];
                    const unsigned n = left > sizeof chunk ? (unsigned)sizeof chunk : left;
                    fifo_read(chunk, n);
                    km_usb_ctrl_out_push(&g_dev, chunk, n);
                    left -= n;
                }
            } else {
                fifo_read(NULL, bytes);           /* a stray OUT packet: drained, not used */
            }
        }
        /* status 3 (OUT transfer complete) and 4 (SETUP complete) carry no data */
    }
}

static void on_out_endpoints(void)
{
    const uint32_t pending = (DAINT & DAINTMSK) >> 16;
    for (unsigned ep = 0; ep < NUM_EP; ep++) {
        if (!(pending & (1u << ep))) {
            continue;
        }
        const uint32_t v = DOEPINT(ep);
        DOEPINT(ep) = v & ~(EPINT_XFRC | EPINT_STUP);   /* other bits are not used */
        if (v & EPINT_STUP) {
            DOEPINT(ep) = EPINT_STUP;
            if (ep == 0) {
                handle_setup();
            }
        }
        if (v & EPINT_XFRC) {
            DOEPINT(ep) = EPINT_XFRC;
            if (ep == 0) {
                if (g_ep0_state == EP0_DATA_OUT) {
                    (void)km_usb_ctrl_out_end(&g_dev);   /* act on the data (host LEDs) before the status stage */
                    g_ep0_state = EP0_STATUS_IN;
                    ep0_out_arm();
                    ep0_send(NULL, 0);
                } else {
                    g_ep0_state = EP0_IDLE;
                    ep0_out_arm();
                }
            } else if (ep == VENDOR_OUT_EP) {
                g_vendor_rx_ready = true;       /* re-armed by vendor_task() once the report has been consumed */
            } else if (ep == XI_OUT_EP && g_dev.variant == KM_USB_VARIANT_XINPUT) {
                xi_out_arm();                   /* the message was taken in on_rxflvl(); listen for the next one */
            }
        }
    }
}

static void on_in_endpoints(void)
{
    const uint32_t pending = (DAINT & DAINTMSK) & 0xFFFFu;
    for (unsigned ep = 0; ep < NUM_EP; ep++) {
        if (!(pending & (1u << ep))) {
            continue;
        }
        const uint32_t v = DIEPINT(ep);
        DIEPINT(ep) = v & 0xFB7Fu;                       /* TXFE (bit 7) is a status bit, not cleared here */
        if ((v & EPINT_XFRC) && ep == (KM_USB_EP_VND_IN & 0xFu)) {
            g_vendor_tx_busy = false;
        }
        if ((v & EPINT_XFRC) && (ep == (KM_USB_EP_KBD_IN & 0xFu) || ep == (KM_USB_EP_NKRO_IN & 0xFu) || ep == PAD_EP)) {
            km_usb_in_complete(&g_dev, ep);
        }
        if ((v & EPINT_XFRC) && ep == 0) {
            if (g_ep0_state == EP0_DATA_IN) {
                if (g_tx_left != 0) {
                    ep0_send_next_chunk();
                } else if (g_tx_zlp) {
                    g_tx_zlp = false;
                    ep0_send(NULL, 0);
                } else {
                    g_ep0_state = EP0_STATUS_OUT;       /* the host's zero-length OUT ends the transfer */
                }
            } else if (g_ep0_state == EP0_STATUS_IN) {
                g_ep0_state = EP0_IDLE;
            }
        }
    }
}

/* One request at a time: consume the received report, queue the reply on EP2 (the host's next IN poll takes it). */
static void vendor_task(void)
{
    if (!g_vendor_rx_ready || g_vendor_tx_busy) {
        return;
    }
    uint8_t out[64];
    const bool reply = (g_vendor_fn != NULL) && g_vendor_fn(g_vendor_rx, out);
    g_vendor_rx_ready = false;
    vendor_out_arm();
    if (reply) {
        const unsigned ep = KM_USB_EP_VND_IN & 0xFu;
        g_vendor_tx_busy = true;
        DIEPTSIZ(ep) = (1u << 19) | 64u;
        DIEPCTL(ep) |= EPCTL_EPENA | EPCTL_CNAK;
        fifo_write(ep, out, 64);
    }
}

/* The host has not configured the PAD descriptor set in KM_USB_PAD_FALLBACK_MS: leave the bus, come back with the PLAIN set (the
 * keyboard the host has always seen). Blocks for RECONNECT_GAP_MS with the watchdog fed; happens at most once per power-up. */
static void fall_back_to_plain(void)
{
    DCTL |= DCTL_SDIS;                           /* D+ pull-up off: the host sees the device leave */
    set_configuration(0);
    delay_ms(RECONNECT_GAP_MS);
    g_dev.variant = g_guard.active;              /* PLAIN */
    km_usb_dev_reset(&g_dev);
    g_ep0_state = EP0_IDLE;
    g_vendor_rx_len = 0;
    g_vendor_rx_ready = false;
    g_vendor_tx_busy = false;
    DCTL &= ~DCTL_SDIS;                          /* back on the bus: the host starts over with a bus reset */
}

void km_usbhs_poll(void)
{
    const uint32_t gi = GINTSTS;
    if (gi & GINT_USBRST) {
        on_reset();
    }
    if (gi & GINT_ENUMDNE) {
        on_enum_done();
    }
    if (gi & GINT_RXFLVL) {
        on_rxflvl();
    }
    if (gi & GINT_OEPINT) {
        on_out_endpoints();
    }
    if (gi & GINT_IEPINT) {
        on_in_endpoints();
    }
    if (gi & GINT_USBSUSP) {
        GINTSTS = GINT_USBSUSP;
        g_events.suspends++;
    }
    if (gi & GINT_WKUPINT) {
        GINTSTS = GINT_WKUPINT;
        g_events.resumes++;
    }
    if (gi & GINT_SOF) {
        GINTSTS = GINT_SOF;
        g_uframes_seen++;
        g_sof_total++;
        if (g_uframes_seen >= 8u) {          /* about once a millisecond: HID idle repeat */
            km_usb_in_tick(&g_dev, g_uframes_seen, in_start);
            g_uframes_seen = 0;
        }
    }
    vendor_task();
    if (g_clock != NULL && km_usb_guard_poll(&g_guard, g_clock())) {
        fall_back_to_plain();
    }
}

/* ---- M5 T-E: stuck IN endpoint ---- */
#define EPINT_INEPNE         (1u << 6)    /* IN endpoint NAK effective */

static void flush_tx_fifo(unsigned ep)
{
    GRSTCTL = (ep << 6) | GRSTCTL_TXFFLSH;
    for (unsigned t = 0; t < 20000u && (GRSTCTL & GRSTCTL_TXFFLSH); t++) {
    }
}

static void recover_in_endpoint(unsigned ep)
{
    if (DIEPCTL(ep) & EPCTL_EPENA) {             /* a transfer is still armed: NAK, then disable it */
        DIEPCTL(ep) |= EPCTL_SNAK;
        for (unsigned t = 0; t < 20000u && !(DIEPINT(ep) & EPINT_INEPNE); t++) {
        }
        DIEPCTL(ep) |= EPCTL_EPDIS | EPCTL_SNAK;
        for (unsigned t = 0; t < 20000u && !(DIEPINT(ep) & EPINT_EPDISD); t++) {
        }
    }
    DIEPINT(ep) = 0xFB7Fu;
    flush_tx_fifo(ep);
    km_usb_in_complete(&g_dev, ep);              /* the report is considered gone; the caller sends the current state again */
}

void km_usbhs_in_recover(void)
{
    recover_in_endpoint(KM_USB_EP_KBD_IN & 0xFu);
    recover_in_endpoint(KM_USB_EP_NKRO_IN & 0xFu);
}

void km_usbhs_pad_recover(void)
{
    if (KM_USB_VARIANT_HAS_IF3(g_dev.variant)) {
        recover_in_endpoint(PAD_EP);
    }
}

/* ---- bring-up (order and values as listed in USB_BRINGUP_NOTES.md section 2; the order is part of what works) ---- */
static void clocks_and_phy(void)
{
    /* GPIO and AFIO clocks, PB13 = OTG_HS_VBUS as input, AFIO remap bit */
    RCC_REG(0x3C) |= 7u;
    RCC_REG(0x20) |= 1u;
    KM_REG32(GPIOB_BASE + 0x00) &= ~(3u << 26);
    KM_REG32(GPIOB_BASE + 0x04) &= ~(3u << 26);
    KM_REG32(GPIOB_BASE + 0x20) &= ~(1u << 13);
    KM_REG32(GPIOB_BASE + 0x1C) = (KM_REG32(GPIOB_BASE + 0x1C) & ~(3u << 26)) | (1u << 26);
    KM_REG32(GPIOB_BASE + 0x18) &= ~(0x1Fu << 8);
    KM_REG32(GPIOB_BASE + 0x08) &= ~(1u << 13);
    AFIO_RMP_CFG = (AFIO_RMP_CFG & 0xFFFE3FFFu) | 0x8000u;

    /* USB clock divider from the PLL output, then the Nations-specific PHY bits (meaning of each bit unknown, USB_BRINGUP_NOTES.md) */
    RCC_REG(0x0C) = (RCC_REG(0x0C) & 0xFFF07FFFu) | 0xF0000u;
    RCC_REG(0x84) &= 0xFFFFF7FFu;
    RCC_REG(0x84) = (RCC_REG(0x84) & 0xFFFFF8FFu) | 0x300u;
    RCC_REG(0x84) = (RCC_REG(0x84) & 0xFF7FFFFFu) | 0x800000u;
    RCC_REG(0x24) |= 0x40u;                      /* USB_HS reset pulse */
    RCC_REG(0x24) &= ~0x40u;
    RCC_REG(0x18) |= 0x800u;                     /* USB_HS clock enable */
    RCC_REG(0x88) = (RCC_REG(0x88) & 0xFFFFFF0Fu) | 0x50u;
    RCC_REG(0x88) |= 0x3000u;
    RCC_REG(0x88) |= 0xC000u;
    RCC_REG(0x88) |= 0x30000u;
}

static void core_reset(void)
{
    unsigned t = 0;
    do {
        delay_us(3);
    } while (!(GRSTCTL & GRSTCTL_AHBIDL) && ++t < 200000u);
    GRSTCTL |= GRSTCTL_CSRST;
    for (t = 0; t < 200000u && !(GRSTCTL & GRSTCTL_CSRSTDONE); t++) {
    }
    GRSTCTL &= ~GRSTCTL_CSRST;
    delay_us(3);
}

void km_usbhs_init(void)
{
    km_usbhs_init_variant(KM_USB_VARIANT_PLAIN);
}

void km_usbhs_init_variant(km_usb_variant_t variant)
{
    /* The gamepad descriptor set is never presented without the means to leave it again. */
    if (variant != KM_USB_VARIANT_PLAIN && g_clock == NULL) {
        variant = KM_USB_VARIANT_PLAIN;
    }
    km_usb_guard_init(&g_guard, variant);
    g_dev.variant = variant;
    g_dev.uid[0] = KM_REG32(UID_BASE);
    g_dev.uid[1] = KM_REG32(UID_BASE + 4);
    g_dev.uid[2] = KM_REG32(UID_BASE + 8);
    km_usb_dev_reset(&g_dev);

    clocks_and_phy();

    /* core init, force device mode, device init, connect */
    GAHBCFG &= ~1u;
    GUSBCFG &= ~0x40u;
    GUSBCFG &= ~0x08u;
    core_reset();
    delay_ms(20);
    RCC_REG(0x88) |= 0x80000u;                   /* Nations-specific bit that goes with forcing device mode (USB_BRINGUP_NOTES.md section 2, step 4) */
    RCC_REG(0x84) |= 0x80000000u;
    GUSBCFG = (GUSBCFG & 0x9FFFFFFFu) | 0x40000000u;
    delay_ms(50);

    PCGCCTL = 0;                                 /* power and clock gating off */
    DCFG &= ~0x1800u;
    DCFG &= ~3u;                                 /* high speed */
    {   /* FIFO layout from the portable plan (km_usb_fifo_plan): RX 128 words, EP0 32 at 0x80, EP1 16 at 0xA0, EP2 32 at 0xB0,
         * EP3 32 at 0xD0 (ends at 0xF0 words, inside the 0x100 the bootloader uses), and in the PAD variant EP4 16 at 0xF0. */
        km_usb_fifo_t plan[KM_USB_FIFO_MAX];
        const unsigned n = km_usb_fifo_plan(g_dev.variant, plan);
        for (unsigned i = 0; i < n; i++) {
            const uint32_t v = ((uint32_t)plan[i].depth << 16) | plan[i].start;
            if (plan[i].ep == KM_USB_FIFO_RX) {
                GRXFSIZ = plan[i].depth;
            } else if (plan[i].ep == 0) {
                GNPTXFSIZ = v;
            } else {
                DIEPTXF(plan[i].ep) = v;
            }
        }
    }
    flush_tx_all();
    flush_rx();
    DIEPMSK = 0;
    DOEPMSK = 0;
    DAINT = 0xFFFFFFFFu;
    DAINTMSK = 0;
    for (unsigned n = 0; n < NUM_EP; n++) {
        DIEPCTL(n) = (DIEPCTL(n) & EPCTL_EPENA) ? (EPCTL_EPDIS | EPCTL_SNAK) : 0u;
        DIEPTSIZ(n) = 0;
        DIEPINT(n) = 0xFFu;
        DOEPCTL(n) = (DOEPCTL(n) & EPCTL_EPENA) ? (EPCTL_EPDIS | EPCTL_SNAK) : 0u;
        DOEPTSIZ(n) = 0;
        DOEPINT(n) = 0xFFu;
    }
    DIEPMSK |= 0x100u;
    GINTMSK = 0;
    GINTSTS = 0xBFFFFFFFu;
    GINTMSK = GINT_RXFLVL | GINT_USBRST | GINT_ENUMDNE | GINT_IEPINT | GINT_OEPINT | GINT_USBSUSP;
    GAHBCFG |= 1u;
    DCTL &= ~DCTL_SDIS;                          /* connect */
}
