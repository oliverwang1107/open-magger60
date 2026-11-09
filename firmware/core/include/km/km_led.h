/*
 * km_led.h - LED lighting core (M5 task L5): frame buffer for the 61 key lights, five effects, the brightness / white /
 * total-current limits, and the encoder that turns the final colours into the SPI bit stream the lights expect.
 * Original work, pure logic: no hardware, time is passed in as milliseconds. The hardware facts it relies on (WS2812-style
 * single wire, 4 SPI bits per data bit, 12 bytes per light, light order) are in LED_NOTES.md.
 *
 * Pipeline of one frame, all in small slices so that the 39 us sensing loop is never held up (km_led_work()):
 *   render   colour of light i (0..255 per channel), made in LAYERS, bottom to top (M6 package P6c, LED_NOTES.md section 18):
 *              1 base         one of the effects: the five classic ones, 7 CUSTOM (a palette colour per key, from the scene), 8 EXTERNAL
 *                             (the stream frame, dark while there is none), 9 ANIM (the animation player's frame, dark while there is none)
 *              2 indicators   the scene's rules (km_lightscene.h): a light that shows the host's Caps Lock, the active layer, ...; they replace
 *                             the colour below them while their condition holds, and may black out or dim it while it does not
 *            Both layers produce PRE-limit colours; nothing in them can exceed what the next stage allows.
 *   shape    user brightness, then the hard per-channel cap (KM_LED_CHANNEL_CAP, 55 % of 255),
 *            then the white limit: the three channels of one light add up to at most KM_LED_PIXEL_CAP
 *   limit    the sum over all lights of all channels is kept below KM_LED_BUDGET_SUM (the current budget); when it would be
 *            exceeded every channel is scaled by the same factor, so the hue is preserved
 *   encode   bit stream into the idle one of two DMA buffers (km_led_frame())
 * The three limits are compile-time constants. No configuration value (km_led_cfg_t) can raise them: brightness only ever
 * scales a colour DOWN from what the effect produced, and every stage after it can only lower a channel.
 *
 * Frame pacing: a new frame is started every KM_LED_FRAME_MS_ANIM ms for the moving effects and every
 * KM_LED_FRAME_MS_STATIC ms for the still ones; a finished frame waits until km_led_frame_taken() is called, and the core never
 * writes the buffer that was handed out last (the DMA may still be reading it).
 */
#ifndef KM_LED_H
#define KM_LED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct km_lightscene;                   /* km_lightscene.h: indicator rules, key colours, animation settings (km_led_set_scene) */

#define KM_LED_COUNT            61u    /* one light behind every key that has a sensor */
#define KM_LED_SLOTS            76u    /* sensing slots (KM_ENGINE_SLOTS) */
#define KM_LED_NONE             0xFFu

/* ---- stream format (LED_NOTES.md section 1) ---- */
#define KM_LED_BYTES_PER_LED    12u    /* 3 channels x 8 data bits x 4 SPI bits */
#define KM_LED_LEAD_BYTES       4u     /* zero bytes in front: the line is low before the first bit */
#define KM_LED_TRAIL_BYTES      72u    /* zero bytes behind: 576 SPI bits low = the reset / latch gap (>= 150 us at 3.75 MHz) */
#define KM_LED_FRAME_BYTES      (KM_LED_LEAD_BYTES + KM_LED_COUNT * KM_LED_BYTES_PER_LED + KM_LED_TRAIL_BYTES)

#define KM_LED_WIRE_GRB         0      /* standard WS2812 */
#define KM_LED_WIRE_RGB         1
#ifndef KM_LED_WIRE_ORDER
#define KM_LED_WIRE_ORDER       KM_LED_WIRE_GRB
#endif

/* ---- the limits (LED_NOTES.md section 4); override with -D at build time only ---- */
#ifndef KM_LED_CHANNEL_CAP
#define KM_LED_CHANNEL_CAP      140u                     /* 55 % of 255 (LED_NOTES.md section 4) */
#endif
#ifndef KM_LED_PIXEL_CAP
#define KM_LED_PIXEL_CAP        (2u * KM_LED_CHANNEL_CAP)   /* white limit: a white light gets 2/3 of the channel cap per channel */
#endif
#ifndef KM_LED_MA_PER_CHANNEL
#define KM_LED_MA_PER_CHANNEL   15u                      /* model: current of one channel at 255 (ASSUMED, 12..20 mA for this chip class) */
#endif
#ifndef KM_LED_BUDGET_MA
#define KM_LED_BUDGET_MA        180u                     /* total LED current the model allows (declared USB budget is 300 mA) */
#endif
#define KM_LED_BUDGET_SUM       ((KM_LED_BUDGET_MA * 255u) / KM_LED_MA_PER_CHANNEL)   /* sum of all channel values allowed */

#define KM_LED_FRAME_MS_ANIM    20u
#define KM_LED_FRAME_MS_STATIC  100u
#define KM_LED_DEMO_PHASE_MS    8000u    /* KM_LED_FX_DEMO: length of every effect in the cycle */

/* ---- idle-off (LED_NOTES.md section 14): after idle_s seconds without any key activity the lights fade out, the first key
 * activity brings them back. The gain only ever scales the brightness DOWN, so it can not get past the limits above. ---- */
#define KM_LED_IDLE_DEFAULT_S   600u     /* 10 minutes; 0 = never */
#define KM_LED_IDLE_FADE_OUT_MS 2000u    /* full to dark */
#define KM_LED_IDLE_FADE_IN_MS  150u     /* dark to full, on the first key activity */
#define KM_LED_IDLE_GAIN_FULL   0xFFFFu

typedef struct {
    uint8_t r, g, b;
} km_led_rgb_t;

typedef enum {
    KM_LED_FX_OFF = 0,
    KM_LED_FX_STATIC,        /* every light in cfg.color */
    KM_LED_FX_BREATH,        /* cfg.color, brightness following a slow triangle (squared) wave */
    KM_LED_FX_RAINBOW,       /* a hue wave travelling left to right */
    KM_LED_FX_REACTIVE,      /* dark; a key that goes down lights in cfg.color and fades out after it is released */
    KM_LED_FX_DEPTH,         /* "depth heat": the colour follows how far the key is pressed, blue (up) .. red (bottom) */
    KM_LED_FX_DEMO,          /* rainbow, reactive, depth, breath, static in turn, KM_LED_DEMO_PHASE_MS each (bring-up default) */
    KM_LED_FX_CUSTOM,        /* 7: every light in its own palette colour (scene.palette[scene.key_idx[i]]) */
    KM_LED_FX_EXTERNAL,      /* 8: the colours of the latest stream frame (km_led_ext_rgb, KM_LED_EXT_STREAM); dark while none is valid */
    KM_LED_FX_ANIM,          /* 9: the colours of the animation player's frame (km_led_ext_gray / _rgb, KM_LED_EXT_ANIM); dark while none */
    KM_LED_FX_COUNT
} km_led_fx_t;

/* What the user may change at run time (RAM only). All fields are validated by km_led_cfg_valid(). */
typedef struct {
    uint8_t effect;          /* km_led_fx_t */
    uint8_t brightness;      /* 0..255, scales the effect colour DOWN (the limits stay in force) */
    uint8_t speed;           /* 1..255, bigger = faster (wave speed, breathing rate, fade-out time) */
    km_led_rgb_t color;      /* base colour of static / breath / reactive */
} km_led_cfg_t;

/* ---- external frames (LED_NOTES.md section 18.3): the input contract of the stream (P7a) and the animation player (P7b) ---- */
#define KM_LED_EXT_STREAM       0u       /* source of effect 8; a frame stays valid KM_LED_EXT_TIMEOUT_MS after the latest push */
#define KM_LED_EXT_ANIM         1u       /* source of effect 9; a frame stays valid until km_led_ext_end() (a still picture is pushed once) */
#define KM_LED_EXT_SOURCES      2u
#define KM_LED_EXT_TIMEOUT_MS   1000u    /* the core's own safety net for the stream; the stream code ends it earlier (500 ms of silence) */

/* ---- inputs of the indicator rules, set by the integration every main-loop pass (km_led_set_inputs) ---- */
#define KM_LED_HOST_NUM         0x01u    /* bits of km_led_inputs_t.host_leds, the host's keyboard LED output report (HID: bit0 Num, bit1 Caps, bit2 Scroll) */
#define KM_LED_HOST_CAPS        0x02u
#define KM_LED_HOST_SCROLL      0x04u
#define KM_LED_IN_PAD           0x01u    /* km_led_inputs_t.flags: the gamepad output is on */
#define KM_LED_IN_TOGGLE        0x02u    /* at least one Toggle key is on */
#define KM_LED_BLINK_HALF_MS    500u     /* a blinking indicator is on for this long, then off for this long (from the frame clock) */

typedef struct {
    uint8_t host_leds;                   /* KM_LED_HOST_*; bits above Scroll are ignored; 0 = off or unknown (the host never told us) */
    uint8_t layer;                       /* the layer that is active now, 0..3 */
    uint8_t profile;                     /* the active profile, 0..3 */
    uint8_t flags;                       /* KM_LED_IN_* */
} km_led_inputs_t;

/* depth callback: pressed depth of a sensing slot in 0.01 mm (0..340), may be NULL (then DEPTH sees "all keys up") */
typedef uint16_t (*km_led_depth_fn)(void *ctx, unsigned slot);

typedef struct {
    km_led_cfg_t cfg;
    bool     blank;                          /* force black output (fault, suspend) */

    /* idle-off */
    uint16_t idle_s;                         /* seconds without key activity before the lights fade out, 0 = never */
    uint16_t gain;                           /* 0..KM_LED_IDLE_GAIN_FULL, the fade factor applied to the brightness */
    bool     idle;                           /* the idle time has run out (latched until the next key activity) */
    bool     activity;                       /* a key was seen since the last frame began */
    bool     act_armed;                      /* last_act_ms is valid */
    uint32_t last_act_ms;

    /* frame pacing and the work state machine */
    uint8_t  stage;                          /* 0 idle, 1 render, 2 encode, 3 frame ready */
    uint8_t  next_led;                       /* next light to render / encode */
    uint8_t  out_idx;                        /* buffer the stage writes into */
    uint8_t  inflight;                       /* buffer handed out last (0xFF = none yet) */
    bool     started;                        /* at least one frame was begun */
    uint32_t next_frame_ms;
    uint32_t last_frame_ms;

    /* per frame constants, latched when the frame begins */
    uint32_t t_ms;
    uint16_t dec;                            /* reactive: fade-out step for this frame (of 255) */
    uint8_t  fx;                             /* the effect actually rendered (DEMO resolves to one of the others) */
    uint8_t  mscale;                         /* brightness * channel cap / 255 */
    uint8_t  phase8;                         /* rainbow phase, breath factor */
    uint8_t  breath;
    uint32_t total;                          /* sum of all channels after shaping (current model) */
    uint32_t scale16;                        /* final budget factor, 65536 = unchanged */
    uint32_t out16;                          /* scale16 times the idle fade, what the encoder applies */

    /* layers (M6 P6c) */
    const struct km_lightscene *scene;       /* never NULL: km_led_init points it at the built-in default scene */
    km_led_inputs_t in;
    uint8_t  nrules;                         /* rules in force for this frame (0 while blank) */
    uint8_t  shown;                          /* this frame: bit i = rule i shows its colour (condition true and, if it blinks, in the on phase) */
    uint8_t  ext_use;                        /* this frame: 0 no external frame drawn, else 1 + the source (KM_LED_EXT_*) drawn by the effect */
    bool     blink;                          /* this frame: a blinking rule has a true condition (needs the fast frame pace) */
    bool     ext_valid[KM_LED_EXT_SOURCES];
    bool     ext_gray[KM_LED_EXT_SOURCES];   /* the frame holds grey levels (in .r), tinted with cfg.color when rendered */
    uint32_t ext_ms[KM_LED_EXT_SOURCES];     /* time of the latest push */
    km_led_rgb_t ext[KM_LED_EXT_SOURCES][KM_LED_COUNT];

    uint8_t  lvl[KM_LED_COUNT];              /* reactive fade level of every light */
    km_led_rgb_t px[KM_LED_COUNT];           /* shaped colours of the frame being made */
    uint8_t  buf[2][KM_LED_FRAME_BYTES];

    /* statistics */
    uint32_t frames;                         /* frames finished */
    uint32_t units;                          /* work units done in the last km_led_work() call */
    uint32_t units_max;
    uint32_t last_total;                     /* current-model sum of the last finished frame */
    uint32_t last_scale16;
} km_led_t;

/* ---- configuration ---- */
void km_led_cfg_default(km_led_cfg_t *cfg);
bool km_led_cfg_valid(const km_led_cfg_t *cfg);              /* effect < COUNT, speed != 0 */
/* Apply a configuration: false (and no change) if invalid. */
bool km_led_set_cfg(km_led_t *l, const km_led_cfg_t *cfg);

/* ---- scene and inputs (P6c). Neither is kept by flash here: km_lightscene.h / m5_led.c own the storage ---- */
/* Use this scene from the next frame on. The core keeps the POINTER (the caller keeps the scene alive and does not edit it while a frame is
 * being made, i.e. between km_led_work() calls it may); NULL = the built-in default scene (no rules, every light on palette colour 0). */
void km_led_set_scene(km_led_t *l, const struct km_lightscene *scene);
/* The state the indicator rules look at. Cheap (a few compares); a change counts as key activity (faded-out lights come back) and starts a frame at once. */
void km_led_set_inputs(km_led_t *l, const km_led_inputs_t *in);

/* ---- external frames (P7a stream, P7b animation) ----
 * One frame = KM_LED_COUNT colours in light order (km_led index), PRE-limit: brightness, the channel cap, the white limit and the current
 * budget are applied afterwards and never relaxed, whatever the source says. A frame replaces the one before it as a whole (a push is one
 * copy; push only COMPLETE frames). `now_ms` is the time base of km_led_work(). The first valid frame after none counts as key activity.
 * ext_rgb: colours. ext_gray: grey levels 0..255 (what km_anim_tick() produces), tinted with cfg.color when drawn.
 * ext_end: no frame any more (the stream ended / the clip stopped): the effect shows dark again. A source expires on its own as described at
 * KM_LED_EXT_STREAM / KM_LED_EXT_ANIM. Frames are stored even while another effect is selected, but only effect 8 (stream) / 9 (anim) draws them. */
void km_led_ext_rgb(km_led_t *l, unsigned source, const km_led_rgb_t px[KM_LED_COUNT], uint32_t now_ms);
void km_led_ext_gray(km_led_t *l, unsigned source, const uint8_t gray[KM_LED_COUNT], uint32_t now_ms);
void km_led_ext_end(km_led_t *l, unsigned source);
bool km_led_ext_valid(const km_led_t *l, unsigned source);

/* ---- life cycle ---- */
void km_led_init(km_led_t *l);                                /* default cfg, nothing started */
void km_led_set_blank(km_led_t *l, bool blank);
/* Idle-off time in seconds (0 = never). Takes effect at once; it does not count as key activity by itself. */
void km_led_set_idle(km_led_t *l, uint16_t seconds);
/* Count this as key activity: the idle time starts again and faded-out lights come back. (km_led_note_keys does this for a key down.) */
void km_led_wake(km_led_t *l);
/* true while the lights are faded out or fading because the idle time ran out */
bool km_led_idle(const km_led_t *l);

/* Tell the core which keys are down right now (call every main-loop pass; it costs a few cycles when nothing is pressed).
 * `down_slots` are sensing slots (the engine's list). Slots without a light are ignored. */
void km_led_note_keys(km_led_t *l, const uint8_t *down_slots, unsigned n);

/* Do at most `max_units` units of work (one unit = one light rendered or encoded) for the frame in progress, starting a new frame
 * when its time has come. Never waits. Returns true when a finished frame is waiting (km_led_frame() != NULL). */
bool km_led_work(km_led_t *l, uint32_t now_ms, km_led_depth_fn depth, void *ctx, unsigned max_units);

/* The finished frame (KM_LED_FRAME_BYTES bytes) or NULL. It stays valid and unchanged until km_led_frame_taken(). */
const uint8_t *km_led_frame(const km_led_t *l);
/* The consumer started sending the frame: the core may start the next one (in the other buffer). */
void km_led_frame_taken(km_led_t *l);

/* ---- geometry (the physical layout, LED_NOTES.md section 3) ---- */
unsigned km_led_slot_of_led(unsigned led);           /* sensing slot of light `led`, KM_LED_SLOTS if out of range */
unsigned km_led_led_of_slot(unsigned slot);          /* light behind a slot, KM_LED_NONE if the slot has no light */
unsigned km_led_x8(unsigned led);                    /* horizontal centre of the key in 1/8 key widths, 0..119 */
unsigned km_led_row(unsigned led);                   /* 0..4, top row first */

/* ---- helpers exposed for the tests ---- */
void km_led_hsv(uint8_t h, uint8_t s, uint8_t v, km_led_rgb_t *out);
/* Encode one light into KM_LED_BYTES_PER_LED bytes. */
void km_led_encode_pixel(const km_led_rgb_t *c, uint8_t *dst);

#endif /* KM_LED_H */
