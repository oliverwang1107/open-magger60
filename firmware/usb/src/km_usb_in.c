/*
 * Bookkeeping of the report IN endpoints (EP1 boot keyboard, EP3 NKRO + consumer, EP4 gamepad in the PAD variant): "is the
 * previous report still on its way", "what was the last report" (GET_REPORT and the HID idle repeat), and the byte layout of the
 * gamepad report. Policy only; the driver queues the bytes.
 */
#include <string.h>

#include "km/km_usb.h"

#define EP_KBD   (KM_USB_EP_KBD_IN & 0xFu)
#define EP_NKRO  (KM_USB_EP_NKRO_IN & 0xFu)
#define EP_PAD   (KM_USB_EP_PAD_IN & 0xFu)
#define UFRAMES_PER_IDLE_UNIT 32u   /* the idle rate counts 4 ms = 32 micro-frames */

void km_usb_in_reset(km_usb_dev_t *dev)
{
    memset(dev->last_kbd, 0, sizeof dev->last_kbd);
    memset(dev->last_nkro, 0, sizeof dev->last_nkro);
    memset(dev->last_consumer, 0, sizeof dev->last_consumer);
    dev->last_nkro[0] = KM_USB_NKRO_ID_KEYS;
    dev->last_consumer[0] = KM_USB_NKRO_ID_CONSUMER;
    memset(dev->last_pad, 0, sizeof dev->last_pad);
    memset(dev->last_xi, 0, sizeof dev->last_xi);
    dev->last_xi[1] = KM_USB_XI_LEN_BYTE;
    dev->xi_rumble_large = dev->xi_rumble_small = dev->xi_led = 0;
    dev->busy_kbd = false;
    dev->busy_nkro = false;
    dev->busy_pad = false;
    dev->sent_kbd = false;
    dev->sent_nkro = false;
    for (unsigned i = 0; i < KM_USB_NUM_IF_MAX; i++) {
        dev->idle_age[i] = 0;
    }
}

bool km_usb_in_kbd_ready(const km_usb_dev_t *dev)
{
    return dev->configuration != 0 && !dev->busy_kbd;
}

bool km_usb_in_nkro_ready(const km_usb_dev_t *dev)
{
    return dev->configuration != 0 && !dev->busy_nkro;
}

bool km_usb_in_send_kbd(km_usb_dev_t *dev, const uint8_t report[KM_USB_BOOT_REPORT_LEN], km_usb_in_start_fn start)
{
    if (!km_usb_in_kbd_ready(dev)) {
        return false;
    }
    memcpy(dev->last_kbd, report, KM_USB_BOOT_REPORT_LEN);
    dev->busy_kbd = true;
    dev->sent_kbd = true;
    dev->idle_age[KM_USB_IF_KBD] = 0;
    start(EP_KBD, dev->last_kbd, KM_USB_BOOT_REPORT_LEN);
    return true;
}

bool km_usb_in_send_nkro(km_usb_dev_t *dev, const uint8_t *report, unsigned len, km_usb_in_start_fn start)
{
    if (report == NULL || !km_usb_in_nkro_ready(dev)) {
        return false;
    }
    if (len == KM_USB_NKRO_REPORT_LEN && report[0] == KM_USB_NKRO_ID_KEYS) {
        memcpy(dev->last_nkro, report, len);
        dev->sent_nkro = true;
        dev->idle_age[KM_USB_IF_NKRO] = 0;
        dev->busy_nkro = true;
        start(EP_NKRO, dev->last_nkro, len);
        return true;
    }
    if (len == KM_USB_CONSUMER_REPORT_LEN && report[0] == KM_USB_NKRO_ID_CONSUMER) {
        memcpy(dev->last_consumer, report, len);
        dev->busy_nkro = true;
        start(EP_NKRO, dev->last_consumer, len);
        return true;
    }
    return false;
}

bool km_usb_in_pad_ready(const km_usb_dev_t *dev)
{
    return dev->variant == KM_USB_VARIANT_PAD && dev->configuration != 0 && !dev->busy_pad;
}

bool km_usb_in_send_pad(km_usb_dev_t *dev, const uint8_t *report, unsigned len, km_usb_in_start_fn start)
{
    if (report == NULL || len != KM_USB_PAD_REPORT_LEN || !km_usb_in_pad_ready(dev)) {
        return false;
    }
    memcpy(dev->last_pad, report, KM_USB_PAD_REPORT_LEN);
    dev->busy_pad = true;
    start(EP_PAD, dev->last_pad, KM_USB_PAD_REPORT_LEN);
    return true;
}

bool km_usb_in_xinput_ready(const km_usb_dev_t *dev)
{
    return dev->variant == KM_USB_VARIANT_XINPUT && dev->configuration != 0 && !dev->busy_pad;
}

bool km_usb_in_send_xinput(km_usb_dev_t *dev, const uint8_t *report, unsigned len, km_usb_in_start_fn start)
{
    if (report == NULL || len != KM_USB_XI_REPORT_LEN || report[0] != KM_USB_XI_TYPE_INPUT || report[1] != KM_USB_XI_LEN_BYTE ||
        !km_usb_in_xinput_ready(dev)) {
        return false;
    }
    memcpy(dev->last_xi, report, KM_USB_XI_REPORT_LEN);
    dev->busy_pad = true;
    start(EP_PAD, dev->last_xi, KM_USB_XI_REPORT_LEN);
    return true;
}

void km_usb_in_complete(km_usb_dev_t *dev, unsigned ep)
{
    if (ep == EP_KBD) {
        dev->busy_kbd = false;
    } else if (ep == EP_NKRO) {
        dev->busy_nkro = false;
    } else if (ep == EP_PAD) {
        dev->busy_pad = false;
    }
}

/* ---- the gamepad report: the only code that knows the byte layout ---- */
static void put16(uint8_t *o, uint16_t v)
{
    o[0] = (uint8_t)v;
    o[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *i)
{
    return (uint16_t)(i[0] | (i[1] << 8));
}

static bool pad_state_valid(const km_usb_pad_state_t *s)
{
    const int16_t lo = -KM_USB_PAD_STICK_MAX;
    return s->hat <= KM_USB_PAD_HAT_MAX && s->lx >= lo && s->ly >= lo && s->rx >= lo && s->ry >= lo &&
           s->lt <= KM_USB_PAD_STICK_MAX && s->rt <= KM_USB_PAD_STICK_MAX;
}

bool km_usb_pad_pack(const km_usb_pad_state_t *s, uint8_t out[KM_USB_PAD_REPORT_LEN])
{
    if (!pad_state_valid(s)) {
        return false;
    }
    put16(out + 0, s->buttons);
    out[2] = s->hat;                                   /* upper 4 bits are the padding of the descriptor: 0 */
    put16(out + 3, (uint16_t)s->lx);
    put16(out + 5, (uint16_t)s->ly);
    put16(out + 7, (uint16_t)s->rx);
    put16(out + 9, (uint16_t)s->ry);
    put16(out + 11, s->lt);
    put16(out + 13, s->rt);
    return true;
}

bool km_usb_pad_unpack(const uint8_t in[KM_USB_PAD_REPORT_LEN], km_usb_pad_state_t *s)
{
    km_usb_pad_state_t t;
    t.buttons = get16(in + 0);
    t.hat = in[2];
    t.lx = (int16_t)get16(in + 3);
    t.ly = (int16_t)get16(in + 5);
    t.rx = (int16_t)get16(in + 7);
    t.ry = (int16_t)get16(in + 9);
    t.lt = get16(in + 11);
    t.rt = get16(in + 13);
    if (!pad_state_valid(&t)) {                        /* includes hat > 8: the padding nibble must be 0 */
        return false;
    }
    *s = t;
    return true;
}

/* Triangle wave of `period` ms: +32767 at 0, -32767 at period/2, back to +32767. */
static int16_t triangle(uint32_t t_ms, uint32_t period)
{
    const uint32_t ph = t_ms % period;
    const uint32_t half = period / 2u;
    const int32_t max = KM_USB_PAD_STICK_MAX;
    if (ph < half) {
        return (int16_t)(max - (int32_t)(ph * 2u * (uint32_t)max / half));
    }
    return (int16_t)(-max + (int32_t)((ph - half) * 2u * (uint32_t)max / (period - half)));
}

void km_usb_pad_sweep(uint32_t t_ms, km_usb_pad_state_t *s)
{
    const unsigned step = (t_ms / 1000u) % 17u;       /* 0 = no button, then buttons 1..16 one second each */
    s->buttons = step == 0 ? 0u : (uint16_t)(1u << (step - 1u));
    s->hat = (uint8_t)((t_ms / 1000u) % 9u);
    s->lx = triangle(t_ms, KM_USB_PAD_SWEEP_LX_MS);
    s->ly = triangle(t_ms, KM_USB_PAD_SWEEP_LY_MS);
    s->rx = triangle(t_ms, KM_USB_PAD_SWEEP_RX_MS);
    s->ry = triangle(t_ms, KM_USB_PAD_SWEEP_RY_MS);
    s->lt = (uint16_t)((triangle(t_ms, KM_USB_PAD_SWEEP_LT_MS) + KM_USB_PAD_STICK_MAX) / 2);
    s->rt = (uint16_t)((triangle(t_ms, KM_USB_PAD_SWEEP_RT_MS) + KM_USB_PAD_STICK_MAX) / 2);
}

static void age(uint32_t *a, uint32_t uframes)
{
    *a = (*a > 0xFFFFFFFFu - uframes) ? 0xFFFFFFFFu : *a + uframes;
}

void km_usb_in_tick(km_usb_dev_t *dev, uint32_t uframes, km_usb_in_start_fn start)
{
    if (dev->configuration == 0) {
        return;
    }
    if (dev->sent_kbd) {
        age(&dev->idle_age[KM_USB_IF_KBD], uframes);
        if (dev->idle[KM_USB_IF_KBD] != 0 && !dev->busy_kbd &&
            dev->idle_age[KM_USB_IF_KBD] >= dev->idle[KM_USB_IF_KBD] * UFRAMES_PER_IDLE_UNIT) {
            dev->idle_age[KM_USB_IF_KBD] = 0;
            dev->busy_kbd = true;
            start(EP_KBD, dev->last_kbd, KM_USB_BOOT_REPORT_LEN);
        }
    }
    if (dev->sent_nkro) {   /* only the keyboard report (ID 1) repeats; a media key is an event, not a state */
        age(&dev->idle_age[KM_USB_IF_NKRO], uframes);
        if (dev->idle[KM_USB_IF_NKRO] != 0 && !dev->busy_nkro &&
            dev->idle_age[KM_USB_IF_NKRO] >= dev->idle[KM_USB_IF_NKRO] * UFRAMES_PER_IDLE_UNIT) {
            dev->idle_age[KM_USB_IF_NKRO] = 0;
            dev->busy_nkro = true;
            start(EP_NKRO, dev->last_nkro, KM_USB_NKRO_REPORT_LEN);
        }
    }
}

/* ---- the XInput-style report: the only code that knows its byte layout ---- */
bool km_usb_xinput_pack(const km_usb_pad_state_t *s, uint8_t out[KM_USB_XI_REPORT_LEN])
{
    if (!pad_state_valid(s)) {
        return false;
    }
    uint8_t b2 = 0, b3 = 0;
    switch (s->hat) {                                  /* 0 centred, 1 up, 2 up-right, 3 right ... 8 up-left */
    case 1: b2 = KM_USB_XI_DPAD_UP; break;
    case 2: b2 = KM_USB_XI_DPAD_UP | KM_USB_XI_DPAD_RIGHT; break;
    case 3: b2 = KM_USB_XI_DPAD_RIGHT; break;
    case 4: b2 = KM_USB_XI_DPAD_DOWN | KM_USB_XI_DPAD_RIGHT; break;
    case 5: b2 = KM_USB_XI_DPAD_DOWN; break;
    case 6: b2 = KM_USB_XI_DPAD_DOWN | KM_USB_XI_DPAD_LEFT; break;
    case 7: b2 = KM_USB_XI_DPAD_LEFT; break;
    case 8: b2 = KM_USB_XI_DPAD_UP | KM_USB_XI_DPAD_LEFT; break;
    default: break;
    }
    static const struct { uint8_t byte; uint8_t bit; } k_map[11] = {   /* HID button 1..11 -> XInput bit (byte 2 or 3) */
        { 3, KM_USB_XI_A }, { 3, KM_USB_XI_B }, { 3, KM_USB_XI_X }, { 3, KM_USB_XI_Y },
        { 3, KM_USB_XI_LB }, { 3, KM_USB_XI_RB }, { 2, KM_USB_XI_BACK }, { 2, KM_USB_XI_START },
        { 2, KM_USB_XI_L3 }, { 2, KM_USB_XI_R3 }, { 3, KM_USB_XI_GUIDE },
    };
    for (unsigned i = 0; i < 11u; i++) {
        if (s->buttons & (1u << i)) {
            if (k_map[i].byte == 2) { b2 |= k_map[i].bit; } else { b3 |= k_map[i].bit; }
        }
    }
    memset(out, 0, KM_USB_XI_REPORT_LEN);
    out[0] = KM_USB_XI_TYPE_INPUT;
    out[1] = KM_USB_XI_LEN_BYTE;
    out[2] = b2;
    out[3] = b3;
    out[4] = (uint8_t)(((uint32_t)s->lt * 255u + KM_USB_PAD_STICK_MAX / 2u) / KM_USB_PAD_STICK_MAX);
    out[5] = (uint8_t)(((uint32_t)s->rt * 255u + KM_USB_PAD_STICK_MAX / 2u) / KM_USB_PAD_STICK_MAX);
    put16(out + 6, (uint16_t)s->lx);
    put16(out + 8, (uint16_t)(-s->ly));                /* HID: negative = up; XInput: positive = up */
    put16(out + 10, (uint16_t)s->rx);
    put16(out + 12, (uint16_t)(-s->ry));
    return true;
}

bool km_usb_xinput_from_pad(const uint8_t pad[KM_USB_PAD_REPORT_LEN], uint8_t out[KM_USB_XI_REPORT_LEN])
{
    km_usb_pad_state_t s;
    return km_usb_pad_unpack(pad, &s) && km_usb_xinput_pack(&s, out);
}

bool km_usb_xinput_out(km_usb_dev_t *dev, const uint8_t *data, unsigned len)
{
    if (data == NULL || dev->variant != KM_USB_VARIANT_XINPUT) {
        return false;
    }
    if (len == KM_USB_XI_OUT_MAX && data[0] == 0x00u && data[1] == 0x08u) {          /* rumble */
        dev->xi_rumble_large = data[3];
        dev->xi_rumble_small = data[4];
        dev->xi_out_messages++;
        return true;
    }
    if (len == 3u && data[0] == 0x01u && data[1] == 0x03u) {                         /* LED pattern */
        dev->xi_led = data[2];
        dev->xi_out_messages++;
        return true;
    }
    return false;
}
