/*
 * km_report.c - boot, NKRO and consumer report builders (M4 task T-B, M5 task T-C). Layout: see km_report.h.
 * The slot-based and the usage-based builders share one body: `items` are slots (looked up on layer 0) or usages.
 */
#include "km/km_report.h"

#include <string.h>

#include "km/km_keymap.h"

/* The keyboard usage of item i; usages given directly must be real keys (0x04..0xE7), anything else counts as none. */
static uint8_t item_usage(const uint8_t *items, unsigned i, bool is_slot)
{
    if (is_slot) {
        return km_keymap_usage(0, items[i]);
    }
    return (items[i] >= 4u && items[i] <= KM_USAGE_MOD_LAST) ? items[i] : (uint8_t)KM_USAGE_NONE;
}

static void build_boot(const uint8_t *items, unsigned n, bool is_slot, uint8_t report[KM_REPORT_BOOT_LEN])
{
    uint8_t keys[KM_REPORT_BOOT_KEYS] = { 0 };
    unsigned count = 0;
    uint8_t mods = 0;
    bool overflow = false;

    for (unsigned i = 0; i < n; i++) {
        const uint8_t usage = item_usage(items, i, is_slot);
        if (usage == KM_USAGE_NONE) {
            continue;
        }
        if (km_usage_is_modifier(usage)) {
            mods = (uint8_t)(mods | (1u << (usage - KM_USAGE_MOD_FIRST)));
            continue;
        }
        bool dup = false;
        for (unsigned j = 0; j < count; j++) {
            dup = dup || keys[j] == usage;
        }
        if (dup) {
            continue;
        }
        if (count == KM_REPORT_BOOT_KEYS) {
            overflow = true;  /* keep scanning: later modifiers still count */
        } else {
            keys[count++] = usage;
        }
    }

    report[0] = mods;
    report[1] = 0;
    for (unsigned j = 0; j < KM_REPORT_BOOT_KEYS; j++) {
        report[2 + j] = overflow ? (uint8_t)KM_REPORT_ROLLOVER : keys[j];
    }
}

static void build_nkro(const uint8_t *items, unsigned n, bool is_slot, uint8_t report[KM_REPORT_NKRO_LEN])
{
    memset(report, 0, KM_REPORT_NKRO_LEN);
    for (unsigned i = 0; i < n; i++) {
        const uint8_t usage = item_usage(items, i, is_slot);
        if (usage >= 4u && usage < KM_REPORT_NKRO_BITS) {
            report[usage >> 3] = (uint8_t)(report[usage >> 3] | (1u << (usage & 7u)));
        }
    }
}

void km_report_build_boot(const uint8_t *down_slots, unsigned n, uint8_t report[KM_REPORT_BOOT_LEN])
{
    build_boot(down_slots, n, true, report);
}

void km_report_build_nkro(const uint8_t *down_slots, unsigned n, uint8_t report[KM_REPORT_NKRO_LEN])
{
    build_nkro(down_slots, n, true, report);
}

void km_report_build_boot_usages(const uint8_t *usages, unsigned n, uint8_t report[KM_REPORT_BOOT_LEN])
{
    build_boot(usages, n, false, report);
}

void km_report_build_nkro_usages(const uint8_t *usages, unsigned n, uint8_t report[KM_REPORT_NKRO_LEN])
{
    build_nkro(usages, n, false, report);
}

void km_report_build_consumer(uint16_t usage, uint8_t report[KM_REPORT_CONSUMER_LEN])
{
    if (usage > KM_REPORT_CONSUMER_MAX) {
        usage = 0;  /* outside what the report descriptor declares: send "released" rather than a wrong key */
    }
    report[0] = KM_REPORT_CONSUMER_ID;
    report[1] = (uint8_t)(usage & 0xFFu);
    report[2] = (uint8_t)(usage >> 8);
}
