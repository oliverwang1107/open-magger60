/* Vendor-class HID channel (D3): the reply to cmd 3 (version + build string) and the "unknown command" behaviour. */
#include "km/km_scope.h"
#include "km/km_vendor.h"
#include "km_test.h"

/* The expected reply, worked out by hand from the frame layout in km_hid.h for version 0x0102, build "KMFW-TEST":
 *   aa 03 00 | checksum | len | offset 00 00 00 | 02 01 | 'K' 'M' 'F' 'W' '-' 'T' 'E' 'S' 'T' | zeros
 *   len = 2 + 9 = 11 = 0x0b, checksum = 11 + (2 + 1) + (75+77+70+87+45+84+69+83+84 = 674) = 688 -> 0xb0 */
static const char k_expected_reply[] = "aa0300b00b000000" "0201" "4b4d46572d54455354";

static const km_vendor_info_t k_test_info = { 0x0102, "KMFW-TEST" };

void test_vendor(void)
{
    uint8_t want[KM_HID_REPORT_LEN] = { 0 };
    CHECK_EQ(km_hex(k_expected_reply, want, sizeof want), 19);

    /* the reply does not depend on the requested length */
    static const struct { uint8_t len; } reqs[3] = { { 31 }, { 0 }, { 56 } };
    for (unsigned i = 0; i < 3; i++) {
        uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
        CHECK(km_hid_build_read(in, KM_CMD_GET_INFO, 0, reqs[i].len) == KM_HID_OK);
        memset(out, 0x77, sizeof out);
        CHECK(km_vendor_handle(&k_test_info, in, out));
        CHECK(memcmp(out, want, sizeof want) == 0);
    }

    /* an XOR-masked request gets the same plain reply */
    {
        uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
        CHECK(km_hid_build(in, KM_HID_HDR_NORMAL, KM_CMD_GET_INFO, 0x5A, 0, false, NULL, 0) == KM_HID_OK);
        CHECK(km_vendor_handle(&k_test_info, in, out));
        CHECK(memcmp(out, want, sizeof want) == 0);
    }

    /* a different build string keeps the layout; very long strings are cut, never overflow */
    {
        uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
        const km_vendor_info_t shortinfo = { 0x0200, "x" };
        CHECK(km_hid_build_read(in, KM_CMD_GET_INFO, 0, 0) == KM_HID_OK);
        CHECK(km_vendor_handle(&shortinfo, in, out));
        CHECK(out[0] == 0xAA && out[1] == 3 && out[4] == 3 && out[8] == 0x00 && out[9] == 0x02 && out[10] == 'x' && out[11] == 0);
        CHECK_EQ(out[3], (3 + 0x00 + 0x02 + 'x') & 0xFF);

        char longstr[200];
        memset(longstr, 'A', sizeof longstr - 1);
        longstr[sizeof longstr - 1] = '\0';
        const km_vendor_info_t longinfo = { 0x0104, longstr };
        CHECK(km_vendor_handle(&longinfo, in, out));
        CHECK(out[4] == KM_HID_MAX_DATA && out[8 + KM_HID_MAX_DATA - 1] == 'A');
    }

    /* bad checksum: 0xAB "cscr", the answer the stock protocol uses */
    {
        uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
        CHECK(km_hid_build_read(in, KM_CMD_GET_INFO, 0, 31) == KM_HID_OK);
        in[3] ^= 0x01;
        CHECK(km_vendor_handle(&k_test_info, in, out));
        CHECK(out[0] == 0xAB && out[1] == 3 && memcmp(out + 8, "cscr", 4) == 0 && out[4] == 4);
    }

    /* not answered and not executed: wrong header, raw '_' packets (the enter-bootloader packet among them) */
    {
        uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
        CHECK(km_hid_build_enter_boot(in) == KM_HID_OK);
        CHECK(!km_vendor_handle(&k_test_info, in, out));
        memset(in, 0, sizeof in);
        CHECK(!km_vendor_handle(&k_test_info, in, out));
        memset(in, 0xFF, sizeof in);
        CHECK(!km_vendor_handle(&k_test_info, in, out));
        CHECK(km_hid_build_read(in, KM_CMD_GET_INFO, 0, 0) == KM_HID_OK);
        in[0] = 0x80;                          /* a bootloader-style packet */
        CHECK(!km_vendor_handle(&k_test_info, in, out));
        CHECK(km_hid_build(in, KM_HID_HDR_RAW, KM_CMD_GET_INFO, 0, 0, false, NULL, 0) == KM_HID_OK);
        CHECK(!km_vendor_handle(&k_test_info, in, out));
    }

    /* every other command, with every offset/length shape, is "unknown" and returns only "unknw" */
    for (unsigned cmd = 0; cmd < 256; cmd++) {
        if (cmd == KM_CMD_GET_INFO || cmd == KM_CMD_SCOPE) {   /* the scope command is tested in test_scope.c */
            continue;
        }
        uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
        uint8_t data[KM_HID_MAX_DATA];
        memset(data, 0xA5, sizeof data);
        CHECK(km_hid_build(in, KM_HID_HDR_NORMAL, (uint8_t)cmd, 0, 0x1234, false, data, 56) == KM_HID_OK);
        CHECK(km_vendor_handle(&k_test_info, in, out));
        CHECK(out[0] == 0xAA && out[1] == cmd && out[4] == 5 && memcmp(out + 8, "unknw", 5) == 0);
        for (unsigned i = 13; i < KM_HID_REPORT_LEN; i++) {
            CHECK(out[i] == 0);                 /* nothing from the request leaks into the reply */
        }
    }
}
