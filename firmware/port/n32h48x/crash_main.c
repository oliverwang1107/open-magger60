/*
 * M2-C rescue drill image (M2_PLAN.md): a VALID app (app-ID and vector table pass every bootloader check)
 * that does nothing and never feeds the IWDG that the bootloader started. The keyboard therefore shows no
 * USB device and resets itself about every 2 seconds. Holding ESC + left CTRL while it resets must bring
 * the bootloader (19F5:AF1F) back; then flash the stock image again. It touches no peripheral and writes
 * no flash.
 */
int main(void)
{
    for (;;) {
    }
}
