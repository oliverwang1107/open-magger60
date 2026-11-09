/*
 * sense_tables.h - the pure (register-free, host-testable) part of the sensing driver: which ADC channel feeds
 * which slot, the mux phase bits, the slot <-> DMA word mapping, and the encodings of the ADC sequence registers.
 * Source: SENSING_BRINGUP_NOTES.md section 1.2 (channel lists, verified key by key in S3); the slot-to-key map is
 * the measured one in firmware/core/data/slotmap_measured.json.
 */
#ifndef SENSE_TABLES_H
#define SENSE_TABLES_H

#include <stdbool.h>
#include <stdint.h>

#define SENSE_NUM_ADC       4u
#define SENSE_NUM_PHASES    4u
#define SENSE_PHASE_WORDS   9u      /* DMA words per phase: 6 (ADC1+2) + 3 (ADC3+4) */
#define SENSE_NUM_WORDS     38u     /* 4 * 9 + 2 injected words */
#define SENSE_NUM_SLOTS     76u
#define SENSE_MAX_REGULAR   6u

/* Regular-sequence channels per ADC (index 0 = ADC1), by rank 1.. ; lengths 6, 6, 3, 3. */
extern const uint8_t sense_regular_ch[SENSE_NUM_ADC][SENSE_MAX_REGULAR];
extern const uint8_t sense_regular_len[SENSE_NUM_ADC];
/* The one injected channel of each ADC (WASD: slots 72 S = ADC1, 73 A = ADC2, 74 D = ADC3, 75 W = ADC4). */
extern const uint8_t sense_injected_ch[SENSE_NUM_ADC];

/* Mux phase 0..3 -> GPIOB_POD bits [4:3] (PB3 = bit 0 of the phase, PB4 = bit 1). */
uint32_t sense_mux_bits(unsigned phase);
/* New GPIOB_POD value for `phase`, leaving every other output bit alone. */
uint32_t sense_mux_apply(uint32_t pod, unsigned phase);

/* Slot of (phase 0..3, word 0..8 inside the phase, half 0/1): s = 2 * (9 * phase + word) + half. */
unsigned sense_slot_of(unsigned phase, unsigned word, unsigned half);
/* Where a slot's sample comes from. `phase` is 4 for the injected slots 72..75. Returns false when slot >= 76. */
typedef struct {
    uint8_t phase;     /* 0..3, or 4 = injected */
    uint8_t adc;       /* 1..4 */
    uint8_t rank;      /* 1..6, 1 for the injected channel */
    uint8_t channel;
} sense_slot_src_t;
bool sense_slot_source(unsigned slot, sense_slot_src_t *out);

/* One phase worth of DMA words -> 18 values in slot order (value[2 * w + half] belongs to slot_of(phase, w, half)). */
void sense_unpack_phase(const uint32_t words[SENSE_PHASE_WORDS], uint16_t out[18]);
/* All four phases and the two injected words -> the 76 slot values (what S2/S3 publish). */
void sense_unpack_all(const uint32_t phase_words[SENSE_NUM_PHASES][SENSE_PHASE_WORDS], const uint32_t inj[2],
                      uint16_t out[SENSE_NUM_SLOTS]);

/* ADC sample-time register: which of SAMPT1..3 holds `ch` and the bit position of its 4-bit field. */
unsigned sense_sampt_index(unsigned ch);
unsigned sense_sampt_shift(unsigned ch);
uint32_t sense_sampt_apply(uint32_t reg, unsigned ch, unsigned code);

/* Regular sequence register of `rank` (1..16): index into RSEQ1..3 and the shift of its 5-bit field. */
unsigned sense_rseq_index(unsigned rank);
unsigned sense_rseq_shift(unsigned rank);
uint32_t sense_rseq_apply(uint32_t reg, unsigned rank, unsigned ch);
/* RSEQ3 with the sequence length field (bits [28:25] = n - 1) set to n conversions. */
uint32_t sense_rseq_len_apply(uint32_t rseq3, unsigned n);

/* Injected sequence (one conversion): JSEQ length n (field bits [28:25] = n - 1) and the channel of rank 1. */
uint32_t sense_jseq_len_apply(uint32_t jseq, unsigned n);
uint32_t sense_jseq_apply(uint32_t jseq, unsigned rank, unsigned ch);

/* DMA stock CHCFG value for the ADC channels (before TCIE / enable), and the final armed value. */
uint32_t sense_dma_chcfg_value(void);

#endif /* SENSE_TABLES_H */
