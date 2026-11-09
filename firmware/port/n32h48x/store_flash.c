#include "store_flash.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "km/km_calib.h"
#include "km/km_flash_guard.h"
#include "km/km_ledprof.h"
#include "km/km_ledstore.h"
#ifdef KM_STORE_HOST_TEST            /* tests/test_store_flash.c replaces the registers with a simulated flash controller */
#include "store_sim_regs.h"
#else
#include "n32h48x_regs.h"
#endif

/* Same controller registers and bits as flash_boot.c. */
#define FLASH_CTRL   KM_REG32(0x40022004u)
#define FLASH_STS    KM_REG32(0x40022008u)
#define FLASH_ADDR   KM_REG32(0x4002200Cu)
#define FLASH_KEY    KM_REG32(0x40022010u)

#define CTRL_PG      (1u << 1)
#define CTRL_PER     (1u << 2)
#define CTRL_STRT    (1u << 4)

#define STS_BUSY     (1u << 1)
#define STS_CLEAR    0x0001003Du
#define STS_ERROR    (0x4u | 0x8u | 0x10u | 0x20u | 0x10000u)

#define KEY1         0x45670123u
#define KEY2         0xCDEF89ABu

#define ERASE_SPINS   0x400000u      /* 4x the bootloader's bound for an erase */
#define PROGRAM_SPINS 0x20000u       /* 16x the bootloader's bound for one 8-byte unit */
#define FEED_MASK     0x0FFFu        /* feed the watchdog every 4096 spins of a wait */

static bool g_unlocked;
static struct { uint16_t stage, sts_lo, sts_hi, ctrl, addr; } g_diag;

static bool fail(uint16_t stage, uint32_t addr)
{
    const uint32_t sts = FLASH_STS;
    g_diag.stage = stage;
    g_diag.sts_lo = (uint16_t)sts;
    g_diag.sts_hi = (uint16_t)(sts >> 16);
    g_diag.ctrl = (uint16_t)FLASH_CTRL;
    g_diag.addr = (uint16_t)addr;
    return false;
}

void km_store_flash_diag(uint16_t out[5])
{
    out[0] = g_diag.stage;
    out[1] = g_diag.sts_lo;
    out[2] = g_diag.sts_hi;
    out[3] = g_diag.ctrl;
    out[4] = g_diag.addr;
}

/* The whole programming window runs from RAM: while the controller works on a unit the CPU must not fetch instructions from the
 * flash (a fetch stalls, and the two word stores of one 8-byte unit then no longer reach the controller back to back), and the two
 * words go out as ONE STRD instruction. Seen on the hardware: with the stores as separate instructions in flash, the second unit of a
 * 16-byte line was refused (programming error) in some builds and accepted in others, depending only on how the code was laid out. */
#ifdef KM_STORE_HOST_TEST
#define KM_RAMFN static
#define STORE_UNIT(addr, lo, hi) do { KM_REG32(addr) = (lo); KM_REG32((addr) + 4u) = (hi); } while (0)
#else
#define KM_RAMFN __attribute__((section(".data.km_ramfn"), noinline, long_call)) static
#define STORE_UNIT(addr, lo, hi) __asm volatile("strd %1, %2, [%0]" : : "r"(addr), "r"(lo), "r"(hi) : "memory")
#endif

/* Returns the status register once the controller is idle again; bit 31 set = it did not become idle within spins. */
KM_RAMFN uint32_t ram_program(uint32_t addr, uint32_t lo, uint32_t hi, uint32_t spins)
{
    FLASH_CTRL |= CTRL_PG;
    STORE_UNIT(addr, lo, hi);
    while ((FLASH_STS & STS_BUSY) != 0u && spins != 0u) {
        spins--;
    }
    const uint32_t sts = FLASH_STS;
    FLASH_CTRL &= ~CTRL_PG;
    return spins == 0u ? (sts | 0x80000000u) : sts;
}

static bool wait_ready(uint32_t spins)
{
    while (FLASH_STS & STS_BUSY) {
        if (spins-- == 0) {
            return false;
        }
        if ((spins & FEED_MASK) == 0) {
            km_iwdg_feed();
        }
    }
    return (FLASH_STS & STS_ERROR) == 0;
}

static void unlock_once(void)
{
    if (!g_unlocked) {
        FLASH_KEY = KEY1;
        FLASH_KEY = KEY2;
        g_unlocked = true;
    }
}

static bool op_read(void *ctx, uint32_t addr, uint8_t *dst, uint32_t len)
{
    (void)ctx;
    if (dst == NULL || !km_flash_store_allowed(addr, len)) {
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        dst[i] = KM_READ8(addr + i);
    }
    return true;
}

static bool op_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (!km_flash_store_allowed(addr, KM_FLASH_SECTOR) || (addr & (KM_FLASH_SECTOR - 1u)) != 0) {
        return false;
    }
    unlock_once();
    FLASH_STS = STS_CLEAR;
    if (!wait_ready(ERASE_SPINS)) {
        return fail(1, addr);
    }
    FLASH_CTRL |= CTRL_PER;
    FLASH_ADDR = addr;
    FLASH_CTRL |= CTRL_STRT;
    const bool ok = wait_ready(ERASE_SPINS);
    if (!ok) {
        (void)fail(2, addr);
    }
    FLASH_CTRL &= ~CTRL_PER;
    if (!ok) {
        return false;
    }
    for (uint32_t i = 0; i < KM_FLASH_SECTOR; i++) {           /* blank check: a sector that is not erased is a failure */
        if (KM_READ8(addr + i) != 0xFFu) {
            return fail(3, addr + i);
        }
        if ((i & 0x3FFu) == 0) {
            km_iwdg_feed();
        }
    }
    return true;
}

static bool op_program(void *ctx, uint32_t addr, const uint8_t data[8])
{
    (void)ctx;
    if (data == NULL || !km_flash_store_allowed(addr, 8) || (addr & 7u) != 0) {
        return false;
    }
    unlock_once();
    uint32_t lo, hi;
    memcpy(&lo, data, 4);
    memcpy(&hi, data + 4, 4);
    FLASH_STS = STS_CLEAR;
    if (!wait_ready(PROGRAM_SPINS)) {
        return fail(4, addr);
    }
    /* The two word stores must reach the controller back to back: an interrupt between them (the sensing ISR runs
     * every ~39 us) leaves the unit half written and the controller reports a programming error. Interrupts stay
     * masked from the first store until the controller is done with this one 8-byte unit. */
    const uint32_t irq = km_irq_save();
    const uint32_t sts = ram_program(addr, lo, hi, PROGRAM_SPINS);
    const bool ok = (sts & (0x80000000u | STS_BUSY | STS_ERROR)) == 0u;
    if (!ok) {
        (void)fail(5, addr);
    }
    km_irq_restore(irq);
    if (!ok) {
        return false;
    }
    for (unsigned i = 0; i < 8; i++) {                         /* read back every unit */
        if (KM_READ8(addr + i) != data[i]) {
            return fail(6, addr + i);
        }
    }
    return true;
}

const km_store_ops_t *km_store_flash_ops(void)
{
    static const km_store_ops_t ops = { NULL, op_read, op_erase, op_program };
    return &ops;
}

const km_store_t *km_store_flash(void)
{
    static km_store_t st;
    static bool ready;
    if (!ready) {
        km_store_init(&st, km_store_flash_ops(), KM_FLASH_SETTINGS_A, KM_FLASH_SETTINGS_B);
        ready = true;
    }
    return &st;
}

const km_store_t *km_lightscene_store_flash(void)
{
    static km_store_t st;
    static bool ready;
    if (!ready) {
        km_store_init(&st, km_store_flash_ops(), KM_FLASH_SCENE_A, KM_FLASH_SCENE_B);
        ready = true;
    }
    return &st;
}

const km_clog_t *km_calstore_flash(void)
{
    static km_clog_t log;
    static bool ready;
    if (!ready) {
        ready = km_clog_init(&log, km_store_flash_ops(), KM_FLASH_CALIBRATION, KM_CAL_PAYLOAD_BYTES);
    }
    return ready ? &log : NULL;
}

const km_clog_t *km_ledstore_flash(void)
{
    static km_clog_t log;
    static bool ready;
    if (!ready) {
        ready = km_clog_init(&log, km_store_flash_ops(), KM_FLASH_LED_CONFIG, KM_LEDSTORE_PAYLOAD_BYTES);
    }
    return ready ? &log : NULL;
}

const km_clog_t *km_ledprof_flash(void)
{
    static km_clog_t log;
    static bool ready;
    if (!ready) {
        ready = km_clog_init(&log, km_store_flash_ops(), KM_FLASH_LED_CONFIG, KM_LEDPROF_PAYLOAD_BYTES);
    }
    return ready ? &log : NULL;
}
