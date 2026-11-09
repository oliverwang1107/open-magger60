/*
 * D1 "alive" image (M2_PLAN.md M2-D1): proves our own startup runs on the real keyboard.
 * It does nothing visible: no USB, no LEDs. It only feeds the IWDG forever.
 *
 * How to tell it is alive without any output: after flashing, the keyboard is absent from USB (same as the
 * rescue drill), but unlike the drill it does NOT reset every 2 s. So holding ESC + left CTRL without
 * replugging does NOT bring the bootloader back; unplugging, holding the keys and plugging in does.
 */
#include "n32h48x_regs.h"

int main(void)
{
    for (;;) {
        km_iwdg_feed();
        for (volatile uint32_t i = 0; i < 20000u; i++) {
        }
    }
}
