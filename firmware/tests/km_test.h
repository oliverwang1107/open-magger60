/* km_test.h - tiny dependency-free test helpers for the host unit tests. */
#ifndef KM_TEST_H
#define KM_TEST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int km_test_checks;
extern int km_test_failures;

#ifdef KM_TRACE /* -DKM_TRACE prints each check's line before it runs (finds crashes without a debugger) */
#define KM_TRACE_LINE() printf("  @%s:%d\n", __FILE__, __LINE__)
#else
#define KM_TRACE_LINE() ((void)0)
#endif

#define CHECK(cond)                                                                    \
    do {                                                                               \
        KM_TRACE_LINE();                                                               \
        km_test_checks++;                                                              \
        if (!(cond)) {                                                                 \
            km_test_failures++;                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        const long long km_a_ = (long long)(a);                                        \
        const long long km_b_ = (long long)(b);                                        \
        km_test_checks++;                                                              \
        if (km_a_ != km_b_) {                                                          \
            km_test_failures++;                                                        \
            printf("  FAIL %s:%d: %s == %s  (got %lld, want %lld)\n", __FILE__,        \
                   __LINE__, #a, #b, km_a_, km_b_);                                    \
        }                                                                              \
    } while (0)

/* Decode a hex string into out; returns the byte count. */
size_t km_hex(const char *hex, uint8_t *out, size_t cap);

typedef void (*km_suite_fn)(void);
void test_hid(void);
void test_flash_map(void);
void test_usb(void);
void test_vendor(void);
void test_flash_guard(void);
void test_flash_boot(void);
void test_scan(void);
void test_scope(void);
void test_sense(void);
void test_depth_model(void);
void test_engine(void);
void test_engine_filter(void);
void test_layers(void);
void test_layers_ext(void);
void test_layers_m6(void);
void test_settings(void);
void test_store(void);
void test_store_flash(void);
void test_calstore(void);
void test_config(void);
void test_health(void);
void test_report_sync(void);
void test_led(void);
void test_led_hw(void);
void test_ledstore(void);
void test_ledprof(void);
void test_lightscene(void);
void test_capture(void);
void test_filter_traces(void);
void test_drift(void);
void test_socd(void);
void test_dks(void);
void test_pad(void);
void test_latency_probe(void);
void test_usbfast(void);
void test_stream(void);
void test_anim(void);
void test_pipeline_m6(void);

#endif /* KM_TEST_H */
