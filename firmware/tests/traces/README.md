# tests/traces

Small trace files in the KMTR v1 format of `tools/magger60_noise.py` (documented there and in `core/NOISE_NOTES.md`), read by
`tests/filter_lab.c` (`lab_trace_load`) and `tools/filter_eval.py --trace`.

* `synthetic_idle_2col.kmtr` - SYNTHETIC, made by `python firmware/tools/filter_eval.py --make-test-trace` from a fixed seed: two idle
  columns (slots 0 and 1), 6000 frames at 23300 Hz, white noise of 15 counts sd around 3300 / 3290, column 1 with a 3-count 1 kHz tone,
  the condition flag set every 97th frame. It exists to test the loader and the replay path; it is NOT a measurement.

Real captures (from the diagnostic image `km_noise`) go here only when they are small (a few hundred KB at most) and come from our own
keyboard; never put data that comes from the vendor's software here.
