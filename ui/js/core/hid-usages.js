// Everything a key can be bound to. A binding is a small object (our own format, see ui/README.md):
//   {type: "key", usage, mods?: [modifier usages]}   USB HID keyboard usage (modifiers are usages 224..231)
//   {type: "media", usage}                           USB HID consumer usage
//   {type: "mouse", button: 1..5}   {type: "wheel", dir: "up"|"down"}   {type: "system", op: "power"|"sleep"|"wake"}
//   {type: "layer", layer: 1..3}                     hold to use that layer (Fn)
//   {type: "toggle", layer: 1..3}                    press to switch that layer on / off until pressed again (or the profile changes)
//   {type: "oneshot", usage: 224..231}               a modifier that stays on for the NEXT key after a tap (held: an ordinary modifier)
//   {type: "taphold", tap, hold}                     tap = {type: "key"|"media", usage}, hold = {type: "layer", layer} or {type: "key", usage: 224..231}
//   {type: "none"}                                   the key does nothing on this layer
//   {type: "profile", index: 1..4}                   switch profile
// `null` means "transparent" (on layers 1..3 the key of the layer below; on layer 0: no key). Usage numbers are the public USB HID Usage Tables values.

const KEY_NAMES = {};
for (let i = 0; i < 26; i++) KEY_NAMES[4 + i] = String.fromCharCode(65 + i);
"1234567890".split("").forEach((c, i) => (KEY_NAMES[30 + i] = c));
Object.assign(KEY_NAMES, {
  40: "Enter", 41: "Esc", 42: "Backspace", 43: "Tab", 44: "Space", 45: "- _", 46: "= +", 47: "[ {", 48: "] }",
  49: "\\ |", 51: "; :", 52: "' \"", 53: "` ~", 54: ", <", 55: ". >", 56: "/ ?", 57: "Caps Lock",
  70: "Print Screen", 71: "Scroll Lock", 72: "Pause", 73: "Insert", 74: "Home", 75: "Page Up", 76: "Delete",
  77: "End", 78: "Page Down", 79: "→", 80: "←", 81: "↓", 82: "↑", 83: "Num Lock", 84: "Keypad /", 85: "Keypad *",
  86: "Keypad -", 87: "Keypad +", 88: "Keypad Enter", 99: "Keypad .", 100: "Intl \\", 101: "Menu",
});
for (let i = 1; i <= 12; i++) KEY_NAMES[57 + i] = `F${i}`;
for (let i = 13; i <= 24; i++) KEY_NAMES[104 + i - 13] = `F${i}`;
for (let i = 1; i <= 9; i++) KEY_NAMES[88 + i] = `Keypad ${i}`;
KEY_NAMES[98] = "Keypad 0";
const MOD_NAMES = ["Left Ctrl", "Left Shift", "Left Alt", "Left Win", "Right Ctrl", "Right Shift", "Right Alt", "Right Win"];
MOD_NAMES.forEach((n, i) => (KEY_NAMES[224 + i] = n));

const GROUPS = {
  letters: (u) => u >= 4 && u <= 29,
  numbers: (u) => u >= 30 && u <= 39,
  punct: (u) => (u >= 44 && u <= 49) || (u >= 51 && u <= 56) || (u >= 40 && u <= 43) || u === 100,
  func: (u) => (u >= 58 && u <= 69) || (u >= 104 && u <= 115),
  nav: (u) => u === 57 || (u >= 70 && u <= 82) || u === 101,
  keypad: (u) => u === 83 || (u >= 84 && u <= 99),
  modifiers: (u) => u >= 224 && u <= 231,
};

/** Stable text id of a binding, used to compare and to mark the active entry in the list. */
export function bindingId(b) {
  if (!b) return "none";
  if (b.type === "none") return "off";
  if (b.type === "taphold") return `taphold:${bindingId(b.tap)}>${bindingId(b.hold)}`;
  const v = b.usage ?? b.button ?? b.dir ?? b.op ?? b.layer ?? b.index;
  return `${b.type}:${v}${b.mods?.length ? "+" + b.mods.join(",") : ""}`;
}

export const sameBinding = (a, b) => bindingId(a) === bindingId(b);

const items = [];
const add = (binding, name, group) => items.push({ id: bindingId(binding), binding, name, group });

for (const [uStr, name] of Object.entries(KEY_NAMES)) {
  const u = Number(uStr);
  add({ type: "key", usage: u }, name, Object.keys(GROUPS).find((g) => GROUPS[g](u)) ?? "punct");
}

[["Play / Pause", 0xcd], ["Stop", 0xb7], ["Next track", 0xb5], ["Previous track", 0xb6], ["Volume up", 0xe9],
  ["Volume down", 0xea], ["Mute", 0xe2], ["Brightness up", 0x6f], ["Brightness down", 0x70],
  ["Calculator", 0x192], ["Mail", 0x18a], ["File explorer", 0x194], ["Browser home", 0x223],
  ["Browser back", 0x224], ["Browser forward", 0x225], ["Browser refresh", 0x227], ["Media player", 0x183],
].forEach(([name, usage]) => add({ type: "media", usage }, name, "media"));

[["System power", "power"], ["System sleep", "sleep"], ["System wake", "wake"]].forEach(([name, op]) => add({ type: "system", op }, name, "system"));

[["Mouse left", 1], ["Mouse right", 2], ["Mouse middle", 3], ["Mouse back", 4], ["Mouse forward", 5]].forEach(([name, button]) =>
  add({ type: "mouse", button }, name, "mouse"));
add({ type: "wheel", dir: "up" }, "Wheel up", "mouse");
add({ type: "wheel", dir: "down" }, "Wheel down", "mouse");

add({ type: "layer", layer: 1 }, "Hold layer 1", "special");
add({ type: "layer", layer: 2 }, "Hold layer 2", "special");
add({ type: "layer", layer: 3 }, "Hold layer 3", "special");
for (let l = 1; l <= 3; l++) add({ type: "toggle", layer: l }, `Toggle layer ${l}`, "special");
MOD_NAMES.forEach((n, i) => add({ type: "oneshot", usage: 224 + i }, `One-shot ${n}`, "oneshot"));
add({ type: "none" }, "No key", "special");
for (let p = 1; p <= 4; p++) add({ type: "profile", index: p }, `Switch to profile ${p}`, "special");
add(null, "Transparent", "special"); // overlay layers only: the key of the layer below

export const ALL_USAGES = items;
export const GROUP_ORDER = ["letters", "numbers", "punct", "func", "nav", "keypad", "modifiers", "oneshot", "media", "system", "mouse", "special"];

/** Tap actions a tap-hold key can have (a keyboard usage or one of the media keys the keyboard stores) and its hold actions. */
export const TAP_MEDIA_USAGES = [0xea, 0xe9, 0xe2, 0xb6, 0xcd, 0xb5];
export const MODIFIER_USAGES = [224, 225, 226, 227, 228, 229, 230, 231];
export const modifierName = (u) => MOD_NAMES[u - 224];
export const keyName = (u) => KEY_NAMES[u] ?? `0x${u.toString(16)}`;
const BY_ID = new Map(items.map((i) => [i.id, i]));
export const findUsage = (b) => (b ? BY_ID.get(bindingId(b)) ?? null : null);

/** Binding that makes a key send its own HID usage (what a default layout holds). Fn (usage null) is the layer key. */
export const bindingFromUsage = (usage) => (usage === null ? { type: "layer", layer: 1 } : { type: "key", usage });

/** Validation shape check for imported bindings. Returns true when `b` is a well-formed binding or null. */
export function isValidBinding(b) {
  if (b === null) return true;
  if (!b || typeof b !== "object") return false;
  const int = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;
  switch (b.type) {
    case "key": return int(b.usage, 0, 0xff) && (b.mods === undefined || (Array.isArray(b.mods) && b.mods.length <= 8 && b.mods.every((m) => int(m, 224, 231))));
    case "media": return int(b.usage, 0, 0xffff);
    case "mouse": return int(b.button, 1, 5);
    case "wheel": return b.dir === "up" || b.dir === "down";
    case "system": return ["power", "sleep", "wake"].includes(b.op);
    case "layer": return int(b.layer, 1, 3);
    case "toggle": return int(b.layer, 1, 3);
    case "oneshot": return int(b.usage, 224, 231);
    case "taphold": {
      const tapOk = b.tap && ((b.tap.type === "key" && int(b.tap.usage, 4, 0xe7) && !b.tap.mods?.length) || (b.tap.type === "media" && TAP_MEDIA_USAGES.includes(b.tap.usage)));
      const holdOk = b.hold && ((b.hold.type === "layer" && int(b.hold.layer, 1, 3)) || (b.hold.type === "key" && int(b.hold.usage, 224, 231) && !b.hold.mods?.length));
      return Boolean(tapOk && holdOk);
    }
    case "none": return true;
    case "profile": return int(b.index, 1, 4);
    case "fw": return int(b.index, 0, 0xffffff); // an opaque firmware action (DKS, Mod-Morph, toggle key, gamepad ...), see core/behaviors.js
    default: return false;
  }
}

/** Keep only the fields our format defines. */
export function cleanBinding(b) {
  if (!b) return null;
  const out = { type: b.type };
  for (const f of ["usage", "button", "dir", "op", "layer", "index"]) if (b[f] !== undefined) out[f] = b[f];
  if (b.mods?.length) out.mods = [...b.mods];
  if (b.type === "taphold") { out.tap = cleanBinding(b.tap); out.hold = cleanBinding(b.hold); }
  return out;
}

/** Full-length text for lists and tooltips. Unknown actions fall back to their id. */
export function bindingLabel(b) {
  if (!b) return "";
  const known = findUsage(b);
  if (known) return known.name;
  if (b.type === "taphold") return `Tap ${bindingLabel(b.tap)} / Hold ${bindingLabel(b.hold)}`;
  if (b.type === "key" && b.mods?.length) {
    return `${b.mods.map((m) => MOD_NAMES[m - 224].replace("Left ", "L").replace("Right ", "R")).join("+")}+${KEY_NAMES[b.usage] ?? b.usage}`;
  }
  return bindingId(b);
}

const CAP_ABBR = {
  "Caps Lock": "Caps", "Print Screen": "PrtSc", "Scroll Lock": "ScrLk", "Page Up": "PgUp", "Page Down": "PgDn",
  "Num Lock": "NumLk", "Backspace": "Bksp", "Insert": "Ins", "Delete": "Del", "Keypad Enter": "KP Ent",
  "Play / Pause": "⏯", "Next track": "⏭", "Previous track": "⏮", "Volume up": "Vol+", "Volume down": "Vol−",
  "Mute": "Mute", "Stop": "Stop", "Brightness up": "Bri+", "Brightness down": "Bri−", "Calculator": "Calc",
  "File explorer": "Files", "Browser home": "Home", "Browser back": "Back", "Browser forward": "Fwd",
  "Browser refresh": "Refresh", "Media player": "Media", "System power": "Power", "System sleep": "Sleep",
  "System wake": "Wake", "Mouse left": "M1", "Mouse right": "M2", "Mouse middle": "M3", "Mouse back": "M4",
  "Mouse forward": "M5", "Wheel up": "Whl↑", "Wheel down": "Whl↓", "Hold layer 1": "Hold L1",
  "Hold layer 2": "Hold L2", "Hold layer 3": "Hold L3", "Transparent": "▽", "No key": "–",
};

let fwCapText = () => "•";
/** core/behaviors.js registers the cap text of firmware actions ({type: "fw"}) here (it imports this file, so it cannot be imported back). */
export const setFwCapLabel = (fn) => { fwCapText = fn; };

/** Compact text for a key cap (fits a 1u key). */
export function capLabel(b) {
  if (b?.type === "fw") return fwCapText(b);
  if (b?.type === "taphold") {
    const hold = b.hold.type === "layer" ? `L${b.hold.layer}` : capLabel(b.hold);
    return `${capLabel(b.tap)}/${hold}`;
  }
  if (b?.type === "toggle") return `TG${b.layer}`;
  if (b?.type === "oneshot") return `OS ${capLabel({ type: "key", usage: b.usage })}`;
  const u = findUsage(b);
  if (!u) return bindingLabel(b);
  if (u.group === "modifiers") return u.name.replace("Left ", "L").replace("Right ", "R");
  if (CAP_ABBR[u.name]) return CAP_ABBR[u.name];
  return u.name.replace(/^Keypad /, "KP ").replace(/^Switch to profile (\d)/, "P$1");
}

/** Text typed in the search box, matched against name and group (case-insensitive, all words must hit). */
export function searchUsages(query, groupLabel = (g) => g) {
  const words = query.toLowerCase().split(/\s+/).filter(Boolean);
  if (!words.length) return items;
  return items.filter((it) => {
    const hay = `${it.name} ${groupLabel(it.group)} ${it.group}`.toLowerCase();
    return words.every((w) => hay.includes(w));
  });
}
