/* Host-test replacement for n32h48x_regs.h used by store_flash.c: registers and flash are a simulation (tests/test_store_flash.c). */
#ifndef STORE_SIM_REGS_H
#define STORE_SIM_REGS_H

#include <stdint.h>

uint32_t *km_ssim_reg32(uintptr_t addr);
uint8_t km_ssim_read8(uintptr_t addr);
void km_ssim_feed(void);

#define KM_REG32(addr) (*km_ssim_reg32((uintptr_t)(addr)))
#define KM_READ8(addr) km_ssim_read8((uintptr_t)(addr))

static inline void km_iwdg_feed(void)
{
    km_ssim_feed();
}

static inline uint32_t km_irq_save(void) { return 0; }
static inline void km_irq_restore(uint32_t primask) { (void)primask; }

#endif
