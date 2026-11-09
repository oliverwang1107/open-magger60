// Which sensor slot (index into the live depth array) belongs to which key. This is a property of the
// board's wiring, not of the UI, so it is kept apart from the layout: when the custom firmware publishes its
// own key table, replace this file with that table and nothing else in the UI changes.
// Source: the project's own board analysis (slot table in FIRMWARE_Magger60.md, section 3.2).

export const SLOT_BY_KEY_ID = {
  Escape: 18, Digit1: 56, Digit2: 22, Digit3: 4, Digit4: 60, Digit5: 31, Digit6: 67, Digit7: 70, Digit8: 19,
  Digit9: 55, Digit0: 57, Minus: 3, Equal: 26, Backspace: 8,
  Tab: 54, KeyQ: 2, KeyW: 75, KeyE: 6, KeyR: 42, KeyT: 13, KeyY: 16, KeyU: 37, KeyI: 1, KeyO: 21, KeyP: 39,
  BracketLeft: 62, BracketRight: 44, Backslash: 64,
  CapsLock: 36, KeyA: 73, KeyS: 72, KeyD: 74, KeyF: 49, KeyG: 34, KeyH: 52, KeyJ: 9, KeyK: 47, KeyL: 12,
  Semicolon: 48, Quote: 32, Enter: 10,
  ShiftLeft: 0, KeyZ: 38, KeyX: 43, KeyC: 25, KeyV: 7, KeyB: 61, KeyN: 45, KeyM: 27, Comma: 29, Period: 11,
  Slash: 30, ShiftRight: 28,
  ControlLeft: 5, MetaLeft: 59, AltLeft: 23, Space: 63, AltRight: 65, MetaRight: 66, Fn: 14, ControlRight: 68,
};
