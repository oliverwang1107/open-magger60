// Standard ANSI 60% keyboard (61 keys), drawn from the common layout rather than from any vendor data.
// Each key: [id, label, shifted legend, width in key units, USB HID keyboard usage (null = not a normal key), name].
// `id` follows the browser's KeyboardEvent.code names. `name` defaults to the label.
// The first key is Esc (where many 60% boards put the backquote key), and the third key from the right of
// the bottom row is Fn, as on this board.

const K = (id, label, alt, w, usage, name) => ({ id, label, alt, w, usage, name: name ?? (label || id) });

export const ANSI60_ROWS = [
  [
    K("Escape", "Esc", "", 1, 41),
    K("Digit1", "1", "!", 1, 30), K("Digit2", "2", "@", 1, 31), K("Digit3", "3", "#", 1, 32),
    K("Digit4", "4", "$", 1, 33), K("Digit5", "5", "%", 1, 34), K("Digit6", "6", "^", 1, 35),
    K("Digit7", "7", "&", 1, 36), K("Digit8", "8", "*", 1, 37), K("Digit9", "9", "(", 1, 38),
    K("Digit0", "0", ")", 1, 39), K("Minus", "-", "_", 1, 45), K("Equal", "=", "+", 1, 46),
    K("Backspace", "⌫", "", 2, 42, "Backspace"),
  ],
  [
    K("Tab", "Tab", "", 1.5, 43),
    K("KeyQ", "Q", "", 1, 20), K("KeyW", "W", "", 1, 26), K("KeyE", "E", "", 1, 8), K("KeyR", "R", "", 1, 21),
    K("KeyT", "T", "", 1, 23), K("KeyY", "Y", "", 1, 28), K("KeyU", "U", "", 1, 24), K("KeyI", "I", "", 1, 12),
    K("KeyO", "O", "", 1, 18), K("KeyP", "P", "", 1, 19),
    K("BracketLeft", "[", "{", 1, 47), K("BracketRight", "]", "}", 1, 48), K("Backslash", "\\", "|", 1.5, 49),
  ],
  [
    K("CapsLock", "Caps", "", 1.75, 57, "Caps Lock"),
    K("KeyA", "A", "", 1, 4), K("KeyS", "S", "", 1, 22), K("KeyD", "D", "", 1, 7), K("KeyF", "F", "", 1, 9),
    K("KeyG", "G", "", 1, 10), K("KeyH", "H", "", 1, 11), K("KeyJ", "J", "", 1, 13), K("KeyK", "K", "", 1, 14),
    K("KeyL", "L", "", 1, 15), K("Semicolon", ";", ":", 1, 51), K("Quote", "'", "\"", 1, 52),
    K("Enter", "↵", "", 2.25, 40, "Enter"),
  ],
  [
    K("ShiftLeft", "Shift", "", 2.25, 225, "Left Shift"),
    K("KeyZ", "Z", "", 1, 29), K("KeyX", "X", "", 1, 27), K("KeyC", "C", "", 1, 6), K("KeyV", "V", "", 1, 25),
    K("KeyB", "B", "", 1, 5), K("KeyN", "N", "", 1, 17), K("KeyM", "M", "", 1, 16),
    K("Comma", ",", "<", 1, 54), K("Period", ".", ">", 1, 55), K("Slash", "/", "?", 1, 56),
    K("ShiftRight", "Shift", "", 2.75, 229, "Right Shift"),
  ],
  [
    K("ControlLeft", "Ctrl", "", 1.25, 224, "Left Ctrl"), K("MetaLeft", "Win", "", 1.25, 227, "Left Win"),
    K("AltLeft", "Alt", "", 1.25, 226, "Left Alt"), K("Space", "", "", 6.25, 44, "Space"),
    K("AltRight", "Alt", "", 1.25, 230, "Right Alt"), K("MetaRight", "Win", "", 1.25, 231, "Right Win"),
    K("Fn", "Fn", "", 1.25, null), K("ControlRight", "Ctrl", "", 1.25, 228, "Right Ctrl"),
  ],
];
