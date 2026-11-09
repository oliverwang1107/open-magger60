#include "km/km_health.h"

#include <stddef.h>
#include <string.h>

#define TRANSIENT_FAULTS (KM_FAULT_SCAN_STALL | KM_FAULT_DMA_ERROR | KM_FAULT_ENGINE_OVERRUN)
#define SLOTS 76u

/* a is at or after b, for times closer than 2^31 cycles (8.9 s) */
static bool reached(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

static void start_holdoff(km_health_t *h, uint32_t now)
{
    h->hold_until = now + KM_HEALTH_HOLDOFF_CYCLES;
}

void km_health_init(km_health_t *h, uint32_t now_cycles, uint8_t reset_cause)
{
    memset(h, 0, sizeof *h);
    h->reset_cause = reset_cause;
    h->fault_flags = KM_FAULT_CAL_LOST;
    h->scan_ts = h->usb_ts = h->engine_ts = now_cycles;
    h->hold_until = now_cycles;
    h->require = KM_PROG_SCAN | KM_PROG_USB;
    h->transient = TRANSIENT_FAULTS;
    h->frame_seen_since_stall = true;
}

void km_health_progress(km_health_t *h, unsigned what)
{
    h->prog_pending = (uint8_t)(h->prog_pending | (what & (KM_PROG_SCAN | KM_PROG_USB | KM_PROG_ENGINE)));
}

/* true while `ts` is not older than the feed window */
static bool fresh(uint32_t now, uint32_t ts)
{
    return (uint32_t)(now - ts) <= KM_HEALTH_FEED_WINDOW_CYCLES;
}

bool km_health_should_feed(km_health_t *h, uint32_t now_cycles)
{
    if (h->prog_pending & KM_PROG_SCAN) {
        h->scan_ts = now_cycles;
    }
    if (h->prog_pending & KM_PROG_USB) {
        h->usb_ts = now_cycles;
    }
    if (h->prog_pending & KM_PROG_ENGINE) {
        h->engine_ts = now_cycles;
    }
    h->prog_pending = 0;
    if ((h->require & KM_PROG_SCAN) && !fresh(now_cycles, h->scan_ts)) {
        return false;
    }
    if ((h->require & KM_PROG_USB) && !fresh(now_cycles, h->usb_ts)) {
        return false;
    }
    if ((h->require & KM_PROG_ENGINE) && !fresh(now_cycles, h->engine_ts)) {
        return false;
    }
    return true;
}

bool km_health_keys_allowed(const km_health_t *h)
{
    return h->fault_flags == 0;
}

void km_health_tick(km_health_t *h, uint32_t now_cycles)
{
    if (!(h->fault_flags & h->transient) || !reached(now_cycles, h->hold_until)) {
        return;
    }
    uint8_t clear = (uint8_t)(h->fault_flags & h->transient);
    if (!h->frame_seen_since_stall) {
        clear = (uint8_t)(clear & ~KM_FAULT_SCAN_STALL);   /* a stall ends only when a frame has actually arrived */
    }
    h->fault_flags = (uint8_t)(h->fault_flags & ~clear);
}

void km_health_on_frame(km_health_t *h)
{
    h->frames++;
    h->frame_seen_since_stall = true;
    km_health_progress(h, KM_PROG_SCAN);
}

bool km_health_on_scan_timeout(km_health_t *h, uint32_t now_cycles)
{
    h->scan_stalls++;
    h->scan_restarts++;
    h->fault_flags |= KM_FAULT_SCAN_STALL;
    h->frame_seen_since_stall = false;
    start_holdoff(h, now_cycles);
    return true;
}

void km_health_on_dma_error(km_health_t *h, uint32_t now_cycles, uint32_t new_errors)
{
    if (new_errors == 0) {
        return;
    }
    h->dma_errors += new_errors;
    h->fault_flags |= KM_FAULT_DMA_ERROR;
    start_holdoff(h, now_cycles);
}

void km_health_on_engine(km_health_t *h, uint32_t now_cycles, uint32_t cycles)
{
    if (cycles > h->engine_max_cycles) {
        h->engine_max_cycles = cycles;
    }
    if (cycles > KM_HEALTH_ENGINE_BUDGET_CYCLES) {
        h->engine_over_budget++;
    }
    if (cycles > KM_HEALTH_ENGINE_LIMIT_CYCLES) {
        h->engine_overruns++;
        h->fault_flags |= KM_FAULT_ENGINE_OVERRUN;
        start_holdoff(h, now_cycles);
    }
    km_health_progress(h, KM_PROG_ENGINE);
}

void km_health_on_calibration(km_health_t *h, bool ok)
{
    if (ok) {
        h->calibrations++;
        h->fault_flags = (uint8_t)(h->fault_flags & ~KM_FAULT_CAL_LOST);
    } else {
        h->calibration_refused++;
        h->fault_flags |= KM_FAULT_CAL_LOST;
    }
}

void km_health_cal_lost(km_health_t *h)
{
    h->fault_flags |= KM_FAULT_CAL_LOST;
}

void km_health_set_glitches(km_health_t *h, uint32_t engine_glitch_count)
{
    h->glitches = engine_glitch_count;
}

void km_health_on_usb(km_health_t *h, km_usb_event_t ev)
{
    switch (ev) {
    case KM_USB_EV_RESET:
        h->usb_resets++;
        h->usb_susp = false;
        h->fault_flags = (uint8_t)(h->fault_flags & ~KM_FAULT_USB_SUSPENDED);   /* a reset ends any suspend */
        break;
    case KM_USB_EV_SUSPEND:
        h->usb_susp = true;
        h->usb_suspends++;
        h->fault_flags |= KM_FAULT_USB_SUSPENDED;
        break;
    case KM_USB_EV_RESUME:
        h->usb_susp = false;
        h->usb_resumes++;
        h->fault_flags = (uint8_t)(h->fault_flags & ~KM_FAULT_USB_SUSPENDED);
        break;
    default:
        break;
    }
}

void km_health_set_usb_suspended(km_health_t *h, bool suspended)
{
    if (suspended != h->usb_susp) {
        km_health_on_usb(h, suspended ? KM_USB_EV_SUSPEND : KM_USB_EV_RESUME);
    }
}

void km_health_set_report_stats(km_health_t *h, uint32_t retries, uint32_t resyncs)
{
    h->report_retries = retries;
    h->report_resyncs = resyncs;
}

uint8_t km_health_reset_cause_from_reg(uint32_t reg)
{
    return (uint8_t)((reg >> 24) & ~KM_RESET_RMV);
}

const char *km_health_reset_cause_name(uint8_t cause)
{
    if (cause & KM_RESET_IWDG) {
        return "iwdg";
    }
    if (cause & KM_RESET_WWDG) {
        return "wwdg";
    }
    if (cause & KM_RESET_SOFT) {
        return "soft";
    }
    if (cause & KM_RESET_POR) {
        return "por";
    }
    if (cause & KM_RESET_PIN) {
        return "pin";
    }
    if (cause & KM_RESET_LPWR) {
        return "lowpower";
    }
    return "none";
}

static uint16_t c16(uint32_t v)
{
    return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
}

void km_health_pack_scope(const km_health_t *h, const uint16_t extra[7], uint16_t out[KM_HEALTH_SCOPE_VALUES])
{
    out[0] = (uint16_t)(h->fault_flags | ((unsigned)h->reset_cause << 8));
    out[1] = (uint16_t)h->frames;
    out[2] = (uint16_t)(h->frames >> 16);
    out[3] = c16(h->scan_stalls);
    out[4] = c16(h->dma_errors);
    out[5] = c16(h->scan_restarts);
    out[6] = c16(h->engine_max_cycles);
    out[7] = c16(h->engine_over_budget);
    out[8] = c16(h->engine_overruns);
    out[9] = c16(h->usb_resets);
    out[10] = c16(h->usb_suspends);
    out[11] = c16(h->usb_resumes);
    out[12] = c16(h->report_retries);
    out[13] = c16(h->report_resyncs);
    out[14] = c16(h->glitches);
    out[15] = c16(h->calibrations);
    out[16] = c16(h->calibration_refused);
    for (unsigned i = 0; i < 7u; i++) {
        out[17 + i] = extra != NULL ? extra[i] : 0u;
    }
}

void km_health_pack_keymask(const uint8_t *down_slots, unsigned n, uint16_t seq, uint8_t fault_flags, bool undelivered,
                            uint16_t out[KM_KEYMASK_SCOPE_VALUES])
{
    memset(out, 0, KM_KEYMASK_SCOPE_VALUES * sizeof out[0]);
    for (unsigned i = 0; down_slots != NULL && i < n; i++) {
        const unsigned s = down_slots[i];
        if (s < SLOTS) {
            out[s >> 4] = (uint16_t)(out[s >> 4] | (1u << (s & 15u)));
        }
    }
    out[5] = seq;
    out[6] = fault_flags;
    out[7] = undelivered ? 1u : 0u;
}
