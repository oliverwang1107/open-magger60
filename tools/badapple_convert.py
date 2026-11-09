#!/usr/bin/env python3
"""badapple_convert.py - turn a video or an image sequence into a KMAN clip for the 61 key lights (M6 package P7b).

    python tools/badapple_convert.py my_video.mp4 -o badapple.kman --preview badapple.gif
    python tools/badapple_convert.py frames_dir/ --src-fps 30 --fps 15 --bits 2 --fit crop
    python tools/badapple_convert.py --self-test          # no video needed, no keyboard needed

What it does (docs/BADAPPLE_NOTES.md, zh-TW, explains the whole chain):
  1. reads frames: a video through ffmpeg (if ffmpeg is installed), or a folder / glob of images through Pillow
  2. maps the picture onto the light layout (15 x 5 key widths, lights in the serpentine order of km_led.c) and takes the
     area average of a 1 x 1 key square around every light, in LINEAR light (gamma 2.2)
  3. optionally inverts (default: yes; see the notes: the three hard brightness limits make a white background very dim),
     quantizes to 1 / 2 / 4 bits with threshold / ordered dither / error diffusion
  4. writes the compact "KMAN" format that firmware/core/src/km_anim.c plays (key frames + difference run lengths)
  5. can write a preview GIF (decoded from the file that was just written, so what you see is what the keyboard plays),
     and a C array for compiling the clip into the firmware image

Needs: Python 3.8+, numpy and Pillow (both imported lazily; `pip install numpy pillow` if missing), ffmpeg only for video files.
Nothing here is copied from anywhere: the video and the music are somebody else's work and are NOT part of this repository;
the output files (*.kman, *.gif, badapple_data.c) must never be committed (see docs/BADAPPLE_NOTES.md section 7).
"""
import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# ---------------------------------------------------------------- the light layout (mirror of km_led.c, checked by --self-test)

LIGHTS = 61
# Horizontal centre of every light in 1/8 key widths, in light order (the serpentine over the five key rows).
X8 = [
    4, 12, 20, 28, 36, 44, 52, 60, 68, 76, 84, 92, 100, 112,
    114, 104, 96, 88, 80, 72, 64, 56, 48, 40, 32, 24, 16, 6,
    7, 18, 26, 34, 42, 50, 58, 66, 74, 82, 90, 98, 111,
    109, 94, 86, 78, 70, 62, 54, 46, 38, 30, 22, 9,
    5, 15, 25, 55, 85, 95, 105, 115,
]
ROW_START = [0, 14, 28, 41, 53, 61]
CANVAS_W, CANVAS_H = 15.0, 5.0              # key widths

# The hardware limits of km_led.h, only used to estimate what the lights will really show (--stats, --preview-limited).
CHANNEL_CAP, PIXEL_CAP = 140, 280
BUDGET_MA, MA_PER_CHANNEL = 180, 15
BUDGET_SUM = BUDGET_MA * 255 // MA_PER_CHANNEL

FORMAT_VERSION = 1
HEADER_BYTES = 20
MAX_GROUPS = 1024
MAX_DATA = 65535
VIDEO_WIDTH = 480                           # width of the picture ffmpeg hands over (32 px per key width)


def row_of(i):
    r = 0
    while r < 4 and i >= ROW_START[r + 1]:
        r += 1
    return r


# ---------------------------------------------------------------- the KMAN format: writer (reference) and strict reader

class KmanError(ValueError):
    pass


def eg_len(v, k):
    return 2 * (v + (1 << k)).bit_length() - 1 - k


class BitWriter:
    def __init__(self):
        self.val = 0
        self.n = 0

    def put(self, v, nbits):
        if nbits:
            self.val = (self.val << nbits) | (v & ((1 << nbits) - 1))
            self.n += nbits

    def eg(self, v, k):
        x = v + (1 << k)
        ln = x.bit_length()
        self.put(0, ln - 1 - k)
        self.put(x, ln)

    def to_bytes(self):
        pad = (-self.n) % 8
        return (self.val << pad).to_bytes((self.n + pad) // 8, "big")


def default_palette(bits, max_level=255, gamma=2.2):
    """Grey level of every sample value: the sample is a perceptual (display coded) step, the light is linear, so the level
    is max_level * (q / (L - 1)) ** gamma. Kept strictly increasing, as the format requires: when the dark steps would round to
    the same level they are pushed up by one (palette_squeezed() tells whether that happened)."""
    if not 1 <= max_level <= 255:
        raise KmanError("max-level must be 1..255")
    levels = 1 << bits
    pal = []
    for q in range(levels):
        v = int(round(max_level * (q / (levels - 1)) ** gamma))
        if pal and v <= pal[-1]:
            v = pal[-1] + 1
        pal.append(v)
    if pal[-1] > 255:
        raise KmanError("max-level %d is too small for %d grey steps (the levels would collapse); use fewer bits" % (max_level, levels))
    return pal


def palette_squeezed(bits, max_level=255, gamma=2.2):
    levels = 1 << bits
    ideal = [int(round(max_level * (q / (levels - 1)) ** gamma)) for q in range(levels)]
    return ideal != default_palette(bits, max_level, gamma)


def diff_list(prev, cur):
    return [(i, cur[i]) for i in range(LIGHTS) if cur[i] != prev[i]]


def encode_group_sizes(frames, key, bits, kc, kg, diffs):
    """Encode all groups with the given Exp-Golomb orders; returns the list of group byte strings."""
    groups = []
    n = len(frames)
    for g0 in range(0, n, key):
        w = BitWriter()
        for f in range(g0, min(n, g0 + key)):
            if f == g0:
                for q in frames[f]:
                    w.put(int(q), bits)
            else:
                d = diffs[f]
                w.eg(len(d), kc)
                last = 0
                for cell, val in d:
                    w.eg(cell - last, kg)
                    if bits > 1:
                        w.put(int(val), bits)
                    last = cell + 1
        groups.append(w.to_bytes())
    return groups


def assemble(groups, n, bits, fps, key, kc, kg, flags, palette):
    m = len(groups)
    data_len = sum(len(g) for g in groups)
    if not 1 <= fps <= 60:
        raise KmanError("fps must be 1..60")
    if not 1 <= n <= 65535:
        raise KmanError("frame count must be 1..65535")
    if m > MAX_GROUPS:
        raise KmanError("too many key-frame groups (%d > %d): use a bigger --key-interval" % (m, MAX_GROUPS))
    if data_len > MAX_DATA:
        raise KmanError("frame data is %d bytes, the format allows at most %d: lower fps / bits or shorten the clip" % (data_len, MAX_DATA))
    out = bytearray(b"KMAN")
    out += struct.pack("<BBBBHBBBBHHH", FORMAT_VERSION, LIGHTS, bits, fps, n, key, flags, kc, kg, m, data_len, 0)
    assert len(out) == HEADER_BYTES
    out += bytes(palette)
    off = 0
    for g in groups:
        out += struct.pack("<H", off)
        off += len(g)
    for g in groups:
        out += g
    out += struct.pack("<I", zlib.crc32(bytes(out)) & 0xFFFFFFFF)
    return bytes(out)


def encode(frames, bits, fps, key, palette=None, flags=0, orders=None):
    """frames: list of 61-sample lists (values 0 .. 2**bits-1). Tries every Exp-Golomb order pair and keeps the smallest result
    unless `orders` = (kc, kg) is given."""
    if bits not in (1, 2, 4):
        raise KmanError("bits must be 1, 2 or 4")
    if not 1 <= key <= 255:
        raise KmanError("key interval must be 1..255")
    for fr in frames:
        if len(fr) != LIGHTS or max(fr) >= (1 << bits):
            raise KmanError("bad frame")
    if palette is None:
        palette = default_palette(bits)
    n = len(frames)
    diffs = [None] + [diff_list(frames[f - 1], frames[f]) for f in range(1, n)]
    best = None
    for kc, kg in ([orders] if orders else [(a, b) for a in range(5) for b in range(5)]):
        groups = encode_group_sizes(frames, key, bits, kc, kg, diffs)
        size = sum(len(g) for g in groups)
        if best is None or size < best[0]:
            best = (size, kc, kg, groups)
    _, kc, kg, groups = best
    return assemble(groups, n, bits, fps, key, kc, kg, flags, palette)


class BitReader:
    def __init__(self, data, pos, end):
        self.d, self.pos, self.end = data, pos, end

    def bits(self, n):
        if n > self.end - self.pos:
            raise KmanError("stream ends inside a frame")
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.d[self.pos >> 3] >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def eg(self, k):
        q = 0
        while self.bits(1) == 0:
            q += 1
            if q > 7:
                raise KmanError("bad Exp-Golomb prefix")
        return (((1 << q) - 1) << k) + self.bits(q + k)


def parse(data):
    """Strict header check, same rules as km_anim_parse(). Returns a dict."""
    if len(data) < HEADER_BYTES + 9:
        raise KmanError("too short")
    if data[:4] != b"KMAN":
        raise KmanError("bad magic")
    if data[4] != FORMAT_VERSION:
        raise KmanError("bad version")
    (_, lights, bits, fps, n, key, flags, kc, kg, m, dlen, rsv) = struct.unpack("<BBBBHBBBBHHH", data[4:HEADER_BYTES])
    groups_ok = key >= 1 and m == (n + key - 1) // key
    if (lights != LIGHTS or bits not in (1, 2, 4) or not 1 <= fps <= 60 or n < 1 or key < 1 or flags & ~1 or kc > 4 or kg > 4
            or not groups_ok or m > MAX_GROUPS or dlen < 1 or rsv):
        raise KmanError("bad header field")
    levels = 1 << bits
    total = HEADER_BYTES + levels + 2 * m + dlen + 4
    if len(data) != total:
        raise KmanError("length %d does not match the header (%d)" % (len(data), total))
    pal = list(data[HEADER_BYTES:HEADER_BYTES + levels])
    if any(pal[i] <= pal[i - 1] for i in range(1, levels)):
        raise KmanError("palette not strictly increasing")
    idx_at = HEADER_BYTES + levels
    index = [struct.unpack_from("<H", data, idx_at + 2 * g)[0] for g in range(m)]
    min_group = (LIGHTS * bits + 7) // 8
    for g in range(m):
        nxt = index[g + 1] if g + 1 < m else dlen
        if (g == 0 and index[0] != 0) or (g and index[g] <= index[g - 1]) or index[g] >= dlen or nxt < index[g] or nxt - index[g] < min_group:
            raise KmanError("bad group index")
    return dict(bits=bits, fps=fps, frames=n, key=key, flags=flags, kc=kc, kg=kg, groups=m, data_len=dlen, palette=pal,
                index=index, data_at=idx_at + 2 * m, crc=struct.unpack_from("<I", data, total - 4)[0], total=total)


def decode(data, check_crc=True):
    """Decode every frame with the same checks as the firmware. Returns (info, list of 61-sample lists)."""
    info = parse(data)
    if check_crc and zlib.crc32(data[:-4]) & 0xFFFFFFFF != info["crc"]:
        raise KmanError("checksum mismatch")
    bits, key, n = info["bits"], info["key"], info["frames"]
    base, dlen = info["data_at"], info["data_len"]
    frames = []
    cur = None
    for g in range(info["groups"]):
        start = info["index"][g]
        end = info["index"][g + 1] if g + 1 < info["groups"] else dlen
        r = BitReader(data, (base + start) * 8, (base + end) * 8)
        for f in range(g * key, min(n, (g + 1) * key)):
            if f == g * key:
                cur = [r.bits(bits) for _ in range(LIGHTS)]
            else:
                cur = list(cur)
                count = r.eg(info["kc"])
                if count > LIGHTS:
                    raise KmanError("more than 61 changes")
                cell = 0
                for _ in range(count):
                    cell += r.eg(info["kg"])
                    if cell >= LIGHTS:
                        raise KmanError("cell past the last light")
                    if bits == 1:
                        cur[cell] ^= 1
                    else:
                        v = r.bits(bits)
                        if v == cur[cell]:
                            raise KmanError("a change that changes nothing")
                        cur[cell] = v
                    cell += 1
            frames.append(cur)
        pos = r.pos
        if (pos + 7) // 8 != base + end:
            raise KmanError("group does not end where the index says")
        if pos & 7 and data[pos >> 3] & (0xFF >> (pos & 7)):
            raise KmanError("padding bits are not zero")
    return info, frames


def to_levels(info, frames):
    pal = info["palette"]
    return [[pal[q] for q in fr] for fr in frames]


# ---------------------------------------------------------------- picture -> 61 lights

def _np():
    try:
        import numpy as np
    except ImportError:
        sys.exit("badapple_convert: numpy is needed for this (pip install numpy pillow)")
    return np


def _pil():
    try:
        from PIL import Image
    except ImportError:
        sys.exit("badapple_convert: Pillow is needed for this (pip install numpy pillow)")
    return Image


def srgb_lut(gamma=2.2):
    np = _np()
    return (np.arange(256, dtype=np.float64) / 255.0) ** gamma


class Mapper:
    """Maps the 15 x 5 key-width canvas onto a source picture and averages the 1 x 1 square around every light.
    fit: 'crop' (the light field is a window into the picture, the rest of the picture is cut away), 'stretch' (squeeze the whole
    picture onto the field), 'letterbox' (the whole picture fits inside the field, bars at the sides; the bars count as dark in
    the FINAL picture, whatever --invert says)."""

    def __init__(self, w, h, fit="crop", zoom=1.0, pos_x=0.5, pos_y=0.5):
        np = _np()
        self.w, self.h = w, h
        if fit == "stretch":
            sx, sy = w / CANVAS_W / zoom, h / CANVAS_H / zoom
        elif fit in ("crop", "letterbox"):
            s = (min if fit == "crop" else max)(w / CANVAS_W, h / CANVAS_H) / zoom
            sx = sy = s
        else:
            raise ValueError("fit must be crop, stretch or letterbox")
        ox = w * pos_x - CANVAS_W * sx / 2
        oy = h * pos_y - CANVAS_H * sy / 2
        if fit == "crop":
            ox = min(max(ox, 0.0), max(w - CANVAS_W * sx, 0.0))
            oy = min(max(oy, 0.0), max(h - CANVAS_H * sy, 0.0))
        x0 = np.array([ox + (X8[i] / 8 - 0.5) * sx for i in range(LIGHTS)])
        x1 = np.array([ox + (X8[i] / 8 + 0.5) * sx for i in range(LIGHTS)])
        y0 = np.array([oy + row_of(i) * sy for i in range(LIGHTS)])
        y1 = np.array([oy + (row_of(i) + 1) * sy for i in range(LIGHTS)])
        full = (x1 - x0) * (y1 - y0)
        cx0, cx1 = np.clip(x0, 0, w), np.clip(x1, 0, w)
        cy0, cy1 = np.clip(y0, 0, h), np.clip(y1, 0, h)
        area = np.maximum(cx1 - cx0, 0) * np.maximum(cy1 - cy0, 0)
        self.cov = area / full                      # share of the light's square that lies on the picture
        self.area = area
        self.corners = [self._prep(x, y) for x, y in ((cx1, cy1), (cx0, cy1), (cx1, cy0), (cx0, cy0))]

    def _prep(self, xs, ys):
        np = _np()
        xi = np.minimum(np.floor(xs).astype(np.int64), self.w - 1)
        yi = np.minimum(np.floor(ys).astype(np.int64), self.h - 1)
        return xi, xs - xi, yi, ys - yi

    @staticmethod
    def _eval(S, p):
        xi, fx, yi, fy = p
        return (S[yi, xi] * (1 - fx) * (1 - fy) + S[yi, xi + 1] * fx * (1 - fy) +
                S[yi + 1, xi] * (1 - fx) * fy + S[yi + 1, xi + 1] * fx * fy)

    def average(self, lin):
        """lin: float array (h, w) of linear luminance 0..1. Returns the mean over the covered part of every light's square."""
        np = _np()
        S = np.zeros((self.h + 1, self.w + 1))
        S[1:, 1:] = lin.cumsum(0).cumsum(1)
        a, b, c, d = (self._eval(S, p) for p in self.corners)
        total = a - b - c + d
        with np.errstate(divide="ignore", invalid="ignore"):
            avg = np.where(self.area > 0, total / np.where(self.area > 0, self.area, 1), 0.0)
        return avg


def bayer4():
    m = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
    return [[(v + 0.5) / 16 for v in row] for row in m]


class Quantizer:
    """Linear light (0..1) -> sample values. Works frame by frame because the hysteresis and the error diffusion have state."""

    def __init__(self, bits, gamma=2.2, threshold=128, dither="none", hysteresis=0.0):
        self.bits, self.L = bits, 1 << bits
        self.gamma, self.threshold, self.dither, self.hyst = gamma, threshold, dither, hysteresis
        self.prev = None
        self.bayer = bayer4()
        # for error diffusion: lights of each row sorted by x, and for every light the nearest light of the next row
        rows = {}
        for i in range(LIGHTS):
            rows.setdefault(row_of(i), []).append(i)
        self.right, self.below = {}, {}
        for r, ids in rows.items():
            ids.sort(key=lambda i: X8[i])
            for a, b in zip(ids, ids[1:]):
                self.right[a] = b
            if r + 1 in rows:
                for i in ids:
                    self.below[i] = min(rows[r + 1], key=lambda j: abs(X8[j] - X8[i]))
        self.order = sorted(range(LIGHTS), key=lambda i: (row_of(i), X8[i]))

    def frame(self, lin):
        p = [min(max(float(v), 0.0), 1.0) ** (1.0 / self.gamma) for v in lin]     # perceptual value 0..1
        L1 = self.L - 1
        q = [0] * LIGHTS
        if self.dither == "fs":
            err = [0.0] * LIGHTS
            for i in self.order:
                v = min(max(p[i] + err[i], 0.0), 1.0)
                qi = int(round(v * L1)) if self.bits > 1 or self.threshold is None else int(v * 255 >= self.threshold)
                q[i] = qi
                e = v - qi / L1
                if i in self.right:
                    err[self.right[i]] += e * 7 / 16
                    e = e * 9 / 16
                if i in self.below:
                    err[self.below[i]] += e
        else:
            for i in range(LIGHTS):
                if self.dither == "bayer":
                    t = self.bayer[row_of(i) % 4][(X8[i] // 8) % 4]
                    qi = int(p[i] * L1 + t)
                elif self.bits == 1:
                    qi = int(p[i] * 255 >= self.threshold)
                else:
                    qi = int(round(p[i] * L1))
                q[i] = min(max(qi, 0), L1)
            if self.hyst > 0 and self.prev is not None:
                for i in range(LIGHTS):
                    if abs(p[i] * L1 - self.prev[i]) <= 0.5 + self.hyst:
                        q[i] = self.prev[i]
        self.prev = q
        return q


# ---------------------------------------------------------------- brightness model (what the hardware limits make of a frame)

def effective_levels(levels, tint=(255, 255, 255)):
    """Model of km_led's limits on one frame of pre-limit grey levels (white by default). Returns the mean-channel value every light
    ends up with, as floats. Order as in km_led.h: per-channel cap, per-light cap, total budget (assumed 15 mA per channel)."""
    px = []
    for lv in levels:
        ch = [min(lv * t // 255, CHANNEL_CAP) for t in tint]
        s = sum(ch)
        if s > PIXEL_CAP:
            ch = [c * PIXEL_CAP / s for c in ch]
        px.append(ch)
    total = sum(sum(c) for c in px)
    scale = min(1.0, BUDGET_SUM / total) if total > 0 else 1.0
    return [sum(c) * scale / len(c) for c in px], scale, total * scale * MA_PER_CHANNEL / 255


# ---------------------------------------------------------------- frame sources

IMAGE_EXT = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".webp", ".ppm", ".pgm"}


def image_files(spec):
    p = Path(spec)
    if p.is_dir():
        files = sorted(f for f in p.iterdir() if f.suffix.lower() in IMAGE_EXT)
    else:
        files = sorted(Path(f) for f in glob.glob(spec))
    return [f for f in files if f.suffix.lower() in IMAGE_EXT]


def is_image_sequence(spec):
    return Path(spec).is_dir() or any(c in spec for c in "*?[")


def gray_frames_from_images(files, src_fps, fps, start, duration):
    """Yields (h, w) uint8 grey arrays, picking the source picture nearest to every output time."""
    np, Image = _np(), _pil()
    first = int(round(start * src_fps))
    count = len(files) - first
    if duration is not None:
        count = min(count, int(duration * src_fps))
    out_n = int(count * fps / src_fps)
    for j in range(out_n):
        k = min(first + int(round(j * src_fps / fps)), len(files) - 1)
        with Image.open(files[k]) as im:
            yield np.asarray(im.convert("L"), dtype=np.uint8)


def find_ffmpeg(hint):
    if hint:
        return hint
    return shutil.which("ffmpeg")


def probe_size(ffmpeg, path):
    r = subprocess.run([ffmpeg, "-hide_banner", "-i", path], capture_output=True, text=True)
    for line in r.stderr.splitlines():
        if "Video:" in line:
            m = re.search(r"[ ,](\d{2,5})x(\d{2,5})[ ,\[]", line)
            if m:
                return int(m.group(1)), int(m.group(2))
    raise SystemExit("badapple_convert: could not read the picture size of %s (ffmpeg said:\n%s)" % (path, r.stderr[-500:]))


def gray_frames_from_video(ffmpeg, path, fps, start, duration):
    np = _np()
    w0, h0 = probe_size(ffmpeg, path)
    w = VIDEO_WIDTH
    h = max(2, int(round(w * h0 / w0 / 2)) * 2)
    cmd = [ffmpeg, "-v", "error", "-nostdin"]
    if start:
        cmd += ["-ss", str(start)]
    cmd += ["-i", path]
    if duration is not None:
        cmd += ["-t", str(duration)]
    cmd += ["-vf", "fps=%s,scale=%d:%d:flags=area" % (fps, w, h), "-pix_fmt", "gray", "-f", "rawvideo", "-"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    size = w * h
    try:
        while True:
            buf = proc.stdout.read(size)
            while buf and len(buf) < size:
                more = proc.stdout.read(size - len(buf))
                if not more:
                    break
                buf += more
            if len(buf) < size:
                break
            yield np.frombuffer(buf, dtype=np.uint8).reshape(h, w)
    finally:
        proc.stdout.close()
        proc.wait()


# ---------------------------------------------------------------- the conversion

def sample_clip(gray_frames, args, progress=None):
    """grey pictures -> list of 61-value linear final brightness (after fit, area average, invert)."""
    np = _np()
    lut = srgb_lut(args.gamma)
    mapper = None
    out = []
    for k, g in enumerate(gray_frames):
        if mapper is None:
            h, w = g.shape
            mapper = Mapper(w, h, args.fit, args.zoom, args.crop_x, args.crop_y)
        avg = mapper.average(lut[g])
        fin = (1.0 - avg if args.invert else avg) * mapper.cov
        out.append([float(min(max(v, 0.0), 1.0)) for v in fin])
        if progress and k % 500 == 499:
            progress(k + 1)
    return out


def quantize_clip(lin_frames, args):
    qz = Quantizer(args.bits, args.gamma, args.threshold, args.dither, args.hysteresis)
    return [qz.frame(f) for f in lin_frames]


def decimate(lin_frames, src_fps, fps):
    n = int(len(lin_frames) * fps / src_fps)
    return [lin_frames[min(int(round(j * src_fps / fps)), len(lin_frames) - 1)] for j in range(n)]


def stats_text(info, frames, data_len_total, src_n=None):
    pal = info["palette"]
    n = len(frames)
    changes = [sum(1 for a, b in zip(frames[f - 1], frames[f]) if a != b) for f in range(1, n)]
    lit = sum(1 for fr in frames for q in fr if q) / (n * LIGHTS)
    lv = to_levels(info, frames)
    scales = []
    cur = []
    for fr in lv:
        _, s, ma = effective_levels(fr)
        scales.append(s)
        cur.append(ma)
    scales.sort()
    dur = n / info["fps"]
    raw = n * ((LIGHTS * info["bits"] + 7) // 8)
    lines = [
        "frames            %d at %d fps (%.1f s), %d bits, key every %d, Exp-Golomb orders count %d / gap %d" % (
            n, info["fps"], dur, info["bits"], info["key"], info["kc"], info["kg"]),
        "file size         %d bytes (frame data %d, index %d, palette %d, header+crc 24); raw %d bytes -> x%.2f" % (
            data_len_total, info["data_len"], 2 * info["groups"], 1 << info["bits"], raw, raw / max(info["data_len"], 1)),
        "bytes per second  %.0f" % (info["data_len"] / dur),
        "changed cells     mean %.1f per frame (max %d); lit %.0f %% of the cells on average" % (
            sum(changes) / max(len(changes), 1), max(changes, default=0), 100 * lit),
        "palette levels    %s (pre-limit grey, 255 = full)" % pal,
        "white tint, with the hard limits (per channel 140, per light 280, total %d mA model): brightness scale mean %.2f, worst %.2f, "
        "peak current model %.0f mA" % (BUDGET_MA, sum(scales) / len(scales), scales[0], max(cur)),
    ]
    return "\n".join(lines)


def render_preview(path, info, frames, scale=24, limited=False):
    np, Image = _np(), _pil()
    w, h = int(CANVAS_W * scale), int(CANVAS_H * scale)
    pal = info["palette"]
    ims = []
    pad = max(1, scale // 10)
    for fr in frames:
        lv = [pal[q] for q in fr]
        if limited:
            eff, _, _ = effective_levels(lv)
            lv = [min(255, e * 255 / 93) for e in eff]        # 93 = what a white light gets at best (280 / 3)
        arr = np.full((h, w, 3), 18, dtype=np.uint8)
        for i in range(LIGHTS):
            cx, cy = X8[i] / 8, row_of(i) + 0.5
            x0, x1 = int((cx - 0.5) * scale) + pad, int((cx + 0.5) * scale) - pad
            y0, y1 = int((cy - 0.5) * scale) + pad, int((cy + 0.5) * scale) - pad
            shown = 255 * (min(lv[i], 255) / 255) ** (1 / 2.2)         # what the eye makes of a linear light
            arr[y0:y1, x0:x1] = (max(shown, 30) * np.array([1.0, 0.95, 0.88])).astype(np.uint8)
        ims.append(Image.fromarray(arr))
    # a GIF counts time in 10 ms steps: spread the rounding so that the average speed is right (15 fps = 70, 60, 70, 70, 60, ...)
    t = [int(round(k * 100 / info["fps"])) * 10 for k in range(len(ims) + 1)]
    durations = [max(20, t[k + 1] - t[k]) for k in range(len(ims))]
    ims[0].save(path, save_all=True, append_images=ims[1:], duration=durations, loop=0)


def write_c_array(path, data, symbol):
    lines = ["/* generated by tools/badapple_convert.py - do not commit (docs/BADAPPLE_NOTES.md section 7) */",
             "#include <stdint.h>", "", "const uint8_t %s[] = {" % symbol]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",")
    lines += ["};", "const unsigned %s_len = %d;" % (symbol, len(data)), ""]
    Path(path).write_text("\n".join(lines), encoding="ascii")


# ---------------------------------------------------------------- synthetic content (self-test, size report, golden vectors)

class Xorshift:
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF or 1

    def next(self):
        s = self.s
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self.s = s
        return s

    def below(self, n):
        return self.next() % n

    def uniform(self):
        return self.next() / 4294967296.0


def synth_gray_frames(n, w, h, seed=1, cut_every=180):
    """A made-up 4:3 clip that moves like a silhouette animation: dark ellipses on white, smooth motion, a scene cut every
    `cut_every` frames. Only for tests and size estimates; it has nothing to do with any real video."""
    np = _np()
    rng = Xorshift(seed)
    yy, xx = np.mgrid[0:h, 0:w]
    xx = xx / w * 15.0           # key widths
    yy = yy / w * 15.0           # same unit in y (the picture is 15 x 11.25 units)
    blobs = []

    def new_scene():
        blobs.clear()
        for _ in range(2 + rng.below(3)):
            blobs.append([1 + 13 * rng.uniform(), 1 + 9 * rng.uniform(), 1.0 + 2.5 * rng.uniform(), 0.8 + 2.0 * rng.uniform(),
                          (rng.uniform() - 0.5) * 6, (rng.uniform() - 0.5) * 4])

    new_scene()
    for f in range(n):
        if f and f % cut_every == 0:
            new_scene()
        img = np.full((h, w), 255.0)
        for b in blobs:
            b[4] += (rng.uniform() - 0.5) * 0.6
            b[5] += (rng.uniform() - 0.5) * 0.4
            b[4] = max(-7.0, min(7.0, b[4]))
            b[5] = max(-5.0, min(5.0, b[5]))
            b[0] += b[4] / 30.0
            b[1] += b[5] / 30.0
            if not 0 < b[0] < 15:
                b[4] = -b[4]
            if not 0 < b[1] < 11.25:
                b[5] = -b[5]
            m = ((xx - b[0]) / b[2]) ** 2 + ((yy - b[1]) / b[3]) ** 2 <= 1.0
            img[m] = 0.0
        yield img.astype(np.uint8)


def golden_frames(kind, bits, n, seed):
    """Integer-only test content (no floating point, so the committed vectors do not depend on the machine)."""
    rng = Xorshift(seed)
    L = 1 << bits
    cur = [rng.below(L) for _ in range(LIGHTS)]
    frames = [list(cur)]
    for f in range(1, n):
        cur = list(cur)
        if kind == "sparse":
            for _ in range(rng.below(5)):
                c = rng.below(LIGHTS)
                cur[c] = (cur[c] + 1 + rng.below(L - 1)) % L
        elif kind == "blocks":
            a = rng.below(LIGHTS)
            b = a + rng.below(LIGHTS - a)
            for c in range(a, b + 1):
                cur[c] = (cur[c] + 1) % L
            cur[0] = (cur[0] + 1) % L
            cur[LIGHTS - 1] = (cur[LIGHTS - 1] + 1) % L
        else:                                            # 'static' with a rare change
            if f % 7 == 0:
                c = rng.below(LIGHTS)
                cur[c] = (cur[c] + 1) % L
        frames.append(cur)
    return frames


GOLDEN_SPECS = [  # kind, bits, frames, fps, key interval, seed
    ("sparse", 1, 30, 10, 8, 11),
    ("blocks", 2, 36, 12, 12, 22),
    ("sparse", 4, 25, 15, 5, 33),
    ("static", 2, 20, 20, 7, 44),
]


def golden_header_text():
    out = ["/* anim_vectors.h - golden KMAN clips for tests/test_anim.c, written by tools/badapple_convert.py --emit-test-vector.",
           " * Integer-only synthetic content (no video, nothing derived from anything): regenerate with",
           " *   python tools/badapple_convert.py --emit-test-vector firmware/tests/golden/anim_vectors.h",
           " * (--self-test checks that this file is up to date). */",
           "#ifndef ANIM_VECTORS_H", "#define ANIM_VECTORS_H", "", "#include <stdint.h>", ""]
    entries = []
    for k, (kind, bits, n, fps, key, seed) in enumerate(GOLDEN_SPECS):
        frames = golden_frames(kind, bits, n, seed)
        data = encode(frames, bits, fps, key)
        info, dec = decode(data)
        assert dec == frames
        lv = to_levels(info, frames)
        out.append("/* %s, %d bits, %d frames, %d fps, key every %d, Exp-Golomb %d/%d */" % (kind, bits, n, fps, key, info["kc"], info["kg"]))
        out.append("static const uint8_t anim_vec%d_data[] = {" % k)
        for i in range(0, len(data), 16):
            out.append("    " + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",")
        out.append("};")
        out.append("static const uint8_t anim_vec%d_levels[] = {" % k)
        flat = [v for fr in lv for v in fr]
        for i in range(0, len(flat), 61):
            out.append("    " + ", ".join(str(v) for v in flat[i:i + 61]) + ",")
        out.append("};")
        out.append("")
        entries.append("    { anim_vec%d_data, sizeof anim_vec%d_data, %d, %d, anim_vec%d_levels }," % (k, k, n, fps, k))
    out += ["typedef struct { const uint8_t *data; unsigned len; unsigned frames; unsigned fps; const uint8_t *levels; } anim_vec_t;",
            "static const anim_vec_t anim_vecs[] = {"] + entries + ["};",
            "#define ANIM_VEC_COUNT %d" % len(GOLDEN_SPECS), "", "#endif", ""]
    return "\n".join(out)


# ---------------------------------------------------------------- size report on a made-up 3:39 clip

def synth_size_report(seconds=219, src_fps=30, quick=False):
    np = _np()
    n = int(seconds * src_fps)
    print("size estimate on a MADE-UP clip (%d s, moving dark ellipses on white, a cut every 6 s): not Bad Apple, not a measurement of it." % seconds)
    print("sampling %d pictures ..." % n, flush=True)
    args = argparse.Namespace(gamma=2.2, fit="crop", zoom=1.0, crop_x=0.5, crop_y=0.5, invert=True)
    lin = sample_clip(synth_gray_frames(n, 240, 180, seed=7, cut_every=src_fps * 6), args)
    print("%-6s %-5s %-6s %-9s %-9s %-8s %s" % ("fps", "bits", "key", "frames", "bytes", "x raw", "changed cells/frame"))
    for fps in (10, 15, 30):
        for bits in (1, 2, 4):
            frames_lin = decimate(lin, src_fps, fps)
            qz = Quantizer(bits, 2.2, 128, "none", 0.0)
            frames = [qz.frame(f) for f in frames_lin]
            data = encode(frames, bits, fps, 30)
            info = parse(data)
            ch = [sum(1 for a, b in zip(frames[f - 1], frames[f]) if a != b) for f in range(1, len(frames))]
            raw = len(frames) * ((LIGHTS * bits + 7) // 8)
            print("%-6d %-5d %-6d %-9d %-9d %-8.2f %.1f" % (fps, bits, 30, len(frames), len(data), raw / info["data_len"], sum(ch) / len(ch)), flush=True)


# ---------------------------------------------------------------- self test

def self_test():
    np = _np()
    Image = _pil()
    ok = [0, 0]

    def check(cond, what):
        ok[0 if cond else 1] += 1
        if not cond:
            print("FAIL:", what)

    def raises(fn, what):
        try:
            fn()
        except KmanError:
            ok[0] += 1
            return
        except Exception as e:                           # noqa: BLE001 - any other exception type is a bug in the reader
            print("FAIL: %s raised %r instead of KmanError" % (what, e))
            ok[1] += 1
            return
        print("FAIL: %s did not raise" % what)
        ok[1] += 1

    # 1. geometry equals the firmware's tables
    src = ROOT / "firmware/core/src/km_led.c"
    if src.exists():
        text = src.read_text()
        m = re.search(r"k_x8\[KM_LED_COUNT\]\s*=\s*\{([^}]*)\}", text)
        x8 = [int(v) for v in re.findall(r"\d+", m.group(1))] if m else []
        check(x8 == X8, "X8 equals k_x8 in km_led.c")
        m = re.search(r"k_row_start\[6\]\s*=\s*\{([^}]*)\}", text)
        rs = [61 if v == "KM_LED_COUNT" else int(v) for v in re.findall(r"KM_LED_COUNT|\d+", m.group(1))] if m else []
        check(rs == ROW_START, "ROW_START equals k_row_start in km_led.c")
    hdr = ROOT / "firmware/core/include/km/km_led.h"
    if hdr.exists():
        t = hdr.read_text()
        ch = re.search(r"define KM_LED_CHANNEL_CAP\s+(\d+)u", t)
        ma = re.search(r"define KM_LED_BUDGET_MA\s+(\d+)u", t)
        per = re.search(r"define KM_LED_MA_PER_CHANNEL\s+(\d+)u", t)
        check(bool(ch and ma and per) and (int(ch.group(1)), int(ma.group(1)), int(per.group(1))) == (CHANNEL_CAP, BUDGET_MA, MA_PER_CHANNEL),
              "limit constants equal km_led.h")
    check(sum(1 for i in range(LIGHTS) if row_of(i) == 0) == 14 and [row_of(i) for i in (13, 14, 27, 28, 40, 41, 52, 53, 60)] == [0, 1, 1, 2, 2, 3, 3, 4, 4],
          "row layout 14/14/13/12/8")

    # 2. format round trip with the reference writer and reader, every parameter combination
    rng = Xorshift(5)
    for bits in (1, 2, 4):
        for key in (1, 2, 7, 30, 255):
            for n in (1, 2, 30, 31, 65):
                for kind in ("sparse", "blocks", "static"):
                    fr = golden_frames(kind, bits, n, rng.next())
                    data = encode(fr, bits, 15, key)
                    info, dec = decode(data)
                    check(dec == fr, "round trip bits=%d key=%d n=%d %s" % (bits, key, n, kind))
                    check(info["frames"] == n and info["groups"] == (n + key - 1) // key, "header counts")
    fr = golden_frames("sparse", 2, 40, 9)
    for kc in range(5):
        for kg in range(5):
            data = encode(fr, 2, 10, 8, orders=(kc, kg))
            check(decode(data)[1] == fr, "orders %d/%d" % (kc, kg))
    # EG lengths agree with what is written
    for k in range(5):
        for v in range(62):
            w = BitWriter()
            w.eg(v, k)
            check(w.n == eg_len(v, k), "eg_len %d %d" % (v, k))

    # 3. the strict reader refuses damage (same classes as km_anim.c)
    data = encode(golden_frames("sparse", 2, 40, 3), 2, 15, 8)
    raises(lambda: decode(data[:-1]), "truncated")
    raises(lambda: decode(data + b"\0"), "extended")
    raises(lambda: decode(b"KMAX" + data[4:]), "magic")
    bad = bytearray(data); bad[-1] ^= 1
    raises(lambda: decode(bytes(bad)), "crc")
    for at in (5, 6, 7, 10, 11, 12, 13, 14, 16, 18):
        bad = bytearray(data); bad[at] ^= 0x20
        raises(lambda: decode(bytes(bad)), "header byte %d" % at)
    bad = bytearray(data); bad[HEADER_BYTES + 1] = 0
    raises(lambda: decode(bytes(bad)), "palette")
    flips = 0
    for i in range(0, len(data) - 4, 3):                  # every third byte of the whole file with a bit flipped, with the CRC repaired:
        bad = bytearray(data); bad[i] ^= 0x04             # nothing but KmanError may come out
        bad[-4:] = struct.pack("<I", zlib.crc32(bytes(bad[:-4])) & 0xFFFFFFFF)
        try:
            decode(bytes(bad))
        except KmanError:
            flips += 1
    check(flips > 20, "flipped bytes are noticed (%d)" % flips)

    # 4. golden header is up to date
    gold = ROOT / "firmware/tests/golden/anim_vectors.h"
    if gold.exists():
        check(gold.read_text() == golden_header_text(), "firmware/tests/golden/anim_vectors.h is up to date (run --emit-test-vector)")

    # 5. mapper: uniform picture, half picture, fit modes
    ones = np.ones((90, 120))
    mp = Mapper(120, 90, "crop")
    avg = mp.average(ones)
    check(np.allclose(avg, 1.0) and np.allclose(mp.cov, 1.0), "crop of a 4:3 picture covers every light")
    half = np.zeros((90, 120)); half[:, :60] = 1.0
    avg = mp.average(half)
    left = [i for i in range(LIGHTS) if X8[i] / 8 + 0.5 <= 7.5]
    right = [i for i in range(LIGHTS) if X8[i] / 8 - 0.5 >= 7.5]
    check(all(avg[i] > 0.999 for i in left) and all(avg[i] < 0.001 for i in right), "left half white -> left lights lit")
    straddle = [i for i in range(LIGHTS) if X8[i] / 8 - 0.5 < 7.5 < X8[i] / 8 + 0.5]
    check(all(0.0 < avg[i] < 1.0 for i in straddle), "a light on the edge gets a mix")
    mp = Mapper(120, 90, "letterbox")
    check(np.allclose(mp.average(ones)[mp.cov > 0.999], 1.0), "letterbox: covered lights see the picture")
    check(min(mp.cov) == 0.0 and max(mp.cov) > 0.99, "letterbox: the bars are outside the picture (4:3 is narrower than 3:1)")
    wide = Mapper(300, 90, "crop")
    check(np.allclose(wide.average(np.ones((90, 300))), 1.0), "crop of a very wide picture")
    # a vertical gradient: crop shows the middle band, stretch the whole picture
    grad = np.tile(np.linspace(0, 1, 90)[:, None], (1, 120))
    a_crop, a_str = Mapper(120, 90, "crop").average(grad), Mapper(120, 90, "stretch").average(grad)
    check(abs(a_crop[0] - a_crop[-1]) < 0.4 and a_str[0] < 0.15 and a_str[-1] > 0.85, "crop keeps the middle, stretch squeezes all")
    # row 0 is the top of the picture
    top = np.zeros((90, 120)); top[:30, :] = 1.0
    st = Mapper(120, 90, "stretch").average(top)
    top[:18, :] = 1.0; top[18:, :] = 0.0                  # exactly the top fifth of the picture = the first row of lights
    st = Mapper(120, 90, "stretch").average(top)
    check(all(st[i] > 0.99 for i in range(14)) and all(st[i] < 0.01 for i in range(14, LIGHTS)), "stretch: top row of lights = top of the picture")

    # 6. quantizer
    qz = Quantizer(1, 2.2, 128, "none")
    check(qz.frame([0.0] * 30 + [1.0] * 31) == [0] * 30 + [1] * 31, "1 bit threshold")
    check(Quantizer(1, 2.2, 128, "none").frame([0.2] * 61) == [0] * 61 and Quantizer(1, 2.2, 128, "none").frame([0.25] * 61) == [1] * 61,
          "threshold 128 = 0.5 perceptual = 0.218 linear")
    check(Quantizer(1, 2.2, 250, "none").frame([0.9] * 61) == [0] * 61, "threshold option")
    check(Quantizer(2, 2.2, 128).frame([1.0] * 61) == [3] * 61 and Quantizer(4, 2.2, 128).frame([0.0] * 61) == [0] * 61, "range ends")
    check(Quantizer(4, 2.2, 128).frame([0.5 ** 2.2] * 61) == [8] * 61, "mid grey is step 8 of 15 (0.5 * 15 = 7.5 rounds up)")
    for dither in ("bayer", "fs"):
        q = Quantizer(1, 2.2, 128, dither).frame([0.5 ** 2.2] * 61)
        check(0 < sum(q) < 61, "dither %s produces a mix at mid grey (%d lit)" % (dither, sum(q)))
        q = Quantizer(2, 2.2, 128, dither).frame([1.0] * 61)
        check(q == [3] * 61, "dither %s keeps full white" % dither)
    h = Quantizer(2, 2.2, 128, "none", hysteresis=0.4)
    h.frame([0.5 ** 2.2 * 0.0] * 61)
    first = h.frame([0.3] * 61)
    again = h.frame([0.3] * 61)
    check(first == again, "hysteresis keeps a steady value steady")
    pal = default_palette(2)
    check(pal == sorted(set(pal)) and pal[0] == 0 and pal[-1] == 255, "palette %s" % pal)
    check(default_palette(1) == [0, 255] and len(default_palette(4)) == 16, "palettes")
    check(palette_squeezed(4, 40) and not palette_squeezed(4, 255) and default_palette(4, 40) == sorted(set(default_palette(4, 40))), "squeezed palette stays strictly increasing")
    raises(lambda: default_palette(4, max_level=0), "max-level 0")
    raises(lambda: default_palette(4, max_level=300), "max-level 300")

    # 7. brightness model matches the table of M6_PLAN.md section 5.6 (all white -> 17.9 %; 25 % lit -> 73 %)
    eff, s, ma = effective_levels([255] * 61)
    check(abs(s - 3060 / (61 * 280)) < 1e-6 and abs(ma - 180) < 0.01, "all white scale %.3f %.0f mA" % (s, ma))
    eff, s, ma = effective_levels([255] * 15 + [0] * 46)
    check(abs(s - 3060 / (15 * 280)) < 1e-6, "25 %% lit scale %.3f" % s)

    # 8. full pipeline on a made-up clip, no video needed
    tmp = Path(tempfile.mkdtemp(prefix="badapple_selftest_"))
    try:
        files = []
        for k, g in enumerate(synth_gray_frames(60, 120, 90, seed=3, cut_every=25)):
            p = tmp / ("f%04d.png" % k)
            Image.fromarray(g).save(p)
            files.append(p)
        check(len(image_files(str(tmp))) == 60 and len(image_files(str(tmp / "f00*.png"))) == 60, "image sequence discovery")
        out = tmp / "clip.kman"
        gif = tmp / "clip.gif"
        carr = tmp / "clip.c"
        rc = main(["--no-selftest-recursion", str(tmp), "--src-fps", "30", "--fps", "15", "--bits", "2", "-o", str(out), "--preview", str(gif),
                   "--c-array", str(carr), "--stats"], quiet=True)
        check(rc == 0 and out.exists() and gif.exists() and carr.exists(), "convert run")
        info, frames = decode(out.read_bytes())
        check(info["frames"] == 30 and info["fps"] == 15 and info["bits"] == 2, "30 frames at 15 fps from 60 at 30")
        check(info["palette"] == default_palette(2), "palette written")
        # inverted by default: the dark blobs of the made-up clip are the bright cells
        lit = sum(1 for fr in frames for q in fr if q) / (30 * LIGHTS)
        check(0.02 < lit < 0.95, "some cells lit (%.2f)" % lit)
        with Image.open(gif) as im:
            total_ms = 0
            for k in range(im.n_frames):
                im.seek(k)
                total_ms += im.info.get("duration", 0)
            # a GIF writer merges identical neighbouring pictures into one longer one, so only the total time is fixed
            check(im.size == (int(CANVAS_W * 24), int(CANVAS_H * 24)) and 1 < im.n_frames <= 30 and abs(total_ms - 2000) <= 20,
                  "preview gif %s %d frames %d ms" % (im.size, im.n_frames, total_ms))
        ctext = carr.read_text()
        arr = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-F]{2})", ctext))
        check(arr == out.read_bytes() and ("_len = %d;" % len(arr)) in ctext, "C array equals the file")
        # --no-invert is the complement of the default for a picture of hard black and white (cells that straddle the edge see grey)
        pic = np.zeros((90, 120), dtype=np.uint8); pic[:, :60] = 255
        base = build_parser().parse_args(["x", "--bits", "1"])
        base.invert = True
        a1 = quantize_clip(sample_clip([pic], base), base)[0]
        base.invert = False
        a2 = quantize_clip(sample_clip([pic], base), base)[0]
        clean = [i for i in range(LIGHTS) if not X8[i] / 8 - 0.5 < 7.5 < X8[i] / 8 + 0.5]
        check(all(a1[i] != a2[i] for i in clean) and sum(a1) > 10 and sum(a2) > 10, "--no-invert is the complement")
        check(all(a1[i] == 0 for i in left) and all(a1[i] == 1 for i in right), "default: the dark right half of the picture is lit")
        # a size limit is enforced
        rc = main([str(tmp), "--src-fps", "30", "--fps", "30", "--bits", "4", "--max-bytes", "100", "-o", str(tmp / "c.kman")], quiet=True)
        check(rc != 0, "--max-bytes refuses an oversize clip")
        # a corrupt file is refused by --check
        bad = bytearray(out.read_bytes()); bad[40] ^= 0x10
        (tmp / "bad.kman").write_bytes(bytes(bad))
        check(main(["--check", str(tmp / "bad.kman")], quiet=True) != 0 and main(["--check", str(out)], quiet=True) == 0, "--check")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("badapple_convert self-test: %d checks, %d failures" % (ok[0] + ok[1], ok[1]))
    return 1 if ok[1] else 0


# ---------------------------------------------------------------- command line

def build_parser():
    ap = argparse.ArgumentParser(description="video / image sequence -> KMAN clip for the 61 key lights")
    ap.add_argument("input", nargs="?", help="video file (needs ffmpeg), image folder or glob pattern")
    ap.add_argument("-o", "--out", default="badapple.kman", help="output clip (default badapple.kman)")
    ap.add_argument("--fps", type=int, default=15, help="frames per second of the clip, 1..60 (default 15)")
    ap.add_argument("--src-fps", type=float, default=30.0, help="frame rate of an image sequence (videos know their own)")
    ap.add_argument("--bits", type=int, choices=(1, 2, 4), default=2, help="grey steps: 1 = black/white, 2 = 4 steps, 4 = 16 steps (default 2)")
    ap.add_argument("--key-interval", type=int, default=30, help="a key frame every N frames, 1..255 (default 30)")
    ap.add_argument("--fit", choices=("crop", "stretch", "letterbox"), default="crop", help="how the 3:1 light field meets the picture (default crop)")
    ap.add_argument("--zoom", type=float, default=1.0, help="zoom into the picture (crop / letterbox)")
    ap.add_argument("--crop-x", type=float, default=0.5, help="horizontal centre of the window, 0..1")
    ap.add_argument("--crop-y", type=float, default=0.5, help="vertical centre of the window, 0..1")
    ap.add_argument("--invert", dest="invert", action="store_true", default=True, help="bright where the picture is dark (default)")
    ap.add_argument("--no-invert", dest="invert", action="store_false", help="keep the picture as it is (white background: dim, see notes)")
    ap.add_argument("--threshold", type=int, default=128, help="1 bit only: lit when the brightness (0..255, display coded) reaches this")
    ap.add_argument("--dither", choices=("none", "bayer", "fs"), default="none", help="ordered / error-diffusion dither (costs size)")
    ap.add_argument("--hysteresis", type=float, default=0.0, help="steps of grey a cell must move past the half-way point before it changes (cuts flicker)")
    ap.add_argument("--gamma", type=float, default=2.2, help="display gamma of the source and of the quantizer steps (default 2.2)")
    ap.add_argument("--max-level", type=int, default=255, help="grey level of the brightest sample (default 255; see the notes about the hardware limits)")
    ap.add_argument("--loop", action="store_true", help="set the loop hint in the header")
    ap.add_argument("--start", type=float, default=0.0, help="start second in the source")
    ap.add_argument("--duration", type=float, default=None, help="seconds to convert")
    ap.add_argument("--max-bytes", type=int, default=40000, help="refuse a clip bigger than this (the firmware image has room for about 40 KB)")
    ap.add_argument("--preview", metavar="GIF", help="also write a preview GIF decoded from the written clip")
    ap.add_argument("--preview-scale", type=int, default=24, help="pixels per key width in the preview")
    ap.add_argument("--preview-limited", action="store_true", help="preview with the hardware brightness limits applied (white lights)")
    ap.add_argument("--c-array", metavar="FILE.c", help="also write the clip as a C array")
    ap.add_argument("--symbol", default="km_anim_clip", help="name of the C array (default km_anim_clip)")
    ap.add_argument("--stats", action="store_true", help="print size, change and brightness statistics")
    ap.add_argument("--ffmpeg", help="path of ffmpeg (default: the one on PATH)")
    ap.add_argument("--check", metavar="FILE", help="strictly check a clip (as the firmware would) and exit")
    ap.add_argument("--self-test", action="store_true", help="run the built-in tests (no video needed) and exit")
    ap.add_argument("--synth-report", action="store_true", help="print the size of a made-up 3:39 clip at 10/15/30 fps (takes a minute)")
    ap.add_argument("--emit-test-vector", metavar="FILE.h", help="write the golden vectors header for firmware/tests/test_anim.c")
    ap.add_argument("--no-selftest-recursion", action="store_true", help=argparse.SUPPRESS)
    return ap


def main(argv=None, quiet=False):
    args = build_parser().parse_args(argv)
    say = (lambda *a, **k: None) if quiet else print
    err = say if quiet else (lambda *a: print(*a, file=sys.stderr))
    if args.self_test:
        return self_test()
    if args.synth_report:
        synth_size_report()
        return 0
    if args.emit_test_vector:
        Path(args.emit_test_vector).write_text(golden_header_text(), encoding="ascii", newline="\n")
        say("wrote", args.emit_test_vector)
        return 0
    if args.check:
        try:
            info, frames = decode(Path(args.check).read_bytes())
        except KmanError as e:
            err("BAD: %s" % e)
            return 1
        say("ok: %d frames, %d fps, %d bits" % (info["frames"], info["fps"], info["bits"]))
        return 0
    if not args.input:
        build_parser().print_usage()
        return 2
    if not 1 <= args.fps <= 60:
        err("--fps must be 1..60")
        return 2

    try:
        if is_image_sequence(args.input):
            files = image_files(args.input)
            if not files:
                err("no pictures found in", args.input)
                return 2
            gray = gray_frames_from_images(files, args.src_fps, args.fps, args.start, args.duration)
            lin = sample_clip(gray, args)
        else:
            ffmpeg = find_ffmpeg(args.ffmpeg)
            if not ffmpeg:
                err("ffmpeg is needed for video files and was not found (install it, or give --ffmpeg PATH, or export the video "
                    "to a folder of pictures first and pass the folder)")
                return 2
            gray = gray_frames_from_video(ffmpeg, args.input, args.fps, args.start, args.duration)
            lin = sample_clip(gray, args, progress=None if quiet else (lambda k: print("  %d frames ..." % k, flush=True)))
        if not lin:
            err("no frames were read")
            return 2
        frames = quantize_clip(lin, args)
        palette = default_palette(args.bits, args.max_level, args.gamma)
        if palette_squeezed(args.bits, args.max_level, args.gamma):
            err("note: --max-level %d is low for %d bits; the darkest grey steps were pushed apart by one level each" % (args.max_level, args.bits))
        data = encode(frames, args.bits, args.fps, args.key_interval, palette, 1 if args.loop else 0)
        info, check_frames = decode(data)
        assert check_frames == frames, "internal error: the clip does not decode to what was encoded"
    except KmanError as e:
        err("error:", e)
        return 1

    if len(data) > args.max_bytes:
        err("the clip is %d bytes, over --max-bytes %d: lower --fps or --bits, or shorten it" % (len(data), args.max_bytes))
        return 1
    Path(args.out).write_bytes(data)
    say("wrote %s: %d bytes, %d frames" % (args.out, len(data), len(frames)))
    if args.stats:
        say(stats_text(info, frames, len(data)))
    if args.preview:
        render_preview(args.preview, info, frames, args.preview_scale, args.preview_limited)
        say("wrote", args.preview)
    if args.c_array:
        write_c_array(args.c_array, data, args.symbol)
        say("wrote", args.c_array)
    return 0


if __name__ == "__main__":
    sys.exit(main())
