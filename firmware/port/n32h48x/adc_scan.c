#include "adc_scan.h"

#include <string.h>

#include "sense_hw.h"
#include "sense_regs.h"

#ifdef KM_SENSE_HOST_TEST
#define IRQ_OFF()  ((void)0)
#define IRQ_ON()   ((void)0)
#else
#define IRQ_OFF()  __asm volatile("cpsid i" ::: "memory")
#define IRQ_ON()   __asm volatile("cpsie i" ::: "memory")
#endif

#define IRQ_PRIORITY 0x20u      /* all four sensing interrupts; the USB driver is polled, nothing competes */

uint32_t g_phase_words[SENSE_NUM_PHASES][SENSE_PHASE_WORDS] __attribute__((aligned(4)));
uint32_t g_inj[2];

static adc_scan_stats_t s_stats;
static volatile uint8_t s_flags;            /* bit0 = DMA ch1 done, bit1 = DMA ch3 done */
static volatile uint8_t s_phase;            /* phase being sampled */
static volatile uint8_t s_phase_mask;
static volatile bool s_frame_ready;
static volatile bool s_running;
static uint32_t s_t_phase, s_t_frame;
static volatile uint32_t s_t_armed;         /* CYCCNT when the current frame was started (start / release / restart) */
static volatile uint32_t s_irq_cycles;      /* CPU cycles spent inside the four interrupt handlers, summed (wraps) */
static volatile uint32_t s_dma_err_taken;   /* dma_errors already handed out by adc_scan_take_dma_errors() */

static void compiler_barrier(void)
{
    __asm volatile("" ::: "memory");
}

/* ---- NVIC ---- */

static void nvic_set_priority(unsigned irq)
{
    const unsigned w = irq / 4u, sh = (irq % 4u) * 8u;
    SENSE_NVIC_IPR(w) = (SENSE_NVIC_IPR(w) & ~(0xFFu << sh)) | (IRQ_PRIORITY << sh);
}

static void nvic_enable(unsigned irq)
{
    SENSE_NVIC_ISER(irq / 32u) = 1u << (irq % 32u);
}

static void nvic_disable(unsigned irq)
{
    SENSE_NVIC_ICER(irq / 32u) = 1u << (irq % 32u);
}

/* ---- DMA: disable and clear, configure, enable (SENSING_BRINGUP_NOTES.md section 1.3) ---- */

static void dma_channel_setup(unsigned ch, uint32_t clear_all, uint32_t paddr, uint32_t maddr, uint32_t count, uint32_t chsel)
{
    SENSE_DMA_CHCFG(ch) &= ~SENSE_DMA_CHCFG_EN;
    SENSE_DMA_CHCFG(ch) = 0;
    SENSE_DMA_TXNUM(ch) = 0;
    SENSE_DMA_PADDR(ch) = 0;
    SENSE_DMA_MADDR(ch) = 0;
    SENSE_DMA_INTCLR = clear_all;
    SENSE_DMA_CHCFG(ch) = (SENSE_DMA_CHCFG(ch) & SENSE_DMA_CHCFG_KEEP) | sense_dma_chcfg_value();
    SENSE_DMA_TXNUM(ch) = count;
    SENSE_DMA_PADDR(ch) = paddr;
    SENSE_DMA_MADDR(ch) = maddr;
    SENSE_DMA_BURSTEN = 0;
    SENSE_DMA_CHSEL(ch) = chsel;
}

static void dma_arm_phase(unsigned phase)
{
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC12) &= ~SENSE_DMA_CHCFG_EN;
    SENSE_DMA_MADDR(SENSE_DMA_CH_ADC12) = (uint32_t)(uintptr_t)&g_phase_words[phase][0];
    SENSE_DMA_TXNUM(SENSE_DMA_CH_ADC12) = SENSE_DMA_COUNT_ADC12;
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC12) |= SENSE_DMA_CHCFG_EN;
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC34) &= ~SENSE_DMA_CHCFG_EN;
    SENSE_DMA_MADDR(SENSE_DMA_CH_ADC34) = (uint32_t)(uintptr_t)&g_phase_words[phase][6];
    SENSE_DMA_TXNUM(SENSE_DMA_CH_ADC34) = SENSE_DMA_COUNT_ADC34;
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC34) |= SENSE_DMA_CHCFG_EN;
}

/* ---- GTIM1: prescaler, auto-reload, compare 4 (SENSING_BRINGUP_NOTES.md section 1.3) ---- */

static void timer_setup(void)
{
    SENSE_RCC_APB1PCLKEN |= SENSE_APB1_GTIM1;
    SENSE_GTIM1_CTRL1 &= ~SENSE_GTIM_CEN;
    SENSE_GTIM1_CTRL1 &= 0xFFFFFFF1u;
    SENSE_GTIM1_CTRL1 &= 0xFFFFFF3Fu;
    SENSE_GTIM1_PSC = SENSE_GTIM1_PSC_VALUE;
    SENSE_GTIM1_AR = SENSE_GTIM1_AR_VALUE;
    SENSE_GTIM1_EVTGEN = SENSE_GTIM1_EVTGEN_VALUE;
    SENSE_GTIM1_CTRL1 &= 0xFFF0FFFFu & 0xFFFFDFFFu;
    SENSE_GTIM1_INSEL &= 0xF0F00000u;
    SENSE_GTIM1_CCEN &= 0xFFFFBFFFu;
    SENSE_GTIM1_CCMOD2 = (SENSE_GTIM1_CCMOD2 & 0xFFFB1CFFu) | 0xC000u;
    SENSE_GTIM1_CCDAT4 = SENSE_GTIM1_CCDAT4_VALUE;
    SENSE_GTIM1_CCEN = (SENSE_GTIM1_CCEN & 0xFFFF7FFFu) | 0x8000u | 0x4000u;
    SENSE_GTIM1_CCMOD2 &= 0xFFFFFBFFu;
    SENSE_GTIM1_CTRL1 &= 0xFFFFFDFFu;
}

static void timer_restart(void)
{
    SENSE_GTIM1_CNT = 0;
    SENSE_GTIM1_CTRL1 |= SENSE_GTIM_CEN;
}

/* ---- API ---- */

uint32_t adc_scan_init(void)
{
    memset(&s_stats, 0, sizeof s_stats);
    s_stats.frame_cycles_min = 0xFFFFFFFFu;
    s_running = false;
    s_frame_ready = false;

    sense_dwt_init();
    sense_gpio_init();
    SENSE_RCC_AHBPCLKEN |= SENSE_AHB_DMA1;
    dma_channel_setup(SENSE_DMA_CH_ADC12, SENSE_DMA_INTCLR_CH1_ALL, SENSE_ADC_DAT_ADDR(1),
                      (uint32_t)(uintptr_t)&g_phase_words[0][0], SENSE_DMA_COUNT_ADC12, SENSE_DMA_CHSEL_ADC12);
    dma_channel_setup(SENSE_DMA_CH_ADC34, SENSE_DMA_INTCLR_CH3_ALL, SENSE_ADC_DAT_ADDR(3),
                      (uint32_t)(uintptr_t)&g_phase_words[0][6], SENSE_DMA_COUNT_ADC34, SENSE_DMA_CHSEL_ADC34);
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC12) |= SENSE_DMA_CHCFG_TCIE;
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC34) |= SENSE_DMA_CHCFG_TCIE;
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC12) |= SENSE_DMA_CHCFG_EN;
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC34) |= SENSE_DMA_CHCFG_EN;

    s_stats.init_err = sense_scan_adc_init();
    timer_setup();

    nvic_set_priority(SENSE_IRQ_DMA1_CH1);
    nvic_set_priority(SENSE_IRQ_DMA1_CH3);
    nvic_set_priority(SENSE_IRQ_ADC1_2);
    nvic_set_priority(SENSE_IRQ_ADC3_4);
    return s_stats.init_err;
}

void adc_scan_start(void)
{
    IRQ_OFF();
    SENSE_GTIM1_CTRL1 &= ~SENSE_GTIM_CEN;
    s_running = false;
    s_flags = 0;
    s_phase = 0;
    s_phase_mask = 0;
    s_frame_ready = false;
    sense_mux_set(0);
    SENSE_DMA_INTCLR = SENSE_DMA_INTCLR_CH1_ALL | SENSE_DMA_INTCLR_CH3_ALL;
    dma_arm_phase(0);
    IRQ_ON();
    nvic_enable(SENSE_IRQ_DMA1_CH1);
    nvic_enable(SENSE_IRQ_DMA1_CH3);
    nvic_enable(SENSE_IRQ_ADC1_2);
    nvic_enable(SENSE_IRQ_ADC3_4);
    s_running = true;
    s_t_phase = s_t_frame = s_t_armed = sense_cycles();
    timer_restart();
}

void adc_scan_stop(void)
{
    IRQ_OFF();
    s_running = false;
    SENSE_GTIM1_CTRL1 &= ~SENSE_GTIM_CEN;
    IRQ_ON();
    nvic_disable(SENSE_IRQ_DMA1_CH1);
    nvic_disable(SENSE_IRQ_DMA1_CH3);
    nvic_disable(SENSE_IRQ_ADC1_2);
    nvic_disable(SENSE_IRQ_ADC3_4);
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC12) &= ~SENSE_DMA_CHCFG_EN;
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC34) &= ~SENSE_DMA_CHCFG_EN;
}

bool adc_scan_frame_ready(void)
{
    const bool r = s_frame_ready;
    compiler_barrier();
    return r;
}

uint8_t adc_scan_phase_mask(void)
{
    return s_phase_mask;
}

void adc_scan_release_frame(void)
{
    compiler_barrier();
    IRQ_OFF();
    s_phase_mask = 0;
    s_frame_ready = false;
    s_flags = 0;
    if (s_running) {
        s_t_phase = s_t_frame = s_t_armed = sense_cycles();
        timer_restart();
    }
    IRQ_ON();
}

void adc_scan_get_stats(adc_scan_stats_t *out)
{
    IRQ_OFF();
    *out = s_stats;
    IRQ_ON();
}

/* ---- M5 T-E ---- */

bool adc_scan_frame_timeout(uint32_t now)
{
    return s_running && !s_frame_ready && (int32_t)(now - s_t_armed) > (int32_t)ADC_SCAN_FRAME_TIMEOUT_CYCLES;
}

void adc_scan_test_stall(void)
{
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC12) &= ~SENSE_DMA_CHCFG_EN;
}

uint32_t adc_scan_irq_cycles(void)
{
    return s_irq_cycles;
}

uint32_t adc_scan_take_dma_errors(void)
{
    IRQ_OFF();
    const uint32_t n = s_stats.dma_errors - s_dma_err_taken;
    s_dma_err_taken = s_stats.dma_errors;
    IRQ_ON();
    return n;
}

void adc_scan_restart(uint32_t now)
{
    IRQ_OFF();
    s_running = false;
    SENSE_GTIM1_CTRL1 &= ~SENSE_GTIM_CEN;
    IRQ_ON();
    nvic_disable(SENSE_IRQ_DMA1_CH1);
    nvic_disable(SENSE_IRQ_DMA1_CH3);
    nvic_disable(SENSE_IRQ_ADC1_2);
    nvic_disable(SENSE_IRQ_ADC3_4);
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC12) &= ~SENSE_DMA_CHCFG_EN;
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC34) &= ~SENSE_DMA_CHCFG_EN;
    if (SENSE_DMA_INTSTS & (SENSE_DMA_TEIF_CH1 | SENSE_DMA_TEIF_CH3)) {
        s_stats.dma_errors++;                                   /* the chain stopped because of a transfer error */
    }
    /* full re-configuration of both channels, as at init (the transfer-complete interrupt enable is part of it) */
    dma_channel_setup(SENSE_DMA_CH_ADC12, SENSE_DMA_INTCLR_CH1_ALL, SENSE_ADC_DAT_ADDR(1),
                      (uint32_t)(uintptr_t)&g_phase_words[0][0], SENSE_DMA_COUNT_ADC12, SENSE_DMA_CHSEL_ADC12);
    dma_channel_setup(SENSE_DMA_CH_ADC34, SENSE_DMA_INTCLR_CH3_ALL, SENSE_ADC_DAT_ADDR(3),
                      (uint32_t)(uintptr_t)&g_phase_words[0][6], SENSE_DMA_COUNT_ADC34, SENSE_DMA_CHSEL_ADC34);
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC12) |= SENSE_DMA_CHCFG_TCIE;
    SENSE_DMA_CHCFG(SENSE_DMA_CH_ADC34) |= SENSE_DMA_CHCFG_TCIE;
    SENSE_ADC_STS(SENSE_ADC_BASE(1)) = SENSE_ADC_STS_JENDC;     /* same write the interrupt handlers do */
    SENSE_ADC_STS(SENSE_ADC_BASE(3)) = SENSE_ADC_STS_JENDC;
    SENSE_NVIC_ICPR(0) = (1u << SENSE_IRQ_DMA1_CH1) | (1u << SENSE_IRQ_DMA1_CH3) | (1u << SENSE_IRQ_ADC1_2);
    SENSE_NVIC_ICPR(1) = 1u << (SENSE_IRQ_ADC3_4 - 32u);
    s_stats.restarts++;
    adc_scan_start();                                           /* flags, phase, mux 0, DMA armed, NVIC on, timer */
    s_t_armed = now;
}

/* ---- interrupts ---- */

/* Both DMA channels of the current phase are done: advance the mux phase, re-arm the DMA, restart the timer (the sliding sums are done by the main loop). */
static void phase_done(void)
{
    SENSE_GTIM1_CTRL1 &= ~SENSE_GTIM_CEN;                       /* stop the timer first */
    const uint32_t now = sense_cycles();
    const unsigned done = s_phase;
    if (SENSE_DMA_INTSTS & (SENSE_DMA_TEIF_CH1 | SENSE_DMA_TEIF_CH3)) {
        s_stats.dma_errors++;
        SENSE_DMA_INTCLR = SENSE_DMA_TEIF_CH1 | SENSE_DMA_TEIF_CH3;
    }
    s_stats.phase_cycles[done] = now - s_t_phase;
    s_stats.phases++;
    s_phase_mask = (uint8_t)(s_phase_mask | (1u << done));
    const unsigned next = (done + 1u) & 3u;
    s_phase = (uint8_t)next;
    sense_mux_set(next);                                        /* GPIOB_POD is written before the DMA is re-armed */
    dma_arm_phase(next);
    if (done == 3u) {
        const uint32_t fc = now - s_t_frame;
        s_stats.frames++;
        s_stats.frame_cycles = fc;
        if (fc < s_stats.frame_cycles_min) {
            s_stats.frame_cycles_min = fc;
        }
        if (fc > s_stats.frame_cycles_max) {
            s_stats.frame_cycles_max = fc;
        }
        s_frame_ready = true;                                   /* the timer stays stopped until adc_scan_release_frame() */
    } else if (s_running) {
        s_t_phase = sense_cycles();
        timer_restart();                                        /* CNT = 0, CEN = 1 */
    }
}

static void dma_done(uint8_t bit)
{
    if (!s_running) {
        return;
    }
    if (s_flags & bit) {
        s_stats.overruns++;
    }
    s_flags = (uint8_t)(s_flags | bit);
    if (s_flags == 3u) {
        s_flags = 0;
        phase_done();
    }
}

void DMA1_Channel1_IRQHandler(void)
{
    const uint32_t t0 = sense_cycles();
    SENSE_DMA_INTCLR = SENSE_DMA_INTCLR_CH1;
    dma_done(1u);
    s_irq_cycles += sense_cycles() - t0;
}

void DMA1_Channel3_IRQHandler(void)
{
    const uint32_t t0 = sense_cycles();
    SENSE_DMA_INTCLR = SENSE_DMA_INTCLR_CH3;
    dma_done(2u);
    s_irq_cycles += sense_cycles() - t0;
}

/* ADC1_2 (IRQ18) and ADC3_4 (IRQ47) handlers: clear JENDC of the master ADC, combine both JDAT1. */
void ADC1_2_IRQHandler(void)
{
    const uint32_t t0 = sense_cycles();
    SENSE_ADC_STS(SENSE_ADC_BASE(1)) = SENSE_ADC_STS_JENDC;
    g_inj[0] = (SENSE_ADC_JDAT1(SENSE_ADC_BASE(1)) & 0xFFFFu) | (SENSE_ADC_JDAT1(SENSE_ADC_BASE(2)) << 16);
    s_stats.inj_events++;
    s_irq_cycles += sense_cycles() - t0;
}

void ADC3_4_IRQHandler(void)
{
    const uint32_t t0 = sense_cycles();
    SENSE_ADC_STS(SENSE_ADC_BASE(3)) = SENSE_ADC_STS_JENDC;
    g_inj[1] = (SENSE_ADC_JDAT1(SENSE_ADC_BASE(3)) & 0xFFFFu) | (SENSE_ADC_JDAT1(SENSE_ADC_BASE(4)) << 16);
    s_stats.inj_events++;
    s_irq_cycles += sense_cycles() - t0;
}
