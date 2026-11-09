// Which light (km_led index 0..60, the order of the light scene's key colours and of an indicator rule's `led`) sits under which sensor slot.
// Copy of k_slot_of_led in firmware/core/src/km_led.c (serpentine over the five key rows, LED_NOTES.md section 3); a test compares the two.
export const LED_COUNT = 61;
export const SLOT_OF_LED = Object.freeze([
  18, 56, 22, 4, 60, 31, 67, 70, 19, 55, 57, 3, 26, 8,
  64, 44, 62, 39, 21, 1, 37, 16, 13, 42, 6, 75, 2, 54, 36, 73,
  72, 74, 49, 34, 52, 9, 47, 12, 48, 32, 10, 28, 30, 11, 29, 27,
  45, 61, 7, 25, 43, 38, 0, 5, 59, 23, 63, 65, 66, 14, 68,
]);
const LED_OF_SLOT = new Map(SLOT_OF_LED.map((slot, led) => [slot, led]));

/** The light under a sensor slot, or -1 when the slot has no light. */
export const ledOfSlot = (slot) => LED_OF_SLOT.get(slot) ?? -1;
/** The sensor slot above a light, or -1 for an index outside 0..60. */
export const slotOfLed = (led) => (Number.isInteger(led) && led >= 0 && led < LED_COUNT ? SLOT_OF_LED[led] : -1);
/** Lights under a list of slots, sorted, without duplicates (slots without a light are dropped). */
export const ledsOfSlots = (slots) => [...new Set([...slots].map(ledOfSlot).filter((l) => l >= 0))].sort((a, b) => a - b);
