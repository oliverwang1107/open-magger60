#include <stdbool.h>

#include "km_test.h"

int km_test_checks;
int km_test_failures;

size_t km_hex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (hex[0] != '\0' && hex[1] != '\0' && n < cap) {
        unsigned v;
        char tmp[3] = { hex[0], hex[1], 0 };
        sscanf(tmp, "%2x", &v);
        out[n++] = (uint8_t)v;
        hex += 2;
    }
    return n;
}

static int g_argc;
static char **g_argv;

/* With arguments only the named suites run (the mutation tools use this); without, all of them. */
static bool wanted(const char *name)
{
    if (g_argc <= 1) {
        return true;
    }
    for (int i = 1; i < g_argc; i++) {
        if (strcmp(g_argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void run(const char *name, km_suite_fn fn)
{
    if (!wanted(name)) {
        return;
    }
    const int before = km_test_failures;
    fn();
    printf("%-12s %s\n", name, (km_test_failures == before) ? "ok" : "FAILED");
}

int main(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;
    setvbuf(stdout, NULL, _IONBF, 0); /* keep output if a test crashes */
    run("hid", test_hid);
    run("flash_map", test_flash_map);
    run("usb", test_usb);
    run("vendor", test_vendor);
    run("flash_guard", test_flash_guard);
    run("flash_boot", test_flash_boot);
    run("scan", test_scan);
    run("scope", test_scope);
    run("sense", test_sense);
    run("depth_model", test_depth_model);
    run("engine", test_engine);
    run("engine_filter", test_engine_filter);
    run("layers", test_layers);
    run("layers_ext", test_layers_ext);
    run("layers_m6", test_layers_m6);
    run("settings", test_settings);
    run("store", test_store);
    run("store_flash", test_store_flash);
    run("calstore", test_calstore);
    run("config", test_config);
    run("report_sync", test_report_sync);
    run("health", test_health);
    run("led", test_led);
    run("led_hw", test_led_hw);
    run("ledstore", test_ledstore);
    run("ledprof", test_ledprof);
    run("lightscene", test_lightscene);
    run("capture", test_capture);
    run("filter_traces", test_filter_traces);
    run("drift", test_drift);
    run("socd", test_socd);
    run("dks", test_dks);
    run("pad", test_pad);
    run("stream", test_stream);
    run("anim", test_anim);
    run("pipeline_m6", test_pipeline_m6);
    run("latency", test_latency_probe);
    run("usbfast", test_usbfast);
    printf("%d checks, %d failures\n", km_test_checks, km_test_failures);
    return km_test_failures == 0 ? 0 : 1;
}
