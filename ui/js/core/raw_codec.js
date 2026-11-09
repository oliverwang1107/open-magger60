// The codec a KmfwDevice gets for readProfile/writeProfile: the blob <-> its four RAW profiles (settings_codec.js model).
// (The UI works on the whole blob through profile_codec.js; this is the device-level view.)
import { decodeSettings, encodeSettings } from "./settings_codec.js";

export const rawCodec = Object.freeze({
  decode: (blob) => decodeSettings(blob).profiles,
  encode(blob, index, profile) {
    const s = decodeSettings(blob);
    s.profiles[index] = profile;
    return encodeSettings(s);
  },
});
