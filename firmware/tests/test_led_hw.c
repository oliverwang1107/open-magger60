/*
 * test_led_hw.c - M5 task L5: the LED output driver (led_hw.c) against a simulated register file, and the LED service of the M5 image
 * (m5_led.c) on top of it: start-up gating, frame flow, the kill switch in every form, the boot-loop guard.
 *
 * The register simulation records every access. Three properties are checked on all of them:
 *   - only addresses of the LED path are touched (clock enables, PB8, PF5, SPI5, DMA1 channel 5 and its two shared registers);
 *     in particular nothing of the flash controller, the ADCs, DMA channels 1 and 3, the mux pins PB3..PB5, or the analog pins;
 *   - in a shared register only the bits that belong to the LED path change (tested with all-ones, all-zeros and mixed initial values);
 *   - the order: PF5 and PB8 are set up before the SPI is enabled, and the SPI is enabled last.
 * led_hw.c is compiled three times (levels 2, 1, 0 with renamed functions) to check which registers each level touches.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "km/km_engine.h"
#include "km/km_health.h"
#include "km/km_led.h"
#include "km/km_led_guard.h"
#include "km_test.h"
#include "led_hw.h"
#include "m5_led.h"

/* the level-1 / level-0 copies of led_hw.c (run_tests.py renames their functions) */
bool l1_start(const uint8_t *dark, unsigned len, uint32_t now);
void l1_kill(void);
bool l1_send(const uint8_t *frame, unsigned len, uint32_t now);
led_hw_state_t l1_poll(uint32_t now);
void l1_reset(void);
bool l0_start(const uint8_t *dark, unsigned len, uint32_t now);
void l0_kill(void);
bool l0_send(const uint8_t *frame, unsigned len, uint32_t now);
led_hw_state_t l0_poll(uint32_t now);
void l0_reset(void);

void led_hw_test_reset(void);
const uint8_t *led_hw_test_last_frame(void);
void m5_led_test_power_cycle(void);

/* ---------------------------------------------------------------- register simulation */

#define SIM_MAX 96u
#define LOG_MAX 4096u
#define A_DMA5_CHCFG 0x40020050u
#define A_DMA5_TXNUM 0x40020060u

static struct {
    uintptr_t addr;
    uint32_t  val;
} g_reg[SIM_MAX];
static unsigned g_nreg;
static struct {
    uintptr_t addr;
    uint32_t  val;
} g_wlog[LOG_MAX];
static unsigned g_nwlog, g_nacc, g_viol, g_seed_mode;
static uint32_t *g_last_ptr, g_last_before;
static uintptr_t g_last_addr;
static bool g_dma_auto, g_dma_active;
static uint32_t g_cycles;
static unsigned g_gpiof_acc;

uint32_t km_led_sim_cycles(void)
{
    g_cycles += 7u;
    return g_cycles;
}

static bool allowed(uintptr_t a)
{
    static const uintptr_t ok[] = {
        0x40021004u, 0x40021008u, 0x40021018u, 0x40021020u, 0x4002103Cu,                                  /* RCC: reads of CFG / CFG2, clock enables */
        0x4002C400u, 0x4002C404u, 0x4002C408u, 0x4002C414u, 0x4002C41Cu, 0x4002C420u,                     /* GPIOB for PB8 */
        0x4002D400u, 0x4002D404u, 0x4002D408u, 0x4002D41Cu, 0x4002D420u, 0x4002D424u, 0x4002D428u,         /* GPIOF for PF5 */
        0x40013000u, 0x40013004u, 0x40013018u, 0x4001301Cu,                                               /* SPI5 */
        0x40020050u, 0x40020054u, 0x40020058u, 0x4002005Cu, 0x40020060u, 0x400200A0u, 0x400200A8u,         /* DMA1 channel 5, INTCLR, BURSTEN */
    };
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        if (ok[i] == a) {
            return true;
        }
    }
    return false;
}

static uint32_t seed_value(uintptr_t addr)
{
    switch (g_seed_mode) {
    case 0:  return 0xFFFFFFFFu;
    case 1:  return 0u;
    default: return 0xA5A5A5A5u ^ (uint32_t)(addr * 2654435761u);
    }
}

static uint32_t *entry(uintptr_t addr)
{
    for (unsigned i = 0; i < g_nreg; i++) {
        if (g_reg[i].addr == addr) {
            return &g_reg[i].val;
        }
    }
    if (g_nreg < SIM_MAX) {
        g_reg[g_nreg].addr = addr;
        g_reg[g_nreg].val = seed_value(addr);
        return &g_reg[g_nreg++].val;
    }
    g_viol++;
    return &g_reg[0].val;
}

static void settle(void)
{
    if (g_last_ptr != NULL && *g_last_ptr != g_last_before) {
        if (g_nwlog < LOG_MAX) {
            g_wlog[g_nwlog].addr = g_last_addr;
            g_wlog[g_nwlog].val = *g_last_ptr;
            g_nwlog++;
        }
        if (g_last_addr == A_DMA5_CHCFG) {
            if ((*g_last_ptr & 1u) != 0u && *entry(A_DMA5_TXNUM) != 0u) {
                g_dma_active = true;
            } else if ((*g_last_ptr & 1u) == 0u) {
                g_dma_active = false;
            }
        }
    }
    g_last_ptr = NULL;
}

uint32_t *km_led_sim_reg32(uintptr_t addr)
{
    settle();
    g_nacc++;
    if (!allowed(addr)) {
        g_viol++;
    }
    if (addr >= 0x4002D400u && addr < 0x4002D800u) {
        g_gpiof_acc++;
    }
    uint32_t *p = entry(addr);
    if ((addr == A_DMA5_CHCFG || addr == A_DMA5_TXNUM) && g_dma_active && g_dma_auto) {   /* the transfer ends by itself */
        *entry(A_DMA5_TXNUM) = 0u;
        *entry(A_DMA5_CHCFG) &= ~1u;
        g_dma_active = false;
    }
    g_last_ptr = p;
    g_last_before = *p;
    g_last_addr = addr;
    return p;
}

static void sim_reset(unsigned seed_mode)
{
    g_nreg = g_nwlog = g_nacc = g_viol = g_gpiof_acc = 0;
    g_last_ptr = NULL;
    g_seed_mode = seed_mode;
    g_dma_auto = g_dma_active = false;
}

static uint32_t reg_now(uintptr_t addr)
{
    settle();
    return *entry(addr);
}

static void sim_dma_finish(void)
{
    settle();
    *entry(A_DMA5_TXNUM) = 0u;
    *entry(A_DMA5_CHCFG) &= ~1u;
    g_dma_active = false;
}

/* index of the first / last logged write to an address (-1: none) */
static int first_write(uintptr_t addr)
{
    for (unsigned i = 0; i < g_nwlog; i++) {
        if (g_wlog[i].addr == addr) {
            return (int)i;
        }
    }
    return -1;
}

static int last_write(uintptr_t addr)
{
    for (int i = (int)g_nwlog - 1; i >= 0; i--) {
        if (g_wlog[i].addr == addr) {
            return i;
        }
    }
    return -1;
}

static unsigned writes_to_range(uintptr_t lo, uintptr_t hi)
{
    unsigned n = 0;
    for (unsigned i = 0; i < g_nwlog; i++) {
        n += (g_wlog[i].addr >= lo && g_wlog[i].addr < hi);
    }
    return n;
}

/* After a change, only the bits of `mask` may differ from the seed. */
static void only_bits(uintptr_t addr, uint32_t mask)
{
    const uint32_t after = reg_now(addr);
    const uint32_t before = seed_value(addr);
    if (((after ^ before) & ~mask) != 0u) {
        printf("  FAIL only_bits %08lx: seed %08x now %08x allowed %08x\n", (unsigned long)addr, (unsigned)before, (unsigned)after, (unsigned)mask);
        km_test_failures++;
    }
    km_test_checks++;
}

static bool dark_stream(const uint8_t *f)
{
    for (unsigned i = 0; i < KM_LED_COUNT * KM_LED_BYTES_PER_LED; i++) {
        if (f[KM_LED_LEAD_BYTES + i] != 0x88u) {
            return false;
        }
    }
    return true;
}

static uint8_t g_dark[KM_LED_FRAME_BYTES];

static void make_dark(void)
{
    memset(g_dark, 0, sizeof g_dark);
    memset(g_dark + KM_LED_LEAD_BYTES, 0x88, KM_LED_COUNT * KM_LED_BYTES_PER_LED);
}

/* ---------------------------------------------------------------- the driver */

/* KM_LED_AUDIT=1 prints every register the driver touched over the whole run (the exact list for the address audit in LED_NOTES.md) */
static struct {
    uintptr_t addr;
    bool      written;
} g_audit[64];
static unsigned g_naudit;

static void audit_collect(void)
{
    settle();
    for (unsigned i = 0; i < g_nreg; i++) {
        unsigned k = 0;
        while (k < g_naudit && g_audit[k].addr != g_reg[i].addr) {
            k++;
        }
        if (k == g_naudit && g_naudit < 64u) {
            g_audit[g_naudit].addr = g_reg[i].addr;
            g_audit[g_naudit++].written = false;
        }
        for (unsigned w = 0; w < g_nwlog; w++) {
            if (g_wlog[w].addr == g_reg[i].addr && k < g_naudit) {
                g_audit[k].written = true;
            }
        }
    }
}

static void audit_print(void)
{
    if (getenv("KM_LED_AUDIT") == NULL) {
        return;
    }
    for (unsigned i = 0; i < g_naudit; i++) {
        for (unsigned j = i + 1; j < g_naudit; j++) {
            if (g_audit[j].addr < g_audit[i].addr) {
                const uintptr_t a = g_audit[i].addr;
                const bool w = g_audit[i].written;
                g_audit[i] = g_audit[j];
                g_audit[j].addr = a;
                g_audit[j].written = w;
            }
        }
    }
    for (unsigned i = 0; i < g_naudit; i++) {
        printf("  AUDIT %08lx %s\n", (unsigned long)g_audit[i].addr, g_audit[i].written ? "R/W" : "R");
    }
}

static void test_driver_full(unsigned seed)
{
    make_dark();
    sim_reset(seed);
    led_hw_test_reset();
    CHECK(led_hw_start(g_dark, KM_LED_FRAME_BYTES, 1000));
    settle();
    CHECK_EQ(g_viol, 0);

    /* clocks: only ever OR-ed in */
    only_bits(0x40021018u, 0x80u);
    only_bits(0x40021020u, 0x5u);
    only_bits(0x4002103Cu, 0x22u);
    CHECK(reg_now(0x40021018u) & 0x80u);
    CHECK((reg_now(0x40021020u) & 0x5u) == 0x5u);

    /* PB8: AF mode (2), pull-up (1), push-pull, AF 0; no other pin of the port changes */
    only_bits(0x4002C400u, 0x30000u);
    only_bits(0x4002C404u, 0x30000u);
    only_bits(0x4002C408u, 0x100u);
    only_bits(0x4002C414u, 0x1Fu);
    only_bits(0x4002C41Cu, 0x30000u);
    only_bits(0x4002C420u, 0x100u);
    CHECK_EQ((reg_now(0x4002C400u) >> 16) & 3u, 2);
    CHECK_EQ((reg_now(0x4002C404u) >> 16) & 3u, 1);
    CHECK_EQ((reg_now(0x4002C408u) >> 8) & 1u, 0);
    CHECK_EQ(reg_now(0x4002C414u) & 0x1Fu, 0);

    /* PF5: output, open drain, no pull, driven low; no other pin of the port changes */
    only_bits(0x4002D400u, 0xC00u);
    only_bits(0x4002D404u, 0xC00u);
    only_bits(0x4002D408u, 0x20u);
    only_bits(0x4002D41Cu, 0xC00u);
    only_bits(0x4002D420u, 0x20u);
    CHECK_EQ((reg_now(0x4002D400u) >> 10) & 3u, 1);
    CHECK_EQ((reg_now(0x4002D408u) >> 5) & 1u, 1);
    CHECK_EQ((reg_now(0x4002D404u) >> 10) & 3u, 0);
    CHECK(last_write(0x4002D428u) >= 0 && g_wlog[last_write(0x4002D428u)].val == 0x20u);     /* PBC: pin low */
    CHECK_EQ(first_write(0x4002D424u), -1);                                                 /* PBSC never written: never driven high */

    /* SPI5 */
    CHECK_EQ(reg_now(0x40013000u), (seed_value(0x40013000u) & 0xFFFFF9F7u) | 0xD844u);
    CHECK((reg_now(0x40013004u) & 5u) == 5u);
    CHECK_EQ(reg_now(0x40013018u) & 0xFFFFu, 7);
    CHECK_EQ(reg_now(0x40013018u) >> 16, seed_value(0x40013018u) >> 16);
    CHECK_EQ(reg_now(0x4001301Cu) & 2u, 0);
    CHECK_EQ(reg_now(0x4001301Cu) | 2u, seed_value(0x4001301Cu) | 2u);                      /* nothing else cleared */

    /* DMA1 channel 5 */
    CHECK_EQ(reg_now(0x40020050u) & 0x3091u, 0x3091);                                       /* mem->periph, mem increment, priority 3, enabled */
    CHECK_EQ(reg_now(0x40020060u), KM_LED_FRAME_BYTES);
    CHECK_EQ(reg_now(0x40020058u), 0x4001300Cu);                                            /* the SPI data register */
    CHECK_EQ(reg_now(0x40020054u), (uint32_t)(uintptr_t)g_dark);
    CHECK_EQ(reg_now(0x4002005Cu), 0x20);                                                   /* request select: SPI5 TX */
    CHECK_EQ(reg_now(0x400200A8u), 1);
    CHECK(last_write(0x400200A0u) >= 0 && g_wlog[last_write(0x400200A0u)].val == 0xF0000u);   /* flags of channel 5 only */

    /* order: pins and DMA first, the SPI is enabled last */
    const int pf5 = first_write(0x4002D428u), pb8 = first_write(0x4002C400u);
    const int spi_cfg = first_write(0x40013000u), dma_en = last_write(0x40020050u);
    const int spi_en = last_write(0x40013004u);
    if (seed == 1) {                                                                        /* (with all bits set already, an OR leaves no trace) */
        CHECK(pf5 >= 0 && pb8 >= 0 && spi_cfg >= 0 && dma_en >= 0 && spi_en >= 0);
        CHECK(pf5 < pb8 && pb8 < spi_cfg && spi_cfg < dma_en && dma_en < spi_en);
    }
    CHECK_EQ(writes_to_range(0x40013000u, 0x40014000u) > 0, 1);

    /* the frame is on its way; it ends by itself */
    CHECK(led_hw_poll(2000) == LED_HW_BUSY);
    CHECK(!led_hw_send(g_dark, KM_LED_FRAME_BYTES, 2000));
    sim_dma_finish();
    CHECK(led_hw_poll(3000) == LED_HW_IDLE);
    CHECK(led_hw_send(g_dark, KM_LED_FRAME_BYTES, 4000));
    CHECK(led_hw_poll(4001) == LED_HW_BUSY);
    sim_dma_finish();
    CHECK(led_hw_poll(5000) == LED_HW_IDLE);
    led_hw_diag_t d;
    led_hw_get_diag(&d);
    CHECK_EQ(d.sent, 2);
    CHECK_EQ(d.done, 2);
    CHECK_EQ(d.timeouts, 0);
    CHECK_EQ(d.spi_ctrl1, reg_now(0x40013000u));

    /* kill: everything is given back, once */
    led_hw_kill();
    settle();
    CHECK_EQ(g_viol, 0);
    CHECK_EQ(reg_now(0x40020050u) & 1u, 0);                                                 /* DMA off */
    CHECK_EQ(reg_now(0x40013004u) & 5u, 0);                                                 /* SPI off */
    CHECK_EQ(reg_now(0x400200A8u), 0);                                                      /* the ADC driver's value again */
    CHECK_EQ((reg_now(0x4002C400u) >> 16) & 3u, 0);                                         /* PB8 back to an input */
    CHECK_EQ((reg_now(0x4002C404u) >> 16) & 3u, 0);
    CHECK(last_write(0x4002D424u) >= 0 && g_wlog[last_write(0x4002D424u)].val == 0x20u);   /* PF5 released (high) */
    only_bits(0x4002C400u, 0x30000u);
    only_bits(0x4002C404u, 0x30000u);
    only_bits(0x4002D400u, 0xC00u);
    const unsigned w = g_nwlog;
    led_hw_kill();
    settle();
    CHECK_EQ(g_nwlog, w);                                                                   /* idempotent */
    CHECK(!led_hw_start(g_dark, KM_LED_FRAME_BYTES, 9000));                                 /* and it stays off */
    CHECK(!led_hw_send(g_dark, KM_LED_FRAME_BYTES, 9000));
    settle();
    CHECK_EQ(g_nwlog, w);
    CHECK_EQ(g_viol, 0);
    led_hw_get_diag(&d);                                                                    /* (read-backs only while started) */
    audit_collect();
}

static void test_driver_timeout(void)
{
    make_dark();
    sim_reset(2);
    led_hw_test_reset();
    CHECK(led_hw_start(g_dark, KM_LED_FRAME_BYTES, 0));
    CHECK(led_hw_poll(LED_HW_TIMEOUT_CYCLES - 1u) == LED_HW_BUSY);
    CHECK(led_hw_poll(LED_HW_TIMEOUT_CYCLES) == LED_HW_TIMEOUT);                            /* aborted */
    CHECK_EQ(reg_now(0x40020050u) & 1u, 0);
    CHECK(led_hw_poll(LED_HW_TIMEOUT_CYCLES + 1u) == LED_HW_IDLE);                          /* reported once */
    CHECK(led_hw_send(g_dark, KM_LED_FRAME_BYTES, LED_HW_TIMEOUT_CYCLES + 2u));             /* and it can send again */
    led_hw_diag_t d;
    led_hw_get_diag(&d);
    CHECK_EQ(d.timeouts, 1);

    /* a cycle counter that wraps in the middle of a transfer */
    sim_reset(2);
    led_hw_test_reset();
    CHECK(led_hw_start(g_dark, KM_LED_FRAME_BYTES, 0xFFFFF000u));
    CHECK(led_hw_poll(0xFFFFF000u + 1000u) == LED_HW_BUSY);
    CHECK(led_hw_poll(0xFFFFF000u + LED_HW_TIMEOUT_CYCLES) == LED_HW_TIMEOUT);
    CHECK_EQ(g_viol, 0);
}

static void test_driver_levels(void)
{
    make_dark();
    /* level 1: PB8 / SPI5 / DMA5 yes, PF5 never touched */
    sim_reset(2);
    l1_reset();
    CHECK(l1_start(g_dark, KM_LED_FRAME_BYTES, 0));
    sim_dma_finish();
    CHECK(l1_poll(10) == LED_HW_IDLE);
    CHECK(l1_send(g_dark, KM_LED_FRAME_BYTES, 20));
    l1_kill();
    settle();
    CHECK_EQ(g_viol, 0);
    CHECK_EQ(g_gpiof_acc, 0);
    CHECK(g_nwlog > 10);
    CHECK(first_write(0x40013004u) >= 0);

    /* level 0 (dry run): no register access at all, the books are kept */
    sim_reset(2);
    l0_reset();
    CHECK(l0_start(g_dark, KM_LED_FRAME_BYTES, 0));
    CHECK(l0_poll(100) == LED_HW_BUSY);
    CHECK(!l0_send(g_dark, KM_LED_FRAME_BYTES, 100));
    CHECK(l0_poll(2u * 240000u) == LED_HW_IDLE);
    CHECK(l0_send(g_dark, KM_LED_FRAME_BYTES, 2u * 240000u));
    l0_kill();
    settle();
    CHECK_EQ(g_nacc, 0);
    CHECK_EQ(g_nwlog, 0);
}

/* ---------------------------------------------------------------- the service */

static km_engine_t g_eng;
static uint32_t g_cyc;                  /* the DWT counter handed to the service */

static void boot(unsigned seed)
{
    sim_reset(seed);
    led_hw_test_reset();
    km_engine_init(&g_eng);
    g_dma_auto = true;
    m5_led_init();
}

static uint8_t g_down[2];

/* Run the service for `ms` milliseconds, one call every 43 us (a main-loop pass). */
static void run_ms(unsigned ms, uint8_t faults, unsigned n_down)
{
    const unsigned passes = ms * 1000u / 43u;
    for (unsigned i = 0; i < passes; i++) {
        g_cyc += 10320u;
        m5_led_service(g_cyc, &g_eng, g_down, n_down, faults);
    }
}

static uint16_t diag(unsigned i)
{
    uint16_t d[M5_LED_DIAG_VALUES];
    m5_led_diag(d);
    return d[i];
}

static bool hw_started(void)
{
    return (diag(0) & 1u) != 0u;
}

static bool hw_killed(void)
{
    return (diag(0) & 2u) != 0u;
}

static void test_service_start_gating(void)
{
    m5_led_test_power_cycle();
    g_cyc = 5000;
    boot(2);
    run_ms(1400, 0, 0);
    CHECK_EQ(g_nacc, 0);                                           /* nothing touched during the first 1.4 s */
    CHECK(!hw_started());
    run_ms(150, 0, 0);
    CHECK(hw_started());
    CHECK(g_nwlog > 10);
    CHECK_EQ(g_viol, 0);

    /* a fault restarts the clock of the healthy period */
    m5_led_test_power_cycle();
    boot(2);
    run_ms(1000, 0, 0);
    run_ms(5, KM_FAULT_CAL_LOST, 0);
    run_ms(1450, 0, 0);
    CHECK(!hw_started());
    CHECK_EQ(g_nacc, 0);
    run_ms(100, 0, 0);
    CHECK(hw_started());

    /* a keyboard that never becomes healthy never gets lights */
    m5_led_test_power_cycle();
    boot(2);
    run_ms(5000, KM_FAULT_SCAN_STALL, 0);
    CHECK(!hw_started());
    CHECK_EQ(g_nacc, 0);
}

static void test_service_flow(void)
{
    m5_led_test_power_cycle();
    g_cyc = 0xFFFFF000u;                                          /* the cycle counter wraps right at the start */
    boot(2);
    run_ms(1600, 0, 0);
    CHECK(hw_started());
    const uint16_t sent0 = diag(5);
    run_ms(2000, 0, 0);
    const unsigned sent = (unsigned)diag(5) - sent0;
    CHECK(sent >= 90 && sent <= 105);                              /* the demo effect moves: a frame every 20 ms */
    CHECK_EQ(diag(6), 0);                                          /* no timeouts */
    CHECK(diag(9) <= M5_LED_WORK_UNITS);                           /* no pass did more than one slice */
    CHECK(diag(9) > 0);
    CHECK_EQ(g_viol, 0);
    CHECK_EQ(diag(15), (uint16_t)((seed_value(0x40013000u) & 0xFFFFF9F7u) | 0xD844u));      /* the diagnostics see the SPI config */
    CHECK_EQ(diag(17) & 0x3090u, 0x3090);
    CHECK_EQ(diag(10), KM_LED_FX_DEMO);

    /* a fault: lights go dark within a few ms and stay dark; they come back when it ends */
    run_ms(5, KM_FAULT_ENGINE_OVERRUN, 0);
    const uint8_t *f = led_hw_test_last_frame();
    CHECK(f != NULL && dark_stream(f));
    CHECK((diag(0) & 32u) != 0u);
    run_ms(300, 0, 0);
    CHECK(!dark_stream(led_hw_test_last_frame()));
    CHECK(!hw_killed());

    /* the bus suspended: dark, but this is not an episode and never kills */
    for (unsigned i = 0; i < 6; i++) {
        run_ms(200, KM_FAULT_USB_SUSPENDED, 0);
        CHECK(dark_stream(led_hw_test_last_frame()));
        run_ms(200, 0, 0);
    }
    CHECK(!hw_killed());
    CHECK_EQ(diag(2) >> 8, 1);                                     /* only the one engine overrun episode above */

    /* run time configuration: static red shows up in the stream */
    km_led_cfg_t c;
    m5_led_get_cfg(&c);
    CHECK_EQ(c.effect, KM_LED_FX_DEMO);
    c.effect = KM_LED_FX_STATIC;
    c.color.r = 200;
    c.color.g = 0;
    c.color.b = 0;
    CHECK(m5_led_set_cfg(&c));
    run_ms(150, 0, 0);
    CHECK_EQ(diag(10), KM_LED_FX_STATIC);
    const uint8_t *s = led_hw_test_last_frame();
    CHECK(s != NULL && !dark_stream(s));
    c.effect = 99;
    CHECK(!m5_led_set_cfg(&c));
    CHECK_EQ(diag(10), KM_LED_FX_STATIC);                          /* unchanged */

    /* the same through the configuration protocol's light ops */
    const km_config_led_ops_t *ops = m5_led_cfg_ops();
    uint8_t v[KM_CONFIG_LED_BYTES];
    CHECK_EQ(ops->get(ops->ctx, v), KM_CFG_OK);
    CHECK(v[0] == KM_LED_FX_STATIC && v[3] == 200);
    static const uint8_t good[6] = { KM_LED_FX_RAINBOW, 100, 50, 9, 8, 7 }, bad[6] = { 50, 100, 50, 9, 8, 7 };
    CHECK_EQ(ops->set(ops->ctx, good), KM_CFG_OK);
    CHECK_EQ(diag(10), KM_LED_FX_RAINBOW);
    CHECK_EQ(diag(11), 100);
    CHECK_EQ(diag(12), 50);
    CHECK_EQ(ops->set(ops->ctx, bad), KM_CFG_ERR_RANGE);
    CHECK_EQ(diag(10), KM_LED_FX_RAINBOW);                         /* unchanged */
    CHECK_EQ(ops->get(ops->ctx, v), KM_CFG_OK);
    CHECK(memcmp(v, good, 6) == 0);
}

static void test_service_kill_dma_hang(void)
{
    m5_led_test_power_cycle();
    g_cyc = 0;
    boot(2);
    g_dma_auto = false;                                            /* the DMA never completes */
    run_ms(1510, 0, 0);
    CHECK(hw_started());                                           /* started, and the first frame (the dark one) hangs */
    run_ms(150, 0, 0);                                             /* three timeouts of 5 ms plus frame pacing */
    CHECK(hw_killed());
    CHECK_EQ(diag(6), 3);
    CHECK_EQ(diag(1) & 0xFFu, KM_LED_WHY_DMA);
    CHECK_EQ(reg_now(0x40020050u) & 1u, 0);
    CHECK_EQ(reg_now(0x40013004u) & 5u, 0);
    CHECK_EQ((reg_now(0x4002C400u) >> 16) & 3u, 0);
    CHECK(last_write(0x4002D424u) >= 0);                           /* PF5 released */
    const unsigned acc = g_nacc;
    run_ms(2000, 0, 0);
    CHECK_EQ(g_nacc, acc);                                         /* and the LED code never touches a register again */
    CHECK_EQ(g_viol, 0);
}

static void test_service_kill_faults(void)
{
    m5_led_test_power_cycle();
    g_cyc = 0;
    boot(2);
    run_ms(1600, 0, 0);
    CHECK(hw_started());
    for (unsigned i = 0; i < 2; i++) {
        run_ms(30, KM_FAULT_SCAN_STALL, 0);
        run_ms(30, 0, 0);
        CHECK(!hw_killed());
    }
    run_ms(30, KM_FAULT_DMA_ERROR, 0);                             /* the third episode */
    CHECK(hw_killed());
    CHECK_EQ(diag(1) & 0xFFu, KM_LED_WHY_FAULTS);
    CHECK_EQ(reg_now(0x40013004u) & 5u, 0);
    const unsigned acc = g_nacc;
    run_ms(500, 0, 0);
    CHECK_EQ(g_nacc, acc);
}

static void test_service_boot_loop(void)
{
    m5_led_test_power_cycle();

    /* boot 1: the hardware starts, then the keyboard is reset before it is "stable" (3 s) */
    g_cyc = 0;
    boot(2);
    run_ms(1500 + 2000, 0, 0);
    CHECK(hw_started());
    CHECK(!(diag(0) & 8u));

    /* boot 2: same */
    boot(2);
    CHECK_EQ(diag(2) & 0xFFu, 1);
    run_ms(1500 + 2000, 0, 0);
    CHECK(hw_started());

    /* boot 3: the guard keeps the LED off; the keyboard side is not touched at all */
    boot(2);
    CHECK(diag(0) & 4u);
    CHECK_EQ(diag(1) & 0xFFu, KM_LED_WHY_BOOT_LOOP);
    run_ms(10000, 0, 0);
    CHECK_EQ(g_nacc, 0);
    CHECK(!hw_started());

    /* a power cycle clears it */
    m5_led_test_power_cycle();
    boot(2);
    run_ms(1700, 0, 0);
    CHECK(hw_started());

    /* a boot that gets to "stable" resets the count */
    run_ms(3500, 0, 0);
    CHECK(diag(0) & 8u);
    boot(2);
    CHECK_EQ(diag(2) & 0xFFu, 0);
    run_ms(1700, 0, 0);
    CHECK(hw_started());
}

/* ---- the layers through the service (M6 P6c): host LED source, inputs, scene, external frames, scene persistence ---- */

static unsigned stream_channel(const uint8_t *p)
{
    unsigned v = 0;
    for (unsigned i = 0; i < 4; i++) {
        const unsigned hi = (p[i] >> 4) == 0xEu ? 1u : 0u, lo = (p[i] & 0xFu) == 0xEu ? 1u : 0u;
        v = (v << 2) | (hi << 1) | lo;
    }
    return v;
}

static km_led_rgb_t stream_px(const uint8_t *f, unsigned i)
{
    const uint8_t *p = f + KM_LED_LEAD_BYTES + i * KM_LED_BYTES_PER_LED;
    km_led_rgb_t c;
#if KM_LED_WIRE_ORDER == KM_LED_WIRE_RGB
    c.r = (uint8_t)stream_channel(p);
    c.g = (uint8_t)stream_channel(p + 4);
#else
    c.g = (uint8_t)stream_channel(p);
    c.r = (uint8_t)stream_channel(p + 4);
#endif
    c.b = (uint8_t)stream_channel(p + 8);
    return c;
}

static uint8_t g_host_bits;

static uint8_t host_source(void)
{
    return g_host_bits;
}

/* a RAM flash for the two scene sectors */
static uint8_t g_scene_mem[2 * 0x2000];

static bool scene_read(void *ctx, uint32_t addr, uint8_t *dst, uint32_t len)
{
    (void)ctx;
    if (addr < 0x08038000u || addr + len > 0x08038000u + sizeof g_scene_mem) {
        return false;
    }
    memcpy(dst, &g_scene_mem[addr - 0x08038000u], len);
    return true;
}

static bool scene_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (addr < 0x08038000u || addr + 0x2000u > 0x08038000u + sizeof g_scene_mem) {
        return false;
    }
    memset(&g_scene_mem[addr - 0x08038000u], 0xFF, 0x2000u);
    return true;
}

static bool scene_program(void *ctx, uint32_t addr, const uint8_t d[8])
{
    (void)ctx;
    if (addr < 0x08038000u || addr + 8u > 0x08038000u + sizeof g_scene_mem) {
        return false;
    }
    memcpy(&g_scene_mem[addr - 0x08038000u], d, 8);
    return true;
}

static void test_service_layers(void)
{
    m5_led_test_power_cycle();
    g_cyc = 0;
    boot(2);
    run_ms(1700, 0, 0);
    CHECK(hw_started());

    km_led_cfg_t c = { KM_LED_FX_STATIC, 255, 128, { 0, 0, 20 } };
    CHECK(m5_led_set_cfg(&c));
    km_lightscene_t sc, back;
    km_lightscene_default(&sc);
    sc.n_rules = 2;
    sc.rule[0] = (km_ls_rule_t){ KM_LS_SRC_HOST_CAPS, 0, 28, KM_LS_OFF_KEEP, { 60, 10, 5 }, 0 };
    sc.rule[1] = (km_ls_rule_t){ KM_LS_SRC_LAYER, 2, 3, KM_LS_OFF_KEEP, { 0, 40, 0 }, 0 };
    CHECK(m5_led_scene_set(&sc));
    m5_led_scene_get(&back);
    CHECK_EQ(back.n_rules, 2);
    CHECK_EQ(back.rule[1].led, 3);
    km_lightscene_t bad = sc;
    bad.n_rules = 9;
    CHECK(!m5_led_scene_set(&bad));                                           /* refused: the old scene stays */
    m5_led_scene_get(&back);
    CHECK_EQ(back.n_rules, 2);

    /* no host source yet: Caps Lock is "off" whatever the bits say */
    g_host_bits = 2;
    run_ms(200, 0, 0);
    const uint8_t *f = led_hw_test_last_frame();
    CHECK(f != NULL);
    CHECK_EQ(stream_px(f, 28).r, 0);
    CHECK(stream_px(f, 28).b > 0);

    /* a scene that arrives while Caps Lock is on shows within a few milliseconds (not only at the next slow frame) */
    {
        km_lightscene_t empty;
        km_lightscene_default(&empty);
        CHECK(m5_led_scene_set(&empty));
        m5_led_set_host_leds_source(host_source);
        g_host_bits = 2;
        run_ms(100, 0, 0);
        CHECK_EQ(stream_px(led_hw_test_last_frame(), 28).r, 0);                 /* the empty scene has no rule */
        CHECK(m5_led_scene_set(&sc));
        run_ms(10, 0, 0);
        CHECK(stream_px(led_hw_test_last_frame(), 28).r > 0);
        m5_led_set_host_leds_source(NULL);
        g_host_bits = 2;
        run_ms(100, 0, 0);
    }

    /* with the source, the light follows it, one pass after it changes */
    m5_led_set_host_leds_source(host_source);
    run_ms(100, 0, 0);
    CHECK(stream_px(led_hw_test_last_frame(), 28).r > 0);
    CHECK_EQ(stream_px(led_hw_test_last_frame(), 27).r, 0);
    g_host_bits = 0;
    run_ms(100, 0, 0);
    CHECK_EQ(stream_px(led_hw_test_last_frame(), 28).r, 0);
    g_host_bits = 0xFA;                                                        /* bits above Scroll are not state */
    run_ms(100, 0, 0);
    CHECK(stream_px(led_hw_test_last_frame(), 28).r > 0);
    g_host_bits = 0xF8;
    run_ms(100, 0, 0);
    CHECK_EQ(stream_px(led_hw_test_last_frame(), 28).r, 0);
    g_host_bits = 2;
    run_ms(100, 0, 0);
    CHECK(stream_px(led_hw_test_last_frame(), 28).r > 0);
    m5_led_set_host_leds_source(NULL);                                         /* detached: back to "off" */
    run_ms(100, 0, 0);
    CHECK_EQ(stream_px(led_hw_test_last_frame(), 28).r, 0);

    /* layer / profile / flags */
    CHECK_EQ(stream_px(led_hw_test_last_frame(), 3).g, 0);
    m5_led_set_inputs(&(km_led_inputs_t){ 0, 2, 0, 0 });
    run_ms(100, 0, 0);
    CHECK(stream_px(led_hw_test_last_frame(), 3).g > 0);
    m5_led_set_inputs(&(km_led_inputs_t){ 0, 0, 0, 0 });
    run_ms(100, 0, 0);
    CHECK_EQ(stream_px(led_hw_test_last_frame(), 3).g, 0);

    /* profile and flags */
    {
        km_lightscene_t p2;
        km_lightscene_default(&p2);
        p2.n_rules = 3;
        p2.rule[0] = (km_ls_rule_t){ KM_LS_SRC_PROFILE, 3, 20, KM_LS_OFF_KEEP, { 0, 40, 0 }, 0 };
        p2.rule[1] = (km_ls_rule_t){ KM_LS_SRC_PAD, 0, 21, KM_LS_OFF_KEEP, { 0, 40, 0 }, 0 };
        p2.rule[2] = (km_ls_rule_t){ KM_LS_SRC_TOGGLE, 0, 22, KM_LS_OFF_KEEP, { 0, 40, 0 }, 0 };
        CHECK(m5_led_scene_set(&p2));
        run_ms(100, 0, 0);
        CHECK_EQ(stream_px(led_hw_test_last_frame(), 20).g, 0);
        m5_led_set_inputs(&(km_led_inputs_t){ 0, 0, 3, 0 });
        run_ms(100, 0, 0);
        CHECK(stream_px(led_hw_test_last_frame(), 20).g > 0);
        CHECK_EQ(stream_px(led_hw_test_last_frame(), 21).g, 0);
        m5_led_set_inputs(&(km_led_inputs_t){ 0, 0, 0, KM_LED_IN_PAD });
        run_ms(100, 0, 0);
        CHECK_EQ(stream_px(led_hw_test_last_frame(), 20).g, 0);
        CHECK(stream_px(led_hw_test_last_frame(), 21).g > 0);
        CHECK_EQ(stream_px(led_hw_test_last_frame(), 22).g, 0);
        m5_led_set_inputs(&(km_led_inputs_t){ 0, 0, 0, KM_LED_IN_TOGGLE });
        run_ms(100, 0, 0);
        CHECK(stream_px(led_hw_test_last_frame(), 22).g > 0);
        m5_led_set_inputs(&(km_led_inputs_t){ 0, 0, 0, 0 });
        CHECK(m5_led_scene_set(&sc));
        run_ms(100, 0, 0);
    }

    /* a fault shows nothing, indicators included */
    g_host_bits = 2;
    m5_led_set_host_leds_source(host_source);
    run_ms(100, 0, 0);
    run_ms(100, KM_FAULT_ENGINE_OVERRUN, 0);
    CHECK(dark_stream(led_hw_test_last_frame()));
    run_ms(200, 0, 0);
    CHECK(!dark_stream(led_hw_test_last_frame()));
    m5_led_set_host_leds_source(NULL);

    /* external frames: effect 8 draws the stream frame, gone 1 s after the last push */
    km_led_rgb_t fr[KM_LED_COUNT];
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        fr[i] = (km_led_rgb_t){ 0, 0, 0 };
    }
    fr[40] = (km_led_rgb_t){ 0, 50, 0 };
    c.effect = KM_LED_FX_EXTERNAL;
    CHECK(m5_led_set_cfg(&c));
    run_ms(100, 0, 0);
    CHECK(dark_stream(led_hw_test_last_frame()));                              /* no frame yet */
    m5_led_ext_rgb(KM_LED_EXT_STREAM, fr);
    run_ms(100, 0, 0);
    CHECK(stream_px(led_hw_test_last_frame(), 40).g > 0);
    CHECK_EQ(stream_px(led_hw_test_last_frame(), 41).g, 0);
    run_ms(700, 0, 0);
    CHECK(stream_px(led_hw_test_last_frame(), 40).g > 0);                      /* 0.8 s without a push: still there */
    run_ms(400, 0, 0);
    CHECK(dark_stream(led_hw_test_last_frame()));                              /* 1.2 s: gone */
    uint8_t gray[KM_LED_COUNT];
    memset(gray, 0, sizeof gray);
    gray[10] = 255;
    c.effect = KM_LED_FX_ANIM;
    c.color = (km_led_rgb_t){ 0, 0, 30 };
    CHECK(m5_led_set_cfg(&c));
    m5_led_ext_gray(KM_LED_EXT_ANIM, gray);
    run_ms(100, 0, 0);
    CHECK(stream_px(led_hw_test_last_frame(), 10).b > 0);
    CHECK_EQ(stream_px(led_hw_test_last_frame(), 11).b, 0);
    m5_led_ext_end(KM_LED_EXT_ANIM);
    run_ms(100, 0, 0);
    CHECK(dark_stream(led_hw_test_last_frame()));
    c.effect = KM_LED_FX_EXTERNAL;                                             /* the stream source takes grey frames too, and expires like any stream frame */
    CHECK(m5_led_set_cfg(&c));
    m5_led_ext_gray(KM_LED_EXT_STREAM, gray);
    run_ms(500, 0, 0);
    CHECK(stream_px(led_hw_test_last_frame(), 10).b > 0);
    run_ms(700, 0, 0);
    CHECK(dark_stream(led_hw_test_last_frame()));
    CHECK_EQ(g_viol, 0);

    /* scene persistence through the two-sector store (a RAM flash here; the real driver is tested in test_store_flash.c) */
    memset(g_scene_mem, 0xFF, sizeof g_scene_mem);
    static const km_store_ops_t ops = { NULL, scene_read, scene_erase, scene_program };
    km_store_t st;
    km_store_init(&st, &ops, 0x08038000u, 0x0803A000u);
    {
        km_lightscene_t other = sc;
        other.n_rules = 1;
        CHECK_EQ(m5_led_scene_commit(&other, true), KM_STORE_ERR_ARG);         /* no store attached: a save request applies nothing */
        m5_led_scene_get(&back);
        CHECK_EQ(back.n_rules, 2);
    }
    CHECK(!m5_led_scene_attach(&st));                                          /* blank: the default scene stays */
    m5_led_scene_get(&back);
    CHECK_EQ(back.n_rules, 2);                                                 /* (the RAM scene of before is untouched by a failed attach) */
    {
        km_lightscene_t other = sc, inv = sc;
        other.n_rules = 1;
        inv.key_idx[3] = 99;
        CHECK_EQ(m5_led_scene_commit(&other, false), KM_STORE_OK);             /* apply only */
        m5_led_scene_get(&back);
        CHECK_EQ(back.n_rules, 1);
        CHECK_EQ(m5_led_scene_commit(&inv, false), KM_STORE_ERR_ARG);          /* invalid: refused with or without a save */
        m5_led_scene_get(&back);
        CHECK_EQ(back.n_rules, 1);
    }
    CHECK_EQ(m5_led_scene_commit(&sc, false), KM_STORE_OK);
    CHECK(!km_lightscene_load(&st, &back));                                    /* ... nothing was written */
    CHECK_EQ(m5_led_scene_commit(&sc, true), KM_STORE_OK);
    CHECK_EQ(m5_led_scene_commit(&sc, true), KM_STORE_UNCHANGED);
    CHECK_EQ(m5_led_scene_commit(&bad, true), KM_STORE_ERR_ARG);               /* invalid: not applied, not saved */
    CHECK(km_lightscene_load(&st, &back));
    CHECK_EQ(back.n_rules, 2);
    sc.rule[0].led = 7;
    sc.rule[0].on.g = 77;
    CHECK_EQ(m5_led_scene_commit(&sc, true), KM_STORE_OK);
    boot(2);                                                                   /* power cycle: the RAM scene is the default again */
    m5_led_scene_get(&back);
    CHECK_EQ(back.n_rules, 0);
    CHECK(m5_led_scene_attach(&st));                                           /* ... and the saved one comes back */
    m5_led_scene_get(&back);
    CHECK_EQ(back.n_rules, 2);
    CHECK_EQ(back.rule[0].led, 7);
    CHECK_EQ(back.rule[0].on.g, 77);
    g_scene_mem[100] ^= 0xFF;                                                  /* damaged records: no scene, no crash */
    g_scene_mem[0x2000 + 100] ^= 0xFF;
    boot(2);
    CHECK(!m5_led_scene_attach(&st));
    m5_led_scene_get(&back);
    CHECK_EQ(back.n_rules, 0);
}

void test_led_hw(void)
{
    for (unsigned seed = 0; seed < 3; seed++) {
        test_driver_full(seed);
    }
    audit_print();
    test_driver_timeout();
    test_driver_levels();
    test_service_start_gating();
    test_service_flow();
    test_service_layers();
    test_service_kill_dma_hang();
    test_service_kill_faults();
    test_service_boot_loop();
}
