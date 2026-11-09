// Strings of the gamepad page. They live in their own files (zh-TW-pad.js, en-pad.js) so that this page does not touch the shared
// dictionaries; pt() looks there first and falls back to the shared t(), so merging the two files into zh-TW.js / en.js later changes nothing.
import { getLang, t } from "../i18n.js";
import zhTW from "./zh-TW-pad.js";
import en from "./en-pad.js";

export const PAD_DICTS = { "zh-TW": zhTW, en };

export function pt(key, params) {
  const str = PAD_DICTS[getLang()]?.[key] ?? PAD_DICTS.en[key];
  if (str === undefined) return t(key, params);
  if (!params) return str;
  let out = str;
  for (const [k, v] of Object.entries(params)) out = out.replaceAll(`{${k}}`, v);
  return out;
}
