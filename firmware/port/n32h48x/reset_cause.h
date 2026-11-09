/*
 * reset_cause.h - why did the MCU restart? (M5 task T-E, rule 7)
 *
 * Two independent sources, both harmless if they turn out not to work:
 *   1. The reset flags of the clock controller (RCC_CTRLSTS, 0x40021014, name from the vendor SVD). The flag POSITIONS (bits 24..31:
 *      remove-request, -, pin, power-on, software, IWDG, WWDG, low-power) are the usual layout of this chip family and are
 *      UNVERIFIED for the N32H48x: the repository has the register name and address only. Every use of them is guarded: the flags
 *      are cleared only if at least one of the assumed flag bits was set, and a layout that is different just produces an odd
 *      number in the scope value.  The bootloader runs before the app and may already have cleared them: "0" is a normal answer.
 *   2. A breadcrumb in RAM that survives a reset (the S2 image proved the RAM word at 0x20026F08 survives the watchdog reset): the
 *      fault flags that were active shortly before the restart, plus a boot counter. It sits at 0x20025F00, in RAM the image never uses
 *      (the linker script puts the 8 KB stack right behind .bss, the end is about 0x20005B28; km_breadcrumb_boot() refuses to
 *      use the word if the image ever grows to there). Whether the bootloader leaves RAM alone is
 *      the thing to confirm on hardware (L3-0): if it does not, the magic is wrong at boot and the previous faults read as "none".
 */
#ifndef RESET_CAUSE_H
#define RESET_CAUSE_H

#include <stdbool.h>
#include <stdint.h>

/* Read the reset flags (top byte of RCC_CTRLSTS, km_health.h KM_RESET_* bits, 0 = none) and clear them. */
uint8_t km_reset_cause_read_and_clear(void);

typedef struct {
    bool     valid;          /* the breadcrumb of the previous run was intact */
    uint8_t  prev_faults;    /* km_health fault flags as last recorded by the previous run */
    uint32_t boots;          /* boot counter including this one (0 when the RAM was not valid before) */
} km_breadcrumb_t;

/* At boot: read the previous run's breadcrumb into *prev, then start a new one (boots + 1, faults 0). */
void km_breadcrumb_boot(km_breadcrumb_t *prev);
/* Record the current fault flags (cheap; call when they change and about once a second). */
void km_breadcrumb_update(uint8_t fault_flags);

#endif /* RESET_CAUSE_H */
