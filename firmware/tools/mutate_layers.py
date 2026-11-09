#!/usr/bin/env python3
"""Mutation pass for the layered light core (M6 P6c): the indicator rules, effects 7-9, the external frame inputs, the scene and input
handling in km_led.c, and the scene / host-LED / external-frame glue in m5_led.c.

    python firmware/tools/mutate_layers.py            # run every mutation, list survivors
    python firmware/tools/mutate_layers.py --list     # just print them
    python firmware/tools/mutate_layers.py --only blink

Same idea as mutate_ledstore.py: each mutation changes one expression in a scratch copy of ONE source file, that file is recompiled (all other
objects come from one clean build) and the suites led, led_hw and ledstore are run. "killed" = the build works and a check fails; a "survivor"
means the tests do not notice the change.
"""
import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
SUITES = ["led", "led_hw", "ledstore"]

L = "core/src/km_led.c"
M = "port/n32h48x/m5_led.c"

# (name, file, old text, new text); `old` must occur exactly once in the file.
MUTATIONS = [
    # ---- indicator conditions ----
    ("caps looks at the Num bit", L, "case KM_LS_SRC_HOST_CAPS:   return (l->in.host_leds & KM_LED_HOST_CAPS) != 0u;", "case KM_LS_SRC_HOST_CAPS:   return (l->in.host_leds & KM_LED_HOST_NUM) != 0u;"),
    ("num looks at the Caps bit", L, "case KM_LS_SRC_HOST_NUM:    return (l->in.host_leds & KM_LED_HOST_NUM) != 0u;", "case KM_LS_SRC_HOST_NUM:    return (l->in.host_leds & KM_LED_HOST_CAPS) != 0u;"),
    ("scroll looks at the Caps bit", L, "case KM_LS_SRC_HOST_SCROLL: return (l->in.host_leds & KM_LED_HOST_SCROLL) != 0u;", "case KM_LS_SRC_HOST_SCROLL: return (l->in.host_leds & KM_LED_HOST_CAPS) != 0u;"),
    ("layer rule: any layer from N up", L, "case KM_LS_SRC_LAYER:       return l->in.layer == r->param;", "case KM_LS_SRC_LAYER:       return l->in.layer >= r->param;"),
    ("layer rule looks at the profile", L, "case KM_LS_SRC_LAYER:       return l->in.layer == r->param;", "case KM_LS_SRC_LAYER:       return l->in.profile == r->param;"),
    ("profile rule looks at the layer", L, "case KM_LS_SRC_PROFILE:     return l->in.profile == r->param;", "case KM_LS_SRC_PROFILE:     return l->in.layer == r->param;"),
    ("pad rule looks at the toggle flag", L, "case KM_LS_SRC_PAD:         return (l->in.flags & KM_LED_IN_PAD) != 0u;", "case KM_LS_SRC_PAD:         return (l->in.flags & KM_LED_IN_TOGGLE) != 0u;"),
    ("toggle rule looks at the pad flag", L, "case KM_LS_SRC_TOGGLE:      return (l->in.flags & KM_LED_IN_TOGGLE) != 0u;", "case KM_LS_SRC_TOGGLE:      return (l->in.flags & KM_LED_IN_PAD) != 0u;"),
    ("external rule always false", L, "case KM_LS_SRC_EXTERNAL:    return l->ext_use != 0u;", "case KM_LS_SRC_EXTERNAL:    return false;"),
    ("unknown source is true", L, "    default:                    return false;\n    }\n}", "    default:                    return true;\n    }\n}"),
    # ---- blinking ----
    ("blink phase inverted", L, "const bool phase_on = ((now_ms / KM_LED_BLINK_HALF_MS) & 1u) == 0u;", "const bool phase_on = ((now_ms / KM_LED_BLINK_HALF_MS) & 1u) != 0u;"),
    ("blink half period", L, "const bool phase_on = ((now_ms / KM_LED_BLINK_HALF_MS) & 1u) == 0u;", "const bool phase_on = ((now_ms / (KM_LED_BLINK_HALF_MS / 2u)) & 1u) == 0u;"),
    ("blink flag ignored", L, "            if (!blinks || phase_on) {", "            if (1 || phase_on) {"),
    ("blink always dark", L, "            if (!blinks || phase_on) {", "            if (!blinks || (phase_on && 0)) {"),
    ("blink does not ask for the fast pace", L, "((is_animated(fx) || l->blink) && l->gain != 0u)", "(is_animated(fx) && l->gain != 0u)"),
    # ---- rule precedence and off behaviour ----
    ("a shown rule does not stop the search", L, "            *raw = r->on;\n            return;", "            *raw = r->on;\n            continue;"),
    ("last off behaviour wins", L, "        if (off < 0 && (r->flags & KM_LS_FLAG_OFF_MASK) != KM_LS_OFF_KEEP) {", "        if ((r->flags & KM_LS_FLAG_OFF_MASK) != KM_LS_OFF_KEEP) {"),
    ("keep behaves like black", L, "        if (off < 0 && (r->flags & KM_LS_FLAG_OFF_MASK) != KM_LS_OFF_KEEP) {", "        if (off < 0) {"),
    ("rules of other lights apply", L, "        if (r->led != i) {", "        if (r->led != i && 0) {"),
    ("black does nothing", L, "            raw->r = raw->g = raw->b = 0;", "            (void)raw;"),
    ("dim uses the inverse", L, "            raw->g = scale8(raw->g, r->dim);", "            raw->g = scale8(raw->g, 255u - r->dim);"),
    ("dim leaves green alone", L, "            raw->g = scale8(raw->g, r->dim);", "            (void)raw->g;"),
    ("dim uses the inverse on red", L, "            raw->r = scale8(raw->r, r->dim);", "            raw->r = scale8(raw->r, 255u - r->dim);"),
    ("dim leaves blue alone", L, "            raw->b = scale8(raw->b, r->dim);", "            (void)raw->b;"),
    ("indicators ignored", L, "    if (l->nrules != 0u) {\n        apply_rules(l, i, &raw);", "    if (l->nrules != 0u && 0) {\n        apply_rules(l, i, &raw);"),
    ("indicators survive a fault", L, "    l->nrules = l->blank ? 0u : l->scene->n_rules;", "    l->nrules = l->scene->n_rules;"),
    ("rule count not limited", L, "        l->nrules = KM_LS_MAX_RULES;                                       /* only reachable with a scene that was not validated */", "        (void)0;"),
    # ---- effects 7, 8, 9 ----
    ("custom ignores the key index", L, "raw = l->scene->palette[l->scene->key_idx[i] % KM_LS_PALETTE];", "raw = l->scene->palette[l->scene->key_idx[0] % KM_LS_PALETTE];"),
    ("custom uses the palette index as the colour", L, "raw = l->scene->palette[l->scene->key_idx[i] % KM_LS_PALETTE];", "raw = l->scene->palette[i % KM_LS_PALETTE];"),
    ("custom takes the fast pace", L, "fx != KM_LED_FX_STATIC && fx != KM_LED_FX_CUSTOM;", "fx != KM_LED_FX_STATIC;"),
    ("effect 8 draws the animation frame", L, "if (fx == KM_LED_FX_EXTERNAL && l->ext_valid[KM_LED_EXT_STREAM]) {", "if (fx == KM_LED_FX_EXTERNAL && l->ext_valid[KM_LED_EXT_ANIM]) {"),
    ("effect 9 draws the stream frame", L, "} else if (fx == KM_LED_FX_ANIM && l->ext_valid[KM_LED_EXT_ANIM]) {", "} else if (fx == KM_LED_FX_ANIM && l->ext_valid[KM_LED_EXT_STREAM]) {"),
    ("external frames drawn during a fault", L, "    if (!l->blank) {                                                       /* a fault shows nothing, not even an indicator */", "    if (1) {"),
    ("grey not tinted", L, "                raw.r = scale8(c->color.r, g);", "                raw.r = (uint8_t)g;"),
    ("grey tint blue wrong", L, "                raw.b = scale8(c->color.b, g);", "                raw.b = scale8(c->color.g, g);"),
    ("external frame not drawn", L, "            raw = l->ext[src][i];", "            (void)src;"),
    ("stream never expires", L, "(int32_t)(now_ms - l->ext_ms[KM_LED_EXT_STREAM]) > (int32_t)KM_LED_EXT_TIMEOUT_MS", "0"),
    ("stream expires one millisecond early", L, "(int32_t)(now_ms - l->ext_ms[KM_LED_EXT_STREAM]) > (int32_t)KM_LED_EXT_TIMEOUT_MS", "(int32_t)(now_ms - l->ext_ms[KM_LED_EXT_STREAM]) >= (int32_t)KM_LED_EXT_TIMEOUT_MS"),
    ("stream expiry not safe over the clock wrap", L, "(int32_t)(now_ms - l->ext_ms[KM_LED_EXT_STREAM]) > (int32_t)KM_LED_EXT_TIMEOUT_MS", "now_ms > l->ext_ms[KM_LED_EXT_STREAM] + KM_LED_EXT_TIMEOUT_MS"),
    ("stream expiry never clears the frame", L, "        l->ext_valid[KM_LED_EXT_STREAM] = false;                            /* the stream went quiet: back to dark */", "        (void)0;"),
    ("push does not copy", L, "        memcpy(l->ext[source], px, sizeof l->ext[source]);", "        (void)px;"),
    ("grey push keeps the old frame", L, "            l->ext[source][i].r = gray[i];", "            (void)gray[i];"),
    ("rgb frame flagged as grey", L, "        ext_store(l, source, now_ms, false);", "        ext_store(l, source, now_ms, true);"),
    ("grey frame flagged as colour", L, "        ext_store(l, source, now_ms, true);", "        ext_store(l, source, now_ms, false);"),
    ("push does not stamp the time", L, "    l->ext_ms[source] = now_ms;", "    (void)now_ms;"),
    ("first frame does not wake", L, "    if (!l->ext_valid[source]) {\n        km_led_wake(l);", "    if (0) {\n        km_led_wake(l);"),
    ("every frame wakes", L, "    if (!l->ext_valid[source]) {\n        km_led_wake(l);", "    if (1) {\n        km_led_wake(l);"),
    ("ext_end ends the wrong source", L, "        l->ext_valid[source] = false;", "        l->ext_valid[0] = false;"),
    ("ext_valid always true", L, "return source < KM_LED_EXT_SOURCES && l->ext_valid[source];", "return source < KM_LED_EXT_SOURCES && (l->ext_valid[source] || 1);"),
    ("ext_valid ignores the range", L, "return source < KM_LED_EXT_SOURCES && l->ext_valid[source];", "return l->ext_valid[source % KM_LED_EXT_SOURCES];"),
    # ---- scene and inputs ----
    ("NULL scene is not the default", L, "l->scene = scene != NULL ? scene : &k_default_scene;", "l->scene = scene;"),
    ("a new scene does not start a frame", L, "    l->scene = scene != NULL ? scene : &k_default_scene;\n    l->started = false;", "    l->scene = scene != NULL ? scene : &k_default_scene;"),
    ("default scene colour", L, "{ { 0, 150, 255 } }, { 0 }, 0, 0, 0 };", "{ { 0, 150, 254 } }, { 0 }, 0, 0, 0 };"),
    ("host bits not masked", L, "(uint8_t)(in->host_leds & (KM_LED_HOST_NUM | KM_LED_HOST_CAPS | KM_LED_HOST_SCROLL))", "in->host_leds"),
    ("flags not masked", L, "(uint8_t)(in->flags & (KM_LED_IN_PAD | KM_LED_IN_TOGGLE))", "in->flags"),
    ("input change does not wake", L, "        l->in = n;\n        km_led_wake(l);", "        l->in = n;"),
    ("input change does not start a frame", L, "        km_led_wake(l);                     /* a change is something to look at (the host's Caps Lock, a layer switch) */\n        l->started = false;", "        km_led_wake(l);"),
    ("layer change not noticed", L, "n.host_leds != l->in.host_leds || n.layer != l->in.layer ||", "n.host_leds != l->in.host_leds ||"),
    ("profile change not noticed", L, "|| n.profile != l->in.profile ||", "||"),
    ("flag change not noticed", L, "|| n.flags != l->in.flags) {", ") {"),
    ("host change not noticed", L, "if (n.host_leds != l->in.host_leds || n.layer", "if (n.layer"),
    ("input never stored", L, "        l->in = n;\n        km_led_wake(l);", "        km_led_wake(l);"),
    # ---- the service glue ----
    ("scene accepted without validation", M, "    if (!km_lightscene_valid(sc)) {\n        return false;\n    }\n    g_scene = *sc;", "    g_scene = *sc;"),
    ("scene copied but the core not pointed at it", M, "    km_led_set_scene(&g_led, &g_scene);\n    km_led_wake(&g_led);\n    return true;", "    km_led_wake(&g_led);\n    return true;"),
    ("scene not copied", M, "    g_scene = *sc;                                      /* the core points at g_scene: the next frame uses the new content */", "    (void)0;"),
    ("init keeps the previous scene", M, "    km_lightscene_default(&g_scene);\n    km_led_set_scene(&g_led, &g_scene);", "    km_led_set_scene(&g_led, &g_scene);"),
    ("commit does not apply", M, "    (void)m5_led_scene_set(sc);\n    return save", "    return save"),
    ("commit always saves", M, "    return save ? km_lightscene_save(g_scene_store, sc) : KM_STORE_OK;", "    return km_lightscene_save(g_scene_store, sc);"),
    ("commit never saves", M, "    return save ? km_lightscene_save(g_scene_store, sc) : KM_STORE_OK;", "    return KM_STORE_OK;"),
    ("commit applies when saving is impossible", M, "    if (!km_lightscene_valid(sc) || (save && g_scene_store == NULL)) {\n        return KM_STORE_ERR_ARG;\n    }", "    if (!km_lightscene_valid(sc)) {\n        return KM_STORE_ERR_ARG;\n    }"),
    ("commit accepts an invalid scene", M, "    if (!km_lightscene_valid(sc) || (save && g_scene_store == NULL)) {", "    if ((save && g_scene_store == NULL)) {"),
    ("attach forgets the store", M, "    g_scene_store = store;\n    km_lightscene_t t;", "    km_lightscene_t t;"),
    ("attach does not apply", M, "        return m5_led_scene_set(&t);\n    }\n    return false;", "        return true;\n    }\n    return false;"),
    ("host source not polled", M, "                g_in.host_leds = g_host_fn();", "                (void)g_host_fn;"),
    ("inputs not given to the core in the service", M, "            km_led_set_inputs(&g_led, &g_in);\n\n            const led_hw_state_t hs", "            const led_hw_state_t hs"),
    ("inputs call does not reach the core", M, "    g_in.flags = in->flags;\n    km_led_set_inputs(&g_led, &g_in);", "    g_in.flags = in->flags;"),
    ("layer input not stored", M, "    g_in.layer = in->layer;", "    (void)0;"),
    ("profile input not stored", M, "    g_in.profile = in->profile;", "    (void)0;"),
    ("flags input not stored", M, "    g_in.flags = in->flags;", "    (void)0;"),
    ("detaching the source keeps the bits", M, "    if (fn == NULL) {\n        g_in.host_leds = 0;", "    if (fn == NULL) {"),
    ("detaching the source does not tell the core", M, "        g_in.host_leds = 0;\n        km_led_set_inputs(&g_led, &g_in);", "        g_in.host_leds = 0;"),
    ("stream frames stamped with time 0", M, "    km_led_ext_rgb(&g_led, source, px, s.ms);", "    km_led_ext_rgb(&g_led, source, px, 0);"),
    ("grey frames stamped with time 0", M, "    km_led_ext_gray(&g_led, source, gray, s.ms);", "    km_led_ext_gray(&g_led, source, gray, 0);"),
    ("ext_end does nothing", M, "    km_led_ext_end(&g_led, source);", "    (void)source;"),
]

EQUIVALENT = {
    "external frames drawn during a fault": "redundant guard: while blank the effect is already OFF, so no external frame can be selected",
    "inputs call does not reach the core": "the service forwards its inputs to the core on every pass anyway; the direct call only saves one pass",
    "detaching the source does not tell the core": "the service forwards its inputs on every pass anyway; the direct call only saves one pass",
}


def env() -> dict:
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


def load_runner():
    spec = importlib.util.spec_from_file_location("run_tests", FW / "tools" / "run_tests.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only", help="run only mutations whose name contains this text")
    args = ap.parse_args()
    muts = [m for m in MUTATIONS if m[0] not in EQUIVALENT]
    if args.list:
        for i, m in enumerate(muts, 1):
            print("%2d  %s" % (i, m[0]))
        return 0

    rt = load_runner()
    e = env()
    cc = shutil.which("gcc", path=e["PATH"])
    work = Path(tempfile.mkdtemp(prefix="km_mut_ly_"))

    def compile_one(src: Path, obj: Path, kind: str, root: Path) -> bool:
        inc = ["-I" + str(root / "core/include"), "-I" + str(root / "hid/include"), "-I" + str(root / "usb/include"),
               "-I" + str(root / "tests"), "-I" + str(root / "tests/sim"), "-I" + str(root / "port/n32h48x")]
        if kind == "lib":
            cmd = [cc, *rt.STRICT, *inc, "-c", str(src), "-o", str(obj)]
        else:
            cmd = [cc, *rt.STRICT, "-DKM_FLASH_HOST_TEST", "-DKM_SENSE_HOST_TEST", "-DKM_STORE_HOST_TEST", "-DKM_LED_HOST_TEST", *inc,
                   "-c", str(src), "-o", str(obj)]
        return subprocess.run(cmd, env=e, capture_output=True).returncode == 0

    survivors, broken = [], []
    try:
        # one clean build of everything, kept as objects keyed by the source path relative to FW
        objs = {}
        kinds = {}
        for src in rt.LIB_SOURCES:
            kinds[src] = "lib"
        for src in rt.SIM_SOURCES:
            kinds[src] = "sim"
        for src, kind in kinds.items():
            rel = src.relative_to(FW).as_posix()
            obj = work / ("base_" + rel.replace("/", "_") + ".o")
            if not compile_one(src, obj, kind, FW):
                print("clean build failed:", rel)
                return 2
            objs[rel] = obj
        # led_hw.c at levels 1 and 0, as run_tests.py does
        extra = []
        for level, prefix in ((1, "l1_"), (0, "l0_")):
            obj = work / ("base_led_hw_level%d.o" % level)
            ren = ["-Dled_hw_%s=%s%s" % (n, prefix, n) for n in ("start", "kill", "send", "poll", "get_diag", "test_reset", "test_last_frame")]
            ren = [r.replace(prefix + "test_reset", prefix + "reset") for r in ren]
            cmd = [cc, *rt.STRICT, "-DKM_LED_HOST_TEST", "-DKM_LED_HW_LEVEL=%d" % level, *ren, *rt.INCLUDES, "-c",
                   str(FW / "port/n32h48x/led_hw.c"), "-o", str(obj)]
            if subprocess.run(cmd, env=e, capture_output=True).returncode != 0:
                print("clean build failed: led_hw level", level)
                return 2
            extra.append(str(obj))
        for src in rt.TEST_SOURCES:
            obj = work / ("t_" + src.stem + ".o")
            cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-DKM_SENSE_HOST_TEST", *rt.INCLUDES, "-c", str(src), "-o", str(obj)]
            if subprocess.run(cmd, env=e, capture_output=True).returncode != 0:
                print("clean build failed:", src)
                return 2
            extra.append(str(obj))

        def run_suites(exe: Path):
            try:
                r = subprocess.run([str(exe), *SUITES], env=e, capture_output=True, text=True, timeout=300)
                return r.returncode, r.stdout
            except subprocess.TimeoutExpired:
                return 1, "(timeout)"

        exe = work / "clean.exe"
        subprocess.run([cc, *[str(o) for o in objs.values()], *extra, "-o", str(exe)], env=e, check=True)
        rc, out = run_suites(exe)
        if rc != 0:
            print("the clean build does not pass its own suites:\n" + out[-600:])
            return 2

        for i, (name, rel, old, new) in enumerate(muts, 1):
            if args.only and args.only not in name:
                continue
            src = FW / rel
            text = src.read_text(newline="").replace("\r\n", "\n")
            if text.count(old) != 1:
                print("%2d  %-52s BAD MUTATION (pattern found %d times)" % (i, name, text.count(old)))
                broken.append(name)
                continue
            msrc = work / ("mut_" + src.name)
            msrc.write_text(text.replace(old, new), newline="")
            mobj = work / ("mut_%d.o" % i)
            kind = kinds[src]
            if not compile_one(msrc, mobj, kind, FW):
                print("%2d  %-52s BUILD FAILED" % (i, name))
                broken.append(name)
                continue
            mexe = work / ("m%d.exe" % i)
            link = [str(mobj) if k == rel else str(o) for k, o in objs.items()]
            r = subprocess.run([cc, *link, *extra, "-o", str(mexe)], env=e, capture_output=True, text=True)
            if r.returncode != 0:
                print("%2d  %-52s LINK FAILED" % (i, name))
                broken.append(name)
                continue
            rc, out = run_suites(mexe)
            if rc == 0:
                print("%2d  %-52s SURVIVED" % (i, name))
                survivors.append(name)
            else:
                first = next((l.strip() for l in out.splitlines() if "FAIL" in l), "(crash/timeout)")
                print("%2d  %-52s killed  %s" % (i, name, first[-70:]))
            for f in (mexe, mobj, msrc):
                f.unlink(missing_ok=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\n%d mutations, %d survivors, %d broken" % (len(muts), len(survivors), len(broken)))
    for s in survivors:
        print("  survivor:", s)
    return 1 if survivors or broken else 0


if __name__ == "__main__":
    sys.exit(main())
