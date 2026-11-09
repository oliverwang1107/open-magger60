/*
 * M6 full-feature image (M6_PLAN.md section 7, package P10): the M5 image of m5_main.c (km_l51: settings, calibration and light
 * configuration in flash, layers, rapid trigger, lights, configuration protocol) plus everything M6 built, wired in one place.
 * m5_main.c is NOT touched (the M5 images keep building from it); this file is its copy with the additions below.
 *
 *   keys       km_pipeline.[hc] (portable, tested on the host as tests/test_pipeline_m6.c) runs the order
 *                engine pressed list -> SOCD -> layers (toggle keys, Mod-Morph, tap-hold, ...) -> DKS / gamepad / light actions.
 *              apply_profile() loads the SOCD pairs, DKS records, Mod-Morph records and gamepad curves of the active profile and gives
 *              the key engine the trigger settings those keys need (km_pipeline_slot_cfg: a DKS key is FIXED at its start point, a
 *              gamepad key FIXED at 0.20 mm). A fault, a USB bus reset and a table change reset every module.
 *   gamepad    KM_M6_PAD = 1 (image km_m6): when the settings say device.pad_mode = 1 the USB descriptor set with the gamepad interface
 *              IF3 / EP4 is presented (bcdDevice 0x0002) and the gamepad report goes out through its own reconciled report path.
 *              The host has KM_USB_GUARD_MS (6 s) to configure the device; if it does not, usb_hs.c drops the connection and presents the
 *              plain keyboard descriptors (km_usbhs_fell_back()). pad_mode is read ONCE at boot: a change needs a re-plug.
 *              pad_mode = 2 (M6 P11) presents an XInput-style vendor interface on the same IF3 / EP4 instead (bcdDevice 0x0003, never both):
 *              the same pad state, converted to the 20-byte XInput report (km_usb_xinput_from_pad); the host's rumble messages are recorded only.
 *              KM_M6_PAD = 0 (image km_m6a): the gamepad interface does not exist, whatever the settings say.
 *   lights     host Caps/Num/Scroll lock state (km_usbhs_host_leds) and layer / profile / gamepad / toggle state feed the indicator rules of
 *              the scene; the light scene (km_lightscene) is loaded at boot from its two flash sectors and written by the SCENE_COMMIT op of the
 *              configuration protocol; KM_CODE_LIGHT keys change effect / brightness / speed / on-off (RAM only).
 *   stream     vendor command 0xC7 (km_stream): complete frames go to the light core's external-frame input; while a stream is live the effect
 *              is forced to EXTERNAL (RAM only) and put back when it ends. 500 ms of silence end it.
 *   animation  km_anim plays a clip that is linked into the image (symbols km_anim_clip / km_anim_clip_len, optional: without them there is
 *              no player; docs/BADAPPLE_NOTES.md) while the light effect is ANIM (9). The clip is verified in the background first.
 *   protocol   configuration protocol version 2: DEVICE_STATUS and SCENE_* (km_config.h, CONFIG_PROTOCOL.md).
 *   scope      tag 0x0500: M6 diagnostics (worst pass cycles, SOCD / DKS / gamepad / stream counters, host LED bits, pad mode, fall-back).
 *
 * Everything below the M6 additions is m5_main.c's text; its comment follows for reference.
 */
#include <stdbool.h>
#include <string.h>

#include "adc_scan.h"
#include "flash_boot.h"
#include "km/km_anim.h"
#include "km/km_calmgr.h"
#include "km/km_config.h"
#include "km/km_drift.h"
#include "km/km_engine.h"
#include "km/km_health.h"
#include "km/km_latency.h"
#include "km/km_usbfast.h"
#include "km/km_keymap.h"
#include "km/km_layers.h"
#include "km/km_lightscene.h"
#include "km/km_pad.h"
#include "km/km_pipeline.h"
#include "km/km_report.h"
#include "km/km_report_sync.h"
#include "km/km_scan.h"
#include "km/km_scope.h"
#include "km/km_settings.h"
#include "km/km_stream.h"
#include "km/km_vendor.h"
#include "n32h48x_regs.h"
#include "reset_cause.h"
#include "sense_hw.h"
#include "usb_hs.h"

#ifndef KM_M5_PERSIST
#define KM_M5_PERSIST 1
#endif
#ifndef KM_M5_NAME
#define KM_M5_NAME "KMFW-M6"
#endif
/* KM_M6_LAT_PROBE = 1 (image km_m6p): the press-to-report probe (km_latency.h, scope tag 0x0600, LATENCY_NOTES.md section 1). It only counts. */
#ifndef KM_M6_LAT_PROBE
#define KM_M6_LAT_PROBE 0
#endif

/* KM_M6_DRIFT: calibration drift compensation (km_drift.h, DRIFT_NOTES.md). 0 = not linked in (km_m6a, km_m6h: nothing changes), 2 = linked in
 * and ON from boot (image km_m6d). There is no settings switch yet (it would change the 16-byte DEVICE record, see DRIFT_NOTES.md section 7). */
#ifndef KM_M6_DRIFT
#define KM_M6_DRIFT 0
#endif

/* KM_M6_FASTPATH = 1 (image km_m6h): 8 kHz work (USB8K_NOTES.md). (a) A key report is also retried between the slow stages of a pass,
 * (b) the slow housekeeping (light stream, animation, lights) is skipped in a pass that sent a key report, for at most 1 ms, (c) a started
 * light-configuration flash write waits for a quiet moment unless keys have been held for 2 s. Counters: scope tags 0x0601 / 0x0602. */
#ifndef KM_M6_FASTPATH
#define KM_M6_FASTPATH 0
#endif
/* Hardware result (km_m6h, 1 kHz host): the slow-work deferral and the in-pass retry did not help (retries 0) and the worst press-to-report time
 * did not improve, so both are OFF unless asked for. FASTPATH alone = only the flash rule (c). */
#ifndef KM_M6_DEFER
#define KM_M6_DEFER 0
#endif
#ifndef KM_M6_RETRY
#define KM_M6_RETRY 0
#endif

/* KM_M6_FILTER_MODE (images km_m6q / km_m6l, which link the km_core_f twin built with KM_ENGINE_FILTER_MODES = 1, FILTER_NOTES.md): the engine filter in force
 * from boot: 0 = the original (km_m6a), 1 = low latency, 2 = quiet, 3 = fast (km_m6f / km_m6hf, the chain designed for the measured noise; image-level only). A non-zero settings.device.filter_mode wins over it. Modes 1 and 2 also switch
 * fast confirm on. Read ONCE at boot, like pad_mode. With KM_ENGINE_FILTER_MODES = 0 (every other image) none of this exists. */
#ifndef KM_M6_FILTER_MODE
#define KM_M6_FILTER_MODE 0
#endif

/* KM_M6_PAD = 1: the gamepad interface may be presented (image km_m6); 0: never (image km_m6a) */
#ifndef KM_M6_PAD
#define KM_M6_PAD 0
#endif

#if KM_M5_PERSIST
#include "store_flash.h"
#endif

/* KM_M5_LED = 1: the lights (LED_NOTES.md). All of it lives in m5_led.c; this file only calls it, the key path never waits for it. */
#ifndef KM_M5_LED
#define KM_M5_LED 1
#endif
#if !KM_M5_LED || !KM_M5_PERSIST
#error "the M6 images are built with the lights and with flash persistence (km_m6a, km_m6)"
#endif
#include "m5_led.h"

#define PUBLISH_CYCLES (10u * KM_MS_CYCLES)      /* 10 ms */
#define WARMUP_FRAMES  32u                       /* the 16-point sums are full after 16 frames */
#define CAL_FRAMES     64u                       /* length of the quiet stretch fed to the calibration */
#define NKRO_REPORT_LEN (1u + KM_REPORT_NKRO_LEN)
#define SAVE_IDLE_CYCLES (1000u * KM_MS_CYCLES)  /* a profile change is stored after this much time without a key down */

_Static_assert(KM_SETTINGS_ENCODED_SIZE <= KM_CONFIG_MAX_SETTINGS, "the configuration protocol buffer must hold a whole settings blob");
_Static_assert(KM_STREAM_LIGHTS == KM_LED_COUNT, "the stream carries one colour per light");
_Static_assert(KM_ANIM_LIGHTS == KM_LED_COUNT, "the animation carries one grey level per light");
_Static_assert(KM_LS_MAX_ENCODED <= KM_CONFIG_MAX_SCENE, "the scene protocol buffer must hold a whole scene");
_Static_assert(KM_USB_PAD_REPORT_LEN == KM_PAD_REPORT_LEN, "the USB layer and the gamepad core agree on the report length");

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

/* A wrap-safe millisecond clock from the CPU cycle counter (the counter wraps every 17 s at 240 MHz, so it must be called more often than
 * that: the main loop calls it every pass). Used by the USB fall-back, the stream and the animation. Main loop only. */
static uint32_t g_clk_last, g_clk_acc, g_ms;

static uint32_t ms_clock(void)
{
    const uint32_t c = sense_cycles();
    g_clk_acc += (uint32_t)(c - g_clk_last);
    g_clk_last = c;
    const uint32_t d = g_clk_acc / KM_MS_CYCLES;
    g_clk_acc -= d * KM_MS_CYCLES;
    g_ms += d;
    return g_ms;
}

static km_scan_t g_scan;
static km_engine_t g_engine;
static km_health_t g_health;
static uint16_t g_counts[KM_ENGINE_SLOTS];

/* ---- settings: the running copy, applied to the engine and the layers ---- */

static km_settings_t g_settings;
static km_settings_t g_staged;            /* scratch for validating / applying a committed blob */
static km_config_t g_cfg;

static uint8_t g_settings_source;         /* 0 defaults, 1 loaded from flash, 2 flash held no usable record */
static uint32_t g_saves_ok, g_saves_failed;
static bool g_active_dirty;               /* the active profile changed and is not stored yet (persistent build only) */
static uint32_t g_last_key_cycles;        /* last time a key was down */
static uint32_t g_down_now;               /* number of keys the engine reports down, updated every loop pass */
static uint32_t g_uptime_s;               /* seconds since boot, advanced by the main loop */

/* M6 state */
static km_pipeline_t g_pipe;
static km_stream_t g_stream;
static km_report_sync_t g_sync_pad;
static uint8_t g_layer, g_toggled, g_toggle_keys;   /* layer a key pressed now would use / toggled layers / toggle keys on, from the last pipeline pass */
static uint8_t g_reported[KM_ENGINE_MAX_DOWN];   /* the pressed slots in press order, kept for the scope key mask */
static unsigned g_reported_n;
static bool g_flash_this_pass;           /* a flash operation stalled the CPU in this main loop pass (its time does not count as a pass time) */

static void apply_profile(unsigned p)
{
    if (p >= KM_PROFILES) {
        p = 0;
    }
    const km_profile_t *prof = &g_settings.profile[p];
    for (unsigned slot = 0; slot < KM_ENGINE_SLOTS; slot++) {
        if (km_keymap_slot_is_key(slot)) {
            km_engine_key_cfg_t cfg;
            (void)km_pipeline_slot_cfg(prof, slot, &cfg);       /* the user's config, or what a DKS / gamepad key needs */
            (void)km_engine_apply_key_cfg(&g_engine, slot, &cfg);   /* an invalid config falls back to FIXED defaults */
        }
    }
    km_pipeline_set_profile(&g_pipe, prof, g_reported, g_reported_n);   /* tables, Mod-Morph, SOCD, DKS, gamepad curves */
}

static km_cfg_status_t save_settings(void)
{
#if KM_M5_PERSIST
    g_flash_this_pass = true;                                   /* the sector erase stalls the CPU */
    if (km_store_save(&g_settings)) {
        g_saves_ok++;
        g_active_dirty = false;
        return KM_CFG_OK;
    }
    g_saves_failed++;
    return KM_CFG_ERR_FAILED;
#else
    g_active_dirty = false;
    return KM_CFG_OK;
#endif
}

#if KM_M6_DRIFT
static km_drift_t g_drift;
#endif

/* ---- calibration persistence (km_l33): record in sector 0x08034000, restored at boot, saved in the background ---- */

#if KM_M5_PERSIST
#define CAL_IDLE_CYCLES (1000u * KM_MS_CYCLES)   /* quiet time (no key, no fault) before a background save may start */

static km_calmgr_t g_calmgr;

static void cal_after_flash(void *ctx)
{
    (void)ctx;
    g_flash_this_pass = true;
    adc_scan_restart(sense_cycles());           /* the flash stalled the CPU: start the scan chain cleanly again */
}

/* The host's save_calibration command ("SCAL"): to the end, now. BUSY while a key is down or a fault is active. */
static km_cfg_status_t cfg_save_calibration(void *ctx)
{
    (void)ctx;
    switch (km_calmgr_save_request(&g_calmgr, g_uptime_s, g_down_now == 0u && g_health.fault_flags == 0u)) {
    case KM_CALMGR_OK:
        return KM_CFG_OK;
    case KM_CALMGR_BUSY:
        return KM_CFG_ERR_BUSY;
    default:
        return KM_CFG_ERR_FAILED;
    }
}
#define CFG_SAVE_CAL cfg_save_calibration
#else
#define CFG_SAVE_CAL 0
#endif

/* ---- the configuration protocol backend (km_config_ops_t) ---- */

static unsigned cfg_read(void *ctx, uint8_t *buf, unsigned cap)
{
    (void)ctx;
    return km_settings_encode(&g_settings, buf, cap);
}

static km_cfg_status_t cfg_validate(void *ctx, const uint8_t *blob, unsigned len)
{
    (void)ctx;
    return km_settings_decode(&g_staged, blob, len) ? KM_CFG_OK : KM_CFG_ERR_INVALID;
}

static km_cfg_status_t cfg_commit(void *ctx, const uint8_t *blob, unsigned len)
{
    (void)ctx;
    if (g_down_now != 0u) {
        return KM_CFG_ERR_BUSY;                 /* never touch flash or the key tables while a key is down: the host retries */
    }
    if (!km_settings_decode(&g_staged, blob, len)) {
        return KM_CFG_ERR_INVALID;
    }
    g_settings = g_staged;
    apply_profile(g_settings.active);
    m5_led_set_profile(g_settings.active);      /* the blob may carry another active profile */
    return save_settings();
}

static km_cfg_status_t cfg_select_profile(void *ctx, unsigned index)
{
    (void)ctx;
    if (index >= KM_PROFILES) {
        return KM_CFG_ERR_RANGE;
    }
    g_settings.active = (uint8_t)index;
    apply_profile(index);
    m5_led_set_profile(index);                  /* the lights of that profile: RAM only, nothing is stored here */
    g_active_dirty = (KM_M5_PERSIST != 0);
    return KM_CFG_OK;
}

static unsigned cfg_active_profile(void *ctx)
{
    (void)ctx;
    return g_settings.active;
}

static bool g_recal_request;

static km_cfg_status_t cfg_recalibrate(void *ctx)
{
    (void)ctx;
    if (g_down_now != 0u) {
        return KM_CFG_ERR_BUSY;
    }
    g_recal_request = true;                     /* the main loop starts the calibration at the next frame */
    return KM_CFG_OK;
}

static km_cfg_status_t cfg_live_depth(void *ctx, unsigned first, unsigned count, uint16_t *out)
{
    (void)ctx;
    if (first >= KM_ENGINE_SLOTS || count > KM_ENGINE_SLOTS - first) {
        return KM_CFG_ERR_RANGE;
    }
    for (unsigned i = 0; i < count; i++) {
        out[i] = km_engine_depth(&g_engine, first + i);
    }
    return KM_CFG_OK;
}

/* ---- protocol 2: DEVICE_STATUS and the light scene ---- */

static uint32_t g_scene_saves_ok, g_scene_saves_failed;
static km_lightscene_t g_scene_stage;      /* scratch for validating / applying a committed scene */

static uint16_t sat16(uint32_t v)
{
    return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
}

static bool pad_active_now(void)
{
    return KM_M6_PAD && KM_USB_VARIANT_HAS_IF3(km_usbhs_variant()) && !km_usbhs_fell_back();
}

/* The gamepad mode in force: 0 none, 1 HID gamepad, 2 XInput-style gamepad (the same numbers as the pad_mode setting). */
static unsigned pad_mode_in_force(void)
{
    if (!pad_active_now()) {
        return 0u;
    }
    return km_usbhs_variant() == KM_USB_VARIANT_XINPUT ? 2u : 1u;
}

static bool g_anim_running;

static km_cfg_status_t cfg_device_status(void *ctx, uint8_t v[KM_CONFIG_DEVICE_STATUS_BYTES])
{
    (void)ctx;
    const km_stream_stats_t *st = km_stream_stats(&g_stream);
    v[0] = (uint8_t)(km_usbhs_host_leds() & 7u);
    v[1] = g_settings.device.pad_mode;
    v[2] = (uint8_t)pad_mode_in_force();
    v[3] = (uint8_t)((km_usbhs_fell_back() ? KM_CFG_DEVST_FALLBACK : 0u) | (km_stream_active(&g_stream) ? KM_CFG_DEVST_STREAM : 0u) |
                     (g_anim_running ? KM_CFG_DEVST_ANIM : 0u) | (km_usbhs_led_report_count() != 0u ? KM_CFG_DEVST_LEDS_KNOWN : 0u));
    v[4] = g_settings.active;
    v[5] = g_layer;
    v[6] = (uint8_t)km_stream_state(&g_stream);
    v[7] = 0;
    const uint16_t shown = st->committed, lost = sat16((uint32_t)st->dropped + st->lost + st->stale + st->rejected), rep = sat16(km_usbhs_led_report_count());
    v[8] = (uint8_t)shown; v[9] = (uint8_t)(shown >> 8);
    v[10] = (uint8_t)lost; v[11] = (uint8_t)(lost >> 8);
    v[12] = (uint8_t)rep; v[13] = (uint8_t)(rep >> 8);
    v[14] = v[15] = 0;
    return KM_CFG_OK;
}

static const km_config_device_ops_t k_dev_ops = { 0, cfg_device_status };

static unsigned cfg_scene_read(void *ctx, uint8_t *buf, unsigned cap)
{
    (void)ctx;
    km_lightscene_t sc;
    m5_led_scene_get(&sc);
    return km_lightscene_encode(&sc, buf, cap);
}

static km_cfg_status_t cfg_scene_validate(void *ctx, const uint8_t *blob, unsigned len)
{
    (void)ctx;
    return km_lightscene_decode(&g_scene_stage, blob, len) ? KM_CFG_OK : KM_CFG_ERR_INVALID;
}

/* THE only path to the scene sectors: a complete, confirmed SCENE_COMMIT. The sector erase stalls the CPU, so: no key down. */
static km_cfg_status_t cfg_scene_commit(void *ctx, const uint8_t *blob, unsigned len)
{
    (void)ctx;
    if (g_down_now != 0u) {
        return KM_CFG_ERR_BUSY;
    }
    if (!km_lightscene_decode(&g_scene_stage, blob, len)) {
        return KM_CFG_ERR_INVALID;
    }
    const km_store_result_t r = m5_led_scene_commit(&g_scene_stage, true);
    cal_after_flash(0);                                  /* the erase stalled the CPU: restart the scan chain */
    if (r == KM_STORE_OK || r == KM_STORE_UNCHANGED) {
        g_scene_saves_ok++;
        return KM_CFG_OK;
    }
    g_scene_saves_failed++;
    return r == KM_STORE_ERR_ARG ? KM_CFG_ERR_INVALID : KM_CFG_ERR_FAILED;
}

static const km_config_scene_ops_t k_scene_ops = { 0, cfg_scene_read, cfg_scene_validate, cfg_scene_commit };

/* save_calibration exists in the persistent build only (km_l31 has no flash to put it in: the protocol answers UNSUPPORTED). */
static const km_config_ops_t k_cfg_ops = {
    0, KM_PROFILES, cfg_read, cfg_validate, cfg_commit, cfg_select_profile, cfg_active_profile, cfg_recalibrate, CFG_SAVE_CAL, cfg_live_depth,
};

/* ---- report paths: IF2 NKRO (id 1) + IF2 consumer (id 2) + IF0 boot keyboard; the boot path is live only in boot protocol ---- */

static km_report_sync_t g_sync_nkro, g_sync_boot, g_sync_cons;

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

#if KM_M6_LAT_PROBE
static km_ustat_t g_ustat;
#endif

static bool nkro_send(void *c, const uint8_t *r, unsigned len)
{
    (void)c;
    const bool ok = km_usbhs_send_nkro(r, len);
#if KM_M6_LAT_PROBE
    if (ok) {
        km_ustat_sent(&g_ustat, sense_cycles(), km_usbhs_sof_count());
    }
#endif
    return ok;
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

/* the gamepad report (IF3 / EP4): its own endpoint, its own reconciliation. A host that never polls EP4 delays nothing else, and the
 * stalled-endpoint watchdog rule does NOT apply to it (a game that does not read the pad must not reset the keyboard). */
static bool pad_link(void *c)
{
    (void)c;
    return bus_ok() && pad_active_now();
}

static bool pad_ready(void *c)
{
    (void)c;
    return km_usbhs_variant() == KM_USB_VARIANT_XINPUT ? km_usbhs_xinput_ready() : km_usbhs_pad_ready();
}

static bool pad_send(void *c, const uint8_t *r, unsigned len)
{
    (void)c;
    if (km_usbhs_variant() == KM_USB_VARIANT_XINPUT) {
        return len == KM_USB_XI_REPORT_LEN && km_usbhs_send_xinput(r, len);
    }
    return len == KM_USB_PAD_REPORT_LEN && km_usbhs_send_pad(r, len);
}

static void pad_recover(void *c)
{
    (void)c;
    km_usbhs_pad_recover();
}

static const km_sync_io_t k_io_pad = { 0, pad_link, pad_ready, pad_send, pad_recover };
static const km_sync_io_t k_io_nkro = { 0, nkro_link, nkro_ready, nkro_send, ep_recover };
static const km_sync_io_t k_io_boot = { 0, boot_link, boot_ready, boot_send, ep_recover };
/* the consumer report is a second report of the same IF2 endpoint: same link / ready / send functions */
static const km_sync_io_t k_io_cons = { 0, nkro_link, nkro_ready, nkro_send, ep_recover };

/* ---- M6 diagnostics (scope tag 0x0500) ---- */
static uint32_t g_m6_last, g_m6_max;       /* cycles of pump_reports() = the whole key path of a pass (SOCD, layers, DKS, gamepad, light actions, building and sending the reports) */
static uint32_t g_pass_max, g_pass_over;   /* cycles of a whole main loop pass (flash passes excluded); passes above PASS_BUDGET_CYCLES */
static uint32_t g_light_keys;
#define PASS_BUDGET_CYCLES 9300u           /* a sensing frame is about 9.3k..10.4k cycles (M6_PLAN.md 1.1): a pass longer than this can cost a frame */

static uint16_t depth_cb(void *ctx, unsigned slot)
{
    return km_engine_depth((const km_engine_t *)ctx, slot);
}

/* KM_CODE_LIGHT keys: RAM only, the same limits as LED_SET (m5_led_set_cfg validates). */
static uint8_t g_fx_saved;

static void apply_light_action(uint8_t act)
{
    km_led_cfg_t c;
    m5_led_get_cfg(&c);
    switch (act) {
    case KM_LIGHT_ACT_NEXT_FX:
        c.effect = c.effect >= (uint8_t)KM_LED_FX_CUSTOM ? (uint8_t)(KM_LED_FX_OFF + 1u) : (uint8_t)(c.effect + 1u);
        break;
    case KM_LIGHT_ACT_PREV_FX:
        c.effect = c.effect == (uint8_t)KM_LED_FX_OFF || c.effect > (uint8_t)KM_LED_FX_CUSTOM ? (uint8_t)KM_LED_FX_CUSTOM : (uint8_t)(c.effect - 1u);
        break;
    case KM_LIGHT_ACT_BRIGHT_UP:
        c.brightness = c.brightness > 231u ? 255u : (uint8_t)(c.brightness + 24u);
        break;
    case KM_LIGHT_ACT_BRIGHT_DOWN:
        c.brightness = c.brightness > 32u ? (uint8_t)(c.brightness - 24u) : 8u;     /* never to black by this key: ON/OFF is its own action */
        break;
    case KM_LIGHT_ACT_SPEED_UP:
        c.speed = c.speed > 200u ? 255u : (uint8_t)(c.speed + c.speed / 4u + 1u);
        break;
    case KM_LIGHT_ACT_SPEED_DOWN:
        c.speed = c.speed > 4u ? (uint8_t)(c.speed - c.speed / 4u - 1u) : 1u;
        break;
    case KM_LIGHT_ACT_TOGGLE:
        if (c.effect != (uint8_t)KM_LED_FX_OFF) {
            g_fx_saved = c.effect;
            c.effect = (uint8_t)KM_LED_FX_OFF;
        } else {
            c.effect = g_fx_saved != 0u ? g_fx_saved : (uint8_t)KM_LED_FX_STATIC;
        }
        break;
    default:
        return;
    }
    (void)m5_led_set_cfg(&c);
    g_light_keys++;
}

#if KM_M6_LAT_PROBE
static km_lat_t g_lat;
#endif

/* Slots -> pipeline (SOCD, layers, DKS, gamepad, light actions) -> the report formats; pump() decides what goes out. */
#if KM_M6_FASTPATH
static uint8_t g_des_nkro[NKRO_REPORT_LEN], g_des_boot[KM_REPORT_BOOT_LEN], g_des_cons[KM_REPORT_CONSUMER_LEN];
static bool g_des_allowed;
static km_defer_t g_defer;
static uint32_t g_fast_retry_sends;                 /* key reports that went out in a retry between the slow stages (instead of one pass later) */

static bool keys_undelivered(void)
{
    return g_sync_nkro.undelivered || g_sync_boot.undelivered || g_sync_cons.undelivered;
}

/* A key report is due and the endpoint was busy when the pass began: look at the endpoint again (the host may have taken the previous
 * report meanwhile) and send. Called between the slow stages, so a stage that ran long does not delay the report to the next pass. */
__attribute__((unused)) static void retry_keys(void)
{
    if (!keys_undelivered()) {
        return;
    }
    const uint32_t before = g_sync_nkro.sent + g_sync_boot.sent + g_sync_cons.sent;
    km_usbhs_poll();
#if KM_M6_LAT_PROBE
    km_ustat_observe(&g_ustat, sense_cycles(), km_usbhs_nkro_ready(), km_usbhs_sof_count());
#endif
    const uint32_t now = sense_cycles();
    km_report_sync_pump(&g_sync_nkro, &k_io_nkro, g_des_nkro, g_des_allowed, now);
    km_report_sync_pump(&g_sync_boot, &k_io_boot, g_des_boot, g_des_allowed, now);
    km_report_sync_pump(&g_sync_cons, &k_io_cons, g_des_cons, g_des_allowed, now);
    if ((g_sync_nkro.sent + g_sync_boot.sent + g_sync_cons.sent) != before) {
        g_fast_retry_sends++;
    }
}
#endif

static void pump_reports(uint32_t now, bool allowed)
{
    unsigned n = 0;
    const uint8_t *down = km_engine_down_slots(&g_engine, &n);
    if (n > KM_ENGINE_MAX_DOWN) {
        n = KM_ENGINE_MAX_DOWN;
    }
    memcpy(g_reported, down, n);
    g_reported_n = n;
    g_down_now = n;

    const uint32_t t0 = sense_cycles();
#if KM_M6_LAT_PROBE
    km_lat_observe(&g_lat, t0, down, n);            /* a key the engine reports down for the first time */
    const uint32_t lat_sent0 = g_sync_nkro.sent;
#endif
    km_pipe_out_t po;
    const km_pipe_in_t pin = { down, n, allowed, now, KM_MS_CYCLES, depth_cb, &g_engine };
    km_pipeline_run(&g_pipe, &pin, &po);
    const km_layers_out_t *lo = &po.lo;
    g_layer = lo->layer;
    g_toggled = lo->toggled;
    g_toggle_keys = lo->toggle_keys;
    if (lo->profile_request != 0u && allowed) {
        const unsigned idx = (unsigned)lo->profile_request - 1u;
        if (idx < KM_PROFILES && idx != g_settings.active) {
            (void)cfg_select_profile(0, idx);       /* the latched codes of keys still down stay as they are; the pipeline holds those keys over */
        }
    }
    for (unsigned i = 0; i < po.light_n; i++) {
        apply_light_action(po.light[i]);
    }

    uint8_t nkro[NKRO_REPORT_LEN];
    nkro[0] = 0x01u;                                    /* report id 1: keyboard bitmap (km_usb_reports.h) */
    km_report_build_nkro_usages(lo->kbd, lo->kbd_n, &nkro[1]);
    uint8_t boot[KM_REPORT_BOOT_LEN];
    km_report_build_boot_usages(lo->kbd, lo->kbd_n, boot);
    uint8_t cons[KM_REPORT_CONSUMER_LEN];
    km_report_build_consumer(lo->consumer, cons);
#if KM_M6_FASTPATH
    memcpy(g_des_nkro, nkro, sizeof nkro);
    memcpy(g_des_boot, boot, sizeof boot);
    memcpy(g_des_cons, cons, sizeof cons);
    g_des_allowed = allowed;
#endif
    km_report_sync_pump(&g_sync_nkro, &k_io_nkro, nkro, allowed, now);
    km_report_sync_pump(&g_sync_boot, &k_io_boot, boot, allowed, now);
    km_report_sync_pump(&g_sync_cons, &k_io_cons, cons, allowed, now);
#if KM_M6_LAT_PROBE
    if (g_sync_nkro.sent != lat_sent0) {
        km_lat_sent(&g_lat, sense_cycles());
    }
#endif
    if (pad_active_now()) {
        uint8_t pb[KM_PAD_REPORT_LEN];
        km_pad_report_pack(&po.pad, pb);
        if (km_usbhs_variant() == KM_USB_VARIANT_XINPUT) {      /* pad_mode 2: the same state in the XInput layout */
            uint8_t xb[KM_USB_XI_REPORT_LEN];
            if (km_usb_xinput_from_pad(pb, xb)) {
                km_report_sync_pump(&g_sync_pad, &k_io_pad, xb, allowed, now);
            }
        } else {
            km_report_sync_pump(&g_sync_pad, &k_io_pad, pb, allowed, now);
        }
    }
    const uint32_t dt = sense_cycles() - t0;
    g_m6_last = dt;
    if (dt > g_m6_max) {
        g_m6_max = dt;
    }
    /* what the lights show of the keyboard's state */
    static km_led_inputs_t last_in;
    km_led_inputs_t in = { 0, lo->layer, g_settings.active, (uint8_t)((pad_active_now() ? KM_LED_IN_PAD : 0u) | (lo->toggle_keys != 0u ? KM_LED_IN_TOGGLE : 0u)) };
    if (memcmp(&in, &last_in, sizeof in) != 0) {
        last_in = in;
        m5_led_set_inputs(&in);
    }
}

/* ---- the light stream (0xC7) and the animation player -> the light core's external-frame inputs ---- */

static uint32_t g_stream_serial;
static bool g_stream_live, g_stream_fx_forced;
static uint8_t g_stream_prev_fx;

static void stream_to_lights(void)
{
    if (km_stream_active(&g_stream)) {
        if (!g_stream_live) {
            km_led_cfg_t c;
            m5_led_get_cfg(&c);
            if (c.effect != (uint8_t)KM_LED_FX_EXTERNAL) {       /* "instead of the normal effect for as long as the stream lives" */
                g_stream_prev_fx = c.effect;
                c.effect = (uint8_t)KM_LED_FX_EXTERNAL;
                g_stream_fx_forced = m5_led_set_cfg(&c);
            }
        }
        const uint32_t serial = km_stream_serial(&g_stream);
        if (!g_stream_live || serial != g_stream_serial) {
            const uint8_t *f = km_stream_frame(&g_stream);
            km_led_rgb_t px[KM_LED_COUNT];
            for (unsigned i = 0; i < KM_LED_COUNT; i++) {
                px[i].r = f[3u * i];
                px[i].g = f[3u * i + 1u];
                px[i].b = f[3u * i + 2u];
            }
            m5_led_ext_rgb(KM_LED_EXT_STREAM, px);
            g_stream_serial = serial;
        }
        g_stream_live = true;
    } else if (g_stream_live) {
        g_stream_live = false;
        m5_led_ext_end(KM_LED_EXT_STREAM);
        if (g_stream_fx_forced) {
            km_led_cfg_t c;
            m5_led_get_cfg(&c);
            if (c.effect == (uint8_t)KM_LED_FX_EXTERNAL) {       /* nobody changed the effect meanwhile: put the old one back */
                c.effect = g_stream_prev_fx;
                (void)m5_led_set_cfg(&c);
            }
            g_stream_fx_forced = false;
        }
    }
}

/* The clip, if one was linked into the image (tools/badapple_convert.py --c-array ... --symbol km_anim_clip). Weak symbols: an image
 * built without it has no player and nothing else changes. */
extern const uint8_t km_anim_clip[] __attribute__((weak));
extern const unsigned km_anim_clip_len __attribute__((weak));

static km_anim_info_t g_anim_info;
static km_anim_t g_anim;
static km_anim_verify_t g_anim_ver;
static uint8_t g_anim_state;               /* 0 no clip, 1 verifying, 2 ready, 3 refused (bad data) */

static void anim_boot(void)
{
    if ((const void *)km_anim_clip != 0 && (const void *)&km_anim_clip_len != 0 && km_anim_clip_len != 0u &&
        km_anim_parse(km_anim_clip, km_anim_clip_len, &g_anim_info) == KM_ANIM_OK) {
        km_anim_init(&g_anim, &g_anim_info);
        km_anim_verify_begin(&g_anim_ver, &g_anim_info);
        g_anim_state = 1;
    }
}

static void anim_service(uint32_t ms)
{
    if (g_anim_state == 1u && g_down_now == 0u) {          /* the background check: one budget unit per pass, never while a key is down */
        const km_anim_verify_result_t r = km_anim_verify_step(&g_anim_ver, 1);
        if (r == KM_ANIM_V_OK) {
            g_anim_state = 2;
            km_lightscene_t sc;
            m5_led_scene_get(&sc);
            if ((sc.anim_flags & KM_LS_ANIM_AUTOSTART) != 0u && sc.anim_clip == 0u) {
                km_led_cfg_t c;
                m5_led_get_cfg(&c);
                c.effect = (uint8_t)KM_LED_FX_ANIM;           /* "starts by itself": RAM only, the saved light configuration is not touched */
                (void)m5_led_set_cfg(&c);
            }
        } else if (r == KM_ANIM_V_BAD) {
            g_anim_state = 3;
        }
        return;
    }
    if (g_anim_state != 2u) {
        return;
    }
    km_led_cfg_t c;
    m5_led_get_cfg(&c);
    if (c.effect == (uint8_t)KM_LED_FX_ANIM) {
        if (!g_anim_running) {
            km_lightscene_t sc;
            m5_led_scene_get(&sc);
            km_anim_start(&g_anim, ms, sc.anim_offset_ms, (uint8_t)(KM_ANIM_PLAY_BLEND | ((sc.anim_flags & KM_LS_ANIM_LOOP) != 0u ? KM_ANIM_PLAY_LOOP : 0u)));
            g_anim_running = true;
        }
        uint8_t gray[KM_ANIM_LIGHTS];
        if (km_anim_tick(&g_anim, ms, gray)) {
            m5_led_ext_gray(KM_LED_EXT_ANIM, gray);
        }
        if (km_anim_state(&g_anim) != KM_ANIM_PLAYING) {       /* the clip ended (no loop) or the data broke: the lights show dark / their effect again */
            km_anim_stop(&g_anim);
            m5_led_ext_end(KM_LED_EXT_ANIM);
            g_anim_running = false;
            if (km_anim_state(&g_anim) == KM_ANIM_ERROR) {
                g_anim_state = 3;
            }
            c.effect = (uint8_t)KM_LED_FX_STATIC;
            (void)m5_led_set_cfg(&c);
        }
    } else if (g_anim_running) {
        km_anim_stop(&g_anim);
        m5_led_ext_end(KM_LED_EXT_ANIM);
        g_anim_running = false;
    }
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
        km_pipeline_reset(&g_pipe);                         /* toggle keys, DKS, gamepad, tap-hold state end with the connection ... */
        km_pipeline_hold_over(&g_pipe, g_reported, g_reported_n);   /* ... and keys still held do not start again by themselves */
    }
    g_ev_seen = ev;
    km_health_set_usb_suspended(&g_health, km_usbhs_suspended());
}

#define M6_SCOPE_TAG    0x0500u
#define M6_DIAG_VALUES  24u

/* Tag 0x0500, 24 values (all u16, counters saturate):
 *   [0] cycles of pump_reports() in the latest pass           [1] the largest so far        (the key path: SOCD, layers, DKS, gamepad, light actions, report building and sending)
 *   [2] cycles of the longest whole main loop pass so far (passes with a flash operation not counted)
 *   [3] passes longer than 9300 cycles (a sensing frame is 9.3k..10.4k): each can cost a frame
 *   [4] SOCD: keys suppressed so far       [5] DKS events applied    [6] DKS keys that could not be tracked   [7] DKS keys at most at once
 *   [8] gamepad state recomputations       [9] gamepad reports sent  [10] gamepad send retries   [11] gamepad resyncs   [12] pad keys at most at once
 *   [13] stream frames shown   [14] stream frames dropped+lost   [15] stream stale+rejected   [16] stream state (km_stream_state_t)
 *   [17] host lock lights (bits 0..2) | host LED reports known << 8      [18] host LED reports received
 *   [19] pad_mode in the settings | pad mode in force << 4 | USB fell back << 8 | descriptor variant << 12
 *   [20] animation: state (0 no clip, 1 verifying, 2 ready, 3 refused) | playing << 4 | km_anim_state << 8 | km_anim_error << 12
 *   [21] light-action keys handled | scene saves ok << 8 | scene saves failed << 12   [22] layer | toggled layers << 4 | toggle keys on << 8
 *   [23] pipeline resets (fault, USB reset, profile change) */
static void m6_diag(uint16_t d[M6_DIAG_VALUES])
{
    const km_stream_stats_t *st = km_stream_stats(&g_stream);
    d[0] = sat16(g_m6_last);
    d[1] = sat16(g_m6_max);
    d[2] = sat16(g_pass_max);
    d[3] = sat16(g_pass_over);
    d[4] = sat16(g_pipe.socd.suppress_events);
    d[5] = sat16(g_pipe.dks.events_applied);
    d[6] = sat16(g_pipe.dks.overflow_presses);
    d[7] = sat16(g_pipe.dks_keys_max);
    d[8] = sat16(g_pipe.pad.computes);
    d[9] = sat16(g_sync_pad.sent);
    d[10] = sat16(g_sync_pad.retries);
    d[11] = sat16(g_sync_pad.resyncs);
    d[12] = sat16(g_pipe.pad_keys_max);
    d[13] = st->committed;
    d[14] = sat16((uint32_t)st->dropped + st->lost);
    d[15] = sat16((uint32_t)st->stale + st->rejected);
    d[16] = (uint16_t)km_stream_state(&g_stream);
    d[17] = (uint16_t)((km_usbhs_host_leds() & 7u) | (km_usbhs_led_report_count() != 0u ? 0x100u : 0u));
    d[18] = sat16(km_usbhs_led_report_count());
    d[19] = (uint16_t)(g_settings.device.pad_mode | (pad_mode_in_force() << 4) | ((km_usbhs_fell_back() ? 1u : 0u) << 8) |
                       ((unsigned)km_usbhs_variant() << 12));
    d[20] = (uint16_t)(g_anim_state | ((g_anim_running ? 1u : 0u) << 4) | ((unsigned)km_anim_state(&g_anim) << 8) | ((unsigned)km_anim_error(&g_anim) << 12));
    d[21] = (uint16_t)((g_light_keys & 0xFFu) | ((g_scene_saves_ok & 0xFu) << 8) | ((g_scene_saves_failed & 0xFu) << 12));
    d[22] = (uint16_t)(g_layer | (g_toggled << 4) | (g_toggle_keys << 8));
    d[23] = sat16(g_pipe.resets);
}

int main(void)
{
    const uint8_t cause = km_reset_cause_read_and_clear();
    km_breadcrumb_t crumb;
    km_breadcrumb_boot(&crumb);

    sense_dwt_init();                                    /* the cycle counter must run before the first ms_clock() call (idempotent) */
    km_scan_init(&g_scan);
    km_engine_init(&g_engine);
    km_pipeline_init(&g_pipe);
    km_stream_init(&g_stream);
#if KM_M6_LAT_PROBE
    km_lat_init(&g_lat);
#endif
    static unsigned s_filter_mode;      /* what is in force (scope tag 0x0100, value 21); stays 0 when the engine has no filter modes */

    /* settings: defaults, then whatever the flash holds (persistent build) */
    km_settings_default(&g_settings);
#if KM_M5_PERSIST
    km_store_bind(km_store_flash());
    g_settings_source = km_store_load(&g_settings) ? 1u : 2u;
    if (g_settings_source == 2u) {
        km_settings_default(&g_settings);               /* a failed load leaves *s untouched, but be explicit */
    }
#endif
#if KM_ENGINE_FILTER_MODES
    s_filter_mode = g_settings.device.filter_mode != 0u ? g_settings.device.filter_mode : (unsigned)KM_M6_FILTER_MODE;
    if (!km_engine_set_filter_mode(&g_engine, s_filter_mode)) {
        s_filter_mode = 0u;
    }
    km_engine_set_fast_confirm(&g_engine, s_filter_mode != 0u);
#endif
    /* The USB descriptor set is chosen from the settings, ONCE: pad_mode = 1 presents the gamepad interface (image km_m6 only). The settings
     * are read first (a flash read, no erase), so the host sees the keyboard a few milliseconds later than in the M5 images. */
    km_usbhs_set_vendor_handler(vendor);
    km_usbhs_set_clock(ms_clock);
    km_usbhs_init_variant(!KM_M6_PAD ? KM_USB_VARIANT_PLAIN
                          : g_settings.device.pad_mode == 1u ? KM_USB_VARIANT_PAD
                          : g_settings.device.pad_mode == 2u ? KM_USB_VARIANT_XINPUT : KM_USB_VARIANT_PLAIN);
    apply_profile(g_settings.active);
#if KM_M5_PERSIST
    km_calmgr_init(&g_calmgr, km_calstore_flash(), &g_engine, cal_after_flash, 0);
#endif
#if KM_M6_DRIFT
    km_drift_init(&g_drift);
    km_drift_set_enabled(&g_drift, &g_engine, KM_M6_DRIFT == 2);
#endif
    km_layers_reset();
    km_config_init(&g_cfg, &k_cfg_ops);
    km_config_set_led_ops(&g_cfg, m5_led_cfg_ops());
    km_config_set_led_ex_ops(&g_cfg, m5_led_cfg_ex_ops());
    km_config_set_device_ops(&g_cfg, &k_dev_ops);
    km_config_set_scene_ops(&g_cfg, &k_scene_ops);
    km_config_set_extra_features(&g_cfg, KM_CFG_FEAT_STREAM | KM_CFG_FEAT_HOST_LEDS);
    km_vendor_set_config(&g_cfg);
    km_vendor_set_stream(&g_stream);

    const uint32_t init_err = adc_scan_init();
    adc_scan_start();
    km_usbhs_get_events(&g_ev_seen);

    uint32_t now = sense_cycles();
    km_health_init(&g_health, now, cause);
    static const uint8_t nkro_released[NKRO_REPORT_LEN] = { 0x01u };
    static const uint8_t boot_released[KM_REPORT_BOOT_LEN] = { 0 };
    static const uint8_t cons_released[KM_REPORT_CONSUMER_LEN] = { KM_REPORT_CONSUMER_ID, 0, 0 };
    static const uint8_t pad_released[KM_PAD_REPORT_LEN] = { 0 };
    static const uint8_t xi_released[KM_USB_XI_REPORT_LEN] = { KM_USB_XI_TYPE_INPUT, KM_USB_XI_LEN_BYTE };
    if (km_usbhs_variant() == KM_USB_VARIANT_XINPUT) {      /* the mode is fixed at boot: the sync works on 20-byte reports */
        km_report_sync_init(&g_sync_pad, xi_released, KM_USB_XI_REPORT_LEN, now);
    } else {
        km_report_sync_init(&g_sync_pad, pad_released, KM_PAD_REPORT_LEN, now);
    }
    km_report_sync_init(&g_sync_nkro, nkro_released, NKRO_REPORT_LEN, now);
    km_report_sync_init(&g_sync_boot, boot_released, KM_REPORT_BOOT_LEN, now);
    km_report_sync_init(&g_sync_cons, cons_released, KM_REPORT_CONSUMER_LEN, now);
    g_last_key_cycles = now;
    m5_led_init();
    m5_led_persist_set_legacy(km_ledstore_flash());                   /* an old single-record light configuration becomes the global lights */
    m5_led_persist_attach(km_ledprof_flash(), cal_after_flash, 0);    /* the stored per-profile lights, if there is a valid table */
    m5_led_set_profile(g_settings.active);                            /* the lights of the profile the keyboard starts in */
    (void)m5_led_scene_attach(km_lightscene_store_flash());           /* the stored light scene (indicator rules, key colours, animation settings) */
    m5_led_set_host_leds_source(km_usbhs_host_leds);                  /* the host's Caps / Num / Scroll Lock state */
    anim_boot();

    uint32_t pushed = 0, publishes = 0, evaluated = 0;
    uint32_t cal_frames = 0, cal_attempts = 0;
    uint16_t cal_result = 0xFFFFu;
    bool was_calibrated = false;
    uint32_t eng_last = 0, eng_max = 0, eng_total_max = 0, eng_irq_max = 0;
    uint8_t last_mask = 0;
    uint8_t crumb_flags = 0;
    uint16_t keymask_seq = 0;
    uint32_t next_second = now + KM_CPU_HZ;
#if KM_M6_FASTPATH
    uint32_t g_last_all_up = now;
    km_defer_init(&g_defer);
#endif
#if KM_M6_LAT_PROBE
    km_ustat_init(&g_ustat);
#endif
    uint32_t next = now;
    for (;;) {
        now = sense_cycles();
        const uint32_t pass_t0 = now;
        g_flash_this_pass = false;
        const uint32_t ms = ms_clock();
        km_stream_tick(&g_stream, ms);                      /* before the USB poll that may deliver the next stream packet */
        km_usbhs_poll();
#if KM_M6_LAT_PROBE
        km_ustat_observe(&g_ustat, sense_cycles(), km_usbhs_nkro_ready(), km_usbhs_sof_count());
#endif
        if (!km_report_sync_stalled(&g_sync_nkro, now) && !km_report_sync_stalled(&g_sync_boot, now) &&
            !km_report_sync_stalled(&g_sync_cons, now)) {
            km_health_progress(&g_health, KM_PROG_USB);     /* the poll ran AND the endpoint is taking what is due */
        }
        poll_usb_events();
        if (g_enter_boot) {
            adc_scan_stop();
            km_usbhs_disconnect();
            km_flash_request_bootloader();
        }

        if (g_recal_request) {                              /* user asked for a recalibration (keys were up when asked) */
            g_recal_request = false;
#if KM_M5_PERSIST
            km_calmgr_keep_learned(&g_calmgr);              /* what the keys have learned survives the recalibration */
#endif
            g_engine.calibrated = false;
            cal_frames = 0;
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
#if KM_M5_PERSIST
                        /* a record from flash: ONE live frame instead of the quiet stretch, if the sensor still looks like the record */
                        if (cal_frames == 0 && km_calmgr_try_restore(&g_calmgr, g_counts)) {
                            cal_result = 0;
                            cal_attempts++;
                            was_calibrated = true;
                            km_health_on_calibration(&g_health, true);
                        }
#endif
                        if (!g_engine.calibrated) {
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
#if KM_M5_PERSIST
                                if (g_engine.calibrated) {
                                    km_calmgr_armed(&g_calmgr, true);   /* put back what was learned before (a recalibration), where the sensor agrees */
                                }
#endif
                            }
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
                adc_scan_restart(now);
                const uint32_t e = adc_scan_take_dma_errors();
                g_dma_err_total += e;
                km_health_on_dma_error(&g_health, now, e);
            }
        }

        km_health_tick(&g_health, now);
#if KM_M6_FASTPATH
        const uint32_t keys_sent0 = g_sync_nkro.sent + g_sync_boot.sent + g_sync_cons.sent;
#endif
        pump_reports(now, g_health.fault_flags == 0u);
        km_health_set_report_stats(&g_health, g_sync_nkro.retries + g_sync_boot.retries + g_sync_cons.retries,
                                   g_sync_nkro.resyncs + g_sync_boot.resyncs + g_sync_cons.resyncs);
#if KM_M6_FASTPATH
        /* a pass that sent (or still owes) a key report leaves the slow housekeeping to the next quiet pass, at most 1 ms late */
        const bool key_event = KM_M6_DEFER && (((g_sync_nkro.sent + g_sync_boot.sent + g_sync_cons.sent) != keys_sent0) || keys_undelivered());
        if (KM_M6_DEFER == 0 || km_defer_slow_ok(&g_defer, key_event, sense_cycles(), KM_MS_CYCLES)) {
            stream_to_lights();
#if KM_M6_RETRY
            retry_keys();
#endif
            anim_service(ms);
#if KM_M6_RETRY
            retry_keys();
#endif
            m5_led_service(now, &g_engine, g_reported, g_reported_n, g_health.fault_flags);   /* after everything the keys need */
#if KM_M6_RETRY
            retry_keys();
#endif
        }
#else
        stream_to_lights();
        anim_service(ms);
        m5_led_service(now, &g_engine, g_reported, g_reported_n, g_health.fault_flags);   /* after everything the keys need */
#endif

        /* a profile change (key or command) is stored once the keyboard has been idle for a while */
        if (g_down_now != 0u) {
            g_last_key_cycles = now;
        } else if (g_active_dirty && (uint32_t)(now - g_last_key_cycles) >= SAVE_IDLE_CYCLES) {
            (void)save_settings();
            g_last_key_cycles = sense_cycles();
        }

#if KM_M5_PERSIST
        {   /* calibration save in the background: one flash operation per pass, only while nothing is happening */
            const bool idle_now = g_down_now == 0u && g_health.fault_flags == 0u;
            const bool idle_long = idle_now && !g_sync_nkro.undelivered && !g_sync_boot.undelivered && !g_sync_cons.undelivered &&
                                   (uint32_t)(now - g_last_key_cycles) >= CAL_IDLE_CYCLES;
            /* ONE flash operation per pass in all: a light-configuration save that has already erased its sector goes first (no record exists
             * until it is finished), then the calibration job, then a light-configuration save may use the pass if no calibration job is running */
            bool did_flash = false;
#if KM_M5_LED
#if KM_M6_FASTPATH
            /* the started write must finish, but not in the middle of play: keys held for 2 s with no gap let it go ahead anyway */
            if (g_down_now == 0u) {
                g_last_all_up = now;
            }
            if (m5_led_persist_urgent() && (g_down_now == 0u || (uint32_t)(now - g_last_all_up) >= 2u * KM_CPU_HZ)) {
#else
            if (m5_led_persist_urgent()) {
#endif
                did_flash = m5_led_persist_pass(g_uptime_s, idle_now, idle_long);
            }
#endif
            if (!did_flash) {
                did_flash = km_calmgr_pass(&g_calmgr, g_uptime_s, idle_now, idle_long);
            }
#if KM_M5_LED
            if (!did_flash && !km_calmgr_job_active(&g_calmgr)) {
                did_flash = m5_led_persist_pass(g_uptime_s, idle_now, idle_long);
            }
#endif
            if (did_flash) {
                g_last_key_cycles = sense_cycles();         /* the stall must not count as quiet time for the settings save above */
            }
        }
#endif

#if KM_M6_DRIFT
        km_drift_pass(&g_drift, &g_engine, g_uptime_s);     /* a few loads per pass; the work is once per second, nothing in the frame path */
#endif

        if (km_health_should_feed(&g_health, now)) {
            km_iwdg_feed();                                 /* the ONLY feed in the main loop */
        }
        if (!g_flash_this_pass) {                           /* a pass with a flash operation is slow by nature: not counted */
            const uint32_t pass = sense_cycles() - pass_t0;
            if (pass > g_pass_max) {
                g_pass_max = pass;
            }
            if (pass > PASS_BUDGET_CYCLES) {
                g_pass_over++;
            }
        }
        if (g_health.fault_flags != crumb_flags) {
            crumb_flags = g_health.fault_flags;
            km_breadcrumb_update(crumb_flags);
        }
        if ((int32_t)(now - next_second) >= 0) {
            next_second += KM_CPU_HZ;
            g_uptime_s++;
#if KM_M5_PERSIST
            km_calmgr_second(&g_calmgr, g_uptime_s);
#endif
        }

        if ((int32_t)(now - next) >= 0) {
            next += PUBLISH_CYCLES;
            publishes++;
            const uint32_t phase = publishes % 10u;
            if ((phase & 1u) == 0u) {
                uint16_t d[KM_KEYMASK_SCOPE_VALUES];
                const bool allowed = g_health.fault_flags == 0u;
                km_health_pack_keymask(g_reported, allowed ? g_reported_n : 0u, ++keymask_seq, g_health.fault_flags,
                                       g_sync_nkro.undelivered || g_sync_boot.undelivered || g_sync_cons.undelivered, d);
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
                    last_mask, (uint16_t)(s_filter_mode | (km_engine_get_fast_confirm(&g_engine) ? 0x10u : 0u)), 0, 0,
                };
                km_scope_publish(0x0100u, d, 24);
            } else if (phase == 5u) {
                unsigned n = 0;
                const uint8_t *down = km_engine_down_slots(&g_engine, &n);
                const uint32_t sent = g_sync_nkro.sent + g_sync_boot.sent + g_sync_cons.sent;
                uint16_t d[24] = {
                    cal_result, clamp16(cal_attempts), (uint16_t)n, (uint16_t)km_engine_glitch_count(&g_engine),
                    (uint16_t)(km_engine_glitch_count(&g_engine) >> 16), clamp16(eng_last), clamp16(eng_max),
                    (uint16_t)sent, (uint16_t)(sent >> 16),
                    km_usbhs_kbd_boot_protocol() ? 1u : 0u,
                    g_settings.active, KM_M5_PERSIST ? 1u : 0u,
                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                    g_engine.calibrated ? 1u : 0u, (uint16_t)evaluated, (uint16_t)(evaluated >> 16),
                    g_settings_source, clamp16(g_saves_ok), clamp16(g_saves_failed),
                };
                for (unsigned i = 0; i < 6u && i < n; i++) {
                    d[12 + i] = down[i];
                }
                km_scope_publish(0x0200u, d, 24);
            } else if (phase == 7u) {
                const uint32_t sent = g_sync_nkro.sent + g_sync_boot.sent + g_sync_cons.sent;
                const uint16_t extra[7] = {
                    (uint16_t)sent,
                    clamp16(g_sync_nkro.heartbeats + g_sync_boot.heartbeats + g_sync_cons.heartbeats),
                    clamp16(g_sync_nkro.recoveries + g_sync_boot.recoveries + g_sync_cons.recoveries),
                    crumb.valid ? crumb.prev_faults : 0xFFFFu,
                    clamp16(crumb.boots),
                    clamp16(g_uptime_s),
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
#if KM_M5_PERSIST
                if ((publishes % 100u) == 51u) {
                    /* tag 0x0202, calibration store: [0] bits 0-3 boot result (0 no record, 1 restored, 2 record disagreed, 3 frame refused),
                     * bit 4 record valid, bit 5 job running, bit 6 save due, bit 7 changed noticeably  [1] saves ok  [2] saves failed
                     * [3] last job result (km_calstore.h km_clog_result_t)  [4] strokes applied at boot  [5] log at boot: valid records
                     * | dirty slots << 8  [6] log at boot: next slot  [7] seconds since the last save (0xFFFF = none) */
                    const km_calmgr_t *cm = &g_calmgr;
                    const uint16_t d[8] = {
                        (uint16_t)(cm->boot | (cm->rec_valid ? 0x10u : 0u) | (km_calmgr_job_active(cm) ? 0x20u : 0u) |
                                   (cm->due ? 0x40u : 0u) | (km_calmgr_changed(cm) ? 0x80u : 0u)),
                        clamp16(cm->saves_ok), clamp16(cm->saves_failed), cm->last, cm->applied,
                        (uint16_t)(cm->scan0.used | (cm->scan0.dirty << 8)), (uint16_t)cm->scan0.next,
                        cm->pol.saved_once ? clamp16(g_uptime_s - cm->pol.last_save_s) : 0xFFFFu,
                    };
                    km_scope_publish(0x0202u, d, 8);
                }
#if KM_M6_DRIFT
                else if ((publishes % 100u) == 31u) {        /* drift compensation (tag 0x0210), layout in km_drift.h; one key per second, rotating through the keys */
                    uint16_t d[KM_DRIFT_SCOPE_VALUES];
                    km_drift_pack(&g_drift, &g_engine, g_uptime_s, d);
                    km_scope_publish(KM_DRIFT_SCOPE_TAG, d, KM_DRIFT_SCOPE_VALUES);
                }
#endif
                else if ((publishes % 100u) == 21u) {        /* the lights' diagnostics (tag 0x0400), phase 1 like the calibration store's */
                    uint16_t d[M5_LED_DIAG_VALUES];
                    m5_led_diag(d);
                    km_scope_publish(M5_LED_SCOPE_TAG, d, M5_LED_DIAG_VALUES);
                } else if ((publishes % 100u) == 71u) {      /* M6 diagnostics (tag 0x0500), see M6_TEST_GUIDE.md section 3 for the layout */
                    uint16_t d[M6_DIAG_VALUES];
                    m6_diag(d);
                    km_scope_publish(M6_SCOPE_TAG, d, M6_DIAG_VALUES);
                }
#if KM_M6_LAT_PROBE
#if KM_M6_FASTPATH
#define FP_SKIPPED clamp16(g_defer.skipped)
#define FP_RETRY clamp16(g_fast_retry_sends)
#else
#define FP_SKIPPED 0
#define FP_RETRY 0
#endif
                else if (((publishes / 10u) % 6u) == 0u) {   /* press-to-report probe (tag 0x0600): every 400 ms, in place of the flash diagnostics (0x0601, 0x0602 and 0x0203 take the other turns) */
                    uint16_t d[KM_LAT_VALUES];
                    km_lat_pack(&g_lat, KM_CPU_HZ / 1000000u, d);
                    km_scope_publish(KM_LAT_SCOPE_TAG, d, KM_LAT_VALUES);
                } else if (((publishes / 10u) % 6u) == 1u) {         /* endpoint busy time (tag 0x0601): how long a report keeps the NKRO endpoint occupied */
                    uint16_t d[KM_USTAT_VALUES];
                    km_ustat_pack(&g_ustat, KM_CPU_HZ / 1000000u, d);
                    km_scope_publish(KM_USTAT_SCOPE_TAG, d, KM_USTAT_VALUES);
                } else if (((publishes / 10u) % 6u) == 3u) {         /* micro-frames between send and "endpoint free" (tag 0x0603) */
                    uint16_t d[KM_USTAT_VALUES];
                    km_ustat_pack_uf(&g_ustat, d);
                    km_scope_publish(KM_USTAT_SCOPE_TAG_UF, d, KM_USTAT_VALUES);
                } else if (((publishes / 10u) % 6u) == 4u) {         /* the link: speed, SOF counter, cycle counter, served bInterval (tag 0x0604) */
                    size_t cn = 0;
                    const uint8_t *cfg = km_usb_config_desc_for(km_usbhs_variant(), &cn);
                    const uint32_t sof = km_usbhs_sof_count(), cyc = sense_cycles();
                    const uint16_t d[8] = {
                        km_usbhs_enum_speed(), (uint16_t)sof, (uint16_t)(sof >> 16), (uint16_t)cyc, (uint16_t)(cyc >> 16),
                        (uint16_t)km_usb_cfg_binterval(cfg, (unsigned)cn, KM_USB_EP_KBD_IN),
                        (uint16_t)km_usb_cfg_binterval(cfg, (unsigned)cn, KM_USB_EP_NKRO_IN), (uint16_t)km_usbhs_dsts(),
                    };
                    km_scope_publish(0x0604u, d, 8);
                } else if (((publishes / 10u) % 6u) == 2u) {   /* report delivery counters (tag 0x0602), see USB8K_NOTES.md */
                    const uint16_t d[8] = {
                        clamp16(g_sync_nkro.late + g_sync_boot.late + g_sync_cons.late),
                        clamp16(g_sync_nkro.deferred + g_sync_boot.deferred + g_sync_cons.deferred),
                        clamp16(g_sync_nkro.coalesced + g_sync_boot.coalesced + g_sync_cons.coalesced),
                        clamp16(g_sync_nkro.wait_max / (KM_CPU_HZ / 1000000u)),
                        clamp16(g_sync_boot.wait_max / (KM_CPU_HZ / 1000000u)),
                        FP_SKIPPED,
                        FP_RETRY,
                        clamp16(g_pass_over),
                    };
                    km_scope_publish(0x0602u, d, 8);
                }
#endif
                else {
                    uint16_t fd[5];
                    km_store_flash_diag(fd);
                    const uint16_t d[8] = { (uint16_t)km_store_last_result(), fd[0], fd[1], fd[2], fd[3], fd[4], clamp16(g_saves_ok), clamp16(g_saves_failed) };
                    km_scope_publish(0x0203u, d, 8);   /* flash diagnostics in place of the sums snapshot */
                }
#else
#if KM_M5_LED
                if (publishes % 80u == 1u) {                /* one in eight of the sums snapshots makes room for the LED diagnostics */
                    uint16_t d[M5_LED_DIAG_VALUES];
                    m5_led_diag(d);
                    km_scope_publish(M5_LED_SCOPE_TAG, d, M5_LED_DIAG_VALUES);
                } else
#endif
                km_scope_publish(0x0003u, km_scan_sums(&g_scan), 76);
#endif
            }
        }
    }
}
