/*
 * M3 sensing driver, host side: (1) the pure tables and register encoders (sense_tables.c) against explicit register positions, (2) the REAL sense_hw.c / adc_scan.c against a simulated register file that
 * behaves just enough like the ADC (power-up ready flag, self-clearing calibration and software start, end of
 * conversion) and the DMA/timer. Checks the final register values the stock sensing_init leaves, the order of the
 * power-up steps, that no address outside the sensing peripherals is touched (in particular nothing of the flash
 * controller), and the phase state machine of the interrupt handlers.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "adc_scan.h"
#include "km_test.h"
#include "sense_hw.h"
#include "sense_regs.h"
#include "sense_tables.h"

/* ---------- simulated registers ---------- */

#define SIM_MAX 1024
static struct { uintptr_t addr; uint32_t val; } g_regs[SIM_MAX];
static unsigned g_nregs;
static unsigned g_bad_access, g_feeds;
static unsigned g_cal_done[5];                 /* calibrations completed per ADC */
static unsigned g_cal_without_on;              /* CAL started while ADON was clear */
static unsigned g_conv_without_on;             /* conversion started while ADON was clear */
static unsigned g_order_violations;            /* conversion before the ADC was calibrated */
static bool g_calibrated[5];
static uintptr_t g_last_bad;
static uint64_t g_sim_cycles;                  /* simulated time: every register access costs SIM_ACCESS_CYCLES */
#define SIM_ACCESS_CYCLES 8u
static unsigned g_fail_rdy, g_fail_cal, g_fail_eoc;   /* bit n-1: ADC n never ready / never finishes calibrating / never ends a conversion */
static unsigned g_need_bit19;                  /* bit n-1: ADC n only becomes ready when RCC_CFG2 bit 19 is set */
static unsigned g_cal_order[16], g_ncal_order;
static uint32_t g_nvic_acc[4];                 /* ISER0, ISER1, ICER0, ICER1: write-1-to-set/clear, so accumulate */

static bool addr_allowed(uintptr_t a)
{
    return (a >= 0x40021000u && a < 0x40021100u)       /* RCC */
        || (a >= 0x40010800u && a < 0x40010840u)       /* AFIO */
        || a == 0x4001004Cu                            /* unnamed register the bootloader and stock set bit 13 of */
        || (a >= 0x4002C000u && a < 0x4002D800u)       /* GPIOA-F */
        || (a >= 0x4002B000u && a < 0x4002C000u)       /* ADC1-4 */
        || (a >= 0x40020000u && a < 0x40020100u)       /* DMA1 */
        || (a >= 0x40002C00u && a < 0x40002D00u)       /* GTIM1 */
        || (a >= 0xE000E100u && a < 0xE000E500u)       /* NVIC set/clear enable, priority */
        || a == 0x20026F08u                            /* progress marker word */
        || a == 0xE000EDFCu                            /* DEMCR */
        || (a >= 0xE0001000u && a < 0xE0001008u);      /* DWT CTRL, CYCCNT */
}

static uint32_t *slot(uintptr_t a)
{
    for (unsigned i = 0; i < g_nregs; i++) {
        if (g_regs[i].addr == a) {
            return &g_regs[i].val;
        }
    }
    if (g_nregs >= SIM_MAX) {
        printf("sim register table full\n");
        exit(2);
    }
    g_regs[g_nregs].addr = a;
    g_regs[g_nregs].val = 0;
    return &g_regs[g_nregs++].val;
}

static int adc_of(uintptr_t a)
{
    if (a >= 0x4002B000u && a < 0x4002C000u) {
        return (int)((a - 0x4002B000u) / 0x400u) + 1;
    }
    return 0;
}

uint32_t *km_sense_sim_reg32(uintptr_t a)
{
    if (!addr_allowed(a)) {
        g_bad_access++;
        g_last_bad = a;
    }
    const int n = adc_of(a);
    if (n != 0) {
        const uintptr_t b = 0x4002B000u + 0x400u * (uintptr_t)(n - 1);
        const unsigned off = (unsigned)(a - b);
        uint32_t *ctrl2 = slot(b + 8u), *sts = slot(b + 0u);
        if (off == 0u || off == 0x08u) {               /* look at STS or CTRL2: finish a pending action */
            const unsigned bit = 1u << (n - 1);
            const bool alive = (*ctrl2 & SENSE_ADC_CTRL2_ADON) && !(g_fail_rdy & bit)
                               && !((g_need_bit19 & bit) && !(*slot(0x40021008u) & 0x80000u));
            if (*ctrl2 & SENSE_ADC_CTRL2_CAL) {
                if (!(*ctrl2 & SENSE_ADC_CTRL2_ADON)) {
                    g_cal_without_on++;
                }
                if (!(g_fail_cal & bit) && alive) {
                    *ctrl2 &= ~SENSE_ADC_CTRL2_CAL;
                    g_cal_done[n]++;
                    g_calibrated[n] = true;
                    if (g_ncal_order < 16u) {
                        g_cal_order[g_ncal_order++] = (unsigned)n;
                    }
                }
            }
            if (*ctrl2 & SENSE_ADC_CTRL2_SWSTRT) {
                if (!(*ctrl2 & SENSE_ADC_CTRL2_ADON)) {
                    g_conv_without_on++;
                }
                if (!g_calibrated[n] && alive && !(g_fail_cal & bit)) {
                    g_order_violations++;
                }
                *ctrl2 &= ~SENSE_ADC_CTRL2_SWSTRT;
                if (alive && !(g_fail_eoc & bit)) {
                    *sts |= SENSE_ADC_STS_EOC;
                    const uint32_t ch = *slot(b + 0x64u) & 0x1Fu;
                    *slot(b + 0x84u) = 0x100u * (uint32_t)n + ch;
                }
            }
            if (alive) {
                *sts |= SENSE_ADC_STS_RDY;
            } else {
                *sts &= ~SENSE_ADC_STS_RDY;
            }
        }
    }
    g_sim_cycles += SIM_ACCESS_CYCLES;
    if (a == 0xE0001004u) {
        *slot(a) = (uint32_t)g_sim_cycles;             /* the cycle counter follows the simulated time */
    }
    if (a == 0xE000E100u || a == 0xE000E104u || a == 0xE000E180u || a == 0xE000E184u) {
        const unsigned i = (unsigned)((a - 0xE000E100u) / 4u);
        const unsigned idx = (a >= 0xE000E180u) ? 2u + (i - 0x20u) : i;
        g_nvic_acc[idx] |= *slot(a);
        *slot(a) = 0;
    }
    return slot(a);
}

void km_sense_sim_feed(void)
{
    g_feeds++;
}

static uint32_t reg(uintptr_t a)
{
    for (unsigned i = 0; i < g_nregs; i++) {
        if (g_regs[i].addr == a) {
            return g_regs[i].val;
        }
    }
    return 0;
}

static uint32_t nvic_acc(unsigned idx)
{
    static const uintptr_t a[4] = { 0xE000E100u, 0xE000E104u, 0xE000E180u, 0xE000E184u };
    (void)*km_sense_sim_reg32(a[idx]);                 /* folds the last write into the accumulator */
    return g_nvic_acc[idx];
}

static bool touched(uintptr_t lo, uintptr_t hi)
{
    for (unsigned i = 0; i < g_nregs; i++) {
        if (g_regs[i].addr >= lo && g_regs[i].addr < hi) {
            return true;
        }
    }
    return false;
}

static void sim_reset(void)
{
    memset(g_regs, 0, sizeof g_regs);
    g_nregs = 0;
    g_bad_access = g_feeds = g_cal_without_on = g_conv_without_on = g_order_violations = 0;
    memset(g_cal_done, 0, sizeof g_cal_done);
    memset(g_calibrated, 0, sizeof g_calibrated);
    memset(g_nvic_acc, 0, sizeof g_nvic_acc);
    g_sim_cycles = 0;
    g_fail_rdy = g_fail_cal = g_fail_eoc = g_need_bit19 = 0;
    g_ncal_order = 0;
    sense_set_idle_hook(0);
    *slot(0x40021008u) = 0x7000u;                      /* RCC_CFG2 after the bootloader's SystemInit */
    memset(g_phase_words, 0, sizeof g_phase_words);
    g_inj[0] = g_inj[1] = 0;
}

static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

static void test_tables(void)
{
    /* rows of the channel table in SENSING_BRINGUP_NOTES.md section 1.2 */
    sense_slot_src_t s;
    CHECK(sense_slot_source(0, &s) && s.phase == 0 && s.adc == 1 && s.rank == 1 && s.channel == 3);
    CHECK(sense_slot_source(1, &s) && s.phase == 0 && s.adc == 2 && s.rank == 1 && s.channel == 0);
    CHECK(sense_slot_source(5, &s) && s.phase == 0 && s.adc == 2 && s.rank == 3 && s.channel == 17);   /* left CTRL */
    CHECK(sense_slot_source(12, &s) && s.phase == 0 && s.adc == 3 && s.rank == 1 && s.channel == 12);
    CHECK(sense_slot_source(13, &s) && s.phase == 0 && s.adc == 4 && s.rank == 1 && s.channel == 6);
    CHECK(sense_slot_source(17, &s) && s.phase == 0 && s.adc == 4 && s.rank == 3 && s.channel == 6);
    CHECK(sense_slot_source(18, &s) && s.phase == 1 && s.adc == 1 && s.rank == 1 && s.channel == 3);   /* ESC */
    CHECK(sense_slot_source(29, &s) && s.phase == 1 && s.adc == 2 && s.rank == 6 && s.channel == 11);
    CHECK(sense_slot_source(71, &s) && s.phase == 3 && s.adc == 4 && s.rank == 3 && s.channel == 6);
    CHECK(sense_slot_source(72, &s) && s.phase == 4 && s.adc == 1 && s.channel == 15);                 /* S */
    CHECK(sense_slot_source(73, &s) && s.phase == 4 && s.adc == 2 && s.channel == 1);                  /* A */
    CHECK(sense_slot_source(74, &s) && s.phase == 4 && s.adc == 3 && s.channel == 7);                  /* D */
    CHECK(sense_slot_source(75, &s) && s.phase == 4 && s.adc == 4 && s.channel == 13);                 /* W */
    CHECK(!sense_slot_source(76, &s));

    /* slot_of is a bijection onto 0..71 and agrees with slot_source */
    bool seen[SENSE_NUM_SLOTS] = { false };
    for (unsigned p = 0; p < 4; p++) {
        for (unsigned w = 0; w < 9; w++) {
            for (unsigned h = 0; h < 2; h++) {
                const unsigned sl = sense_slot_of(p, w, h);
                CHECK(sl < 72u && !seen[sl]);
                seen[sl] = true;
                CHECK(sense_slot_source(sl, &s) && s.phase == p && s.adc == (w < 6 ? 1 + h : 3 + h));
            }
        }
    }

    /* mux */
    CHECK_EQ(sense_mux_bits(0), 0x00);
    CHECK_EQ(sense_mux_bits(1), 0x08);
    CHECK_EQ(sense_mux_bits(2), 0x10);
    CHECK_EQ(sense_mux_bits(3), 0x18);
    CHECK_EQ(sense_mux_apply(0xFFFFu, 1), 0xFFEFu);          /* other outputs untouched, PB4 cleared, PB3 set */
    CHECK_EQ(sense_mux_apply(0x0020u, 3), 0x0038u);          /* PB5 kept */

    /* word packing: high half = ADC2/ADC4, low half = ADC1/ADC3 */
    uint32_t pw[4][9];
    uint32_t inj[2] = { 0x0222u << 16 | 0x0111u, 0x0444u << 16 | 0x0333u };
    for (unsigned p = 0; p < 4; p++) {
        for (unsigned w = 0; w < 9; w++) {
            pw[p][w] = ((uint32_t)(0x800u + p * 16u + w) << 16) | (p * 16u + w);
        }
    }
    uint16_t v[76];
    sense_unpack_all((const uint32_t (*)[9])pw, inj, v);
    CHECK_EQ(v[0], 0);
    CHECK_EQ(v[1], 0x800);
    CHECK_EQ(v[2], 1);
    CHECK_EQ(v[18], 16);                                     /* phase 1, word 0, ADC1 half */
    CHECK_EQ(v[19], 0x810);
    CHECK_EQ(v[sense_slot_of(3, 8, 1)], 0x800 + 3 * 16 + 8);
    CHECK_EQ(v[72], 0x111);
    CHECK_EQ(v[73], 0x222);
    CHECK_EQ(v[74], 0x333);
    CHECK_EQ(v[75], 0x444);

    /* encoders: explicit register positions (5-bit sequence fields, six per register; 4-bit sample-time fields) */
    CHECK_EQ(sense_rseq_index(1), 0);  CHECK_EQ(sense_rseq_shift(1), 0);
    CHECK_EQ(sense_rseq_index(6), 0);  CHECK_EQ(sense_rseq_shift(6), 25);
    CHECK_EQ(sense_rseq_index(7), 1);  CHECK_EQ(sense_rseq_shift(7), 0);
    CHECK_EQ(sense_rseq_index(12), 1); CHECK_EQ(sense_rseq_shift(12), 25);
    CHECK_EQ(sense_rseq_index(13), 2); CHECK_EQ(sense_rseq_shift(13), 0);
    CHECK_EQ(sense_rseq_index(16), 2); CHECK_EQ(sense_rseq_shift(16), 15);
    CHECK_EQ(sense_sampt_index(0), 0);  CHECK_EQ(sense_sampt_shift(7), 28);
    CHECK_EQ(sense_sampt_index(8), 1);  CHECK_EQ(sense_sampt_shift(8), 0);
    CHECK_EQ(sense_sampt_index(15), 1); CHECK_EQ(sense_sampt_shift(15), 28);
    CHECK_EQ(sense_sampt_index(16), 2); CHECK_EQ(sense_sampt_shift(17), 4);
    CHECK_EQ(sense_rseq_len_apply(0xFFFFFFFFu, 6), 0xE1FFFFFFu | (5u << 25));
    CHECK_EQ(sense_jseq_len_apply(0, 4), 3u << 25);
    CHECK_EQ(sense_jseq_apply(0, 1, 15), 15u << 15);                   /* one conversion: sits at [19:15] */
    CHECK_EQ(sense_jseq_apply(3u << 25, 4, 9), (3u << 25) | (9u << 15));   /* four: the last one sits at [19:15] ... */
    CHECK_EQ(sense_jseq_apply(3u << 25, 1, 9), (3u << 25) | (9u << 0));    /* ... the first at [4:0] */

    /* property: applying a field sets exactly that field and leaves every other bit alone */
    uint32_t seed = 12345;
    for (int iter = 0; iter < 2000; iter++) {
        const uint32_t before = lcg(&seed);
        const unsigned ch = lcg(&seed) % 18u, rank = 1u + lcg(&seed) % 16u, smp = lcg(&seed) % 16u;
        const uint32_t r = sense_rseq_apply(before, rank, ch);
        const unsigned sh = sense_rseq_shift(rank);
        CHECK_EQ((r >> sh) & 0x1Fu, ch);
        CHECK_EQ(r & ~(0x1Fu << sh), before & ~(0x1Fu << sh));
        const uint32_t q = sense_sampt_apply(before, ch, smp);
        const unsigned qs = sense_sampt_shift(ch);
        CHECK_EQ((q >> qs) & 0xFu, smp);
        CHECK_EQ(q & ~(0xFu << qs), before & ~(0xFu << qs));
    }
    CHECK_EQ(sense_dma_chcfg_value(), 0x3AA0);
}

static uint32_t rseq1(const uint8_t *chs, unsigned n)
{
    uint32_t r = 0;
    for (unsigned i = 0; i < n; i++) {
        r |= (uint32_t)chs[i] << (5u * i);
    }
    return r;
}

static void test_poll(void)
{
    sim_reset();
    const uint32_t err = sense_poll_init(0x3u);
    CHECK_EQ(err, 0);
    CHECK_EQ(g_bad_access, 0);
    CHECK_EQ(g_cal_done[1], 1);
    CHECK_EQ(g_cal_done[2], 1);
    CHECK_EQ(g_cal_done[3], 0);
    CHECK_EQ(g_cal_without_on, 0);
    CHECK_EQ(reg(0x4002B008u), 0x002A0000u | 1u);            /* ADC1 CTRL2: bootloader mode word + ADON, CAL finished */
    CHECK_EQ(reg(0x4002B408u), 0x002A0000u | 1u);
    CHECK_EQ(reg(0x4002B004u), 0);                           /* CTRL1 */
    CHECK_EQ(reg(0x4002B00Cu), 3);                           /* CTRL3: RES = 3, clock-select bit 4 clear */
    CHECK_EQ(reg(0x4002B090u), 0x28);                        /* PCTRL */
    CHECK_EQ(reg(0x4002B06Cu), 0);                           /* RSEQ3: one conversion -> LEN = 0 */
    CHECK_EQ(reg(0x40021008u), 0x7008u);                     /* CFG2: [3:0] = 8, [8:4] = 0, 0x7000 */
    CHECK_EQ(reg(0x4001004Cu), 0x2000u);
    CHECK_EQ(reg(0x40010800u), 0x8000u);                     /* SWJ_CFG field 2 as the stock app */
    CHECK_EQ(reg(0x40021000u + 0x3Cu), 0x3Fu | 0x600u);      /* GPIOA-F, ADC1, ADC2 */
    CHECK_EQ(reg(0x40021024u), 0);                           /* reset pulse released again */
    /* pins: PB3/PB4/PB5 outputs, PA0 and PA4 analog */
    CHECK_EQ((reg(0x4002C400u) >> 6) & 0x3Fu, 0x15u);        /* PB3, PB4, PB5 = mode 01 */
    CHECK_EQ(reg(0x4002C000u) & 3u, 3u);
    CHECK_EQ((reg(0x4002C000u) >> 8) & 3u, 3u);
    CHECK_EQ(reg(0x4002C428u), 0x38u);                       /* PBC: levels low first */
    CHECK_EQ(reg(0x40010814u) | reg(0x40010818u) | reg(0x4001081Cu), 0);

    /* the bootloader's reads */
    sense_mux_set(1);
    CHECK_EQ(sense_mux_get(), 1);
    CHECK_EQ(reg(0x4002C430u) & 0x38u, 0x08u);               /* PB3 high, PB4 and PB5 low */
    CHECK_EQ(sense_poll_convert(1, 3), 0x100 + 3);
    CHECK_EQ(sense_poll_convert(2, 17), 0x200 + 17);
    CHECK_EQ(reg(0x4002B410u), 0);                           /* ADC2 SAMPT1 untouched by ch 17 */
    CHECK_EQ(reg(0x4002B418u), 12u << 4);                    /* ch17: SAMPT3 field 1 = code 12 */
    CHECK_EQ(reg(0x4002B464u) & 0x1Fu, 17);                  /* rank 1 = ch17 */
    CHECK_EQ(g_conv_without_on, 0);
    CHECK_EQ(g_order_violations, 0);
    CHECK_EQ(g_bad_access, 0);
    CHECK(g_feeds > 0);
    /* the flash controller and the bootloader window are never touched */
    CHECK(!touched(0x40022000u, 0x40022100u));
    CHECK(!touched(0x08000000u, 0x08008000u));
}

static void test_scan_init(void)
{
    sim_reset();
    const uint32_t err = adc_scan_init();
    CHECK_EQ(err, 0);
    CHECK_EQ(g_bad_access, 0);
    CHECK(!touched(0x40022000u, 0x40022100u));
    for (unsigned n = 1; n <= 4; n++) {
        CHECK_EQ(g_cal_done[n], 1);
    }
    CHECK_EQ(g_cal_without_on, 0);

    /* clocks */
    CHECK_EQ(reg(0x4002103Cu), 0x3Fu | 0x1E00u);
    CHECK_EQ(reg(0x40021018u), 0x80u);
    CHECK_EQ(reg(0x4002101Cu), 0x400u);
    CHECK_EQ(reg(0x40021008u), 0x87150u);
    CHECK_EQ(reg(0x40021024u), 0);

    /* ADC registers as the stock sensing_init leaves them (all four ADCs) */
    for (unsigned n = 1; n <= 4; n++) {
        const uintptr_t b = 0x4002B000u + 0x400u * (n - 1u);
        CHECK_EQ(reg(b + 4u), 0x81);
        CHECK_EQ(reg(b + 8u), ((n & 1u) ? 0x892450u : 0x092450u) | 1u);
        CHECK_EQ(reg(b + 0xCu), 0x13);
        CHECK_EQ(reg(b + 0x90u), 0x28);
        CHECK_EQ(reg(b + 0x98u), (n & 1u) ? 4u : 0u);
        CHECK_EQ(reg(b + 0x70u), (uint32_t)sense_injected_ch[n - 1u] << 15);
        CHECK_EQ(reg(b + 0x64u), rseq1(sense_regular_ch[n - 1u], sense_regular_len[n - 1u]));
        CHECK_EQ(reg(b + 0x6Cu), (uint32_t)(sense_regular_len[n - 1u] - 1u) << 25);
    }
    CHECK_EQ(reg(0x4002B010u), 0x55555500u);                 /* ADC1 SAMPT1: ch2..7 = 5 */
    CHECK_EQ(reg(0x4002B014u), 0x50000000u);                 /* ADC1 SAMPT2: injected ch15 = 5 */
    CHECK_EQ(reg(0x4002B418u), 5u << 4);                     /* ADC2 ch17 */

    /* DMA */
    const uint32_t m0 = (uint32_t)(uintptr_t)&g_phase_words[0][0], m6 = (uint32_t)(uintptr_t)&g_phase_words[0][6];
    CHECK_EQ(reg(0x40020000u), 0x3AA3);
    CHECK_EQ(reg(0x40020004u), m0);
    CHECK_EQ(reg(0x40020008u), 0x4002B084u);
    CHECK_EQ(reg(0x4002000Cu), 0);                           /* CHSEL1 */
    CHECK_EQ(reg(0x40020010u), 6);
    CHECK_EQ(reg(0x40020028u), 0x3AA3);
    CHECK_EQ(reg(0x4002002Cu), m6);
    CHECK_EQ(reg(0x40020030u), 0x4002B884u);
    CHECK_EQ(reg(0x40020034u), 2);                           /* CHSEL3 */
    CHECK_EQ(reg(0x40020038u), 3);
    CHECK_EQ(reg(0x400200A8u), 0);

    /* GTIM1: configured, not running */
    CHECK_EQ(reg(0x40002C40u), 0x17);
    CHECK_EQ(reg(0x40002C44u), 0xFFFE);
    CHECK_EQ(reg(0x40002C34u), 7);
    CHECK_EQ(reg(0x40002C0Cu), 0x100);
    CHECK_EQ(reg(0x40002C1Cu), 0xC000);
    CHECK_EQ(reg(0x40002C24u), 0xC000);
    CHECK_EQ(reg(0x40002C00u) & 1u, 0);
}

static void irq_phase(void)
{
    DMA1_Channel1_IRQHandler();
    DMA1_Channel3_IRQHandler();
}

static void test_scan_run(void)
{
    sim_reset();
    CHECK_EQ(adc_scan_init(), 0);
    adc_scan_start();
    CHECK(!adc_scan_frame_ready());
    CHECK_EQ(reg(0x40002C00u) & 1u, 1);                      /* timer running: first trigger */
    CHECK_EQ(reg(0x4002C430u) & 0x18u, 0);                   /* mux phase 0 */
    CHECK_EQ(reg(0x40020004u), (uint32_t)(uintptr_t)&g_phase_words[0][0]);

    for (unsigned p = 0; p < 3; p++) {
        irq_phase();
        CHECK(!adc_scan_frame_ready());
        CHECK_EQ(adc_scan_phase_mask(), (1u << (p + 1u)) - 1u);
        CHECK_EQ((reg(0x4002C430u) & 0x18u) >> 3, p + 1u);  /* mux advanced before the next trigger */
        CHECK_EQ(reg(0x40020004u), (uint32_t)(uintptr_t)&g_phase_words[p + 1][0]);
        CHECK_EQ(reg(0x4002002Cu), (uint32_t)(uintptr_t)&g_phase_words[p + 1][6]);
        CHECK_EQ(reg(0x40020010u), 6);
        CHECK_EQ(reg(0x40020038u), 3);
        CHECK_EQ(reg(0x40020000u) & 1u, 1);                  /* DMA re-armed */
        CHECK_EQ(reg(0x40002C00u) & 1u, 1);                  /* timer restarted, CNT cleared */
        CHECK_EQ(reg(0x40002C48u), 0);
    }
    irq_phase();                                             /* phase 3 done: frame ready, timer stays off */
    CHECK(adc_scan_frame_ready());
    CHECK_EQ(adc_scan_phase_mask(), 0xF);
    CHECK_EQ(reg(0x40002C00u) & 1u, 0);
    CHECK_EQ(reg(0x4002C430u) & 0x18u, 0);                   /* back to phase 0 */
    CHECK_EQ(reg(0x40020004u), (uint32_t)(uintptr_t)&g_phase_words[0][0]);

    /* injected interrupts */
    *slot(0x4002B074u) = 0x111; *slot(0x4002B474u) = 0x222; *slot(0x4002B874u) = 0x333; *slot(0x4002BC74u) = 0x444;
    ADC1_2_IRQHandler();
    ADC3_4_IRQHandler();
    CHECK_EQ(g_inj[0], (0x222u << 16) | 0x111u);
    CHECK_EQ(g_inj[1], (0x444u << 16) | 0x333u);

    adc_scan_stats_t st;
    adc_scan_get_stats(&st);
    CHECK_EQ(st.phases, 4);
    CHECK_EQ(st.frames, 1);
    CHECK_EQ(st.overruns, 0);
    CHECK_EQ(st.dma_errors, 0);
    CHECK_EQ(st.inj_events, 2);
    CHECK(st.frame_cycles > 0 && st.frame_cycles_min == st.frame_cycles && st.frame_cycles_max == st.frame_cycles);

    adc_scan_release_frame();                                /* next frame starts */
    CHECK(!adc_scan_frame_ready());
    CHECK_EQ(adc_scan_phase_mask(), 0);
    CHECK_EQ(reg(0x40002C00u) & 1u, 1);

    /* an unpaired completion is counted, a transfer-error flag is counted */
    DMA1_Channel1_IRQHandler();
    DMA1_Channel1_IRQHandler();
    adc_scan_get_stats(&st);
    CHECK_EQ(st.overruns, 1);
    DMA1_Channel3_IRQHandler();
    *slot(0x400200A4u) = SENSE_DMA_TEIF_CH3;                 /* error flag set while the next phase completes */
    DMA1_Channel1_IRQHandler();
    DMA1_Channel3_IRQHandler();
    adc_scan_get_stats(&st);
    CHECK_EQ(st.dma_errors, 1);

    adc_scan_stop();
    CHECK_EQ(reg(0x40002C00u) & 1u, 0);
    CHECK_EQ(g_bad_access, 0);
    CHECK(!touched(0x40022000u, 0x40022100u));
    CHECK(!touched(0x08000000u, 0x08008000u));
    /* interrupt numbers: ISER0 gets IRQ11, 13, 18; ISER1 gets IRQ47 (bit 15) */
    CHECK_EQ(nvic_acc(0), (1u << 11) | (1u << 13) | (1u << 18));      /* enabled */
    CHECK_EQ(nvic_acc(1), 1u << 15);
    CHECK_EQ(nvic_acc(2), (1u << 11) | (1u << 13) | (1u << 18));      /* disabled again by adc_scan_stop */
    CHECK_EQ(nvic_acc(3), 1u << 15);
    CHECK_EQ(reg(0xE000E400u + 8u) >> 24, 0x20u);                     /* IRQ11 priority byte lives in word 2, byte 3 */
}

/* ---------- M5 T-E: frame timeout, DMA error hand-over, restart of a stalled chain ---------- */

static void test_scan_failsafe(void)
{
    adc_scan_stats_t st;
    sim_reset();
    CHECK_EQ(adc_scan_init(), 0);
    CHECK(!adc_scan_frame_timeout(sense_cycles() + 10u * ADC_SCAN_FRAME_TIMEOUT_CYCLES));   /* not running: no timeout */
    adc_scan_start();
    const uint32_t cyc = sense_cycles();
    CHECK(!adc_scan_frame_timeout(cyc));
    CHECK(!adc_scan_frame_timeout(cyc + ADC_SCAN_FRAME_TIMEOUT_CYCLES - 10000u));
    CHECK(adc_scan_frame_timeout(cyc + ADC_SCAN_FRAME_TIMEOUT_CYCLES + 10000u));

    /* a completed frame is never a timeout, however long the consumer takes */
    for (unsigned p = 0; p < 4; p++) {
        irq_phase();
    }
    CHECK(adc_scan_frame_ready());
    CHECK(!adc_scan_frame_timeout(cyc + 100u * ADC_SCAN_FRAME_TIMEOUT_CYCLES));
    /* the clock restarts at the release */
    adc_scan_release_frame();
    const uint32_t cyc2 = sense_cycles();
    CHECK(!adc_scan_frame_timeout(cyc2 + ADC_SCAN_FRAME_TIMEOUT_CYCLES - 10000u));
    CHECK(adc_scan_frame_timeout(cyc2 + ADC_SCAN_FRAME_TIMEOUT_CYCLES + 10000u));
    /* wrap of the cycle counter */
    CHECK(!adc_scan_frame_timeout(cyc2 - 100000u));

    /* DMA error handed over exactly once */
    CHECK_EQ(adc_scan_take_dma_errors(), 0);
    DMA1_Channel1_IRQHandler();
    *slot(0x400200A4u) = SENSE_DMA_TEIF_CH1;
    DMA1_Channel3_IRQHandler();
    CHECK_EQ(adc_scan_take_dma_errors(), 1);
    CHECK_EQ(adc_scan_take_dma_errors(), 0);

    /* a stall: half of a phase completed, then nothing. restart() must forget the half phase and start frame 0 again */
    DMA1_Channel1_IRQHandler();
    CHECK_EQ(adc_scan_phase_mask(), 1);                           /* phase 0 done (the pair above), phase 1 half done */
    *slot(0x400200A4u) = SENSE_DMA_TEIF_CH3;                      /* the chain stopped with a transfer error */
    *slot(0x40020000u) = 0;                                       /* ch1 config as left by a wedged channel */
    const unsigned bad_before = g_bad_access;
    adc_scan_restart(cyc2 + 5u);
    CHECK_EQ(g_bad_access, bad_before);
    CHECK_EQ(adc_scan_take_dma_errors(), 1);                      /* the error flag that explains the stall is reported */
    *slot(0x400200A4u) = 0;                                       /* real hardware clears it through INTCLR, the simulation does not */
    adc_scan_get_stats(&st);
    CHECK_EQ(st.restarts, 1);
    CHECK(!adc_scan_frame_ready());
    CHECK_EQ(adc_scan_phase_mask(), 0);
    CHECK_EQ(reg(0x40002C00u) & 1u, 1);                           /* timer running */
    CHECK_EQ(reg(0x4002C430u) & 0x18u, 0);                        /* mux phase 0 */
    CHECK_EQ(reg(0x40020004u), (uint32_t)(uintptr_t)&g_phase_words[0][0]);
    CHECK_EQ(reg(0x4002002Cu), (uint32_t)(uintptr_t)&g_phase_words[0][6]);
    CHECK_EQ(reg(0x40020010u), 6);
    CHECK_EQ(reg(0x40020038u), 3);
    CHECK_EQ(reg(0x40020000u) & 3u, 3);                           /* channel 1 enabled with its transfer-complete interrupt */
    CHECK_EQ(reg(0x40020028u) & 3u, 3);                           /* ... and channel 3 */
    CHECK_EQ(reg(0xE000E280u), (1u << 11) | (1u << 13) | (1u << 18));   /* pending interrupts cleared */
    CHECK_EQ(reg(0xE000E284u), 1u << 15);
    CHECK(!adc_scan_frame_timeout(cyc2 + 5u + ADC_SCAN_FRAME_TIMEOUT_CYCLES - 10000u));    /* the clock was re-armed with `now` */
    CHECK(adc_scan_frame_timeout(cyc2 + 5u + ADC_SCAN_FRAME_TIMEOUT_CYCLES + 1u));
    /* the next frame completes normally from phase 0 */
    for (unsigned p = 0; p < 4; p++) {
        CHECK(!adc_scan_frame_ready());
        irq_phase();
    }
    CHECK(adc_scan_frame_ready());
    CHECK_EQ(adc_scan_phase_mask(), 0xF);

    /* the stall test hook: the ADC1+2 DMA channel is switched off, the frame can no longer complete, the timeout sees it, restart repairs it */
    adc_scan_release_frame();
    adc_scan_test_stall();
    CHECK_EQ(reg(0x40020000u) & 1u, 0);
    CHECK(adc_scan_frame_timeout(sense_cycles() + ADC_SCAN_FRAME_TIMEOUT_CYCLES + 10000u));
    adc_scan_restart(sense_cycles());
    CHECK_EQ(reg(0x40020000u) & 1u, 1);
    CHECK(!adc_scan_frame_timeout(sense_cycles()));

    /* restarting a healthy running chain is allowed too */
    adc_scan_release_frame();
    adc_scan_restart(sense_cycles());
    adc_scan_get_stats(&st);
    CHECK_EQ(st.restarts, 3);
    CHECK_EQ(adc_scan_take_dma_errors(), 0);
    CHECK(!touched(0x40022000u, 0x40022100u));                   /* the flash controller is never touched */
    adc_scan_stop();
}

/* ---------- main-loop latency: the polled USB driver must be serviced while the sensing code waits ---------- */

static unsigned g_hook_calls;
static uint64_t g_hook_last, g_hook_max_gap;

static void test_hook(void)
{
    const uint64_t gap = g_sim_cycles - g_hook_last;
    if (gap > g_hook_max_gap) {
        g_hook_max_gap = gap;
    }
    g_hook_last = g_sim_cycles;
    g_hook_calls++;
}

static void hook_arm(void)
{
    g_hook_calls = 0;
    g_hook_max_gap = 0;
    g_hook_last = g_sim_cycles;
    sense_set_idle_hook(test_hook);
}

/* Longest allowed time without a USB poll: the 50 us idle period plus the longest stretch of straight-line register code
 * (a few hundred accesses). The first S2 build had an unbounded gap here: dead ADCs cost ~25 ms per conversion. */
#define GAP_LIMIT_US   100u
#define US(cycles)     ((unsigned)((cycles) / SENSE_CYCLES_PER_US))

static void test_s2_latency(void)
{
    uint16_t v[76];
    sense_poll_status_t st;

    /* A: everything healthy. Init order ADC1..4, every calibration after its power-up. */
    sim_reset();
    hook_arm();
    CHECK_EQ(sense_poll_init(0xFu), 0);
    sense_poll_status(&st);
    CHECK_EQ(st.ok_mask, 0xF);
    CHECK_EQ(g_ncal_order, 4);
    for (unsigned i = 0; i < 4 && i < g_ncal_order; i++) {
        CHECK_EQ(g_cal_order[i], i + 1);
    }
    CHECK_EQ(g_cal_without_on, 0);
    uint64_t t0 = g_sim_cycles;
    sense_poll_scan(v, 200);
    const unsigned scan_us = US(g_sim_cycles - t0);
    CHECK(scan_us >= 800 && scan_us < 1500);                 /* 4 x 200 us settle plus conversions */
    CHECK_EQ(v[0], 0x100 + 3);                               /* slot 0: ADC1 ch3 */
    CHECK_EQ(v[1], 0x200 + 0);                               /* slot 1: ADC2 ch0 */
    CHECK_EQ(v[12], 0x300 + 12);                             /* slot 12: ADC3 ch12 */
    CHECK_EQ(v[13], 0x400 + 6);                              /* slot 13: ADC4 ch6 */
    CHECK_EQ(v[72], 0x100 + 15);
    CHECK_EQ(v[73], 0x200 + 1);
    CHECK_EQ(v[74], 0x300 + 7);
    CHECK_EQ(v[75], 0x400 + 13);
    CHECK(g_hook_calls > 10);
    CHECK(US(g_hook_max_gap) <= GAP_LIMIT_US);
    CHECK_EQ(g_conv_without_on, 0);
    CHECK_EQ(g_order_violations, 0);
    CHECK_EQ(g_bad_access, 0);

    /* B: ADC3 and ADC4 never become ready (the suspected cause of the failed S2). Init and scan stay short and USB stays
     * serviced; their slots read 0xFFFF; the alternative-clock retry fails and is undone. */
    sim_reset();
    g_fail_rdy = 0xC;
    hook_arm();
    t0 = g_sim_cycles;
    const uint32_t err_b = sense_poll_init(0xFu);
    CHECK(err_b & SENSE_ERR_RDY_TIMEOUT);
    CHECK(US(g_sim_cycles - t0) < 1000u + 2u * 5000u + 500u); /* reset pulse + two RDY timeouts */
    sense_poll_status(&st);
    CHECK_EQ(st.ok_mask, 0x3);
    CHECK_EQ(st.rdy_fail_mask, 0xC);
    (void)sense_poll_try_alt_clock();
    sense_poll_status(&st);
    CHECK_EQ(st.alt_state, 1);
    CHECK_EQ(st.ok_mask, 0x3);
    CHECK_EQ(reg(0x40021008u) & 0x80000u, 0);                /* bit 19 restored */
    t0 = g_sim_cycles;
    sense_poll_scan(v, 200);
    CHECK(US(g_sim_cycles - t0) < 1500u);                    /* the dead ADCs cost nothing */
    CHECK_EQ(v[0], 0x103);
    CHECK_EQ(v[12], 0xFFFF);
    CHECK_EQ(v[13], 0xFFFF);
    CHECK_EQ(v[74], 0xFFFF);
    CHECK_EQ(v[75], 0xFFFF);
    CHECK(US(g_hook_max_gap) <= GAP_LIMIT_US);
    CHECK_EQ(g_bad_access, 0);

    /* C: ADC3/4 only become ready with CFG2 bit 19: the retry fixes them and leaves the bit set. */
    sim_reset();
    g_need_bit19 = 0xC;
    hook_arm();
    CHECK(sense_poll_init(0xFu) & SENSE_ERR_RDY_TIMEOUT);
    (void)sense_poll_try_alt_clock();
    sense_poll_status(&st);
    CHECK_EQ(st.alt_state, 2);
    CHECK_EQ(st.ok_mask, 0xF);
    CHECK_EQ(reg(0x40021008u) & 0x80000u, 0x80000u);
    sense_poll_scan(v, 200);
    CHECK_EQ(v[12], 0x300 + 12);
    CHECK_EQ(v[13], 0x400 + 6);
    CHECK(US(g_hook_max_gap) <= GAP_LIMIT_US);

    /* D: ADC4 is ready but never ends a conversion: three misses (2 ms each) retire it, then it costs nothing. */
    sim_reset();
    g_fail_eoc = 0x8;
    hook_arm();
    CHECK_EQ(sense_poll_init(0xFu), 0);
    sense_poll_scan(v, 200);
    sense_poll_status(&st);
    CHECK_EQ(st.timeouts, 3);
    CHECK_EQ(st.ok_mask, 0x7);
    CHECK_EQ(v[13], 0xFFFF);
    CHECK_EQ(v[0], 0x103);
    t0 = g_sim_cycles;
    sense_poll_scan(v, 200);
    CHECK(US(g_sim_cycles - t0) < 1500u);
    CHECK_EQ(st.timeouts, 3);
    CHECK(US(g_hook_max_gap) <= GAP_LIMIT_US);

    /* E: ADC2's calibration never finishes: reported, bounded by 50 ms, USB serviced. */
    sim_reset();
    g_fail_cal = 0x2;
    hook_arm();
    t0 = g_sim_cycles;
    CHECK(sense_poll_init(0xFu) & SENSE_ERR_CAL_TIMEOUT);
    CHECK(US(g_sim_cycles - t0) < 1000u + 50000u + 1000u);
    sense_poll_status(&st);
    CHECK_EQ(st.cal_fail_mask, 0x2);
    CHECK(US(g_hook_max_gap) <= GAP_LIMIT_US);

    /* F: the progress marker follows the code: the last stage of a full scan is "scan done". */
    sim_reset();
    CHECK_EQ(sense_poll_init(0xFu), 0);
    CHECK_EQ(reg(0x20026F08u), SENSE_STAGE_POLL_DONE);
    sense_poll_scan(v, 0);
    CHECK_EQ(reg(0x20026F08u), SENSE_STAGE_SCAN_DONE);
    sim_reset();
    g_fail_rdy = 0x8;
    CHECK(sense_poll_init(0xFu) != 0);
    CHECK_EQ(reg(0x20026F08u), SENSE_STAGE_POLL_DONE);
}

void test_sense(void)
{
    test_tables();
    test_poll();
    test_scan_init();
    test_scan_run();
    test_scan_failsafe();
    test_s2_latency();
}
