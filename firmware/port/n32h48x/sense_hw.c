#include "sense_hw.h"

#include "sense_regs.h"
#include "sense_tables.h"

/* ---- cycle counter ---- */

void sense_dwt_init(void)
{
    SENSE_DEMCR |= 0x01000000u;        /* TRCENA */
    if (!(SENSE_DWT_CTRL & 1u)) {      /* already counting (a second call must not restart the counter under a caller's timing) */
        SENSE_DWT_CYCCNT = 0;
        SENSE_DWT_CTRL |= 1u;          /* CYCCNTENA */
    }
}

uint32_t sense_cycles(void)
{
    return SENSE_DWT_CYCCNT;
}

/* ---- idle hook and bounded waits ---- */

#define IDLE_PERIOD_CYCLES  (SENSE_IDLE_PERIOD_US * SENSE_CYCLES_PER_US)
#define RDY_TIMEOUT_US      5000u       /* a healthy ADC is ready within microseconds */
#define CAL_TIMEOUT_US      50000u      /* calibration length is unknown; generous, and the idle hook keeps USB alive meanwhile */
#define EOC_TIMEOUT_US      2000u
#define DEAD_STREAK         3u          /* consecutive missing end-of-conversion flags after which an ADC is no longer asked */

static void (*g_idle_hook)(void);
static uint32_t g_idle_last;
static uint8_t g_ok_mask, g_init_mask, g_rdy_fail, g_cal_fail, g_alt_state;
static uint8_t g_streak[SENSE_NUM_ADC + 1u];
static uint32_t g_timeouts;

void sense_set_idle_hook(void (*fn)(void))
{
    g_idle_hook = fn;
    g_idle_last = SENSE_DWT_CYCCNT;
}

/* Called from every wait loop: feeds the watchdog and, at most every SENSE_IDLE_PERIOD_US, runs the idle hook. */
static void idle_tick(void)
{
    SENSE_IWDG_FEED();
    if (g_idle_hook != 0) {
        const uint32_t now = SENSE_DWT_CYCCNT;
        if ((uint32_t)(now - g_idle_last) >= IDLE_PERIOD_CYCLES) {
            g_idle_last = now;
            g_idle_hook();
        }
    }
}

void sense_stage(uint32_t stage)
{
    SENSE_STAGE_REG = stage;
}

void sense_delay_us(uint32_t us)
{
    const uint32_t start = SENSE_DWT_CYCCNT;
    const uint32_t want = us * SENSE_CYCLES_PER_US;
    while ((uint32_t)(SENSE_DWT_CYCCNT - start) < want) {
        idle_tick();
    }
}

/* ---- GPIO ---- */

/* Configure every pin in `mask` from a descriptor {mode, pupd, sr, ds, af}. */
static void gpio_config(uint32_t port, uint32_t mask, uint32_t mode, uint32_t pupd, uint32_t sr, uint32_t ds, uint32_t af)
{
    for (unsigned pin = 0; pin < 16u; pin++) {
        if (!(mask & (1u << pin))) {
            continue;
        }
        const unsigned afi = pin / 4u, afsh = (pin % 4u) * 8u;
        SENSE_GPIO_AF(port, afi) = (SENSE_GPIO_AF(port, afi) & ~(0x1Fu << afsh)) | (af << afsh);
        SENSE_GPIO_POTYPE(port) &= ~(1u << pin);                    /* push-pull */
        SENSE_GPIO_PMODE(port) = (SENSE_GPIO_PMODE(port) & ~(3u << (2u * pin))) | ((mode & 3u) << (2u * pin));
        SENSE_GPIO_PUPD(port) = (SENSE_GPIO_PUPD(port) & ~(3u << (2u * pin))) | (pupd << (2u * pin));
        SENSE_GPIO_SR(port) = (SENSE_GPIO_SR(port) & ~(1u << pin)) | (sr << pin);
        SENSE_GPIO_DS(port) = (SENSE_GPIO_DS(port) & ~(3u << (2u * pin))) | (ds << (2u * pin));
    }
}

void sense_gpio_init(void)
{
    SENSE_RCC_APB2PCLKEN |= SENSE_APB2_AFIO;
    SENSE_RCC_AHB1PCLKEN |= SENSE_AHB1_GPIO_ALL;
    for (unsigned i = 0; i < 3u; i++) {
        SENSE_AFIO_TOL5V_CFG(i) = 0;                                 /* 5 V tolerance off on all six ports, as the bootloader leaves it */
    }
    SENSE_AFIO_RMP_CFG = (SENSE_AFIO_RMP_CFG & SENSE_AFIO_SWJ_MASK) | SENSE_AFIO_SWJ_VALUE;   /* frees PB3/PB4 for GPIO */

    /* PB3 / PB4 = mux address, PB5 = driven low like the bootloader does; levels low first, no glitch. */
    SENSE_GPIO_PBC(SENSE_MUX_PORT) = SENSE_MUX_MASK | SENSE_PB5_MASK;
    gpio_config(SENSE_MUX_PORT, SENSE_MUX_MASK | SENSE_PB5_MASK, SENSE_PMODE_OUTPUT, 0, 0, 1, 0);

    gpio_config(SENSE_GPIOA, SENSE_ANALOG_A, SENSE_PMODE_ANALOG, 0, 0, 0, 0);
    gpio_config(SENSE_GPIOB, SENSE_ANALOG_B, SENSE_PMODE_ANALOG, 0, 0, 0, 0);
    gpio_config(SENSE_GPIOC, SENSE_ANALOG_C, SENSE_PMODE_ANALOG, 0, 0, 0, 0);
    gpio_config(SENSE_GPIOE, SENSE_ANALOG_E, SENSE_PMODE_ANALOG, 0, 0, 0, 0);
    gpio_config(SENSE_GPIOF, SENSE_ANALOG_F, SENSE_PMODE_ANALOG, 0, 0, 0, 0);
    gpio_config(SENSE_GPIOD, SENSE_ANALOG_D, SENSE_PMODE_ANALOG, 0, 0, 0, 0);
}

void sense_mux_set(unsigned phase)
{
    SENSE_GPIO_POD(SENSE_MUX_PORT) = sense_mux_apply(SENSE_GPIO_POD(SENSE_MUX_PORT), phase);
}

unsigned sense_mux_get(void)
{
    return (SENSE_GPIO_POD(SENSE_MUX_PORT) & SENSE_MUX_MASK) >> SENSE_MUX_SHIFT;
}

/* ---- ADC ---- */

/* The same sequence for both configurations; only the mode words differ. */
static void adc_regs_init(uint32_t b, uint32_t ctrl1, uint32_t ctrl2, unsigned n_regular)
{
    SENSE_UNK_4001004C |= SENSE_UNK_4001004C_BIT;
    SENSE_ADC_PCTRL(b) = (SENSE_ADC_PCTRL(b) & SENSE_PCTRL_KEEP) | SENSE_PCTRL_SET;
    SENSE_ADC_CTRL1(b) = (SENSE_ADC_CTRL1(b) & SENSE_CTRL1_KEEP) | ctrl1;
    SENSE_ADC_CTRL2(b) = (SENSE_ADC_CTRL2(b) & SENSE_CTRL2_KEEP) | ctrl2;
    SENSE_ADC_CTRL3(b) = (SENSE_ADC_CTRL3(b) & ~3u) | SENSE_CTRL3_MODE;
    SENSE_ADC_RSEQ(b, 2) = sense_rseq_len_apply(SENSE_ADC_RSEQ(b, 2), n_regular);
}

static void adc_set_channel(uint32_t b, unsigned rank, unsigned ch, unsigned smp)
{
    SENSE_ADC_SAMPT(b, sense_sampt_index(ch)) = sense_sampt_apply(SENSE_ADC_SAMPT(b, sense_sampt_index(ch)), ch, smp);
    const unsigned ri = sense_rseq_index(rank);
    SENSE_ADC_RSEQ(b, ri) = sense_rseq_apply(SENSE_ADC_RSEQ(b, ri), rank, ch);
}

typedef enum { WAIT_RDY, WAIT_CAL_DONE, WAIT_EOC } wait_kind_t;

/* Wait for a condition of ADC `b`, at most `timeout_us`; ticks the idle hook all the while. */
static bool adc_wait(uint32_t b, wait_kind_t kind, uint32_t timeout_us)
{
    const uint32_t start = SENSE_DWT_CYCCNT;
    const uint32_t limit = timeout_us * SENSE_CYCLES_PER_US;
    for (;;) {
        idle_tick();
        const bool met = (kind == WAIT_RDY)      ? (SENSE_ADC_STS(b) & SENSE_ADC_STS_RDY) != 0u
                       : (kind == WAIT_CAL_DONE) ? (SENSE_ADC_CTRL2(b) & SENSE_ADC_CTRL2_CAL) == 0u
                                                 : (SENSE_ADC_STS(b) & SENSE_ADC_STS_EOC) != 0u;
        if (met) {
            return true;
        }
        if ((uint32_t)(SENSE_DWT_CYCCNT - start) >= limit) {
            return false;
        }
    }
}

static bool adc_power_up(uint32_t b)
{
    SENSE_ADC_CTRL2(b) |= SENSE_ADC_CTRL2_ADON;
    return adc_wait(b, WAIT_RDY, RDY_TIMEOUT_US);
}

static bool adc_calibrate(uint32_t b)
{
    SENSE_ADC_CTRL3(b) &= ~SENSE_ADC_CTRL3_CALDIF;           /* single-ended calibration */
    SENSE_ADC_CTRL2(b) |= SENSE_ADC_CTRL2_CAL;
    return adc_wait(b, WAIT_CAL_DONE, CAL_TIMEOUT_US);
}

static void adc_reset_pulse(void)
{
    SENSE_RCC_AHBPRST |= SENSE_AHBPRST_ADC_ALL;              /* pulse the ADC reset bits */
    SENSE_RCC_AHBPRST &= ~SENSE_AHBPRST_ADC_ALL;
    sense_delay_us(1000);                                     /* 1 ms */
}

/* Init + power up + calibrate one ADC in the bootloader style. Records which step failed. */
static uint32_t poll_bring_up(unsigned n)
{
    uint32_t err = 0;
    const uint32_t b = SENSE_ADC_BASE(n);
    const uint8_t bit = (uint8_t)(1u << (n - 1u));
    sense_stage(SENSE_STAGE_ADC_INIT + n);
    adc_regs_init(b, SENSE_POLL_CTRL1, SENSE_POLL_CTRL2, 1);
    if (!adc_power_up(b)) {
        g_rdy_fail |= bit;
        SENSE_ADC_CTRL2(b) &= ~SENSE_ADC_CTRL2_ADON;
        return SENSE_ERR_RDY_TIMEOUT;
    }
    g_rdy_fail &= (uint8_t)~bit;
    sense_stage(SENSE_STAGE_ADC_RDY + n);
    if (!adc_calibrate(b)) {
        g_cal_fail |= bit;
        err |= SENSE_ERR_CAL_TIMEOUT;
    } else {
        g_cal_fail &= (uint8_t)~bit;
    }
    sense_stage(SENSE_STAGE_ADC_CAL + n);
    g_ok_mask |= bit;                                         /* ready; an uncalibrated ADC is still asked, 3 misses retire it */
    g_streak[n] = 0;
    return err;
}

uint32_t sense_poll_init(unsigned adc_mask)
{
    uint32_t err = 0;
    sense_dwt_init();
    sense_stage(SENSE_STAGE_POLL_BEGIN);
    g_init_mask = (uint8_t)adc_mask;
    g_ok_mask = g_rdy_fail = g_cal_fail = g_alt_state = 0;
    sense_gpio_init();
    adc_reset_pulse();
    sense_stage(SENSE_STAGE_RESET_DONE);
    for (unsigned n = 1; n <= SENSE_NUM_ADC; n++) {
        if (adc_mask & (1u << (n - 1u))) {
            SENSE_RCC_AHB1PCLKEN |= SENSE_AHB1_ADC(n);
        }
    }
    /* set A (as the bootloader): ADC clock = HCLK divided, CFG2[8:4] = 0, CFG2[3:0] = 8, ADC CTRL3 bit4 = 0 */
    SENSE_RCC_CFG2 &= SENSE_CFG2_ADC_KEEP;
    SENSE_RCC_CFG2 = (SENSE_RCC_CFG2 & SENSE_CFG2_NIB_KEEP) | SENSE_CFG2_POLL_LOW_NIBBLE;
    for (unsigned n = 1; n <= SENSE_NUM_ADC; n++) {
        if (adc_mask & (1u << (n - 1u))) {
            SENSE_ADC_CTRL3(SENSE_ADC_BASE(n)) &= ~SENSE_ADC_CTRL3_CLKSEL;
        }
    }
    SENSE_RCC_CFG2 = (SENSE_RCC_CFG2 & SENSE_CFG2_UPPER_KEEP) | SENSE_CFG2_UPPER_POLL;
    sense_stage(SENSE_STAGE_CLOCKS_DONE);
    for (unsigned n = 1; n <= SENSE_NUM_ADC; n++) {
        if (adc_mask & (1u << (n - 1u))) {
            err |= poll_bring_up(n);
        }
    }
    sense_stage(SENSE_STAGE_POLL_DONE);
    return err;
}

/* Diagnostic second attempt for ADCs that did not become ready: set CFG2 bit 19 (the one bit that set A, the bootloader's
 * key-read setup, leaves clear and set B, the scan, sets) and bring them up again. Undone if nothing improves. */
uint32_t sense_poll_try_alt_clock(void)
{
    const uint8_t failed = (uint8_t)(g_init_mask & ~g_ok_mask);
    if (failed == 0u) {
        return 0;
    }
    sense_stage(SENSE_STAGE_ALT_BEGIN);
    SENSE_RCC_CFG2 |= SENSE_CFG2_ALT_BIT;
    uint32_t err = 0;
    for (unsigned n = 1; n <= SENSE_NUM_ADC; n++) {
        if (failed & (1u << (n - 1u))) {
            err |= poll_bring_up(n);
        }
    }
    const uint8_t fixed = (uint8_t)(g_ok_mask & failed);
    if (fixed == 0u) {
        SENSE_RCC_CFG2 &= ~SENSE_CFG2_ALT_BIT;
        g_alt_state = 1;                                      /* tried, no help, undone */
    } else {
        g_alt_state = (fixed == failed) ? 2u : 3u;            /* 2 = all fixed, 3 = some fixed; bit stays set */
    }
    sense_stage(SENSE_STAGE_ALT_DONE);
    return err;
}

void sense_poll_status(sense_poll_status_t *out)
{
    out->ok_mask = g_ok_mask;
    out->rdy_fail_mask = g_rdy_fail;
    out->cal_fail_mask = g_cal_fail;
    out->alt_state = g_alt_state;
    out->timeouts = g_timeouts;
}

int32_t sense_poll_convert(unsigned adc, unsigned ch)
{
    const uint32_t b = SENSE_ADC_BASE(adc);
    if (!(g_ok_mask & (1u << (adc - 1u)))) {
        return -1;                                            /* not ready, or retired: never wait for it */
    }
    adc_set_channel(b, 1, ch, SENSE_POLL_SAMPT);
    SENSE_ADC_CTRL2(b) |= SENSE_ADC_CTRL2_SWSTRT;
    if (adc_wait(b, WAIT_EOC, EOC_TIMEOUT_US)) {
        g_streak[adc] = 0;
        SENSE_ADC_STS(b) = 1u;                                /* clear the status flags */
        SENSE_ADC_STS(b) = 0x400u;
        return (int32_t)(SENSE_ADC_DAT(b) & 0xFFFFu);
    }
    g_timeouts++;
    if (++g_streak[adc] >= DEAD_STREAK) {
        g_ok_mask &= (uint8_t)~(1u << (adc - 1u));
    }
    return -1;
}

void sense_poll_scan(uint16_t out[SENSE_NUM_SLOTS], uint32_t settle_us)
{
    for (unsigned phase = 0; phase < SENSE_NUM_PHASES; phase++) {
        sense_stage(SENSE_STAGE_SCAN_PHASE + phase);
        sense_mux_set(phase);
        sense_delay_us(settle_us);
        for (unsigned w = 0; w < SENSE_PHASE_WORDS; w++) {
            for (unsigned half = 0; half < 2u; half++) {
                const unsigned adc = (w < 6u ? 1u : 3u) + half;
                const unsigned rank = w < 6u ? w + 1u : w - 5u;
                const int32_t v = sense_poll_convert(adc, sense_regular_ch[adc - 1u][rank - 1u]);
                out[sense_slot_of(phase, w, half)] = v < 0 ? 0xFFFFu : (uint16_t)v;
            }
        }
    }
    sense_stage(SENSE_STAGE_SCAN_INJ);
    for (unsigned n = 0; n < SENSE_NUM_ADC; n++) {
        const int32_t v = sense_poll_convert(n + 1u, sense_injected_ch[n]);
        out[72u + n] = v < 0 ? 0xFFFFu : (uint16_t)v;
    }
    sense_stage(SENSE_STAGE_SCAN_DONE);
}

uint32_t sense_scan_adc_init(void)
{
    uint32_t err = 0;
    sense_dwt_init();
    adc_reset_pulse();
    SENSE_RCC_AHB1PCLKEN |= SENSE_AHB1_ADC_ALL;
    SENSE_RCC_CFG2 = (SENSE_RCC_CFG2 & SENSE_CFG2_UPPER_KEEP) | SENSE_CFG2_UPPER_SCAN;
    /* set B: CFG2[8:4] <- 0x150 field, CFG2[3:0] <- 0, ADC CTRL3 bit4 <- 1 (PLL clock source) */
    SENSE_RCC_CFG2 = (SENSE_RCC_CFG2 & SENSE_CFG2_ADC_KEEP) | SENSE_CFG2_SCAN_FIELD;
    SENSE_RCC_CFG2 = SENSE_RCC_CFG2 & SENSE_CFG2_NIB_KEEP;
    for (unsigned n = 1; n <= SENSE_NUM_ADC; n++) {
        SENSE_ADC_CTRL3(SENSE_ADC_BASE(n)) |= SENSE_ADC_CTRL3_CLKSEL;
    }

    for (unsigned n = 1; n <= SENSE_NUM_ADC; n++) {
        const uint32_t b = SENSE_ADC_BASE(n);
        const unsigned len = sense_regular_len[n - 1u];
        adc_regs_init(b, SENSE_SCAN_CTRL1, SENSE_SCAN_CTRL2, len);
        for (unsigned r = 1; r <= len; r++) {
            adc_set_channel(b, r, sense_regular_ch[n - 1u][r - 1u], SENSE_SCAN_SAMPT);
        }
        SENSE_ADC_JSEQ(b) = sense_jseq_len_apply(SENSE_ADC_JSEQ(b), 1);
        const unsigned ich = sense_injected_ch[n - 1u];
        SENSE_ADC_SAMPT(b, sense_sampt_index(ich)) =
            sense_sampt_apply(SENSE_ADC_SAMPT(b, sense_sampt_index(ich)), ich, SENSE_SCAN_SAMPT);
        SENSE_ADC_JSEQ(b) = sense_jseq_apply(SENSE_ADC_JSEQ(b), 1, ich);
        SENSE_ADC_CTRL1(b) &= ~2u;
        SENSE_ADC_CTRL2(b) = (SENSE_ADC_CTRL2(b) & SENSE_SCAN_EXTRSEL_CTRL2_KEEP) | SENSE_SCAN_EXTRSEL_CTRL2_SET;
        if (n == 1u || n == 3u) {
            SENSE_ADC_INTEN(b) |= SENSE_ADC_INTEN_JENDC;                                /* only ADC1 and ADC3 */
        }
    }
    for (unsigned n = 1; n <= SENSE_NUM_ADC; n += 2u) {                                 /* ADC1 and ADC3 only (the masters) */
        const uint32_t b = SENSE_ADC_BASE(n);
        SENSE_ADC_CTRL2(b) = (SENSE_ADC_CTRL2(b) & SENSE_SCAN_MASTER_CTRL2_KEEP) | SENSE_SCAN_MASTER_CTRL2_SET;
    }
    for (unsigned n = 1; n <= SENSE_NUM_ADC; n++) {
        if (!adc_power_up(SENSE_ADC_BASE(n))) {
            err |= SENSE_ERR_RDY_TIMEOUT;
        }
    }
    for (unsigned n = 1; n <= SENSE_NUM_ADC; n++) {
        if (!adc_calibrate(SENSE_ADC_BASE(n))) {
            err |= SENSE_ERR_CAL_TIMEOUT;
        }
    }
    return err;
}
