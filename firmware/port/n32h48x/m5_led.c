/*
 * m5_led.c - the LED service of the M5 image, see m5_led.h and LED_NOTES.md.
 */
#include "m5_led.h"

#include <string.h>

#include "km/km_health.h"
#include "km/km_led_guard.h"
#include "km/km_ledprof.h"
#include "km/km_ledstore.h"
#include "led_hw.h"

#ifdef KM_LED_HOST_TEST
uint32_t km_led_sim_cycles(void);
#define LED_CYCLES() km_led_sim_cycles()
static km_led_rec_t s_sim_rec;
#define REC_PTR() (&s_sim_rec)
#else
#include "sense_hw.h"
#define LED_CYCLES() sense_cycles()
/* RAM that survives a reset and that the image never uses: next to the health breadcrumb of reset_cause.c (0x20025F00, 16 bytes). */
#define REC_ADDR 0x20025F20u
extern uint32_t _estack;      /* end of the stack, which the linker script places right behind .bss */
#define REC_PTR() (((uintptr_t)&_estack <= REC_ADDR) ? (km_led_rec_t *)(uintptr_t)REC_ADDR : (km_led_rec_t *)0)
#endif

enum { ST_OFF = 0, ST_WAIT_HEALTHY, ST_RUN };

static km_led_t g_led;
static km_ledstore_t g_lps;        /* zero = no persistence (log NULL) */
static km_ledprof_t g_prof_tab;    /* the per-profile lights (km_ledprof.h): RAM copy of the newest table, used when g_prof_mode */
static bool g_prof_mode;           /* the attached log is the 64-byte table log: the lights are stored per key profile */
static uint8_t g_prof_active;      /* the key profile in force (m5_led_set_profile) */
static const km_clog_t *g_legacy_log;   /* version-1 reader of the same sector, for the boot migration */
static km_led_guard_t g_guard;
static km_lightscene_t g_scene;                        /* the scene the core points at */
static const km_store_t *g_scene_store;                /* NULL = no scene persistence */
static m5_led_host_leds_fn g_host_fn;
static km_led_inputs_t g_in;
static uint8_t g_dark[KM_LED_FRAME_BYTES];

static struct {
    uint8_t  st;
    bool     in_flight;
    bool     healthy;
    bool     killed_by_guard;
    uint32_t last_cycles, acc_cycles, ms;
    uint32_t healthy_since_ms;
    uint32_t svc_last, svc_max;
} s;

#ifdef KM_LED_HOST_TEST
void m5_led_test_power_cycle(void)      /* the RAM record is lost, as after unplugging the keyboard */
{
    memset(&s_sim_rec, 0, sizeof s_sim_rec);
}

km_led_t *m5_led_test_core(void)        /* the LED core, for the tests of the host operations */
{
    return &g_led;
}
#endif

static uint16_t clamp16(uint32_t v)
{
    return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
}

static uint16_t depth_cb(void *ctx, unsigned slot)
{
    return km_engine_depth((const km_engine_t *)ctx, slot);
}

void m5_led_init(void)
{
    memset(&s, 0, sizeof s);
    km_led_init(&g_led);
    km_lightscene_default(&g_scene);
    km_led_set_scene(&g_led, &g_scene);
    g_scene_store = NULL;
    g_host_fn = NULL;
    km_ledprof_default(&g_prof_tab);
    g_prof_mode = false;
    g_prof_active = 0;
    g_legacy_log = NULL;
    memset(&g_in, 0, sizeof g_in);
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {                  /* the all-dark frame the hardware starts with */
        const km_led_rgb_t black = { 0, 0, 0 };
        km_led_encode_pixel(&black, &g_dark[KM_LED_LEAD_BYTES + i * KM_LED_BYTES_PER_LED]);
    }
    km_led_guard_boot(&g_guard, REC_PTR(), km_led_build_id(__DATE__ " " __TIME__));
    s.st = km_led_guard_may_start(&g_guard) ? ST_WAIT_HEALTHY : ST_OFF;
    s.killed_by_guard = (s.st == ST_OFF);
    s.last_cycles = LED_CYCLES();
}

static void shut_down(void)
{
    led_hw_kill();
    s.st = ST_OFF;
    s.killed_by_guard = true;
}

static void advance_time(uint32_t now_cycles)
{
    s.acc_cycles += (uint32_t)(now_cycles - s.last_cycles);
    s.last_cycles = now_cycles;
    const uint32_t ms = s.acc_cycles / KM_MS_CYCLES;
    s.acc_cycles -= ms * KM_MS_CYCLES;
    s.ms += ms;
}

void m5_led_service(uint32_t now_cycles, const km_engine_t *eng, const uint8_t *down, unsigned n_down, uint8_t fault_flags)
{
    if (s.st == ST_OFF) {
        return;
    }
    const uint32_t t0 = LED_CYCLES();
    advance_time(now_cycles);

    if (s.st == ST_WAIT_HEALTHY) {
        if (fault_flags != 0u) {
            s.healthy = false;
        } else if (!s.healthy) {
            s.healthy = true;
            s.healthy_since_ms = s.ms;
        } else if ((uint32_t)(s.ms - s.healthy_since_ms) >= M5_LED_START_DELAY_MS) {
            km_led_guard_hw_starting(&g_guard, s.ms);                  /* recorded BEFORE the first register access */
            if (led_hw_start(g_dark, KM_LED_FRAME_BYTES, now_cycles)) {
                s.in_flight = true;
                s.st = ST_RUN;
            } else {
                km_led_guard_hw_failed(&g_guard);
                shut_down();
            }
        }
    } else {
        const km_led_verdict_t v = km_led_guard_step(&g_guard, s.ms, (uint8_t)(fault_flags & ~KM_FAULT_USB_SUSPENDED));
        if (v == KM_LED_KILL) {
            shut_down();
        } else {
            km_led_set_blank(&g_led, fault_flags != 0u);               /* a fault or a suspended bus: lights off */
            km_led_note_keys(&g_led, down, n_down);
            if (g_host_fn != NULL) {
                g_in.host_leds = g_host_fn();
            }
            km_led_set_inputs(&g_led, &g_in);

            const led_hw_state_t hs = led_hw_poll(now_cycles);
            if (hs == LED_HW_TIMEOUT) {
                km_led_guard_dma_result(&g_guard, true);
                s.in_flight = false;
            } else if (hs == LED_HW_IDLE && s.in_flight) {
                km_led_guard_dma_result(&g_guard, false);
                s.in_flight = false;
            }
            if (g_guard.tripped) {
                shut_down();
            } else {
                (void)km_led_work(&g_led, s.ms, depth_cb, (void *)(uintptr_t)eng, M5_LED_WORK_UNITS);
                const uint8_t *f = km_led_frame(&g_led);
                if (f != NULL && hs != LED_HW_BUSY && led_hw_send(f, KM_LED_FRAME_BYTES, now_cycles)) {
                    km_led_frame_taken(&g_led);
                    s.in_flight = true;
                }
            }
        }
    }

    s.svc_last = (uint32_t)(LED_CYCLES() - t0);
    if (s.svc_last > s.svc_max) {
        s.svc_max = s.svc_last;
    }
}

void m5_led_get_cfg(km_led_cfg_t *cfg)
{
    *cfg = g_led.cfg;
}

bool m5_led_set_cfg(const km_led_cfg_t *cfg)
{
    if (!km_led_set_cfg(&g_led, cfg)) {
        return false;
    }
    km_led_wake(&g_led);
    return true;
}

void m5_led_set_host_leds_source(m5_led_host_leds_fn fn)
{
    g_host_fn = fn;
    if (fn == NULL) {
        g_in.host_leds = 0;
        km_led_set_inputs(&g_led, &g_in);
    }
}

void m5_led_set_inputs(const km_led_inputs_t *in)
{
    g_in.layer = in->layer;
    g_in.profile = in->profile;
    g_in.flags = in->flags;
    km_led_set_inputs(&g_led, &g_in);
}

bool m5_led_scene_set(const km_lightscene_t *sc)
{
    if (!km_lightscene_valid(sc)) {
        return false;
    }
    g_scene = *sc;                                      /* the core points at g_scene: the next frame uses the new content */
    km_led_set_scene(&g_led, &g_scene);
    km_led_wake(&g_led);
    return true;
}

void m5_led_scene_get(km_lightscene_t *sc)
{
    *sc = g_scene;
}

bool m5_led_scene_attach(const km_store_t *store)
{
    g_scene_store = store;
    km_lightscene_t t;
    if (store != NULL && km_lightscene_load(store, &t)) {
        return m5_led_scene_set(&t);
    }
    return false;
}

km_store_result_t m5_led_scene_commit(const km_lightscene_t *sc, bool save)
{
    if (!km_lightscene_valid(sc) || (save && g_scene_store == NULL)) {
        return KM_STORE_ERR_ARG;
    }
    (void)m5_led_scene_set(sc);
    return save ? km_lightscene_save(g_scene_store, sc) : KM_STORE_OK;
}

void m5_led_ext_rgb(unsigned source, const km_led_rgb_t px[KM_LED_COUNT])
{
    km_led_ext_rgb(&g_led, source, px, s.ms);
}

void m5_led_ext_gray(unsigned source, const uint8_t gray[KM_LED_COUNT])
{
    km_led_ext_gray(&g_led, source, gray, s.ms);
}

void m5_led_ext_end(unsigned source)
{
    km_led_ext_end(&g_led, source);
}

void m5_led_persist_set_legacy(const km_clog_t *legacy)
{
    g_legacy_log = legacy;
}

/* Apply the lights the table gives a profile: its own entry, else the global one, else the built-in defaults. RAM only. */
static void prof_apply(unsigned p)
{
    km_ledstore_rec_t r;
    if (!km_ledprof_effective(&g_prof_tab, p, &r)) {
        km_ledstore_rec_default(&r);
    }
    (void)km_led_set_cfg(&g_led, &r.cfg);                          /* valid by construction (km_ledprof_decode / km_ledprof_set / the defaults) */
    km_led_set_idle(&g_led, r.idle_s);
}

void m5_led_set_profile(unsigned profile)
{
    if (profile >= KM_LEDPROF_PROFILES) {
        return;
    }
    const bool changed = profile != g_prof_active;
    g_prof_active = (uint8_t)profile;
    if (g_prof_mode && changed) {
        prof_apply(profile);
        km_led_wake(&g_led);                                       /* the change is something to look at: faded-out lights come back */
    }
}

void m5_led_persist_attach(const km_clog_t *log, void (*after_flash)(void *ctx), void *ctx)
{
    km_ledstore_init(&g_lps, log, after_flash, ctx);
    g_prof_mode = log != NULL && log->payload_len == KM_LEDPROF_PAYLOAD_BYTES;
    if (g_prof_mode) {
        km_ledprof_default(&g_prof_tab);
        g_lps.loaded = km_ledprof_load(log, g_legacy_log, &g_prof_tab);
        if (g_lps.loaded) {
            prof_apply(g_prof_active);
        }
        return;
    }
    km_ledstore_rec_t r;
    if (log != NULL && km_ledstore_boot(&g_lps, &r)) {
        (void)km_led_set_cfg(&g_led, &r.cfg);                      /* valid by construction (km_ledstore_decode) */
        km_led_set_idle(&g_led, r.idle_s);
    }
}

bool m5_led_persist_pass(uint32_t now_s, bool idle_now, bool idle_long)
{
    return km_ledstore_pass(&g_lps, now_s, idle_now, idle_long);
}

bool m5_led_persist_urgent(void)
{
    return km_ledstore_must_finish(&g_lps);
}

static km_cfg_status_t cfg_op_get(void *ctx, uint8_t v[KM_CONFIG_LED_BYTES])
{
    (void)ctx;
    v[0] = g_led.cfg.effect;
    v[1] = g_led.cfg.brightness;
    v[2] = g_led.cfg.speed;
    v[3] = g_led.cfg.color.r;
    v[4] = g_led.cfg.color.g;
    v[5] = g_led.cfg.color.b;
    return KM_CFG_OK;
}

static km_cfg_status_t cfg_op_set(void *ctx, const uint8_t v[KM_CONFIG_LED_BYTES])
{
    (void)ctx;
    km_led_cfg_t c;
    c.effect = v[0];
    c.brightness = v[1];
    c.speed = v[2];
    c.color.r = v[3];
    c.color.g = v[4];
    c.color.b = v[5];
    if (!km_led_set_cfg(&g_led, &c)) {
        return KM_CFG_ERR_RANGE;
    }
    km_led_wake(&g_led);                  /* a change from the host is something to look at: faded-out lights come back */
    return KM_CFG_OK;
}

static uint8_t sat8(unsigned v)
{
    return v > 255u ? 255u : (uint8_t)v;
}

/* The extended reply of LED_GET / LED_SET (km_config.h): light bytes, idle seconds, status bits, latest save result and the counters. */
static km_cfg_status_t cfg_op_get_ex(void *ctx, uint8_t v[KM_CONFIG_LED_GET_EX_BYTES])
{
    uint8_t six[KM_CONFIG_LED_BYTES];
    (void)cfg_op_get(ctx, six);
    memcpy(v, six, sizeof six);
    v[6] = (uint8_t)(g_led.idle_s & 0xFFu);
    v[7] = (uint8_t)(g_led.idle_s >> 8);
    v[8] = (uint8_t)((km_ledstore_pending(&g_lps) ? KM_CFG_LED_ST_SAVE_PENDING : 0u) |
                     (g_lps.log != NULL && g_lps.last >= (uint8_t)KM_CLOG_ERR_ARG ? KM_CFG_LED_ST_SAVE_FAILED : 0u) |
                     (g_lps.loaded ? KM_CFG_LED_ST_LOADED : 0u) | (km_led_idle(&g_led) ? KM_CFG_LED_ST_IDLE_OFF : 0u) |
                     (g_lps.log != NULL ? KM_CFG_LED_ST_PERSISTENT : 0u) |
                     (g_prof_mode && km_ledprof_has_own(&g_prof_tab, g_prof_active) ? KM_CFG_LED_ST_PROFILE_OWN : 0u));
    v[9] = g_lps.last;
    v[10] = sat8(g_lps.saves_ok);
    v[11] = sat8(g_lps.saves_failed);
    return KM_CFG_OK;
}

/* profile: 0..3 or KM_CFG_LED_PROFILE_ACTIVE. The active profile is applied now (and saved with the SAVE flag); another profile is only
 * stored (the SAVE flag is required) and shows when it is selected. Outside the table mode there is one set of lights: only the active
 * profile can be addressed. */
static km_cfg_status_t led_set_profile_ex(unsigned profile, const uint8_t v[KM_CONFIG_LED_SET_EX_BYTES])
{
    km_ledstore_rec_t r;
    r.cfg.effect = v[0];
    r.cfg.brightness = v[1];
    r.cfg.speed = v[2];
    r.cfg.color.r = v[3];
    r.cfg.color.g = v[4];
    r.cfg.color.b = v[5];
    r.idle_s = (uint16_t)(v[6] | ((unsigned)v[7] << 8));
    const bool save = (v[8] & KM_CFG_LED_FLAG_SAVE) != 0u;
    if (!km_ledstore_rec_valid(&r)) {
        return KM_CFG_ERR_RANGE;
    }
    if (save && g_lps.log == NULL) {
        return KM_CFG_ERR_UNSUPPORTED;    /* this build keeps nothing in flash: apply nothing, so the host does not believe it was saved */
    }
    const unsigned p = (profile == KM_CFG_LED_PROFILE_ACTIVE) ? g_prof_active : profile;
    if (p >= KM_LEDPROF_PROFILES) {
        return KM_CFG_ERR_RANGE;
    }
    const bool is_active = p == g_prof_active;
    if (!g_prof_mode && !is_active) {
        return KM_CFG_ERR_UNSUPPORTED;
    }
    if (!is_active && !save) {
        return KM_CFG_ERR_RANGE;          /* nothing to preview on a profile that is not showing */
    }
    km_ledprof_t tmp = g_prof_tab;
    uint8_t payload[KM_LEDPROF_PAYLOAD_BYTES];
    if (save && g_prof_mode) {            /* build the new table first: a failure here changes nothing */
        if (!km_ledprof_set(&tmp, p, &r) || km_ledprof_encode(&tmp, payload, sizeof payload) == 0u) {
            return KM_CFG_ERR_FAILED;
        }
    }
    if (is_active) {
        if (!km_led_set_cfg(&g_led, &r.cfg)) {
            return KM_CFG_ERR_RANGE;
        }
        km_led_set_idle(&g_led, r.idle_s);
        km_led_wake(&g_led);
    }
    if (save) {
        if (g_prof_mode) {
            if (!km_ledstore_request_raw(&g_lps, payload, sizeof payload)) {
                return KM_CFG_ERR_FAILED;
            }
            g_prof_tab = tmp;
        } else if (!km_ledstore_request(&g_lps, &r)) {
            return KM_CFG_ERR_FAILED;
        }
    }
    return KM_CFG_OK;
}

static km_cfg_status_t cfg_op_set_ex(void *ctx, const uint8_t v[KM_CONFIG_LED_SET_EX_BYTES])
{
    (void)ctx;
    return led_set_profile_ex(KM_CFG_LED_PROFILE_ACTIVE, v);
}

static km_cfg_status_t cfg_op_set_p(void *ctx, unsigned profile, const uint8_t v[KM_CONFIG_LED_SET_EX_BYTES])
{
    (void)ctx;
    return led_set_profile_ex(profile, v);
}

static km_cfg_status_t cfg_op_get_p(void *ctx, unsigned profile, uint8_t v[KM_CONFIG_LED_GET_EX_BYTES])
{
    const unsigned p = (profile == KM_CFG_LED_PROFILE_ACTIVE) ? g_prof_active : profile;
    if (p >= KM_LEDPROF_PROFILES || (!g_prof_mode && p != g_prof_active)) {
        return KM_CFG_ERR_RANGE;
    }
    (void)cfg_op_get_ex(ctx, v);                          /* flags and counters; the light bytes are replaced below when another profile is asked for */
    if (p == g_prof_active) {
        return KM_CFG_OK;
    }
    km_ledstore_rec_t r;
    if (!km_ledprof_effective(&g_prof_tab, p, &r)) {
        km_ledstore_rec_default(&r);
    }
    v[0] = r.cfg.effect;
    v[1] = r.cfg.brightness;
    v[2] = r.cfg.speed;
    v[3] = r.cfg.color.r;
    v[4] = r.cfg.color.g;
    v[5] = r.cfg.color.b;
    v[6] = (uint8_t)(r.idle_s & 0xFFu);
    v[7] = (uint8_t)(r.idle_s >> 8);
    v[8] = (uint8_t)((v[8] & (uint8_t)~(KM_CFG_LED_ST_PROFILE_OWN | KM_CFG_LED_ST_IDLE_OFF)) |
                     (km_ledprof_has_own(&g_prof_tab, p) ? KM_CFG_LED_ST_PROFILE_OWN : 0u));
    return KM_CFG_OK;
}

const km_config_led_ops_t *m5_led_cfg_ops(void)
{
    static const km_config_led_ops_t k_ops = { 0, cfg_op_get, cfg_op_set };
    return &k_ops;
}

const km_config_led_ex_ops_t *m5_led_cfg_ex_ops(void)
{
    static const km_config_led_ex_ops_t k_ops = { 0, cfg_op_get_ex, cfg_op_set_ex, cfg_op_get_p, cfg_op_set_p };
    return &k_ops;
}

void m5_led_diag(uint16_t d[M5_LED_DIAG_VALUES])
{
    led_hw_diag_t h;
    led_hw_get_diag(&h);
    d[0] = (uint16_t)((h.started ? 1u : 0u) | (h.killed ? 2u : 0u) | (g_guard.boot_allowed ? 0u : 4u) | (g_guard.stable ? 8u : 0u) |
                      (g_guard.tripped ? 16u : 0u) | (g_led.blank ? 32u : 0u) | (s.in_flight ? 64u : 0u) |
                      (g_guard.rec != NULL ? 128u : 0u) | ((unsigned)s.st << 8));
    d[1] = (uint16_t)(g_guard.why | ((unsigned)h.level << 8));
    d[2] = (uint16_t)(clamp16(g_guard.strikes) & 0xFFu) | (uint16_t)((unsigned)g_guard.episodes << 8);
    d[3] = (uint16_t)g_led.frames;
    d[4] = (uint16_t)(g_led.frames >> 16);
    d[5] = clamp16(h.sent);
    d[6] = clamp16(h.timeouts);
    d[7] = clamp16(s.svc_max);
    d[8] = clamp16(s.svc_last);
    d[9] = clamp16(g_led.units_max);
    d[10] = g_led.cfg.effect;
    d[11] = g_led.cfg.brightness;
    d[12] = g_led.cfg.speed;
    d[13] = clamp16(g_led.last_total);
    d[14] = clamp16(g_led.last_scale16 >> 4);                          /* 4096 = frame not scaled by the budget */
    d[15] = (uint16_t)h.spi_ctrl1;
    d[16] = (uint16_t)h.spi_ctrl2;
    d[17] = (uint16_t)h.dma_cfg;
    d[18] = clamp16(h.dma_txnum);
    d[19] = (uint16_t)h.rcc_cfg;
    d[20] = (uint16_t)(h.rcc_cfg >> 16);
    d[21] = (uint16_t)h.rcc_cfg2;
    d[22] = (uint16_t)(h.rcc_cfg2 >> 16);
    d[23] = (uint16_t)h.rcc_apb2;
}
