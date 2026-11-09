/*
 * usb_diag image (M6 P5b, hardware ladder steps L6-5a / L6-5b / L6-5c): the USB interface work of the gamepad, tested on its
 * own, with no scanning, no key reports and none of the engine. One source, three images chosen by KM_USBDIAG_MODE:
 *
 *   0  "u5a"  PLAIN descriptor (exactly the keyboard's 3 interfaces, bcdDevice 0x0001) and the capability readout.
 *             Reads GHWCFG1..4 of the USB core and says whether the 4-interface PAD descriptor fits (how many IN endpoints,
 *             dedicated FIFOs, FIFO space for EP4). Changes nothing on the bus: the safe first step.
 *   1  "u5b"  PAD descriptor (4 interfaces, bcdDevice 0x0002). Sends a synthetic gamepad sweep on EP4 (every axis a
 *             triangle wave with its own period, one button and one hat direction per second): tools/magger60_padtest.py
 *             shows it. The 6-second fall-back to the PLAIN descriptor is armed, as in the real image.
 *   2  "u5c"  The fall-back drill: the PAD descriptor, but the device REFUSES SET_CONFIGURATION while it presents it, so
 *             the host never configures. After 6 s of that, the firmware must drop off the bus and come back as PLAIN
 *             (bcdDevice 0x0001, typing works). The scope value "fell back" shows it happened.
 *
 * It keeps the stock behaviours of K1: vendor command 3 (version + build string, label KMFW), the scope channel 0xC5 and
 * the "enter the bootloader from the host" command, so the keyboard can always be put back with the host tools.
 *
 * Scope snapshot, tag 0x0510, 18 values (all u16; the tool tools/magger60_padtest.py --hwcfg decodes them):
 *   [0..1] GHWCFG1 low/high   [2..3] GHWCFG2   [4..5] GHWCFG3   [6..7] GHWCFG4
 *   [8] verdict: bit mask of km_usb_hwcfg_check() for the PAD variant (0 = fits)      [9] active variant (0 plain, 1 pad)
 *   [10] configured   [11] fell back   [12] pad reports sent (low 16 bits)   [13] pad endpoint recoveries
 *   [14] host LEDs   [15] LED reports accepted   [16] seconds since power-up   [17] fall-back clock running (1/0)
 *
 * No key report is ever sent; the only flash write is the boot flag of "enter the bootloader".
 */
#include <stdbool.h>

#include "flash_boot.h"
#include "km/km_scope.h"
#include "km/km_usb.h"
#include "km/km_vendor.h"
#include "n32h48x_regs.h"
#include "usb_hs.h"

#ifndef KM_USBDIAG_MODE
#define KM_USBDIAG_MODE 0
#endif

#define CPU_HZ        240000000u     /* what the bootloader sets up */
#define DWT_CTRL      KM_REG32(0xE0001000u)
#define DWT_CYCCNT    KM_REG32(0xE0001004u)
#define DEMCR         KM_REG32(0xE000EDFCu)
#define SCOPE_TAG     0x0510u
#define PAD_RECOVER_AFTER_MS 500u    /* an EP4 report the host has not taken for this long: flush the endpoint and go on */

static const km_vendor_info_t k_info = { 0x0105, __DATE__ "," __TIME__ "KMFW-U5" };
static volatile bool g_enter_boot;

static bool vendor(const uint8_t in[64], uint8_t out[64])
{
    if (km_vendor_is_enter_boot_request(in)) {
        g_enter_boot = true;               /* acted on in the main loop, after the USB transfer is over */
        return false;
    }
    return km_vendor_handle(&k_info, in, out);
}

/* Milliseconds since power-up from the 32-bit cycle counter (wraps every 17.9 s, so it must be called more often). */
static uint32_t clock_ms(void)
{
    static uint32_t last, rest, ms;
    const uint32_t now = DWT_CYCCNT;
    rest += now - last;
    last = now;
    ms += rest / (CPU_HZ / 1000u);
    rest %= (CPU_HZ / 1000u);
    return ms;
}

int main(void)
{
    DEMCR |= 1u << 24;                     /* TRCENA */
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1u;

    km_usbhs_set_vendor_handler(vendor);
    km_usbhs_set_clock(clock_ms);
    km_usbhs_init_variant(KM_USBDIAG_MODE == 0 ? KM_USB_VARIANT_PLAIN : KM_USB_VARIANT_PAD);
#if KM_USBDIAG_MODE == 2
    km_usbhs_rehearsal_refuse_configuration(true);
#endif
    km_scope_reset();

    uint32_t hw[4];
    km_usbhs_read_hwcfg(hw);
    km_usb_hwcfg_t cfg;
    km_usb_hwcfg_decode(hw, &cfg);
    const uint16_t verdict = (uint16_t)km_usb_hwcfg_check(&cfg, KM_USB_VARIANT_PAD);

    uint32_t passes = 0, configured_at = 0, last_pad_ms = 0xFFFFFFFFu, busy_since = 0;
    bool was_configured = false;
    uint16_t pad_sent = 0, recoveries = 0;
    for (;;) {
        km_iwdg_feed();
        km_usbhs_poll();
        const uint32_t now = clock_ms();
        if (g_enter_boot) {
            km_usbhs_disconnect();
            km_flash_request_bootloader();
        }

        const bool configured = km_usbhs_configured();
        if (configured && !was_configured) {
            configured_at = now;
            last_pad_ms = 0xFFFFFFFFu;
            busy_since = 0;
        }
        was_configured = configured;

        if (km_usbhs_variant() == KM_USB_VARIANT_PAD && configured) {
            if (km_usbhs_pad_ready()) {
                busy_since = 0;
                const uint32_t t = (now - configured_at) & ~3u;     /* new state at most every 4 ms */
                if (t != last_pad_ms) {
                    km_usb_pad_state_t s;
                    uint8_t report[KM_USB_PAD_REPORT_LEN];
                    km_usb_pad_sweep(t, &s);
                    km_usb_pad_pack(&s, report);
                    if (km_usbhs_send_pad(report, sizeof report)) {
                        last_pad_ms = t;
                        pad_sent++;
                    }
                }
            } else if (busy_since == 0) {
                busy_since = now | 1u;
            } else if (now - busy_since > PAD_RECOVER_AFTER_MS) {   /* the host does not poll IF3: keep the other endpoints healthy */
                km_usbhs_pad_recover();
                recoveries++;
                busy_since = 0;
            }
        }

        if ((++passes & 0x1FFu) == 0) {
            const uint16_t v[18] = {
                (uint16_t)hw[0], (uint16_t)(hw[0] >> 16), (uint16_t)hw[1], (uint16_t)(hw[1] >> 16),
                (uint16_t)hw[2], (uint16_t)(hw[2] >> 16), (uint16_t)hw[3], (uint16_t)(hw[3] >> 16),
                verdict, (uint16_t)km_usbhs_variant(), configured ? 1u : 0u, km_usbhs_fell_back() ? 1u : 0u,
                pad_sent, recoveries, km_usbhs_host_leds(), (uint16_t)km_usbhs_led_report_count(),
                (uint16_t)(now / 1000u), km_usbhs_fallback_clock_running() ? 1u : 0u };
            km_scope_publish(SCOPE_TAG, v, 18);
        }
    }
}
