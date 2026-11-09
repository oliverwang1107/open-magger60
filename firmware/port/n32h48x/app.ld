/*
 * Linker script for the Magger60 application (Nations N32H48x, Cortex-M4F).
 *
 * The application lives at 0x08008000 (behind the 32 KB bootloader that is NOT part of
 * any update image) and must stay below 0x08028000, where the settings
 * sectors begin (FIRMWARE_Magger60.md section 2, CUSTOM_FIRMWARE_PLAN.md section 7).
 * RAM: 160 KB at 0x20000000 (the stock firmware only touches < 0x2000A000; pdsc declares
 * 192 KB, the extra 32 KB is probably CCM at an unconfirmed address, so it is not used).
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08008000, LENGTH = 128K   /* 0x08008000 - 0x08027FFF */
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 160K   /* 0x20000000 - 0x20027FFF */
}

ENTRY(Reset_Handler)

_stack_size = 8K;

SECTIONS
{
    .isr_vector :
    {
        KEEP(*(.isr_vector))   /* vector table first: file offset 0 = 0x08008000, as in the stock image */
    } > FLASH

    .text :
    {
        *(.text*)
        *(.rodata*)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx :
    {
        *(.ARM.exidx*)
    } > FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        . = . + _stack_size;
        . = ALIGN(8);
        _estack = .;
    } > RAM

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }
}

/* The image must never grow into the settings sectors. */
ASSERT(LENGTH(FLASH) == 0x20000, "app flash window must be 128 KB (0x08008000-0x08027FFF)")
