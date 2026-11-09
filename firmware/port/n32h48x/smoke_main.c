/*
 * Smoke test image: runs the portable core on synthetic data so the linker keeps a
 * representative amount of code and the size/layout checks mean something. It does not
 * touch any peripheral and is NOT meant to be flashed.
 */
#include <stdint.h>

#include "km/km_engine.h"
#include "km/km_hid.h"
#include "km/km_report.h"
#include "km/km_scan.h"

volatile uint32_t km_smoke_sink;

static km_engine_t g_engine;
static km_scan_t   g_scan;

int main(void)
{
    uint16_t counts[KM_ENGINE_SLOTS];
    uint32_t words[KM_SCAN_PHASE_WORDS];
    uint8_t  boot[KM_REPORT_BOOT_LEN];
    uint8_t  nkro[KM_REPORT_NKRO_LEN];
    uint8_t  pkt[KM_HID_REPORT_LEN];
    km_hid_req_t req;
    uint32_t acc = 0;

    /* Scan core: feed a constant ADC level and read the 16-point sums back. */
    km_scan_init(&g_scan);
    for (unsigned k = 0; k < KM_SCAN_PHASE_WORDS; k++) {
        words[k] = (3200u << 16) | 3200u;
    }
    for (unsigned cycle = 0; cycle < KM_SCAN_DEPTH; cycle++) {
        for (unsigned phase = 0; phase < KM_SCAN_PHASES; phase++) {
            km_scan_push_phase(&g_scan, phase, words);
        }
        km_scan_push_injected(&g_scan, (3200u << 16) | 3200u, (3200u << 16) | 3200u);
    }
    acc += km_scan_sums(&g_scan)[0];

    /* Key engine: calibrate on a released keyboard, then press and release every slot once. */
    km_engine_init(&g_engine);
    km_engine_calibrate_begin(&g_engine);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        counts[s] = 3200u;
    }
    for (unsigned t = 0; t < KM_ENGINE_CAL_MIN_FRAMES + 4u; t++) {
        km_engine_calibrate_idle(&g_engine, counts);
    }
    acc += (uint32_t)km_engine_calibrate_finish(&g_engine);

    for (uint32_t t = 0; t < 2000u; t++) {
        /* A synthetic press on one slot: the reading dips by ~1200 counts and comes back. */
        const unsigned s = (t / 100u) % KM_ENGINE_SLOTS;
        const uint16_t dip = (uint16_t)((t % 100u) < 50u ? (t % 100u) * 24u : (100u - (t % 100u)) * 24u);
        for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
            counts[i] = 3200u;
        }
        counts[s] = (uint16_t)(3200u - dip);
        km_engine_update(&g_engine, counts);

        unsigned n = 0;
        const uint8_t *down = km_engine_down_slots(&g_engine, &n);
        km_report_build_boot(down, n, boot);
        km_report_build_nkro(down, n, nkro);
        acc += boot[2] + nkro[4] + km_engine_depth(&g_engine, s);
    }

    /* HID: build a stock-protocol read request for the trigger table, then parse it back. */
    if (km_hid_build_read(pkt, KM_CMD_GET_TRIGGER, 1024, 56) == KM_HID_OK &&
        km_hid_parse(pkt, &req) == KM_HID_OK) {
        acc += req.offset;
    }

    km_smoke_sink = acc;
    return 0;
}
