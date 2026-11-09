// localStorage wrapper. Every access is guarded: the app must work with storage blocked or full.
const PREFIX = "magger60-ui:";

export function loadJson(key, fallback = null) {
  try {
    const raw = localStorage.getItem(PREFIX + key);
    return raw === null ? fallback : JSON.parse(raw);
  } catch {
    return fallback;
  }
}

export function saveJson(key, value) {
  try {
    localStorage.setItem(PREFIX + key, JSON.stringify(value));
    return true;
  } catch {
    return false;
  }
}

/** Debounced saver so dragging a slider does not write on every pixel. */
export function debouncedSaver(key, getValue, delay = 250) {
  let timer = 0;
  const flush = () => { timer = 0; saveJson(key, getValue()); };
  return () => { clearTimeout(timer); timer = setTimeout(flush, delay); };
}
