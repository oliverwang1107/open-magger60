#include "km/km_vendor.h"

#include <string.h>

#include "km/km_scope.h"

static km_config_t *s_config;
static km_stream_t *s_stream;

void km_vendor_set_config(km_config_t *cfg)
{
    s_config = cfg;
}

void km_vendor_set_stream(km_stream_t *stream)
{
    s_stream = stream;
}

bool km_vendor_is_enter_boot_request(const uint8_t in[KM_HID_REPORT_LEN])
{
    uint8_t want[KM_HID_REPORT_LEN];
    return km_hid_build_enter_boot(want) == KM_HID_OK && memcmp(in, want, KM_HID_REPORT_LEN) == 0;
}

bool km_vendor_handle(const km_vendor_info_t *info, const uint8_t in[KM_HID_REPORT_LEN],
                      uint8_t out[KM_HID_REPORT_LEN])
{
    km_hid_req_t req;
    const km_hid_status_t st = km_hid_parse(in, &req);

    if (st == KM_HID_ERR_CHECKSUM) {
        /* a bad checksum is answered with 0xAB and "cscr", as the stock firmware does (CONFIG_PROTOCOL.md section 1) */
        return km_hid_build_response(out, KM_HID_RESP_CSUM, in[1], 0, (const uint8_t *)"cscr", 4) == KM_HID_OK;
    }
    if (st != KM_HID_OK || req.header != KM_HID_HDR_NORMAL) {
        return false;                      /* garbage, wrong header, or a raw '_' packet: not answered, not executed */
    }

    if (req.cmd == KM_CMD_GET_INFO) {
        uint8_t data[KM_HID_MAX_DATA];
        size_t n = 0;
        data[n++] = (uint8_t)info->bcd_version;
        data[n++] = (uint8_t)(info->bcd_version >> 8);
        for (const char *p = info->build; *p != '\0' && n < sizeof data; p++) {
            data[n++] = (uint8_t)*p;   /* no terminator is counted: len = 2 + strlen, zero padding follows */
        }
        return km_hid_build_response(out, KM_HID_RESP_OK, KM_CMD_GET_INFO, 0, data, (uint8_t)n) == KM_HID_OK;
    }

    if (req.cmd == KM_CMD_SCOPE) {
        return km_scope_handle(&req, out);   /* M3 scope channel: read-only, see km_scope.h */
    }

    if (req.cmd == KM_CMD_CONFIG && s_config != NULL) {
        return km_config_handle(s_config, &req, out);   /* M5 configuration protocol: see km_config.h */
    }

    if (req.cmd == KM_CMD_STREAM && s_stream != NULL) {
        return km_stream_handle(s_stream, &req, out);   /* P7a light frame stream: see km_stream.h */
    }

    /* everything else is "unknown command": no side effects */
    return km_hid_build_response(out, KM_HID_RESP_OK, req.cmd, req.offset, (const uint8_t *)"unknw", 5) == KM_HID_OK;
}
