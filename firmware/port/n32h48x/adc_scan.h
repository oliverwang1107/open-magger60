/*
 * adc_scan.h - hall-sensor scan driver: GTIM1 trigger -> ADC1+2 / ADC3+4 (6 + 3 conversions each) -> DMA1 ch1 / ch3,
 * four mux phases per frame, injected channels (WASD) by ADC interrupt. The register sequence is the
 * one verified on the keyboard in S3 (SENSING_BRINGUP_NOTES.md lists the structure and, per item, how well it is
 * established; where the values were first learned: DISCLAIMER.md section 5), with two design choices:
 *
 *   - The sliding sums are NOT computed in the interrupt. The interrupt only advances the mux phase, re-arms the DMA
 *     and restarts the timer; the main loop does the rest (km_scan_push_*).
 *   - After phase 3 the timer is NOT restarted. The frame (4 phases + injected words) stays in g_phase_words / g_inj
 *     until the main loop calls adc_scan_release_frame(), which starts the next frame. A frame takes tens of
 *     microseconds, the main loop (USB polling) is much slower, so the driver is paced by its consumer instead of
 *     letting the DMA overwrite buffers the main loop is still reading.
 *
 * Interrupts used: DMA1_Channel1 (IRQ11), DMA1_Channel3 (IRQ13), ADC1_2 (IRQ18), ADC3_4 (IRQ47).
 */
#ifndef ADC_SCAN_H
#define ADC_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#include "sense_tables.h"

/* DMA targets, M3_PLAN.md section 2: phase p words 0..5 = ADC1+2 (high 16 = ADC2, low 16 = ADC1), words 6..8 = ADC3+4. */
extern uint32_t g_phase_words[SENSE_NUM_PHASES][SENSE_PHASE_WORDS];
/* g_inj[0] = ADC2 << 16 | ADC1 (slots 73 A, 72 S), g_inj[1] = ADC4 << 16 | ADC3 (slots 75 W, 74 D). */
extern uint32_t g_inj[2];

typedef struct {
    uint32_t phases;            /* mux phases completed (both DMA channels done) */
    uint32_t frames;            /* frames completed (phase 3 done) */
    uint32_t dma_errors;        /* transfer-error flag seen on DMA ch1 or ch3 (flag position assumed, see notes) */
    uint32_t overruns;          /* a channel completed twice before its partner did (an unpaired completion) */
    uint32_t inj_events;        /* injected-conversion interrupts handled (ADC1_2 + ADC3_4) */
    uint32_t phase_cycles[SENSE_NUM_PHASES];   /* last timer-start -> both-DMA-done time of each phase, CPU cycles */
    uint32_t frame_cycles;      /* last first-trigger -> phase-3-done time of a frame */
    uint32_t frame_cycles_min;
    uint32_t frame_cycles_max;
    uint32_t init_err;          /* SENSE_ERR_* bits from the ADC power-up / calibration */
    uint32_t restarts;          /* adc_scan_restart() calls (M5 T-E) */
} adc_scan_stats_t;

/* Clocks, GPIO/mux, DMA, the four ADCs (power-up + calibration), GTIM1, NVIC. Does not start scanning.
 * Returns 0, or SENSE_ERR_* bits when an ADC did not become ready / finish calibrating. */
uint32_t adc_scan_init(void);
/* Start the first frame (mux phase 0). */
void adc_scan_start(void);
/* Stop the timer and the interrupts. A frame in progress is abandoned. */
void adc_scan_stop(void);

/* True once the four phases of a frame are in g_phase_words (the injected words are the latest seen). */
bool adc_scan_frame_ready(void);
/* Bit p set = phase p of the current frame is complete (phase-done flags; all four set when the frame is ready). */
uint8_t adc_scan_phase_mask(void);
/* The consumer is done with g_phase_words / g_inj: start the next frame. */
void adc_scan_release_frame(void);

void adc_scan_get_stats(adc_scan_stats_t *out);

/* ---- M5 T-E: fail-safe support (the main loop owns the policy, see km_health.h) ---- */

/* A frame that has been released (or started) must complete well within a millisecond; this is the longest the main loop
 * waits for it. Measured from the release, so a slow consumer never looks like a stalled scan. */
#define ADC_SCAN_FRAME_TIMEOUT_CYCLES (3u * 240000u)    /* 3 ms at 240 MHz */

/* true: scanning is running, no frame is ready and none has completed for ADC_SCAN_FRAME_TIMEOUT_CYCLES. `now` = CYCCNT. */
bool adc_scan_frame_timeout(uint32_t now);
/* Test hook for the "scan stalls" hardware test (image variants km_l30s / km_l30d): disable the ADC1+2 DMA channel so that the
 * running frame can never complete. Nothing else changes; adc_scan_restart() repairs it. */
void adc_scan_test_stall(void);
/* Running total of the CPU cycles spent inside the four interrupt handlers (wraps at 2^32). A caller that times a stretch of
 * main-loop code reads it before and after and subtracts the difference, to separate its own time from interrupt time. */
uint32_t adc_scan_irq_cycles(void);
/* DMA transfer errors seen since the last call (the count is cleared by the call). Errors are noticed when a phase completes,
 * and by adc_scan_restart() when it inspects a chain that never completed. */
uint32_t adc_scan_take_dma_errors(void);
/* Bring a stalled or wedged chain back: stop the timer and the interrupts, disable and fully re-configure both DMA channels,
 * clear every pending flag (DMA, ADC end-of-injection, NVIC pending), drop the frame in progress, mux phase 0, start again.
 * Bounded (no waiting loops), safe to call at any time from the main loop, also when the chain is running normally. */
void adc_scan_restart(uint32_t now);

/* Interrupt handlers (named as the vector table in app_startup.c expects; public for the host tests). */
void DMA1_Channel1_IRQHandler(void);
void DMA1_Channel3_IRQHandler(void);
void ADC1_2_IRQHandler(void);
void ADC3_4_IRQHandler(void);

#endif /* ADC_SCAN_H */
