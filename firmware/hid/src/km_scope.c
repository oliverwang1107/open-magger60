#include "km/km_scope.h"

static uint16_t s_values[KM_SCOPE_MAX_VALUES];
static uint16_t s_tag;
static uint16_t s_seq;
static uint16_t s_count;
static const km_scope_capture_t *s_cap;
static void *s_cap_ctx;

void km_scope_set_capture(const km_scope_capture_t *ops, void *ctx)
{
    s_cap = ops;
    s_cap_ctx = ctx;
}

/* tag/seq/count header, then up to `n` of the `have` values from `vals`; the reply to the capture ops */
static bool cap_reply(uint8_t out[KM_HID_REPORT_LEN], unsigned start, unsigned want_len, unsigned tag, unsigned seq,
                      unsigned count, const uint16_t *vals, unsigned have)
{
    uint8_t data[KM_HID_MAX_DATA];
    unsigned n = want_len >= KM_SCOPE_HEADER_BYTES ? (unsigned)(want_len - KM_SCOPE_HEADER_BYTES) / 2u : 0u;
    if (n > KM_SCOPE_PAGE_VALUES) {
        n = KM_SCOPE_PAGE_VALUES;
    }
    if (n > have) {
        n = have;
    }
    data[0] = (uint8_t)tag;
    data[1] = (uint8_t)(tag >> 8);
    data[2] = (uint8_t)seq;
    data[3] = (uint8_t)(seq >> 8);
    data[4] = (uint8_t)count;
    data[5] = (uint8_t)(count >> 8);
    for (unsigned i = 0; i < n; i++) {
        data[KM_SCOPE_HEADER_BYTES + 2u * i] = (uint8_t)vals[i];
        data[KM_SCOPE_HEADER_BYTES + 2u * i + 1u] = (uint8_t)(vals[i] >> 8);
    }
    return km_hid_build_response(out, KM_HID_RESP_OK, KM_CMD_SCOPE, start,
                                 data, (uint8_t)(KM_SCOPE_HEADER_BYTES + 2u * n)) == KM_HID_OK;
}

/* want_tag in 0xC000..0xCFFF and a capture registered. Returns -1 when the request is not a capture op (the caller goes on as before). */
static int cap_handle(const km_hid_req_t *req, uint8_t out[KM_HID_REPORT_LEN])
{
    const unsigned start = req->offset & 0xFFu;
    const unsigned tag = (req->offset >> 8) & 0xFFFFu;
    const unsigned op = (tag >> 8) & 0xFu;
    uint16_t buf[KM_SCOPE_PAGE_VALUES];
    unsigned nw;

    switch (op) {
    case 0x0:
        break;
    case 0x1:
        if ((tag & 0xFFu) < 4u) {
            (void)s_cap->select(s_cap_ctx, tag & 0xFFu, start);     /* a refusal shows in the status block that follows */
        }
        break;
    case 0x2:
        (void)s_cap->control(s_cap_ctx, start);
        break;
    case 0x3: {
        const unsigned first = ((((tag & 0xFFu) << 8) | start)) * KM_SCOPE_PAGE_VALUES;
        const unsigned total = s_cap->value_count(s_cap_ctx);
        const unsigned got = first < total ? s_cap->read(s_cap_ctx, first, KM_SCOPE_PAGE_VALUES, buf) : 0u;
        return cap_reply(out, start, req->len, tag, s_cap->run(s_cap_ctx), total, buf, got) ? 1 : 0;
    }
    default:
        return -1;                                  /* unknown op: answered like a tag that is not published */
    }
    nw = s_cap->status(s_cap_ctx, buf);
    return cap_reply(out, start, req->len, tag, s_cap->run(s_cap_ctx), nw, buf, nw) ? 1 : 0;
}

void km_scope_reset(void)
{
    s_tag = 0;
    s_seq = 0;
    s_count = 0;
    for (unsigned i = 0; i < KM_SCOPE_MAX_VALUES; i++) {
        s_values[i] = 0;
    }
}

void km_scope_publish(uint16_t tag, const uint16_t *values, uint16_t count)
{
    if (tag == 0 || (count != 0 && values == NULL)) {
        return;
    }
    if (count > KM_SCOPE_MAX_VALUES) {
        count = KM_SCOPE_MAX_VALUES;
    }
    for (unsigned i = 0; i < count; i++) {
        s_values[i] = values[i];
    }
    s_tag = tag;
    s_count = count;
    s_seq = (uint16_t)(s_seq + 1u);
    if (s_seq == 0) {
        s_seq = 1;                          /* 0 means "nothing published" */
    }
}

bool km_scope_handle(const km_hid_req_t *req, uint8_t out[KM_HID_REPORT_LEN])
{
    const unsigned start = req->offset & 0xFFu;
    const unsigned want_tag = (req->offset >> 8) & 0xFFFFu;
    uint8_t data[KM_HID_MAX_DATA];
    unsigned n = 0;

    if (s_cap != NULL && want_tag >= KM_SCOPE_CAP_FIRST && want_tag <= 0xCFFFu) {
        const int r = cap_handle(req, out);
        if (r >= 0) {
            return r != 0;
        }
    }
    if (want_tag == 0 || want_tag == s_tag) {
        n = req->len >= KM_SCOPE_HEADER_BYTES ? (unsigned)(req->len - KM_SCOPE_HEADER_BYTES) / 2u : 0u;
        if (n > KM_SCOPE_PAGE_VALUES) {
            n = KM_SCOPE_PAGE_VALUES;
        }
        if (start >= s_count) {
            n = 0;
        } else if (n > s_count - start) {
            n = s_count - start;
        }
    }

    data[0] = (uint8_t)s_tag;
    data[1] = (uint8_t)(s_tag >> 8);
    data[2] = (uint8_t)s_seq;
    data[3] = (uint8_t)(s_seq >> 8);
    data[4] = (uint8_t)s_count;
    data[5] = (uint8_t)(s_count >> 8);
    for (unsigned i = 0; i < n; i++) {
        const uint16_t v = s_values[start + i];
        data[KM_SCOPE_HEADER_BYTES + 2u * i] = (uint8_t)v;
        data[KM_SCOPE_HEADER_BYTES + 2u * i + 1u] = (uint8_t)(v >> 8);
    }
    return km_hid_build_response(out, KM_HID_RESP_OK, KM_CMD_SCOPE, start,
                                 data, (uint8_t)(KM_SCOPE_HEADER_BYTES + 2u * n)) == KM_HID_OK;
}
