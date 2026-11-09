/*
 * D4 image (M2_PLAN.md M2-D4): D3 plus "enter the bootloader from the host", the way the stock firmware
 * accepts it: one exact raw packet ('_' 06 00 52 01 00 00 00 51). This image recognises only
 * that exact packet; the firmware then sets bit 0 of the byte at 0x0802C000 (the only flash it ever writes, see
 * km_flash_guard.h), disconnects from USB and resets. The bootloader sees the flag, clears it again and stays in
 * update mode (19F5:AF1F).
 *
 * Still no key reports and no LEDs.
 */
#include <stdbool.h>

#include "flash_boot.h"
#include "km/km_vendor.h"
#include "n32h48x_regs.h"
#include "usb_hs.h"

static const km_vendor_info_t k_info = { 0x0104, __DATE__ "," __TIME__ "KMFW-D4" };

static volatile bool g_enter_boot;

static bool vendor(const uint8_t in[64], uint8_t out[64])
{
    if (km_vendor_is_enter_boot_request(in)) {
        g_enter_boot = true;               /* acted on in the main loop, after the USB transfer is over */
        return false;
    }
    return km_vendor_handle(&k_info, in, out);
}

int main(void)
{
    km_usbhs_set_vendor_handler(vendor);
    km_usbhs_init();
    for (;;) {
        km_iwdg_feed();
        km_usbhs_poll();
        if (g_enter_boot) {
            km_usbhs_disconnect();
            km_flash_request_bootloader();
        }
    }
}
