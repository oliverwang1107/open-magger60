#!/usr/bin/env python3
"""make_local_clip.py - turn a video you own into a LOCAL clip for the configurator's animation page (never published).

    python tools/make_local_clip.py my_video.mp4 --name "My clip" --out ui/local/clips --id myclip [--fps 15] [--fit crop] [--no-invert]

Writes into --out (default ui/local/clips):
    <id>.json   the animation page's own file format (core/anim.js serializeAnim): 61 lights per frame, grey, 15 fps by default
    <id>.m4a    the sound track (AAC, from the same video), played in sync by the page
    index.json  the list the page reads at load (./local/clips/index.json); this script adds / replaces its own entry

The page looks for ui/local/clips/index.json when it opens; when the file is missing (every public deployment) it shows nothing.
ui/local/ is in .gitignore, excluded by PUBLISH_MANIFEST.txt and not in the deploy whitelist (ui/tests/local-clips.test.mjs proves all three):
a video and its music are somebody else's work and nothing made from them may be committed or published.

The picture is mapped onto the real light positions the same way the page's own import does it: the 15 x 5 key canvas, "crop" fit (fills the strip,
cuts what sticks out), the area average of a 1 x 1 key square around every light taken in linear light. White-background clips are inverted
by default (the figure becomes the bright part), because the keyboard's current limit makes a mostly white strip very dim.
Needs: Python 3.8+, numpy, ffmpeg and ffprobe in PATH (or --ffmpeg-dir).
"""
import argparse
import base64
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("badapple_convert", ROOT / "tools" / "badapple_convert.py")
conv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(conv)          # only for the light layout constants (X8, ROW_START), checked there against km_led.c

LIGHTS = 61
CANVAS_W, CANVAS_H = 15.0, 5.0
MAX_FRAMES = 7200                      # the page's limit (core/anim.js MAX_FRAMES)


def light_positions():
    rows = []
    for r in range(5):
        rows += [r] * (conv.ROW_START[r + 1] - conv.ROW_START[r])
    return [(x8 / 8.0, r + 0.5) for x8, r in zip(conv.X8, rows)]


def sample_index(pos, w, h, fit, n=4):
    """(61, n*n) flat source pixel index for every sample point; -1 = outside the picture (black bar). Mirrors core/anim.js fitMapper + sampleRgba."""
    if fit == "stretch":
        sx_of = lambda u, v: (u / CANVAS_W * w, v / CANVAS_H * h)
    else:
        scale = (min if fit == "letterbox" else max)(CANVAS_W / w, CANVAS_H / h)
        ox, oy = (CANVAS_W - w * scale) / 2, (CANVAS_H - h * scale) / 2
        sx_of = lambda u, v: ((u - ox) / scale, (v - oy) / scale)
    idx = np.full((LIGHTS, n * n), -1, dtype=np.int64)
    for i, (px, py) in enumerate(pos):
        k = 0
        for a in range(n):
            for c in range(n):
                sx, sy = sx_of(px - 0.5 + (c + 0.5) / n, py - 0.5 + (a + 0.5) / n)
                if 0 <= sx < w and 0 <= sy < h:
                    idx[i, k] = min(h - 1, int(sy)) * w + min(w - 1, int(sx))
                k += 1
    return idx


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("video")
    ap.add_argument("--out", default=str(ROOT / "ui" / "local" / "clips"))
    ap.add_argument("--id", default=None, help="file name stem (default: the video's name, simplified)")
    ap.add_argument("--name", default=None, help="name shown on the page")
    ap.add_argument("--fps", type=int, default=15)
    ap.add_argument("--fit", choices=["crop", "stretch", "letterbox"], default="crop")
    ap.add_argument("--no-invert", action="store_true")
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--ffmpeg-dir", default=None)
    a = ap.parse_args()
    exe = lambda n: str(Path(a.ffmpeg_dir) / n) if a.ffmpeg_dir else n
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    stem = a.id or "clip"
    if not 1 <= a.fps <= 30:
        sys.exit("--fps must be 1..30")

    pr = subprocess.run([exe("ffprobe"), "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0:s=x", a.video],
                        capture_output=True, text=True, check=True)
    vw, vh = (int(x) for x in pr.stdout.strip().split("x"))
    w = 240
    h = max(2, round(vh * w / vw) // 2 * 2)
    idx = sample_index(light_positions(), w, h, a.fit)
    valid = idx >= 0
    lin = (np.arange(256) / 255.0) ** 2.2
    lut_lin = np.append(lin, 0.0)                    # index 256 = outside the picture
    flat = np.where(valid, idx, -1)

    cmd = [exe("ffmpeg"), "-v", "error", "-i", a.video, "-an", "-vf", f"fps={a.fps},scale={w}:{h}", "-pix_fmt", "gray", "-f", "rawvideo", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    frames = []
    size = w * h
    while True:
        buf = p.stdout.read(size)
        if len(buf) < size:
            break
        g = np.frombuffer(buf, dtype=np.uint8)
        vals = np.where(flat >= 0, g[np.maximum(flat, 0)], 256)
        l = lut_lin[vals].mean(axis=1)
        v = np.clip(255.0 * l ** (1 / 2.2) + 0.5, 0, 255).astype(np.uint8)
        if not a.no_invert:
            v = 255 - v
        frames.append(np.repeat(v, 3).tobytes())
        if len(frames) > MAX_FRAMES:
            sys.exit(f"more than {MAX_FRAMES} frames: lower --fps or cut the video")
    p.wait()
    if p.returncode or not frames:
        sys.exit("ffmpeg could not read the video")

    name = a.name or stem
    (out / f"{stem}.json").write_text(json.dumps({
        "format": "magger60-anim", "version": 1, "name": name[:60], "fps": a.fps, "lights": LIGHTS,
        "frames": [base64.b64encode(f).decode("ascii") for f in frames]}), encoding="utf8")
    entry = {"name": name, "anim": f"{stem}.json"}
    if not a.no_audio:
        r = subprocess.run([exe("ffmpeg"), "-v", "error", "-y", "-i", a.video, "-vn", "-c:a", "aac", "-b:a", "128k", str(out / f"{stem}.m4a")])
        if r.returncode == 0:
            entry["audio"] = f"{stem}.m4a"
        else:
            print("no audio track extracted")
    idx_path = out / "index.json"
    try:
        entries = json.loads(idx_path.read_text(encoding="utf8"))
        entries = [e for e in entries if isinstance(e, dict) and e.get("anim") != entry["anim"]]
    except (OSError, ValueError):
        entries = []
    idx_path.write_text(json.dumps(entries + [entry], ensure_ascii=False, indent=1), encoding="utf8")
    print(f"{len(frames)} frames at {a.fps} fps ({len(frames) / a.fps:.1f} s) -> {out / (stem + '.json')}")
    print("These files are local only: do not commit them (ui/local/ is ignored).")


if __name__ == "__main__":
    main()
