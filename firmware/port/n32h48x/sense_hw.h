/*
 * sense_hw.h - hardware helpers shared by the S1/S2/S3 programs and adc_scan.c: GPIO and mux, ADC power-up and
 * calibration, polled single conversions, a cycle-counter delay.
 *
 * Two ADC configurations (SENSING_BRINGUP_NOTES.md section 2; the values are in sense_regs.h):
 *   - "poll"  = set A, the setup the keyboard's bootloader uses to read ESC and left CTRL at every power-up
 *               (BOOTLOADER_ANALYSIS.md section 3). Used by S1 and S2 v1.
 *   - "scan"  = set B, used by S2 v2 and S3 through adc_scan.c. Verified on the keyboard in S3.
 */
#ifndef SENSE_HW_H
#define SENSE_HW_H

#include <stdbool.h>
#include <stdint.h>

/* Cycle counter (DWT CYCCNT, enabled by setting TRCENA and CYCCNTENA) and a busy-wait that feeds the IWDG every ~50 us. */
void sense_dwt_init(void);
uint32_t sense_cycles(void);
void sense_delay_us(uint32_t us);

/* AFIO clock, 5 V tolerance off, SWJ_CFG = JTAG off / SWD kept, PB3/PB4/PB5 push-pull outputs (low), then the 20 analog pins. */
void sense_gpio_init(void);
/* Select mux phase 0..3 on PB3/PB4 (read-modify-write of GPIOB_POD bits [4:3] only). */
void sense_mux_set(unsigned phase);
unsigned sense_mux_get(void);

/* Idle hook: run by every wait loop and delay in this file at least every SENSE_IDLE_PERIOD_US (when it is set), so a
 * polled USB driver stays serviced while the sensing code waits. The hook runs in the caller's context. */
#define SENSE_IDLE_PERIOD_US 50u
void sense_set_idle_hook(void (*fn)(void));

/* Progress marker kept in RAM that survives a reset (see SENSE_STAGE_ADDR), written at each step of the init/scan. */
#define SENSE_STAGE_ADC_INIT    20u     /* + ADC number */
#define SENSE_STAGE_ADC_RDY     30u
#define SENSE_STAGE_ADC_CAL     40u
#define SENSE_STAGE_POLL_BEGIN  10u
#define SENSE_STAGE_RESET_DONE  11u
#define SENSE_STAGE_CLOCKS_DONE 12u
#define SENSE_STAGE_POLL_DONE   50u
#define SENSE_STAGE_ALT_BEGIN   60u
#define SENSE_STAGE_ALT_DONE    61u
#define SENSE_STAGE_SCAN_PHASE  71u     /* + phase 0..3 */
#define SENSE_STAGE_SCAN_INJ    75u
#define SENSE_STAGE_SCAN_DONE   76u
void sense_stage(uint32_t stage);

/* Failure bits of the init functions below. */
#define SENSE_ERR_RDY_TIMEOUT   0x01u
#define SENSE_ERR_CAL_TIMEOUT   0x02u

/*
 * Bootloader-style polled setup of the ADCs in `adc_mask` (bit n-1 = ADC n): clocks, ADC init, power up, calibration.
 * Calls sense_gpio_init() and sense_dwt_init() first. Returns 0 or SENSE_ERR_* bits (an ADC that fails is left off).
 */
uint32_t sense_poll_init(unsigned adc_mask);
/* A second attempt for ADCs that did not become ready (diagnostic, see sense_hw.c). Returns SENSE_ERR_* bits of the retry. */
uint32_t sense_poll_try_alt_clock(void);

typedef struct {
    uint8_t ok_mask;            /* bit n-1: ADC n is ready and still answering */
    uint8_t rdy_fail_mask;      /* bit n-1: ADC n never became ready */
    uint8_t cal_fail_mask;      /* bit n-1: ADC n's calibration did not finish */
    uint8_t alt_state;          /* 0 not tried, 1 tried and undone, 2 all failed ADCs fixed, 3 some fixed */
    uint32_t timeouts;          /* conversions whose end-of-conversion flag never came */
} sense_poll_status_t;
void sense_poll_status(sense_poll_status_t *out);

/* All 76 slots by polled conversions, mux settle `settle_us` per phase. A slot of an ADC that is not usable reads 0xFFFF
 * and costs no waiting. Waits are bounded and tick the idle hook. */
void sense_poll_scan(uint16_t out[76], uint32_t settle_us);

/* One software-triggered conversion of regular channel `ch` on ADC `adc` (1..4), sample time code 12 like the
 * bootloader. Returns the 12-bit result, or -1 when the ADC is not usable or the end-of-conversion flag never came (3 misses in a row retire the ADC). */
int32_t sense_poll_convert(unsigned adc, unsigned ch);

/* Stock-style setup of all four ADCs for the DMA scan (clocks, reset pulse, ADC clock source, sequences, injected
 * channels, power-up, calibration) - everything of the sensing setup except the DMA and the timer. Returns 0 or SENSE_ERR_*. */
uint32_t sense_scan_adc_init(void);

#endif /* SENSE_HW_H */
