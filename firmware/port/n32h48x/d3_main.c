/*
 * D3 image (M2_PLAN.md M2-D3): D2 plus the vendor channel with ONE command, 0x03 (version + build string),
 * answered like the stock firmware. Everything else on that channel gets the stock "unknw" reply and does
 * nothing. Still no key reports and no LEDs.
 *
 * Expected on the host: the D2 behaviour, and additionally
 *     python tools/magger60_backup.py --list      (device present)
 * and a read-only version query (cmd 3) is answered with the build string below. The backup tool's full
 * --run stops at its flash-read check, which this firmware does not implement (0xE0 is "unknw"): that is
 * intended, nothing can be read or written through this firmware.
 */
#include "km/km_vendor.h"
#include "n32h48x_regs.h"
#include "usb_hs.h"

static const km_vendor_info_t k_info = { 0x0104, __DATE__ "," __TIME__ "KMFW-D3" };

static bool vendor(const uint8_t in[64], uint8_t out[64])
{
    return km_vendor_handle(&k_info, in, out);
}

int main(void)
{
    km_usbhs_set_vendor_handler(vendor);
    km_usbhs_init();
    for (;;) {
        km_iwdg_feed();
        km_usbhs_poll();
    }
}
