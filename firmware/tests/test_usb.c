/*
 * USB descriptors, control-request policy and the key-report IN endpoints of the open firmware.
 *
 * The expectations below come from the USB 2.0 / HID 1.11 specifications and from the layout documented in
 * km_usb_reports.h and km_usb_ids.h, never from another device's bytes. Two independent walkers read the descriptors the
 * way a host does: validate_config() (chapter 9 structure) and parse_report() (HID item stream). Every check is also run
 * against deliberately broken copies, so the walkers themselves are known to catch length/address/size mistakes.
 */
#include "km/km_pad.h"
#include "km/km_usb.h"
#include "km_test.h"

/* ================================================================ HID report descriptor parser */

#define MAX_IDS 4u
#define MAX_PAGES 8u

typedef struct {
    unsigned dir;                   /* 0 input, 1 output */
    unsigned id, flags, size, count;
    unsigned page, umin, umax;      /* usage page and usage range (single usage: both equal); 0 for constant padding */
    long     lmin, lmax;
    long     pmin, pmax;            /* physical limits (0, 0 = same as logical) */
    unsigned unit;
    unsigned nusages, usages[6];    /* an explicit usage list (the second hat/axis style); empty for a range or a single usage */
    unsigned bitpos;                /* position of the field inside its report (not compared by check_report; used to decode) */
} item_t;

#define MAX_ITEMS 12u

typedef struct {
    int      err;
    item_t   items[MAX_ITEMS];
    unsigned nitems;                   /* 0 = well formed, else the source line that rejected it */
    unsigned max_depth;
    unsigned top_count;
    struct { unsigned page, usage; } top[4];
    unsigned in_bits[MAX_IDS], out_bits[MAX_IDS], feature_bits[MAX_IDS];   /* index = report ID */
    unsigned pages[MAX_PAGES];
    unsigned npages;
    bool     items_without_id, items_with_id;
} rd_info_t;

#define BAD(info) do { (info)->err = __LINE__; return; } while (0)

static void note_page(rd_info_t *info, unsigned page)
{
    for (unsigned i = 0; i < info->npages; i++) {
        if (info->pages[i] == page) {
            return;
        }
    }
    if (info->npages < MAX_PAGES) {
        info->pages[info->npages++] = page;
    }
}

static bool has_page(const rd_info_t *info, unsigned page)
{
    for (unsigned i = 0; i < info->npages; i++) {
        if (info->pages[i] == page) {
            return true;
        }
    }
    return false;
}

static void parse_report(const uint8_t *d, size_t len, rd_info_t *info)
{
    memset(info, 0, sizeof *info);
    unsigned page = 0, size = 0, count = 0, id = 0, depth = 0;
    long lmin = 0, lmax = 0, pmin = 0, pmax = 0;
    unsigned unit = 0;
    long umin = -1, umax = -1;
    unsigned nusage = 0, first_usage = 0, last_usage = 0, usage_list[6] = { 0 };
    size_t pos = 0;

    while (pos < len) {
        const uint8_t b = d[pos];
        if (b == 0xFE) {
            BAD(info);                                  /* long items are never used */
        }
        unsigned n = b & 3u;
        n = (n == 3u) ? 4u : n;
        const unsigned type = (b >> 2) & 3u;
        const unsigned tag = b >> 4;
        if (pos + 1 + n > len) {
            BAD(info);                                  /* item runs past the end */
        }
        unsigned long uval = 0;
        for (unsigned i = 0; i < n; i++) {
            uval |= (unsigned long)d[pos + 1 + i] << (8 * i);
        }
        long sval = (long)uval;
        if (n != 0 && n < 4 && (uval & (1ul << (8 * n - 1)))) {
            sval = (long)uval - (1l << (8 * n));        /* logical limits are signed */
        }
        pos += 1 + n;

        if (type == 3) {
            BAD(info);                                  /* reserved item type */
        } else if (type == 1) {                         /* global */
            switch (tag) {
            case 0x0: page = (unsigned)uval; note_page(info, page); break;
            case 0x1: lmin = sval; break;
            case 0x2: lmax = sval; break;
            case 0x3: pmin = sval; break;
            case 0x4: pmax = sval; break;
            case 0x6: unit = (unsigned)uval; break;
            case 0x7: size = (unsigned)uval; break;
            case 0x8:
                if (uval == 0 || uval > 255) {
                    BAD(info);
                }
                id = (unsigned)uval;
                break;
            case 0x9: count = (unsigned)uval; break;
            default: BAD(info);                         /* physical limits, units, push/pop: not used here */
            }
        } else if (type == 2) {                         /* local */
            switch (tag) {
            case 0x0:
                if (nusage < 6) {
                    usage_list[nusage] = (unsigned)uval;
                }
                if (nusage++ == 0) {
                    first_usage = (unsigned)uval;
                }
                last_usage = (unsigned)uval;
                break;
            case 0x1: umin = (long)uval; break;
            case 0x2: umax = (long)uval; break;
            default: BAD(info);
            }
        } else {                                        /* main */
            if (tag == 0xA) {
                if (nusage == 0) {
                    BAD(info);                          /* a collection needs a usage */
                }
                if (uval != 1 && depth == 0) {
                    BAD(info);                          /* top level: application collection */
                }
                depth++;
                if (depth > info->max_depth) {
                    info->max_depth = depth;
                }
                if (depth == 1) {
                    if (info->top_count >= 4) {
                        BAD(info);
                    }
                    info->top[info->top_count].page = page;
                    info->top[info->top_count].usage = first_usage;
                    info->top_count++;
                }
            } else if (tag == 0xC) {
                if (depth == 0) {
                    BAD(info);
                }
                depth--;
            } else if (tag == 0x8 || tag == 0x9 || tag == 0xB) {
                if (depth == 0 || size == 0 || count == 0 || id >= MAX_IDS) {
                    BAD(info);
                }
                const bool constant = (uval & 1u) != 0;
                if (!constant && lmin > lmax) {
                    BAD(info);                          /* e.g. a one-byte 0xFF is -1, not 255 */
                }
                if (!constant && (uval & 2u) && umin >= 0 && umax >= 0 && (unsigned long)(umax - umin + 1) != count) {
                    BAD(info);                          /* a usage range must name every variable field */
                }
                if (!constant && (uval & 2u) && nusage == 0 && umin < 0) {
                    BAD(info);
                }
                if (!constant && (uval & 2u) && nusage > 1 && (nusage != count || nusage > 6)) {
                    BAD(info);                          /* a list of usages must name every variable field */
                }
                const unsigned bits = size * count;
                const unsigned pos = tag == 0x8 ? info->in_bits[id] : tag == 0x9 ? info->out_bits[id] : info->feature_bits[id];
                if (tag != 0xB) {
                    if (info->nitems >= MAX_ITEMS) {
                        BAD(info);
                    }
                    item_t *it = &info->items[info->nitems++];
                    it->dir = tag == 0x8 ? 0u : 1u;
                    it->id = id;
                    it->flags = (unsigned)uval;
                    it->size = size;
                    it->count = count;
                    it->bitpos = pos;
                    if (!constant) {
                        it->page = page;
                        it->lmin = lmin;
                        it->lmax = lmax;
                        it->pmin = pmin;
                        it->pmax = pmax;
                        it->unit = unit;
                        if (nusage > 1 && umin < 0) {
                            it->nusages = nusage;
                            for (unsigned k = 0; k < nusage; k++) {
                                it->usages[k] = usage_list[k];
                            }
                        }
                        it->umin = (umin >= 0 && umax >= 0) ? (unsigned)umin : first_usage;
                        it->umax = (umin >= 0 && umax >= 0) ? (unsigned)umax : last_usage;
                    }
                }
                if (tag == 0x8) {
                    info->in_bits[id] += bits;
                } else if (tag == 0x9) {
                    info->out_bits[id] += bits;
                } else {
                    info->feature_bits[id] += bits;
                }
                if (id == 0) {
                    info->items_without_id = true;
                } else {
                    info->items_with_id = true;
                }
            } else {
                BAD(info);
            }
            nusage = 0;                                 /* local items are consumed by every main item */
            umin = umax = -1;
        }
    }
    if (depth != 0) {
        BAD(info);                                      /* unbalanced collections */
    }
    if (info->items_without_id && info->items_with_id) {
        BAD(info);                                      /* once report IDs are used, every report has one */
    }
    for (unsigned i = 0; i < MAX_IDS; i++) {
        if (info->in_bits[i] % 8 || info->out_bits[i] % 8 || info->feature_bits[i] % 8) {
            BAD(info);                                  /* every report is a whole number of bytes */
        }
    }
}

/* Bytes of the report on the wire: the data bytes plus the report ID byte when IDs are in use. */
static unsigned wire_len(unsigned bits, unsigned id)
{
    return bits / 8 + (id != 0 ? 1u : 0u);
}

typedef struct {
    unsigned ntop;
    unsigned top[2][2];             /* page, usage */
    unsigned in_len[MAX_IDS], out_len[MAX_IDS];   /* wire bytes, 0 = no such report */
    unsigned pages[4];
    unsigned npages;
    unsigned nitems;
    item_t   items[8];              /* the fields in order: the item-by-item specification of the descriptor */
} rd_expect_t;

/* Returns 0 when the descriptor parses and matches the documented layout, else the line of the first mismatch. */
static int check_report(const uint8_t *d, size_t len, const rd_expect_t *e)
{
    rd_info_t info;
    parse_report(d, len, &info);
    if (info.err) {
        return info.err;
    }
    if (info.top_count != e->ntop) {
        return __LINE__;
    }
    for (unsigned i = 0; i < e->ntop; i++) {
        if (info.top[i].page != e->top[i][0] || info.top[i].usage != e->top[i][1]) {
            return __LINE__;
        }
    }
    for (unsigned id = 0; id < MAX_IDS; id++) {
        const unsigned in = info.in_bits[id] ? wire_len(info.in_bits[id], id) : 0;
        const unsigned out = info.out_bits[id] ? wire_len(info.out_bits[id], id) : 0;
        if (in != e->in_len[id] || out != e->out_len[id] || info.feature_bits[id] != 0) {
            return __LINE__;
        }
    }
    if (info.nitems != e->nitems) {
        return __LINE__;
    }
    for (unsigned i = 0; i < e->nitems; i++) {
        const item_t *a = &info.items[i], *b = &e->items[i];
        if (a->dir != b->dir || a->id != b->id || a->flags != b->flags || a->size != b->size || a->count != b->count ||
            a->page != b->page || a->umin != b->umin || a->umax != b->umax || a->lmin != b->lmin || a->lmax != b->lmax ||
            a->pmin != b->pmin || a->pmax != b->pmax || a->unit != b->unit || a->nusages != b->nusages) {
            return __LINE__;
        }
        for (unsigned k = 0; k < a->nusages; k++) {
            if (a->usages[k] != b->usages[k]) {
                return __LINE__;
            }
        }
    }
    for (unsigned i = 0; i < e->npages; i++) {
        if (!has_page(&info, e->pages[i])) {
            return __LINE__;
        }
    }
    if (info.npages != e->npages) {
        return __LINE__;                    /* no page beyond the documented ones */
    }
    return 0;
}

/* item: dir (0 in / 1 out), report ID, flags (bit0 constant, bit1 variable), bits per field, field count, usage page,
 * usage min, usage max, logical min, logical max */
/* The trailing fields of item_t (physical limits, unit, usage list) stay 0 for the descriptors that do not use them. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const rd_expect_t k_expect_rd[KM_USB_NUM_IF_MAX] = {
    /* IF0 boot keyboard: 8 B in (modifier bits, reserved byte, six key codes), 1 B LEDs out, no IDs */
    { 1, { { 0x01, 0x06 } }, { KM_USB_BOOT_REPORT_LEN, 0, 0, 0 }, { KM_USB_BOOT_LED_LEN, 0, 0, 0 }, { 0x01, 0x07, 0x08 }, 3,
      5, { { 0, 0, 2, 1, 8, 0x07, 0xE0, 0xE7, 0, 1 },
           { 0, 0, 1, 8, 1, 0, 0, 0, 0, 0 },
           { 1, 0, 2, 1, 5, 0x08, 1, 5, 0, 1 },
           { 1, 0, 1, 3, 1, 0, 0, 0, 0, 0 },
           { 0, 0, 0, 8, 6, 0x07, 0, 0x65, 0, 0x65 } } },
    /* IF1 vendor: 64 B each way, no IDs, data in the vendor page */
    { 1, { { KM_USB_VND_TOP_USAGE_PAGE, KM_USB_VND_TOP_USAGE } }, { KM_USB_VND_REPORT_LEN, 0, 0, 0 },
      { KM_USB_VND_REPORT_LEN, 0, 0, 0 }, { KM_USB_VND_TOP_USAGE_PAGE, KM_USB_VND_DATA_USAGE_PAGE }, 2,
      2, { { 0, 0, 2, 8, 64, KM_USB_VND_DATA_USAGE_PAGE, 1, 1, 0, 255 },
           { 1, 0, 2, 8, 64, KM_USB_VND_DATA_USAGE_PAGE, 2, 2, 0, 255 } } },
    /* IF2: ID 1 keyboard bitmap 29+1 B in, LED 1+1 B out; ID 2 consumer 2+1 B in */
    { 2, { { 0x01, 0x06 }, { 0x0C, 0x01 } },
      { 0, KM_USB_NKRO_REPORT_LEN, KM_USB_CONSUMER_REPORT_LEN, 0 }, { 0, KM_USB_NKRO_LED_LEN, 0, 0 },
      { 0x01, 0x07, 0x08, 0x0C }, 4,
      5, { { 0, 1, 1, 1, 4, 0, 0, 0, 0, 0 },
           { 0, 1, 2, 1, 228, 0x07, 0x04, 0xE7, 0, 1 },
           { 1, 1, 2, 1, 5, 0x08, 1, 5, 0, 1 },
           { 1, 1, 1, 1, 3, 0, 0, 0, 0, 0 },
           { 0, 2, 0, 16, 1, 0x0C, 0, 0x3FF, 0, 0x3FF } } },
    /* IF3 gamepad (PAD variant): 15 B in, no IDs, no outputs. Buttons 1..16, hat (logical 1..8, physical 0..315 degrees, null
     * state), 4 bits padding, X Y Rx Ry signed 16 bit, Z Rz unsigned 16 bit; the physical range of the hat is reset after it. */
    { 1, { { 0x01, 0x05 } }, { KM_USB_PAD_REPORT_LEN, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0x01, 0x09 }, 2,
      5, { { 0, 0, 2, 1, 16, 0x09, 1, 16, 0, 1 },
           { 0, 0, 0x42, 4, 1, 0x01, 0x39, 0x39, 1, 8, 0, 315, 0x14 },
           { 0, 0, 1, 4, 1, 0, 0, 0, 0, 0 },
           { 0, 0, 2, 16, 4, 0x01, 0x30, 0x34, -32767, 32767, 0, 0, 0, 4, { 0x30, 0x31, 0x33, 0x34 } },
           { 0, 0, 2, 16, 2, 0x01, 0x32, 0x35, 0, 32767, 0, 0, 0, 2, { 0x32, 0x35 } } } },
};

#pragma GCC diagnostic pop

/* ================================================================ configuration descriptor walker */

typedef struct { unsigned addr, mps, interval; } ep_expect_t;
typedef struct { unsigned sub, proto, neps; ep_expect_t ep[2]; } if_expect_t;

static const if_expect_t k_expect_if[KM_USB_NUM_IF_MAX] = {
    { 1, 1, 1, { { KM_USB_EP_KBD_IN, 8, KM_USB_BINTERVAL_KBD } } },
    { 0, 0, 2, { { KM_USB_EP_VND_IN, 64, KM_USB_BINTERVAL_VND }, { KM_USB_EP_VND_OUT, 64, KM_USB_BINTERVAL_VND } } },
    { 0, 0, 1, { { KM_USB_EP_NKRO_IN, 64, KM_USB_BINTERVAL_NKRO } } },
    { 0, 0, 1, { { KM_USB_EP_PAD_IN, KM_USB_PAD_EP_MPS, KM_USB_BINTERVAL_PAD } } },
};

/* Returns 0 when the configuration descriptor is well formed and matches the design, else the line that rejected it.
 * rd_len[i] = length of the report descriptor of interface i (must equal wDescriptorLength). */
static int validate_config_n(const uint8_t *c, size_t n, const size_t rd_len[KM_USB_NUM_IF_MAX], unsigned nif)
{
    if (n < 9 || c[0] != 9 || c[1] != 0x02 || (c[2] | (c[3] << 8)) != (int)n) {
        return __LINE__;
    }
    if (c[4] != nif || c[5] != 1 || c[6] != 0 || !(c[7] & 0x80) || c[8] > 250) {
        return __LINE__;
    }
    size_t off = c[0];
    int cur = -1;                       /* interface being walked */
    unsigned eps_seen = 0, hid_seen = 0, total_eps = 0;
    unsigned addrs[8];
    while (off < n) {
        const uint8_t len = c[off];
        if (len < 2 || off + len > n) {
            return __LINE__;
        }
        const uint8_t type = c[off + 1];
        if (type == 0x04) {
            if (cur >= 0 && (eps_seen != k_expect_if[cur].neps || hid_seen != 1)) {
                return __LINE__;
            }
            cur++;
            if (cur >= (int)nif || len != 9 || c[off + 2] != cur || c[off + 3] != 0) {
                return __LINE__;
            }
            const if_expect_t *e = &k_expect_if[cur];
            if (c[off + 4] != e->neps || c[off + 5] != 3 || c[off + 6] != e->sub || c[off + 7] != e->proto ||
                c[off + 8] != 0) {
                return __LINE__;
            }
            eps_seen = 0;
            hid_seen = 0;
        } else if (type == 0x21) {
            if (cur < 0 || hid_seen != 0 || eps_seen != 0 || len != 9) {
                return __LINE__;           /* the HID descriptor sits between its interface and its endpoints */
            }
            hid_seen++;
            if ((c[off + 2] | (c[off + 3] << 8)) != 0x0111 || c[off + 4] != 0 || c[off + 5] != 1 || c[off + 6] != 0x22 ||
                (c[off + 7] | (c[off + 8] << 8)) != (int)rd_len[cur]) {
                return __LINE__;
            }
        } else if (type == 0x05) {
            if (cur < 0 || hid_seen != 1 || len != 7 || eps_seen >= k_expect_if[cur].neps || total_eps >= 8) {
                return __LINE__;
            }
            const ep_expect_t *e = &k_expect_if[cur].ep[eps_seen];
            const unsigned mps = c[off + 4] | (unsigned)(c[off + 5] << 8);
            if (c[off + 2] != e->addr || c[off + 3] != 0x03 || mps != e->mps || c[off + 6] != e->interval) {
                return __LINE__;
            }
            if (c[off + 6] < 1 || c[off + 6] > 16 || mps == 0 || mps > 1024) {
                return __LINE__;           /* high-speed interrupt: interval exponent 1..16, at most 1024 bytes */
            }
            for (unsigned i = 0; i < total_eps; i++) {
                if (addrs[i] == c[off + 2]) {
                    return __LINE__;       /* an endpoint address may appear once */
                }
            }
            addrs[total_eps++] = c[off + 2];
            eps_seen++;
        } else {
            return __LINE__;               /* nothing else is expected */
        }
        off += len;
    }
    if (off != n || cur != (int)nif - 1 || eps_seen != k_expect_if[cur].neps || hid_seen != 1 || total_eps != (nif == 3u ? 4u : 5u)) {
        return __LINE__;
    }
    return 0;
}

/* The plain variant's walker (three interfaces), as before. */
static int validate_config(const uint8_t *c, size_t n, const size_t rd_len[KM_USB_NUM_IF_MAX])
{
    return validate_config_n(c, n, rd_len, KM_USB_NUM_IF);
}

static void report_lengths_for(km_usb_variant_t v, size_t rd_len[KM_USB_NUM_IF_MAX])
{
    for (unsigned i = 0; i < km_usb_variant_num_if(v); i++) {
        km_usb_report_desc_for(v, i, &rd_len[i]);
    }
}

static void report_lengths(size_t rd_len[KM_USB_NUM_IF_MAX])
{
    report_lengths_for(KM_USB_VARIANT_PLAIN, rd_len);
}

static void test_report_descriptors(void)
{
    for (unsigned i = 0; i < KM_USB_NUM_IF; i++) {
        size_t n;
        const uint8_t *d = km_usb_report_desc(i, &n);
        CHECK(d != NULL && n > 0);
        CHECK_EQ(check_report(d, n, &k_expect_rd[i]), 0);
        CHECK(n % 64 != 0);                  /* no zero-length packet needed to end the EP0 answer */
    }
    size_t n;
    CHECK(km_usb_report_desc(KM_USB_NUM_IF, &n) == NULL);

    /* the details the generic walker does not name: the boot keyboard follows HID 1.11 appendix B.1, the vendor channel
     * has exactly one 64-byte input and one 64-byte output field, the NKRO bitmap covers usages 4..0xE7 */
    rd_info_t info;
    const uint8_t *d = km_usb_report_desc(KM_USB_IF_NKRO, &n);
    parse_report(d, n, &info);
    CHECK_EQ(info.in_bits[1], KM_USB_NKRO_BITMAP_BYTES * 8);
    CHECK_EQ(info.in_bits[2], 16);
    CHECK_EQ(info.out_bits[1], 8);
    CHECK_EQ(info.max_depth, 1);
    d = km_usb_report_desc(KM_USB_IF_KBD, &n);
    parse_report(d, n, &info);
    CHECK_EQ(info.in_bits[0], 64);
    CHECK_EQ(info.out_bits[0], 8);
}

static void test_config_descriptor(void)
{
    size_t n, rd[KM_USB_NUM_IF_MAX];
    const uint8_t *c = km_usb_config_desc(&n);
    report_lengths(rd);
    CHECK_EQ(validate_config(c, n, rd), 0);
    CHECK(n % 64 != 0);

    /* the HID descriptor accessor points at the HID descriptor inside the configuration */
    size_t off = c[0];
    unsigned iface = 0;
    while (off < n) {
        if (c[off + 1] == 0x04) {
            iface = c[off + 2];
        } else if (c[off + 1] == 0x21) {
            size_t hl;
            const uint8_t *h = km_usb_hid_desc(iface, &hl);
            CHECK(h == c + off && hl == 9);
        }
        off += c[off];
    }
    CHECK(km_usb_hid_desc(KM_USB_NUM_IF, &n) == NULL);

    /* the endpoint constants the driver uses are the ones in the descriptor */
    CHECK_EQ(KM_USB_EP_KBD_IN & 0xF, 1);
    CHECK_EQ(KM_USB_EP_VND_IN & 0xF, 2);
    CHECK_EQ(KM_USB_EP_VND_OUT, 0x0A);
    CHECK_EQ(KM_USB_EP_NKRO_IN & 0xF, 3);
}

static void test_device_descriptor(void)
{
    size_t n;
    const uint8_t *d = km_usb_device_desc(&n);
    CHECK_EQ(n, 18);
    CHECK(d[0] == 18 && d[1] == 0x01);
    CHECK_EQ(d[2] | (d[3] << 8), 0x0200);                 /* USB 2.0 */
    CHECK(d[4] == 0 && d[5] == 0 && d[6] == 0);           /* class given per interface */
    CHECK_EQ(d[7], 64);                                   /* EP0 packet size of a high-speed device */
    CHECK_EQ(d[8] | (d[9] << 8), KM_USB_VID);
    CHECK_EQ(d[10] | (d[11] << 8), KM_USB_PID);
    CHECK_EQ(d[12] | (d[13] << 8), KM_USB_BCD_DEVICE);
    CHECK(d[14] == 1 && d[15] == 2 && d[16] == 3);        /* manufacturer, product, serial strings */
    CHECK_EQ(d[17], 1);
    /* the host tools and ui/js/device/ids.js carry their own copy of this pair: changing it is a deliberate, coordinated act */
    CHECK_EQ(KM_USB_VID, 0x19F5);
    CHECK_EQ(KM_USB_PID, 0xFB49);
    CHECK_EQ(KM_USB_VND_TOP_USAGE_PAGE, 0x01);
    CHECK_EQ(KM_USB_VND_TOP_USAGE, 0x00);

    const uint8_t *q = km_usb_qualifier_desc(&n);
    CHECK_EQ(n, 10);
    CHECK(q[0] == 10 && q[1] == 0x06);
    CHECK_EQ(q[2] | (q[3] << 8), 0x0200);
    CHECK(q[4] == d[4] && q[5] == d[5] && q[6] == d[6] && q[7] == d[7]);
    CHECK(q[8] == 1 && q[9] == 0);
}

/* ================================================================ strings */

static void test_strings(void)
{
    uint8_t b[KM_USB_STRING_MAX];
    const uint32_t uid[3] = { 0x11223344u, 0xAABBCCDDu, 0x0000009Fu };
    CHECK_EQ(km_usb_string_desc(0, uid, b), 4);
    CHECK(b[0] == 4 && b[1] == 3);
    CHECK_EQ(b[2] | (b[3] << 8), KM_USB_LANGID);

    const char *want[3] = { NULL, KM_USB_STR_MANUFACTURER, KM_USB_STR_PRODUCT };
    for (unsigned i = 1; i <= 2; i++) {
        const size_t n = km_usb_string_desc(i, uid, b);
        CHECK_EQ(n, 2 + 2 * strlen(want[i]));
        CHECK_EQ(b[0], (int)n);
        CHECK_EQ(b[1], 3);
        CHECK(n <= KM_USB_STRING_MAX);
        for (size_t k = 0; k < strlen(want[i]); k++) {
            CHECK(b[2 + 2 * k] == (uint8_t)want[i][k] && b[3 + 2 * k] == 0);   /* ASCII as UTF-16LE */
        }
    }
    /* serial: prefix, then uid word 0, 1, 2 as 8 upper-case hex digits each */
    const size_t n = km_usb_string_desc(3, uid, b);
    const size_t chars = strlen(KM_USB_SERIAL_PREFIX) + 24;
    CHECK_EQ(n, 2 + 2 * chars);
    CHECK_EQ(b[0], (int)n);
    CHECK_EQ(b[1], 3);
    char s[40];
    CHECK(chars < sizeof s);
    for (size_t k = 0; k < chars; k++) {
        s[k] = (char)b[2 + 2 * k];
        CHECK_EQ(b[3 + 2 * k], 0);
        CHECK(s[k] >= 0x21 && s[k] <= 0x7E && s[k] != ',');   /* characters Windows accepts in a serial number */
    }
    s[chars] = '\0';
    CHECK(strcmp(s, KM_USB_SERIAL_PREFIX "11223344AABBCCDD0000009F") == 0);
    CHECK_EQ(km_usb_string_desc(4, uid, b), 0);
    CHECK_EQ(km_usb_string_desc(0xEE, uid, b), 0);

    /* different chips give different serials */
    uint8_t b2[KM_USB_STRING_MAX];
    const uint32_t uid2[3] = { 0x11223344u, 0xAABBCCDDu, 0x000000A0u };
    km_usb_string_desc(3, uid2, b2);
    CHECK(memcmp(b, b2, n) != 0);
}

/* ================================================================ walker self-test: broken copies must be rejected */

typedef struct { const char *name; unsigned iface; size_t at; uint8_t xor_mask; } byte_mut_t;

static void test_walkers_catch_mistakes(void)
{
    size_t cn, rd[KM_USB_NUM_IF_MAX];
    const uint8_t *c = km_usb_config_desc(&cn);
    report_lengths(rd);
    uint8_t m[256];

    /* configuration descriptor: flip one field at a time. Offsets follow the layout in km_usb_desc.c: config 9 B, then
     * IF0 (interface 9, HID 9, EP 7) at 9, IF1 (interface 9, HID 9, EP 7, EP 7) at 34, IF2 at 66. */
    static const struct { const char *what; size_t at; uint8_t val; } cm[] = {
        { "wTotalLength", 2, 92 },
        { "bNumInterfaces", 4, 2 },
        { "bConfigurationValue", 5, 2 },
        { "IF0 number", 9 + 2, 1 },
        { "IF0 subclass", 9 + 6, 0 },
        { "IF0 protocol", 9 + 7, 2 },
        { "IF0 HID wDescriptorLength", 9 + 9 + 7, 0 },
        { "IF0 HID bNumDescriptors", 9 + 9 + 5, 2 },
        { "EP1 address", 9 + 18 + 2, 0x82 },
        { "EP1 packet size", 9 + 18 + 4, 16 },
        { "EP1 interval", 9 + 18 + 6, 0 },
        { "EP2 type", 34 + 18 + 3, 0x02 },
        { "EP10 address", 34 + 25 + 2, 0x82 },
        { "IF1 endpoint count", 34 + 4, 1 },
        { "EP3 packet size", 66 + 18 + 4, 8 },
        { "EP3 length", 66 + 18, 6 },
    };
    for (unsigned i = 0; i < sizeof cm / sizeof cm[0]; i++) {
        memcpy(m, c, cn);
        CHECK(m[cm[i].at] != cm[i].val);          /* the mutation really changes something */
        m[cm[i].at] = cm[i].val;
        const int err = validate_config(m, cn, rd);
        if (err == 0) {
            printf("  config mutation not caught: %s\n", cm[i].what);
        }
        CHECK(err != 0);
    }
    /* truncated / padded */
    CHECK(validate_config(c, cn - 1, rd) != 0);
    memcpy(m, c, cn);
    m[cn] = 7;
    m[cn + 1] = 0x05;
    CHECK(validate_config(m, cn + 2, rd) != 0);
    /* a report descriptor of another length than the HID descriptor says */
    size_t bad[KM_USB_NUM_IF_MAX] = { rd[0] + 1, rd[1], rd[2], 0 };
    CHECK(validate_config(c, cn, bad) != 0);

    /* report descriptors */
    for (unsigned i = 0; i < KM_USB_NUM_IF; i++) {
        size_t n;
        const uint8_t *d = km_usb_report_desc(i, &n);
        CHECK(n < sizeof m);
        /* the item-by-item specification must reject every single-byte increment or decrement of every byte */
        for (size_t at = 0; at < n; at++) {
            for (int delta = -1; delta <= 1; delta += 2) {
                memcpy(m, d, n);
                m[at] = (uint8_t)(m[at] + delta);
                CHECK(check_report(m, n, &k_expect_rd[i]) != 0);
            }
        }
        memcpy(m, d, n);
        CHECK(check_report(m, n - 1, &k_expect_rd[i]) != 0);          /* last End Collection missing */
        m[n] = 0xC0;
        CHECK(check_report(m, n + 1, &k_expect_rd[i]) != 0);         /* extra End Collection */
    }
    /* specific mistakes */
    {
        size_t n;
        const uint8_t *d = km_usb_report_desc(KM_USB_IF_VND, &n);
        memcpy(m, d, n);
        bool hit = false;
        for (size_t at = 0; at + 1 < n; at++) {
            if (m[at] == 0x95 && m[at + 1] == 64) {                      /* report count 64 -> 63 */
                m[at + 1] = 63;
                hit = true;
            }
        }
        CHECK(hit && check_report(m, n, &k_expect_rd[KM_USB_IF_VND]) != 0);
        d = km_usb_report_desc(KM_USB_IF_NKRO, &n);
        memcpy(m, d, n);
        hit = false;
        for (size_t at = 0; at + 2 < n; at++) {
            if (m[at] == 0x96 && m[at + 1] == (KM_USB_NKRO_LAST_USAGE - 3u)) {   /* 228 -> 229 bits */
                m[at + 1]++;
                hit = true;
            }
        }
        CHECK(hit && check_report(m, n, &k_expect_rd[KM_USB_IF_NKRO]) != 0);
        memcpy(m, d, n);
        m[0] = 0xFE;                                                     /* a long item */
        CHECK(check_report(m, n, &k_expect_rd[KM_USB_IF_NKRO]) != 0);
        memcpy(m, d, n);
        m[0] = 0xFC;                                                     /* reserved item type */
        CHECK(check_report(m, n, &k_expect_rd[KM_USB_IF_NKRO]) != 0);
    }
}

/* ================================================================ EP0 policy */

static km_ctrl_result_t request(km_usb_dev_t *dev, uint8_t bm, uint8_t req, uint16_t value, uint16_t index,
                                uint16_t length)
{
    uint8_t raw[8] = { bm, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8),
                       (uint8_t)length, (uint8_t)(length >> 8) };
    km_usb_setup_t s;
    km_ctrl_result_t r;
    km_usb_setup_parse(raw, &s);
    km_usb_ctrl_handle(dev, &s, &r);
    return r;
}

static void test_enumeration(void)
{
    km_usb_dev_t dev;
    memset(&dev, 0, sizeof dev);
    dev.uid[0] = 0x01020304u;
    km_usb_dev_reset(&dev);
    size_t n, cn;
    const uint8_t *cfg = km_usb_config_desc(&cn);
    km_ctrl_result_t r;

    /* the first request Windows sends: 8 bytes of the device descriptor */
    r = request(&dev, 0x80, 0x06, 0x0100, 0, 8);
    CHECK(r.kind == KM_CTRL_IN && r.len == 8 && r.data == km_usb_device_desc(&n) && !r.zlp);
    r = request(&dev, 0x80, 0x06, 0x0100, 0, 64);
    CHECK(r.kind == KM_CTRL_IN && r.len == 18 && r.data == km_usb_device_desc(&n));

    r = request(&dev, 0x00, 0x05, 7, 0, 0);
    CHECK(r.kind == KM_CTRL_ACK && r.apply == KM_CTRL_APPLY_ADDRESS && r.value == 7);
    r = request(&dev, 0x00, 0x05, 200, 0, 0);
    CHECK(r.kind == KM_CTRL_STALL);

    r = request(&dev, 0x80, 0x06, 0x0200, 0, 9);
    CHECK(r.kind == KM_CTRL_IN && r.len == 9);
    r = request(&dev, 0x80, 0x06, 0x0200, 0, 255);
    CHECK(r.kind == KM_CTRL_IN && r.len == cn && r.data == cfg && !r.zlp);
    r = request(&dev, 0x80, 0x06, 0x0201, 0, 255);
    CHECK(r.kind == KM_CTRL_STALL);            /* only one configuration */
    r = request(&dev, 0x80, 0x06, 0x0600, 0, 10);
    CHECK(r.kind == KM_CTRL_IN && r.len == 10);
    r = request(&dev, 0x80, 0x06, 0x0700, 0, 255);
    CHECK(r.kind == KM_CTRL_STALL);            /* no other-speed configuration */
    r = request(&dev, 0x80, 0x06, 0x0F00, 0, 255);
    CHECK(r.kind == KM_CTRL_STALL);            /* no BOS descriptor */

    for (unsigned i = 0; i <= 3; i++) {
        r = request(&dev, 0x80, 0x06, (uint16_t)(0x0300 | i), 0x0409, 255);
        CHECK(r.kind == KM_CTRL_IN && r.len == r.data[0] && r.data[1] == 3);
    }
    r = request(&dev, 0x80, 0x06, 0x0304, 0x0409, 255);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x80, 0x06, 0x03EE, 0, 255);
    CHECK(r.kind == KM_CTRL_STALL);            /* Microsoft OS string: not supported */

    CHECK_EQ(dev.configuration, 0);
    r = request(&dev, 0x80, 0x08, 0, 0, 1);
    CHECK(r.kind == KM_CTRL_IN && r.len == 1 && r.data[0] == 0);
    r = request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK(r.kind == KM_CTRL_ACK && r.apply == KM_CTRL_APPLY_CONFIGURATION && r.value == 1);
    CHECK_EQ(dev.configuration, 1);
    r = request(&dev, 0x80, 0x08, 0, 0, 1);
    CHECK(r.kind == KM_CTRL_IN && r.data[0] == 1);
    r = request(&dev, 0x00, 0x09, 2, 0, 0);
    CHECK(r.kind == KM_CTRL_STALL);

    /* HID: report and HID descriptors per interface */
    for (unsigned i = 0; i < 3; i++) {
        size_t rl, hl;
        km_usb_report_desc(i, &rl);
        r = request(&dev, 0x81, 0x06, 0x2200, (uint16_t)i, 0x1000);
        CHECK(r.kind == KM_CTRL_IN && r.len == rl && r.data == km_usb_report_desc(i, &rl));
        r = request(&dev, 0x81, 0x06, 0x2100, (uint16_t)i, 9);
        CHECK(r.kind == KM_CTRL_IN && r.len == 9 && r.data == km_usb_hid_desc(i, &hl));
        r = request(&dev, 0x81, 0x06, 0x2201, (uint16_t)i, 255);       /* only descriptor index 0 */
        CHECK(r.kind == KM_CTRL_STALL);
    }
    r = request(&dev, 0x81, 0x06, 0x2200, 3, 255);
    CHECK(r.kind == KM_CTRL_STALL);

    /* status and standard interface / endpoint requests */
    r = request(&dev, 0x80, 0x00, 0, 0, 2);
    CHECK(r.kind == KM_CTRL_IN && r.len == 2 && r.data[0] == 0 && r.data[1] == 0);
    r = request(&dev, 0x81, 0x0A, 0, 1, 1);
    CHECK(r.kind == KM_CTRL_IN && r.data[0] == 0);
    r = request(&dev, 0x01, 0x0B, 0, 1, 0);
    CHECK(r.kind == KM_CTRL_ACK);
    r = request(&dev, 0x01, 0x0B, 1, 1, 0);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x82, 0x00, 0, 0x82, 2);
    CHECK(r.kind == KM_CTRL_IN && r.len == 2);
    r = request(&dev, 0x82, 0x00, 0, 0x85, 2);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x02, 0x01, 0, 0x0A, 0);                           /* CLEAR_FEATURE(ENDPOINT_HALT) */
    CHECK(r.kind == KM_CTRL_ACK);
    r = request(&dev, 0x01, 0x0B, 0, 3, 0);                              /* no interface 3 */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x81, 0x00, 0, 3, 2);
    CHECK(r.kind == KM_CTRL_STALL);

    /* zero-length packet rule (no descriptor needs it, so test the rule itself) */
    CHECK(km_usb_ctrl_needs_zlp(64, 255));
    CHECK(km_usb_ctrl_needs_zlp(128, 129));
    CHECK(!km_usb_ctrl_needs_zlp(64, 64));      /* host asked for exactly what it got */
    CHECK(!km_usb_ctrl_needs_zlp(63, 255));
    CHECK(!km_usb_ctrl_needs_zlp(91, 255));
    CHECK(!km_usb_ctrl_needs_zlp(0, 255));

    /* a bus reset puts everything back */
    km_usb_dev_reset(&dev);
    CHECK_EQ(dev.configuration, 0);
    CHECK_EQ(dev.protocol[0], 1);
    CHECK_EQ(dev.uid[0], 0x01020304);
}

static void test_hid_class_requests(void)
{
    km_usb_dev_t dev;
    memset(&dev, 0, sizeof dev);
    km_usb_dev_reset(&dev);
    km_ctrl_result_t r;
    request(&dev, 0x00, 0x09, 1, 0, 0);

    /* SET_IDLE / GET_IDLE: the two keyboard interfaces keep a rate, the vendor channel has none */
    for (unsigned i = 0; i < 3; i++) {
        r = request(&dev, 0x21, 0x0A, 0x0000, (uint16_t)i, 0);
        CHECK(i == 1 ? r.kind == KM_CTRL_STALL : r.kind == KM_CTRL_ACK);
        r = request(&dev, 0xA1, 0x03, 0, (uint16_t)i, 1);                /* GET_PROTOCOL: boot interface only */
        if (i == 0) {
            CHECK(r.kind == KM_CTRL_IN && r.data[0] == 1);
        } else {
            CHECK(r.kind == KM_CTRL_STALL);
        }
    }
    r = request(&dev, 0x21, 0x0A, 0x7D00, 0, 0);                         /* SET_IDLE(125 * 4 ms), all reports */
    CHECK(r.kind == KM_CTRL_ACK && dev.idle[0] == 0x7D);
    r = request(&dev, 0xA1, 0x02, 0, 0, 1);
    CHECK(r.kind == KM_CTRL_IN && r.len == 1 && r.data[0] == 0x7D);
    r = request(&dev, 0xA1, 0x02, 0, 0, 2);                              /* GET_IDLE is one byte */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x0A, 0x0A01, 2, 0);                         /* idle for report ID 1 of IF2 */
    CHECK(r.kind == KM_CTRL_ACK && dev.idle[2] == 0x0A);
    r = request(&dev, 0x21, 0x0A, 0x0A05, 2, 0);                         /* there is no report ID 5 */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x0A, 0x0A01, 0, 0);                         /* IF0 has no report IDs */
    CHECK(r.kind == KM_CTRL_STALL);

    r = request(&dev, 0x21, 0x0B, 0, 0, 0);                              /* SET_PROTOCOL(boot) */
    CHECK(r.kind == KM_CTRL_ACK && dev.protocol[0] == 0);
    r = request(&dev, 0xA1, 0x03, 0, 0, 1);
    CHECK(r.kind == KM_CTRL_IN && r.data[0] == 0);
    r = request(&dev, 0x21, 0x0B, 2, 0, 0);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x0B, 0, 2, 0);                              /* not a boot interface */
    CHECK(r.kind == KM_CTRL_STALL && dev.protocol[2] == 1);
    r = request(&dev, 0x21, 0x0B, 1, 0, 0);
    CHECK(r.kind == KM_CTRL_ACK && dev.protocol[0] == 1);

    /* SET_REPORT(output): LEDs on IF0 (1 byte) and IF2 (ID 1 + 1 byte) are taken; test_host_leds() checks what is done with them */
    r = request(&dev, 0x21, 0x09, 0x0200, 0, 1);
    CHECK(r.kind == KM_CTRL_OUT && r.len == 1);
    r = request(&dev, 0x21, 0x09, 0x0200, 0, 2);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x09, 0x0201, 2, 2);
    CHECK(r.kind == KM_CTRL_OUT && r.len == 2);
    r = request(&dev, 0x21, 0x09, 0x0202, 2, 2);                         /* ID 2 has no output report */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x09, 0x0301, 2, 2);                         /* feature report: none exist */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x09, 0x0100, 0, 1);                         /* input report cannot be written */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x09, 0x0200, 1, 64);
    CHECK(r.kind == KM_CTRL_OUT && r.len == 64);
    r = request(&dev, 0x21, 0x09, 0x0200, 1, 65);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x09, 0x0200, 0, 0);                         /* no data stage */
    CHECK(r.kind == KM_CTRL_STALL);

    /* GET_REPORT(input): the last report sent, with its report ID; "nothing pressed" before any */
    r = request(&dev, 0xA1, 0x01, 0x0100, 0, 8);
    CHECK(r.kind == KM_CTRL_IN && r.len == 8 && r.data == dev.last_kbd);
    CHECK_EQ(r.data[0] | r.data[2], 0);
    r = request(&dev, 0xA1, 0x01, 0x0100, 0, 4);                         /* host takes less */
    CHECK(r.kind == KM_CTRL_IN && r.len == 4);
    r = request(&dev, 0xA1, 0x01, 0x0101, 2, 64);
    CHECK(r.kind == KM_CTRL_IN && r.len == KM_USB_NKRO_REPORT_LEN && r.data[0] == 1 && r.data[1] == 0);
    r = request(&dev, 0xA1, 0x01, 0x0102, 2, 64);
    CHECK(r.kind == KM_CTRL_IN && r.len == KM_USB_CONSUMER_REPORT_LEN && r.data[0] == 2);
    r = request(&dev, 0xA1, 0x01, 0x0103, 2, 64);                        /* no ID 3 */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xA1, 0x01, 0x0100, 2, 64);                        /* IF2 always has an ID */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xA1, 0x01, 0x0101, 0, 8);                         /* IF0 has none */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xA1, 0x01, 0x0200, 0, 1);                         /* output reports cannot be read */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xA1, 0x01, 0x0300, 0, 1);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xA1, 0x01, 0x0100, 1, 64);                        /* vendor channel: all zero */
    CHECK(r.kind == KM_CTRL_IN && r.len == 64 && r.data[0] == 0 && r.data[63] == 0);
    r = request(&dev, 0xA1, 0x01, 0x0100, 0, 0);
    CHECK(r.kind == KM_CTRL_STALL);

    /* everything else is a STALL: unknown class requests, requests in the wrong direction, unknown type */
    r = request(&dev, 0xA1, 0x04, 0, 0, 1);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x03, 0, 0, 0);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x01, 0x0100, 0, 8);                         /* GET_REPORT with the OUT direction bit */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xA1, 0x0A, 0, 0, 0);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xC1, 0x01, 0x0100, 0, 8);                         /* vendor request type */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x22, 0x0A, 0, 0, 0);                              /* class request to an endpoint */
    CHECK(r.kind == KM_CTRL_STALL);
}

/* ================================================================ IN endpoints */

typedef struct {
    unsigned calls;
    unsigned ep;
    unsigned len;
    uint8_t  data[64];
} sink_t;

static sink_t g_sink;

static void fake_start(unsigned ep, const uint8_t *data, unsigned len)
{
    g_sink.calls++;
    g_sink.ep = ep;
    g_sink.len = len;
    memcpy(g_sink.data, data, len);
}

static void fresh(km_usb_dev_t *dev)
{
    memset(dev, 0, sizeof *dev);
    km_usb_dev_reset(dev);
    memset(&g_sink, 0, sizeof g_sink);
}

/* ================================================================ host keyboard LEDs (SET_REPORT data stage) */

/* One SET_REPORT(output) as the driver sees it: the SETUP packet, then the data stage in packets of `chunk` bytes (0 = one packet),
 * then the end of the data stage. Returns what km_usb_ctrl_out_end() answered; false too when the SETUP itself was refused. */
static bool set_report_chunked(km_usb_dev_t *dev, unsigned iface, unsigned id, const uint8_t *data, unsigned n,
                               unsigned wlength, unsigned chunk)
{
    const km_ctrl_result_t r = request(dev, 0x21, 0x09, (uint16_t)(0x0200u | id), (uint16_t)iface, (uint16_t)wlength);
    if (r.kind != KM_CTRL_OUT) {
        return false;
    }
    CHECK_EQ(r.len, wlength);
    for (unsigned at = 0; at < n;) {
        const unsigned c = (chunk == 0 || chunk > n - at) ? n - at : chunk;
        km_usb_ctrl_out_push(dev, data + at, c);
        at += c;
    }
    return km_usb_ctrl_out_end(dev);
}

static bool set_report(km_usb_dev_t *dev, unsigned iface, unsigned id, const uint8_t *data, unsigned n)
{
    return set_report_chunked(dev, iface, id, data, n, n, 0);
}

static void test_host_leds(void)
{
    km_usb_dev_t dev;
    fresh(&dev);
    request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK_EQ(dev.host_leds, 0);                                          /* nothing told yet = all off */
    CHECK_EQ(dev.led_reports, 0);

    /* the three lock LEDs, from either keyboard interface; the last report wins */
    const uint8_t caps0[1] = { KM_USB_LED_CAPS_LOCK };
    CHECK(set_report(&dev, KM_USB_IF_KBD, 0, caps0, 1));
    CHECK_EQ(dev.host_leds, KM_USB_LED_CAPS_LOCK);
    CHECK_EQ(dev.led_reports, 1);
    const uint8_t num_scroll2[2] = { KM_USB_NKRO_ID_KEYS, KM_USB_LED_NUM_LOCK | KM_USB_LED_SCROLL_LOCK };
    CHECK(set_report(&dev, KM_USB_IF_NKRO, KM_USB_NKRO_ID_KEYS, num_scroll2, 2));
    CHECK_EQ(dev.host_leds, 5);
    CHECK_EQ(dev.led_reports, 2);
    const uint8_t off0[1] = { 0 };
    CHECK(set_report(&dev, KM_USB_IF_KBD, 0, off0, 1));
    CHECK_EQ(dev.host_leds, 0);
    for (unsigned v = 0; v < 256; v++) {                                 /* every LED byte: only the three lock bits are kept */
        const uint8_t one[1] = { (uint8_t)v };
        const uint8_t two[2] = { KM_USB_NKRO_ID_KEYS, (uint8_t)v };
        CHECK(set_report(&dev, KM_USB_IF_KBD, 0, one, 1));
        CHECK_EQ(dev.host_leds, v & 7u);
        CHECK(set_report(&dev, KM_USB_IF_NKRO, KM_USB_NKRO_ID_KEYS, two, 2));
        CHECK_EQ(dev.host_leds, v & 7u);
    }

    /* the data stage may arrive in several packets or one byte at a time */
    const uint8_t all[2] = { KM_USB_NKRO_ID_KEYS, 0x03 };
    CHECK(set_report_chunked(&dev, KM_USB_IF_NKRO, 1, all, 2, 2, 1));
    CHECK_EQ(dev.host_leds, 3);
    km_usb_ctrl_out_push(&dev, NULL, 0);                                 /* nothing in progress: ignored */
    CHECK(!km_usb_ctrl_out_end(&dev));

    /* everything below is refused or ignored and must leave host_leds exactly as it is */
    const uint8_t keep[1] = { 7 };
    CHECK(set_report(&dev, KM_USB_IF_KBD, 0, keep, 1));
    const unsigned good = dev.led_reports;
    CHECK_EQ(dev.host_leds, 7);
    const uint8_t zero2[2] = { 0, 0 };
    const uint8_t many[80] = { 0 };

    /* a refused report leaves its bytes in the buffer; the next short data stage must not read them */
    const uint8_t stale[2] = { KM_USB_NKRO_ID_CONSUMER, 2 };
    CHECK(!set_report(&dev, KM_USB_IF_NKRO, 1, stale, 2));
    /* short: the host ended the data stage early */
    CHECK(!set_report_chunked(&dev, KM_USB_IF_NKRO, 1, zero2, 1, 2, 0));
    CHECK(!set_report_chunked(&dev, KM_USB_IF_NKRO, 1, zero2, 0, 2, 0));         /* no data at all */
    CHECK(!set_report_chunked(&dev, KM_USB_IF_KBD, 0, off0, 0, 1, 0));
    /* long: more than wLength announced, in one packet or spread over several */
    CHECK(!set_report_chunked(&dev, KM_USB_IF_KBD, 0, zero2, 2, 1, 0));
    CHECK(!set_report_chunked(&dev, KM_USB_IF_KBD, 0, zero2, 2, 1, 1));
    CHECK(!set_report_chunked(&dev, KM_USB_IF_NKRO, 1, many, 64, 2, 0));
    CHECK(!set_report_chunked(&dev, KM_USB_IF_NKRO, 1, many, 3, 2, 1));
    /* a request that claims more than any report is refused outright and the stray data stage after it changes nothing */
    CHECK(!set_report(&dev, KM_USB_IF_KBD, 0, zero2, 2));                        /* wLength 2 on the boot interface: STALL */
    CHECK(!set_report(&dev, KM_USB_IF_NKRO, 1, many, 3));
    CHECK(!set_report(&dev, KM_USB_IF_KBD, 0, many, 64));
    km_usb_ctrl_out_push(&dev, zero2, 2);
    CHECK(!km_usb_ctrl_out_end(&dev));
    /* wrong report ID, inside the request or inside the data */
    const uint8_t id2[2] = { KM_USB_NKRO_ID_CONSUMER, 0 };
    const uint8_t id0[2] = { 0, 0 };
    CHECK(!set_report(&dev, KM_USB_IF_NKRO, 1, id2, 2));
    CHECK(!set_report(&dev, KM_USB_IF_NKRO, 1, id0, 2));
    CHECK(!set_report(&dev, KM_USB_IF_NKRO, KM_USB_NKRO_ID_CONSUMER, zero2, 2));
    CHECK(!set_report(&dev, KM_USB_IF_NKRO, 0, zero2, 2));
    CHECK(!set_report(&dev, KM_USB_IF_KBD, 1, off0, 1));                         /* the boot interface has no report IDs */
    /* one byte on the report-ID interface has no room for the LEDs */
    CHECK(!set_report(&dev, KM_USB_IF_NKRO, 1, off0, 1));
    /* not an output report, or not SET_REPORT, or the wrong direction */
    km_ctrl_result_t r = request(&dev, 0x21, 0x09, 0x0100, 0, 1);               /* input report */
    CHECK(r.kind == KM_CTRL_STALL);
    km_usb_ctrl_out_push(&dev, off0, 1);
    CHECK(!km_usb_ctrl_out_end(&dev));
    r = request(&dev, 0x21, 0x09, 0x0300, 0, 1);                                /* feature report */
    CHECK(r.kind == KM_CTRL_STALL);
    km_usb_ctrl_out_push(&dev, off0, 1);
    CHECK(!km_usb_ctrl_out_end(&dev));
    r = request(&dev, 0xA1, 0x09, 0x0200, 0, 1);                                /* SET_REPORT with the IN direction bit */
    CHECK(r.kind == KM_CTRL_STALL);
    km_usb_ctrl_out_push(&dev, off0, 1);
    CHECK(!km_usb_ctrl_out_end(&dev));
    /* the vendor channel takes up to 64 bytes; one more is an overrun that must be noticed and never stored */
    r = request(&dev, 0x21, 0x09, 0x0200, KM_USB_IF_VND, 64);
    CHECK(r.kind == KM_CTRL_OUT);
    km_usb_ctrl_out_push(&dev, many, 64);
    CHECK(!dev.out_bad);
    km_usb_ctrl_out_push(&dev, many, 1);
    CHECK(dev.out_bad && dev.out_len == 64);
    CHECK(!km_usb_ctrl_out_end(&dev));
    r = request(&dev, 0x21, 0x09, 0x0200, KM_USB_IF_VND, 64);
    km_usb_ctrl_out_push(&dev, many, 65);
    CHECK(dev.out_bad && dev.out_len == 0);
    CHECK(!km_usb_ctrl_out_end(&dev));
    /* the vendor channel's output report is taken and not interpreted as LEDs */
    CHECK(!set_report(&dev, KM_USB_IF_VND, 0, many, 64));
    CHECK(!set_report(&dev, KM_USB_IF_VND, 0, off0, 1));
    CHECK(!set_report(&dev, KM_USB_IF_VND, 0, all, 2));                          /* looks like an IF2 LED report: still not one */
    CHECK_EQ(dev.host_leds, 7);
    CHECK_EQ(dev.led_reports, good);
    /* a data stage that is cut off by the next SETUP packet is abandoned, whatever that next request is */
    r = request(&dev, 0x21, 0x09, 0x0200, 0, 1);
    CHECK(r.kind == KM_CTRL_OUT);
    r = request(&dev, 0x80, 0x06, 0x0100, 0, 18);
    CHECK(r.kind == KM_CTRL_IN);
    km_usb_ctrl_out_push(&dev, off0, 1);
    CHECK(!km_usb_ctrl_out_end(&dev));
    CHECK_EQ(dev.host_leds, 7);
    /* ... and a second SET_REPORT starts from scratch: the half-received bytes of the first do not count */
    const uint8_t caps[1] = { KM_USB_LED_CAPS_LOCK };
    r = request(&dev, 0x21, 0x09, 0x0200, 0, 1);
    CHECK(r.kind == KM_CTRL_OUT);
    km_usb_ctrl_out_push(&dev, off0, 1);
    CHECK(set_report(&dev, KM_USB_IF_KBD, 0, caps, 1));
    CHECK_EQ(dev.host_leds, KM_USB_LED_CAPS_LOCK);
    CHECK_EQ(dev.led_reports, good + 1);
    /* the end of a data stage is used once */
    CHECK(!km_usb_ctrl_out_end(&dev));
    km_usb_ctrl_out_push(&dev, off0, 1);
    CHECK(!km_usb_ctrl_out_end(&dev));
    CHECK_EQ(dev.host_leds, KM_USB_LED_CAPS_LOCK);
    CHECK_EQ(dev.led_reports, good + 1);                                         /* every refusal above left the counter alone */

    /* a new configuration keeps the LED state (the host does not repeat it); a bus reset forgets it, the counter stays */
    CHECK(set_report(&dev, KM_USB_IF_KBD, 0, keep, 1));
    request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK_EQ(dev.host_leds, 7);
    const unsigned total = dev.led_reports;
    km_usb_dev_reset(&dev);
    CHECK_EQ(dev.host_leds, 0);
    CHECK_EQ(dev.led_reports, total);
    r = request(&dev, 0x21, 0x09, 0x0200, 0, 1);                                /* a data stage cut off by a reset is gone */
    CHECK(r.kind == KM_CTRL_OUT);
    km_usb_dev_reset(&dev);
    km_usb_ctrl_out_push(&dev, keep, 1);
    CHECK(!km_usb_ctrl_out_end(&dev));
    CHECK_EQ(dev.host_leds, 0);
}

static void test_in_endpoints(void)
{
    km_usb_dev_t dev;
    const uint8_t kbd[8] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };            /* LShift + A */
    uint8_t nk[KM_USB_NKRO_REPORT_LEN] = { KM_USB_NKRO_ID_KEYS };
    nk[1] = 0x10;                                                        /* usage 4 (A) = bit 4 of the first data byte */
    const uint8_t cons[3] = { KM_USB_NKRO_ID_CONSUMER, 0xE2, 0x00 };     /* mute */

    fresh(&dev);
    /* not configured: nothing is queued */
    CHECK(!km_usb_in_kbd_ready(&dev) && !km_usb_in_nkro_ready(&dev));
    CHECK(!km_usb_in_send_kbd(&dev, kbd, fake_start));
    CHECK(!km_usb_in_send_nkro(&dev, nk, sizeof nk, fake_start));
    CHECK_EQ(g_sink.calls, 0);

    request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK(km_usb_in_kbd_ready(&dev) && km_usb_in_nkro_ready(&dev));

    /* boot keyboard: one in flight, the second is refused until the host took the first */
    CHECK(km_usb_in_send_kbd(&dev, kbd, fake_start));
    CHECK(g_sink.calls == 1 && g_sink.ep == 1 && g_sink.len == 8 && memcmp(g_sink.data, kbd, 8) == 0);
    CHECK(!km_usb_in_kbd_ready(&dev));
    CHECK(km_usb_in_nkro_ready(&dev));                                   /* the endpoints are independent */
    CHECK(!km_usb_in_send_kbd(&dev, kbd, fake_start));
    CHECK_EQ(g_sink.calls, 1);
    km_usb_in_complete(&dev, 3);                                         /* completion of the other endpoint changes nothing */
    CHECK(!km_usb_in_kbd_ready(&dev));
    km_usb_in_complete(&dev, 2);
    CHECK(!km_usb_in_kbd_ready(&dev));
    km_usb_in_complete(&dev, 1);
    CHECK(km_usb_in_kbd_ready(&dev));
    CHECK(km_usb_in_send_kbd(&dev, kbd, fake_start));
    CHECK_EQ(g_sink.calls, 2);

    /* NKRO endpoint: both report kinds share EP3 and its busy flag; wrong ID/length is refused without side effects */
    CHECK(km_usb_in_send_nkro(&dev, nk, sizeof nk, fake_start));
    CHECK(g_sink.ep == 3 && g_sink.len == 30 && memcmp(g_sink.data, nk, 30) == 0);
    CHECK(!km_usb_in_send_nkro(&dev, cons, sizeof cons, fake_start));
    km_usb_in_complete(&dev, 3);
    CHECK(km_usb_in_send_nkro(&dev, cons, sizeof cons, fake_start));
    CHECK(g_sink.ep == 3 && g_sink.len == 3 && memcmp(g_sink.data, cons, 3) == 0);
    km_usb_in_complete(&dev, 3);
    const unsigned calls = g_sink.calls;
    uint8_t bad[KM_USB_NKRO_REPORT_LEN];
    memcpy(bad, nk, sizeof bad);
    bad[0] = 2;                                                          /* consumer ID with a keyboard length */
    CHECK(!km_usb_in_send_nkro(&dev, bad, sizeof bad, fake_start));
    CHECK(!km_usb_in_send_nkro(&dev, nk, sizeof nk - 1, fake_start));
    CHECK(!km_usb_in_send_nkro(&dev, nk, 64, fake_start));
    CHECK(!km_usb_in_send_nkro(&dev, cons, 2, fake_start));
    CHECK(!km_usb_in_send_nkro(&dev, NULL, 30, fake_start));
    CHECK(!km_usb_in_send_nkro(&dev, nk, 0, fake_start));
    CHECK_EQ(g_sink.calls, calls);
    CHECK(km_usb_in_nkro_ready(&dev));

    /* GET_REPORT sees the last report sent */
    km_ctrl_result_t r = request(&dev, 0xA1, 0x01, 0x0100, 0, 8);
    CHECK(r.kind == KM_CTRL_IN && memcmp(r.data, kbd, 8) == 0);
    r = request(&dev, 0xA1, 0x01, 0x0101, 2, 64);
    CHECK(r.kind == KM_CTRL_IN && r.len == 30 && memcmp(r.data, nk, 30) == 0);
    r = request(&dev, 0xA1, 0x01, 0x0102, 2, 64);
    CHECK(r.kind == KM_CTRL_IN && r.len == 3 && memcmp(r.data, cons, 3) == 0);

    /* a bus reset or a new configuration forgets everything */
    CHECK(km_usb_in_send_kbd(&dev, kbd, fake_start) == false);           /* still in flight from the repeat above */
    request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK(km_usb_in_kbd_ready(&dev));
    r = request(&dev, 0xA1, 0x01, 0x0100, 0, 8);
    CHECK(r.kind == KM_CTRL_IN && r.data[0] == 0 && r.data[2] == 0);
    CHECK(km_usb_in_send_kbd(&dev, kbd, fake_start));
    km_usb_dev_reset(&dev);
    CHECK(!km_usb_in_kbd_ready(&dev));
    request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK(km_usb_in_kbd_ready(&dev));
    request(&dev, 0x00, 0x09, 0, 0, 0);                                  /* configuration 0: not ready */
    CHECK(!km_usb_in_kbd_ready(&dev) && !km_usb_in_nkro_ready(&dev));
}

static void test_idle_repeat(void)
{
    km_usb_dev_t dev;
    const uint8_t kbd[8] = { 0, 0, 0x05, 0, 0, 0, 0, 0 };
    uint8_t nk[KM_USB_NKRO_REPORT_LEN] = { KM_USB_NKRO_ID_KEYS, 0x20 };
    const uint8_t cons[3] = { KM_USB_NKRO_ID_CONSUMER, 0xE9, 0x00 };

    fresh(&dev);
    request(&dev, 0x00, 0x09, 1, 0, 0);
    /* idle 0 (the default, and what Windows sets): no repeats, however long it takes */
    km_usb_in_tick(&dev, 1000000, fake_start);
    CHECK_EQ(g_sink.calls, 0);
    CHECK(km_usb_in_send_kbd(&dev, kbd, fake_start));
    km_usb_in_complete(&dev, 1);
    km_usb_in_tick(&dev, 1000000, fake_start);
    CHECK_EQ(g_sink.calls, 1);

    /* idle 24 ms (6 units of 4 ms) = 192 micro-frames */
    request(&dev, 0x21, 0x0A, 0x0600, 0, 0);
    km_usb_in_tick(&dev, 191, fake_start);
    CHECK_EQ(g_sink.calls, 1);                                           /* the age counted from the earlier send as well */
    km_usb_in_tick(&dev, 1, fake_start);
    CHECK(g_sink.calls == 2 && g_sink.ep == 1 && g_sink.len == 8 && memcmp(g_sink.data, kbd, 8) == 0);
    CHECK(!km_usb_in_kbd_ready(&dev));
    km_usb_in_tick(&dev, 5000, fake_start);                              /* in flight: no second repeat queued on top */
    CHECK_EQ(g_sink.calls, 2);
    km_usb_in_complete(&dev, 1);
    km_usb_in_tick(&dev, 1, fake_start);                                 /* the overdue repeat goes out at the next tick */
    CHECK_EQ(g_sink.calls, 3);
    km_usb_in_complete(&dev, 1);

    /* a real send restarts the timer */
    CHECK(km_usb_in_send_kbd(&dev, kbd, fake_start));
    km_usb_in_complete(&dev, 1);
    CHECK_EQ(g_sink.calls, 4);
    km_usb_in_tick(&dev, 191, fake_start);
    CHECK_EQ(g_sink.calls, 4);

    /* NKRO: only the keyboard report (ID 1) repeats; a media key never does */
    request(&dev, 0x21, 0x0A, 0x0600, 2, 0);
    CHECK(km_usb_in_send_nkro(&dev, cons, sizeof cons, fake_start));
    km_usb_in_complete(&dev, 3);
    km_usb_in_tick(&dev, 100000, fake_start);
    CHECK(g_sink.ep != 3 || g_sink.len != 30);
    const unsigned c = g_sink.calls;
    CHECK(km_usb_in_send_nkro(&dev, nk, sizeof nk, fake_start));
    km_usb_in_complete(&dev, 3);
    CHECK_EQ(g_sink.calls, c + 1);
    km_usb_in_tick(&dev, 192, fake_start);
    CHECK(g_sink.calls == c + 2 && g_sink.ep == 3 && g_sink.len == 30 && memcmp(g_sink.data, nk, 30) == 0);

    /* not configured: no repeats */
    km_usb_dev_reset(&dev);
    km_usb_in_tick(&dev, 100000, fake_start);
    CHECK_EQ(g_sink.calls, c + 2);
}

/* ================================================================ M6 P5b: the PAD descriptor variant (gamepad interface IF3) */

static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t n)
{
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

/* The plain variant is the keyboard as it was before the gamepad existed. These bytes were hashed from the descriptors of the
 * last commit before M6 P5b (device, configuration, qualifier, then the three report descriptors). If this fails, the
 * keyboard presents something new to the host: only do that on purpose, with a new bcdDevice. */
#if KM_USB_HS_8KHZ
/* the 8 kHz set (km_m6h): the same bytes except bInterval 1 on the two keyboard endpoints and bcdDevice 0x0003 */
#define K_PLAIN_DESCRIPTOR_HASH 0x6543E78Au
#else
#define K_PLAIN_DESCRIPTOR_HASH 0x4A920842u
#endif

static void test_variant_plain_unchanged(void)
{
    size_t n;
    uint32_t h = 2166136261u;
    const uint8_t *p = km_usb_device_desc(&n);
    h = fnv1a(h, p, n);
    p = km_usb_config_desc(&n);
    h = fnv1a(h, p, n);
    p = km_usb_qualifier_desc(&n);
    h = fnv1a(h, p, n);
    for (unsigned i = 0; i < KM_USB_NUM_IF; i++) {
        p = km_usb_report_desc(i, &n);
        h = fnv1a(h, p, n);
    }
    CHECK_EQ(h, K_PLAIN_DESCRIPTOR_HASH);
    CHECK_EQ(km_usb_variant_num_if(KM_USB_VARIANT_PLAIN), 3);
    CHECK_EQ(km_usb_variant_num_if(KM_USB_VARIANT_PAD), 4);
    /* the short names are the plain variant */
    CHECK(km_usb_config_desc(&n) == km_usb_config_desc_for(KM_USB_VARIANT_PLAIN, &n));
    CHECK(km_usb_device_desc(&n) == km_usb_device_desc_for(KM_USB_VARIANT_PLAIN, &n));
    /* a stray value of the enum is the plain set, never an invalid one */
    CHECK(km_usb_config_desc_for((km_usb_variant_t)7, &n) == km_usb_config_desc(&n));
}

static void test_variant_pad_descriptors(void)
{
    size_t pn, cn, rd[KM_USB_NUM_IF_MAX];
    const uint8_t *pd = km_usb_device_desc_for(KM_USB_VARIANT_PAD, &pn);
    const uint8_t *d = km_usb_device_desc_for(KM_USB_VARIANT_PLAIN, &cn);
    CHECK(pn == 18 && cn == 18);
    for (size_t i = 0; i < 18; i++) {
        if (i == 12 || i == 13) {
            continue;
        }
        CHECK_EQ(pd[i], d[i]);                                  /* the device descriptors differ in bcdDevice only */
    }
    CHECK_EQ(d[12] | (d[13] << 8), KM_USB_BCD_DEVICE_PLAIN);
    CHECK_EQ(pd[12] | (pd[13] << 8), KM_USB_BCD_DEVICE_PAD);
#if KM_USB_HS_8KHZ
    CHECK_EQ(KM_USB_BCD_DEVICE_PLAIN, 0x0003);                  /* the 8 kHz descriptor set has its own numbers (never reused) */
    CHECK_EQ(KM_USB_BCD_DEVICE_PAD, 0x0004);
#else
    CHECK_EQ(KM_USB_BCD_DEVICE_PLAIN, 0x0001);                  /* what the keyboard has always said */
    CHECK_EQ(KM_USB_BCD_DEVICE_PAD, 0x0002);
#endif
    CHECK_EQ(KM_USB_BCD_DEVICE, KM_USB_BCD_DEVICE_PLAIN);
    CHECK(KM_USB_BCD_DEVICE_PAD != KM_USB_BCD_DEVICE_PLAIN);

    size_t pcn, ccn;
    const uint8_t *pc = km_usb_config_desc_for(KM_USB_VARIANT_PAD, &pcn);
    const uint8_t *c = km_usb_config_desc_for(KM_USB_VARIANT_PLAIN, &ccn);
    CHECK_EQ(ccn, 91);
    CHECK_EQ(pcn, 91 + 25);                                      /* + interface 9, HID 9, endpoint 7 */
    CHECK(pcn % 64 != 0);                                        /* no zero-length packet to end the EP0 answer */
    for (size_t i = 9; i < ccn; i++) {
        CHECK_EQ(pc[i], c[i]);                                   /* IF0..IF2 are byte for byte the plain ones */
    }
    CHECK(pc[4] == 4 && c[4] == 3);
    report_lengths_for(KM_USB_VARIANT_PAD, rd);
    CHECK_EQ(validate_config_n(pc, pcn, rd, 4), 0);
    CHECK(validate_config_n(pc, pcn, rd, 3) != 0);               /* the walker really tells the two apart */
    CHECK(validate_config_n(c, ccn, rd, 4) != 0);

    /* report descriptors: IF0..IF2 are shared, IF3 is the gamepad */
    for (unsigned i = 0; i < KM_USB_NUM_IF; i++) {
        size_t a, b;
        CHECK(km_usb_report_desc_for(KM_USB_VARIANT_PAD, i, &a) == km_usb_report_desc_for(KM_USB_VARIANT_PLAIN, i, &b));
        CHECK_EQ(a, b);
    }
    size_t n;
    const uint8_t *r = km_usb_report_desc_for(KM_USB_VARIANT_PAD, KM_USB_IF_PAD, &n);
    CHECK(r != NULL && n > 0 && n % 64 != 0 && n < 256);
    CHECK_EQ(check_report(r, n, &k_expect_rd[KM_USB_IF_PAD]), 0);
    CHECK(km_usb_report_desc_for(KM_USB_VARIANT_PLAIN, KM_USB_IF_PAD, &n) == NULL);   /* the plain keyboard has no IF3 */
    CHECK(km_usb_report_desc_for(KM_USB_VARIANT_PAD, 4, &n) == NULL);
    CHECK(km_usb_hid_desc_for(KM_USB_VARIANT_PLAIN, KM_USB_IF_PAD, &n) == NULL);
    CHECK(km_usb_hid_desc_for(KM_USB_VARIANT_PAD, 4, &n) == NULL);

    /* the HID descriptors point into the configuration descriptor of their own variant */
    size_t off = pc[0];
    unsigned iface = 0;
    while (off < pcn) {
        if (pc[off + 1] == 0x04) {
            iface = pc[off + 2];
        } else if (pc[off + 1] == 0x21) {
            size_t hl;
            const uint8_t *h = km_usb_hid_desc_for(KM_USB_VARIANT_PAD, iface, &hl);
            CHECK(h == pc + off && hl == 9);
        }
        off += pc[off];
    }

    /* the endpoint constants the driver uses are the ones in the descriptor */
    CHECK_EQ(KM_USB_EP_PAD_IN & 0xF, 4);
    CHECK_EQ(KM_USB_EP_PAD_IN & 0x80, 0x80);
    CHECK_EQ(KM_USB_PAD_EP_MPS, 16);
    CHECK_EQ(KM_USB_PAD_REPORT_LEN, 15);
    CHECK(KM_USB_PAD_REPORT_LEN <= KM_USB_PAD_EP_MPS);
    /* the qualifier is the same set of values for both */
    CHECK(km_usb_qualifier_desc(&n) != NULL && n == 10);
}

/* The gamepad report descriptor is read like a host reads it: fields at the bit positions the descriptor gives them. */
static long field_value(const rd_info_t *info, unsigned item, unsigned index, const uint8_t *report)
{
    const item_t *it = &info->items[item];
    const unsigned bit0 = it->bitpos + index * it->size;
    unsigned long v = 0;
    for (unsigned b = 0; b < it->size; b++) {
        const unsigned at = bit0 + b;
        v |= (unsigned long)((report[at / 8] >> (at % 8)) & 1u) << b;
    }
    if (it->lmin < 0 && (v & (1ul << (it->size - 1)))) {
        return (long)v - (1l << it->size);
    }
    return (long)v;
}

static void test_pad_report_layout(void)
{
    size_t n;
    const uint8_t *d = km_usb_report_desc_for(KM_USB_VARIANT_PAD, KM_USB_IF_PAD, &n);
    rd_info_t info;
    parse_report(d, n, &info);
    CHECK_EQ(info.err, 0);
    CHECK_EQ(info.in_bits[0], KM_USB_PAD_REPORT_LEN * 8);
    CHECK_EQ(info.out_bits[0], 0);
    CHECK_EQ(info.nitems, 5);
    CHECK_EQ(info.max_depth, 1);

    /* distinctive values in every field; the byte layout must decode the way the descriptor says */
    km_usb_pad_state_t s = { 0xA5C3u, 5, -12345, 23456, -32767, 32767, 111, 32767 };
    uint8_t rep[KM_USB_PAD_REPORT_LEN];
    CHECK(km_usb_pad_pack(&s, rep));
    for (unsigned b = 0; b < 16; b++) {
        CHECK_EQ(field_value(&info, 0, b, rep), (s.buttons >> b) & 1);
    }
    CHECK_EQ(field_value(&info, 1, 0, rep), 5);
    CHECK_EQ(field_value(&info, 3, 0, rep), -12345);
    CHECK_EQ(field_value(&info, 3, 1, rep), 23456);
    CHECK_EQ(field_value(&info, 3, 2, rep), -32767);
    CHECK_EQ(field_value(&info, 3, 3, rep), 32767);
    CHECK_EQ(field_value(&info, 4, 0, rep), 111);
    CHECK_EQ(field_value(&info, 4, 1, rep), 32767);
    CHECK_EQ(info.items[2].bitpos, 20);                          /* the padding nibble sits right after the hat */
    CHECK_EQ(info.items[3].bitpos, 24);
    /* ... and the bytes are exactly what km_usb_reports.h documents */
    static const uint8_t want[KM_USB_PAD_REPORT_LEN] = { 0xC3, 0xA5, 0x05, 0xC7, 0xCF, 0xA0, 0x5B, 0x01, 0x80, 0xFF, 0x7F, 111, 0, 0xFF, 0x7F };
    CHECK(memcmp(rep, want, sizeof want) == 0);

    /* unpack is the inverse */
    km_usb_pad_state_t t;
    CHECK(km_usb_pad_unpack(rep, &t));
    CHECK(t.buttons == s.buttons && t.hat == s.hat && t.lx == s.lx && t.ly == s.ly && t.rx == s.rx && t.ry == s.ry &&
          t.lt == s.lt && t.rt == s.rt);
    memset(&s, 0, sizeof s);
    CHECK(km_usb_pad_pack(&s, rep));
    for (unsigned i = 0; i < sizeof rep; i++) {
        CHECK_EQ(rep[i], 0);                                     /* centred = all zero bytes (also the "nothing sent yet" report) */
    }

    /* out of range is refused, never clamped; nothing is written */
    uint8_t keep[KM_USB_PAD_REPORT_LEN];
    memset(keep, 0x5A, sizeof keep);
    memcpy(rep, keep, sizeof rep);
    km_usb_pad_state_t bad = { 0, 9, 0, 0, 0, 0, 0, 0 };
    CHECK(!km_usb_pad_pack(&bad, rep));
    bad.hat = 0;
    bad.lx = -32768;
    CHECK(!km_usb_pad_pack(&bad, rep));
    bad.lx = 0; bad.ry = -32768;
    CHECK(!km_usb_pad_pack(&bad, rep));
    bad.ry = 0; bad.lt = 32768;
    CHECK(!km_usb_pad_pack(&bad, rep));
    bad.lt = 0; bad.rt = 65535;
    CHECK(!km_usb_pad_pack(&bad, rep));
    CHECK(memcmp(rep, keep, sizeof rep) == 0);
    bad.rt = 0; bad.hat = 8;
    CHECK(km_usb_pad_pack(&bad, rep));
    bad.lx = 32767; bad.ly = -32767;
    CHECK(km_usb_pad_pack(&bad, rep));
    memset(rep, 0, sizeof rep);
    rep[2] = 9;                                                  /* hat 9, or a set padding nibble (0x10) */
    CHECK(!km_usb_pad_unpack(rep, &t));
    rep[2] = 0x10;
    CHECK(!km_usb_pad_unpack(rep, &t));
    rep[2] = 0; rep[3] = 0x00; rep[4] = 0x80;                    /* LX = -32768 */
    CHECK(!km_usb_pad_unpack(rep, &t));
    rep[3] = 0; rep[4] = 0; rep[11] = 0x00; rep[12] = 0x80;      /* LT = 32768 */
    CHECK(!km_usb_pad_unpack(rep, &t));

    /* every possible hat value and every single button, every axis extreme: pack and decode through the descriptor */
    for (unsigned hat = 0; hat <= 8; hat++) {
        for (unsigned b = 0; b < 16; b++) {
            km_usb_pad_state_t x = { (uint16_t)(1u << b), (uint8_t)hat, 0, 0, 0, 0, 0, 0 };
            CHECK(km_usb_pad_pack(&x, rep));
            CHECK_EQ(field_value(&info, 1, 0, rep), hat);
            for (unsigned k = 0; k < 16; k++) {
                CHECK_EQ(field_value(&info, 0, k, rep), k == b ? 1 : 0);
            }
        }
    }
}

static void test_pad_sweep(void)
{
    /* the period of each axis: it repeats exactly after its period and not after a quarter of it */
    static const struct { uint32_t period; int which; } ax[6] = {
        { KM_USB_PAD_SWEEP_LX_MS, 0 }, { KM_USB_PAD_SWEEP_LY_MS, 1 }, { KM_USB_PAD_SWEEP_RX_MS, 2 },
        { KM_USB_PAD_SWEEP_RY_MS, 3 }, { KM_USB_PAD_SWEEP_LT_MS, 4 }, { KM_USB_PAD_SWEEP_RT_MS, 5 },
    };
    CHECK_EQ(KM_USB_PAD_SWEEP_LX_MS, 4000);
    CHECK_EQ(KM_USB_PAD_SWEEP_RT_MS, 9000);
    for (unsigned a = 0; a < 6; a++) {
        int32_t lo = 100000, hi = -100000;
        const int w = ax[a].which;
        for (uint32_t t = 0; t < 3 * ax[a].period; t += 7) {
            km_usb_pad_state_t s, s2;
            km_usb_pad_sweep(t, &s);
            km_usb_pad_sweep(t + ax[a].period, &s2);
            const int32_t v[6] = { s.lx, s.ly, s.rx, s.ry, s.lt, s.rt };
            const int32_t v2[6] = { s2.lx, s2.ly, s2.rx, s2.ry, s2.lt, s2.rt };
            CHECK_EQ(v[w], v2[w]);                                /* repeats after its period */
            lo = v[w] < lo ? v[w] : lo;
            hi = v[w] > hi ? v[w] : hi;
        }
        km_usb_pad_state_t top, bottom, quarter;
        km_usb_pad_sweep(0, &top);
        km_usb_pad_sweep(ax[a].period / 2u, &bottom);
        km_usb_pad_sweep(ax[a].period / 4u, &quarter);
        const int32_t tv[6] = { top.lx, top.ly, top.rx, top.ry, top.lt, top.rt };
        const int32_t bv[6] = { bottom.lx, bottom.ly, bottom.rx, bottom.ry, bottom.lt, bottom.rt };
        const int32_t qv[6] = { quarter.lx, quarter.ly, quarter.rx, quarter.ry, quarter.lt, quarter.rt };
        CHECK_EQ(tv[w], 32767);                                   /* a triangle: top at 0, bottom at half a period, middle at a quarter */
        if (a < 4) {
            CHECK_EQ(bv[w], -32767);
            CHECK(qv[w] > -50 && qv[w] < 50);
            CHECK(lo >= -32767 && lo <= -32700 && hi <= 32767 && hi >= 32700);   /* never outside, nearly reaches both ends */
        } else {
            CHECK_EQ(bv[w], 0);
            CHECK(qv[w] > 16383 - 50 && qv[w] < 16383 + 50);
            CHECK(lo >= 0 && lo <= 40 && hi <= 32767 && hi >= 32700);
        }
    }
    uint8_t rep[KM_USB_PAD_REPORT_LEN];
    for (uint32_t t = 0; t < 40000; t += 3) {
        km_usb_pad_state_t s, u;
        km_usb_pad_sweep(t, &s);
        CHECK(km_usb_pad_pack(&s, rep));                         /* always a valid report */
        CHECK(km_usb_pad_unpack(rep, &u) && u.lx == s.lx && u.rt == s.rt);
        CHECK(s.hat <= 8);
        CHECK(s.buttons == 0 || (s.buttons & (s.buttons - 1)) == 0);   /* at most one button */
    }
    /* one button, one hat direction per second */
    km_usb_pad_state_t s;
    km_usb_pad_sweep(0, &s);
    CHECK(s.buttons == 0 && s.hat == 0);
    km_usb_pad_sweep(1000, &s);
    CHECK(s.buttons == 1 && s.hat == 1);
    km_usb_pad_sweep(16500, &s);
    CHECK(s.buttons == 0x8000 && s.hat == 7);
    km_usb_pad_sweep(17000, &s);
    CHECK(s.buttons == 0 && s.hat == 8);
    km_usb_pad_sweep(0xFFFFFFFFu, &s);                           /* the largest time does not overflow anything */
    CHECK(km_usb_pad_pack(&s, rep));
}

/* A host that enumerates the way Windows does: device descriptor in two reads, address, descriptors by their announced
 * lengths, configuration, then every interface's HID and report descriptor and the first requests of the HID class driver. */
static void enumerate_like_windows(km_usb_dev_t *dev, km_usb_variant_t v)
{
    size_t n;
    km_ctrl_result_t r = request(dev, 0x80, 0x06, 0x0100, 0, 8);
    CHECK(r.kind == KM_CTRL_IN && r.len == 8 && r.data == km_usb_device_desc_for(v, &n));
    km_usb_dev_reset(dev);                                       /* the bus reset Windows issues after the first 8 bytes */
    r = request(dev, 0x80, 0x06, 0x0100, 0, 18);
    CHECK(r.kind == KM_CTRL_IN && r.len == 18);
    const unsigned bcd = r.data[12] | (r.data[13] << 8);
    CHECK_EQ(bcd, v == KM_USB_VARIANT_PAD ? KM_USB_BCD_DEVICE_PAD : KM_USB_BCD_DEVICE_PLAIN);
    r = request(dev, 0x00, 0x05, 9, 0, 0);
    CHECK(r.kind == KM_CTRL_ACK && r.apply == KM_CTRL_APPLY_ADDRESS && r.value == 9);
    r = request(dev, 0x80, 0x06, 0x0200, 0, 9);
    CHECK(r.kind == KM_CTRL_IN && r.len == 9);
    const unsigned total = r.data[2] | (r.data[3] << 8);
    const unsigned nif = r.data[4];
    CHECK_EQ(nif, km_usb_variant_num_if(v));
    r = request(dev, 0x80, 0x06, 0x0200, 0, (uint16_t)total);
    CHECK(r.kind == KM_CTRL_IN && r.len == total && !r.zlp && r.data == km_usb_config_desc_for(v, &n) && n == total);
    for (unsigned i = 1; i <= 3; i++) {
        r = request(dev, 0x80, 0x06, (uint16_t)(0x0300 | i), 0x0409, 255);
        CHECK(r.kind == KM_CTRL_IN);
    }
    r = request(dev, 0x00, 0x09, 1, 0, 0);
    CHECK(r.kind == KM_CTRL_ACK && r.apply == KM_CTRL_APPLY_CONFIGURATION && r.value == 1);
    for (unsigned i = 0; i < nif; i++) {
        size_t hl, rl;
        const uint8_t *h = km_usb_hid_desc_for(v, i, &hl);
        r = request(dev, 0x81, 0x06, 0x2100, (uint16_t)i, 9);
        CHECK(r.kind == KM_CTRL_IN && r.len == 9 && r.data == h);
        const unsigned want = h[7] | (h[8] << 8);                 /* wDescriptorLength from the HID descriptor */
        r = request(dev, 0x81, 0x06, 0x2200, (uint16_t)i, (uint16_t)want);
        CHECK(r.kind == KM_CTRL_IN && r.len == want && !r.zlp && r.data == km_usb_report_desc_for(v, i, &rl) && rl == want);
        r = request(dev, 0x21, 0x0A, 0x0000, (uint16_t)i, 0);   /* SET_IDLE(0): keyboards take it, the others may refuse */
        CHECK(i == KM_USB_IF_VND || i == KM_USB_IF_PAD ? r.kind == KM_CTRL_STALL : r.kind == KM_CTRL_ACK);
        r = request(dev, 0xA1, 0x01, (uint16_t)(0x0100u + (i == KM_USB_IF_NKRO ? 1u : 0u)), (uint16_t)i, 64);   /* first GET_REPORT */
        CHECK(r.kind == KM_CTRL_IN);
    }
    r = request(dev, 0x01, 0x0B, 0, 3, 0);                       /* SET_INTERFACE(IF3, alt 0): only the PAD variant has IF3 */
    CHECK(v == KM_USB_VARIANT_PAD ? r.kind == KM_CTRL_ACK : r.kind == KM_CTRL_STALL);
    r = request(dev, 0x82, 0x00, 0, KM_USB_EP_PAD_IN, 2);        /* GET_STATUS(EP4) */
    CHECK(v == KM_USB_VARIANT_PAD ? r.kind == KM_CTRL_IN && r.len == 2 : r.kind == KM_CTRL_STALL);
    r = request(dev, 0x02, 0x01, 0, KM_USB_EP_PAD_IN, 0);        /* CLEAR_FEATURE(ENDPOINT_HALT, EP4) */
    CHECK(v == KM_USB_VARIANT_PAD ? r.kind == KM_CTRL_ACK : r.kind == KM_CTRL_STALL);
}

static void test_variant_enumeration(void)
{
    for (unsigned vi = 0; vi < 2; vi++) {
        const km_usb_variant_t v = vi ? KM_USB_VARIANT_PAD : KM_USB_VARIANT_PLAIN;
        km_usb_dev_t dev;
        fresh(&dev);
        dev.variant = v;
        dev.uid[0] = 0x01020304u;
        enumerate_like_windows(&dev, v);
        CHECK_EQ(dev.configuration, 1);
        CHECK(dev.variant == v);                                  /* the host cannot change the descriptor set */
        km_usb_dev_reset(&dev);
        CHECK(dev.variant == v);                                  /* neither does a bus reset */
        CHECK_EQ(dev.uid[0], 0x01020304);
        enumerate_like_windows(&dev, v);                          /* and it can be done again after a reset */

        /* the serial number is the same in both sets (Windows keys its cache on it) */
        uint8_t s1[KM_USB_STRING_MAX];
        km_ctrl_result_t r = request(&dev, 0x80, 0x06, 0x0303, 0x0409, 255);
        CHECK(r.kind == KM_CTRL_IN);
        memcpy(s1, r.data, r.len);
        uint8_t s2[KM_USB_STRING_MAX];
        CHECK(km_usb_string_desc(3, dev.uid, s2) == r.len && memcmp(s1, s2, r.len) == 0);
    }
}

/* IF3 requests (GET_REPORT is the only one it answers) */
static void test_pad_class_requests(void)
{
    km_usb_dev_t dev;
    fresh(&dev);
    dev.variant = KM_USB_VARIANT_PAD;
    request(&dev, 0x00, 0x09, 1, 0, 0);
    km_ctrl_result_t r = request(&dev, 0xA1, 0x01, 0x0100, 3, 64);
    CHECK(r.kind == KM_CTRL_IN && r.len == KM_USB_PAD_REPORT_LEN && r.data == dev.last_pad);
    for (unsigned i = 0; i < r.len; i++) {
        CHECK_EQ(r.data[i], 0);                                   /* before anything was sent: centred */
    }
    r = request(&dev, 0xA1, 0x01, 0x0100, 3, 4);
    CHECK(r.kind == KM_CTRL_IN && r.len == 4);
    r = request(&dev, 0xA1, 0x01, 0x0101, 3, 64);                 /* no report IDs on IF3 */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xA1, 0x01, 0x0200, 3, 64);                 /* output reports cannot be read back */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xA1, 0x01, 0x0300, 3, 64);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xA1, 0x01, 0x0100, 3, 0);
    CHECK(r.kind == KM_CTRL_STALL);
    /* nothing can be written to the gamepad and it has no idle rate or protocol */
    r = request(&dev, 0x21, 0x09, 0x0200, 3, 1);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x09, 0x0200, 3, 15);
    CHECK(r.kind == KM_CTRL_STALL);
    km_usb_ctrl_out_push(&dev, dev.last_pad, 1);
    CHECK(!km_usb_ctrl_out_end(&dev));
    CHECK_EQ(dev.host_leds, 0);
    r = request(&dev, 0x21, 0x0A, 0x0000, 3, 0);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xA1, 0x02, 0, 3, 1);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x0B, 0, 3, 0);
    CHECK(r.kind == KM_CTRL_STALL && dev.protocol[3] == 1);
    r = request(&dev, 0xA1, 0x03, 0, 3, 1);
    CHECK(r.kind == KM_CTRL_STALL);
    /* the plain variant does not know interface 3 at all */
    dev.variant = KM_USB_VARIANT_PLAIN;
    r = request(&dev, 0xA1, 0x01, 0x0100, 3, 64);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x81, 0x06, 0x2200, 3, 255);
    CHECK(r.kind == KM_CTRL_STALL);
}

static void test_pad_in_endpoint(void)
{
    km_usb_dev_t dev;
    const uint8_t kbd[8] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };
    uint8_t nk[KM_USB_NKRO_REPORT_LEN] = { KM_USB_NKRO_ID_KEYS, 0x10 };
    km_usb_pad_state_t s = { 0x0001, 3, 1000, -1000, 2000, -2000, 300, 400 };
    uint8_t a[KM_USB_PAD_REPORT_LEN], b[KM_USB_PAD_REPORT_LEN];
    CHECK(km_usb_pad_pack(&s, a));
    s.buttons = 0x0002;
    CHECK(km_usb_pad_pack(&s, b));

    /* the plain variant never has a gamepad endpoint, however it is configured */
    fresh(&dev);
    request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK(!km_usb_in_pad_ready(&dev));
    CHECK(!km_usb_in_send_pad(&dev, a, sizeof a, fake_start));
    CHECK_EQ(g_sink.calls, 0);

    /* PAD variant, not yet configured */
    fresh(&dev);
    dev.variant = KM_USB_VARIANT_PAD;
    CHECK(!km_usb_in_pad_ready(&dev));
    CHECK(!km_usb_in_send_pad(&dev, a, sizeof a, fake_start));
    request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK(km_usb_in_pad_ready(&dev));

    /* wrong sizes and null are refused without side effects */
    CHECK(!km_usb_in_send_pad(&dev, a, sizeof a - 1, fake_start));
    CHECK(!km_usb_in_send_pad(&dev, a, sizeof a + 1, fake_start));
    CHECK(!km_usb_in_send_pad(&dev, a, 0, fake_start));
    CHECK(!km_usb_in_send_pad(&dev, NULL, sizeof a, fake_start));
    CHECK_EQ(g_sink.calls, 0);
    CHECK(km_usb_in_pad_ready(&dev));

    /* delivery on EP4: the bytes, the endpoint, the length; one report in flight at a time */
    CHECK(km_usb_in_send_pad(&dev, a, sizeof a, fake_start));
    CHECK(g_sink.calls == 1 && g_sink.ep == 4 && g_sink.len == KM_USB_PAD_REPORT_LEN && memcmp(g_sink.data, a, sizeof a) == 0);
    CHECK(!km_usb_in_pad_ready(&dev));
    CHECK(!km_usb_in_send_pad(&dev, b, sizeof b, fake_start));
    CHECK_EQ(g_sink.calls, 1);
    km_usb_in_complete(&dev, 1);                                  /* another endpoint's completion frees nothing here */
    km_usb_in_complete(&dev, 3);
    km_usb_in_complete(&dev, 2);
    CHECK(!km_usb_in_pad_ready(&dev));
    km_ctrl_result_t r = request(&dev, 0xA1, 0x01, 0x0100, 3, 64);   /* GET_REPORT sees the report most recently queued */
    CHECK(r.kind == KM_CTRL_IN && memcmp(r.data, a, sizeof a) == 0);
    km_usb_in_complete(&dev, 4);
    CHECK(km_usb_in_pad_ready(&dev));
    CHECK(km_usb_in_send_pad(&dev, b, sizeof b, fake_start));
    CHECK(g_sink.calls == 2 && g_sink.ep == 4 && memcmp(g_sink.data, b, sizeof b) == 0);
    km_usb_in_complete(&dev, 4);

    /* the endpoints are independent: a gamepad report in flight never delays the keyboard, and the keyboard never delays it */
    CHECK(km_usb_in_send_pad(&dev, a, sizeof a, fake_start));
    CHECK(km_usb_in_kbd_ready(&dev) && km_usb_in_nkro_ready(&dev));
    CHECK(km_usb_in_send_kbd(&dev, kbd, fake_start));
    CHECK(km_usb_in_send_nkro(&dev, nk, sizeof nk, fake_start));
    CHECK(!km_usb_in_pad_ready(&dev));
    km_usb_in_complete(&dev, 1);
    km_usb_in_complete(&dev, 3);
    CHECK(!km_usb_in_pad_ready(&dev));                            /* EP4 is still waiting for its own host poll */
    km_usb_in_complete(&dev, 4);
    CHECK(km_usb_in_pad_ready(&dev));

    /* THE HOST NEVER POLLS EP4 (no driver on IF3, a game controller driver that failed): the report stays in flight for ever and
     * nothing else suffers. Keyboard and NKRO reports keep flowing, the pad refuses new reports and repeats nothing. */
    CHECK(km_usb_in_send_pad(&dev, a, sizeof a, fake_start));
    for (unsigned i = 0; i < 2000; i++) {
        nk[1] = (uint8_t)(i & 0xFF);
        CHECK(km_usb_in_send_kbd(&dev, kbd, fake_start));
        CHECK_EQ(g_sink.ep, 1);
        km_usb_in_complete(&dev, 1);
        CHECK(km_usb_in_send_nkro(&dev, nk, sizeof nk, fake_start));
        CHECK_EQ(g_sink.ep, 3);
        km_usb_in_complete(&dev, 3);
        km_usb_in_tick(&dev, 8, fake_start);
        km_usb_in_complete(&dev, 1);
        km_usb_in_complete(&dev, 3);
        CHECK(g_sink.ep != 4);                                    /* the last endpoint started was never EP4: no repeat of the stuck report */
        CHECK(!km_usb_in_pad_ready(&dev));
        CHECK(!km_usb_in_send_pad(&dev, b, sizeof b, fake_start));
    }
    r = request(&dev, 0xA1, 0x01, 0x0100, 3, 64);
    CHECK(r.kind == KM_CTRL_IN && memcmp(r.data, a, sizeof a) == 0);   /* the stuck report is still the one GET_REPORT shows */
    /* recovery (the driver's flush of EP4 calls km_usb_in_complete(4)): the next send carries the CURRENT state */
    km_usb_in_complete(&dev, 4);
    CHECK(km_usb_in_send_pad(&dev, b, sizeof b, fake_start));
    CHECK(g_sink.ep == 4 && memcmp(g_sink.data, b, sizeof b) == 0);

    /* a new configuration or a bus reset forgets the gamepad report too, including one stuck in flight */
    request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK(km_usb_in_pad_ready(&dev));
    r = request(&dev, 0xA1, 0x01, 0x0100, 3, 64);
    for (unsigned i = 0; i < KM_USB_PAD_REPORT_LEN; i++) {
        CHECK_EQ(r.data[i], 0);
    }
    CHECK(km_usb_in_send_pad(&dev, a, sizeof a, fake_start));
    km_usb_dev_reset(&dev);
    CHECK(!km_usb_in_pad_ready(&dev));                            /* configuration 0 */
    request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK(km_usb_in_pad_ready(&dev) && dev.variant == KM_USB_VARIANT_PAD);
    request(&dev, 0x00, 0x09, 0, 0, 0);
    CHECK(!km_usb_in_pad_ready(&dev));
}

static void test_fallback_guard(void)
{
    km_usb_guard_t g;
    CHECK_EQ(KM_USB_PAD_FALLBACK_MS, 6000);

    /* a host that resets the bus and never configures: exactly 6 s after the FIRST reset the guard says "fall back", once */
    km_usb_guard_init(&g, KM_USB_VARIANT_PAD);
    CHECK(g.active == KM_USB_VARIANT_PAD && !g.running && !g.fell_back);
    CHECK(!km_usb_guard_poll(&g, 100000));                        /* nothing running before the host has appeared */
    km_usb_guard_on_reset(&g, 1000);
    CHECK(g.running);
    km_usb_guard_on_reset(&g, 3000);                              /* a retrying host does not restart the clock */
    km_usb_guard_on_reset(&g, 6000);
    CHECK(!km_usb_guard_poll(&g, 1000));
    CHECK(!km_usb_guard_poll(&g, 6999));
    CHECK(km_usb_guard_poll(&g, 7000));
    CHECK(g.fell_back && g.active == KM_USB_VARIANT_PLAIN && !g.running && g.requested == KM_USB_VARIANT_PAD);
    CHECK(!km_usb_guard_poll(&g, 7001));                          /* once */
    CHECK(!km_usb_guard_poll(&g, 70000));
    km_usb_guard_on_reset(&g, 8000);                              /* the host's reset after the fall-back starts nothing */
    CHECK(!g.running);
    CHECK(!km_usb_guard_poll(&g, 90000));
    km_usb_guard_on_configured(&g);
    CHECK(g.fell_back && !g.accepted && g.active == KM_USB_VARIANT_PLAIN);   /* the plain set being configured is not "PAD accepted" */

    /* configured in time: never falls back, however long it runs */
    km_usb_guard_init(&g, KM_USB_VARIANT_PAD);
    km_usb_guard_on_reset(&g, 50);
    CHECK(!km_usb_guard_poll(&g, 50 + 5999));
    km_usb_guard_on_configured(&g);
    CHECK(g.accepted && !g.running);
    CHECK(!km_usb_guard_poll(&g, 50 + 6000));
    CHECK(!km_usb_guard_poll(&g, 0xFFFFFFFFu));
    km_usb_guard_on_reset(&g, 100000);                            /* later bus resets (resume, hub) never start it again */
    CHECK(!g.running && !km_usb_guard_poll(&g, 0x80000000u));
    CHECK(g.active == KM_USB_VARIANT_PAD && !g.fell_back);

    /* no host, no reset: nothing happens */
    km_usb_guard_init(&g, KM_USB_VARIANT_PAD);
    for (uint32_t t = 0; t < 100000; t += 997) {
        CHECK(!km_usb_guard_poll(&g, t));
    }
    CHECK(!g.fell_back);

    /* configured exactly at the limit still counts if it arrives before the poll; after the poll it is too late */
    km_usb_guard_init(&g, KM_USB_VARIANT_PAD);
    km_usb_guard_on_reset(&g, 10);
    km_usb_guard_on_configured(&g);
    CHECK(!km_usb_guard_poll(&g, 10 + 6000));
    km_usb_guard_init(&g, KM_USB_VARIANT_PAD);
    km_usb_guard_on_reset(&g, 10);
    CHECK(km_usb_guard_poll(&g, 10 + 6000));
    km_usb_guard_on_configured(&g);
    CHECK(g.fell_back && !g.accepted);

    /* the millisecond clock wraps: the 6 s are measured with unsigned subtraction */
    km_usb_guard_init(&g, KM_USB_VARIANT_PAD);
    km_usb_guard_on_reset(&g, 0xFFFFF000u);
    CHECK(!km_usb_guard_poll(&g, 0xFFFFFFFFu));
    CHECK(!km_usb_guard_poll(&g, 0x00000500u));                   /* 4096 ms to the wrap + 1280 = 5376 ms */
    CHECK(km_usb_guard_poll(&g, 0xFFFFF000u + 6000u));

    /* the plain variant has nothing to fall back from */
    km_usb_guard_init(&g, KM_USB_VARIANT_PLAIN);
    km_usb_guard_on_reset(&g, 0);
    CHECK(!g.running);
    CHECK(!km_usb_guard_poll(&g, 1000000));
    CHECK(g.active == KM_USB_VARIANT_PLAIN && !g.fell_back);
}

/* The whole drill with the real descriptor code: the host never configures the PAD descriptor set, the guard fires, the device
 * comes back as the plain keyboard and everything a keyboard needs works. The driver's part is the three lines marked "driver". */
static void test_fallback_drill(void)
{
    km_usb_dev_t dev;
    km_usb_guard_t g;
    fresh(&dev);
    dev.uid[0] = 0x0BADF00Du;
    km_usb_guard_init(&g, KM_USB_VARIANT_PAD);
    dev.variant = g.active;                                        /* driver: present the requested set */
    dev.test_refuse_configuration = true;                          /* the hardware drill's switch */
    uint32_t now = 0;
    size_t n;

    /* the host retries a few times, every attempt a bus reset, descriptors fine, SET_CONFIGURATION answered with a STALL */
    for (unsigned attempt = 0; attempt < 5; attempt++) {
        km_usb_dev_reset(&dev);                                    /* driver: bus reset */
        km_usb_guard_on_reset(&g, now);
        km_ctrl_result_t r = request(&dev, 0x80, 0x06, 0x0100, 0, 18);
        CHECK(r.kind == KM_CTRL_IN && (r.data[12] | (r.data[13] << 8)) == KM_USB_BCD_DEVICE_PAD);
        r = request(&dev, 0x80, 0x06, 0x0200, 0, 255);
        CHECK(r.kind == KM_CTRL_IN && r.len == 116);
        r = request(&dev, 0x00, 0x09, 1, 0, 0);
        CHECK(r.kind == KM_CTRL_STALL && r.apply == KM_CTRL_APPLY_NONE);
        CHECK_EQ(dev.configuration, 0);
        r = request(&dev, 0x00, 0x09, 0, 0, 0);                    /* unconfiguring is still allowed */
        CHECK(r.kind == KM_CTRL_ACK);
        CHECK(!km_usb_in_kbd_ready(&dev));                         /* nothing can be sent: the host never configured */
        now += 1000;
        CHECK(!km_usb_guard_poll(&g, now));                        /* 1 s, 2 s, ... 5 s after the first reset: patience */
    }
    now += 999;
    CHECK(!km_usb_guard_poll(&g, now));                            /* 5999 ms after the first reset */
    now += 1;
    CHECK(km_usb_guard_poll(&g, now));                             /* 6000 ms */

    dev.variant = g.active;                                        /* driver: switch the set, drop off the bus, come back */
    km_usb_dev_reset(&dev);
    km_usb_guard_on_reset(&g, now + 300);
    CHECK(dev.variant == KM_USB_VARIANT_PLAIN);
    CHECK(dev.uid[0] == 0x0BADF00Du);

    /* what the host sees now is the keyboard it always knew: bcdDevice 1, 3 interfaces, and it can be configured and used even
     * though the drill's refusal switch is still set (it only ever applies to the PAD set) */
    km_ctrl_result_t r = request(&dev, 0x80, 0x06, 0x0100, 0, 18);
    CHECK(r.kind == KM_CTRL_IN && (r.data[12] | (r.data[13] << 8)) == KM_USB_BCD_DEVICE_PLAIN);
    r = request(&dev, 0x80, 0x06, 0x0200, 0, 255);
    CHECK(r.kind == KM_CTRL_IN && r.len == 91 && r.data == km_usb_config_desc(&n) && r.data[4] == 3);
    enumerate_like_windows(&dev, KM_USB_VARIANT_PLAIN);
    CHECK_EQ(dev.configuration, 1);
    km_usb_guard_on_configured(&g);
    CHECK(g.fell_back && !g.accepted);
    const uint8_t kbd[8] = { 0, 0, 0x05, 0, 0, 0, 0, 0 };
    CHECK(km_usb_in_send_kbd(&dev, kbd, fake_start) && g_sink.ep == 1);
    uint8_t rep[KM_USB_PAD_REPORT_LEN] = { 0 };
    CHECK(!km_usb_in_pad_ready(&dev) && !km_usb_in_send_pad(&dev, rep, sizeof rep, fake_start));
    CHECK(!km_usb_guard_poll(&g, now + 1000000));                  /* and it never happens again */

    /* the same drill but the host DOES configure in time: nothing changes */
    fresh(&dev);
    km_usb_guard_init(&g, KM_USB_VARIANT_PAD);
    dev.variant = g.active;
    km_usb_dev_reset(&dev);
    km_usb_guard_on_reset(&g, 0);
    enumerate_like_windows(&dev, KM_USB_VARIANT_PAD);
    km_usb_guard_on_configured(&g);
    CHECK(!km_usb_guard_poll(&g, 6000) && !km_usb_guard_poll(&g, 60000));
    CHECK(dev.variant == KM_USB_VARIANT_PAD && km_usb_in_pad_ready(&dev));

    /* the refusal switch only touches SET_CONFIGURATION(1) of the PAD set */
    fresh(&dev);
    dev.test_refuse_configuration = true;
    r = request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK(r.kind == KM_CTRL_ACK);                                  /* plain set: configurable */
    dev.variant = KM_USB_VARIANT_PAD;
    dev.test_refuse_configuration = false;
    r = request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK(r.kind == KM_CTRL_ACK && dev.configuration == 1);        /* PAD set without the switch: configurable */
}

static void test_fifo_plan_and_hwcfg(void)
{
    km_usb_fifo_t plan[KM_USB_FIFO_MAX];
    unsigned n = km_usb_fifo_plan(KM_USB_VARIANT_PLAIN, plan);
    /* the layout the keyboard has always programmed: RX 0x80 words, EP0 0x20 at 0x80, EP1 0x10 at 0xA0, EP2 0x20 at 0xB0, EP3 0x20 at 0xD0 */
    static const km_usb_fifo_t plain[5] = { { KM_USB_FIFO_RX, 0, 0x80 }, { 0, 0x80, 0x20 }, { 1, 0xA0, 0x10 }, { 2, 0xB0, 0x20 }, { 3, 0xD0, 0x20 } };
    CHECK_EQ(n, 5);
    for (unsigned i = 0; i < n; i++) {
        CHECK(plan[i].ep == plain[i].ep && plan[i].start == plain[i].start && plan[i].depth == plain[i].depth);
    }
    n = km_usb_fifo_plan(KM_USB_VARIANT_PAD, plan);
    CHECK_EQ(n, 6);
    for (unsigned i = 0; i < 5; i++) {
        CHECK(plan[i].ep == plain[i].ep && plan[i].start == plain[i].start && plan[i].depth == plain[i].depth);
    }
    CHECK(plan[5].ep == 4 && plan[5].start == 0xF0 && plan[5].depth == 0x10);
    for (unsigned v = 0; v < 2; v++) {
        n = km_usb_fifo_plan(v ? KM_USB_VARIANT_PAD : KM_USB_VARIANT_PLAIN, plan);
        unsigned at = 0;
        for (unsigned i = 0; i < n; i++) {
            CHECK_EQ(plan[i].start, at);                                 /* back to back, no gaps, no overlap */
            at += plan[i].depth;
            if (plan[i].ep != KM_USB_FIFO_RX) {
                CHECK(plan[i].depth >= 16);                              /* the smallest transmit FIFO of a DWC2 core */
            }
        }
        CHECK(at <= KM_USB_FIFO_TOTAL_WORDS);
        CHECK_EQ(at, v ? 0x100 : 0xF0);
        /* every transmit FIFO holds a whole packet of its endpoint */
        for (unsigned i = 0; i < n; i++) {
            unsigned mps = 0;
            switch (plan[i].ep) {
            case 0: mps = KM_USB_EP0_MPS; break;
            case 1: mps = KM_USB_BOOT_REPORT_LEN; break;
            case 2: mps = KM_USB_VND_REPORT_LEN; break;
            case 3: mps = KM_USB_NKRO_REPORT_LEN; break;
            case 4: mps = KM_USB_PAD_EP_MPS; break;
            default: break;
            }
            CHECK(plan[i].depth * 4u >= mps);
        }
    }

    /* the registers' field positions: GHWCFG1 2 bits per endpoint, GHWCFG2[13:10] device endpoints, GHWCFG3[31:16] FIFO RAM,
     * GHWCFG4[29:26] IN endpoints - 1 and [25] dedicated FIFOs */
    uint32_t raw[4];
    km_usb_hwcfg_t hw;
    raw[0] = 0u;                                                   /* every endpoint bidirectional */
    raw[1] = 11u << 10;
    raw[2] = 0x400u << 16;
    raw[3] = (1u << 25) | (7u << 26);
    km_usb_hwcfg_decode(raw, &hw);
    CHECK(hw.num_dev_eps == 11 && hw.num_in_eps == 8 && hw.ded_fifo && hw.fifo_depth == 0x400 && hw.raw[3] == raw[3]);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PLAIN), 0);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PAD), 0);
    raw[0] = (1u << 2) | (2u << 20) | (3u << 8);                   /* EP1 IN only, EP10 OUT only, EP4 reserved */
    km_usb_hwcfg_decode(raw, &hw);
    CHECK(hw.ep_dir[1] == 1 && hw.ep_dir[10] == 2 && hw.ep_dir[4] == 3 && hw.ep_dir[0] == 0);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PLAIN), 0);    /* the endpoints the keyboard uses are fine ... */
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PAD), KM_USB_HW_IN_DIR);   /* ... and EP4 is not an IN endpoint */

    /* the case this diagnostic exists for: the core has IN endpoints 0..3 only. The plain keyboard works (it does today), the gamepad cannot. */
    raw[0] = 0; raw[1] = 11u << 10; raw[2] = 0x400u << 16; raw[3] = (1u << 25) | (3u << 26);
    km_usb_hwcfg_decode(raw, &hw);
    CHECK_EQ(hw.num_in_eps, 4);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PLAIN), 0);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PAD), KM_USB_HW_TOO_FEW_IN);
    /* not enough FIFO RAM: 0xF0 words are enough for the keyboard, EP4 needs 0x100 */
    raw[3] = (1u << 25) | (7u << 26); raw[2] = 0xF8u << 16;
    km_usb_hwcfg_decode(raw, &hw);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PLAIN), 0);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PAD), KM_USB_HW_FIFO_SPACE);
    raw[2] = 0xEFu << 16;
    km_usb_hwcfg_decode(raw, &hw);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PLAIN), KM_USB_HW_FIFO_SPACE);
    raw[2] = 0xF0u << 16;
    km_usb_hwcfg_decode(raw, &hw);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PLAIN), 0);
    /* no dedicated FIFOs, too few endpoints at all, the vendor OUT endpoint only IN */
    raw[2] = 0x400u << 16; raw[3] = (7u << 26);
    km_usb_hwcfg_decode(raw, &hw);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PLAIN), KM_USB_HW_NO_DED_FIFO);
    raw[3] = (1u << 25) | (7u << 26); raw[1] = 9u << 10;
    km_usb_hwcfg_decode(raw, &hw);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PLAIN), KM_USB_HW_TOO_FEW_EPS);
    raw[1] = 11u << 10; raw[0] = 1u << 20;
    km_usb_hwcfg_decode(raw, &hw);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PLAIN), KM_USB_HW_OUT_DIR);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PAD), KM_USB_HW_OUT_DIR);
}

/* the control-request fuzz of both descriptor sets (the plain set is the one tested by test_fuzz below) */
static void test_fuzz_pad(void)
{
    km_usb_dev_t dev;
    fresh(&dev);
    dev.variant = KM_USB_VARIANT_PAD;
    size_t longest = 0, n;
    km_usb_config_desc_for(KM_USB_VARIANT_PAD, &n);
    longest = n;
    for (unsigned i = 0; i < KM_USB_NUM_IF_PAD; i++) {
        km_usb_report_desc_for(KM_USB_VARIANT_PAD, i, &n);
        longest = n > longest ? n : longest;
    }
    uint32_t x = 777u;
    for (unsigned k = 0; k < 100000; k++) {
        uint8_t raw[8];
        for (unsigned i = 0; i < 8; i++) {
            x = x * 1664525u + 1013904223u;
            raw[i] = (uint8_t)(x >> 24);
        }
        if ((k & 3u) == 0) {
            raw[0] = (uint8_t)(raw[0] & 0xA3u);
            raw[1] &= 0x0Fu;
            raw[5] = 0;
            raw[4] &= 7u;                                          /* interface / endpoint numbers around 3 and 4 */
        }
        km_usb_setup_t s;
        km_ctrl_result_t r;
        km_usb_setup_parse(raw, &s);
        km_usb_ctrl_handle(&dev, &s, &r);
        CHECK(r.kind <= KM_CTRL_OUT);
        if (r.kind == KM_CTRL_IN) {
            CHECK(r.data != NULL && r.len >= 1 && r.len <= s.w_length && r.len <= longest);
        }
        if (r.kind == KM_CTRL_OUT) {
            CHECK(r.len >= 1 && r.len <= 64 && (s.w_index & 0xFFu) != KM_USB_IF_PAD);
            km_usb_ctrl_out_push(&dev, raw, 8);
            (void)km_usb_ctrl_out_end(&dev);
        }
        CHECK(dev.configuration <= 1 && dev.variant == KM_USB_VARIANT_PAD);
        CHECK(dev.protocol[1] == 1 && dev.protocol[2] == 1 && dev.protocol[3] == 1 && dev.protocol[0] <= 1);
        CHECK(dev.idle[1] == 0 && dev.idle[3] == 0);
        CHECK((dev.host_leds & ~KM_USB_LED_MASK) == 0);
    }
}

/* The walkers must also catch broken copies of the PAD descriptors (same idea as test_walkers_catch_mistakes). */
static void test_walkers_catch_pad_mistakes(void)
{
    size_t cn, rd[KM_USB_NUM_IF_MAX];
    const uint8_t *c = km_usb_config_desc_for(KM_USB_VARIANT_PAD, &cn);
    report_lengths_for(KM_USB_VARIANT_PAD, rd);
    uint8_t m[256];
    CHECK(cn <= sizeof m);
    /* offsets: config 9, IF0 at 9, IF1 at 34, IF2 at 66, IF3 at 91 (interface 9, HID 9, endpoint 7) */
    static const struct { const char *what; size_t at; uint8_t val; } cm[] = {
        { "wTotalLength", 2, 91 },
        { "bNumInterfaces", 4, 3 },
        { "IF3 number", 91 + 2, 2 },
        { "IF3 endpoint count", 91 + 4, 2 },
        { "IF3 class", 91 + 5, 1 },
        { "IF3 subclass", 91 + 6, 1 },
        { "IF3 protocol", 91 + 7, 1 },
        { "IF3 HID wDescriptorLength", 91 + 9 + 7, 0 },
        { "IF3 HID bDescriptorType of the report", 91 + 9 + 6, 0x23 },
        { "EP4 address", 91 + 18 + 2, 0x04 },
        { "EP4 address (second IN endpoint number)", 91 + 18 + 2, 0x85 },
        { "EP4 type", 91 + 18 + 3, 0x02 },
        { "EP4 packet size", 91 + 18 + 4, 8 },
        { "EP4 packet size high", 91 + 18 + 5, 1 },
        { "EP4 interval", 91 + 18 + 6, 0 },
        { "EP4 interval too long", 91 + 18 + 6, 17 },
        { "EP4 length", 91 + 18, 6 },
        { "EP4 duplicate of EP3", 91 + 18 + 2, KM_USB_EP_NKRO_IN },
    };
    for (unsigned i = 0; i < sizeof cm / sizeof cm[0]; i++) {
        memcpy(m, c, cn);
        CHECK(m[cm[i].at] != cm[i].val);
        m[cm[i].at] = cm[i].val;
        const int err = validate_config_n(m, cn, rd, 4);
        if (err == 0) {
            printf("  PAD config mutation not caught: %s\n", cm[i].what);
        }
        CHECK(err != 0);
    }
    CHECK(validate_config_n(c, cn - 1, rd, 4) != 0);
    memcpy(m, c, cn);
    m[cn] = 7;
    m[cn + 1] = 0x05;
    CHECK(validate_config_n(m, cn + 2, rd, 4) != 0);
    size_t bad[KM_USB_NUM_IF_MAX] = { rd[0], rd[1], rd[2], rd[3] + 1 };
    CHECK(validate_config_n(c, cn, bad, 4) != 0);

    /* the gamepad report descriptor: every single-byte increment or decrement is rejected by the item-by-item specification */
    size_t n;
    const uint8_t *d = km_usb_report_desc_for(KM_USB_VARIANT_PAD, KM_USB_IF_PAD, &n);
    CHECK(n < sizeof m);
    for (size_t at = 0; at < n; at++) {
        for (int delta = -1; delta <= 1; delta += 2) {
            memcpy(m, d, n);
            m[at] = (uint8_t)(m[at] + delta);
            if (check_report(m, n, &k_expect_rd[KM_USB_IF_PAD]) == 0) {
                printf("  gamepad report descriptor byte %zu %+d not caught\n", at, delta);
            }
            CHECK(check_report(m, n, &k_expect_rd[KM_USB_IF_PAD]) != 0);
        }
    }
    memcpy(m, d, n);
    CHECK(check_report(m, n - 1, &k_expect_rd[KM_USB_IF_PAD]) != 0);
    m[n] = 0xC0;
    CHECK(check_report(m, n + 1, &k_expect_rd[KM_USB_IF_PAD]) != 0);
    /* a usage list that does not match the field count, and a hat that leaks its physical range onto the axes, are malformed */
    memcpy(m, d, n);
    bool hit = false;
    for (size_t at = 0; at + 1 < n; at++) {
        if (m[at] == 0x95 && m[at + 1] == 4) {                    /* the four-stick report count 4 -> 3 */
            m[at + 1] = 3;
            hit = true;
        }
    }
    CHECK(hit && check_report(m, n, &k_expect_rd[KM_USB_IF_PAD]) != 0);
    rd_info_t info;
    parse_report(m, n, &info);
    CHECK(info.err != 0);                                         /* 4 usages for 3 fields */
}

/* ================================================================ XInput-style variant (M6 P11)
 * The expectations are written from km_usb_reports.h (the documented layout), not from the code under test: an independent decoder
 * reads the 20 bytes back, and the descriptor walk below follows USB 2.0 chapter 9 only. */

static int16_t xi_s16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }

/* One independent walk of the XINPUT configuration descriptor: every length chains to the total, IF0..IF2 equal the plain set,
 * IF3 is class 0xFF/0x5D/0x01 with one class descriptor and two interrupt endpoints. */
static void test_xinput_descriptors(void)
{
    size_t xn, cn, dn, pn;
    const uint8_t *xd = km_usb_device_desc_for(KM_USB_VARIANT_XINPUT, &dn);
    const uint8_t *pd = km_usb_device_desc_for(KM_USB_VARIANT_PLAIN, &pn);
    CHECK(dn == 18 && pn == 18);
    for (size_t i = 0; i < 18; i++) {
        if (i != 12 && i != 13) {
            CHECK_EQ(xd[i], pd[i]);
        }
    }
    CHECK_EQ(xd[12] | (xd[13] << 8), 0x0005);
    CHECK_EQ(KM_USB_BCD_DEVICE_XINPUT, 0x0005);
    CHECK(KM_USB_BCD_DEVICE_XINPUT != KM_USB_BCD_DEVICE_PAD && KM_USB_BCD_DEVICE_XINPUT != KM_USB_BCD_DEVICE_PLAIN);

    const uint8_t *x = km_usb_config_desc_for(KM_USB_VARIANT_XINPUT, &xn);
    const uint8_t *c = km_usb_config_desc_for(KM_USB_VARIANT_PLAIN, &cn);
    CHECK_EQ(cn, 91);
    CHECK_EQ(xn, 91 + 9 + 17 + 7 + 7);
    CHECK(xn % 64 != 0);                                        /* no zero-length packet to end the EP0 answer */
    CHECK_EQ(x[2] | (x[3] << 8), (int)xn);
    CHECK_EQ(x[4], 4);                                          /* bNumInterfaces */
    for (size_t i = 9; i < cn; i++) {
        CHECK_EQ(x[i], c[i]);                                   /* IF0..IF2 are the plain ones, byte for byte */
    }
    /* walk: header, then descriptors by their own lengths */
    size_t off = x[0];
    unsigned ifaces = 0, eps = 0, classd = 0;
    unsigned cur_if = 99;
    while (off < xn) {
        CHECK(x[off] >= 2 && off + x[off] <= xn);
        const uint8_t type = x[off + 1];
        if (type == 0x04) {
            CHECK_EQ(x[off], 9);
            cur_if = x[off + 2];
            CHECK_EQ(cur_if, ifaces);
            ifaces++;
            if (cur_if == 3) {
                CHECK(x[off + 4] == 2 && x[off + 5] == 0xFF && x[off + 6] == 0x5D && x[off + 7] == 0x01 && x[off + 3] == 0);
            }
        } else if (type == 0x21 && cur_if == 3) {
            classd++;
            CHECK_EQ(x[off], 17);
            CHECK(x[off + 5] == 0x25 && x[off + 6] == KM_USB_EP_XI_IN && x[off + 7] == KM_USB_XI_REPORT_LEN);
            CHECK(x[off + 12] == 0x13 && x[off + 13] == KM_USB_EP_XI_OUT && x[off + 14] == KM_USB_XI_OUT_MAX);
        } else if (type == 0x05 && cur_if == 3) {
            eps++;
            CHECK_EQ(x[off], 7);
            CHECK_EQ(x[off + 3], 0x03);                           /* interrupt */
            CHECK_EQ(x[off + 4] | (x[off + 5] << 8), 32);
            CHECK_EQ(x[off + 6], KM_USB_BINTERVAL_XINPUT);
            CHECK(x[off + 2] == (eps == 1 ? 0x84 : 0x04));
        }
        off += x[off];
    }
    CHECK_EQ(off, xn);
    CHECK(ifaces == 4 && classd == 1 && eps == 2);
    CHECK_EQ(km_usb_variant_num_if(KM_USB_VARIANT_XINPUT), 4);

    /* IF3 is not a HID interface: no HID descriptor, no report descriptor, and the control requests agree */
    size_t n;
    CHECK(km_usb_report_desc_for(KM_USB_VARIANT_XINPUT, KM_USB_IF_PAD, &n) == NULL);
    CHECK(km_usb_hid_desc_for(KM_USB_VARIANT_XINPUT, KM_USB_IF_PAD, &n) == NULL);
    for (unsigned i = 0; i < KM_USB_NUM_IF; i++) {
        CHECK(km_usb_report_desc_for(KM_USB_VARIANT_XINPUT, i, &n) == km_usb_report_desc_for(KM_USB_VARIANT_PLAIN, i, &n));
        CHECK(km_usb_hid_desc_for(KM_USB_VARIANT_XINPUT, i, &n) != NULL);
    }
    km_usb_dev_t dev;
    fresh(&dev);
    dev.variant = KM_USB_VARIANT_XINPUT;
    km_ctrl_result_t r = request(&dev, 0x80, 0x06, 0x0200, 0, 255);
    CHECK(r.kind == KM_CTRL_IN && r.len == xn && r.data == x);
    r = request(&dev, 0x80, 0x06, 0x0100, 0, 255);
    CHECK(r.kind == KM_CTRL_IN && r.data == xd);
    request(&dev, 0x00, 0x09, 1, 0, 0);
    r = request(&dev, 0x81, 0x06, 0x2200, 3, 255);                 /* GET_DESCRIPTOR(HID report) of IF3 */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x81, 0x06, 0x2100, 3, 9);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xA1, 0x01, 0x0100, 3, 64);                  /* HID class requests on a non-HID interface */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x09, 0x0200, 3, 1);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x21, 0x0A, 0x0000, 3, 0);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xC1, 0x01, 0x0100, 3, 20);                  /* a vendor request: we do not know any, so none is answered */
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0xC0, 0xEE, 0x0000, 3, 20);
    CHECK(r.kind == KM_CTRL_STALL);
    r = request(&dev, 0x01, 0x0B, 0, 3, 0);                        /* SET_INTERFACE(IF3, 0) is accepted */
    CHECK(r.kind == KM_CTRL_ACK);
    r = request(&dev, 0x82, 0x00, 0, KM_USB_EP_XI_IN, 2);          /* both endpoints answer GET_STATUS / CLEAR_FEATURE */
    CHECK(r.kind == KM_CTRL_IN && r.len == 2);
    r = request(&dev, 0x82, 0x00, 0, KM_USB_EP_XI_OUT, 2);
    CHECK(r.kind == KM_CTRL_IN && r.len == 2);
    r = request(&dev, 0x02, 0x01, 0, KM_USB_EP_XI_OUT, 0);
    CHECK(r.kind == KM_CTRL_ACK);
    /* the other variants do not know EP4 OUT */
    dev.variant = KM_USB_VARIANT_PAD;
    r = request(&dev, 0x82, 0x00, 0, KM_USB_EP_XI_OUT, 2);
    CHECK(r.kind == KM_CTRL_STALL);
    dev.variant = KM_USB_VARIANT_PLAIN;
    r = request(&dev, 0x82, 0x00, 0, KM_USB_EP_XI_IN, 2);
    CHECK(r.kind == KM_CTRL_STALL);
    /* the PAD and PLAIN descriptors did not change because of this variant */
    CHECK_EQ(km_usb_config_desc_for(KM_USB_VARIANT_PAD, &n) != NULL ? (int)n : 0, 116);
    CHECK_EQ(KM_USB_BCD_DEVICE_PAD, 0x0002);
}

static void test_xinput_pack(void)
{
    uint8_t o[KM_USB_XI_REPORT_LEN];
    km_usb_pad_state_t s;
    memset(&s, 0, sizeof s);
    CHECK(km_usb_xinput_pack(&s, o));
    for (unsigned i = 0; i < sizeof o; i++) {
        CHECK_EQ(o[i], i == 1 ? 0x14 : 0);                          /* the neutral report: type 0, length 0x14, all else 0 */
    }
    /* every hat direction */
    static const uint8_t hat_bits[9] = { 0x00, 0x01, 0x09, 0x08, 0x0A, 0x02, 0x06, 0x04, 0x05 };   /* centred, N, NE, E, SE, S, SW, W, NW */
    for (unsigned h = 0; h <= 8; h++) {
        s.hat = (uint8_t)h;
        CHECK(km_usb_xinput_pack(&s, o));
        CHECK_EQ(o[2], hat_bits[h]);
        CHECK_EQ(o[3], 0);
    }
    s.hat = 0;
    /* buttons 1..16 one at a time */
    static const struct { uint8_t byte, bit; } exp[16] = {
        { 3, 0x10 }, { 3, 0x20 }, { 3, 0x40 }, { 3, 0x80 }, { 3, 0x01 }, { 3, 0x02 }, { 2, 0x20 }, { 2, 0x10 },
        { 2, 0x40 }, { 2, 0x80 }, { 3, 0x04 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };
    for (unsigned b = 0; b < 16; b++) {
        s.buttons = (uint16_t)(1u << b);
        CHECK(km_usb_xinput_pack(&s, o));
        for (unsigned i = 2; i < 4; i++) {
            CHECK_EQ(o[i], exp[b].byte == i ? exp[b].bit : 0);
        }
    }
    s.buttons = 0xFFFF;                                             /* all at once: Start Back L3 R3 + LB RB Guide A B X Y */
    CHECK(km_usb_xinput_pack(&s, o));
    CHECK(o[2] == 0xF0 && o[3] == 0xF7);
    s.buttons = 0;
    /* sticks: +Y is up in XInput, so a (negative = up) HID value comes out positive */
    s.lx = 32767; s.ly = -32767; s.rx = -32767; s.ry = 32767; s.lt = 32767; s.rt = 16384;
    CHECK(km_usb_xinput_pack(&s, o));
    CHECK(xi_s16(o + 6) == 32767 && xi_s16(o + 8) == 32767 && xi_s16(o + 10) == -32767 && xi_s16(o + 12) == -32767);
    CHECK(o[4] == 255 && o[5] == 128);                              /* 16384 * 255 / 32767 = 127.5 -> 128 */
    s.ly = 1; s.ry = -300;
    CHECK(km_usb_xinput_pack(&s, o));
    CHECK(xi_s16(o + 8) == -1 && xi_s16(o + 12) == 300);
    CHECK(o[0] == 0 && o[1] == 0x14);
    for (unsigned i = 14; i < 20; i++) {
        CHECK_EQ(o[i], 0);
    }
    /* triggers: monotonic, 0 -> 0, max -> 255, never above 255 */
    unsigned prev = 0;
    for (uint32_t t = 0; t <= 32767; t++) {
        s.lt = (uint16_t)t;
        CHECK(km_usb_xinput_pack(&s, o));
        CHECK(o[4] >= prev);
        prev = o[4];
    }
    CHECK_EQ(prev, 255);
    /* out-of-range input is refused and nothing is written */
    memset(o, 0xA5, sizeof o);
    memset(&s, 0, sizeof s);
    s.hat = 9;
    CHECK(!km_usb_xinput_pack(&s, o));
    s.hat = 0; s.lx = -32768;
    CHECK(!km_usb_xinput_pack(&s, o));
    s.lx = 0; s.ry = -32768;
    CHECK(!km_usb_xinput_pack(&s, o));
    s.ry = 0; s.lt = 32768;
    CHECK(!km_usb_xinput_pack(&s, o));
    s.lt = 0; s.rt = 40000;
    CHECK(!km_usb_xinput_pack(&s, o));
    for (unsigned i = 0; i < sizeof o; i++) {
        CHECK_EQ(o[i], 0xA5);
    }
    /* from the 15-byte pad report of km_pad: random states go through both routes and agree with an independent decode */
    uint32_t rng = 0x1234567u;
    for (unsigned it = 0; it < 20000; it++) {
        km_pad_report_t pr;
        rng = rng * 1664525u + 1013904223u; pr.buttons = (uint16_t)(rng >> 8);
        rng = rng * 1664525u + 1013904223u; pr.hat = (uint8_t)((rng >> 8) % 9u);
        rng = rng * 1664525u + 1013904223u; pr.lx = (int16_t)((int32_t)((rng >> 8) % 65535u) - 32767);
        rng = rng * 1664525u + 1013904223u; pr.ly = (int16_t)((int32_t)((rng >> 8) % 65535u) - 32767);
        rng = rng * 1664525u + 1013904223u; pr.rx = (int16_t)((int32_t)((rng >> 8) % 65535u) - 32767);
        rng = rng * 1664525u + 1013904223u; pr.ry = (int16_t)((int32_t)((rng >> 8) % 65535u) - 32767);
        rng = rng * 1664525u + 1013904223u; pr.lt = (uint16_t)((rng >> 8) % 32768u);
        rng = rng * 1664525u + 1013904223u; pr.rt = (uint16_t)((rng >> 8) % 32768u);
        uint8_t pb[KM_PAD_REPORT_LEN];
        km_pad_report_pack(&pr, pb);
        CHECK(km_usb_xinput_from_pad(pb, o));
        CHECK(xi_s16(o + 6) == pr.lx && xi_s16(o + 8) == -pr.ly && xi_s16(o + 10) == pr.rx && xi_s16(o + 12) == -pr.ry);
        CHECK(o[4] == (uint8_t)(((uint32_t)pr.lt * 255u + 16383u) / 32767u) && o[5] == (uint8_t)(((uint32_t)pr.rt * 255u + 16383u) / 32767u));
        const unsigned up = !!(o[2] & 1), dn = !!(o[2] & 2), lf = !!(o[2] & 4), rt = !!(o[2] & 8);
        const unsigned h = pr.hat;
        CHECK_EQ(up, h == 1 || h == 2 || h == 8);
        CHECK_EQ(rt, h == 2 || h == 3 || h == 4);
        CHECK_EQ(dn, h == 4 || h == 5 || h == 6);
        CHECK_EQ(lf, h == 6 || h == 7 || h == 8);
        CHECK_EQ(!!(o[3] & 0x10), pr.buttons & 1);                  /* A = button 1 */
        CHECK_EQ(!!(o[3] & 0x08), 0);                               /* the unused bit stays 0 */
        CHECK_EQ(!!(o[3] & 0x04), (pr.buttons >> 10) & 1);          /* Guide = button 11 */
        CHECK(o[0] == 0 && o[1] == 0x14);
    }
    /* an invalid pad report (hat 9) is refused */
    uint8_t bad[KM_PAD_REPORT_LEN] = { 0 };
    bad[2] = 9;
    memset(o, 0x5A, sizeof o);
    CHECK(!km_usb_xinput_from_pad(bad, o));
    CHECK_EQ(o[0], 0x5A);
}

static void test_xinput_endpoints(void)
{
    km_usb_dev_t dev;
    uint8_t rep[KM_USB_XI_REPORT_LEN] = { 0, 0x14, 0x01 };
    uint8_t rep2[KM_USB_XI_REPORT_LEN] = { 0, 0x14, 0x02 };

    /* PLAIN and PAD never accept XInput reports; XINPUT never accepts HID gamepad reports */
    for (unsigned vi = 0; vi < 2; vi++) {
        fresh(&dev);
        dev.variant = vi ? KM_USB_VARIANT_PAD : KM_USB_VARIANT_PLAIN;
        request(&dev, 0x00, 0x09, 1, 0, 0);
        CHECK(!km_usb_in_xinput_ready(&dev));
        CHECK(!km_usb_in_send_xinput(&dev, rep, sizeof rep, fake_start));
        CHECK_EQ(g_sink.calls, 0);
    }
    fresh(&dev);
    dev.variant = KM_USB_VARIANT_XINPUT;
    CHECK(!km_usb_in_xinput_ready(&dev));                           /* not configured yet */
    CHECK(!km_usb_in_send_xinput(&dev, rep, sizeof rep, fake_start));
    request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK(km_usb_in_xinput_ready(&dev));
    CHECK(!km_usb_in_pad_ready(&dev));
    uint8_t hid[KM_USB_PAD_REPORT_LEN] = { 0 };
    CHECK(!km_usb_in_send_pad(&dev, hid, sizeof hid, fake_start));
    CHECK_EQ(g_sink.calls, 0);

    /* wrong length, null and a wrong header are refused without side effects */
    CHECK(!km_usb_in_send_xinput(&dev, rep, sizeof rep - 1, fake_start));
    CHECK(!km_usb_in_send_xinput(&dev, rep, sizeof rep + 1, fake_start));
    CHECK(!km_usb_in_send_xinput(&dev, NULL, sizeof rep, fake_start));
    uint8_t badhdr[KM_USB_XI_REPORT_LEN] = { 1, 0x14 };
    CHECK(!km_usb_in_send_xinput(&dev, badhdr, sizeof badhdr, fake_start));
    badhdr[0] = 0; badhdr[1] = 0x13;
    CHECK(!km_usb_in_send_xinput(&dev, badhdr, sizeof badhdr, fake_start));
    CHECK_EQ(g_sink.calls, 0);

    CHECK(km_usb_in_send_xinput(&dev, rep, sizeof rep, fake_start));
    CHECK(g_sink.calls == 1 && g_sink.ep == 4 && g_sink.len == 20 && memcmp(g_sink.data, rep, 20) == 0);
    CHECK(!km_usb_in_xinput_ready(&dev));
    CHECK(!km_usb_in_send_xinput(&dev, rep2, sizeof rep2, fake_start));
    km_usb_in_complete(&dev, 1);
    km_usb_in_complete(&dev, 3);
    CHECK(!km_usb_in_xinput_ready(&dev));
    km_usb_in_complete(&dev, 4);
    CHECK(km_usb_in_xinput_ready(&dev));
    CHECK(km_usb_in_send_xinput(&dev, rep2, sizeof rep2, fake_start));
    CHECK(memcmp(g_sink.data, rep2, 20) == 0);

    /* the host never polls EP4: the keyboard goes on regardless */
    const uint8_t kbd[8] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };
    for (unsigned i = 0; i < 1000; i++) {
        CHECK(km_usb_in_send_kbd(&dev, kbd, fake_start));
        km_usb_in_complete(&dev, 1);
        km_usb_in_tick(&dev, 8, fake_start);
        CHECK(!km_usb_in_xinput_ready(&dev));
    }
    /* a configuration change or bus reset forgets the report in flight and the host's rumble state */
    uint8_t out[8] = { 0x00, 0x08, 0x00, 200, 100, 0, 0, 0 };
    CHECK(km_usb_xinput_out(&dev, out, 8));
    km_usb_dev_reset(&dev);
    CHECK(dev.xi_rumble_large == 0 && dev.xi_rumble_small == 0);
    request(&dev, 0x00, 0x09, 1, 0, 0);
    CHECK(km_usb_in_xinput_ready(&dev));
    CHECK(dev.last_xi[0] == 0 && dev.last_xi[1] == 0x14 && dev.last_xi[2] == 0);
}

static void test_xinput_out(void)
{
    km_usb_dev_t dev;
    fresh(&dev);
    dev.variant = KM_USB_VARIANT_XINPUT;
    const uint8_t rumble[8] = { 0x00, 0x08, 0x00, 0xC8, 0x64, 0, 0, 0 };
    CHECK(km_usb_xinput_out(&dev, rumble, 8));
    CHECK(dev.xi_rumble_large == 200 && dev.xi_rumble_small == 100 && dev.xi_out_messages == 1);
    const uint8_t led[3] = { 0x01, 0x03, 0x06 };
    CHECK(km_usb_xinput_out(&dev, led, 3));
    CHECK(dev.xi_led == 6 && dev.xi_rumble_large == 200 && dev.xi_out_messages == 2);
    /* anything else is dropped without touching the state */
    const uint8_t other[8] = { 0x02, 0x08, 0, 1, 2, 0, 0, 0 };
    CHECK(!km_usb_xinput_out(&dev, other, 8));
    CHECK(!km_usb_xinput_out(&dev, rumble, 7));
    CHECK(!km_usb_xinput_out(&dev, rumble, 9));
    CHECK(!km_usb_xinput_out(&dev, led, 2));
    CHECK(!km_usb_xinput_out(&dev, NULL, 8));
    CHECK(dev.xi_rumble_large == 200 && dev.xi_rumble_small == 100 && dev.xi_led == 6 && dev.xi_out_messages == 2);
    /* other variants ignore it completely */
    dev.variant = KM_USB_VARIANT_PAD;
    CHECK(!km_usb_xinput_out(&dev, rumble, 8));
    dev.variant = KM_USB_VARIANT_PLAIN;
    CHECK(!km_usb_xinput_out(&dev, rumble, 8));
    /* every 8-byte pattern: only a real rumble message is taken */
    dev.variant = KM_USB_VARIANT_XINPUT;
    uint32_t rng = 99;
    for (unsigned it = 0; it < 5000; it++) {
        uint8_t m[8];
        for (unsigned i = 0; i < 8; i++) {
            rng = rng * 1664525u + 1013904223u;
            m[i] = (uint8_t)(rng >> 16);
        }
        const bool want = m[0] == 0 && m[1] == 8;
        CHECK_EQ(km_usb_xinput_out(&dev, m, 8), want);
    }
}

static void test_xinput_guard_and_fifo(void)
{
    km_usb_guard_t g;
    km_usb_guard_init(&g, KM_USB_VARIANT_XINPUT);
    CHECK(g.active == KM_USB_VARIANT_XINPUT);
    km_usb_guard_on_reset(&g, 1000);
    CHECK(g.running);
    CHECK(!km_usb_guard_poll(&g, 6999));
    CHECK(km_usb_guard_poll(&g, 7000));                             /* the same 6 s fall-back as the HID gamepad */
    CHECK(g.active == KM_USB_VARIANT_PLAIN && g.requested == KM_USB_VARIANT_XINPUT);
    km_usb_guard_init(&g, KM_USB_VARIANT_XINPUT);
    km_usb_guard_on_reset(&g, 5);
    km_usb_guard_on_configured(&g);
    CHECK(g.accepted && !km_usb_guard_poll(&g, 100000));

    /* the drill flag refuses SET_CONFIGURATION(1) for this variant too, and only for configuration 1 */
    km_usb_dev_t dev;
    fresh(&dev);
    dev.variant = KM_USB_VARIANT_XINPUT;
    dev.test_refuse_configuration = true;
    CHECK(request(&dev, 0x00, 0x09, 1, 0, 0).kind == KM_CTRL_STALL && dev.configuration == 0);

    /* FIFO plan: the same as the HID gamepad (EP4 needs 32 bytes = 8 words, it has 16) and inside the 0x100 words */
    km_usb_fifo_t a[KM_USB_FIFO_MAX], b[KM_USB_FIFO_MAX];
    const unsigned na = km_usb_fifo_plan(KM_USB_VARIANT_XINPUT, a), nb = km_usb_fifo_plan(KM_USB_VARIANT_PAD, b);
    CHECK(na == 6 && nb == 6);
    for (unsigned i = 0; i < na; i++) {
        CHECK(a[i].ep == b[i].ep && a[i].start == b[i].start && a[i].depth == b[i].depth);
    }
    CHECK(a[5].ep == 4 && a[5].depth * 4u >= KM_USB_XI_EP_MPS && a[5].start + a[5].depth == KM_USB_FIFO_TOTAL_WORDS);

    /* capability check: EP4 must be able to do OUT as well */
    uint32_t raw[4] = { 0, 11u << 10, 0x400u << 16, (1u << 25) | (7u << 26) };
    km_usb_hwcfg_t hw;
    km_usb_hwcfg_decode(raw, &hw);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_XINPUT), 0);
    raw[0] = 1u << 8;                                               /* EP4 IN only */
    km_usb_hwcfg_decode(raw, &hw);
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_PAD), 0);       /* the HID gamepad needs only the IN direction ... */
    CHECK_EQ(km_usb_hwcfg_check(&hw, KM_USB_VARIANT_XINPUT), KM_USB_HW_OUT_DIR);   /* ... the XInput interface needs both */
}

/* No SETUP packet makes the XINPUT variant answer with HID data for IF3, or point the driver outside its buffers. */
static void test_xinput_fuzz(void)
{
    km_usb_dev_t dev;
    fresh(&dev);
    dev.variant = KM_USB_VARIANT_XINPUT;
    request(&dev, 0x00, 0x09, 1, 0, 0);
    uint32_t rng = 0xBEEF;
    for (unsigned it = 0; it < 200000; it++) {
        uint8_t raw[8];
        for (unsigned i = 0; i < 8; i++) {
            rng = rng * 1664525u + 1013904223u;
            raw[i] = (uint8_t)(rng >> 16);
        }
        if (it & 1) {
            raw[0] = (uint8_t)((raw[0] & 0x80u) | 0x21u);          /* bias towards class requests to an interface */
            raw[4] = 3;
            raw[5] = 0;
        }
        km_usb_setup_t s;
        km_ctrl_result_t r;
        km_usb_setup_parse(raw, &s);
        km_usb_ctrl_handle(&dev, &s, &r);
        const bool to_if3 = ((s.bm_request_type & 0x1F) == 1) && (s.w_index & 0xFF) == 3 && ((s.bm_request_type >> 5) & 3) != 0;
        if (to_if3 && ((s.bm_request_type >> 5) & 3) == 1) {
            CHECK(r.kind == KM_CTRL_STALL);
        }
        if (r.kind == KM_CTRL_IN) {
            CHECK(r.len <= s.w_length && r.data != NULL);
        }
        if (dev.variant != KM_USB_VARIANT_XINPUT) {
            dev.variant = KM_USB_VARIANT_XINPUT;
        }
    }
}

static void test_xinput(void)
{
    test_xinput_descriptors();
    test_xinput_pack();
    test_xinput_endpoints();
    test_xinput_out();
    test_xinput_guard_and_fifo();
    test_xinput_fuzz();
}

static void test_variants(void)
{
    test_walkers_catch_pad_mistakes();
    test_variant_plain_unchanged();
    test_variant_pad_descriptors();
    test_pad_report_layout();
    test_pad_sweep();
    test_variant_enumeration();
    test_pad_class_requests();
    test_pad_in_endpoint();
    test_fallback_guard();
    test_fallback_drill();
    test_fifo_plan_and_hwcfg();
    test_fuzz_pad();
    test_xinput();
}

/* No matter what the host sends, the result stays inside what the driver can safely execute. */
static void test_fuzz(void)
{
    km_usb_dev_t dev;
    memset(&dev, 0, sizeof dev);
    km_usb_dev_reset(&dev);
    size_t longest = 0, n;
    km_usb_config_desc(&n);
    longest = n;
    for (unsigned i = 0; i < KM_USB_NUM_IF; i++) {
        km_usb_report_desc(i, &n);
        longest = n > longest ? n : longest;
    }
    uint32_t x = 12345u;
    for (unsigned k = 0; k < 200000; k++) {
        x = x * 1664525u + 1013904223u;
        uint8_t raw[8];
        for (unsigned i = 0; i < 8; i++) {
            x = x * 1664525u + 1013904223u;
            raw[i] = (uint8_t)(x >> 24);
        }
        if ((k & 3u) == 0) {              /* bias toward real request types and small indices */
            raw[0] = (uint8_t)(raw[0] & 0xA3u);
            raw[1] &= 0x0Fu;
            raw[5] = 0;
        }
        km_usb_setup_t s;
        km_ctrl_result_t r;
        km_usb_setup_parse(raw, &s);
        km_usb_ctrl_handle(&dev, &s, &r);
        CHECK(r.kind <= KM_CTRL_OUT);
        if (r.kind == KM_CTRL_IN) {
            CHECK(r.data != NULL && r.len >= 1 && r.len <= s.w_length && r.len <= longest);
            CHECK((s.bm_request_type & 0x80u) != 0);
        }
        if (r.kind == KM_CTRL_OUT) {
            CHECK(r.len >= 1 && r.len <= 64 && r.len == s.w_length);
            CHECK((s.bm_request_type & 0x80u) == 0);
            uint8_t blob[80];                 /* a data stage of random size and content, in random pieces */
            unsigned have = 0, total;
            x = x * 1664525u + 1013904223u;
            total = (x >> 24) % 80u;
            for (unsigned i = 0; i < total; i++) {
                x = x * 1664525u + 1013904223u;
                blob[i] = (uint8_t)(x >> 24);
            }
            while (have < total) {
                x = x * 1664525u + 1013904223u;
                unsigned c = 1u + (x >> 24) % 70u;
                c = c > total - have ? total - have : c;
                km_usb_ctrl_out_push(&dev, blob + have, c);
                have += c;
            }
            (void)km_usb_ctrl_out_end(&dev);
        }
        CHECK((dev.host_leds & ~KM_USB_LED_MASK) == 0);
        if (r.kind != KM_CTRL_ACK) {
            CHECK(r.apply == KM_CTRL_APPLY_NONE);
        }
        if (r.apply == KM_CTRL_APPLY_ADDRESS) {
            CHECK(r.value <= 127);
        }
        if (r.apply == KM_CTRL_APPLY_CONFIGURATION) {
            CHECK(r.value <= 1 && dev.configuration == r.value);
        }
        CHECK(dev.configuration <= 1);
        CHECK(dev.protocol[1] == 1 && dev.protocol[2] == 1 && dev.protocol[0] <= 1);
        CHECK(dev.idle[1] == 0);
    }
}

/* Polling intervals as the host reads them from the raw configuration bytes (LATENCY_NOTES.md section 4): high speed, period =
 * 2^(bInterval-1) micro-frames of 125 us. The keyboard endpoints (boot keyboard, NKRO) follow KM_USB_HS_8KHZ; the host tools' channel
 * and the gamepad stay at 1 ms in every build. */
static void test_hs_polling_intervals(void)
{
    CHECK_EQ(125u * (1u << (KM_USB_BINTERVAL_125US - 1u)), 125);        /* 1 -> 125 us = 8 kHz */
    CHECK_EQ(125u * (1u << (KM_USB_BINTERVAL_1MS - 1u)), 1000);         /* 4 -> 1 ms = 1 kHz */
    CHECK_EQ(KM_USB_BINTERVAL_VND, KM_USB_BINTERVAL_1MS);
    CHECK_EQ(KM_USB_BINTERVAL_PAD, KM_USB_BINTERVAL_1MS);
    for (int v = 0; v < 2; v++) {
        size_t n = 0;
        const uint8_t *c = km_usb_config_desc_for(v == 0 ? KM_USB_VARIANT_PLAIN : KM_USB_VARIANT_PAD, &n);
        unsigned seen = 0;
        for (size_t i = 0; i + 1 < n && c[i] != 0; i += c[i]) {
            if (c[i] == 7 && c[i + 1] == 0x05) {                         /* endpoint descriptor: address at +2, bInterval at +6 */
                const unsigned addr = c[i + 2];
                const unsigned bi = c[i + 6];
                seen++;
                if (addr == KM_USB_EP_KBD_IN) {
                    CHECK_EQ(bi, KM_USB_HS_8KHZ ? 1 : 4);
                } else if (addr == KM_USB_EP_NKRO_IN) {
                    CHECK_EQ(bi, KM_USB_HS_8KHZ ? 1 : 4);
                } else {
                    CHECK_EQ(bi, 4);                                    /* vendor in / out and the gamepad */
                }
            }
        }
        CHECK_EQ(seen, v == 0 ? 4 : 5);                                 /* kbd, vendor in+out, nkro (+ pad) */
    }
}

void test_usb(void)
{
    test_hs_polling_intervals();
    test_device_descriptor();
    test_report_descriptors();
    test_config_descriptor();
    test_strings();
    test_walkers_catch_mistakes();
    test_enumeration();
    test_hid_class_requests();
    test_host_leds();
    test_in_endpoints();
    test_idle_repeat();
    test_fuzz();
    test_variants();
}
