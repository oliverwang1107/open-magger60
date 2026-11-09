// Key travel units. Depth arrays and "raw" values use 0.01 mm per unit; the UI and profile files use millimetres.
export const RAW_PER_MM = 100;
export const MAX_TRAVEL_MM = 3.4; // assumed full travel; make this per-device once the firmware reports it
export const MAX_TRAVEL_RAW = 340;
export const SLOT_COUNT = 76; // sensor slots 0..75; 61 of them carry a key

export const mmToRaw = (mm) => Math.round(mm * RAW_PER_MM);
export const rawToMm = (raw) => raw / RAW_PER_MM;
export const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
export const roundMm = (mm) => Math.round(mm * RAW_PER_MM) / RAW_PER_MM;
export const clampMm = (mm, lo = 0, hi = MAX_TRAVEL_MM) => roundMm(clamp(mm, lo, hi));
export const formatMm = (mm, digits = 2) => `${Number(mm).toFixed(digits)}`;
