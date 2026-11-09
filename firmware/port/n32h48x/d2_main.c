/*
 * D2 image (M2_PLAN.md M2-D2): the keyboard enumerates on USB as 19F5:FB49 with the stock descriptors
 * and does nothing else. No key reports, no vendor commands, no LEDs. The IWDG is fed every pass.
 *
 * Expected on the host: a "Magger60" composite device with three HID interfaces appears (Device Manager /
 * tools/hid_survey.py), typing does nothing. If the device does not appear, the keyboard is still
 * recoverable: hold ESC + left CTRL while plugging in.
 */
#include "n32h48x_regs.h"
#include "usb_hs.h"

int main(void)
{
    km_usbhs_init();
    for (;;) {
        km_iwdg_feed();
        km_usbhs_poll();
    }
}
