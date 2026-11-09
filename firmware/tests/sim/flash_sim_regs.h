/* Host-test replacement for n32h48x_regs.h: registers and flash are a simulation (see tests/test_flash_boot.c). */
#ifndef FLASH_SIM_REGS_H
#define FLASH_SIM_REGS_H

#include <stdint.h>

uint32_t *km_sim_reg32(uintptr_t addr);
uint8_t km_sim_read8(uintptr_t addr);
void km_sim_reset(void);
void km_sim_feed(void);

#define KM_REG32(addr)    (*km_sim_reg32((uintptr_t)(addr)))
#define KM_READ8(addr)    km_sim_read8((uintptr_t)(addr))
#define KM_SYSTEM_RESET() km_sim_reset()

static inline void km_iwdg_feed(void)
{
    km_sim_feed();
}

static inline uint32_t km_irq_save(void) { return 0; }
static inline void km_irq_restore(uint32_t primask) { (void)primask; }

#endif
