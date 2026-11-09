import { injectCss } from "../dom.js";

// Styles of the gamepad page. They are injected by the page itself (once) so that the shared stylesheets stay untouched; the integration step
// (P9d) may move this text into css/ as it is: every rule is scoped to the page ([data-page="pad"]) and uses the shared design tokens.
export const PAD_CSS = `
[data-page="pad"] .pad-grid { display: grid; gap: var(--gutter); align-items: start; grid-template-columns: minmax(0, 1fr); }
@media (min-width: 1500px) { [data-page="pad"] .pad-grid { grid-template-columns: minmax(0, 1.4fr) minmax(26rem, 1fr); } }
[data-page="pad"] .pad-col { display: grid; gap: var(--gutter); min-width: 0; }
[data-page="pad"] .pad-lead { display: flex; flex-wrap: wrap; gap: var(--s3) var(--s5); align-items: center; justify-content: space-between; }
[data-page="pad"] .pad-state { display: flex; flex-wrap: wrap; gap: var(--s2); align-items: center; }
[data-page="pad"] .pad-explain { margin: var(--s3) 0 0; padding-left: 1.2rem; display: grid; gap: var(--s2); color: var(--text-dim); font-size: 0.88rem; }
[data-page="pad"] .pad-explain strong { color: var(--text); font-weight: 600; }
[data-page="pad"] .pad-checks { display: grid; gap: var(--s2); margin-top: var(--s3); }
[data-page="pad"] .pad-checks:empty { display: none; }
[data-page="pad"] .pad-checks .banner .btn { margin-top: var(--s2); }
[data-page="pad"] .pad-sub { font-size: 0.8rem; color: var(--text-dim); }
[data-page="pad"] .pad-group { display: grid; gap: var(--s2); }
[data-page="pad"] .pad-chips { display: flex; flex-wrap: wrap; gap: var(--s2); }
[data-page="pad"] .pad-chips .chip { min-width: 2.4rem; justify-content: center; }
[data-page="pad"] .pad-chips .chip[aria-pressed="true"] { background: var(--accent-soft); color: var(--accent); border-color: var(--accent-line); }
[data-page="pad"] .pad-chips .chip:disabled { opacity: 0.45; cursor: not-allowed; }
[data-page="pad"] .pad-targets { display: grid; gap: var(--s4); grid-template-columns: repeat(auto-fit, minmax(14rem, 1fr)); }
[data-page="pad"] .pad-targets .wide { grid-column: 1 / -1; }
[data-page="pad"] .pad-opts { display: grid; gap: var(--s4); margin-top: var(--s4); padding-top: var(--s4); border-top: 1px solid var(--line); }
[data-page="pad"] .pad-opts[hidden] { display: none; }
[data-page="pad"] .pad-sel { font-size: 0.88rem; color: var(--text-dim); margin-bottom: var(--s3); min-height: 1.3em; }
[data-page="pad"] .pad-sel strong { color: var(--text); }
[data-page="pad"] .pad-tip { margin-top: var(--s4); }
[data-page="pad"] .pad-curvetabs { margin-bottom: var(--s3); }
[data-page="pad"] .pad-curvebody { display: grid; gap: var(--s4); }
[data-page="pad"] .pad-twocol { display: grid; gap: var(--s4); grid-template-columns: repeat(auto-fit, minmax(13rem, 1fr)); }
[data-page="pad"] .pad-live-off { color: var(--text-faint); font-size: 0.8rem; margin-top: var(--s2); }
[data-page="pad"] .pad-sdl { font: 0.78rem/1.5 var(--mono); padding: var(--s3); min-height: 7rem; resize: vertical; color: var(--text); word-break: break-all; }
[data-page="pad"] .pad-where { margin-top: var(--s4); font-size: 0.86rem; color: var(--text-dim); }
[data-page="pad"] .pad-where summary { cursor: pointer; font-weight: 600; color: var(--text); }
[data-page="pad"] .pad-where ul { padding-left: 1.2rem; display: grid; gap: var(--s2); margin: var(--s3) 0 0; }

/* the keyboard picture: gamepad keys show their function */
[data-page="pad"] .kb-svg.pad-has .key:not(.pad-key) .lbl { opacity: 0.5; }
[data-page="pad"] .key.pad-key .cap { stroke: var(--accent-line); fill: var(--accent-soft); }
[data-page="pad"] .key.pad-key .lbl { fill: var(--accent); }

[data-page="pad"] .sw-padkey { background: var(--accent-soft); border-color: var(--accent-line); }

/* virtual gamepad */
[data-page="pad"] .vp { display: grid; gap: var(--s3); justify-items: center; }
[data-page="pad"] .vp-svg { width: 100%; max-width: 30rem; height: auto; }
[data-page="pad"] .vp-body { fill: var(--surface-2); stroke: var(--line-strong); stroke-width: 1.5; }
[data-page="pad"] .vp-well { fill: var(--surface-3); stroke: var(--line-strong); stroke-width: 1.5; }
[data-page="pad"] .vp-square { fill: none; stroke: var(--text-faint); stroke-width: 1; stroke-dasharray: 3 3; }
[data-page="pad"] .vp-dead { fill: none; stroke: var(--text-faint); stroke-width: 1; stroke-dasharray: 2 2; }
[data-page="pad"] .vp-dot { fill: var(--text-dim); stroke: var(--bg); stroke-width: 2; }
[data-page="pad"] .vp-dot.on { fill: var(--accent); }
[data-page="pad"] .vp-btn { fill: var(--surface-3); stroke: var(--line-strong); stroke-width: 1.25; }
[data-page="pad"] .vp-btn.on { fill: var(--accent); stroke: var(--accent); }
[data-page="pad"] .vp-cap { fill: var(--text-dim); font: 600 11px var(--font); pointer-events: none; }
[data-page="pad"] .vp-cap.sm { font-size: 9px; }
[data-page="pad"] .vp-btn.on + .vp-cap { fill: var(--accent-ink); }
[data-page="pad"] .vp-trig-back { fill: var(--surface-3); stroke: var(--line-strong); stroke-width: 1.25; }
[data-page="pad"] .vp-trig-fill { fill: var(--accent); opacity: 0.85; }
[data-page="pad"] .vp-hat { fill: var(--surface-3); stroke: var(--line-strong); stroke-width: 1.25; }
[data-page="pad"] .vp-hat.on { fill: var(--accent); stroke: var(--accent); }
[data-page="pad"] .vp-hat-mid { fill: var(--surface-3); }
[data-page="pad"] .vp-vals { display: grid; grid-template-columns: repeat(6, minmax(0, 1fr)); gap: var(--s2); width: 100%; max-width: 30rem; }
[data-page="pad"] .vp-val { display: grid; gap: 2px; justify-items: center; padding: var(--s2); background: var(--surface-2); border: 1px solid var(--line); border-radius: var(--r-sm); font-size: 0.78rem; }
[data-page="pad"] .vp-val-l { color: var(--text-faint); font-size: 0.7rem; letter-spacing: 0.04em; }

/* curve plot */
[data-page="pad"] .cp { display: flex; justify-content: center; }
[data-page="pad"] .cp-svg { width: 100%; max-width: 30rem; height: auto; touch-action: none; user-select: none; -webkit-user-select: none; }
[data-page="pad"] .cp-noise { fill: var(--warn-soft); }
[data-page="pad"] .cp-grid { stroke: var(--line); stroke-width: 1; }
[data-page="pad"] .cp-grid.v { stroke-opacity: 0.55; }
[data-page="pad"] .cp-tick { fill: var(--text-faint); font: 10px var(--mono); }
[data-page="pad"] .cp-axis { fill: var(--text-faint); font: 11px var(--font); }
[data-page="pad"] .cp-area { fill: var(--accent-soft); }
[data-page="pad"] .cp-line { fill: none; stroke: var(--accent); stroke-width: 2.5; stroke-linejoin: round; }
[data-page="pad"] .cp-limit { stroke: var(--text-faint); stroke-width: 1; stroke-dasharray: 3 3; }
[data-page="pad"] .cp-hit { fill: transparent; }
[data-page="pad"] .cp-knob { fill: var(--bg); stroke: var(--accent); stroke-width: 2.5; transition: fill var(--dur) var(--ease); }
[data-page="pad"] .cp-handle { cursor: ew-resize; outline: none; }
[data-page="pad"] .cp-handle:hover .cp-knob, [data-page="pad"] .cp-handle.drag .cp-knob { fill: var(--accent); }
[data-page="pad"] .cp-handle:focus-visible .cp-knob { fill: var(--accent); stroke: var(--text); }
[data-page="pad"] .cp-live { stroke: var(--text); stroke-opacity: 0.5; stroke-width: 1; }
[data-page="pad"] .cp-livedot { fill: var(--text); stroke: var(--bg); stroke-width: 2; }
[data-page="pad"] .cp-livetext { fill: var(--text); font: 600 11px var(--mono); paint-order: stroke; stroke: var(--surface); stroke-width: 3px; }
`;

/** Add the page's styles to the document once. */
export function injectPadStyle(doc = document) {
  injectCss("pad-page-style", PAD_CSS, doc);
}
