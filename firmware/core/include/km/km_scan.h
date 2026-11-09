/*
 * km_scan.h - hall-sensor scan core: DMA words in, 76 sixteen-point sums out.
 * Interface: M3_PLAN.md section 2. Slot numbering and slot -> key: our measured map (core/data/slotmap_measured.json).
 *
 * The driver (port/n32h48x) owns the ADC/DMA/mux hardware and hands the raw words to this
 * file; nothing here touches a register. Slot s = 2 * word + half (half 0 = low 16 bits =
 * ADC1/ADC3, half 1 = high 16 bits = ADC2/ADC4). Words 0..35 come from the four mux phases
 * (9 words each), words 36/37 are the injected channels (slots 72 S, 73 A, 74 D, 75 W).
 */
#ifndef KM_SCAN_H
#define KM_SCAN_H

#include "km_types.h"

#define KM_SCAN_PHASES      4u   /* mux phases per scan cycle (PB3/PB4) */
#define KM_SCAN_PHASE_WORDS 9u   /* DMA words per phase: 6 for ADC1+2, 3 for ADC3+4 */
#define KM_SCAN_WORDS       38u  /* 4 * 9 DMA words + 2 injected words */
#define KM_SCAN_DEPTH       16u  /* samples in each sliding sum */
#define KM_SCAN_PHASE_INJ   4u   /* km_slot_info_t.phase of the injected slots */

/* Where a slot's sample comes from and which key was found on it when the keyboard was measured. */
typedef struct {
    uint8_t     phase;    /* mux phase 0..3, or KM_SCAN_PHASE_INJ */
    uint8_t     adc;      /* 1..4 */
    uint8_t     rank;     /* position in the regular sequence (1..6); 1 for the injected channel */
    uint8_t     channel;  /* ADC input channel number */
    bool        assigned; /* a key was found on this slot (firmware/core/data/slotmap_measured.json) */
    const char *key;      /* measured key name (LSHIFT, ESC, ...), "(unassigned)" when not assigned */
} km_slot_info_t;

/* Plain state, no heap; the owner just allocates one (about 2.6 KB). */
typedef struct km_scan {
    uint32_t ring[KM_SCAN_WORDS][KM_SCAN_DEPTH]; /* last 16 packed samples of every word */
    uint32_t sum[KM_SCAN_WORDS];                 /* packed sums, low 16 bits = half 0 */
    uint16_t out[KM_NUM_SLOTS];                  /* the same sums split per slot */
    uint8_t  idx;                                /* ring position of the current cycle */
} km_scan_t;

/* All samples and sums zero; the first phase 0 push starts ring position 0. */
void km_scan_init(km_scan_t *s);

/*
 * One mux phase (0..3) worth of DMA words: words[0..5] = ADC1+2 (high 16 = ADC2, low 16 =
 * ADC1), words[6..8] = ADC3+4 (high 16 = ADC4, low 16 = ADC3). Pushing phase 0 starts a new
 * cycle (the ring position advances once per four phases); a phase above 3 is ignored.
 *
 * Each sample must be a 12-bit value (<= 4095): 16 of them still fit 16 bits, so the two
 * halves of a word are summed together without carry between them. The samples are not
 * masked (that would cost a cycle per word on the hot path); a larger sample would leak into
 * the neighbouring slot. The ADC is configured for 12 bits and every reading recorded on the
 * keyboard (core/data/slot_ranges_s2.json) is below 4095, so this cannot happen.
 *
 * Each phase updates exactly its own 9 words; nothing of the next phase is touched early.
 */
void km_scan_push_phase(km_scan_t *s, unsigned phase, const uint32_t words[KM_SCAN_PHASE_WORDS]);

/*
 * Injected channels, sampled on every trigger but taken into the sum only once per cycle,
 * so that, like every other slot, each ring position holds exactly one sample per cycle
 * (a 16-cycle window): call it once after the phase 3 push. w36 = ADC2 << 16 | ADC1 (slots 73 A, 72 S),
 * w37 = ADC4 << 16 | ADC3 (slots 75 W, 74 D). Goes into the ring position of the cycle
 * that the last phase 0 push opened.
 */
void km_scan_push_injected(km_scan_t *s, uint32_t w36, uint32_t w37);

/* The 76 sixteen-sample sums (max 65520), valid until the next push. */
const uint16_t *km_scan_sums(const km_scan_t *s);

/* Slot table entry, NULL when slot >= KM_NUM_SLOTS. */
const km_slot_info_t *km_scan_slot_info(unsigned slot);

/* The table behind km_scan_slot_info (generated: core/gen/km_slot_data.c). */
extern const km_slot_info_t km_slot_table[KM_NUM_SLOTS];

#endif /* KM_SCAN_H */
