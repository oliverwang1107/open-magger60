/*
 * Startup for the real application images (D-series of M2_PLAN.md). Differences from startup.c, which the
 * smoke and rescue-drill images keep unchanged:
 *   - VTOR is pointed at this image's vector table first (the bootloader leaves it at 0x08000000);
 *   - the FPU is enabled;
 *   - the IWDG is fed before anything else, because the bootloader already started it.
 * Clocks are whatever the bootloader left (it runs its own clock setup right before jumping here); the
 * app does not depend on them until the USB step.
 */
#include <stdint.h>

#include "n32h48x_regs.h"

#define KM_VECTOR_COUNT 134

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
extern int main(void);

void Reset_Handler(void);
void Default_Handler(void);

void NMI_Handler(void)          __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)          __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)       __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)      __attribute__((weak, alias("Default_Handler")));

/* Sensing interrupts (M3, adc_scan.c). Every other image leaves these on Default_Handler. */
void DMA1_Channel1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));   /* IRQ11 */
void DMA1_Channel3_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));   /* IRQ13 */
void ADC1_2_IRQHandler(void)        __attribute__((weak, alias("Default_Handler")));   /* IRQ18 */
void ADC3_4_IRQHandler(void)        __attribute__((weak, alias("Default_Handler")));   /* IRQ47 */

/* The range initialiser below is deliberately overridden for a few IRQs. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((section(".isr_vector"), used))
const void *const km_vector_table[KM_VECTOR_COUNT] = {
    (void *)&_estack,         /*  0 initial SP */
    (void *)Reset_Handler,    /*  1 */
    (void *)NMI_Handler,      /*  2 */
    (void *)HardFault_Handler,/*  3 */
    (void *)MemManage_Handler,/*  4 */
    (void *)BusFault_Handler, /*  5 */
    (void *)UsageFault_Handler,/* 6 */
    0, 0, 0, 0,               /*  7-10 reserved */
    (void *)SVC_Handler,      /* 11 */
    (void *)DebugMon_Handler, /* 12 */
    0,                        /* 13 reserved */
    (void *)PendSV_Handler,   /* 14 */
    (void *)SysTick_Handler,  /* 15 */
    [16 ... KM_VECTOR_COUNT - 1] = (void *)Default_Handler,
    [16 + 11] = (void *)DMA1_Channel1_IRQHandler,
    [16 + 13] = (void *)DMA1_Channel3_IRQHandler,
    [16 + 18] = (void *)ADC1_2_IRQHandler,
    [16 + 47] = (void *)ADC3_4_IRQHandler,
};
#pragma GCC diagnostic pop

void Reset_Handler(void)
{
    km_iwdg_feed();
    KM_SCB_VTOR = KM_APP_BASE;
    KM_SCB_CPACR |= 0xFu << 20;           /* CP10/CP11 full access */
    __asm volatile("dsb\n isb");

    const uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata; ) {
        *dst++ = *src++;
    }
    for (uint32_t *p = &_sbss; p < &_ebss; ) {
        *p++ = 0;
    }
    km_iwdg_feed();
    (void)main();
    for (;;) {
        km_iwdg_feed();
    }
}

/* An unexpected exception loops WITHOUT feeding the IWDG, so the keyboard resets itself within about 2 s and
 * the bootloader (and its ESC + left CTRL rescue) runs again. */
void Default_Handler(void)
{
    for (;;) {
    }
}
