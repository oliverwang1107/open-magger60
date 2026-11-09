/*
 * Minimal Cortex-M4F startup for the smoke build: vector table, .data/.bss init, main().
 * No hardware is touched. The real bring-up (clock tree 8 MHz HSE x30, VTOR, IWDG policy,
 * the early "enter bootloader" key combination) comes with milestone M2 / level L0
 * (CUSTOM_FIRMWARE_PLAN.md).
 *
 * Vector table length: 134 entries = 16 system vectors + 118 interrupts (IRQ116 = USB_HS; the IRQ numbers the
 * firmware uses are listed in FIRMWARE_Magger60.md section 3).
 */
#include <stdint.h>

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

typedef void (*vector_fn)(void);

/* 16 system vectors (reserved slots are 0), then every device interrupt defaults to
 * Default_Handler via a GNU range designator. */
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
};

void Reset_Handler(void)
{
    const uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata; ) {
        *dst++ = *src++;
    }
    for (uint32_t *p = &_sbss; p < &_ebss; ) {
        *p++ = 0;
    }
    (void)main();
    for (;;) {
    }
}

void Default_Handler(void)
{
    for (;;) {
    }
}
