#include "sense_tables.h"

#include "sense_regs.h"

/* ADC channel per rank (SENSING_BRINGUP_NOTES.md section 1.2). The channel-to-pin match is chip data and was not checked pin
 * by pin; the channels themselves were checked against real key presses in S3. */
const uint8_t sense_regular_ch[SENSE_NUM_ADC][SENSE_MAX_REGULAR] = {
    { 3, 4, 5, 2, 6, 7 },        /* ADC1 */
    { 0, 2, 17, 13, 5, 11 },     /* ADC2 */
    { 12, 1, 2, 0, 0, 0 },       /* ADC3 */
    { 6, 6, 6, 0, 0, 0 },        /* ADC4: the same channel three times */
};
const uint8_t sense_regular_len[SENSE_NUM_ADC] = { 6, 6, 3, 3 };
const uint8_t sense_injected_ch[SENSE_NUM_ADC] = { 15, 1, 7, 13 };

uint32_t sense_mux_bits(unsigned phase)
{
    return (uint32_t)(phase & 3u) << SENSE_MUX_SHIFT;
}

uint32_t sense_mux_apply(uint32_t pod, unsigned phase)
{
    return (pod & ~(uint32_t)SENSE_MUX_MASK) | sense_mux_bits(phase);
}

unsigned sense_slot_of(unsigned phase, unsigned word, unsigned half)
{
    return 2u * (SENSE_PHASE_WORDS * phase + word) + half;
}

bool sense_slot_source(unsigned slot, sense_slot_src_t *out)
{
    if (slot >= SENSE_NUM_SLOTS) {
        return false;
    }
    if (slot >= 72u) {
        const unsigned n = slot - 72u;                        /* 72 = ADC1, 73 = ADC2, 74 = ADC3, 75 = ADC4 */
        out->phase = 4;
        out->adc = (uint8_t)(n + 1u);
        out->rank = 1;
        out->channel = sense_injected_ch[n];
        return true;
    }
    const unsigned word = slot / 2u, half = slot % 2u;
    const unsigned phase = word / SENSE_PHASE_WORDS, w = word % SENSE_PHASE_WORDS;
    out->phase = (uint8_t)phase;
    if (w < 6u) {
        out->adc = (uint8_t)(1u + half);
        out->rank = (uint8_t)(w + 1u);
        out->channel = sense_regular_ch[half][w];
    } else {
        out->adc = (uint8_t)(3u + half);
        out->rank = (uint8_t)(w - 5u);
        out->channel = sense_regular_ch[2u + half][w - 6u];
    }
    return true;
}

void sense_unpack_phase(const uint32_t words[SENSE_PHASE_WORDS], uint16_t out[18])
{
    for (unsigned w = 0; w < SENSE_PHASE_WORDS; w++) {
        out[2u * w] = (uint16_t)(words[w] & 0xFFFFu);
        out[2u * w + 1u] = (uint16_t)(words[w] >> 16);
    }
}

void sense_unpack_all(const uint32_t phase_words[SENSE_NUM_PHASES][SENSE_PHASE_WORDS], const uint32_t inj[2],
                      uint16_t out[SENSE_NUM_SLOTS])
{
    for (unsigned p = 0; p < SENSE_NUM_PHASES; p++) {
        sense_unpack_phase(phase_words[p], &out[18u * p]);     /* slot_of(p, 0, 0) = 18 * p */
    }
    out[72] = (uint16_t)(inj[0] & 0xFFFFu);                    /* ADC1 injected = S */
    out[73] = (uint16_t)(inj[0] >> 16);                        /* ADC2 injected = A */
    out[74] = (uint16_t)(inj[1] & 0xFFFFu);                    /* ADC3 injected = D */
    out[75] = (uint16_t)(inj[1] >> 16);                        /* ADC4 injected = W */
}

/* Register layout (hardware facts): sample-time registers hold 4-bit fields for 8 channels each (ch 0-7, 8-15, 16-17). */
unsigned sense_sampt_index(unsigned ch)
{
    return ch / 8u < 2u ? ch / 8u : 2u;
}

unsigned sense_sampt_shift(unsigned ch)
{
    return (ch % 8u) * 4u;
}

uint32_t sense_sampt_apply(uint32_t reg, unsigned ch, unsigned code)
{
    const unsigned sh = sense_sampt_shift(ch);
    return ((uint32_t)code << sh) | (reg & ~(0xFu << sh));
}

/* The regular sequence has 5-bit fields, six per register: ranks 1-6, 7-12, 13-16. */
unsigned sense_rseq_index(unsigned rank)
{
    return (rank - 1u) / 6u;
}

unsigned sense_rseq_shift(unsigned rank)
{
    return ((rank - 1u) % 6u) * 5u;
}

uint32_t sense_rseq_apply(uint32_t reg, unsigned rank, unsigned ch)
{
    const unsigned sh = sense_rseq_shift(rank);
    return ((uint32_t)ch << sh) | (reg & ~(0x1Fu << sh));
}

uint32_t sense_rseq_len_apply(uint32_t rseq3, unsigned n)
{
    return (rseq3 & SENSE_RSEQ3_KEEP) | ((uint32_t)(n - 1u) << 25);
}

uint32_t sense_jseq_len_apply(uint32_t jseq, unsigned n)
{
    return (jseq & SENSE_JSEQ_KEEP_LEN) | ((uint32_t)(n - 1u) << 25);
}

/* The injected sequence is right-aligned: with n conversions (n = length field + 1) the LAST one always sits at
 * bits [19:15], earlier ranks below it, so rank r is at 15 + 5 * (r - n). */
uint32_t sense_jseq_apply(uint32_t jseq, unsigned rank, unsigned ch)
{
    const unsigned n = ((jseq >> 25) & 0xFu) + 1u;
    const unsigned sh = (unsigned)(15 + 5 * ((int)rank - (int)n));
    return ((uint32_t)ch << sh) | (jseq & ~(0x1Fu << sh));
}

uint32_t sense_dma_chcfg_value(void)
{
    return SENSE_DMA_CHCFG_BASE;
}
