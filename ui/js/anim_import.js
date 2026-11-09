// Browser-only readers that turn the person's files into animations for the 61 lights (the maths is core/anim.js):
//   - a saved animation (.json written by this page),
//   - one picture, or several pictures taken as an image sequence (sorted by file name),
//   - an animated GIF / WebP / APNG (needs the browser's ImageDecoder; without it the page says so),
//   - a video (played frame by frame through a <video> element, at most MAX_VIDEO_S seconds).
// Everything stays in the browser: nothing is uploaded anywhere. The files are somebody's own and are not kept by the page.
import { sampleRgba, resampleByDurations, parseAnim, MAX_FRAMES, FPS_MAX } from "./core/anim.js";

export const MAX_VIDEO_S = 240;
export const SAMPLE_W = 240;                       // the picture is shrunk to this width before sampling; plenty for a 15 x 5 target
const kindOf = (f) => (/\.json$/i.test(f.name) ? "json" : /^video\//.test(f.type) || /\.(mp4|webm|mov|m4v|mkv|ogv)$/i.test(f.name) ? "video"
  : /image\/(gif|webp|apng)/.test(f.type) || /\.(gif|apng)$/i.test(f.name) ? "animated" : /^image\//.test(f.type) || /\.(png|jpe?g|bmp|webp|avif)$/i.test(f.name) ? "image" : "other");

export class ImportError extends Error {
  constructor(code, message) { super(message ?? code); this.name = "ImportError"; this.code = code; }
}

function toRgba(source, w, h) {
  const width = Math.min(SAMPLE_W, w), height = Math.max(1, Math.round((h * width) / w));
  const cv = typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(width, height) : Object.assign(document.createElement("canvas"), { width, height });
  const cx = cv.getContext("2d", { willReadFrequently: true });
  cx.fillStyle = "#000";
  cx.fillRect(0, 0, width, height);
  cx.drawImage(source, 0, 0, width, height);
  return { data: cx.getImageData(0, 0, width, height).data, w: width, h: height };
}

const tick = () => new Promise((r) => setTimeout(r, 0));
const natural = (a, b) => a.name.localeCompare(b.name, undefined, { numeric: true });

/**
 * files: File[] picked by the person. options: { pos, fit, fps, signal, onProgress(0..1) }.
 * Resolves { name, fps, frames } (see core/anim.js), rejects with ImportError (code: "empty" | "unsupported" | "no_decoder" | "too_long" | "bad_file" | "aborted").
 */
export async function importFiles(files, { pos, fit = "crop", fps = 15, signal, onProgress = () => {} } = {}) {
  const list = [...files].sort(natural);
  if (!list.length) throw new ImportError("empty");
  const kinds = list.map(kindOf);
  if (kinds.includes("other")) throw new ImportError("unsupported");
  const abort = () => { if (signal?.aborted) throw new ImportError("aborted"); };
  const name = list[0].name.replace(/\.[^.]+$/, "").slice(0, 60);

  if (kinds[0] === "json") {
    try { return parseAnim(await list[0].text()); } catch (err) { throw new ImportError("bad_file", err.message); }
  }
  if (kinds[0] === "video") return { name, ...(await readVideo(list[0], { pos, fit, fps, signal, onProgress, abort })) };
  if (kinds[0] === "animated" && list.length === 1) {
    const r = await readAnimated(list[0], { pos, fit, abort, onProgress });
    if (r) return { name, ...r };
  }
  // pictures: one or several (an image sequence), at the chosen rate
  const frames = [];
  for (let i = 0; i < Math.min(list.length, MAX_FRAMES); i++) {
    abort();
    const bmp = await createImageBitmap(list[i]).catch(() => { throw new ImportError("bad_file", list[i].name); });
    const px = toRgba(bmp, bmp.width, bmp.height);
    bmp.close?.();
    frames.push(sampleRgba(px.data, px.w, px.h, pos, { fit }));
    onProgress((i + 1) / list.length);
    if (i % 8 === 7) await tick();
  }
  return { name, fps: Math.max(1, Math.min(FPS_MAX, fps)), frames };
}

async function readAnimated(file, { pos, fit, abort, onProgress }) {
  if (typeof ImageDecoder === "undefined") return null;       // the caller falls back to the first picture only
  const type = file.type || (/\.gif$/i.test(file.name) ? "image/gif" : "image/png");
  let dec;
  try {
    dec = new ImageDecoder({ data: file.stream(), type });
    await dec.tracks.ready;
  } catch { return null; }
  const n = Math.min(dec.tracks.selectedTrack.frameCount, MAX_FRAMES);
  if (n < 2) { dec.close(); return null; }
  const frames = [], durs = [];
  for (let i = 0; i < n; i++) {
    abort();
    const { image } = await dec.decode({ frameIndex: i });
    const px = toRgba(image, image.displayWidth, image.displayHeight);
    durs.push((image.duration ?? 100000) / 1000);
    image.close();
    frames.push(sampleRgba(px.data, px.w, px.h, pos, { fit }));
    onProgress((i + 1) / n);
    if (i % 8 === 7) await tick();
  }
  dec.close();
  return resampleByDurations(frames, durs);
}

async function readVideo(file, { pos, fit, fps, onProgress, abort }) {
  const url = URL.createObjectURL(file);
  const v = document.createElement("video");
  v.muted = true; v.preload = "auto"; v.playsInline = true; v.src = url;
  try {
    await new Promise((res, rej) => { v.onloadeddata = res; v.onerror = () => rej(new ImportError("bad_file", file.name)); });
    const dur = v.duration;
    if (!(dur > 0) || !Number.isFinite(dur)) throw new ImportError("bad_file", file.name);
    if (dur > MAX_VIDEO_S) throw new ImportError("too_long", String(MAX_VIDEO_S));
    const rate = Math.max(1, Math.min(FPS_MAX, fps));
    const n = Math.min(MAX_FRAMES, Math.max(1, Math.floor(dur * rate)));
    const frames = [];
    for (let i = 0; i < n; i++) {
      abort();
      v.currentTime = Math.min(dur - 0.001, i / rate);
      await new Promise((res) => { v.onseeked = res; });
      const px = toRgba(v, v.videoWidth, v.videoHeight);
      frames.push(sampleRgba(px.data, px.w, px.h, pos, { fit }));
      onProgress((i + 1) / n);
      if (i % 4 === 3) await tick();
    }
    return { fps: rate, frames };
  } finally {
    URL.revokeObjectURL(url);
    v.removeAttribute("src");
    v.load();
  }
}
