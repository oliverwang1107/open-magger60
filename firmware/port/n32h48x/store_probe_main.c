/*
 * km_store_probe (M5 task T-D): BUILD-ONLY image that exercises the settings store on the real flash driver.
 * It is NEVER flashed in task T-D; it exists so the address audit (tools/audit_addrs.py) can show which absolute
 * addresses the store code can touch. The first real write of the settings sectors is hardware step L3-2 of
 * M5_PLAN.md and will use the integrated firmware, with the owner's go-ahead.
 *
 * Behaviour if it were run: load the stored settings (defaults if there are none), flip the active profile and save.
 */
#include "km/km_settings.h"
#include "km/km_store.h"
#include "n32h48x_regs.h"
#include "store_flash.h"

int main(void)
{
    static km_settings_t s;
    km_store_bind(km_store_flash());
    if (!km_store_load(&s)) {
        km_settings_default(&s);
    }
    s.active = (uint8_t)((s.active + 1u) % KM_PROFILES);
    (void)km_store_save(&s);
    for (;;) {
        km_iwdg_feed();
        for (volatile uint32_t i = 0; i < 20000u; i++) {
        }
    }
}
