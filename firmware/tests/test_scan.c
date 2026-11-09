/* Scan core tests (spec 3.1, 3.2): packed 16-point sliding sums, phase/injected plumbing, slot table. */
#include <stdio.h>

#include "km/km_scan.h"
#include "km_test.h"

typedef uint16_t frame_t[KM_NUM_SLOTS]; /* one 12-bit sample per slot */

static uint32_t pack(const frame_t v, unsigned word)
{
    return (uint32_t)v[2u * word] | ((uint32_t)v[2u * word + 1u] << 16);
}

/* Push phases 0..3 and then the injected words, the way the driver does once per scan cycle. */
static void push_cycle(km_scan_t *s, const frame_t v)
{
    for (unsigned p = 0; p < KM_SCAN_PHASES; p++) {
        uint32_t w[KM_SCAN_PHASE_WORDS];
        for (unsigned k = 0; k < KM_SCAN_PHASE_WORDS; k++) {
            w[k] = pack(v, p * KM_SCAN_PHASE_WORDS + k);
        }
        km_scan_push_phase(s, p, w);
    }
    km_scan_push_injected(s, pack(v, 36), pack(v, 37));
}

static void fill(frame_t v, uint16_t x)
{
    for (unsigned i = 0; i < KM_NUM_SLOTS; i++) {
        v[i] = x;
    }
}

/* ---- reference model: remember every cycle and add up the last 16 per slot ---- */
#define MODEL_CYCLES 400u
static uint16_t g_hist[MODEL_CYCLES][KM_NUM_SLOTS];
static unsigned g_hist_n;

static void model_reset(void)
{
    g_hist_n = 0;
}

static void model_push(const frame_t v)
{
    for (unsigned i = 0; i < KM_NUM_SLOTS; i++) {
        g_hist[g_hist_n][i] = v[i];
    }
    g_hist_n++;
}

static uint32_t model_sum(unsigned slot)
{
    uint32_t sum = 0;
    unsigned from = (g_hist_n > KM_SCAN_DEPTH) ? g_hist_n - KM_SCAN_DEPTH : 0u;
    for (unsigned c = from; c < g_hist_n; c++) {
        sum += g_hist[c][slot];
    }
    return sum;
}

static unsigned model_mismatches(const km_scan_t *s)
{
    unsigned bad = 0;
    const uint16_t *sums = km_scan_sums(s);
    for (unsigned i = 0; i < KM_NUM_SLOTS; i++) {
        if (sums[i] != model_sum(i)) {
            bad++;
        }
    }
    return bad;
}

static uint32_t g_rng = 12345u;
static uint32_t rnd(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

static unsigned count_nonzero(const km_scan_t *s)
{
    unsigned n = 0;
    const uint16_t *sums = km_scan_sums(s);
    for (unsigned i = 0; i < KM_NUM_SLOTS; i++) {
        n += (sums[i] != 0) ? 1u : 0u;
    }
    return n;
}

static void test_scan_basics(void)
{
    km_scan_t s;
    frame_t v;

    km_scan_init(&s);
    CHECK_EQ(count_nonzero(&s), 0);

    /* Constant input: the sum climbs by one sample per cycle, then sits at 16 x value. */
    for (unsigned i = 0; i < KM_NUM_SLOTS; i++) {
        v[i] = (uint16_t)(100u + 50u * i); /* 100..3850, every slot different */
    }
    for (unsigned c = 1; c <= 40; c++) {
        push_cycle(&s, v);
        const unsigned n = (c < KM_SCAN_DEPTH) ? c : KM_SCAN_DEPTH;
        unsigned bad = 0;
        for (unsigned i = 0; i < KM_NUM_SLOTS; i++) {
            bad += (km_scan_sums(&s)[i] != n * v[i]) ? 1u : 0u;
        }
        CHECK_EQ(bad, 0);
    }

    /* Step up, then step down: the new level takes exactly 16 cycles to be fully in the sum. */
    km_scan_init(&s);
    fill(v, 1000);
    for (int c = 0; c < 20; c++) {
        push_cycle(&s, v);
    }
    CHECK_EQ(km_scan_sums(&s)[0], 16000);
    fill(v, 3000);
    for (unsigned j = 1; j <= 20; j++) {
        push_cycle(&s, v);
        const unsigned n = (j < KM_SCAN_DEPTH) ? j : KM_SCAN_DEPTH;
        const unsigned want = (KM_SCAN_DEPTH - n) * 1000u + n * 3000u;
        CHECK_EQ(km_scan_sums(&s)[0], want);
        CHECK_EQ(km_scan_sums(&s)[37], want);
        CHECK_EQ(km_scan_sums(&s)[75], want);
    }
    CHECK_EQ(km_scan_sums(&s)[10], 48000);
    fill(v, 500);
    for (unsigned j = 1; j <= 20; j++) {
        push_cycle(&s, v);
        const unsigned n = (j < KM_SCAN_DEPTH) ? j : KM_SCAN_DEPTH;
        CHECK_EQ(km_scan_sums(&s)[44], (KM_SCAN_DEPTH - n) * 3000u + n * 500u);
    }
    CHECK_EQ(km_scan_sums(&s)[44], 8000);

    /* A single pulse is in the sum for exactly 16 cycles. */
    km_scan_init(&s);
    fill(v, 0);
    v[30] = 2000;
    push_cycle(&s, v);
    v[30] = 0;
    for (unsigned j = 1; j <= 20; j++) {
        CHECK_EQ(km_scan_sums(&s)[30], (j <= KM_SCAN_DEPTH) ? 2000 : 0);
        push_cycle(&s, v);
    }
}

static void test_scan_isolation(void)
{
    km_scan_t s;
    frame_t v;

    /* One live slot among silent ones, and one silent slot among full-scale ones. */
    for (unsigned slot = 0; slot < KM_NUM_SLOTS; slot++) {
        km_scan_init(&s);
        fill(v, 0);
        v[slot] = 1234;
        for (unsigned c = 1; c <= 18; c++) {
            push_cycle(&s, v);
            const unsigned n = (c < KM_SCAN_DEPTH) ? c : KM_SCAN_DEPTH;
            unsigned others = 0;
            for (unsigned i = 0; i < KM_NUM_SLOTS; i++) {
                if (i != slot && km_scan_sums(&s)[i] != 0) {
                    others++;
                }
            }
            CHECK_EQ(others, 0);
            CHECK_EQ(km_scan_sums(&s)[slot], n * 1234u);
        }

        km_scan_init(&s);
        fill(v, 4095);
        v[slot] = 0;
        for (int c = 0; c < 17; c++) {
            push_cycle(&s, v);
        }
        unsigned wrong = 0;
        for (unsigned i = 0; i < KM_NUM_SLOTS; i++) {
            const unsigned want = (i == slot) ? 0u : 16u * 4095u;
            wrong += (km_scan_sums(&s)[i] != want) ? 1u : 0u;
        }
        CHECK_EQ(wrong, 0);
    }
}

static void test_scan_phases(void)
{
    km_scan_t s;
    uint32_t w[KM_SCAN_PHASE_WORDS];
    const uint32_t none[KM_SCAN_PHASE_WORDS] = { 0 };
    frame_t v;

    /* Each phase feeds its own 18 slots and nothing else (9 words, not the vendor's 11). */
    for (unsigned p = 0; p < KM_SCAN_PHASES; p++) {
        km_scan_init(&s);
        for (unsigned k = 0; k < KM_SCAN_PHASE_WORDS; k++) {
            w[k] = 0x0007000Bu + k; /* slot 2w -> 11 + k, slot 2w+1 -> 7 */
        }
        for (unsigned c = 0; c < 3; c++) {
            for (unsigned q = 0; q < KM_SCAN_PHASES; q++) {
                km_scan_push_phase(&s, q, (q == p) ? w : none);
            }
        }
        for (unsigned i = 0; i < KM_NUM_SLOTS; i++) {
            const int mine = (i >= p * 18u && i < p * 18u + 18u);
            const unsigned k = (i / 2u) % KM_SCAN_PHASE_WORDS;
            const unsigned want = mine ? 3u * ((i & 1u) ? 7u : 11u + k) : 0u;
            CHECK_EQ(km_scan_sums(&s)[i], want);
        }
    }

    /* Slots appear in the sums as their phase arrives, within a cycle. */
    km_scan_init(&s);
    fill(v, 10);
    for (unsigned p = 0; p < KM_SCAN_PHASES; p++) {
        for (unsigned k = 0; k < KM_SCAN_PHASE_WORDS; k++) {
            w[k] = pack(v, p * KM_SCAN_PHASE_WORDS + k);
        }
        km_scan_push_phase(&s, p, w);
        CHECK_EQ(count_nonzero(&s), (p + 1u) * 18u);
    }
    CHECK_EQ(count_nonzero(&s), 72);
    km_scan_push_injected(&s, pack(v, 36), pack(v, 37));
    CHECK_EQ(count_nonzero(&s), 76);

    /* A phase number above 3 is ignored (no write, no new cycle). */
    km_scan_t before = s;
    km_scan_push_phase(&s, 4, w);
    km_scan_push_phase(&s, 99, w);
    CHECK(memcmp(&before, &s, sizeof s) == 0);

    /* The ring position advances on phase 0 only: three phases do not age the sums.
     * (Where in the cycle it advances, or where it starts, cannot be seen from outside: every
     * word has its own ring and gets one update per cycle, which is why the vendor's phase-3
     * words may run one position ahead of the others without effect. Mutation runs agree.) */
    km_scan_init(&s);
    fill(v, 100);
    for (int c = 0; c < 16; c++) {
        push_cycle(&s, v);
    }
    CHECK_EQ(km_scan_sums(&s)[0], 1600);
    for (unsigned p = 0; p < KM_SCAN_PHASES; p++) {
        for (unsigned k = 0; k < KM_SCAN_PHASE_WORDS; k++) {
            w[k] = 0;
        }
        km_scan_push_phase(&s, p, w); /* the new cycle's zeros replace the oldest sample */
    }
    CHECK_EQ(km_scan_sums(&s)[0], 1500);
    CHECK_EQ(km_scan_sums(&s)[71], 1500);
    CHECK_EQ(km_scan_sums(&s)[72], 1600); /* injected not pushed yet this cycle */
}

static void test_scan_injected(void)
{
    km_scan_t s;
    frame_t v;
    uint32_t zero[KM_SCAN_PHASE_WORDS] = { 0 };

    /* w36 = ADC2<<16 | ADC1 -> slots 73 (A) and 72 (S); w37 = ADC4<<16 | ADC3 -> 75 (W) and 74 (D). */
    km_scan_init(&s);
    km_scan_push_phase(&s, 0, zero);
    km_scan_push_injected(&s, (0x0222u << 16) | 0x0111u, (0x0444u << 16) | 0x0333u);
    CHECK_EQ(km_scan_sums(&s)[72], 0x111);
    CHECK_EQ(km_scan_sums(&s)[73], 0x222);
    CHECK_EQ(km_scan_sums(&s)[74], 0x333);
    CHECK_EQ(km_scan_sums(&s)[75], 0x444);
    CHECK_EQ(count_nonzero(&s), 4);
    CHECK(km_scan_slot_info(72)->adc == 1 && km_scan_slot_info(73)->adc == 2);
    CHECK(km_scan_slot_info(74)->adc == 3 && km_scan_slot_info(75)->adc == 4);

    /* The four regular phases never touch slots 72..75. */
    km_scan_init(&s);
    fill(v, 4095);
    for (unsigned p = 0; p < KM_SCAN_PHASES; p++) {
        uint32_t w[KM_SCAN_PHASE_WORDS];
        for (unsigned k = 0; k < KM_SCAN_PHASE_WORDS; k++) {
            w[k] = pack(v, p * KM_SCAN_PHASE_WORDS + k);
        }
        km_scan_push_phase(&s, p, w);
    }
    for (unsigned i = 72; i < 76; i++) {
        CHECK_EQ(km_scan_sums(&s)[i], 0);
    }

    /* Once per cycle: a second injected push in the same cycle replaces the first sample. */
    km_scan_init(&s);
    km_scan_push_phase(&s, 0, zero);
    km_scan_push_injected(&s, 1000, 2000);
    km_scan_push_injected(&s, 1500, 2500);
    CHECK_EQ(km_scan_sums(&s)[72], 1500);
    CHECK_EQ(km_scan_sums(&s)[74], 2500);

    /* Injected samples slide through the same 16-cycle window. */
    km_scan_init(&s);
    fill(v, 0);
    for (unsigned c = 1; c <= 20; c++) {
        v[72] = 2000;
        v[75] = (uint16_t)(c * 10u);
        push_cycle(&s, v);
        const unsigned n = (c < KM_SCAN_DEPTH) ? c : KM_SCAN_DEPTH;
        CHECK_EQ(km_scan_sums(&s)[72], n * 2000u);
        unsigned want75 = 0;
        for (unsigned j = c - n + 1u; j <= c; j++) {
            want75 += j * 10u;
        }
        CHECK_EQ(km_scan_sums(&s)[75], want75);
    }
}

static void test_scan_packed(void)
{
    km_scan_t s;
    frame_t v;

    /* Both halves full scale: 16 x 4095 = 0xFFF0 in each half, no carry into the neighbour. */
    km_scan_init(&s);
    fill(v, 4095);
    for (int c = 0; c < 16; c++) {
        push_cycle(&s, v);
    }
    for (unsigned i = 0; i < KM_NUM_SLOTS; i++) {
        CHECK_EQ(km_scan_sums(&s)[i], 0xFFF0);
    }
    CHECK_EQ(s.sum[0], 0xFFF0FFF0u);
    CHECK_EQ(s.sum[37], 0xFFF0FFF0u);
    for (int c = 0; c < 5; c++) {
        push_cycle(&s, v);
    }
    CHECK_EQ(km_scan_sums(&s)[0], 0xFFF0);
    CHECK_EQ(km_scan_sums(&s)[75], 0xFFF0);

    /* Low half falls from full scale to 0 while the high half stays: borrow must not leak. */
    for (unsigned i = 0; i < KM_NUM_SLOTS; i += 2) {
        v[i] = 0;
    }
    for (unsigned j = 1; j <= 16; j++) {
        push_cycle(&s, v);
        CHECK_EQ(km_scan_sums(&s)[0], (16u - j) * 4095u);
        CHECK_EQ(km_scan_sums(&s)[1], 0xFFF0);
        CHECK_EQ(km_scan_sums(&s)[74], (16u - j) * 4095u);
        CHECK_EQ(km_scan_sums(&s)[75], 0xFFF0);
    }
    /* ... and the other way round: high half falls, low half returns to full scale. */
    fill(v, 4095);
    for (unsigned i = 1; i < KM_NUM_SLOTS; i += 2) {
        v[i] = 0;
    }
    for (unsigned j = 1; j <= 16; j++) {
        push_cycle(&s, v);
        CHECK_EQ(km_scan_sums(&s)[0], j * 4095u);
        CHECK_EQ(km_scan_sums(&s)[1], (16u - j) * 4095u);
    }

    /* Alternating extremes against the reference model. */
    km_scan_init(&s);
    model_reset();
    for (unsigned c = 0; c < 100; c++) {
        for (unsigned i = 0; i < KM_NUM_SLOTS; i++) {
            v[i] = (((c + i) & 1u) != 0u) ? 4095 : 0;
        }
        push_cycle(&s, v);
        model_push(v);
        CHECK_EQ(model_mismatches(&s), 0);
    }
}

static void test_scan_random(void)
{
    km_scan_t s;
    frame_t v;

    /* Random 12-bit streams with extremes mixed in, 300 cycles (the ring wraps many times). */
    km_scan_init(&s);
    model_reset();
    for (unsigned c = 0; c < 300; c++) {
        for (unsigned i = 0; i < KM_NUM_SLOTS; i++) {
            const uint32_t r = rnd();
            v[i] = ((r & 7u) == 0u) ? 4095 : ((r & 7u) == 1u) ? 0 : (uint16_t)((r >> 3) & 0xFFFu);
        }
        push_cycle(&s, v);
        model_push(v);
        CHECK_EQ(model_mismatches(&s), 0);
    }
    for (unsigned i = 0; i < KM_NUM_SLOTS; i++) {
        CHECK(km_scan_sums(&s)[i] <= 16u * 4095u);
    }
}

static void test_scan_reset(void)
{
    km_scan_t s;
    frame_t v;

    /* init forgets everything, including the ring position, even in the middle of a cycle. */
    km_scan_init(&s);
    fill(v, 4095);
    for (int c = 0; c < 21; c++) {
        push_cycle(&s, v);
    }
    uint32_t w[KM_SCAN_PHASE_WORDS] = { 0xFFFFFFFFu };
    km_scan_push_phase(&s, 0, w);
    km_scan_push_phase(&s, 1, w);
    km_scan_init(&s);
    CHECK_EQ(count_nonzero(&s), 0);
    km_scan_t fresh;
    km_scan_init(&fresh);
    CHECK(memcmp(&s, &fresh, sizeof s) == 0);

    fill(v, 700);
    for (unsigned c = 1; c <= 20; c++) {
        push_cycle(&s, v);
        CHECK_EQ(km_scan_sums(&s)[0], ((c < 16u) ? c : 16u) * 700u);
        CHECK_EQ(km_scan_sums(&s)[75], ((c < 16u) ? c : 16u) * 700u);
    }
}

/* ---- measured slot map: firmware/core/data/slotmap_measured.json, read next to this source file ---- */
typedef struct {
    char name[KM_NUM_SLOTS][24]; /* key name per slot, "" when none was measured */
    bool nokey[KM_NUM_SLOTS];    /* listed under "no_key_slots" */
} measured_t;

static bool parse_slot_line(const char *p, unsigned *slot, const char **after)
{
    if (*p != '"' || p[1] < '0' || p[1] > '9') {
        return false;
    }
    unsigned n = 0;
    p++;
    while (*p >= '0' && *p <= '9') {
        n = n * 10u + (unsigned)(*p++ - '0');
    }
    if (p[0] != '"' || p[1] != ':' || n >= KM_NUM_SLOTS) {
        return false;
    }
    *slot = n;
    *after = p + 2;
    return true;
}

static bool load_measured(measured_t *m)
{
    char path[512];
    static char text[16384];
    const char *here = __FILE__;
    const char *tests = NULL;
    for (const char *q = here; (q = strstr(q, "tests")) != NULL; q++) {
        tests = q; /* the last "tests" directory of this file's path */
    }
    if (tests == NULL || (size_t)(tests - here) + 40u > sizeof path) {
        return false;
    }
    memcpy(path, here, (size_t)(tests - here));
    strcpy(path + (tests - here), "core/data/slotmap_measured.json");
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("  cannot open %s\n", path);
        return false;
    }
    const size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    memset(m, 0, sizeof *m);

    int section = 0; /* 1 = "slots", 2 = "no_key_slots" */
    for (char *line = text; line != NULL && *line != '\0';) {
        char *next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        unsigned slot;
        const char *rest;
        if (strncmp(line, "\"no_key_slots\"", 14) == 0) {
            section = 2;
        } else if (strncmp(line, "\"slots\"", 7) == 0) {
            section = 1;
        } else if (parse_slot_line(line, &slot, &rest)) {
            if (section == 1) {
                while (*rest == ' ') {
                    rest++;
                }
                if (*rest == '"') {
                    size_t len = 0;
                    for (rest++; *rest != '\0' && *rest != '"' && len < sizeof m->name[0] - 1; rest++) {
                        m->name[slot][len++] = (*rest == '\\' && rest[1] != '\0') ? *++rest : *rest;
                    }
                    m->name[slot][len] = '\0';
                }
            } else if (section == 2) {
                m->nokey[slot] = true;
            }
        }
        line = next;
    }
    return true;
}

static void test_scan_slots(void)
{
    /* Hand-written copy of the hardware's ADC channel lists (rank order), not taken from the table under test. */
    static const uint8_t adc1[6] = { 3, 4, 5, 2, 6, 7 };
    static const uint8_t adc2[6] = { 0, 2, 17, 13, 5, 11 };
    static const uint8_t adc3[3] = { 12, 1, 2 };
    static const uint8_t adc4[3] = { 6, 6, 6 };
    static const uint8_t inj[4] = { 15, 1, 7, 13 };
    static measured_t m;
    unsigned assigned = 0, nokey = 0;

    CHECK(km_scan_slot_info(KM_NUM_SLOTS) == NULL);
    CHECK(km_scan_slot_info(1000) == NULL);
    const bool have_map = load_measured(&m);
    CHECK(have_map);

    for (unsigned slot = 0; slot < KM_NUM_SLOTS; slot++) {
        const km_slot_info_t *si = km_scan_slot_info(slot);
        CHECK(si != NULL);
        if (si == NULL) {
            continue;
        }
        /* key names: exactly the measured map; every other slot is listed as having no key */
        if (have_map) {
            const bool named = m.name[slot][0] != '\0';
            CHECK(named != m.nokey[slot]); /* each slot is in exactly one of the two lists */
            CHECK_EQ(si->assigned, named);
            if (named) {
                CHECK(strcmp(si->key, m.name[slot]) == 0);
                if (strcmp(si->key, m.name[slot]) != 0) {
                    printf("    slot %u: table \"%s\", measured \"%s\"\n", slot, si->key, m.name[slot]);
                }
            } else {
                CHECK(strcmp(si->key, "(unassigned)") == 0);
            }
            nokey += m.nokey[slot] ? 1u : 0u;
        }
        CHECK(si->key != NULL);
        assigned += si->assigned ? 1u : 0u;

        /* channel layout, derived again from the word/half rule */
        const unsigned word = slot / 2u;
        const unsigned half = slot % 2u;
        if (word >= 36u) {
            const unsigned n = half + 2u * (word - 36u);
            CHECK_EQ(si->phase, KM_SCAN_PHASE_INJ);
            CHECK_EQ(si->adc, n + 1u);
            CHECK_EQ(si->rank, 1);
            CHECK_EQ(si->channel, inj[n]);
        } else {
            const unsigned k = word % 9u;
            CHECK_EQ(si->phase, word / 9u);
            if (k < 6u) {
                CHECK_EQ(si->adc, 1u + half);
                CHECK_EQ(si->rank, k + 1u);
                CHECK_EQ(si->channel, half ? adc2[k] : adc1[k]);
            } else {
                CHECK_EQ(si->adc, 3u + half);
                CHECK_EQ(si->rank, k - 5u);
                CHECK_EQ(si->channel, half ? adc4[k - 6u] : adc3[k - 6u]);
            }
            /* ADC4 samples one channel three times: ranks 2 and 3 only mirror rank 1 and carry no key */
            if (si->adc == 4u && si->rank > 1u) {
                CHECK(!si->assigned);
                CHECK_EQ(si->channel, km_scan_slot_info(slot - 2u * (si->rank - 1u))->channel);
            }
        }
    }
    CHECK_EQ(assigned, 61); /* 61 keys of the ANSI 60 % layout were found, one per slot */
    if (have_map) {
        CHECK_EQ(nokey, KM_NUM_SLOTS - 61u);
    }

    /* Spots the bootloader and the S1 step rely on, and the WASD slots. */
    CHECK(strcmp(km_scan_slot_info(18)->key, "ESC") == 0);
    CHECK(km_scan_slot_info(18)->phase == 1 && km_scan_slot_info(18)->adc == 1 && km_scan_slot_info(18)->channel == 3);
    CHECK(strcmp(km_scan_slot_info(5)->key, "LCTRL") == 0);
    CHECK(km_scan_slot_info(5)->phase == 0 && km_scan_slot_info(5)->adc == 2 && km_scan_slot_info(5)->channel == 17);
    CHECK(strcmp(km_scan_slot_info(72)->key, "S") == 0);
    CHECK(strcmp(km_scan_slot_info(73)->key, "A") == 0);
    CHECK(strcmp(km_scan_slot_info(74)->key, "D") == 0);
    CHECK(strcmp(km_scan_slot_info(75)->key, "W") == 0);
    CHECK(!km_scan_slot_info(15)->assigned);
    CHECK(strcmp(km_scan_slot_info(14)->key, "FN") == 0);
}

void test_scan(void)
{
    test_scan_basics();
    test_scan_isolation();
    test_scan_phases();
    test_scan_injected();
    test_scan_packed();
    test_scan_random();
    test_scan_reset();
    test_scan_slots();
}
