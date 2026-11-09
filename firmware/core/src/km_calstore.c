#include "km/km_calstore.h"

#include <string.h>

#include "km/km_settings.h"   /* km_crc32 */

#define CHUNK 32u             /* read / compare chunk, on the stack */

enum { P_IDLE = 0, P_ERASE, P_PAYLOAD, P_SEQ, P_VERIFY };
enum { S_BLANK = 0, S_VALID, S_DIRTY, S_READ_ERR };

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* a is newer than b (sequence numbers wrap) */
static bool seq_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static bool all_ff(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

bool km_clog_init(km_clog_t *log, const km_store_ops_t *ops, uint32_t sector, uint32_t payload_len)
{
    if (log == NULL || ops == NULL || payload_len == 0u || payload_len > KM_CLOG_MAX_PAYLOAD ||
        (sector & (KM_CLOG_SECTOR_BYTES - 1u)) != 0u) {
        return false;
    }
    log->ops = *ops;
    log->sector = sector;
    log->payload_len = payload_len;
    /* Slots start on a 16-byte flash line: the controller refuses a unit programmed below an already programmed unit of
     * its line, and a slot whose header started in the upper half of a line would have its header unit below payload. */
    log->stride = KM_CLOG_HEADER_BYTES + ((payload_len + KM_CLOG_HEADER_BYTES - 1u) / KM_CLOG_HEADER_BYTES) * KM_CLOG_HEADER_BYTES;
    log->slots = KM_CLOG_SECTOR_BYTES / log->stride;
    return log->slots != 0u;
}

/* Read [addr, addr + n) in chunks and check that every byte is 0xFF. */
static int region_blank(const km_clog_t *log, uint32_t addr, uint32_t n, bool *blank)
{
    uint8_t buf[CHUNK];
    *blank = true;
    for (uint32_t off = 0; off < n; off += CHUNK) {
        const uint32_t c = (n - off < CHUNK) ? n - off : CHUNK;
        if (!log->ops.read(log->ops.ctx, addr + off, buf, c)) {
            return 0;
        }
        if (!all_ff(buf, c)) {
            *blank = false;
            return 1;
        }
    }
    return 1;
}

static int classify(const km_clog_t *log, uint32_t slot, uint32_t *seq)
{
    const uint32_t base = log->sector + slot * log->stride;
    uint8_t h[KM_CLOG_HEADER_BYTES];
    if (!log->ops.read(log->ops.ctx, base, h, sizeof h)) {
        return S_READ_ERR;
    }
    if (get32(h) == KM_CLOG_MAGIC && get32(h + 12) == log->payload_len) {
        uint32_t crc = km_crc32(0, h + 8, 8);
        uint8_t buf[CHUNK];
        for (uint32_t off = 0; off < log->payload_len; off += CHUNK) {
            const uint32_t c = (log->payload_len - off < CHUNK) ? log->payload_len - off : CHUNK;
            if (!log->ops.read(log->ops.ctx, base + KM_CLOG_HEADER_BYTES + off, buf, c)) {
                return S_READ_ERR;
            }
            crc = km_crc32(crc, buf, c);
        }
        if (crc == get32(h + 4)) {
            *seq = get32(h + 8);
            return S_VALID;
        }
    }
    if (!all_ff(h, sizeof h)) {
        return S_DIRTY;
    }
    bool blank = true;
    if (!region_blank(log, base + KM_CLOG_HEADER_BYTES, log->stride - KM_CLOG_HEADER_BYTES, &blank)) {
        return S_READ_ERR;
    }
    return blank ? S_BLANK : S_DIRTY;
}

bool km_clog_scan(const km_clog_t *log, km_clog_scan_t *out)
{
    km_clog_scan_t s;
    memset(&s, 0, sizeof s);
    uint32_t last_used = 0;           /* 1 + index of the last non-blank slot */
    for (uint32_t i = 0; i < log->slots; i++) {
        uint32_t seq = 0;
        switch (classify(log, i, &seq)) {
        case S_READ_ERR:
            return false;
        case S_VALID:
            s.used++;
            last_used = i + 1u;
            if (!s.valid || seq_newer(seq, s.seq)) {
                s.valid = true;
                s.index = i;
                s.seq = seq;
            }
            break;
        case S_DIRTY:
            s.dirty++;
            last_used = i + 1u;
            break;
        default:
            break;
        }
    }
    s.next = last_used;
    *out = s;
    return true;
}

static bool payload_equals(const km_clog_t *log, uint32_t slot, const uint8_t *data)
{
    uint8_t buf[CHUNK];
    const uint32_t base = log->sector + slot * log->stride + KM_CLOG_HEADER_BYTES;
    for (uint32_t off = 0; off < log->payload_len; off += CHUNK) {
        const uint32_t c = (log->payload_len - off < CHUNK) ? log->payload_len - off : CHUNK;
        if (!log->ops.read(log->ops.ctx, base + off, buf, c) || memcmp(buf, data + off, c) != 0) {
            return false;
        }
    }
    return true;
}

bool km_clog_read(const km_clog_t *log, uint8_t *buf, uint32_t cap)
{
    km_clog_scan_t s;
    if (log == NULL || buf == NULL || cap < log->payload_len || !km_clog_scan(log, &s) || !s.valid) {
        return false;
    }
    return log->ops.read(log->ops.ctx, log->sector + s.index * log->stride + KM_CLOG_HEADER_BYTES, buf, log->payload_len);
}

/* ---- incremental job ---- */

static void skip_blank_units(km_clog_job_t *job)
{
    /* Programming 0xFF changes nothing, but only whole 16-byte lines are skipped: a blank lower unit still has to be
     * programmed when the upper unit of its line carries data (the controller refuses an upper unit whose lower unit was
     * never programmed, seen on the hardware). pos is always at a line start here (header 16 bytes, stride a multiple of 16). */
    while (job->pos < job->log->stride) {
        if ((job->pos % KM_CLOG_LINE) == 0u && job->pos + KM_CLOG_LINE <= job->log->stride && all_ff(job->image + job->pos, KM_CLOG_LINE)) {
            job->pos += KM_CLOG_LINE;                  /* a blank line */
        } else if ((job->pos % KM_CLOG_LINE) != 0u && all_ff(job->image + job->pos, KM_CLOG_UNIT)) {
            job->pos += KM_CLOG_UNIT;                  /* a blank upper unit */
        } else {
            break;
        }
    }
}

bool km_clog_job_active(const km_clog_job_t *job)
{
    return job->phase != P_IDLE;
}

bool km_clog_job_must_finish(const km_clog_job_t *job)
{
    return job->phase != P_IDLE && job->erased;
}

bool km_clog_job_next_is_erase(const km_clog_job_t *job)
{
    return job->phase == P_ERASE;
}

void km_clog_job_abort(km_clog_job_t *job)
{
    job->phase = P_IDLE;
}

km_clog_result_t km_clog_job_begin(km_clog_job_t *job, const km_clog_t *log, const uint8_t *data)
{
    if (job == NULL || log == NULL || data == NULL) {
        return KM_CLOG_ERR_ARG;
    }
    job->phase = P_IDLE;
    job->erased = false;
    km_clog_scan_t s;
    if (!km_clog_scan(log, &s)) {
        return KM_CLOG_ERR_READ;
    }
    if (s.valid && payload_equals(log, s.index, data)) {
        return KM_CLOG_UNCHANGED;
    }
    job->log = log;
    job->seq = s.valid ? s.seq + 1u : 1u;
    memset(job->image, 0xFF, log->stride);
    memcpy(job->image + KM_CLOG_HEADER_BYTES, data, log->payload_len);
    put32(job->image + 8, job->seq);
    put32(job->image + 12, log->payload_len);
    put32(job->image, KM_CLOG_MAGIC);
    put32(job->image + 4, km_crc32(km_crc32(0, job->image + 8, 8), data, log->payload_len));
    if (s.next >= log->slots) {
        job->slot = 0;
        job->phase = P_ERASE;
    } else {
        job->slot = s.next;
        job->pos = KM_CLOG_HEADER_BYTES;
        skip_blank_units(job);
        job->phase = job->pos < log->stride ? P_PAYLOAD : P_SEQ;
    }
    return KM_CLOG_PENDING;
}

km_clog_result_t km_clog_job_step(km_clog_job_t *job)
{
    const km_clog_t *log = job->log;
    const uint32_t base = log == NULL ? 0u : log->sector + job->slot * log->stride;
    km_clog_result_t r = KM_CLOG_PENDING;
    switch (job->phase) {
    case P_ERASE:
        if (!log->ops.erase(log->ops.ctx, log->sector)) {
            r = KM_CLOG_ERR_ERASE;
            break;
        }
        job->erased = true;
        job->pos = KM_CLOG_HEADER_BYTES;
        skip_blank_units(job);
        job->phase = job->pos < log->stride ? P_PAYLOAD : P_SEQ;
        break;
    case P_PAYLOAD:
        /* Both units of a 16-byte flash line go out in the same step, lower first, back to back: on the keyboard a line whose
         * second unit is programmed after the main loop has run in between is refused (programming error). */
        if (!log->ops.program(log->ops.ctx, base + job->pos, job->image + job->pos)) {
            r = KM_CLOG_ERR_PROGRAM;
            break;
        }
        job->pos += KM_CLOG_UNIT;
        if ((job->pos % KM_CLOG_LINE) != 0u && job->pos < log->stride && !all_ff(job->image + job->pos, KM_CLOG_UNIT)) {
            if (!log->ops.program(log->ops.ctx, base + job->pos, job->image + job->pos)) {
                r = KM_CLOG_ERR_PROGRAM;
                break;
            }
            job->pos += KM_CLOG_UNIT;
        }
        skip_blank_units(job);
        if (job->pos >= log->stride) {
            job->phase = P_SEQ;
        }
        break;
    case P_SEQ:
        /* The flash controller rejects a unit programmed below an already programmed unit of the same 16-byte line,
         * so the header goes in ascending order, both units in this one step: magic + CRC first, then the sequence + length unit. The CRC covers the
         * sequence and length, so the slot is not valid (and a torn one is skipped) until the second unit is in. */
        if (!log->ops.program(log->ops.ctx, base, job->image)) {      /* magic + CRC */
            r = KM_CLOG_ERR_PROGRAM;
            break;
        }
        if (!log->ops.program(log->ops.ctx, base + 8u, job->image + 8)) {   /* sequence + length, same line, straight after: the record exists from here on */
            r = KM_CLOG_ERR_PROGRAM;
            break;
        }
        job->phase = P_VERIFY;
        break;
    case P_VERIFY: {
        uint8_t buf[CHUNK];
        bool same = true;
        for (uint32_t off = 0; off < log->stride && same; off += CHUNK) {
            const uint32_t c = (log->stride - off < CHUNK) ? log->stride - off : CHUNK;
            if (!log->ops.read(log->ops.ctx, base + off, buf, c)) {
                r = KM_CLOG_ERR_READ;
                same = false;
            } else if (memcmp(buf, job->image + off, c) != 0) {
                r = KM_CLOG_ERR_VERIFY;
                same = false;
            }
        }
        uint32_t seq = 0;
        if (same && (classify(log, job->slot, &seq) != S_VALID || seq != job->seq)) {
            r = KM_CLOG_ERR_VERIFY;
            same = false;
        }
        if (same) {
            r = KM_CLOG_OK;
        }
        job->phase = P_IDLE;
        return r;
    }
    default:
        return KM_CLOG_ERR_ARG;
    }
    if (r != KM_CLOG_PENDING) {
        job->phase = P_IDLE;
    }
    return r;
}

km_clog_result_t km_clog_append(const km_clog_t *log, const uint8_t *data)
{
    km_clog_job_t job;
    km_clog_result_t r = km_clog_job_begin(&job, log, data);
    while (r == KM_CLOG_PENDING) {
        r = km_clog_job_step(&job);
    }
    return r;
}
