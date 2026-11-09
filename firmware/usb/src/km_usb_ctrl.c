/*
 * EP0 control requests: what the device answers to the standard requests Windows (and Linux) send while
 * enumerating a composite HID device, plus the HID class requests. Policy, not hardware: see km_usb.h.
 */
#include "km/km_usb.h"

#include <string.h>

#define REQ_GET_STATUS        0x00u
#define REQ_CLEAR_FEATURE     0x01u
#define REQ_SET_FEATURE       0x03u
#define REQ_SET_ADDRESS       0x05u
#define REQ_GET_DESCRIPTOR    0x06u
#define REQ_GET_CONFIGURATION 0x08u
#define REQ_SET_CONFIGURATION 0x09u
#define REQ_GET_INTERFACE     0x0Au
#define REQ_SET_INTERFACE     0x0Bu

#define HID_GET_REPORT   0x01u
#define HID_GET_IDLE     0x02u
#define HID_GET_PROTOCOL 0x03u
#define HID_SET_REPORT   0x09u
#define HID_SET_IDLE     0x0Au
#define HID_SET_PROTOCOL 0x0Bu

#define DESC_DEVICE      0x01u
#define DESC_CONFIG      0x02u
#define DESC_STRING      0x03u
#define DESC_QUALIFIER   0x06u
#define DESC_HID         0x21u
#define DESC_HID_REPORT  0x22u

#define FEATURE_ENDPOINT_HALT   0u
#define FEATURE_DEVICE_REMOTE_WAKEUP 1u

void km_usb_setup_parse(const uint8_t raw[8], km_usb_setup_t *out)
{
    out->bm_request_type = raw[0];
    out->b_request = raw[1];
    out->w_value = (uint16_t)(raw[2] | (raw[3] << 8));
    out->w_index = (uint16_t)(raw[4] | (raw[5] << 8));
    out->w_length = (uint16_t)(raw[6] | (raw[7] << 8));
}

void km_usb_dev_reset(km_usb_dev_t *dev)
{
    dev->configuration = 0;
    for (unsigned i = 0; i < KM_USB_NUM_IF_MAX; i++) {
        dev->idle[i] = 0;
        dev->protocol[i] = 1;
    }
    dev->host_leds = 0;          /* a bus reset forgets the host's LED state: it sends it again after enumeration */
    dev->out_active = false;
    dev->out_bad = false;
    dev->out_len = 0;
    km_usb_in_reset(dev);
}

bool km_usb_ctrl_needs_zlp(size_t have, uint16_t want)
{
    return have < want && have != 0 && have % KM_USB_EP0_MPS == 0;
}

static void stall(km_ctrl_result_t *r)
{
    memset(r, 0, sizeof *r);
    r->kind = KM_CTRL_STALL;
}

static void ack(km_ctrl_result_t *r)
{
    memset(r, 0, sizeof *r);
    r->kind = KM_CTRL_ACK;
}

static void send(km_ctrl_result_t *r, const uint8_t *data, size_t have, uint16_t want)
{
    memset(r, 0, sizeof *r);
    r->kind = KM_CTRL_IN;
    r->data = data;
    r->len = (uint16_t)(have < want ? have : want);
    r->zlp = km_usb_ctrl_needs_zlp(have, want);
}

static void send_bytes(km_usb_dev_t *dev, km_ctrl_result_t *r, uint8_t b0, uint8_t b1, size_t n, uint16_t want)
{
    dev->scratch[0] = b0;
    dev->scratch[1] = b1;
    send(r, dev->scratch, n, want);
}

static void get_descriptor(km_usb_dev_t *dev, const km_usb_setup_t *s, km_ctrl_result_t *r)
{
    const unsigned type = s->w_value >> 8;
    const unsigned index = s->w_value & 0xFFu;
    size_t n = 0;
    const uint8_t *p = NULL;

    switch (type) {
    case DESC_DEVICE:
        p = km_usb_device_desc_for(dev->variant, &n);
        break;
    case DESC_CONFIG:
        if (index == 0) {
            p = km_usb_config_desc_for(dev->variant, &n);
        }
        break;
    case DESC_QUALIFIER:
        p = km_usb_qualifier_desc(&n);
        break;
    case DESC_STRING:
        n = km_usb_string_desc(index, dev->uid, dev->scratch);
        if (n != 0) {
            p = dev->scratch;
        }
        break;
    default:
        break;
    }
    if (p == NULL || n == 0) {
        stall(r);
        return;
    }
    send(r, p, n, s->w_length);
}

static bool endpoint_exists(const km_usb_dev_t *dev, unsigned addr)
{
    return addr == KM_USB_EP_KBD_IN || addr == KM_USB_EP_VND_IN || addr == KM_USB_EP_VND_OUT ||
           addr == KM_USB_EP_NKRO_IN || (addr == KM_USB_EP_PAD_IN && KM_USB_VARIANT_HAS_IF3(dev->variant)) ||
           (addr == KM_USB_EP_XI_OUT && dev->variant == KM_USB_VARIANT_XINPUT);
}

static const uint8_t k_zero_report[KM_USB_VND_REPORT_LEN];

#define REPORT_INPUT   1u
#define REPORT_OUTPUT  2u

/* Valid report IDs of an interface (idle and report requests): IF0, IF1 and IF3 have no IDs, IF2 has 1 and 2. */
static bool report_id_ok(unsigned iface, unsigned id, bool allow_all)
{
    if (iface != KM_USB_IF_NKRO) {
        return id == 0;
    }
    return (allow_all && id == 0) || id == KM_USB_NKRO_ID_KEYS || id == KM_USB_NKRO_ID_CONSUMER;
}

static void hid_get_report(const km_usb_dev_t *dev, const km_usb_setup_t *s, unsigned iface, km_ctrl_result_t *r)
{
    const unsigned type = s->w_value >> 8;
    const unsigned id = s->w_value & 0xFFu;
    const uint8_t *p = NULL;
    size_t n = 0;
    if (type != REPORT_INPUT) {
        return;                              /* output and feature reports cannot be read back */
    }
    if (iface == KM_USB_IF_KBD && id == 0) {
        p = dev->last_kbd;
        n = sizeof dev->last_kbd;
    } else if (iface == KM_USB_IF_VND && id == 0) {
        p = k_zero_report;                   /* the channel is request/reply, there is no state to report */
        n = sizeof k_zero_report;
    } else if (iface == KM_USB_IF_NKRO && id == KM_USB_NKRO_ID_KEYS) {
        p = dev->last_nkro;
        n = sizeof dev->last_nkro;
    } else if (iface == KM_USB_IF_NKRO && id == KM_USB_NKRO_ID_CONSUMER) {
        p = dev->last_consumer;
        n = sizeof dev->last_consumer;
    } else if (iface == KM_USB_IF_PAD && id == 0) {
        p = dev->last_pad;
        n = sizeof dev->last_pad;
    }
    if (p != NULL) {
        send(r, p, n, s->w_length);
    }
}

static void hid_class_request(km_usb_dev_t *dev, const km_usb_setup_t *s, unsigned iface, km_ctrl_result_t *r)
{
    const bool dev_to_host = (s->bm_request_type & 0x80u) != 0;
    const bool keyboard = iface == KM_USB_IF_KBD || iface == KM_USB_IF_NKRO;   /* the interfaces that carry key reports: idle applies */
    switch (s->b_request) {
    case HID_GET_REPORT:
        if (dev_to_host && s->w_length != 0) {
            hid_get_report(dev, s, iface, r);
        }
        break;
    case HID_GET_IDLE:
        if (dev_to_host && keyboard && s->w_length == 1 && report_id_ok(iface, s->w_value & 0xFFu, true)) {
            send_bytes(dev, r, dev->idle[iface], 0, 1, s->w_length);
        }
        break;
    case HID_GET_PROTOCOL:
        if (dev_to_host && iface == KM_USB_IF_KBD && s->w_length == 1) {   /* only the boot interface has a protocol */
            send_bytes(dev, r, dev->protocol[iface], 0, 1, s->w_length);
        }
        break;
    case HID_SET_IDLE:
        if (!dev_to_host && keyboard && s->w_length == 0 && report_id_ok(iface, s->w_value & 0xFFu, true)) {
            dev->idle[iface] = (uint8_t)(s->w_value >> 8);
            dev->idle_age[iface] = 0;   /* the new rate counts from now */
            ack(r);
        }
        break;
    case HID_SET_PROTOCOL:
        if (!dev_to_host && iface == KM_USB_IF_KBD && s->w_length == 0 && s->w_value <= 1u) {
            dev->protocol[iface] = (uint8_t)s->w_value;
            ack(r);
        }
        break;
    case HID_SET_REPORT: {   /* the LED output report: the data stage is taken by km_usb_ctrl_out_push() / _end() */
        const unsigned max_len = iface == KM_USB_IF_KBD ? KM_USB_BOOT_LED_LEN
                               : iface == KM_USB_IF_VND ? KM_USB_VND_REPORT_LEN
                               : iface == KM_USB_IF_NKRO ? KM_USB_NKRO_LED_LEN : 0u;   /* the gamepad has no output report */
        const unsigned id = s->w_value & 0xFFu;
        const bool id_ok = iface == KM_USB_IF_NKRO ? id == KM_USB_NKRO_ID_KEYS : id == 0;
        if (!dev_to_host && (s->w_value >> 8) == REPORT_OUTPUT && id_ok && max_len != 0 && s->w_length != 0 && s->w_length <= max_len) {
            memset(r, 0, sizeof *r);
            r->kind = KM_CTRL_OUT;
            r->len = s->w_length;
            dev->out_setup = *s;
            dev->out_len = 0;
            dev->out_bad = false;
            dev->out_active = true;
        }
        break;
    }
    default:       /* everything else: STALL */
        break;
    }
}

void km_usb_ctrl_handle(km_usb_dev_t *dev, const km_usb_setup_t *s, km_ctrl_result_t *r)
{
    const unsigned type = (s->bm_request_type >> 5) & 3u;
    const unsigned recipient = s->bm_request_type & 0x1Fu;
    const bool dev_to_host = (s->bm_request_type & 0x80u) != 0;
    const unsigned iface = s->w_index & 0xFFu;

    dev->out_active = false;   /* a new SETUP packet abandons a data stage that was still running */
    stall(r);

    if (type == 0) {   /* standard */
        switch (recipient) {
        case 0:        /* device */
            if (dev_to_host) {
                if (s->b_request == REQ_GET_STATUS && s->w_value == 0 && s->w_index == 0) {
                    send_bytes(dev, r, 0, 0, 2, s->w_length);   /* bus powered, no remote wakeup enabled */
                } else if (s->b_request == REQ_GET_DESCRIPTOR) {
                    get_descriptor(dev, s, r);
                } else if (s->b_request == REQ_GET_CONFIGURATION && s->w_value == 0 && s->w_index == 0 &&
                           s->w_length == 1) {
                    send_bytes(dev, r, dev->configuration, 0, 1, s->w_length);
                }
            } else {
                if ((s->b_request == REQ_CLEAR_FEATURE || s->b_request == REQ_SET_FEATURE) &&
                    s->w_value == FEATURE_DEVICE_REMOTE_WAKEUP && s->w_index == 0 && s->w_length == 0) {
                    ack(r);
                } else if (s->b_request == REQ_SET_ADDRESS && s->w_value <= 127u && s->w_index == 0 &&
                           s->w_length == 0) {
                    ack(r);
                    r->apply = KM_CTRL_APPLY_ADDRESS;
                    r->value = (uint8_t)s->w_value;
                } else if (s->b_request == REQ_SET_CONFIGURATION && s->w_value <= 1u && s->w_index == 0 &&
                           s->w_length == 0 &&
                           !(s->w_value == 1u && dev->test_refuse_configuration && KM_USB_VARIANT_HAS_IF3(dev->variant))) {
                    ack(r);
                    r->apply = KM_CTRL_APPLY_CONFIGURATION;
                    r->value = (uint8_t)s->w_value;
                    dev->configuration = (uint8_t)s->w_value;
                    km_usb_in_reset(dev);   /* endpoints start over: nothing in flight, nothing remembered */
                }
            }
            break;
        case 1:        /* interface */
            if (iface >= km_usb_variant_num_if(dev->variant)) {
                break;
            }
            if (dev_to_host) {
                if (s->b_request == REQ_GET_STATUS && s->w_value == 0) {
                    send_bytes(dev, r, 0, 0, 2, s->w_length);
                } else if (s->b_request == REQ_GET_INTERFACE && s->w_value == 0 && dev->configuration != 0) {
                    send_bytes(dev, r, 0, 0, 1, s->w_length);   /* only alternate setting 0 exists */
                } else if (s->b_request == REQ_GET_DESCRIPTOR) {
                    size_t n = 0;
                    const uint8_t *p = NULL;
                    if ((s->w_value >> 8) == DESC_HID_REPORT && (s->w_value & 0xFFu) == 0) {
                        p = km_usb_report_desc_for(dev->variant, iface, &n);
                    } else if ((s->w_value >> 8) == DESC_HID && (s->w_value & 0xFFu) == 0) {
                        p = km_usb_hid_desc_for(dev->variant, iface, &n);
                    }
                    if (p != NULL) {
                        send(r, p, n, s->w_length);
                    }
                }
            } else if (s->b_request == REQ_SET_INTERFACE && s->w_value == 0 && s->w_length == 0 &&
                       dev->configuration != 0) {
                ack(r);
            }
            break;
        case 2:        /* endpoint */
            if (!endpoint_exists(dev, s->w_index & 0xFFu)) {
                break;
            }
            if (dev_to_host && s->b_request == REQ_GET_STATUS && s->w_value == 0) {
                send_bytes(dev, r, 0, 0, 2, s->w_length);       /* never halted */
            } else if (!dev_to_host && (s->b_request == REQ_CLEAR_FEATURE) && s->w_value == FEATURE_ENDPOINT_HALT &&
                       s->w_length == 0) {
                ack(r);                                         /* nothing is ever halted, so clearing is a no-op */
            }
            break;
        default:
            break;
        }
        return;
    }

    if (type == 1 && recipient == 1 && iface < km_usb_variant_num_if(dev->variant) &&
        !(dev->variant == KM_USB_VARIANT_XINPUT && iface == KM_USB_IF_PAD)) {   /* HID class requests to an interface (IF3 of XINPUT is no HID: STALL) */
        hid_class_request(dev, s, iface, r);
    }
}

/* ---- EP0 OUT data stage ---- */
void km_usb_ctrl_out_push(km_usb_dev_t *dev, const uint8_t *data, unsigned len)
{
    if (!dev->out_active || dev->out_bad || len == 0) {
        return;
    }
    const unsigned room = (unsigned)(dev->out_setup.w_length - dev->out_len);   /* out_len <= wLength <= buffer size */
    if (data == NULL || len > room) {
        dev->out_bad = true;       /* more than the request announced: the whole report is distrusted */
        return;
    }
    memcpy(dev->out_buf + dev->out_len, data, len);
    dev->out_len = (uint16_t)(dev->out_len + len);
}

/* The LED output report of IF0 is 1 byte (LED bits); that of IF2 is 2 bytes (report ID 1, LED bits). Anything else that
 * km_usb_ctrl_handle() let through (the vendor channel's output report) is taken and not interpreted. */
static bool apply_led_report(km_usb_dev_t *dev)
{
    const unsigned iface = dev->out_setup.w_index & 0xFFu;
    uint8_t leds;
    if (iface == KM_USB_IF_KBD && dev->out_len == KM_USB_BOOT_LED_LEN) {
        leds = dev->out_buf[0];
    } else if (iface == KM_USB_IF_NKRO && dev->out_len == KM_USB_NKRO_LED_LEN && dev->out_buf[0] == KM_USB_NKRO_ID_KEYS) {
        leds = dev->out_buf[1];
    } else {
        return false;
    }
    dev->host_leds = (uint8_t)(leds & KM_USB_LED_MASK);
    dev->led_reports++;
    return true;
}

bool km_usb_ctrl_out_end(km_usb_dev_t *dev)
{
    const bool complete = dev->out_active && !dev->out_bad && dev->out_len == dev->out_setup.w_length;
    const bool applied = complete && apply_led_report(dev);
    dev->out_active = false;
    dev->out_bad = false;
    dev->out_len = 0;
    return applied;
}

/* ---- fall-back from the PAD variant to PLAIN ---- */
void km_usb_guard_init(km_usb_guard_t *g, km_usb_variant_t requested)
{
    memset(g, 0, sizeof *g);
    g->requested = requested;
    g->active = requested;
}

void km_usb_guard_on_reset(km_usb_guard_t *g, uint32_t now_ms)
{
    /* The first reset starts the clock; later ones (a host that retries) do not restart it. */
    if (g->requested != KM_USB_VARIANT_PLAIN && !g->running && !g->accepted && !g->fell_back) {
        g->running = true;
        g->started_ms = now_ms;
    }
}

void km_usb_guard_on_configured(km_usb_guard_t *g)
{
    if (!g->fell_back) {
        g->accepted = true;
        g->running = false;
    }
}

bool km_usb_guard_poll(km_usb_guard_t *g, uint32_t now_ms)
{
    if (!g->running || (uint32_t)(now_ms - g->started_ms) < KM_USB_PAD_FALLBACK_MS) {
        return false;
    }
    g->running = false;
    g->fell_back = true;
    g->active = KM_USB_VARIANT_PLAIN;
    return true;
}

/* ---- the USB core's capabilities and the FIFO plan ---- */
unsigned km_usb_fifo_plan(km_usb_variant_t v, km_usb_fifo_t out[KM_USB_FIFO_MAX])
{
    /* receive 128 words, EP0 32, EP1 16, EP2 32, EP3 32 (the layout the keyboard has always used), EP4 16 = 0x100 words.
     * 16 words is the smallest transmit FIFO a DWC2 core accepts, and EP4's 16-byte packets need only 4. */
    static const struct { uint8_t ep; uint16_t depth; } k_plan[] = {
        { KM_USB_FIFO_RX, 0x80 }, { 0, 0x20 }, { 1, 0x10 }, { 2, 0x20 }, { 3, 0x20 }, { 4, 0x10 },
    };
    const unsigned n = KM_USB_VARIANT_HAS_IF3(v) ? 6u : 5u;
    uint16_t at = 0;
    for (unsigned i = 0; i < n; i++) {
        out[i].ep = k_plan[i].ep;
        out[i].start = at;
        out[i].depth = k_plan[i].depth;
        at = (uint16_t)(at + k_plan[i].depth);
    }
    return n;
}

void km_usb_hwcfg_decode(const uint32_t raw[4], km_usb_hwcfg_t *out)
{
    memset(out, 0, sizeof *out);
    memcpy(out->raw, raw, sizeof out->raw);
    for (unsigned n = 0; n < 16; n++) {
        out->ep_dir[n] = (uint8_t)((raw[0] >> (2u * n)) & 3u);
    }
    out->num_dev_eps = (uint8_t)((raw[1] >> 10) & 0xFu);
    out->num_in_eps = (uint8_t)(((raw[3] >> 26) & 0xFu) + 1u);
    out->ded_fifo = ((raw[3] >> 25) & 1u) != 0;
    out->fifo_depth = (uint16_t)(raw[2] >> 16);
}

uint32_t km_usb_hwcfg_check(const km_usb_hwcfg_t *hw, km_usb_variant_t v)
{
    uint32_t bad = 0;
    static const uint8_t in_eps[] = { KM_USB_EP_KBD_IN & 0xFu, KM_USB_EP_VND_IN & 0xFu, KM_USB_EP_NKRO_IN & 0xFu,
                                      KM_USB_EP_PAD_IN & 0xFu };
    const unsigned n_in = KM_USB_VARIANT_HAS_IF3(v) ? 4u : 3u;
    unsigned highest = KM_USB_EP_VND_OUT & 0xFu;
    if (!hw->ded_fifo) {
        bad |= KM_USB_HW_NO_DED_FIFO;
    }
    for (unsigned i = 0; i < n_in; i++) {
        if (in_eps[i] >= hw->num_in_eps) {
            bad |= KM_USB_HW_TOO_FEW_IN;
        }
        if (hw->ep_dir[in_eps[i]] != 0u && hw->ep_dir[in_eps[i]] != 1u) {
            bad |= KM_USB_HW_IN_DIR;
        }
        highest = in_eps[i] > highest ? in_eps[i] : highest;
    }
    if (hw->ep_dir[KM_USB_EP_VND_OUT & 0xFu] != 0u && hw->ep_dir[KM_USB_EP_VND_OUT & 0xFu] != 2u) {
        bad |= KM_USB_HW_OUT_DIR;
    }
    if (v == KM_USB_VARIANT_XINPUT && hw->ep_dir[KM_USB_EP_XI_OUT & 0xFu] != 0u && hw->ep_dir[KM_USB_EP_XI_OUT & 0xFu] != 2u) {
        bad |= KM_USB_HW_OUT_DIR;                        /* EP4 must be able to do both directions for the rumble endpoint */
    }
    if (hw->num_dev_eps < highest) {
        bad |= KM_USB_HW_TOO_FEW_EPS;
    }
    km_usb_fifo_t plan[KM_USB_FIFO_MAX];
    const unsigned n = km_usb_fifo_plan(v, plan);
    if (hw->fifo_depth < plan[n - 1].start + plan[n - 1].depth) {
        bad |= KM_USB_HW_FIFO_SPACE;
    }
    return bad;
}
