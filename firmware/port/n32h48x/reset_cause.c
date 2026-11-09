#include "reset_cause.h"

#include "km/km_health.h"
#include "n32h48x_regs.h"

#define RCC_CTRLSTS      KM_REG32(0x40021014u)
#define CTRLSTS_FLAGS    0xFC000000u    /* assumed flag bits 26..31 (UNVERIFIED) */
#define CTRLSTS_RMV      (1u << 24)     /* assumed "remove reset flags" request (UNVERIFIED) */

#define BREADCRUMB_ADDR  0x20025F00u
#define BREADCRUMB_MAGIC 0x4B4D4252u    /* "KMBR" */

typedef struct {
    uint32_t magic, faults, boots, check;
} breadcrumb_t;

#define BC ((volatile breadcrumb_t *)(uintptr_t)BREADCRUMB_ADDR)

extern uint32_t _estack;      /* end of the stack, which the linker script places right behind .bss */

static uint32_t check_of(uint32_t faults, uint32_t boots)
{
    return BREADCRUMB_MAGIC ^ faults ^ (boots * 0x9E3779B1u) ^ 0xA5A5A5A5u;
}

static bool bc_usable(void)
{
    return (uintptr_t)&_estack <= BREADCRUMB_ADDR;
}

uint8_t km_reset_cause_read_and_clear(void)
{
    const uint32_t v = RCC_CTRLSTS;
    const uint8_t cause = km_health_reset_cause_from_reg(v);
    if ((v & CTRLSTS_FLAGS) != 0u) {
        RCC_CTRLSTS = v | CTRLSTS_RMV;
    }
    return cause;
}

void km_breadcrumb_boot(km_breadcrumb_t *prev)
{
    prev->valid = false;
    prev->prev_faults = 0;
    prev->boots = 0;
    if (!bc_usable()) {
        return;
    }
    const uint32_t magic = BC->magic, faults = BC->faults, boots = BC->boots, check = BC->check;
    if (magic == BREADCRUMB_MAGIC && check == check_of(faults, boots) && faults <= 0xFFu) {
        prev->valid = true;
        prev->prev_faults = (uint8_t)faults;
        prev->boots = boots + 1u;
    } else {
        prev->boots = 1u;
    }
    BC->magic = BREADCRUMB_MAGIC;
    BC->faults = 0;
    BC->boots = prev->boots;
    BC->check = check_of(0, prev->boots);
}

void km_breadcrumb_update(uint8_t fault_flags)
{
    if (!bc_usable() || BC->magic != BREADCRUMB_MAGIC || BC->faults == fault_flags) {
        return;
    }
    const uint32_t boots = BC->boots;
    BC->faults = fault_flags;
    BC->check = check_of(fault_flags, boots);
}
