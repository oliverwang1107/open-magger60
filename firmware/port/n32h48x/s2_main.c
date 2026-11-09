/*
 * S2 image (M3_PLAN.md section 3, step S2): S1's bootloader-style polled setup for all four ADCs, then a software scan
 * of all 76 slots: for each mux phase 0..3 set PB3/PB4, wait SETTLE_US, convert every channel one after the other (no DMA,
 * no timer, no interrupts); the four WASD slots are converted as ordinary regular channels (their sensors hang directly
 * on ADC pins, no mux). Slot s = 2 * word + half, see sense_tables.h.
 *
 * Revision 2 (after the first S2 build failed to enumerate on the real keyboard, see SENSING_BRINGUP_NOTES.md section 8):
 *   - USB is serviced from inside every wait of the sensing code (idle hook, at least every 50 us), so no ADC problem can
 *     starve the polled USB driver again; all waits are time-bounded, and an ADC that does not answer is skipped;
 *   - if an ADC does not become ready, a second bring-up with CFG2 bit 19 set is tried (diagnostic);
 *   - progress markers: the stage of the sensing code is kept in RAM that survives a watchdog reset, and this image counts
 *     its own starts, so a reset loop shows up as "boots > 1" and a dead start as "previous stage".
 *
 * Published after every scan:
 *   km_scope_publish tag 0x0002: the 76 raw 12-bit readings in slot order (0xFFFF = ADC not usable / conversion timed out)
 * and tag 0x0100 (diagnostics, 16 values) once right after the init, then every 512th scan (the tag 0x0002 snapshot is published every 20 ms):
 *   [0] starts since power-up (RAM marker)   [1] stage the previous start reached   [2] stage now
 *   [3] ADC usable mask (bits 0-3) | never-ready mask << 4      [4] calibration-failed mask | alt-clock result << 4
 *   [5] conversion timeouts   [6] sense_poll_init error bits   [7] alt-clock retry error bits
 *   [8] longest gap between two USB polls in us (clamped)   [9..10] scans completed (lo, hi)
 *   [11..12] last scan time in us (lo, hi)   [13] SETTLE_US   [14] 0   [15] 0x00D2 (layout version)
 * Stage numbers: sense_hw.h SENSE_STAGE_*; 1 = main entered, 2 = USB initialised.
 */
#include <stdbool.h>

#include "flash_boot.h"
#include "km/km_scope.h"
#include "km/km_vendor.h"
#include "n32h48x_regs.h"
#include "sense_hw.h"
#include "sense_regs.h"
#include "sense_tables.h"
#include "usb_hs.h"

#define SETTLE_US 200u          /* conservative; the scan driver (S3) starts converting ~1 us after the mux change */
#define PUBLISH_US 20000u       /* the 76-value snapshot is replaced at most every 20 ms (see the loop) */
#define MARK_BASE 0x20026F00u   /* [0] magic, [1] starts, [2] stage (SENSE_STAGE_ADDR), [3] unused */
#define MARK_MAGIC 0x4B4D5332u

static const km_vendor_info_t k_info = { 0x0104, __DATE__ "," __TIME__ "KMFW-S2" };

static volatile bool g_enter_boot;

static bool vendor(const uint8_t in[64], uint8_t out[64])
{
    if (km_vendor_is_enter_boot_request(in)) {
        g_enter_boot = true;
        return false;
    }
    return km_vendor_handle(&k_info, in, out);
}

static uint32_t g_last_poll, g_max_gap;
static bool g_polled;

/* The only place that polls USB: used by the main loop and, as the idle hook, from inside every wait. */
static void service(void)
{
    const uint32_t now = sense_cycles();
    if (g_polled && (uint32_t)(now - g_last_poll) > g_max_gap) {
        g_max_gap = now - g_last_poll;
    }
    g_polled = true;
    g_last_poll = now;
    km_iwdg_feed();
    km_usbhs_poll();
}

/* Keep a published snapshot on the wire for a while: only USB polling, no scanning. */
static void hold_ms(uint32_t ms)
{
    const uint32_t start = sense_cycles();
    while ((uint32_t)(sense_cycles() - start) < ms * 1000u * SENSE_CYCLES_PER_US) {
        service();
    }
}

static uint16_t clamp16(uint32_t v)
{
    return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
}

int main(void)
{
    volatile uint32_t *const mark = (volatile uint32_t *)MARK_BASE;
    uint32_t starts = 1, prev_stage = 0;
    if (mark[0] == MARK_MAGIC) {                     /* RAM survived: this is a restart without power loss */
        starts = mark[1] + 1u;
        prev_stage = mark[2];
    } else {
        mark[0] = MARK_MAGIC;
    }
    mark[1] = starts;
    sense_stage(1);

    km_usbhs_set_vendor_handler(vendor);
    km_usbhs_init();
    sense_stage(2);
    sense_dwt_init();
    sense_set_idle_hook(service);
    uint32_t init_err = sense_poll_init(0xFu);
    uint32_t alt_err = 0;
    sense_poll_status_t st;
    sense_poll_status(&st);
    if (st.ok_mask != 0xFu) {
        alt_err = sense_poll_try_alt_clock();
        sense_poll_status(&st);
    }

    static uint16_t slots[SENSE_NUM_SLOTS];
    uint32_t scans = 0, last_scan_us = 0, last_pub = 0;
    bool diag_due = true;
    for (;;) {
        service();
        if (g_enter_boot) {
            km_usbhs_disconnect();
            km_flash_request_bootloader();
        }
        if (diag_due) {
            diag_due = false;
            sense_poll_status(&st);
            const uint16_t d[16] = {
                clamp16(starts), clamp16(prev_stage), clamp16(mark[2]),
                (uint16_t)(st.ok_mask | (st.rdy_fail_mask << 4)), (uint16_t)(st.cal_fail_mask | (st.alt_state << 4)),
                clamp16(st.timeouts), clamp16(init_err), clamp16(alt_err),
                clamp16(g_max_gap / SENSE_CYCLES_PER_US), (uint16_t)scans, (uint16_t)(scans >> 16),
                (uint16_t)last_scan_us, (uint16_t)(last_scan_us >> 16), SETTLE_US, 0, 0x00D2,
            };
            km_scope_publish(0x0100u, d, 16);
            hold_ms(scans == 0u ? 300u : 50u);
        }
        const uint32_t t0 = sense_cycles();
        sense_poll_scan(slots, SETTLE_US);
        last_scan_us = (sense_cycles() - t0) / SENSE_CYCLES_PER_US;
        scans++;
        /* A scan takes ~2 ms but the host reads a snapshot in four pages: publishing every scan would make every read
         * "torn". Publish at 50 Hz so a snapshot stays unchanged for 20 ms. */
        if ((uint32_t)(sense_cycles() - last_pub) >= PUBLISH_US * SENSE_CYCLES_PER_US) {
            last_pub = sense_cycles();
            km_scope_publish(0x0002u, slots, SENSE_NUM_SLOTS);
        }
        if ((scans & 511u) == 0u) {
            diag_due = true;
        }
    }
}
