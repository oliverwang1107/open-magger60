#include "km/km_scan.h"

#include <string.h>

/*
 * The sliding sum, per packed word i:
 *   sum[i] += new - ring[i][idx];  ring[i][idx] = new;
 * Two 16-bit samples share one 32-bit word, so a single add/subtract updates both halves.
 * The arithmetic is modulo 2^32 and the true packed sum is the plain integer sum of the 16
 * packed samples, which stays below 2^32 for 12-bit samples; the intermediate borrows
 * between the halves therefore cancel and the result never carries across the middle.
 */
static void update_word(km_scan_t *s, unsigned word, uint32_t sample)
{
    s->sum[word] += sample - s->ring[word][s->idx];
    s->ring[word][s->idx] = sample;
    s->out[2u * word]      = (uint16_t)(s->sum[word] & 0xFFFFu);
    s->out[2u * word + 1u] = (uint16_t)(s->sum[word] >> 16);
}

void km_scan_init(km_scan_t *s)
{
    memset(s, 0, sizeof *s);
    s->idx = (uint8_t)(KM_SCAN_DEPTH - 1u); /* phase 0 of the first cycle wraps to position 0 */
}

void km_scan_push_phase(km_scan_t *s, unsigned phase, const uint32_t words[KM_SCAN_PHASE_WORDS])
{
    if (phase >= KM_SCAN_PHASES) {
        return;
    }
    if (phase == 0u) {
        s->idx = (uint8_t)((s->idx + 1u) & (KM_SCAN_DEPTH - 1u));
    }
    for (unsigned k = 0; k < KM_SCAN_PHASE_WORDS; k++) {
        update_word(s, phase * KM_SCAN_PHASE_WORDS + k, words[k]);
    }
}

void km_scan_push_injected(km_scan_t *s, uint32_t w36, uint32_t w37)
{
    update_word(s, KM_SCAN_PHASES * KM_SCAN_PHASE_WORDS, w36);
    update_word(s, KM_SCAN_PHASES * KM_SCAN_PHASE_WORDS + 1u, w37);
}

const uint16_t *km_scan_sums(const km_scan_t *s)
{
    return s->out;
}

const km_slot_info_t *km_scan_slot_info(unsigned slot)
{
    return (slot < KM_NUM_SLOTS) ? &km_slot_table[slot] : NULL;
}
