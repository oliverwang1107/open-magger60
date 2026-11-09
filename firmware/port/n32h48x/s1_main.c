/*
 * S1 image (M3_PLAN.md section 3, step S1): D4 plus polled single ADC conversions, no DMA, no timer, no interrupts.
 * The bootloader's own key-read setup (the one that runs on the real keyboard at every power-up): ADC1 channel 3 with mux
 * phase 1 is ESC, ADC2 channel 17 with mux phase 0 is the left CTRL. The bootloader averages 128 readings and treats
 * "< 0x999" as pressed (BOOTLOADER_ANALYSIS.md section 3).
 *
 * Every ~10 ms the main loop publishes (km_scope_publish tag 0x0001, 9 values):
 *   [0] ESC single reading      [1] left CTRL single reading     (0xFFFF = the conversion never finished)
 *   [2] mux phase used for ESC (1)   [3] mux phase used for CTRL (0)
 *   [4] ESC mean of 16 readings [5] CTRL mean of 16 readings
 *   [6] sample counter (low 16 bits)  [7] conversions that timed out so far   [8] sense_poll_init error bits
 * No key reports, no LEDs, no flash write other than D4's enter-bootloader flag.
 */
#include <stdbool.h>

#include "flash_boot.h"
#include "km/km_scope.h"
#include "km/km_vendor.h"
#include "n32h48x_regs.h"
#include "sense_hw.h"
#include "usb_hs.h"

#define PERIOD_CYCLES  (10u * 1000u * 240u)      /* 10 ms at the assumed 240 MHz */
#define SETTLE_US      200u                      /* conservative wait after a mux change; our scan driver waits ~1 us */
#define AVG_N          16u

static const km_vendor_info_t k_info = { 0x0104, __DATE__ "," __TIME__ "KMFW-S1" };

static volatile bool g_enter_boot;

static bool vendor(const uint8_t in[64], uint8_t out[64])
{
    if (km_vendor_is_enter_boot_request(in)) {
        g_enter_boot = true;               /* acted on in the main loop, after the USB transfer is over */
        return false;
    }
    return km_vendor_handle(&k_info, in, out);
}

static uint32_t g_timeouts;

static uint16_t single(unsigned adc, unsigned ch)
{
    const int32_t v = sense_poll_convert(adc, ch);
    if (v < 0) {
        g_timeouts++;
        return 0xFFFFu;
    }
    return (uint16_t)v;
}

static uint16_t mean16(unsigned adc, unsigned ch)
{
    uint32_t sum = 0;
    for (unsigned i = 0; i < AVG_N; i++) {
        const uint16_t v = single(adc, ch);
        sum += (v == 0xFFFFu) ? 0u : v;
    }
    return (uint16_t)(sum / AVG_N);
}

int main(void)
{
    km_usbhs_set_vendor_handler(vendor);
    km_usbhs_init();
    const uint32_t init_err = sense_poll_init(0x3u);      /* ADC1 + ADC2 only, like the bootloader */
    uint16_t counter = 0;
    uint32_t next = sense_cycles();
    for (;;) {
        km_iwdg_feed();
        km_usbhs_poll();
        if (g_enter_boot) {
            km_usbhs_disconnect();
            km_flash_request_bootloader();
        }
        if ((int32_t)(sense_cycles() - next) >= 0) {
            next += PERIOD_CYCLES;
            uint16_t v[9];
            sense_mux_set(1);                              /* ESC */
            sense_delay_us(SETTLE_US);
            v[0] = single(1, 3);
            v[4] = mean16(1, 3);
            sense_mux_set(0);                              /* left CTRL */
            sense_delay_us(SETTLE_US);
            v[1] = single(2, 17);
            v[5] = mean16(2, 17);
            v[2] = 1;
            v[3] = 0;
            v[6] = counter++;
            v[7] = (uint16_t)g_timeouts;
            v[8] = (uint16_t)init_err;
            km_scope_publish(0x0001u, v, 9);
        }
    }
}
