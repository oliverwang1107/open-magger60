/*
 * K1 image (M4_PLAN.md hardware ladder step K1): D4 with OUR OWN USB descriptors. No scanning, no key reports.
 *
 * What it does:
 *   - enumerates as the composite device of km_usb_desc.c (boot keyboard, vendor channel, NKRO + media),
 *   - answers vendor cmd 3 (version + build string "KMFW-K1") and the scope channel 0xC5 with a small diagnostics snapshot
 *     (tag 0x0100 + 1: see below), so tools/magger60_info.py and tools/magger60_scope.py work,
 *   - keeps the "enter the bootloader from the host" command of D4 (the only flash write: the boot flag).
 *
 * Key reports: NONE by default. The compile-time switch KM_K1_TEST_USAGE (default 0 = off) makes the image type exactly one
 * keyboard-page key, once, about 5 seconds after the host configured the device, through the boot interface IF0 (press,
 * then release). It exists only to prove the IN path on the real keyboard in a later, separately approved step; the
 * default build never sends a key. The lead wires the real reports at integration.
 *
 * Scope snapshot (tag 0x0101, 8 values): [0] 1 = configured  [1] seconds since start (wraps at 65535)  [2] boot protocol
 * selected on IF0  [3] test-key state (0 off, 1 waiting, 2 pressed, 3 done)  [4] reports sent  [5..7] 0.
 */
#include <stdbool.h>

#include "flash_boot.h"
#include "km/km_scope.h"
#include "km/km_vendor.h"
#include "n32h48x_regs.h"
#include "usb_hs.h"

#ifndef KM_K1_TEST_USAGE
#define KM_K1_TEST_USAGE 0u
#endif

static const km_vendor_info_t k_info = { 0x0104, __DATE__ "," __TIME__ "KMFW-K1" };

static volatile bool g_enter_boot;

static bool vendor(const uint8_t in[64], uint8_t out[64])
{
    if (km_vendor_is_enter_boot_request(in)) {
        g_enter_boot = true;               /* acted on in the main loop, after the USB transfer is over */
        return false;
    }
    return km_vendor_handle(&k_info, in, out);
}

/* Loop passes stand in for time (no timer is set up in this image): the "seconds" below are only a rough pacing for the
 * demo, not a clock. */
static uint32_t g_passes;

int main(void)
{
    km_usbhs_set_vendor_handler(vendor);
    km_usbhs_init();
    km_scope_reset();

    unsigned state = 0;            /* test key: 0 off, 1 waiting, 2 pressed, 3 done */
    uint16_t sent = 0;
    uint32_t configured_since = 0;
    uint32_t seconds = 0, next_second = 0;
    for (;;) {
        km_iwdg_feed();
        km_usbhs_poll();
        if (g_enter_boot) {
            km_usbhs_disconnect();
            km_flash_request_bootloader();
        }
        g_passes++;
        if (g_passes >= next_second) {
            next_second = g_passes + 200000u;       /* roughly a second: the loop is not timed, the figure only paces the demo */
            seconds++;
        }
        if (!km_usbhs_configured()) {
            configured_since = 0;
            state = (KM_K1_TEST_USAGE != 0u) ? 1u : 0u;
        } else if (configured_since == 0) {
            configured_since = seconds;
        }
        if (KM_K1_TEST_USAGE != 0u && km_usbhs_configured() && km_usbhs_kbd_ready()) {
            uint8_t report[8] = { 0 };
            if (state == 1u && seconds - configured_since >= 5u) {
                report[2] = (uint8_t)KM_K1_TEST_USAGE;
                if (km_usbhs_send_kbd(report)) {
                    state = 2u;
                    sent++;
                }
            } else if (state == 2u) {
                if (km_usbhs_send_kbd(report)) {       /* all zero = released */
                    state = 3u;
                    sent++;
                }
            }
        }
        if ((g_passes & 0x3FFu) == 0) {
            const uint16_t v[8] = { km_usbhs_configured() ? 1u : 0u, (uint16_t)seconds, km_usbhs_kbd_boot_protocol() ? 1u : 0u,
                                    (uint16_t)state, sent, 0, 0, 0 };
            km_scope_publish(0x0101u, v, 8);
        }
    }
}
