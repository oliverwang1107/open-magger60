/*
 * M5 stability image "KMFW-L30" (M5_PLAN.md hardware ladder L3-0): the K3 behaviour (scan -> key engine -> NKRO / boot reports, vendor
 * cmd 3, scope channel, enter-bootloader command) plus the fail-safes of task T-E and NO new features. BUILD ONLY until the user
 * approves the flash step L3-0.
 *
 * What is added to m4_main.c (K3):
 *   - km_health: counters, fault flags, progress-gated watchdog feed. The IWDG is fed only when the scan AND the USB side have
 *     progressed within the last 500 ms (km_health.h); a stalled scan or a dead endpoint ends in a watchdog reset.
 *   - km_report_sync: reports are reconciled instead of sent-on-change: retry until accepted, heartbeat while a key is down, release
 *     echo, resync after enumeration / bus reset / resume, all-released while any fault flag is set, endpoint recovery.
 *   - Scan chain: a frame that never completes (adc_scan_frame_timeout, 3 ms) restarts the chain (adc_scan_restart); a DMA error
 *     drops the frame and holds the keys released for 30 ms.
 *   - USB: bus reset and suspend/resume events from usb_hs.c feed the health flags (suspend = keys released, resume = resync).
 *   - Engine time is measured twice: total, and net of the scan interrupts that ran inside it (adc_scan_irq_cycles).
 *   - Reset cause (RCC flags, UNVERIFIED bits) and a RAM breadcrumb with the previous run's fault flags.
 *
 * Scope snapshots, one every 10 ms in rotation (publish counter p, p % 10):
 *   even p              tag 0x0301  keys the keyboard is reporting as a bitmask + sequence (km_health_pack_keymask) - 20 ms period
 *   p % 10 == 1         tag 0x0003  the 76 sixteen-point sums
 *   p % 10 == 3         tag 0x0100  ADC diagnostics, layout of S3 / K3
 *   p % 10 == 5         tag 0x0200  engine diagnostics, layout of K3 (see m4_main.c)
 *   p % 10 == 7         tag 0x0300  health counters and reset cause (km_health_pack_scope)
 *   p % 10 == 9         tag 0x0201  the 76 key depths in 0.01 mm
 */
#include <stdbool.h>
#include <string.h>

#include "adc_scan.h"
#include "flash_boot.h"
#include "km/km_engine.h"
#include "km/km_health.h"
#include "km/km_keymap.h"
#include "km/km_report.h"
#include "km/km_report_sync.h"
#include "km/km_scan.h"
#include "km/km_scope.h"
#include "km/km_vendor.h"
#include "n32h48x_regs.h"
#include "reset_cause.h"
#include "sense_hw.h"
#include "usb_hs.h"

#ifndef KM_M5_NAME
#define KM_M5_NAME "KMFW-L30"
#endif

/* Hardware-test variants of the L3-0 ladder (M5_PLAN.md section 4): the normal image is 0.
 *   1 (KMFW-L30S)  20 s after start the scan chain is stalled once; the 3 ms timeout must restart it and typing must come back
 *   2 (KMFW-L30D)  the same, but restarts are disabled: the keyboard must send all-released and be reset by the watchdog in about 2.5 s */
#ifndef KM_L30_STALL_TEST
#define KM_L30_STALL_TEST 0
#endif

#define PUBLISH_CYCLES (10u * KM_MS_CYCLES)      /* 10 ms */
#define WARMUP_FRAMES  32u                       /* the 16-point sums are full after 16 frames */
#define CAL_FRAMES     64u                       /* length of the quiet stretch fed to the calibration */
#define NKRO_REPORT_LEN (1u + KM_REPORT_NKRO_LEN)

static const km_vendor_info_t k_info = { 0x0104, __DATE__ "," __TIME__ KM_M5_NAME };

static volatile bool g_enter_boot;

static bool vendor(const uint8_t in[64], uint8_t out[64])
{
    if (km_vendor_is_enter_boot_request(in)) {
        g_enter_boot = true;
        return false;
    }
    return km_vendor_handle(&k_info, in, out);
}

static uint16_t clamp16(uint32_t v)
{
    return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
}

static km_scan_t g_scan;
static km_engine_t g_engine;
static km_health_t g_health;
static uint16_t g_counts[KM_ENGINE_SLOTS];

/* ---- report paths: IF2 NKRO (report id 1) and IF0 boot keyboard, exactly one is live at any time ---- */

static km_report_sync_t g_sync_nkro, g_sync_boot;

static bool bus_ok(void)
{
    return km_usbhs_configured() && !km_usbhs_suspended();
}

static bool nkro_link(void *c)
{
    (void)c;
    return bus_ok() && !km_usbhs_kbd_boot_protocol();
}

static bool boot_link(void *c)
{
    (void)c;
    return bus_ok() && km_usbhs_kbd_boot_protocol();
}

static bool nkro_ready(void *c)
{
    (void)c;
    return km_usbhs_nkro_ready();
}

static bool boot_ready(void *c)
{
    (void)c;
    return km_usbhs_kbd_ready();
}

static bool nkro_send(void *c, const uint8_t *r, unsigned len)
{
    (void)c;
    return km_usbhs_send_nkro(r, len);
}

static bool boot_send(void *c, const uint8_t *r, unsigned len)
{
    (void)c;
    return len == KM_REPORT_BOOT_LEN && km_usbhs_send_kbd(r);
}

static void ep_recover(void *c)
{
    (void)c;
    km_usbhs_in_recover();
}

static const km_sync_io_t k_io_nkro = { 0, nkro_link, nkro_ready, nkro_send, ep_recover };
static const km_sync_io_t k_io_boot = { 0, boot_link, boot_ready, boot_send, ep_recover };

/* The pressed slots in press order -> both report formats; pump() decides what goes out. */
static unsigned g_reported_n;
static uint8_t g_reported[KM_ENGINE_MAX_DOWN];

static void pump_reports(uint32_t now, bool allowed)
{
    unsigned n = 0;
    const uint8_t *down = km_engine_down_slots(&g_engine, &n);
    if (n > KM_ENGINE_MAX_DOWN) {
        n = KM_ENGINE_MAX_DOWN;
    }
    memcpy(g_reported, down, n);
    g_reported_n = n;

    uint8_t nkro[NKRO_REPORT_LEN];
    nkro[0] = 0x01u;                                    /* report id 1: keyboard bitmap (km_usb_reports.h) */
    km_report_build_nkro(down, n, &nkro[1]);
    uint8_t boot[KM_REPORT_BOOT_LEN];
    km_report_build_boot(down, n, boot);
    km_report_sync_pump(&g_sync_nkro, &k_io_nkro, nkro, allowed, now);
    km_report_sync_pump(&g_sync_boot, &k_io_boot, boot, allowed, now);
}

/* ---- fail-safe plumbing ---- */

static km_usbhs_events_t g_ev_seen;
static uint32_t g_dma_err_total;

static void poll_usb_events(void)
{
    km_usbhs_events_t ev;
    km_usbhs_get_events(&ev);
    while (g_ev_seen.resets != ev.resets) {
        g_ev_seen.resets++;
        km_health_on_usb(&g_health, KM_USB_EV_RESET);       /* the sync layer notices the dropped configuration by itself */
    }
    g_ev_seen = ev;
    km_health_set_usb_suspended(&g_health, km_usbhs_suspended());
}

int main(void)
{
    const uint8_t cause = km_reset_cause_read_and_clear();
    km_breadcrumb_t crumb;
    km_breadcrumb_boot(&crumb);

    km_usbhs_set_vendor_handler(vendor);
    km_usbhs_init();
    km_scan_init(&g_scan);
    km_engine_init(&g_engine);
    const uint32_t init_err = adc_scan_init();
    adc_scan_start();
    km_usbhs_get_events(&g_ev_seen);

    uint32_t now = sense_cycles();
    km_health_init(&g_health, now, cause);
    static const uint8_t nkro_released[NKRO_REPORT_LEN] = { 0x01u };
    static const uint8_t boot_released[KM_REPORT_BOOT_LEN] = { 0 };
    km_report_sync_init(&g_sync_nkro, nkro_released, NKRO_REPORT_LEN, now);
    km_report_sync_init(&g_sync_boot, boot_released, KM_REPORT_BOOT_LEN, now);

    uint32_t pushed = 0, publishes = 0, evaluated = 0;
    uint32_t cal_frames = 0, cal_attempts = 0;
    uint16_t cal_result = 0xFFFFu;
    bool was_calibrated = false;
    uint32_t eng_last = 0, eng_max = 0, eng_total_max = 0, eng_irq_max = 0;
    uint8_t last_mask = 0;
    uint8_t crumb_flags = 0;
    uint16_t keymask_seq = 0;
    uint32_t uptime_s = 0, next_second = now + KM_CPU_HZ;
#if KM_L30_STALL_TEST
    bool stall_done = false;
#endif
    uint32_t next = now;
    for (;;) {
        now = sense_cycles();
        km_usbhs_poll();
        if (!km_report_sync_stalled(&g_sync_nkro, now) && !km_report_sync_stalled(&g_sync_boot, now)) {
            km_health_progress(&g_health, KM_PROG_USB);     /* the poll ran AND the endpoint is taking what is due */
        }
        poll_usb_events();
        if (g_enter_boot) {
            adc_scan_stop();
            km_usbhs_disconnect();
            km_flash_request_bootloader();
        }

        if (adc_scan_frame_ready()) {
            const uint32_t dma_new = adc_scan_take_dma_errors();
            if (dma_new != 0u) {
                g_dma_err_total += dma_new;
                km_health_on_dma_error(&g_health, now, dma_new);
                adc_scan_release_frame();                   /* the frame may hold garbage: drop it, keys stay released 30 ms */
            } else {
                last_mask = adc_scan_phase_mask();
                for (unsigned p = 0; p < SENSE_NUM_PHASES; p++) {
                    km_scan_push_phase(&g_scan, p, g_phase_words[p]);
                }
                km_scan_push_injected(&g_scan, g_inj[0], g_inj[1]);
                adc_scan_release_frame();
                pushed++;
                km_health_on_frame(&g_health);
                if (pushed > WARMUP_FRAMES) {
                    const uint16_t *sums = km_scan_sums(&g_scan);
                    for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
                        g_counts[i] = (uint16_t)(sums[i] >> 4);
                    }
                    if (!g_engine.calibrated) {
                        if (was_calibrated) {
                            km_health_cal_lost(&g_health);
                            was_calibrated = false;
                        }
                        if (cal_frames == 0) {
                            km_engine_calibrate_begin(&g_engine);
                        }
                        km_engine_calibrate_idle(&g_engine, g_counts);
                        if (++cal_frames >= CAL_FRAMES) {
                            cal_result = (uint16_t)km_engine_calibrate_finish(&g_engine);
                            cal_attempts++;
                            cal_frames = 0;         /* on failure the next stretch starts at once, until no key is held */
                            was_calibrated = g_engine.calibrated;
                            km_health_on_calibration(&g_health, g_engine.calibrated);
                        }
                    } else {
                        const uint32_t irq0 = adc_scan_irq_cycles();
                        const uint32_t t0 = sense_cycles();
                        km_engine_update(&g_engine, g_counts);
                        const uint32_t total = sense_cycles() - t0;
                        const uint32_t irq = adc_scan_irq_cycles() - irq0;
                        eng_last = total > irq ? total - irq : 0u;      /* the engine's own time */
                        if (eng_last > eng_max) {
                            eng_max = eng_last;
                        }
                        if (total > eng_total_max) {
                            eng_total_max = total;
                        }
                        if (irq > eng_irq_max) {
                            eng_irq_max = irq;
                        }
                        km_health_on_engine(&g_health, now, eng_last);
                        km_health_set_glitches(&g_health, km_engine_glitch_count(&g_engine));
                        evaluated++;
                    }
                }
            }
        } else if (adc_scan_frame_timeout(now)) {
            if (km_health_on_scan_timeout(&g_health, now)) {
#if KM_L30_STALL_TEST != 2
                adc_scan_restart(now);
#endif
                const uint32_t e = adc_scan_take_dma_errors();
                g_dma_err_total += e;
                km_health_on_dma_error(&g_health, now, e);
            }
        }

#if KM_L30_STALL_TEST
        if (!stall_done && uptime_s >= 20u) {
            stall_done = true;
            adc_scan_test_stall();
        }
#endif
        km_health_tick(&g_health, now);
        pump_reports(now, g_health.fault_flags == 0u);
        km_health_set_report_stats(&g_health, g_sync_nkro.retries + g_sync_boot.retries,
                                   g_sync_nkro.resyncs + g_sync_boot.resyncs);
        if (km_health_should_feed(&g_health, now)) {
            km_iwdg_feed();                                 /* the ONLY feed in the main loop */
        }
        if (g_health.fault_flags != crumb_flags) {
            crumb_flags = g_health.fault_flags;
            km_breadcrumb_update(crumb_flags);
        }
        if ((int32_t)(now - next_second) >= 0) {
            next_second += KM_CPU_HZ;
            uptime_s++;
        }

        if ((int32_t)(now - next) >= 0) {
            next += PUBLISH_CYCLES;
            publishes++;
            const uint32_t phase = publishes % 10u;
            if ((phase & 1u) == 0u) {
                uint16_t d[KM_KEYMASK_SCOPE_VALUES];
                const bool allowed = g_health.fault_flags == 0u;
                km_health_pack_keymask(g_reported, allowed ? g_reported_n : 0u, ++keymask_seq, g_health.fault_flags,
                                       g_sync_nkro.undelivered || g_sync_boot.undelivered, d);
                km_scope_publish(KM_SCOPE_TAG_KEYMASK, d, KM_KEYMASK_SCOPE_VALUES);
            } else if (phase == 3u) {
                adc_scan_stats_t st;
                adc_scan_get_stats(&st);
                const uint16_t d[24] = {
                    (uint16_t)st.phases, (uint16_t)(st.phases >> 16), (uint16_t)st.frames, (uint16_t)(st.frames >> 16),
                    clamp16(st.dma_errors), clamp16(st.overruns), (uint16_t)st.inj_events, (uint16_t)(st.inj_events >> 16),
                    clamp16(st.phase_cycles[0]), clamp16(st.phase_cycles[1]), clamp16(st.phase_cycles[2]), clamp16(st.phase_cycles[3]),
                    clamp16(st.frame_cycles), clamp16(st.frame_cycles_min), clamp16(st.frame_cycles_max), (uint16_t)init_err,
                    (uint16_t)pushed, (uint16_t)(pushed >> 16), clamp16(st.restarts), clamp16(eng_irq_max),
                    last_mask, 0, 0, 0,
                };
                km_scope_publish(0x0100u, d, 24);
            } else if (phase == 5u) {
                unsigned n = 0;
                const uint8_t *down = km_engine_down_slots(&g_engine, &n);
                uint16_t d[24] = {
                    cal_result, clamp16(cal_attempts), (uint16_t)n, (uint16_t)km_engine_glitch_count(&g_engine),
                    (uint16_t)(km_engine_glitch_count(&g_engine) >> 16), clamp16(eng_last), clamp16(eng_max),
                    (uint16_t)(g_sync_nkro.sent + g_sync_boot.sent), (uint16_t)((g_sync_nkro.sent + g_sync_boot.sent) >> 16),
                    km_usbhs_kbd_boot_protocol() ? 1u : 0u,
                    0, 0,
                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                    g_engine.calibrated ? 1u : 0u, (uint16_t)evaluated, (uint16_t)(evaluated >> 16), 0, 0, 0,
                };
                for (unsigned i = 0; i < 6u && i < n; i++) {
                    d[12 + i] = down[i];
                }
                km_scope_publish(0x0200u, d, 24);
            } else if (phase == 7u) {
                const uint16_t extra[7] = {
                    (uint16_t)(g_sync_nkro.sent + g_sync_boot.sent),
                    clamp16(g_sync_nkro.heartbeats + g_sync_boot.heartbeats),
                    clamp16(g_sync_nkro.recoveries + g_sync_boot.recoveries),
                    crumb.valid ? crumb.prev_faults : 0xFFFFu,
                    clamp16(crumb.boots),
                    clamp16(uptime_s),
                    clamp16(eng_total_max),
                };
                uint16_t d[KM_HEALTH_SCOPE_VALUES];
                km_health_pack_scope(&g_health, extra, d);
                km_scope_publish(KM_SCOPE_TAG_HEALTH, d, KM_HEALTH_SCOPE_VALUES);
            } else if (phase == 9u) {
                uint16_t depth[KM_ENGINE_SLOTS];
                for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
                    depth[i] = km_engine_depth(&g_engine, i);
                }
                km_scope_publish(0x0201u, depth, KM_ENGINE_SLOTS);
            } else {
                km_scope_publish(0x0003u, km_scan_sums(&g_scan), 76);
            }
        }
    }
}
