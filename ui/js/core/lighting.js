// Lighting model: our own effect list, colour helpers and a browser preview. Whether the device implements
// an effect is up to its firmware; the preview is only an approximation.

export const EFFECTS = [
  { id: "custom", speed: false, color: false },
  { id: "static", speed: false, color: true },
  { id: "breathing", speed: true, color: true },
  { id: "spectrum", speed: true, color: false },
  { id: "wave", speed: true, color: true },
  { id: "waveV", speed: true, color: true },
  { id: "ripple", speed: true, color: true },
  { id: "reactive", speed: true, color: true },
  { id: "rain", speed: true, color: true },
  { id: "starlight", speed: true, color: true },
];

/** Effects whose motion we can preview in the browser. The rest show a static approximation. */
export const PREVIEWABLE = new Set(["custom", "static", "breathing", "spectrum", "wave", "waveV"]);

export const SPEED_RANGE = { min: 1, max: 5 }; // assumed ranges, not tied to any device yet
export const effectById = (id) => EFFECTS.find((e) => e.id === id) ?? EFFECTS[0];

export function hexToRgb(hex) {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex);
  if (!m) return null;
  const n = parseInt(m[1], 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}
export const rgbToHex = ([r, g, b]) => "#" + [r, g, b].map((v) => Math.round(v).toString(16).padStart(2, "0")).join("");
export const isHexColor = (s) => typeof s === "string" && /^#[0-9a-f]{6}$/i.test(s);

function hsv(h, s, v) {
  const f = (n) => {
    const k = (n + h / 60) % 6;
    return v - v * s * Math.max(0, Math.min(k, 4 - k, 1));
  };
  return [f(5) * 255, f(3) * 255, f(1) * 255];
}

/** Colour of a key at time t (seconds) for the preview. x, y are the key centre in key units. */
export function previewColor(effectId, { x, y, t, speed, color, custom }) {
  const rate = 0.25 + speed * 0.25;
  switch (effectId) {
    case "custom": return custom;
    case "spectrum": return hsv(((x * 14 + t * 60 * rate) % 360 + 360) % 360, 1, 1);
    case "breathing": {
      const k = 0.5 - 0.5 * Math.cos(t * Math.PI * rate);
      return color.map((c) => c * (0.15 + 0.85 * k));
    }
    case "wave": {
      const k = 0.5 + 0.5 * Math.sin(x * 0.55 - t * 3 * rate);
      return color.map((c) => c * (0.1 + 0.9 * k));
    }
    case "waveV": {
      const k = 0.5 + 0.5 * Math.sin(y * 1.1 - t * 3 * rate);
      return color.map((c) => c * (0.1 + 0.9 * k));
    }
    default: return color;
  }
}
