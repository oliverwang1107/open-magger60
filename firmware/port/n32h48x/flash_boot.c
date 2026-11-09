#include "flash_boot.h"

#include <stdbool.h>
#include <stdint.h>

#include "km/km_flash_guard.h"
#ifdef KM_FLASH_HOST_TEST            /* tests/test_flash_boot.c replaces the registers with a simulated flash controller */
#include "flash_sim_regs.h"
#else
#include "n32h48x_regs.h"
#endif

#define FLASH_CTRL   KM_REG32(0x40022004u)
#define FLASH_STS    KM_REG32(0x40022008u)
#define FLASH_ADDR   KM_REG32(0x4002200Cu)
#define FLASH_KEY    KM_REG32(0x40022010u)

#define CTRL_PG      (1u << 1)
#define CTRL_PER     (1u << 2)
#define CTRL_STRT    (1u << 4)

#define STS_BUSY     (1u << 1)
#define STS_CLEAR    0x0001003Du                          /* write-1-clear flags, as the bootloader does before each operation */
#define STS_ERROR    (0x4u | 0x8u | 0x10u | 0x20u | 0x10000u)

#define KEY1         0x45670123u
#define KEY2         0xCDEF89ABu

static uint8_t g_sector[KM_FLASH_SECTOR] __attribute__((aligned(8)));

static bool wait_ready(uint32_t spins)
{
    while (FLASH_STS & STS_BUSY) {
        if (spins-- == 0) {
            return false;
        }
    }
    return (FLASH_STS & STS_ERROR) == 0;
}

static bool erase_sector(uint32_t addr)
{
    if (!km_flash_write_allowed(addr, KM_FLASH_SECTOR) || (addr & (KM_FLASH_SECTOR - 1u)) != 0) {
        return false;
    }
    FLASH_STS = STS_CLEAR;
    if (!wait_ready(0x100000u)) {
        return false;
    }
    FLASH_CTRL |= CTRL_PER;
    FLASH_ADDR = addr;
    FLASH_CTRL |= CTRL_STRT;
    const bool ok = wait_ready(0x100000u);
    FLASH_CTRL &= ~CTRL_PER;
    return ok;
}

static bool program_dword(uint32_t addr, uint32_t lo, uint32_t hi)
{
    if (!km_flash_write_allowed(addr, 8) || (addr & 7u) != 0) {
        return false;
    }
    FLASH_STS = STS_CLEAR;
    if (!wait_ready(0x2000u)) {
        return false;
    }
    const uint32_t irq = km_irq_save();                      /* the two stores must not be split by an interrupt */
    FLASH_CTRL |= CTRL_PG;
    KM_REG32(addr) = lo;
    KM_REG32(addr + 4u) = hi;
    const bool ok = wait_ready(0x2000u);
    FLASH_CTRL &= ~CTRL_PG;
    km_irq_restore(irq);
    return ok;
}

static bool rewrite_sector(void)
{
    if (!erase_sector(KM_FLASH_WRITABLE_BASE)) {
        return false;
    }
    for (uint32_t off = 0; off < KM_FLASH_SECTOR; off += 8) {
        const uint32_t lo = *(const uint32_t *)(const void *)&g_sector[off];
        const uint32_t hi = *(const uint32_t *)(const void *)&g_sector[off + 4];
        if (!program_dword(KM_FLASH_WRITABLE_BASE + off, lo, hi)) {
            return false;
        }
        if ((off & 0x3FFu) == 0) {
            km_iwdg_feed();
        }
    }
    for (uint32_t i = 0; i < KM_FLASH_SECTOR; i++) {
        if (KM_READ8(KM_FLASH_WRITABLE_BASE + i) != g_sector[i]) {
            return false;
        }
    }
    return true;
}

void km_flash_request_bootloader(void)
{
    for (uint32_t i = 0; i < KM_FLASH_SECTOR; i++) {
        g_sector[i] = KM_READ8(KM_FLASH_WRITABLE_BASE + i);
    }
    km_iwdg_feed();

    if ((g_sector[0] & KM_BOOT_FLAG_MASK) == 0) {            /* nothing to do if the flag is already set */
        g_sector[0] |= KM_BOOT_FLAG_MASK;                    /* only this bit changes; the rest of the sector is kept */
        FLASH_KEY = KEY1;
        FLASH_KEY = KEY2;
        for (unsigned attempt = 0; attempt < 2; attempt++) { /* RAM copy still intact if the first try fails */
            if (rewrite_sector()) {
                break;
            }
            km_iwdg_feed();
        }
    }

    km_iwdg_feed();
    KM_SYSTEM_RESET();                                       /* SYSRESETREQ: back into the bootloader */
    for (;;) {                                               /* until the reset takes effect; no feeding on purpose */
    }
}
