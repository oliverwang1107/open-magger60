/* The one flash-write rule (km_flash_guard.h) and the enter-bootloader packet matcher. */
#include "km/km_flash_guard.h"
#include "km/km_vendor.h"
#include "km_test.h"

void test_flash_guard(void)
{
    /* allowed: anything fully inside 0x0802C000-0x0802DFFF */
    CHECK(km_flash_write_allowed(0x0802C000u, 1));
    CHECK(km_flash_write_allowed(0x0802C000u, 8));
    CHECK(km_flash_write_allowed(0x0802C000u, 0x2000));
    CHECK(km_flash_write_allowed(0x0802DFF8u, 8));
    CHECK(km_flash_write_allowed(0x0802DFFFu, 1));
    CHECK(km_flash_write_allowed(0x0802C000u, 0));

    /* the bootloader, every byte of it and its edges */
    CHECK(!km_flash_write_allowed(0x08000000u, 1));
    CHECK(!km_flash_write_allowed(0x08000000u, 0x8000));
    CHECK(!km_flash_write_allowed(0x08005FFFu, 1));
    CHECK(!km_flash_write_allowed(0x08006000u, 8));      /* the bootloader's own status sector: not ours either */
    CHECK(!km_flash_write_allowed(0x08007FF8u, 8));
    CHECK(!km_flash_write_allowed(0x08007FFFu, 1));
    /* the app, the ID block, the neighbouring settings sectors */
    CHECK(!km_flash_write_allowed(0x08008000u, 1));
    CHECK(!km_flash_write_allowed(0x08027FFFu, 1));
    CHECK(!km_flash_write_allowed(0x08028000u, 14));     /* app-ID: erasing it strands the keyboard in update mode */
    CHECK(!km_flash_write_allowed(0x0802A000u, 8));
    CHECK(!km_flash_write_allowed(0x0802BFFFu, 1));
    CHECK(!km_flash_write_allowed(0x0802E000u, 8));      /* calibration */
    CHECK(!km_flash_write_allowed(0x0803E000u, 8));
    CHECK(!km_flash_write_allowed(0x08040000u, 1));
    CHECK(!km_flash_write_allowed(0x08080000u, 1));
    CHECK(!km_flash_write_allowed(0x20000000u, 1));      /* not flash at all */
    CHECK(!km_flash_write_allowed(0x40022000u, 4));
    /* straddling the edges of the profile sector */
    CHECK(!km_flash_write_allowed(0x0802BFF8u, 16));
    CHECK(!km_flash_write_allowed(0x0802DFF8u, 16));
    CHECK(!km_flash_write_allowed(0x0802C000u, 0x2001));
    CHECK(!km_flash_write_allowed(0x0802C001u, 0x2000));
    /* wrap-around and huge lengths */
    CHECK(!km_flash_write_allowed(0xFFFFFFF8u, 16));
    CHECK(!km_flash_write_allowed(0x0802C000u, 0xFFFFFFFFu));
    CHECK(!km_flash_write_allowed(0x0802DFFFu, 0xFFFFFFFFu));
    CHECK(!km_flash_write_allowed(0u, 0u + 0x0802C000u));

    /* exhaustive over the range around the sector: exactly the in-sector bytes are allowed */
    unsigned allowed = 0;
    for (uint32_t a = 0x0802B000u; a < 0x0802F000u; a++) {
        if (km_flash_write_allowed(a, 1)) {
            allowed++;
            CHECK(a >= 0x0802C000u && a < 0x0802E000u);
        }
    }
    CHECK_EQ(allowed, 0x2000);
    for (uint32_t a = 0x08000000u; a < 0x08008000u; a += 4) {
        CHECK(!km_flash_write_allowed(a, 4));
    }

    /* the flag byte lives where the guard allows writing, and nowhere else */
    CHECK(km_flash_write_allowed(KM_BOOT_FLAG_ADDR, 1));
    CHECK_EQ(KM_FLASH_WRITABLE_BASE, KM_CFG_PROFILE);

    /* ---- M5: the settings store rule (km_flash_store_allowed): exactly six more sectors (the fourth holds the light configuration, the last two the light scene) ---- */
    CHECK_EQ(KM_FLASH_SETTINGS_A, 0x08030000u);
    CHECK_EQ(KM_FLASH_SETTINGS_B, 0x08032000u);
    CHECK_EQ(KM_FLASH_CALIBRATION, 0x08034000u);
    CHECK_EQ(KM_FLASH_LED_CONFIG, 0x08036000u);
    CHECK_EQ(KM_FLASH_SCENE_A, 0x08038000u);
    CHECK_EQ(KM_FLASH_SCENE_B, 0x0803A000u);
    CHECK_EQ(KM_FLASH_STORE_SECTORS, 6u);
    {
        static const uint32_t sect[6] = { 0x08030000u, 0x08032000u, 0x08034000u, 0x08036000u, 0x08038000u, 0x0803A000u };
        for (unsigned i = 0; i < 6; i++) {
            const uint32_t b = sect[i], e = b + 0x2000u;
            CHECK(km_flash_store_allowed(b, 1));
            CHECK(km_flash_store_allowed(b, 8));
            CHECK(km_flash_store_allowed(b, 0));
            CHECK(km_flash_store_allowed(b, 0x2000));
            CHECK(km_flash_store_allowed(e - 1, 1));
            CHECK(km_flash_store_allowed(e - 8, 8));
            /* one byte before / after: forbidden unless that byte is the neighbouring store sector (the six are adjacent) */
            CHECK_EQ(km_flash_store_allowed(b - 1, 1), i > 0);
            CHECK_EQ(km_flash_store_allowed(e, 1), i < 5);
            CHECK(!km_flash_store_allowed(b - 1, 2));         /* straddling the start */
            CHECK(!km_flash_store_allowed(e - 1, 2));         /* straddling the end */
            CHECK(!km_flash_store_allowed(e - 8, 16));
            CHECK(!km_flash_store_allowed(b - 8, 16));
            CHECK(!km_flash_store_allowed(b, 0x2001));        /* longer than a sector */
            CHECK(!km_flash_store_allowed(b + 1, 0x2000));
            CHECK(!km_flash_store_allowed(b, 0xFFFFFFFFu));
            CHECK(!km_flash_store_allowed(e - 1, 0xFFFFFFFFu));
            CHECK(!km_flash_write_allowed(b, 1));             /* the boot-flag rule does not reach the store sectors */
            CHECK(!km_flash_write_allowed(e - 1, 1));
        }
        /* settings A and B are adjacent: a write may not run from one into the next, even though both are allowed */
        CHECK(!km_flash_store_allowed(0x08031FF8u, 16));
        CHECK(!km_flash_store_allowed(0x08033FF8u, 16));
        CHECK(!km_flash_store_allowed(0x08035FF8u, 16));
        CHECK(!km_flash_store_allowed(0x08037FF8u, 16));                 /* the end of the light sector runs into scene A */
        CHECK(!km_flash_store_allowed(0x08039FF8u, 16));                 /* scene A into scene B */
        CHECK(!km_flash_store_allowed(0x0803BFF8u, 16));                 /* the end of scene B runs into the TGL sector */
        CHECK(!km_flash_store_allowed(0x08030000u, 0x4000));
    }
    /* forbidden, whatever the length: bootloader, app, app-ID block, the neighbours, the profile sector, the rest */
    for (uint32_t a = 0x08000000u; a < 0x08008000u; a += 4) {
        CHECK(!km_flash_store_allowed(a, 4));
    }
    CHECK(!km_flash_store_allowed(0x08000000u, 0x8000));
    CHECK(!km_flash_store_allowed(0x08007FFFu, 1));
    CHECK(!km_flash_store_allowed(0x08008000u, 1));                /* first byte of the app */
    CHECK(!km_flash_store_allowed(0x08027FFFu, 1));                /* last byte of the app */
    CHECK(!km_flash_store_allowed(0x08028000u, 14));               /* app-ID block */
    CHECK(!km_flash_store_allowed(0x08028000u, 1));
    CHECK(!km_flash_store_allowed(0x0802A000u, 8));
    CHECK(!km_flash_store_allowed(0x0802BFFFu, 1));
    CHECK(!km_flash_store_allowed(0x0802C000u, 1));                /* profile sector (boot flag): not part of the store rule */
    CHECK(!km_flash_store_allowed(KM_BOOT_FLAG_ADDR, 1));
    CHECK(!km_flash_store_allowed(0x0802E000u, 8));
    CHECK(!km_flash_store_allowed(0x0802FFFFu, 1));
    CHECK(!km_flash_store_allowed(0x0803C000u, 1));                /* the sector after the light scene ones (stock TGL): still reserved */
    CHECK(!km_flash_store_allowed(0x0803C000u, 8));
    CHECK(!km_flash_store_allowed(0x0803E000u, 8));                /* stock app data */
    CHECK(!km_flash_store_allowed(0x08040000u, 1));
    CHECK(!km_flash_store_allowed(0x08080000u, 1));
    CHECK(!km_flash_store_allowed(0x20000000u, 1));                /* RAM */
    CHECK(!km_flash_store_allowed(0x40022000u, 4));                /* the flash controller itself */
    CHECK(!km_flash_store_allowed(0xFFFFFFF8u, 16));               /* wrap-around */
    CHECK(!km_flash_store_allowed(0xFFFFFFFFu, 1));
    CHECK(!km_flash_store_allowed(0u, 1));
    /* every byte address in the whole 512 KB flash: allowed exactly in the six sectors */
    {
        unsigned n = 0;
        for (uint32_t a = 0x08000000u; a < 0x08080000u; a++) {
            const bool want = a >= 0x08030000u && a < 0x0803C000u;
            if (km_flash_store_allowed(a, 1) != want) {
                n++;
            }
            /* the boot-flag rule and the store rule never allow the same byte */
            if (km_flash_store_allowed(a, 1) && km_flash_write_allowed(a, 1)) {
                n++;
            }
        }
        CHECK_EQ(n, 0);
        unsigned allowed = 0;
        for (uint32_t a = 0x08000000u; a < 0x08080000u; a++) {
            allowed += km_flash_store_allowed(a, 1);
        }
        CHECK_EQ(allowed, 6 * 0x2000);
    }

    /* ---- enter-bootloader packet: exactly the stock firmware's, nothing near it ---- */
    uint8_t exact[KM_HID_REPORT_LEN];
    CHECK(km_hid_build_enter_boot(exact) == KM_HID_OK);
    CHECK(exact[0] == 0x5F && exact[1] == 0x06 && exact[3] == 0x52 && exact[4] == 0x01 && exact[8] == 0x51);
    CHECK(km_vendor_is_enter_boot_request(exact));
    for (unsigned i = 0; i < KM_HID_REPORT_LEN; i++) {            /* any single changed byte breaks it */
        uint8_t p[KM_HID_REPORT_LEN];
        memcpy(p, exact, sizeof p);
        p[i] ^= 0x01;
        CHECK(!km_vendor_is_enter_boot_request(p));
        p[i] = (uint8_t)(exact[i] + 1);
        CHECK(!km_vendor_is_enter_boot_request(p));
    }
    {
        uint8_t u[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
        const km_vendor_info_t info = { 0x0104, "x" };
        /* the same command as a normal 'U' packet is not an enter-boot request and is "unknw" */
        const uint8_t one = 0x51;
        CHECK(km_hid_build(u, KM_HID_HDR_NORMAL, KM_CMD_SET_PROFILE, 0, 0, false, &one, 1) == KM_HID_OK);
        CHECK(!km_vendor_is_enter_boot_request(u));
        CHECK(km_vendor_handle(&info, u, out) && memcmp(out + 8, "unknw", 5) == 0);
        /* the exact packet itself is never answered or executed by the generic handler */
        CHECK(!km_vendor_handle(&info, exact, out));
        uint8_t zero[KM_HID_REPORT_LEN] = { 0 };
        CHECK(!km_vendor_is_enter_boot_request(zero));
    }
}
