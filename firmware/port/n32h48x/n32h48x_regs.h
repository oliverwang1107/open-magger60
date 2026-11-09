/*
 * The few registers the application code touches directly. Addresses come from the chip vendor's
 * SVD (peripheral map: FIRMWARE_Magger60.md section 3); the IWDG key values are the standard ones for this watchdog
 * and the feeding rule is BOOTLOADER_ANALYSIS.md section 5. Anything not listed here has not been needed yet.
 */
#ifndef N32H48X_REGS_H
#define N32H48X_REGS_H

#include <stdint.h>

#define KM_REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* Cortex-M system control block */
#define KM_SCB_CPACR   KM_REG32(0xE000ED88u)
#define KM_SCB_VTOR    KM_REG32(0xE000ED08u)

/* Byte read of flash, and the system reset request. Macros so the host test can replace them with a simulation. */
#define KM_READ8(addr)  (*(volatile const uint8_t *)(uintptr_t)(addr))
#define KM_SYSTEM_RESET()                                      do {                                                           __asm volatile("dsb");                                     KM_REG32(0xE000ED0Cu) = 0x05FA0004u;                       __asm volatile("dsb");                                 } while (0)

/* The app is linked at 0x08008000; the bootloader leaves VTOR at 0x08000000, so we must set it. */
#define KM_APP_BASE    0x08008000u

/* Independent watchdog. The bootloader starts it on EVERY boot (about 2 s), it cannot be stopped,
 * so the application must feed it from its first instruction. */
#define KM_IWDG_KEY    KM_REG32(0x40001800u)
#define KM_IWDG_FEED   0xAAAAu

static inline void km_iwdg_feed(void)
{
    KM_IWDG_KEY = KM_IWDG_FEED;
}

/* Mask interrupts and return the previous mask; km_irq_restore() puts it back. */
static inline uint32_t km_irq_save(void)
{
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask) : : "memory");
    __asm volatile("cpsid i" : : : "memory");
    return primask;
}

static inline void km_irq_restore(uint32_t primask)
{
    __asm volatile("msr primask, %0" : : "r"(primask) : "memory");
}

#endif
