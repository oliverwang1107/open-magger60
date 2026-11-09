#!/usr/bin/env python3
"""magger60_noise.py - measure the REAL sensor noise of the Magger60 with the diagnostic image (KMFW-NOISE), M6 task P12a.

The diagnostic image (firmware/port/n32h48x/noise_main.c, built as km_noise) behaves like the daily L3-0 firmware (typing
works) and can additionally record the RAW 12-bit ADC sample of up to 4 slots, frame by frame (about 23 kHz), into a RAM buffer
(firmware/core/include/km/km_capture.h). This tool arms that capture, reads it back through the read-only scope channel
(capture ops of firmware/hid/include/km/km_scope.h), saves it and reports, per key: standard deviation, peak to peak,
spikes, drift and a spectrum summary. It talks to the "KMFW-NOISE" image only; any other firmware is never sent a capture op.
Everything it can send is a cmd 3 query or a scope request (the guard of magger60_scope.py): no flash, no writes.

    python tools/magger60_noise.py --self-test              # no hardware: protocol, file format and statistics checks
    python tools/magger60_noise.py --list                   # the scenarios and what the person at the keyboard does
    python tools/magger60_noise.py --sim --scenario A       # a scenario against a SIMULATED keyboard (demo, no hardware)
    python tools/magger60_noise.py --run --scenario A --out noise_data     # the real keyboard (Windows, KMFW-NOISE image only)
    python tools/magger60_noise.py --analyze noise_data/A_idle_all          # report again from saved files

Files: one capture = one *.kmtr (binary, format below, also read by firmware/tests/filter_lab.c) + one *.json (what, when,
conditions); slow logs are *.csv. See firmware/core/NOISE_NOTES.md for the scenario list and what to do at the keyboard.

KMTR v1 (little endian): "KMTR", u16 version=1, u16 n_cols, u32 n_frames, f32 sample_rate_hz, u16 flags (0), u16 reserved (0),
n_cols x u8 slot numbers padded with zeros to a multiple of 4, then n_frames x n_cols u16 samples, frame-major;
bits 0..11 = the raw ADC count, bit 15 = the condition flag of that frame (USB packet handled), bits 12..14 zero.
"""
import argparse
import cmath
import importlib.util
import json
import math
import random
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def _load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


bk = _load("magger60_backup")
scope = _load("magger60_scope")

NOISE_LABEL = "KMFW-NOISE"
VECTORS = REPO / "firmware/tests/golden/capture_vectors.h"

# ---- capture ops of the scope command (firmware/hid/include/km/km_scope.h) ----
CAP_STATUS = 0xC000
CAP_SELECT = 0xC100
CAP_CONTROL = 0xC200
CAP_PAGE = 0xC300
CTRL_STOP, CTRL_ONESHOT, CTRL_RING = 0, 1, 2
STATE_IDLE, STATE_RUNNING, STATE_DONE = 0, 1, 2
STATUS_WORDS = 20
PAGE_VALUES = scope.PAGE_VALUES                 # 25
CAPTURE_SAMPLES = 16384
NO_SLOT = 0xFF
FLAG = 0x8000
VALUE_MASK = 0x0FFF
(ST_VERSION, ST_STATE, ST_NSEL, ST_SLOT0, ST_RUN, ST_CAP_FRAMES, ST_FRAMES_LO, ST_FRAMES_HI, ST_AVAIL, ST_T_FIRST_LO, ST_T_FIRST_HI,
 ST_T_LAST_LO, ST_T_LAST_HI, ST_COND_LO, ST_COND_HI, ST_RING, ST_CPU_MHZ) = (0, 1, 2, 3, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19)


class NoiseError(Exception):
    """The keyboard did not answer as the capture protocol says, or it is not the diagnostic image."""


# --------------------------------------------------------------------------- packets and replies

def cap_request(op_tag: int, start: int = 0, length: int = bk.MAX_CHUNK) -> bytes:
    """A scope request with a capture-op tag. Same packet as every scope request: no payload, 'U', no XOR."""
    if not 0xC000 <= op_tag <= 0xCFFF:
        raise bk.SafetyError("not a capture tag")
    return scope.build_request(start, length, op_tag)


def page_request(page: int, length: int = bk.MAX_CHUNK) -> bytes:
    if not 0 <= page <= 0xFFFF:
        raise bk.SafetyError("page out of range")
    return cap_request(CAP_PAGE | (page >> 8), page & 0xFF, length)


class CapReply:
    def __init__(self, tag, run, count, start, values):
        self.tag, self.run, self.count, self.start, self.values = tag, run, count, start, values


def decode_cap_reply(resp: bytes) -> CapReply:
    """Header tag, run counter, count (LE16 each), then LE16 values; the offset field echoes `start`. Unlike the snapshot
    reply, count may exceed 128 (a page read: all readable values)."""
    if len(resp) != bk.REPORT_LEN:
        raise NoiseError("reply is not 64 bytes")
    if resp[0] == 0xAB:
        raise NoiseError("the keyboard reports a checksum error (0xAB)")
    if resp[0] != 0xAA or resp[1] != scope.CMD_SCOPE:
        raise NoiseError("unexpected reply: %s" % resp[:16].hex())
    n = resp[4]
    if n < scope.HEADER_BYTES or n > bk.MAX_CHUNK or n % 2:
        raise NoiseError("bad reply length %d" % n)
    data = resp[8:8 + n]
    if (n + resp[5] + resp[6] + resp[7] + sum(data)) & 0xFF != resp[3]:
        raise NoiseError("reply checksum does not match")
    if any(resp[8 + n:]):
        raise NoiseError("reply has data beyond its length")
    tag = data[0] | (data[1] << 8)
    run = data[2] | (data[3] << 8)
    count = data[4] | (data[5] << 8)
    vals = [data[i] | (data[i + 1] << 8) for i in range(scope.HEADER_BYTES, n, 2)]
    return CapReply(tag, run, count, resp[5], vals)


class Status:
    def __init__(self, words):
        if len(words) < STATUS_WORDS:
            raise NoiseError("status block has %d words, expected %d" % (len(words), STATUS_WORDS))
        w = list(words)
        if w[ST_VERSION] != 1:
            raise NoiseError("capture status version %d is not known" % w[ST_VERSION])
        self.state = w[ST_STATE]
        self.n_sel = w[ST_NSEL]
        self.slots = [s for s in w[ST_SLOT0:ST_SLOT0 + 4] if s != NO_SLOT][:self.n_sel]
        self.run = w[ST_RUN]
        self.cap_frames = w[ST_CAP_FRAMES]
        self.frames = w[ST_FRAMES_LO] | (w[ST_FRAMES_HI] << 16)
        self.avail = w[ST_AVAIL]
        self.t_first = w[ST_T_FIRST_LO] | (w[ST_T_FIRST_HI] << 16)
        self.t_last = w[ST_T_LAST_LO] | (w[ST_T_LAST_HI] << 16)
        self.cond_frames = w[ST_COND_LO] | (w[ST_COND_HI] << 16)
        self.ring = bool(w[ST_RING])
        self.cpu_mhz = w[ST_CPU_MHZ]

    def sample_rate_hz(self):
        """Frames per second from the cycle counter values of the first and the last recorded frame."""
        if self.avail < 2 or self.cpu_mhz == 0 or self.frames < 2:
            return 0.0
        dt = ((self.t_last - self.t_first) & 0xFFFFFFFF) / (self.cpu_mhz * 1e6)
        return (self.frames - 1) / dt if dt > 0 else 0.0


# --------------------------------------------------------------------------- client

class CaptureClient:
    def __init__(self, transport: bk.Transport):
        self.t = transport
        self.verified = False
        self.build = None
        self.requests = 0

    def verify(self) -> str:
        """cmd 3, and the build string must carry KMFW-NOISE. Until then no capture op is ever sent."""
        d = scope.info.decode(self.t.exchange(bk.build_request(bk.CMD_GET_INFO, 0, 0)))
        self.build = d["build"]
        if not scope.is_scope_build(self.build) or NOISE_LABEL not in self.build:
            raise NoiseError("this keyboard runs '%s', not the diagnostic image (the build string must carry %s); "
                             "no capture command was sent" % (self.build, NOISE_LABEL))
        self.verified = True
        return self.build

    def _ask(self, packet: bytes) -> CapReply:
        if not self.verified:
            raise NoiseError("firmware not verified yet; call verify() first")
        self.requests += 1
        r = decode_cap_reply(self.t.exchange(packet))
        want = packet[6] | (packet[7] << 8)
        if r.tag != want:
            raise NoiseError("the keyboard answered tag 0x%04X to a request for 0x%04X: the firmware has no capture "
                             "(is it the diagnostic image?)" % (r.tag, want))
        return r

    def status(self) -> Status:
        return Status(self._ask(cap_request(CAP_STATUS, 0)).values)

    def select(self, slots) -> Status:
        if not 1 <= len(slots) <= 4 or any(not 0 <= s < 76 for s in slots):
            raise bk.SafetyError("1..4 slots in 0..75")
        st = None
        for i in range(4):
            st = Status(self._ask(cap_request(CAP_SELECT | i, slots[i] if i < len(slots) else NO_SLOT)).values)
        if st.slots != list(slots):
            raise NoiseError("selection refused: asked %s, the keyboard has %s (state %d)" % (list(slots), st.slots, st.state))
        return st

    def control(self, ctrl: int) -> Status:
        return Status(self._ask(cap_request(CAP_CONTROL, ctrl)).values)

    def capture(self, slots, ring=False, timeout=5.0, busy=0.0, poll=0.02):
        """Select, arm, wait until the buffer is full (or `busy` seconds of status requests, then stop for a ring run), read back.
        Returns (Status, rows) with rows = list of frames, each a list of n_cols raw 12-bit counts, and flags = list of bool."""
        self.select(slots)
        st0 = self.control(CTRL_RING if ring else CTRL_ONESHOT)
        if st0.state != STATE_RUNNING:
            raise NoiseError("the capture did not start (state %d)" % st0.state)
        t0 = time.monotonic()
        if busy == 0:                                 # quiet condition: no packet at all while it records (23 kHz nominal)
            time.sleep(1.2 * st0.cap_frames / 23000.0)
        if busy > 0:                                  # USB-busy condition: a steady stream of requests while it records
            while time.monotonic() - t0 < busy:
                self.status()
        while True:
            st = self.status()
            if ring and st.state == STATE_RUNNING and time.monotonic() - t0 >= max(busy, 0.3):
                st = self.control(CTRL_STOP)
            if st.state == STATE_DONE:
                break
            if st.state == STATE_IDLE or time.monotonic() - t0 > timeout:
                raise NoiseError("the capture did not finish (state %d)" % st.state)
            time.sleep(poll)
        vals = self.read_values(st)
        cols = st.n_sel
        rows = [vals[f * cols:(f + 1) * cols] for f in range(st.avail)]
        return st, rows

    def read_values(self, st: Status):
        total = st.avail * st.n_sel
        out, page = [], 0
        while len(out) < total:
            r = self._ask(page_request(page))
            if r.run != st.run:
                raise NoiseError("the capture was restarted while it was read (run %d -> %d)" % (st.run, r.run))
            if r.count != total:
                raise NoiseError("value count changed while reading (%d -> %d)" % (total, r.count))
            if not r.values:
                raise NoiseError("empty page %d before the end (%d of %d values)" % (page, len(out), total))
            out += r.values
            page += 1
        if len(out) != total:
            raise NoiseError("read %d values, expected %d" % (len(out), total))
        return out

    def snapshot_counts(self):
        """The 76 sixteen-point sums / 16 (counts) of the ordinary scope tag 0x0003: the slow view, ~10 Hz."""
        # reuse the scope client's torn-read-safe snapshot
        c = scope.ScopeClient(self.t)
        c.verified = True
        last = None
        for _ in range(400):            # the image rotates its tags every 10 ms: ask again until ours is the one published
            try:
                snap = c.snapshot(scope.TAG_S3)
                return [v >> 4 for v in snap.values]
            except scope.ScopeError as e:
                last = e
                if "is not published" not in str(e):
                    raise
        raise last


# --------------------------------------------------------------------------- KMTR files

def write_kmtr(path, slots, rows, fs_hz):
    cols = len(slots)
    n = len(rows)
    head = bytearray(b"KMTR")
    head += struct.pack("<HHIfHH", 1, cols, n, float(fs_hz), 0, 0)
    head += bytes(slots) + bytes((-cols) % 4)
    body = bytearray()
    for r in rows:
        if len(r) != cols:
            raise ValueError("ragged frame")
        body += struct.pack("<%dH" % cols, *r)
    Path(path).write_bytes(bytes(head) + bytes(body))


def read_kmtr(path):
    """Returns (slots, rows, fs_hz). rows = list of frames of raw u16 (bit 15 = condition flag)."""
    b = Path(path).read_bytes()
    if b[:4] != b"KMTR":
        raise NoiseError("%s: not a KMTR file" % path)
    ver, cols, n, fs, flags, rsvd = struct.unpack_from("<HHIfHH", b, 4)
    if ver != 1:
        raise NoiseError("%s: KMTR version %d is not known" % (path, ver))
    hdr = 20 + cols + ((-cols) % 4)
    if len(b) != hdr + 2 * cols * n:
        raise NoiseError("%s: size does not match the header" % path)
    slots = list(b[20:20 + cols])
    vals = struct.unpack_from("<%dH" % (cols * n), b, hdr)
    return slots, [list(vals[f * cols:(f + 1) * cols]) for f in range(n)], fs


# --------------------------------------------------------------------------- statistics (pure Python, no numpy)

def mean(x):
    return sum(x) / len(x) if x else 0.0


def std(x):
    if len(x) < 2:
        return 0.0
    m = mean(x)
    return math.sqrt(sum((v - m) ** 2 for v in x) / (len(x) - 1))


def percentile(sorted_x, p):
    if not sorted_x:
        return 0.0
    k = (len(sorted_x) - 1) * p / 100.0
    lo = int(math.floor(k))
    hi = min(lo + 1, len(sorted_x) - 1)
    return sorted_x[lo] + (sorted_x[hi] - sorted_x[lo]) * (k - lo)


def median(x):
    return percentile(sorted(x), 50)


def fft(a):
    """Iterative radix-2 FFT of a list of complex numbers; len must be a power of two."""
    n = len(a)
    j = 0
    a = list(a)
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    length = 2
    while length <= n:
        w_len = cmath.exp(-2j * math.pi / length)
        for i in range(0, n, length):
            w = 1 + 0j
            half = length // 2
            for k in range(half):
                u, v = a[i + k], a[i + k + half] * w
                a[i + k], a[i + k + half] = u + v, u - v
                w *= w_len
        length <<= 1
    return a


def welch_psd(x, fs, nfft=4096):
    """One-sided power spectral density (counts^2 / Hz), Hann window, 50 % overlap. Returns (freqs, psd)."""
    n = len(x)
    nfft = min(nfft, 1 << (n.bit_length() - 1))
    if nfft < 16:
        return [], []
    m = mean(x)
    win = [0.5 - 0.5 * math.cos(2 * math.pi * i / nfft) for i in range(nfft)]
    wpow = sum(w * w for w in win)
    acc = [0.0] * (nfft // 2 + 1)
    segs = 0
    for s in range(0, n - nfft + 1, nfft // 2):
        seg = [(x[s + i] - m) * win[i] for i in range(nfft)]
        f = fft(seg)
        for k in range(nfft // 2 + 1):
            acc[k] += abs(f[k]) ** 2
        segs += 1
    scale = 1.0 / (fs * wpow * segs)
    psd = [v * scale * (1 if k in (0, nfft // 2) else 2) for k, v in enumerate(acc)]
    return [k * fs / nfft for k in range(nfft // 2 + 1)], psd


BANDS_HZ = ((0, 100), (100, 1000), (1000, 5000), (5000, 1e9))


def spectrum_summary(x, fs):
    """Share of the variance per band and the strong narrow lines (a line = a bin 8x above the median of its neighbourhood)."""
    freqs, psd = welch_psd(x, fs)
    if not freqs:
        return {"bands": [], "lines": []}
    df = freqs[1] - freqs[0]
    tot = sum(psd[1:]) * df
    bands = []
    for lo, hi in BANDS_HZ:
        part = sum(p for f, p in zip(freqs, psd) if lo <= f < hi and f > 0) * df
        bands.append(part / tot if tot > 0 else 0.0)
    lines = []
    for k in range(3, len(psd) - 3):
        nb = sorted(psd[max(1, k - 12):k - 1] + psd[k + 2:k + 13])
        base = nb[len(nb) // 2] if nb else 0
        if base > 0 and psd[k] > 8 * base and psd[k] >= psd[k - 1] and psd[k] >= psd[k + 1]:
            share = (psd[k - 1] + psd[k] + psd[k + 1]) * df / tot if tot > 0 else 0.0
            lines.append((freqs[k], psd[k] / base, share))
    lines.sort(key=lambda t: -t[2])
    return {"bands": bands, "lines": lines[:5], "resolution_hz": df}


def box_mean(x, n=16):
    """What the engine sees: the mean of the last n raw samples (km_scan sum / n), one value per frame."""
    out, acc = [], 0.0
    for i, v in enumerate(x):
        acc += v
        if i >= n:
            acc -= x[i - n]
        if i >= n - 1:
            out.append(acc / n)
    return out


def spikes_per_s(x, fs):
    """Isolated outliers: samples further than 6 robust sigma from the median of their 5-sample neighbourhood."""
    if len(x) < 8:
        return 0.0
    d = [abs(x[i] - x[i - 1]) for i in range(1, len(x))]
    sigma = 1.4826 * median(d) / math.sqrt(2) if d else 0.0
    sigma = max(sigma, 0.5)
    n = 0
    for i in range(2, len(x) - 2):
        if abs(x[i] - median(x[i - 2:i + 3])) > 6 * sigma:
            n += 1
    return n / (len(x) / fs)


def column_stats(x, fs, flags=None):
    """The numbers of one column of one capture (x = raw counts)."""
    x = [float(v) for v in x]
    s = sorted(x)
    box = box_mean(x, 16)
    sd = std(x)
    sd16 = std(box)
    ncol = {
        "n": len(x), "mean": mean(x), "sigma": sd,
        "p2p_999": percentile(s, 99.95) - percentile(s, 0.05), "range": s[-1] - s[0],
        "sigma_box16": sd16,
        # how white the noise is: a white signal gets /4 from a 16-sample mean; a ratio far above 1 = low-frequency noise the
        # averaging cannot remove
        "color_factor": (sd16 / (sd / 4.0)) if sd > 0 else 0.0,
        "spikes_per_s": spikes_per_s(x, fs) if fs > 0 else 0.0,
    }
    if fs > 0:
        ncol["spectrum"] = spectrum_summary(x, fs)
    if flags is not None and any(flags) and not all(flags):
        a = [v for v, f in zip(x, flags) if f]
        b = [v for v, f in zip(x, flags) if not f]
        # variance of the first differences, so that slow drift between the two groups does not count
        da = [abs(a[i] - a[i - 1]) for i in range(1, len(a))]
        db = [abs(b[i] - b[i - 1]) for i in range(1, len(b))]
        ncol["flag_vs_quiet"] = {"frames_flagged": len(a), "mean_abs_diff_flagged": mean(da), "mean_abs_diff_quiet": mean(db),
                                 "mean_shift": mean(a) - mean(b)}
    return ncol


def correlation_matrix(cols):
    """Pearson correlation of the columns of one capture, and the common-mode share: var(mean of columns) / mean(var)
    (1 = identical noise on every column, 1/n = independent)."""
    n = len(cols)
    m = [mean(c) for c in cols]
    sd = [std(c) for c in cols]
    mat = [[1.0 if i == j else 0.0 for j in range(n)] for i in range(n)]
    for i in range(n):
        for j in range(i + 1, n):
            if sd[i] > 0 and sd[j] > 0:
                cov = sum((a - m[i]) * (b - m[j]) for a, b in zip(cols[i], cols[j])) / (len(cols[i]) - 1)
                mat[i][j] = mat[j][i] = cov / (sd[i] * sd[j])
    avg = [mean([cols[i][f] - m[i] for i in range(n)]) for f in range(len(cols[0]))]
    mv = mean([v * v for v in sd])
    return mat, (std(avg) ** 2 / mv if mv > 0 else 0.0)


def trace_columns(rows):
    cols = len(rows[0])
    x = [[(r[c] & VALUE_MASK) for r in rows] for c in range(cols)]
    flags = [bool(r[0] & FLAG) for r in rows]
    return x, flags


def analyze_capture(slots, rows, fs):
    x, flags = trace_columns(rows)
    out = {"slots": slots, "fs_hz": fs, "frames": len(rows), "columns": [column_stats(c, fs, flags) for c in x]}
    if len(x) > 1:
        mat, cm = correlation_matrix(x)
        out["correlation"] = mat
        out["common_mode_share"] = cm
    return out


def linear_slope(ts, ys):
    n = len(ts)
    if n < 2:
        return 0.0
    mt, my = mean(ts), mean(ys)
    den = sum((t - mt) ** 2 for t in ts)
    return sum((t - mt) * (y - my) for t, y in zip(ts, ys)) / den if den > 0 else 0.0


# --------------------------------------------------------------------------- scenarios

def key_slot(name: str) -> int:
    """Slot of a key by the name of the measured slot map (case-insensitive, first word or whole name)."""
    want = name.strip().lower()
    for s, (n, _r, _c) in enumerate(scope.SLOTS):
        if n and (n.lower() == want or n.lower().split()[0] == want):
            return s
    raise ValueError("no key called '%s' in the measured slot map" % name)


def key_slots():
    """The 61 slots with a key, in slot order."""
    return [s for s, (n, _r, _c) in enumerate(scope.SLOTS) if n]


DEFAULT_KEYS = ("ESC", "G", "SPACE", "ENTER")             # corner, centre, bottom row, right edge
# neighbours of G on the keyboard rows (T, B, F, H) for the crosstalk scenario
NEIGHBOUR_TARGET = "G"
NEIGHBOUR_KEYS = ("F", "H", "T", "B")
FAR_TYPING_KEYS = ("ESC", "ENTER", "SPACE")             # idle keys while typing on the other half of the board


SCENARIOS = {
    "A": ("A_idle_all", "全部按鍵靜止（手離開鍵盤）：先 60 秒慢速記錄 76 個槽位，再逐組 4 顆快速擷取全部 61 顆鍵。回答：每鍵 σ、最壞/中位數比、雜訊是不是白的。"),
    "B": ("B_depth", "一顆鍵（預設 G）壓在不同深度各保持 30 秒（用墊片當硬擋最好）。回答：雜訊對深度的關係、量化後的深度抖動。"),
    "C": ("C_usb", "USB 忙碌對雜訊的影響：同樣 4 顆靜止鍵，擷取時主機不發任何封包 vs. 擷取時主機連續發請求。"),
    "D": ("D_typing", "敲別的鍵：D1 在遠處打字、D2 猛敲目標鍵 G 同時量它的四個鄰居，與靜止參考比較。回答：串擾與共模。"),
    "E": ("E_warmup", "暖機：剛插上鍵盤起算 30 分鐘慢速記錄（每秒一筆）＋第 0、5、10、20、30 分鐘各一次快速擷取。回答：漂移多快、基準追蹤夠不夠。"),
}


class UI:
    """Talks to the person at the keyboard. In tests and simulations nothing waits."""

    def __init__(self, interactive=True, out=print):
        self.interactive = interactive
        self.out = out

    def say(self, text):
        self.out(text)

    def wait(self, text):
        self.out(text)
        if self.interactive:
            input("   -> 準備好了就按 Enter ...")


def save_capture(outdir: Path, name: str, st: Status, rows, meta):
    fs = st.sample_rate_hz()
    outdir.mkdir(parents=True, exist_ok=True)
    write_kmtr(outdir / (name + ".kmtr"), st.slots, rows, fs)
    m = dict(meta)
    m.update({"slots": st.slots, "keys": [scope.SLOTS[s][0] for s in st.slots], "sample_rate_hz": fs, "frames": len(rows),
              "run": st.run, "cond_frames": st.cond_frames, "wall_time": time.strftime("%Y-%m-%d %H:%M:%S"),
              "unix_time": time.time()})
    (outdir / (name + ".json")).write_text(json.dumps(m, indent=1, ensure_ascii=False), encoding="utf-8")
    return fs


def slow_log(client, seconds, path, decimate=1, sleep=0.0, ui=None, label=""):
    """The ordinary scope sums (tag 0x0003) at ~10 Hz for `seconds`; one CSV row per `decimate` snapshots (mean of them)."""
    t0 = time.monotonic()
    rows, acc, k = [], [0.0] * 76, 0
    last_seq = None
    while time.monotonic() - t0 < seconds:
        vals = client.snapshot_counts()
        for i, v in enumerate(vals):
            acc[i] += v
        k += 1
        if k >= decimate:
            rows.append([round(time.monotonic() - t0, 3)] + [a / k for a in acc])
            acc, k = [0.0] * 76, 0
        if sleep:
            time.sleep(sleep)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("t_s," + ",".join("s%d" % i for i in range(76)) + "\n")
        for r in rows:
            fh.write(",".join(("%.3f" % v) if i == 0 else ("%.2f" % v) for i, v in enumerate(r)) + "\n")
    return rows


def sweep_groups(slots):
    return [slots[i:i + 4] for i in range(0, len(slots), 4)]


def run_A(client, outdir, ui, args):
    outdir.mkdir(parents=True, exist_ok=True)
    ui.wait("【A 靜止】手離開鍵盤，所有鍵都在原位。接下來 %.0f 秒慢速記錄，再逐組快速擷取全部 61 顆鍵（約 1 分鐘）。" % args.slow_seconds)
    slow_log(client, args.slow_seconds, outdir / "A_slow.csv", ui=ui)
    groups = sweep_groups(key_slots())
    for gi, g in enumerate(groups):
        st, rows = client.capture(g)
        save_capture(outdir, "A_fast_g%02d" % gi, st, rows, {"scenario": "A", "step": "idle sweep", "group": gi})
        ui.say("  組 %d/%d  %s" % (gi + 1, len(groups), " ".join(scope.SLOTS[s][0] for s in g)))


def run_B(client, outdir, ui, args):
    slot = key_slot(args.key)
    outdir.mkdir(parents=True, exist_ok=True)
    depths = [d.strip() for d in args.depths.split(",") if d.strip()]
    for di, label in enumerate(depths):
        ui.wait("【B 深度 %d/%d】把 %s 壓到「%s」並保持不動（用墊片當硬擋，手指穩穩壓住）。之後 %.0f 秒不要動。"
                % (di + 1, len(depths), args.key, label, args.hold_seconds))
        t0 = time.monotonic()
        k = 0
        while time.monotonic() - t0 < args.hold_seconds:
            st, rows = client.capture([slot])
            save_capture(outdir, "B_d%02d_seg%03d" % (di, k), st, rows, {"scenario": "B", "depth_label": label, "key": args.key})
            k += 1
        ui.say("  %s: %d 段" % (label, k))


def run_C(client, outdir, ui, args):
    slots = [key_slot(k) for k in args.keys.split(",")][:4]
    outdir.mkdir(parents=True, exist_ok=True)
    ui.wait("【C USB】手離開鍵盤。先做 %d 次『擷取時主機不發封包』，再做 %d 次『擷取時主機連續發請求』。" % (args.repeats, args.repeats))
    for cond, busy in (("quiet", 0.0), ("busy", 0.25)):
        for k in range(args.repeats):
            st, rows = client.capture(slots, busy=busy)
            save_capture(outdir, "C_%s_%03d" % (cond, k), st, rows, {"scenario": "C", "condition": cond, "busy_s": busy})
        ui.say("  %s 完成" % cond)


def run_D(client, outdir, ui, args):
    far = [key_slot(k) for k in FAR_TYPING_KEYS]
    nb = [key_slot(k) for k in NEIGHBOUR_KEYS]
    outdir.mkdir(parents=True, exist_ok=True)
    ui.wait("【D0 參考】手離開鍵盤，量 %d 次 %s（靜止參考）。" % (args.repeats, "/".join(FAR_TYPING_KEYS)))
    for k in range(args.repeats):
        st, rows = client.capture(far)
        save_capture(outdir, "D0_ref_%03d" % k, st, rows, {"scenario": "D", "condition": "idle reference", "keys": list(FAR_TYPING_KEYS)})
    ui.wait("【D1 遠處打字】在鍵盤『另一側』（例如 I O P K L 一帶）持續亂打字，不要碰 %s。量 %d 次。" % ("/".join(FAR_TYPING_KEYS), args.repeats))
    for k in range(args.repeats):
        st, rows = client.capture(far)
        save_capture(outdir, "D1_far_%03d" % k, st, rows, {"scenario": "D", "condition": "typing elsewhere"})
    ui.wait("【D2 鄰居】只猛敲 %s 這一顆（約每秒 5 次，敲到底），我量它的鄰居 %s。量 %d 次。"
            % (NEIGHBOUR_TARGET, "/".join(NEIGHBOUR_KEYS), args.repeats))
    for k in range(args.repeats):
        st, rows = client.capture(nb)
        save_capture(outdir, "D2_near_%03d" % k, st, rows, {"scenario": "D", "condition": "hammering %s" % NEIGHBOUR_TARGET})
    ui.wait("【D3 鄰居參考】停手，量 %d 次同樣 4 顆鄰居當對照。" % args.repeats)
    for k in range(args.repeats):
        st, rows = client.capture(nb)
        save_capture(outdir, "D3_nearref_%03d" % k, st, rows, {"scenario": "D", "condition": "neighbours idle reference"})


def run_E(client, outdir, ui, args):
    slots = [key_slot(k) for k in args.keys.split(",")][:4]
    outdir.mkdir(parents=True, exist_ok=True)
    ui.wait("【E 暖機】鍵盤要在『剛插上』的狀態（之前至少拔掉 1 小時最好）。手離開鍵盤，不要碰它 %.0f 分鐘。" % (args.warm_minutes))
    t0 = time.monotonic()
    marks = [m for m in (0, 5, 10, 20, 30) if m <= args.warm_minutes]
    rows_all = []
    done = set()
    while True:
        el = (time.monotonic() - t0) / 60.0
        if el >= args.warm_minutes:
            break
        for m in marks:
            if m not in done and el >= m:
                st, rows = client.capture(slots)
                save_capture(outdir, "E_t%02dmin" % m, st, rows, {"scenario": "E", "minute": m})
                done.add(m)
        vals = client.snapshot_counts()
        rows_all.append([round(time.monotonic() - t0, 2)] + vals)
        time.sleep(max(0.0, args.warm_interval))
    with open(outdir / "E_slow.csv", "w", encoding="utf-8") as fh:
        fh.write("t_s," + ",".join("s%d" % i for i in range(76)) + "\n")
        for r in rows_all:
            fh.write(",".join(("%.2f" % v) for v in r) + "\n")


RUNNERS = {"A": run_A, "B": run_B, "C": run_C, "D": run_D, "E": run_E}


# --------------------------------------------------------------------------- report

def load_dir(d: Path):
    caps = []
    for p in sorted(d.glob("*.kmtr")):
        slots, rows, fs = read_kmtr(p)
        meta = {}
        jp = p.with_suffix(".json")
        if jp.exists():
            meta = json.loads(jp.read_text(encoding="utf-8"))
        caps.append((p.stem, slots, rows, fs, meta))
    return caps


def fmt_band(b):
    return " ".join("%3.0f%%" % (100 * v) for v in b)


def report_dir(d: Path, out=print):
    caps = load_dir(d)
    if not caps:
        out("no *.kmtr files in %s" % d)
        return {}
    per_key = {}
    by_cond = {}
    results = {}
    for name, slots, rows, fs, meta in caps:
        res = analyze_capture(slots, rows, fs)
        results[name] = res
        for slot, cs in zip(slots, res["columns"]):
            per_key.setdefault(slot, []).append(cs)
        label = meta.get("condition") or meta.get("depth_label") or meta.get("step") or "all"
        by_cond.setdefault(label, []).append((name, slots, res, meta))
    out("== %s: %d captures ==" % (d, len(caps)))
    out("")
    out("%-4s %-8s %5s %8s %8s %8s %8s %7s %7s  %s" % ("slot", "key", "caps", "mean", "sigma", "p-p99.9", "sigma/16", "color", "spk/s", "variance share <100Hz|<1k|<5k|>5k"))
    sigmas = []
    for slot in sorted(per_key):
        cs = per_key[slot]
        sg = mean([c["sigma"] for c in cs])
        sigmas.append(sg)
        b = cs[0].get("spectrum", {}).get("bands", [])
        out("%-4d %-8s %5d %8.1f %8.2f %8.1f %8.2f %7.2f %7.1f  %s" % (
            slot, (scope.SLOTS[slot][0] or "-")[:8], len(cs), mean([c["mean"] for c in cs]), sg, mean([c["p2p_999"] for c in cs]),
            mean([c["sigma_box16"] for c in cs]), mean([c["color_factor"] for c in cs]), mean([c["spikes_per_s"] for c in cs]), fmt_band(b)))
    if sigmas:
        s = sorted(sigmas)
        med = percentile(s, 50)
        out("")
        out("sigma over keys: min %.2f  median %.2f  max %.2f  (worst / median = %.2f)" % (s[0], med, s[-1], s[-1] / med if med else 0))
    lines = {}
    for name, res in results.items():
        for cs in res["columns"]:
            for f, ratio, share in cs.get("spectrum", {}).get("lines", []):
                key = round(f / 25.0) * 25
                lines.setdefault(key, []).append(share)
    if lines:
        out("")
        out("narrow lines seen (Hz, in how many column-captures, mean variance share):")
        for f, sh in sorted(lines.items(), key=lambda kv: -len(kv[1]))[:8]:
            out("  %7.0f Hz  %3d  %.1f %%" % (f, len(sh), 100 * mean(sh)))
    if len(by_cond) > 1:
        out("")
        out("by condition (mean over captures and columns):")
        out("%-28s %5s %9s %9s %9s %8s" % ("condition", "caps", "sigma", "p-p99.9", "spikes/s", "common"))
        for label, items in by_cond.items():
            cols = [c for (_n, _s, r, _m) in items for c in r["columns"]]
            cm = [r.get("common_mode_share") for (_n, _s, r, _m) in items if r.get("common_mode_share") is not None]
            out("%-28s %5d %9.2f %9.1f %9.2f %8s" % (label[:28], len(items), mean([c["sigma"] for c in cols]), mean([c["p2p_999"] for c in cols]),
                                                     mean([c["spikes_per_s"] for c in cols]), ("%.2f" % mean(cm)) if cm else "-"))
    # drift from saved slow logs
    for csvp in sorted(d.glob("*_slow.csv")):
        ts, cols = [], {}
        for ln in csvp.read_text(encoding="utf-8").splitlines()[1:]:
            parts = ln.split(",")
            ts.append(float(parts[0]))
            for i, v in enumerate(parts[1:]):
                cols.setdefault(i, []).append(float(v))
        keyed = [s for s in key_slots() if s in cols]
        if ts and keyed:
            slopes = sorted(linear_slope(ts, cols[s]) * 60.0 for s in keyed)
            sds = sorted(std(cols[s]) for s in keyed)
            out("")
            out("%s: %d rows, %.0f s. slow sigma (sum/16, ~10 Hz): min %.2f median %.2f max %.2f counts; drift per minute: min %.2f median %.2f max %.2f counts"
                % (csvp.name, len(ts), ts[-1] - ts[0], sds[0], percentile(sds, 50), sds[-1], slopes[0], percentile(slopes, 50), slopes[-1]))
    return results


# --------------------------------------------------------------------------- simulated keyboard (an independent model of km_capture + km_scope)

def synth_column(slot, n, fs, seed, cond=None, white=20.0, slow=4.0, tone_hz=0.0, tone_amp=0.0, spike_rate=0.0):
    """Deterministic fake raw samples for a slot: baseline + white noise + an AR(1) slow component + a tone + rare spikes."""
    rng = random.Random(seed * 1000 + slot)
    base = 3300 - (slot * 7) % 200
    a = math.exp(-1.0 / 2000.0)                 # correlation time ~2000 frames (~85 ms)
    s_sd = slow * math.sqrt(1 - a * a)
    lf = 0.0
    out = []
    for i in range(n):
        lf = a * lf + rng.gauss(0, s_sd)
        v = base + rng.gauss(0, white) + lf
        if tone_amp:
            v += tone_amp * math.sin(2 * math.pi * tone_hz * i / fs)
        if spike_rate and rng.random() < spike_rate:
            v += rng.choice((-1, 1)) * rng.uniform(60, 120)
        out.append(min(4095, max(0, int(round(v)))))
    return out


class SimulatedNoiseKeyboard(bk.Transport):
    """Follows the protocol of km_scope.h / km_capture.h: cmd 3 answers a KMFW-NOISE build, capture ops, snapshot 0x0003."""

    OUR_BUILD = "Jan  1 2026,00:00:00KMFW-NOISE-SIM"

    def __init__(self, build=None, frame_cycles=10300, cpu_mhz=240, white=20.0, slow=4.0, tone_hz=1000.0, tone_amp=0.0, spike_rate=0.0,
                 busy_extra=0.0, run_seconds=0.01):
        self.build = build or self.OUR_BUILD
        self.frame_cycles, self.cpu_mhz = frame_cycles, cpu_mhz
        self.fs = cpu_mhz * 1e6 / frame_cycles
        self.model = dict(white=white, slow=slow, tone_hz=tone_hz, tone_amp=tone_amp, spike_rate=spike_rate)
        self.busy_extra = busy_extra            # extra white sigma on frames flagged "USB busy"
        self.run_seconds = run_seconds            # how long a simulated capture takes (wall clock)
        self.t_arm = 0.0
        self.sel = [NO_SLOT] * 4
        self.state, self.ring, self.run = STATE_IDLE, 0, 0
        self.frames, self.cond_frames = 0, 0
        self.buf = []
        self.polls = 0
        self.seq = 0
        self.seed = 1
        self.busy_requests = 0
        self.seen = []
        self.wrote_anything = False
        self.snapshots = True

    def _nsel(self):
        n = 0
        while n < 4 and self.sel[n] != NO_SLOT:
            n += 1
        return n

    def _cap_frames(self):
        n = self._nsel()
        return CAPTURE_SAMPLES // n if n else 0

    def _status_words(self):
        n = self._nsel()
        frames = self.frames
        t_first = 1000 if frames else 0
        t_last = t_first + (frames - 1) * self.frame_cycles if frames else 0
        w = [0] * STATUS_WORDS
        w[ST_VERSION] = 1
        w[ST_STATE] = self.state
        w[ST_NSEL] = n
        w[ST_SLOT0:ST_SLOT0 + 4] = self.sel
        w[ST_RUN] = self.run
        w[ST_CAP_FRAMES] = self._cap_frames()
        w[ST_FRAMES_LO], w[ST_FRAMES_HI] = frames & 0xFFFF, frames >> 16
        w[ST_AVAIL] = min(frames, self._cap_frames())
        w[ST_T_FIRST_LO], w[ST_T_FIRST_HI] = t_first & 0xFFFF, t_first >> 16
        w[ST_T_LAST_LO], w[ST_T_LAST_HI] = t_last & 0xFFFF, (t_last >> 16) & 0xFFFF
        w[ST_COND_LO], w[ST_COND_HI] = self.cond_frames & 0xFFFF, self.cond_frames >> 16
        w[ST_RING] = self.ring
        w[ST_CPU_MHZ] = self.cpu_mhz
        return w

    def _record(self):
        n = self._nsel()
        cap = self._cap_frames()
        frames = cap if not self.ring else cap
        self.seed += 1
        busy = self.busy_requests >= 4            # the host kept asking while it recorded
        cols = []
        for s in self.sel[:n]:
            m = dict(self.model)
            c = synth_column(s, frames, self.fs, self.seed, **m)
            if busy and self.busy_extra:
                rng = random.Random(self.seed)
                c = [min(4095, max(0, int(round(v + rng.gauss(0, self.busy_extra))))) for v in c]
            cols.append(c)
        flag = FLAG if busy else 0
        self.buf = [[(cols[c][f] & VALUE_MASK) | flag for c in range(n)] for f in range(frames)]
        self.frames = frames
        self.cond_frames = frames if busy else 0

    def _values(self):
        if self.state == STATE_RUNNING:
            return []
        return [v for fr in self.buf[:min(self.frames, self._cap_frames())] for v in fr]

    @staticmethod
    def _reply(data, offset, cmd=scope.CMD_SCOPE):
        r = bytearray(bk.REPORT_LEN)
        r[0], r[1] = 0xAA, cmd
        r[3] = (len(data) + offset + sum(data)) & 0xFF
        r[4] = len(data)
        r[5] = offset
        r[8:8 + len(data)] = data
        return bytes(r)

    def _cap_reply(self, packet, tag, run, count, vals):
        n = max(0, min((packet[4] - 6) // 2 if packet[4] >= 6 else 0, PAGE_VALUES, len(vals)))
        data = bytearray([tag & 0xFF, tag >> 8, run & 0xFF, run >> 8, count & 0xFF, count >> 8])
        for v in vals[:n]:
            data += bytes([v & 0xFF, v >> 8])
        return self._reply(bytes(data), packet[5])

    def exchange(self, packet: bytes) -> bytes:
        packet = scope.guard_outgoing(packet)
        cmd = packet[1]
        self.seen.append(cmd)
        if cmd == bk.CMD_GET_INFO:
            return self._reply(bytes([0x04, 0x01]) + self.build.encode("ascii"), 0, bk.CMD_GET_INFO)
        start, tag = packet[5], packet[6] | (packet[7] << 8)
        if 0xC000 <= tag <= 0xCFFF and "NOISE" in self.build:
            op = (tag >> 8) & 0xF
            if op == 0:
                self.polls += 1
                self.busy_requests += 1
                if self.state == STATE_RUNNING and time.monotonic() - self.t_arm >= self.run_seconds:
                    if self.ring:
                        pass
                    else:
                        self._record()
                        self.state = STATE_DONE
                return self._cap_reply(packet, tag, self.run, STATUS_WORDS, self._status_words())
            if op == 1:
                i = tag & 0xFF
                if self.state != STATE_RUNNING and i < 4 and (start < 76 or start == NO_SLOT):
                    self.sel[i] = start
                    self.state, self.frames, self.cond_frames = STATE_IDLE, 0, 0
                return self._cap_reply(packet, tag, self.run, STATUS_WORDS, self._status_words())
            if op == 2:
                if start == CTRL_STOP:
                    if self.state == STATE_RUNNING:
                        if self.ring:
                            self._record()
                        self.state = STATE_DONE if self.frames else STATE_IDLE
                elif start in (CTRL_ONESHOT, CTRL_RING) and self._nsel():
                    self.ring = 1 if start == CTRL_RING else 0
                    self.frames = self.cond_frames = 0
                    self.run = self.run % 0xFFFF + 1
                    self.state = STATE_RUNNING
                    self.polls = 0
                    self.busy_requests = 0
                    self.t_arm = time.monotonic()
                return self._cap_reply(packet, tag, self.run, STATUS_WORDS, self._status_words())
            if op == 3:
                page = ((tag & 0xFF) << 8) | start
                vals = self._values()
                first = page * PAGE_VALUES
                return self._cap_reply(packet, tag, self.run, len(vals), vals[first:first + PAGE_VALUES])
        # everything else: the ordinary snapshot 0x0003 (sums) or "nothing"
        want = tag
        if self.snapshots and want in (0, 3):
            if start == 0:
                self.seq = self.seq % 0xFFFF + 1            # a new snapshot is published when the host starts a read
            rng = random.Random(self.seq)
            vals = [((3300 - (s * 7) % 200) * 16 + int(rng.gauss(0, 20 * 4))) & 0xFFFF for s in range(76)]
            n = max(0, min((packet[4] - 6) // 2 if packet[4] >= 6 else 0, PAGE_VALUES, 76 - start))
            data = bytearray([3, 0, self.seq & 0xFF, self.seq >> 8, 76, 0])
            for v in vals[start:start + n]:
                data += bytes([v & 0xFF, v >> 8])
            return self._reply(bytes(data), start)
        data = bytearray([0, 0, 0, 0, 0, 0])
        return self._reply(bytes(data), start)


# --------------------------------------------------------------------------- self test

def load_vectors():
    import re
    src = VECTORS.read_text(encoding="utf-8")
    out = []
    for m in re.finditer(r'\{ "([^"]*)", "([0-9a-f]+)", "([0-9a-f]+)" \}', src):
        out.append((m.group(1), bytes.fromhex(m.group(2)), bytes.fromhex(m.group(3))))
    return out


def vector_keyboard():
    """The fixed capture state the C golden vectors use (firmware/tools/gen_capture_vectors.py describes it)."""
    sim = SimulatedNoiseKeyboard()
    sim.sel = [5, 12, NO_SLOT, NO_SLOT]
    sim.state, sim.run, sim.ring = STATE_DONE, 3, 0
    n = sim._cap_frames()
    sim.frames = 100
    sim.cond_frames = 20
    sim.snapshots = False                       # like the C test: nothing is published, so snapshot requests get the empty header
    sim.buf = [[((f * 31 + c * 7 + 1000) & VALUE_MASK) | (FLAG if f % 5 == 0 else 0) for c in range(2)] for f in range(100)]
    return sim


def self_test() -> int:
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    def raises(exc, fn, *a, **kw):
        try:
            fn(*a, **kw)
        except exc:
            return True
        except Exception as e:                                  # noqa: BLE001
            fails.append("%s raised %r" % (getattr(fn, "__name__", fn), e))
            return False
        return False

    # 1. packets: a capture request is an ordinary payload-free scope request; only capture tags are accepted
    p = cap_request(CAP_STATUS, 0)
    check(p[0] == 0x55 and p[1] == 0xC5 and p[2] == 0 and not any(p[8:]), "capture request shape")
    check(p[6] | (p[7] << 8) == 0xC000, "status tag")
    check(raises(bk.SafetyError, cap_request, 0x0003), "a snapshot tag is not a capture tag")
    check(raises(bk.SafetyError, cap_request, 0xD000), "tag above 0xCFFF refused")
    pg = page_request(0x1234)
    check(pg[5] == 0x34 and (pg[6] | (pg[7] << 8)) == 0xC312, "page request packs page high into the tag, low into start")
    check(raises(bk.SafetyError, page_request, 0x10000), "page out of range")

    # 2. golden vectors shared with the C tests
    try:
        vectors = load_vectors()
    except OSError:
        vectors = []
    check(len(vectors) >= 10, "capture golden vectors missing (%d); run firmware/tools/gen_capture_vectors.py" % len(vectors))
    for name, req, reply in vectors:
        sim = vector_keyboard()
        check(sim.exchange(req) == reply, "%s: simulated reply differs from the golden reply" % name)

    # 3. client against the simulator: verify, select, capture, read back
    sim = SimulatedNoiseKeyboard()
    c = CaptureClient(sim)
    check(raises(NoiseError, c.status), "no capture op before verify")
    check(sim.seen == [], "nothing was sent before verify")
    c.verify()
    st, rows = c.capture([5, 12, 20, 33])
    check(st.n_sel == 4 and st.avail == 4096 and len(rows) == 4096 and len(rows[0]) == 4, "4 slots hold 4096 frames")
    check(abs(st.sample_rate_hz() - 240e6 / 10300) < 1, "sample rate from the cycle counter: %.1f" % st.sample_rate_hz())
    st1, rows1 = c.capture([5])
    check(st1.avail == 16384 and len(rows1) == 16384, "1 slot holds 16384 frames")
    check(raises(bk.SafetyError, c.select, []), "empty selection refused")
    check(raises(bk.SafetyError, c.select, [76]), "slot 76 refused")
    check(raises(bk.SafetyError, c.select, [1, 2, 3, 4, 5]), "5 slots refused")
    # the packets of all of the above were only scope/cmd3 requests
    check(set(sim.seen) <= {bk.CMD_GET_INFO, scope.CMD_SCOPE}, "only cmd 3 and the scope command were sent")

    # a firmware that is not the diagnostic image is never sent a capture op
    for build in ("Jan  1 2026,00:00:00STOCK-SIM", "Jan  1 2026,00:00:00KMFW-L51"):
        other = SimulatedNoiseKeyboard(build=build)
        oc = CaptureClient(other)
        check(raises(NoiseError, oc.verify), "%s: verify must refuse" % build)
        check(raises(NoiseError, oc.status), "%s: no capture op before verify" % build)
        check(other.seen == [bk.CMD_GET_INFO], "%s: only the cmd 3 query was sent (%s)" % (build, other.seen))

    # a run restarted while reading is noticed
    sim2 = SimulatedNoiseKeyboard()
    c2 = CaptureClient(sim2)
    c2.verify()
    st2, _ = c2.capture([5, 6])
    sim2.run += 1
    check(raises(NoiseError, c2.read_values, st2), "a changed run counter during a read is an error")

    # 4. the file format round trip, and a hand-checked header
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        pth = Path(td) / "t.kmtr"
        rows_in = [[(1000 + f) | (FLAG if f % 3 == 0 else 0), 2000 + f] for f in range(50)]
        write_kmtr(pth, [5, 12], rows_in, 23456.5)
        raw = pth.read_bytes()
        check(raw[:4] == b"KMTR" and raw[4:6] == b"\x01\x00" and raw[6:8] == b"\x02\x00" and raw[8:12] == b"\x32\x00\x00\x00", "KMTR header bytes")
        check(len(raw) == 20 + 4 + 50 * 4, "KMTR size")
        s2, r2, fs2 = read_kmtr(pth)
        check(s2 == [5, 12] and r2 == rows_in and abs(fs2 - 23456.5) < 0.01, "KMTR round trip")
        pth.write_bytes(raw[:-1])
        check(raises(NoiseError, read_kmtr, pth), "truncated file refused")
        pth.write_bytes(b"XXXX" + raw[4:])
        check(raises(NoiseError, read_kmtr, pth), "bad magic refused")

    # 5. statistics on signals with known answers
    fs = 23300.0
    rng = random.Random(7)
    white = [3300 + rng.gauss(0, 20) for _ in range(8192)]
    cs = column_stats(white, fs)
    check(abs(cs["sigma"] - 20) < 1.0, "white sigma %.2f" % cs["sigma"])
    check(0.8 < cs["color_factor"] < 1.25, "white noise has color factor ~1 (%.2f)" % cs["color_factor"])
    check(cs["spectrum"]["bands"][3] > 0.5, "white noise variance sits mostly above 5 kHz (%s)" % cs["spectrum"]["bands"])
    # a slow wander is NOT removed by the 16-sample mean: color factor far above 1
    lf, slow = 0.0, []
    for _ in range(8192):
        lf = 0.999 * lf + rng.gauss(0, 1.0)
        slow.append(3300 + lf)
    check(column_stats(slow, fs)["color_factor"] > 2.5, "slow wander has a large color factor")
    # a pure tone: sigma = A/sqrt2, one strong line at its frequency
    tone = [3300 + 30 * math.sin(2 * math.pi * 1000 * i / fs) for i in range(8192)]
    ct = column_stats(tone, fs)
    check(abs(ct["sigma"] - 30 / math.sqrt(2)) < 0.6, "tone sigma %.2f" % ct["sigma"])
    lines = ct["spectrum"]["lines"]
    check(lines and abs(lines[0][0] - 1000) < 15 and lines[0][2] > 0.7, "tone found at 1 kHz: %s" % (lines[:1],))
    # spikes
    sp = list(white)
    for i in (1000, 2500, 4000):
        sp[i] += 400
    check(abs(column_stats(sp, fs)["spikes_per_s"] - 3 / (8192 / fs)) < 0.5 * 3 / (8192 / fs) + 1.0, "three spikes counted")
    # a constant column: no crash, zero sigma
    check(column_stats([3000] * 256, fs)["sigma"] == 0.0, "constant column")
    # correlation: identical + independent + inverted columns, and the common-mode share
    a = [rng.gauss(0, 10) for _ in range(4000)]
    b = [rng.gauss(0, 10) for _ in range(4000)]
    mat, cm = correlation_matrix([a, a, b])
    check(abs(mat[0][1] - 1) < 1e-9 and abs(mat[0][2]) < 0.1, "correlation matrix")
    mat2, cm2 = correlation_matrix([a, [-v for v in a]])
    check(abs(mat2[0][1] + 1) < 1e-9 and cm2 < 1e-9, "anti-correlated columns have no common mode")
    _m, cm3 = correlation_matrix([a, a, a, a])
    check(abs(cm3 - 1) < 1e-9, "identical columns: common-mode share 1 (%.3f)" % cm3)
    _m, cm4 = correlation_matrix([[rng.gauss(0, 10) for _ in range(4000)] for _ in range(4)])
    check(0.15 < cm4 < 0.4, "four independent columns: share ~ 1/4 (%.3f)" % cm4)
    # FFT against a direct DFT, and Parseval through the PSD
    xs = [rng.gauss(0, 1) for _ in range(32)]
    f1 = fft([complex(v) for v in xs])
    d1 = [sum(xs[n] * cmath.exp(-2j * math.pi * k * n / 32) for n in range(32)) for k in range(32)]
    check(max(abs(u - v) for u, v in zip(f1, d1)) < 1e-9, "FFT equals the DFT")
    fr, ps = welch_psd(white, fs)
    var_psd = sum(ps[1:]) * (fr[1] - fr[0])
    check(abs(var_psd - std(white) ** 2) / std(white) ** 2 < 0.1, "PSD integrates to the variance (%.1f vs %.1f)" % (var_psd, std(white) ** 2))
    check(abs(box_mean([1, 2, 3, 4, 5], 2)[0] - 1.5) < 1e-9 and len(box_mean([1] * 20, 16)) == 5, "box mean")

    # 6. the analysis recovers what the simulator was told (white 20, slow 4, a 1 kHz tone of 6 counts)
    sim3 = SimulatedNoiseKeyboard(white=20.0, slow=4.0, tone_hz=1000.0, tone_amp=6.0)
    c3 = CaptureClient(sim3)
    c3.verify()
    st3, rows3 = c3.capture([5, 12, 20, 33])
    an = analyze_capture(st3.slots, rows3, st3.sample_rate_hz())
    want_sd = math.sqrt(20 ** 2 + 4 ** 2 + 6 ** 2 / 2)
    for cs in an["columns"]:
        check(abs(cs["sigma"] - want_sd) < 1.2, "simulated sigma %.2f vs %.2f" % (cs["sigma"], want_sd))
        check(any(abs(l[0] - 1000) < 15 for l in cs["spectrum"]["lines"]), "tone line found in the simulated capture")
    check(an["common_mode_share"] < 0.5, "independent simulated columns")

    # 7. busy flag: the simulator marks frames recorded while the host kept the bus busy; quiet ones have no flag
    simb = SimulatedNoiseKeyboard(busy_extra=15.0)
    cb = CaptureClient(simb)
    cb.verify()
    stq, rq = cb.capture([5], busy=0.0)
    stb, rb = cb.capture([5], busy=0.05)
    check(stq.cond_frames == 0 or stq.cond_frames < stb.cond_frames, "busy capture has flagged frames")
    check(std([r[0] & VALUE_MASK for r in rb]) > std([r[0] & VALUE_MASK for r in rq]), "the busy capture is noisier in the simulation")

    # 8. a scenario end to end on the simulator, saved and reported
    with tempfile.TemporaryDirectory() as td:
        class A:
            slow_seconds = 0.2
            key = "G"
            depths = "0.5 mm,1.5 mm"
            hold_seconds = 0.01
            keys = ",".join(DEFAULT_KEYS)
            repeats = 2
            warm_minutes = 0.0
            warm_interval = 0.0
        sim4 = SimulatedNoiseKeyboard()
        c4 = CaptureClient(sim4)
        c4.verify()
        lines = []
        ui = UI(interactive=False, out=lines.append)
        outdir = Path(td) / "A"
        run_A(c4, outdir, ui, A)
        res = report_dir(outdir, out=lines.append)
        files = sorted(p.name for p in outdir.glob("*.kmtr"))
        check(len(files) == (len(key_slots()) + 3) // 4, "one capture per group of four keys (%d files)" % len(files))
        check(any("sigma over keys" in ln for ln in lines), "report has the sigma summary")
        check((outdir / "A_slow.csv").exists(), "slow log written")
        for sc, fn in (("C", run_C), ("D", run_D), ("B", run_B)):
            od = Path(td) / sc
            fn(c4, od, ui, A)
            report_dir(od, out=lines.append)
            check(any(od.glob("*.kmtr")), "scenario %s saved captures" % sc)
    check(set(sim4.seen) <= {bk.CMD_GET_INFO, scope.CMD_SCOPE}, "scenarios sent only cmd 3 and the scope command")

    print("SELF-TEST", "FAILED" if fails else "OK", "(%d golden vectors)" % len(vectors))
    for f in fails:
        print("  FAIL:", f)
    return 1 if fails else 0


# --------------------------------------------------------------------------- main

def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--list", action="store_true", help="list the scenarios")
    ap.add_argument("--run", action="store_true", help="talk to the real keyboard (diagnostic image KMFW-NOISE only)")
    ap.add_argument("--sim", action="store_true", help="use a simulated keyboard (no hardware)")
    ap.add_argument("--scenario", choices=sorted(SCENARIOS), help="A idle, B depth, C usb, D typing, E warm-up")
    ap.add_argument("--out", default="noise_data", help="directory for the files (a sub-directory per scenario)")
    ap.add_argument("--analyze", metavar="DIR", help="report from the saved files of a directory")
    ap.add_argument("--yes", action="store_true", help="do not wait for Enter between steps (simulations)")
    ap.add_argument("--keys", default=",".join(DEFAULT_KEYS), help="C/E: up to 4 key names (default %(default)s)")
    ap.add_argument("--key", default="G", help="B: the key that is pressed (default G)")
    ap.add_argument("--depths", default="0.5 mm,1.0 mm,1.5 mm,2.0 mm,2.5 mm,3.0 mm,到底", help="B: comma separated depth labels")
    ap.add_argument("--hold-seconds", type=float, default=30.0, help="B: seconds per depth (default 30)")
    ap.add_argument("--slow-seconds", type=float, default=60.0, help="A: seconds of slow logging (default 60)")
    ap.add_argument("--repeats", type=int, default=10, help="C/D: captures per condition (default 10)")
    ap.add_argument("--warm-minutes", type=float, default=30.0, help="E: minutes (default 30)")
    ap.add_argument("--warm-interval", type=float, default=1.0, help="E: seconds between slow samples (default 1)")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.list:
        for k, (name, text) in SCENARIOS.items():
            print("%s  %-12s %s" % (k, name, text))
        return 0
    if args.analyze:
        report_dir(Path(args.analyze))
        return 0
    if not args.scenario:
        print("Dry run: nothing was sent. Give --scenario A..E with --sim (simulated keyboard) or --run (real keyboard, KMFW-NOISE image).")
        print("capture status request :", cap_request(CAP_STATUS, 0)[:16].hex())
        print("capture page-0 request :", page_request(0)[:16].hex())
        return 0
    try:
        if args.run and not args.sim:
            if sys.platform != "win32":
                print("ERROR: --run needs Windows")
                return 1
            iface = bk.pick_interface()
            t = scope.ScopeTransport(iface["path"])
        elif args.sim:
            t = SimulatedNoiseKeyboard(tone_amp=4.0)
            args.yes = True
        else:
            print("choose --run or --sim")
            return 1
        try:
            client = CaptureClient(t)
            print("firmware:", client.verify())
            name = SCENARIOS[args.scenario][0]
            outdir = Path(args.out) / name
            print(SCENARIOS[args.scenario][1])
            RUNNERS[args.scenario](client, outdir, UI(interactive=not args.yes), args)
            print()
            report_dir(outdir)
        finally:
            if hasattr(t, "close"):
                t.close()
    except (bk.BackupError, bk.SafetyError, scope.ScopeError, NoiseError) as exc:
        print("ERROR:", exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
