// Minimal i18n: t(key, {param}) with zh-TW (default) and English.
import zhTW from "./locales/zh-TW.js";
import en from "./locales/en.js";

const DICTS = { "zh-TW": zhTW, en };
export const LANGS = [
  { id: "zh-TW", short: "繁中", name: "繁體中文" },
  { id: "en", short: "EN", name: "English" },
];

let current = "zh-TW";

export function setLang(lang) {
  current = DICTS[lang] ? lang : "zh-TW";
  document.documentElement.lang = current;
}
export const getLang = () => current;

export function t(key, params) {
  let str = DICTS[current][key] ?? DICTS.en[key] ?? key;
  if (params) for (const [k, v] of Object.entries(params)) str = str.replaceAll(`{${k}}`, v);
  return str;
}

/** Like t() but returns `fallback` when the key has no translation (used for data-driven names). */
export function tOr(key, fallback) {
  return DICTS[current][key] ?? DICTS.en[key] ?? fallback;
}
