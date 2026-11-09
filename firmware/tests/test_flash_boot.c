/*
 * Runs the REAL port/n32h48x/flash_boot.c against a simulated flash and flash controller (registers and flash
 * window are accessed through km_sim_reg32 / km_sim_read8, see tests/sim/flash_sim_regs.h). The simulation
 * follows the bootloader's use of the controller: unlock with the two keys, clear the status flags, PER + ADDR +
 * STRT erases an 8 KB sector, PG + two word writes program 8 bytes, programming can only clear bits.
 */
#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>

#include "flash_boot.h"
#include "km_test.h"

#define FLASH_SIZE 0x80000u
#define FLASH_BASE 0x08000000u
#define REG_CTRL   0x40022004u
#define REG_STS    0x40022008u
#define REG_ADDR   0x4002200Cu
#define REG_KEY    0x40022010u

typedef struct { int kind; uint32_t addr; } sim_op_t;   /* kind: 0 erase, 1 program */

static uint8_t g_flash[FLASH_SIZE];
static uint32_t g_ctrl, g_sts, g_addr, g_key;
static int g_key_state;
static bool g_unlocked;
static sim_op_t g_ops[4096];
static unsigned g_nops;
static unsigned g_violations, g_resets, g_feeds;
static unsigned g_pending_kind;                 /* 0 none, 1 key, 2 flash word */
static uint32_t g_pending_addr, g_tmp, g_pending_orig;
static int g_fail_program_at;                   /* sim program number (1-based) that reports an error, 0 = never */
static int g_corrupt_program_at;                /* sim program number after which one stored bit silently reads wrong */
static int g_fail_erase;                        /* number of erases that report an error */
static unsigned g_program_count;
static jmp_buf g_reset_jmp;

static void commit_pending(void)
{
    if (g_pending_kind == 1) {
        if (g_tmp == 0x45670123u) {
            g_key_state = 1;
        } else if (g_tmp == 0xCDEF89ABu && g_key_state == 1) {
            g_unlocked = true;
            g_key_state = 0;
        } else {
            g_key_state = 0;
        }
    } else if (g_pending_kind == 3) {
        if (g_tmp != g_pending_orig) {
            g_sts &= ~g_tmp;                     /* the status flags are write-1-clear */
        }
    } else if (g_pending_kind == 2) {
        const uint32_t a = g_pending_addr - FLASH_BASE;
        if (!(g_ctrl & 2u) || !g_unlocked) {
            g_violations++;                      /* a flash write without PG or without unlock */
        } else {
            for (unsigned b = 0; b < 4; b++) {
                const uint8_t nv = (uint8_t)(g_tmp >> (8 * b));
                if (g_flash[a + b] != 0xFF && nv != 0xFF && g_flash[a + b] != nv) {
                    g_violations++;              /* programming over data that was not erased */
                }
                g_flash[a + b] &= nv;
            }
            if ((g_pending_addr & 4u) == 0) {
                g_ops[g_nops < 4096 ? g_nops++ : 0] = (sim_op_t){ 1, g_pending_addr };
                g_program_count++;
                if (g_corrupt_program_at != 0 && (int)g_program_count == g_corrupt_program_at) {
                    for (unsigned b = 0; b < 4; b++) {     /* status says OK, but one stored bit is wrong */
                        if (g_flash[a + b] != 0) {
                            g_flash[a + b] = (uint8_t)(g_flash[a + b] & (g_flash[a + b] - 1u));
                            break;
                        }
                    }
                }
                if (g_fail_program_at != 0 && (int)g_program_count == g_fail_program_at) {
                    g_sts |= 0x4u;               /* programming error flag */
                }
            }
        }
    }
    g_pending_kind = 0;
}

static void run_erase_if_started(void)
{
    if ((g_ctrl & 4u) && (g_ctrl & 0x10u)) {
        g_ctrl &= ~0x10u;
        if (!g_unlocked || g_addr < FLASH_BASE || g_addr >= FLASH_BASE + FLASH_SIZE || (g_addr & 0x1FFFu) != 0) {
            g_violations++;
            return;
        }
        memset(&g_flash[g_addr - FLASH_BASE], 0xFF, 0x2000);
        g_ops[g_nops < 4096 ? g_nops++ : 0] = (sim_op_t){ 0, g_addr };
        if (g_fail_erase > 0) {
            g_fail_erase--;
            g_sts |= 0x4u;
        }
    }
}

uint32_t *km_sim_reg32(uintptr_t addr)
{
    commit_pending();
    switch (addr) {
    case REG_CTRL: return &g_ctrl;
    case REG_ADDR: return &g_addr;
    case REG_STS:
        run_erase_if_started();
        g_sts &= ~0x2u;                          /* never busy: operations complete instantly */
        g_tmp = g_pending_orig = g_sts;
        g_pending_kind = 3;
        return &g_tmp;
    case REG_KEY:
        g_pending_kind = 1;
        return &g_tmp;
    default:
        break;
    }
    if (addr >= FLASH_BASE && addr + 4 <= FLASH_BASE + FLASH_SIZE) {
        g_pending_kind = 2;
        g_pending_addr = (uint32_t)addr;
        g_tmp = 0xFFFFFFFFu;
        return &g_tmp;                           /* the caller writes the word; it is committed at the next access */
    }
    printf("  unexpected register access 0x%08X\n", (unsigned)addr);
    g_violations++;
    static uint32_t scratch;
    return &scratch;
}

uint8_t km_sim_read8(uintptr_t addr)
{
    commit_pending();
    if (addr < FLASH_BASE || addr >= FLASH_BASE + FLASH_SIZE) {
        g_violations++;
        return 0;
    }
    return g_flash[addr - FLASH_BASE];
}

void km_sim_reset(void)
{
    commit_pending();
    g_resets++;
    longjmp(g_reset_jmp, 1);
}

void km_sim_feed(void)
{
    g_feeds++;
}

static void sim_init(uint8_t flag_byte)
{
    for (uint32_t i = 0; i < FLASH_SIZE; i++) {
        g_flash[i] = (uint8_t)((i * 2654435761u) >> 24);       /* deterministic non-trivial content everywhere */
    }
    memset(&g_flash[0x6000], 0xFF, 0x2000);                      /* the bootloader's status sector is erased */
    g_flash[0x0802C000u - FLASH_BASE] = flag_byte;
    g_ctrl = g_sts = g_addr = g_key = 0;
    g_key_state = 0;
    g_unlocked = false;
    g_nops = g_violations = g_resets = g_feeds = g_program_count = 0;
    g_pending_kind = 0;
    g_fail_program_at = 0;
    g_corrupt_program_at = 0;
    g_fail_erase = 0;
}

static bool run_request(void)
{
    if (setjmp(g_reset_jmp) == 0) {
        km_flash_request_bootloader();
        return false;                            /* cannot happen: the function never returns without a reset */
    }
    return true;
}

static bool outside_sector_unchanged(const uint8_t *before)
{
    for (uint32_t i = 0; i < FLASH_SIZE; i++) {
        const uint32_t a = FLASH_BASE + i;
        if ((a < 0x0802C000u || a >= 0x0802E000u) && g_flash[i] != before[i]) {
            return false;
        }
    }
    return true;
}

void test_flash_boot(void)
{
    static uint8_t before[FLASH_SIZE];

    /* normal case: flag clear -> exactly one erase and 1024 programs inside the sector, flag set, rest untouched */
    sim_init(0x50);
    memcpy(before, g_flash, sizeof before);
    CHECK(run_request());
    CHECK_EQ(g_resets, 1);
    CHECK_EQ(g_violations, 0);
    CHECK(outside_sector_unchanged(before));
    CHECK_EQ(g_flash[0x0802C000u - FLASH_BASE], 0x51);
    CHECK(memcmp(&g_flash[0x0802C001u - FLASH_BASE], &before[0x0802C001u - FLASH_BASE], 0x1FFF) == 0);
    unsigned erases = 0, programs = 0;
    for (unsigned i = 0; i < g_nops; i++) {
        CHECK(g_ops[i].addr >= 0x0802C000u && g_ops[i].addr < 0x0802E000u);
        erases += g_ops[i].kind == 0;
        programs += g_ops[i].kind == 1;
    }
    CHECK_EQ(erases, 1);
    CHECK_EQ(programs, 1024);
    CHECK(g_feeds > 0);

    /* other starting bytes: only bit 0 changes */
    for (unsigned v = 0; v < 256; v += 2) {
        sim_init((uint8_t)v);
        CHECK(run_request());
        CHECK_EQ(g_flash[0x0802C000u - FLASH_BASE], v | 1u);
        CHECK_EQ(g_violations, 0);
    }

    /* flag already set: nothing is erased or programmed, the reset still happens */
    sim_init(0x51);
    memcpy(before, g_flash, sizeof before);
    CHECK(run_request());
    CHECK_EQ(g_nops, 0);
    CHECK_EQ(g_resets, 1);
    CHECK(memcmp(g_flash, before, sizeof before) == 0);

    /* one programming error: the second attempt repairs it */
    sim_init(0x50);
    memcpy(before, g_flash, sizeof before);
    g_fail_program_at = 100;
    CHECK(run_request());
    CHECK_EQ(g_flash[0x0802C000u - FLASH_BASE], 0x51);
    CHECK(memcmp(&g_flash[0x0802C001u - FLASH_BASE], &before[0x0802C001u - FLASH_BASE], 0x1FFF) == 0);
    CHECK(outside_sector_unchanged(before));
    CHECK_EQ(g_violations, 0);
    erases = 0;
    for (unsigned i = 0; i < g_nops; i++) {
        erases += g_ops[i].kind == 0;
        CHECK(g_ops[i].addr >= 0x0802C000u && g_ops[i].addr < 0x0802E000u);
    }
    CHECK_EQ(erases, 2);

    /* silent corruption (status OK but a bit stored wrong): the read-back catches it and the second attempt repairs it */
    sim_init(0x50);
    memcpy(before, g_flash, sizeof before);
    g_corrupt_program_at = 200;
    CHECK(run_request());
    CHECK_EQ(g_flash[0x0802C000u - FLASH_BASE], 0x51);
    CHECK(memcmp(&g_flash[0x0802C001u - FLASH_BASE], &before[0x0802C001u - FLASH_BASE], 0x1FFF) == 0);
    erases = 0;
    for (unsigned i = 0; i < g_nops; i++) {
        erases += g_ops[i].kind == 0;
    }
    CHECK_EQ(erases, 2);

    /* a failing erase on the first attempt */
    sim_init(0x50);
    memcpy(before, g_flash, sizeof before);
    g_fail_erase = 1;
    CHECK(run_request());
    CHECK_EQ(g_flash[0x0802C000u - FLASH_BASE], 0x51);
    CHECK(outside_sector_unchanged(before));

    /* errors on both attempts: gives up, still resets, still writes nothing outside the sector */
    sim_init(0x50);
    memcpy(before, g_flash, sizeof before);
    g_fail_erase = 5;
    CHECK(run_request());
    CHECK_EQ(g_resets, 1);
    CHECK(outside_sector_unchanged(before));
    CHECK_EQ(g_violations, 0);
    for (unsigned i = 0; i < g_nops; i++) {
        CHECK(g_ops[i].addr >= 0x0802C000u && g_ops[i].addr < 0x0802E000u);
    }
}
