// The animation page's engine: light positions, built-in demos, pictures -> frames, the look, the player clock, the file format, the
// readers' refusals, and the page's strings.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import {
  FRAME_BYTES, DEMOS, MAX_FRAMES, SPEED_RANGE, lightPositions, makeDemo, fitMapper, sampleRgba, resampleByDurations, applyLook, AnimPlayer,
  serializeAnim, parseAnim, blankFrame, hsv, isValidTint,
} from "../js/core/anim.js";
import { importFiles, ImportError } from "../js/anim_import.js";
import { loadLayout } from "../js/core/layout.js";
import zh from "../js/locales/zh-TW-anim.js";
import en from "../js/locales/en-anim.js";
import zhAll from "../js/locales/zh-TW.js";
import enAll from "../js/locales/en.js";

const layout = loadLayout();
const pos = lightPositions(layout);
const here = dirname(fileURLToPath(import.meta.url));
const pageSrc = readFileSync(join(here, "..", "js", "pages", "animation.js"), "utf8");

const solid = (w, h, [r, g, b, a = 255]) => { const d = new Uint8ClampedArray(w * h * 4); for (let i = 0; i < w * h; i++) d.set([r, g, b, a], i * 4); return d; };
const lum = (f, i) => f[i * 3];

test("61 light positions, inside the 15 x 5 key area, one per key row order (row 0 first)", () => {
  assert.equal(pos.length, 61);
  assert.ok(pos.every((p) => p.x > 0 && p.x < 15 && p.y > 0 && p.y < 5));
  assert.ok(pos[0].y < 1 && pos[60].y > 4, "light 0 is in the top row, light 60 in the bottom row");
  assert.equal(new Set(pos.map((p) => `${p.x},${p.y}`)).size, 61, "no two lights on the same spot");
});

test("demos: right size, alive, and the ripple loops without a seam", () => {
  for (const d of DEMOS) {
    const a = makeDemo(d.id, pos);
    assert.equal(a.frames.length, d.frames);
    assert.equal(a.fps, d.fps);
    assert.ok(a.frames.every((f) => f.length === FRAME_BYTES));
    assert.ok(a.frames.some((f) => f.some((v) => v > 0)), `${d.id} is not black`);
    assert.notDeepEqual([...a.frames[0]], [...a.frames[Math.floor(d.frames / 4)]], `${d.id} moves`);
  }
  const r = makeDemo("ripple", pos);
  const wrap = r.frames[0].reduce((s, v, i) => s + Math.abs(v - r.frames.at(-1)[i]), 0);
  const step = r.frames[0].reduce((s, v, i) => s + Math.abs(v - r.frames[1][i]), 0);
  assert.ok(wrap < step * 3, "the step from the last frame to the first is no bigger than a normal step");
  const b = makeDemo("badapple", pos);
  assert.ok(b.frames.every((f) => f.every((v, i) => v === f[i - (i % 3)])), "black and white demo is grey (r = g = b)");
  assert.ok(new Set(b.frames.flatMap((f) => [...f])).size <= 4, "at most 4 grey levels");
  assert.throws(() => makeDemo("nope", pos), RangeError);
});

test("hsv: primaries", () => {
  assert.deepEqual(hsv(0, 1, 1), [255, 0, 0]);
  assert.deepEqual(hsv(1 / 3, 1, 1), [0, 255, 0]);
  assert.deepEqual(hsv(0.5, 0, 1), [255, 255, 255]);
  assert.deepEqual(hsv(-1 / 3, 1, 1), [0, 0, 255], "hue wraps below 0");
});

test("fit: stretch / crop / letterbox map the 15 x 5 canvas onto a picture", () => {
  const stretch = fitMapper(300, 100, "stretch");
  assert.deepEqual(stretch(0, 0), [0, 0]);
  assert.deepEqual(stretch(7.5, 2.5), [150, 50]);
  const crop = fitMapper(400, 300, "crop");                       // 4:3 picture: width fills, top and bottom are cut
  const [sx, sy] = crop(7.5, 2.5);
  assert.equal(Math.round(sx), 200); assert.equal(Math.round(sy), 150);
  assert.ok(crop(0.01, 0.01)[1] > 50, "the top of the picture is cut off");
  const box = fitMapper(400, 300, "letterbox");                   // the whole picture fits the height: bars left and right
  assert.equal(box(0.5, 2.5), null);
  assert.ok(box(7.5, 0.01));
  assert.equal(box(14.9, 2.5), null);
});

test("sampling: a solid picture is a solid frame; transparent counts as black; halves land on the right lights", () => {
  const white = sampleRgba(solid(30, 10, [255, 255, 255]), 30, 10, pos, { fit: "stretch" });
  assert.ok(white.every((v) => v === 255));
  const clear = sampleRgba(solid(30, 10, [255, 255, 255, 0]), 30, 10, pos, { fit: "stretch" });
  assert.ok(clear.every((v) => v === 0));
  const half = new Uint8ClampedArray(30 * 10 * 4);
  for (let y = 0; y < 10; y++) for (let x = 0; x < 30; x++) half.set(x < 15 ? [255, 255, 255, 255] : [0, 0, 0, 255], (y * 30 + x) * 4);
  const f = sampleRgba(half, 30, 10, pos, { fit: "stretch" });
  pos.forEach((p, i) => { if (p.x < 6.5) assert.equal(lum(f, i), 255); if (p.x > 8.5) assert.equal(lum(f, i), 0); });
  const lb = sampleRgba(solid(10, 30, [255, 255, 255]), 10, 30, pos, { fit: "letterbox" });
  pos.forEach((p, i) => { if (p.x < 5 || p.x > 10) assert.equal(lum(lb, i), 0, "letterbox bars are black"); });
  assert.throws(() => sampleRgba(new Uint8ClampedArray(3), 4, 4, pos), RangeError);
});

test("sampling averages in linear light: a fine black/white checker is 186, not 128", () => {
  const w = 60, h = 20, d = new Uint8ClampedArray(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) d.set((x + y) % 2 ? [255, 255, 255, 255] : [0, 0, 0, 255], (y * w + x) * 4);
  const f = sampleRgba(d, w, h, pos, { fit: "stretch" });
  const mid = lum(f, 30);
  assert.ok(mid >= 180 && mid <= 192, `got ${mid}`);
});

test("animated pictures with their own delays become a constant frame rate", () => {
  const a = Uint8Array.of(1), b = Uint8Array.of(2), c = Uint8Array.of(3);
  const r = resampleByDurations([a, b, c], [100, 200, 100], null);
  assert.equal(r.fps, 10);
  assert.deepEqual(r.frames.map((f) => f[0]), [1, 2, 2, 3]);
  assert.equal(resampleByDurations([a, b], [10, 10]).fps, 30, "capped at 30 fps");
  assert.equal(resampleByDurations([a], [0]).frames.length, 1, "a zero delay counts as 100 ms");
  assert.ok(resampleByDurations(Array(5).fill(a), Array(5).fill(10 * 60 * 1000), 30).frames.length <= MAX_FRAMES);
});

test("look: brightness, invert and one colour", () => {
  const f = Uint8Array.from({ length: FRAME_BYTES }, (_, i) => (i % 3 === 0 ? 200 : i % 3 === 1 ? 100 : 0));
  assert.deepEqual([...applyLook(f, { brightness: 0.5 }).slice(0, 3)], [100, 50, 0]);
  assert.deepEqual([...applyLook(f, { invert: true }).slice(0, 3)], [55, 155, 255]);
  assert.deepEqual([...applyLook(f, { brightness: 0 }).slice(0, 3)], [0, 0, 0]);
  assert.deepEqual([...applyLook(f, { brightness: 5 }).slice(0, 3)], [200, 100, 0], "brightness is limited to 100 %");
  const white = new Uint8Array(FRAME_BYTES).fill(255);
  assert.deepEqual([...applyLook(white, { tint: "#ff8000" }).slice(0, 3)], [255, 128, 0]);
  assert.deepEqual([...applyLook(blankFrame(), { tint: "#ff8000" }).slice(0, 3)], [0, 0, 0]);
  assert.equal(applyLook(f).length, FRAME_BYTES);
  assert.ok(isValidTint("#00ff7f") && !isValidTint("red") && !isValidTint(null) && !isValidTint("#12345"));
});

test("player: plays in real time, loops, ends, pauses, stops, and ignores a long stall", () => {
  const frames = Array.from({ length: 10 }, (_, i) => Uint8Array.from({ length: FRAME_BYTES }, () => i * 20));
  const states = [];
  const p = new AnimPlayer({ onState: (x) => states.push(x.state) });
  assert.equal(p.tick(0), null, "nothing loaded");
  p.load({ name: "t", fps: 10, frames });
  p.blend = false;
  assert.equal(p.duration, 1);
  p.play(1000);
  assert.equal(p.tick(1000)[0], 0);
  assert.equal(p.tick(1200)[0], 40, "200 ms at 10 fps = frame 2");
  p.pause();
  assert.equal(p.tick(1900)[0], 40, "paused: the time does not run");
  p.play(2000);
  assert.equal(p.tick(2100)[0], 60, "resumes where it paused");
  assert.equal(p.tick(2250)[0], 80, "frame 4");
  p.setLoop(false);
  p.play(5000);
  for (let t = 5000; p.state === "playing"; t += 100) p.tick(t);
  assert.equal(p.state, "stopped", "ended without loop");
  assert.equal(p.pos, 9, "stays on the last frame");
  p.setLoop(true); p.setSpeed(2); p.play(9000);
  p.tick(9000); p.tick(9250);
  assert.ok(Math.abs(p.pos - 5) < 0.01, "double speed: 250 ms = 5 frames");
  const before = p.pos;
  p.tick(9250 + 60000);
  assert.ok(((p.pos - before + 10) % 10) <= 250 / 1000 * 10 * 2 + 0.01, "a one-minute stall advances at most 250 ms of the clip");
  p.stop();
  assert.equal(p.pos, 0);
  assert.ok(states.includes("playing") && states.includes("paused") && states.includes("stopped"));
  p.setSpeed(99); assert.equal(p.speed, SPEED_RANGE[1]);
  p.setSpeed(0.001); assert.equal(p.speed, SPEED_RANGE[0]);
});

test("player: blends neighbouring frames, and wraps the blend when looping", () => {
  const a = new Uint8Array(FRAME_BYTES).fill(0), b = new Uint8Array(FRAME_BYTES).fill(100);
  const p = new AnimPlayer();
  p.load({ name: "t", fps: 10, frames: [a, b] });
  assert.equal(p.frameAt(0.5)[0], 50);
  assert.equal(p.frameAt(1.5)[0], 50, "last frame blends towards the first when looping");
  p.setLoop(false);
  assert.equal(p.frameAt(1.5)[0], 100, "no wrap without loop");
  p.blend = false;
  assert.equal(p.frameAt(0.9)[0], 0);
  p.seek(0.5);
  assert.equal(p.pos, 1);
  p.seek(5);
  assert.ok(p.pos < 2);
});

test("animation files: round trip, and every kind of damage is refused with a reason", () => {
  const anim = { name: "mine", fps: 12, frames: [Uint8Array.from({ length: FRAME_BYTES }, (_, i) => i & 255), blankFrame()] };
  const back = parseAnim(serializeAnim(anim));
  assert.equal(back.name, "mine");
  assert.equal(back.fps, 12);
  assert.deepEqual(back.frames.map((f) => [...f]), anim.frames.map((f) => [...f]));
  const o = JSON.parse(serializeAnim(anim));
  const bad = (patch) => assert.throws(() => parseAnim(typeof patch === "string" ? patch : JSON.stringify({ ...o, ...patch })), RangeError);
  bad("not json"); bad("[]"); bad({ format: "other" }); bad({ version: 2 }); bad({ lights: 60 }); bad({ fps: 0 }); bad({ fps: 31 }); bad({ fps: 1.5 });
  bad({ frames: [] }); bad({ frames: [1] }); bad({ frames: ["AAAA"] }); bad({ frames: ["***"] }); bad({ frames: Array(MAX_FRAMES + 1).fill(o.frames[0]) });
  assert.equal(parseAnim(JSON.stringify({ ...o, name: 5 })).name, "5");
});

test("the file readers refuse what they cannot read, before touching anything", async () => {
  const f = (name, type = "", text = "") => ({ name, type, text: async () => text });
  await assert.rejects(importFiles([], { pos }), (e) => e instanceof ImportError && e.code === "empty");
  await assert.rejects(importFiles([f("a.txt")], { pos }), (e) => e.code === "unsupported");
  await assert.rejects(importFiles([f("a.png", "image/png"), f("b.exe")], { pos }), (e) => e.code === "unsupported");
  await assert.rejects(importFiles([f("a.json", "", "garbage")], { pos }), (e) => e.code === "bad_file");
  const anim = { name: "x", fps: 5, frames: [blankFrame()] };
  const got = await importFiles([f("kept.json", "application/json", serializeAnim(anim))], { pos });
  assert.equal(got.fps, 5);
});

test("strings: both languages have the same keys and every key the page uses", () => {
  assert.deepEqual(Object.keys(en).sort(), Object.keys(zh).sort());
  for (const k of Object.keys(zh)) assert.ok(zh[k].trim() && en[k].trim(), k);
  const used = new Set([...pageSrc.matchAll(/\bt\("(anim\.[\w.]+)"/g)].map((m) => m[1]));
  for (const d of DEMOS) { used.add(`anim.demo.${d.id}`); used.add(`anim.demo.${d.id}.d`); }
  for (const f of ["crop", "stretch", "letterbox"]) { used.add(`anim.fit.${f}`); used.add(`anim.fit.${f}.d`); }
  for (const k of ["empty", "unsupported", "bad_file", "too_long", "no_decoder"]) used.add(`anim.err.${k}`);
  for (const k of ["unavailable", "silent", "lost", "unsupported", "other"]) used.add(`anim.st.err.${k}`);
  for (const k of ["offline", "stock", "old"]) used.add(`anim.na.${k}`);
  for (const k of used) assert.ok(k in zh, `missing in zh-TW-anim: ${k}`);
  const unused = Object.keys(zh).filter((k) => !used.has(k) && k !== "nav.animation");
  assert.deepEqual(unused.filter((k) => !pageSrc.includes(k)), [], "strings nobody uses");
  assert.equal(zhAll["nav.animation"], "動畫");
  assert.equal(enAll["nav.animation"], "Animation");
  assert.equal(zhAll["anim.play"], zh["anim.play"]);
});

test("the page tells the truth about saving into the keyboard, and has no way to do it", () => {
  assert.match(en["anim.keep.nofw"], /cannot store/);
  assert.match(pageSrc, /saveBtn = h\("button\.btn", \{ type: "button", disabled: true/);
  assert.ok(!/streamPort\(\)\.send|\.send\(/.test(pageSrc), "the page never builds packets itself: it only uses the stream controller");
});

test("leak: the model blurs, anti-bleed sharpens against it, 0 is exactly off, everything stays 0..255", async () => {
  const { leakKernel, simulateLeak, applyAntiBleed, applyContrast, applyLook, LEAK_GAIN } = await import("../js/core/anim.js");
  const kernel = leakKernel(pos);
  const one = blankFrame(); one.set([255, 255, 255], 30 * 3);
  assert.equal(applyAntiBleed(one, kernel, 0), one, "strength 0 returns the frame untouched");
  assert.equal(applyContrast(one, 0), one);
  assert.deepEqual([...applyLook(one, { antiBleed: 0, contrast: 0, kernel })], [...applyLook(one)], "off = the old output");
  const leaked = simulateLeak(one, kernel);
  const nearest = kernel[30].reduce((a, b) => (b[1] > a[1] ? b : a));
  assert.equal(leaked[90], 255);
  assert.equal(leaked[nearest[0] * 3], Math.round(255 * LEAK_GAIN * nearest[1]), "the closest neighbour shows part of the lit light");
  assert.ok(leaked.some((v, i) => Math.floor(i / 3) !== 30 && v === 0), "far lights stay dark");
  assert.equal(applyAntiBleed(one, kernel, 1)[90], 255, "an isolated light is not dimmed");
  const edge = blankFrame();
  pos.forEach((p, i) => { if (p.x < 7.5) edge.set([255, 255, 255], i * 3); });
  const darkSide = pos.map((p, i) => [p, i]).filter(([p]) => p.x >= 7.5 && p.x < 9);
  const bleed = (f) => darkSide.reduce((s, [, i]) => s + f[i * 3], 0);
  const plain = simulateLeak(edge, kernel), fixed = simulateLeak(applyAntiBleed(edge, kernel, 1), kernel);
  assert.ok(bleed(plain) > 0 && bleed(fixed) <= bleed(plain), "less light leaks into the dark side");
  const lit = pos.map((p, i) => [p, i]).filter(([p]) => p.x < 6);
  const sum = (f) => lit.reduce((s, [, i]) => s + f[i * 3], 0);
  assert.ok(sum(fixed) >= sum(edge) * 0.9, "as the keyboard shows it (after the leak) the bright side is still as bright as meant");
  const noise = Uint8Array.from({ length: FRAME_BYTES }, (_, i) => (i * 37) % 256);
  for (const f of [simulateLeak(noise, kernel), applyAntiBleed(noise, kernel, 1), applyContrast(noise, 1)]) assert.equal(f.length, FRAME_BYTES);
  const c = applyContrast(Uint8Array.from({ length: FRAME_BYTES }, (_, i) => (i % 3 === 0 ? 0 : i % 3 === 1 ? 128 : 255)), 1);
  assert.deepEqual([...c.slice(0, 3)], [0, 64, 255], "gamma 2: 128 -> 64, ends stay");
});

test("afterglow: a pure function of the position (scrub, loop and speed safe), never brighter than the clip", async () => {
  const { trailFrame } = await import("../js/core/anim.js");
  const dark = blankFrame(), lit = new Uint8Array(FRAME_BYTES).fill(200);
  const frames = [lit, dark, dark, dark];
  const p = new AnimPlayer();
  p.load({ name: "t", fps: 10, frames });
  p.blend = false;
  assert.deepEqual([...p.frameAt(2)], [...dark], "off: cuts off");
  p.trailS = 0.4;
  const a = p.frameAt(1), b = p.frameAt(2), c = p.frameAt(3);
  assert.ok(a[0] > b[0] && b[0] > c[0] && c[0] > 0, "fades out step by step");
  assert.ok(a[0] < 200);
  assert.deepEqual([...p.frameAt(2)], [...b], "same position, same picture, no hidden state");
  p.pos = 3; p.frameAt(0); p.frameAt(3.5);
  assert.deepEqual([...p.frameAt(2)], [...b], "scrubbing around changes nothing");
  p.setLoop(false);
  assert.equal(p.frameAt(0)[0], 200);
  const loopTail = new AnimPlayer(); loopTail.load({ name: "t", fps: 10, frames: [dark, dark, lit] }); loopTail.blend = false; loopTail.trailS = 0.4;
  assert.ok(loopTail.frameAt(0)[0] > 0, "looping: the end of the clip glows into the start");
  loopTail.setLoop(false);
  assert.equal(loopTail.frameAt(0)[0], 0, "no loop: nothing before the start");
  assert.equal(trailFrame(frames, 2, 10, 0, true, dark), dark);
  assert.ok(p.frameAt(1).every((v) => v <= 200));
});

test("the page has the new controls with plain names in both languages, and no control that blurs", () => {
  assert.match(zh["anim.anti"], /防漏光/);
  assert.match(zh["anim.contrast"], /黑色更黑/);
  assert.ok(!("anim.bloom" in zh) && !/bloom/.test(pageSrc));
  assert.match(pageSrc, /anim-leak/);
  assert.match(zh["anim.trail"], /殘影/);
  assert.match(pageSrc, /anim-trail/);
});
