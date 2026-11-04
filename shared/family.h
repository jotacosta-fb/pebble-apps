// family.h — shared code for the _app family (author "Jota"):
// _weather, _battery, _countdown, _life and the _watchface fork.
//
// Header-only on purpose: every app builds as a single
// translation unit out of src/c/** (see the wscripts), so a
// shared .c file would need build surgery in five places; a
// header keeps each app building exactly as before. Apps
// include it as "../../shared/family.h" AFTER <pebble.h>.
//
// What lives here is what the apps genuinely share: the tunable
// parameters, the appear-wobble engine, the day/night engine,
// the idle exit, and two math helpers. What does NOT live here:
// persist-key and message-key numbers (each app's own space),
// UUIDs, layout geometry, and each app's own logic.
//
// Tunables are family defaults; an app overrides one by
// #defining it before the include (e.g. _weather's wobble runs
// longer and wider than the family norm).

#pragma once

#include <stdlib.h>
#include <time.h>

#ifndef FAMILY_WOBBLE_DELAY_MS
#define FAMILY_WOBBLE_DELAY_MS 250
#endif
#ifndef FAMILY_WOBBLE_LENGTH_MS
#define FAMILY_WOBBLE_LENGTH_MS 400
#endif
#ifndef FAMILY_WOBBLE_AMP
#define FAMILY_WOBBLE_AMP 10
#endif
#ifndef FAMILY_IDLE_TIMEOUT_MS
#define FAMILY_IDLE_TIMEOUT_MS 30000
#endif

// Header-only statics: an app that doesn't use every piece must
// not warn about the rest.
#define FAMILY_API __attribute__((unused))

// ---------- Math helpers ----------

// Integer square root (floor). The bit-by-bit version from
// _watchface — _weather and _life used naive counting loops
// with identical results.
FAMILY_API static int family_isqrt(int v) {
  uint32_t n = (uint32_t)v;
  uint32_t root = 0;
  uint32_t bit = 1u << 30;
  while (bit > n) bit >>= 2;
  while (bit) {
    if (n >= root + bit) {
      n -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return (int)root;
}

// Blend two 8-bit Pebble colours channel-wise; cov runs 0
// (all bg) to 4 (all fg). Used by the rotated/tilted renderers'
// supersampling (was lean_blend in _weather, blend in _watchface
// — the same code twice).
FAMILY_API static uint8_t family_blend(uint8_t fg, uint8_t bg, int cov) {
  uint8_t out = 0xC0;  // fully opaque
  for (int shift = 0; shift <= 4; shift += 2) {
    int f = (fg >> shift) & 3;
    int b = (bg >> shift) & 3;
    int v = (f * cov + b * (4 - cov) + 2) / 4;
    out |= (uint8_t)(v << shift);
  }
  return out;
}

// ---------- Appear wobble ----------
// The family motion (user-approved on _watchface, then brought
// to every app): one full sine swing with a linear decay. The
// offset starts at 0 (the app is already on screen), swings one
// way, back past centre smaller, and settles at 0 — so the
// resting display is exactly the un-animated one.
//
// The engine owns the offset and the Animation bookkeeping.
// The app supplies a hook called with each new offset (and 0
// when the wobble stops): the hook applies the offset to its
// own drawing (by marking a layer dirty — the draw code reads
// family_wobble_offset — or, on the watchface, by setting its
// own row offsets). What the offset MOVES stays app-specific:
// a diagonal slide, a split around a centre, an underline.
//
// family_wobble_configure() retunes the run parameters; the
// watchface calls it on every start because its delay/length
// are user settings, the apps never call it (compile-time
// FAMILY_ constants).

FAMILY_API static int family_wobble_offset;  // px, 0 at rest
FAMILY_API static bool s_family_wobble_running;
FAMILY_API static Animation *s_family_wobble_anim;
FAMILY_API static void (*s_family_wobble_hook)(int offset);
FAMILY_API static int s_family_wobble_amp = FAMILY_WOBBLE_AMP;
FAMILY_API static int s_family_wobble_delay = FAMILY_WOBBLE_DELAY_MS;
FAMILY_API static int s_family_wobble_length = FAMILY_WOBBLE_LENGTH_MS;

FAMILY_API static void family_wobble_configure(int amp, int delay_ms,
                                               int length_ms) {
  s_family_wobble_amp = amp;
  s_family_wobble_delay = delay_ms;
  s_family_wobble_length = length_ms;
}

static void family_wobble_update(Animation *animation,
                                 const AnimationProgress progress) {
  int32_t t = (int32_t)progress;            // 0..65535
  int32_t angle = (int32_t)(((int64_t)t * TRIG_MAX_ANGLE) >> 16);
  int32_t sn = cos_lookup(angle - TRIG_MAX_ANGLE / 4);  // sin, Q16
  family_wobble_offset = (int)((int64_t)s_family_wobble_amp * sn *
                               (65536 - t) / ((int64_t)65536 * 65536));
  if (s_family_wobble_hook) s_family_wobble_hook(family_wobble_offset);
}

// File scope: animation_set_implementation keeps the pointer.
static const AnimationImplementation s_family_wobble_impl = {
    .update = family_wobble_update};

static void family_wobble_stopped(Animation *animation, bool finished,
                                  void *context) {
  s_family_wobble_running = false;
  s_family_wobble_anim = NULL;
  family_wobble_offset = 0;
  if (s_family_wobble_hook) s_family_wobble_hook(0);
}

FAMILY_API static void family_wobble_start(void (*hook)(int offset)) {
  if (s_family_wobble_running) return;
  s_family_wobble_running = true;
  s_family_wobble_hook = hook;
  Animation *a = animation_create();
  s_family_wobble_anim = a;
  animation_set_delay(a, s_family_wobble_delay);
  animation_set_duration(a, s_family_wobble_length);
  animation_set_implementation(a, &s_family_wobble_impl);
  animation_set_handlers(
      a, (AnimationHandlers){.stopped = family_wobble_stopped}, NULL);
  animation_schedule(a);
}

FAMILY_API static void family_wobble_stop(void) {
  if (!s_family_wobble_running) return;
  Animation *a = s_family_wobble_anim;
  s_family_wobble_anim = NULL;
  s_family_wobble_running = false;
  family_wobble_offset = 0;
  if (a) animation_unschedule(a);  // may re-enter the stopped handler; state already zeroed
  if (s_family_wobble_hook) s_family_wobble_hook(0);
}

// ---------- Pop-in rect ----------
// Button-press feedback for buttons with no icon (in _music: Up/Down):
// the same tall rectangle, sliding into view — the iPhone-volume-button
// feel, localized to the button. No growing in and out: the rect keeps
// its constant size and only translates.
//
// Pure function of animation progress, so any app can reuse it: the app
// owns the Animation (any duration); on each update it draws the
// returned rect, and when the animation stops it draws nothing.
//
// progress runs 0..progress_max (ANIMATION_NORMALIZED_MAX for a Pebble
// Animation). slot_x/slot_y is the rect's fully-in-view top-left, and
// rect_w/rect_h is its constant size (tall, mirroring the button).
// Position follows a sine bump: hidden just off the right edge at rest
// (x = slot_x + rect_w, fully off-screen), fully in view at
// mid-animation, back off the edge at the end. So the motion is
// right-to-left in, left-to-right out, decelerating into the slot so it
// briefly lingers there. Integer math only; rounded, not truncated, so
// the rect truly reaches its in-view position (sin_lookup tops out at
// TRIG_MAX_RATIO - 1, and plain truncation would stop 1/10 short).
FAMILY_API static GRect family_pop_rect(int progress, int progress_max,
                                        int slot_x, int slot_y,
                                        int rect_w, int rect_h) {
  int32_t theta = progress_max > 0
      ? (int32_t)(((int64_t)progress * (TRIG_MAX_ANGLE / 2)) / progress_max)
      : 0;
  int32_t sine = sin_lookup(theta);  // 0..TRIG_MAX_RATIO across 0..180°
  int in_x10 = (10 * sine + TRIG_MAX_RATIO / 2) / TRIG_MAX_RATIO;  // 0..10
  int x = slot_x + rect_w - rect_w * in_x10 / 10;
  return GRect(x, slot_y, rect_w, rect_h);
}

// ---------- Shared button slide ----------
// One implementation of the button-press slide for the whole family:
// every app's Up/Select/Down press shows the same flange tab sliding
// right-to-left in from the screen edge and back out, at the pressed
// button's row. The app only wires three calls: start on the press, draw
// at the end of its full-screen layer's update proc, and teardown in
// window_unload. The animation object, progress, button, layer, and
// finish callback all live here — no per-app copies.
//
// Rows and size scale from the layer bounds, so every platform gets the
// same proportions: body 8/200 of the width by 56/228 of the height,
// row centers at 42/228 (Up), 1/2 (Select), 186/228 (Down) of the height,
// right edge on the screen edge. On emery (200x228) that is exactly the
// 8x56 body at tops 14/86/158, flaring to 116 tall at the edge
// (30 px top, 30 px bottom).
//
// The shape is ONE continuous polygon path (no cutouts, no erasing, no
// overlapping shapes), so there are no stray pixels at the joins: the
// outline is tangent-continuous everywhere — the dome nose meets the
// flange curves with horizontal tangents and turns smoothly at its tip,
// flange curves meet the screen edge with vertical tangents. The left
// end is a curved nose, not a rectangle edge: its depth is the body
// width plus a small per-press random variation (-4..-1 px on emery,
// so 4..7 px), and its peak (tip) sits a random -6..+6 px off the
// button row's center, both picked fresh in family_slide_start and
// held for the whole animation, so the nose differs press to press
// but never morphs mid-slide.
//
// The finish callback fires only when the animation runs to completion
// (not when a newer press unschedules it): apps that exit on Up pass a
// callback that pops the window, so the exit waits for the 350 ms slide
// and the feedback stays visible; pressing another button first cancels
// the exit, as it should.
#ifndef FAMILY_SLIDE_DURATION_MS
#define FAMILY_SLIDE_DURATION_MS 250
#endif
typedef void (*FamilySlideFinishedCb)(int button);
FAMILY_API static Animation *s_family_slide_anim = NULL;
FAMILY_API static Layer *s_family_slide_layer = NULL;
FAMILY_API static int s_family_slide_button = 0;  // 0 none, 1 up, 2 select, 3 down
FAMILY_API static int s_family_slide_progress = 0;
FAMILY_API static FamilySlideFinishedCb s_family_slide_cb = NULL;
FAMILY_API static int s_family_slide_dome_var = 0;  // -4..-1, picked per press
FAMILY_API static int s_family_slide_peak_var = 0;  // -6..+6, picked per press
FAMILY_API static bool s_family_slide_seeded = false;

FAMILY_API static void family_slide_update(Animation *animation,
                                           AnimationProgress progress) {
  s_family_slide_progress = progress;
  if (s_family_slide_layer) layer_mark_dirty(s_family_slide_layer);
}

FAMILY_API static void family_slide_stopped(Animation *animation, bool finished,
                                            void *context) {
  int button = s_family_slide_button;
  s_family_slide_button = 0;
  s_family_slide_progress = 0;
  if (s_family_slide_layer) layer_mark_dirty(s_family_slide_layer);
  if (finished && s_family_slide_cb) s_family_slide_cb(button);
}

// The AnimationImplementation/Handlers must be file-scope static const:
// the animation keeps the pointers.
FAMILY_API static const AnimationImplementation s_family_slide_impl = {
  .update = family_slide_update,
};
FAMILY_API static const AnimationHandlers s_family_slide_handlers = {
  .stopped = family_slide_stopped,
};

FAMILY_API static void family_slide_start(int button, Layer *layer,
                                          FamilySlideFinishedCb cb) {
  if (s_family_slide_anim) {
    animation_unschedule(s_family_slide_anim);
    animation_destroy(s_family_slide_anim);
    s_family_slide_anim = NULL;
  }
  // Set after the old animation is gone: unschedule may fire its stopped
  // handler, which resets s_family_slide_button (but never the callback:
  // an unscheduled animation reports finished=false).
  s_family_slide_button = button;
  s_family_slide_layer = layer;
  s_family_slide_cb = cb;
  // Fresh nose for every press: seed once from the clock, then take a
  // small variation (-2..+2 px on the emery scale) that holds for the
  // whole animation, so the nose differs press to press but never
  // morphs mid-slide.
  if (!s_family_slide_seeded) {
    srand((unsigned)time(NULL));
    s_family_slide_seeded = true;
  }
  s_family_slide_dome_var = (int)(rand() % 4) - 4;
  // Peak offset: the nose tip's vertical shift from the button row's
  // center, also fresh per press and held for the whole animation.
  s_family_slide_peak_var = (int)(rand() % 13) - 6;
  s_family_slide_anim = animation_create();
  animation_set_duration(s_family_slide_anim, FAMILY_SLIDE_DURATION_MS);
  animation_set_implementation(s_family_slide_anim, &s_family_slide_impl);
  animation_set_handlers(s_family_slide_anim, s_family_slide_handlers, NULL);
  animation_schedule(s_family_slide_anim);
}

FAMILY_API static void family_slide_teardown(void) {
  // Clear the layer/callback FIRST: the unschedule below fires the
  // stopped handler, and it must not mark a dying layer dirty or fire
  // the callback during unload.
  s_family_slide_layer = NULL;
  s_family_slide_cb = NULL;
  if (s_family_slide_anim) {
    animation_unschedule(s_family_slide_anim);
    animation_destroy(s_family_slide_anim);
    s_family_slide_anim = NULL;
  }
  s_family_slide_button = 0;
  s_family_slide_progress = 0;
}

// Flange tab with a curved-nose left end, drawn as one filled polygon
// path. (x, y) is the body rect's top-left at the current slide
// position; body_w is the body width, rect_h the body height, dome_d
// the nose depth (how far the nose bulges left of the body), peak_off
// the nose tip's vertical shift from the row center. The right end
// flares to rect_h + flare_top + flare_bot tall at the screen edge
// (top and bottom flares rect_h*15/28 = 30 px each on emery).
// The left end is two quarter-ellipses
// meeting at the tip (x-dome_d, cy+peak_off): the upper half spans
// y..tip_y, the lower tip_y..y+rect_h, so the tip can sit off-center
// while both ends still meet the flange curves with horizontal
// tangents and the tip still turns smoothly (vertical tangent).
// Every join is tangent-continuous (see the section comment), so the
// boundary has no steps, gaps, or stray pixels. 128 points covers
// rect_h <= 94 (shipped platforms top out at 56).
FAMILY_API static void family_flange_draw(GContext *ctx, int x, int y,
                                          int body_w, int rect_h,
                                          int dome_d, int peak_off,
                                          GColor color) {
  int flare_top = rect_h * 15 / 28;
  int flare_bot = rect_h * 15 / 28;
  int edge = x + body_w;  // geometric right edge (clipped at the screen)
  int cy = y + rect_h / 2;
  int tip_y = cy + peak_off;
  int r_up = tip_y - y;  // upper dome half's vertical semi-axis
  int r_dn = y + rect_h - tip_y;  // lower dome half's vertical semi-axis
  GPoint points[128];
  int n = 0;
  // Start at the dome's top point = the top flange curve's start.
  points[n++] = GPoint(x, y);
  // Top flange quadratic: (x, y) via (edge, y) to (edge, y-flare_top).
  // Integer Bezier with t = i/16; endpoints exact.
  for (int i = 1; i <= 16; i++) {
    int u = 16 - i;
    int px = (u * u * x + 2 * u * i * edge + i * i * edge) / 256;
    int py = y - flare_top * i * i / 256;
    points[n++] = GPoint(px, py);
  }
  // Right edge down to the bottom tip.
  points[n++] = GPoint(edge, y + rect_h + flare_bot);
  // Bottom flange quadratic: (edge, y+rect_h+flare_bot) via
  // (edge, y+rect_h) to (x, y+rect_h).
  for (int i = 1; i <= 16; i++) {
    int u = 16 - i;
    int px = (u * u * edge + 2 * u * i * edge + i * i * x) / 256;
    int py = y + rect_h + flare_bot * u * u / 256;
    points[n++] = GPoint(px, py);
  }
  // Dome nose: two quarter-ellipses from (x, y+rect_h) around the tip
  // (x-dome_d, tip_y) back toward (x, y). One point per row; the last
  // row is skipped because it duplicates the start point and the
  // path auto-closes.
  for (int k = rect_h - 1; k >= 1; k--) {
    int row = y + k;
    int dx;
    if (row <= tip_y) {
      int dy = row - tip_y;
      dx = (dome_d * family_isqrt(r_up * r_up - dy * dy)) / r_up;
    } else {
      int dy = row - tip_y;
      dx = (dome_d * family_isqrt(r_dn * r_dn - dy * dy)) / r_dn;
    }
    points[n++] = GPoint(x - dx, row);
  }
  GPathInfo info = { .num_points = n, .points = points };
  GPath *path = gpath_create(&info);
  graphics_context_set_fill_color(ctx, color);
  gpath_draw_filled(ctx, path);
  gpath_destroy(path);
}

FAMILY_API static void family_slide_draw(GContext *ctx, GRect bounds,
                                         GColor color) {
  if (s_family_slide_button == 0) return;
  int w = bounds.size.w, h = bounds.size.h;
  int body_w = w * 8 / 200;
  int rect_h = h * 56 / 228;
  int cy = (s_family_slide_button == 1) ? h * 42 / 228
      : (s_family_slide_button == 2) ? h / 2
      : h * 186 / 228;
  GRect r = family_pop_rect(s_family_slide_progress,
                            ANIMATION_NORMALIZED_MAX,
                            w - body_w, cy - rect_h / 2,
                            body_w, rect_h);
  // Nose depth for this animation: body width plus the per-press
  // variation picked in family_slide_start, floored at half the body
  // width so the nose never goes flat. The peak offset rides along
  // unchanged.
  int dome_d = r.size.w + s_family_slide_dome_var;
  if (dome_d < r.size.w / 2) dome_d = r.size.w / 2;
  family_flange_draw(ctx, r.origin.x, r.origin.y, r.size.w, r.size.h,
                     dome_d, s_family_slide_peak_var, color);
}

// ---------- Half-circle mark ----------
// Filled left semicircle: flat side on the screen edge (x = edge_x),
// bulge reaching edge_x - r. Arc points come from the integer sqrt, so
// no trig-angle convention is involved; the polygon's closing edge is
// the vertical diameter. (graphics_fill_radial was tried first and drew
// nothing with inset_thickness 0, so this fan replaces it.) Used for a
// center-button indicator (in _battery: the Select mark). r <= 31 keeps
// the 64-point buffer safe.
FAMILY_API static void family_half_circle(GContext *ctx, int edge_x, int cy,
                                          int r, GColor color) {
  GPoint points[64];
  int n = 0;
  for (int dy = -r; dy <= r; dy++) {
    points[n++] = GPoint(edge_x - family_isqrt(r * r - dy * dy), cy + dy);
  }
  GPathInfo info = { .num_points = n, .points = points };
  GPath *path = gpath_create(&info);
  graphics_context_set_fill_color(ctx, color);
  gpath_draw_filled(ctx, path);
  gpath_destroy(path);
}

// ---------- Day/night engine ----------
// Shared by _weather and _battery: after sunset the display
// inverts (black background, white ink). The watch computes
// sunrise/sunset itself from persisted coordinates (below) and
// re-evaluates on a minute tick, so the flip happens on time
// even with no new message; the phone's SUNRISE/SUNSET epochs
// (and its own NIGHT verdict) stay as the fallback until
// coordinates arrive. Persist keys and message keys stay
// per-app (their own numbering); the app passes them in.

FAMILY_API static bool family_night;
FAMILY_API static int family_sunrise;  // epoch, 0 = unknown
FAMILY_API static int family_sunset;   // epoch, 0 = unknown

// ---------- On-watch sun computation (ported from _watchface) ----------
// The phone sends the location ONCE (LAT/LON, microdegrees,
// persisted on the watch); from then on the watch computes
// sunrise and sunset itself, in integer fixed point (NOAA
// approximation, accurate to a few minutes), once per UTC day.
// The minute tick only compares the current time against the two
// stored instants: no radio, no GPS, no phone reply needed for
// colors. The phone's SUNRISE/SUNSET epochs stay as the fallback
// until coordinates arrive (family_have_place false).
FAMILY_API static int family_lat_e6 = 0;
FAMILY_API static int family_lon_e6 = 0;
FAMILY_API static bool family_have_place = false;
FAMILY_API static time_t family_sun_day = -1;
FAMILY_API static time_t family_sun_rise = 0;
FAMILY_API static time_t family_sun_set = 0;

// acos for a Q16 cosine via binary search on cos_lookup
// (monotonic over 0..180 degrees). Returns a TRIGANGLE.
FAMILY_API static int32_t family_acos_trig(int32_t cos_q16) {
  int32_t lo = 0, hi = TRIG_MAX_ANGLE / 2;
  for (int i = 0; i < 18; i++) {
    int32_t mid = (lo + hi) / 2;
    if (cos_lookup(mid) > cos_q16) lo = mid; else hi = mid;
  }
  return (lo + hi) / 2;
}

FAMILY_API static void family_sun_recompute(time_t now) {
  family_sun_day = now - (now % 86400);
  family_sun_rise = 0;
  family_sun_set = 0;
  if (!family_have_place) return;
  struct tm *tmv = gmtime(&now);
  int doy = tmv->tm_yday;
  // Fractional-year angle for the day, sampled at midday.
  int32_t g = (int32_t)(((int64_t)(2 * doy + 1) * TRIG_MAX_ANGLE) / 730);
  int32_t sg = sin_lookup(g), cg = cos_lookup(g);
  int32_t sg2 = sin_lookup(g * 2), cg2 = cos_lookup(g * 2);
  int32_t sg3 = sin_lookup(g * 3), cg3 = cos_lookup(g * 3);
  // Equation of time, minutes, Q16. Coefficients are the NOAA
  // series x 65536; the sum is scaled by 229.18 at the end.
  int64_t eq_q16 = 5 + ((int64_t)122 * cg >> 16) - ((int64_t)2102 * sg >> 16)
                     - ((int64_t)958 * cg2 >> 16) - ((int64_t)2677 * sg2 >> 16);
  eq_q16 = eq_q16 * 22918 / 100;
  // Solar declination, radians Q16 -> TRIGANGLE (x 65536/2pi).
  int64_t decl_q16 = 453 - ((int64_t)26207 * cg >> 16)
                       + ((int64_t)4604 * sg >> 16)
                       - ((int64_t)443 * cg2 >> 16)
                       + ((int64_t)59 * sg2 >> 16)
                       - ((int64_t)177 * cg3 >> 16)
                       + ((int64_t)97 * sg3 >> 16);
  int32_t decl = (int32_t)((decl_q16 * 10430) >> 16);
  int32_t lat = (int32_t)(((int64_t)family_lat_e6 * TRIG_MAX_ANGLE) / 360000000);
  int32_t cos_lat = cos_lookup(lat), sin_lat = sin_lookup(lat);
  int32_t cos_decl = cos_lookup(decl), sin_decl = sin_lookup(decl);
  int64_t tan_lat = ((int64_t)sin_lat << 16) / cos_lat;
  int64_t tan_decl = ((int64_t)sin_decl << 16) / cos_decl;
  int32_t cos_zen = cos_lookup(
      (int32_t)(((int64_t)TRIG_MAX_ANGLE * 90833) / 360000));
  // cos(H) = cos(zenith)/(cos lat * cos decl) - tan lat * tan decl
  int64_t denom = ((int64_t)cos_lat * cos_decl) >> 16;
  int64_t cos_h = (((int64_t)cos_zen << 16) / denom) -
                  (((int64_t)(int32_t)tan_lat * (int32_t)tan_decl) >> 16);
  if (cos_h >= 65536) {          // the sun never rises today
    family_sun_rise = family_sun_day + 2 * 86400;
    family_sun_set = family_sun_day - 86400;
    return;
  }
  if (cos_h <= -65536) {         // the sun never sets today
    family_sun_rise = family_sun_day - 86400;
    family_sun_set = family_sun_day + 2 * 86400;
    return;
  }
  int32_t h_trig = family_acos_trig((int32_t)cos_h);
  int64_t h_deg_q16 = (int64_t)h_trig * 360;   // degrees, Q16
  int64_t lon_deg_q16 = ((int64_t)family_lon_e6 * 65536) / 1000000;
  int64_t rise_q16 = ((int64_t)720 << 16) - 4 * (lon_deg_q16 + h_deg_q16)
                     - eq_q16;
  int64_t set_q16 = ((int64_t)720 << 16) - 4 * (lon_deg_q16 - h_deg_q16)
                    - eq_q16;
  family_sun_rise = family_sun_day + (time_t)((rise_q16 * 60) >> 16);
  family_sun_set = family_sun_day + (time_t)((set_q16 * 60) >> 16);
}

FAMILY_API static void family_night_eval(void) {
  time_t now = time(NULL);
  if (family_have_place) {
    // On-watch sun times (below) outrank the phone's epochs:
    // they are fresh for the current UTC day no matter when the
    // phone last answered.
    if (now - (now % 86400) != family_sun_day) family_sun_recompute(now);
    family_night = (now < family_sun_rise || now >= family_sun_set);
  } else if (family_sunrise > 0 && family_sunset > family_sunrise) {
    family_night = (now < family_sunrise || now >= family_sunset);
  }
}

// For the minute tick: returns true when the flip just changed
// (the app then refreshes its display).
FAMILY_API static bool family_night_tick(void) {
  bool was = family_night;
  family_night_eval();
  return family_night != was;
}

FAMILY_API static GColor family_ink_color(void) {
  return family_night ? GColorWhite : GColorBlack;
}

FAMILY_API static GColor family_bg_color(void) {
  return family_night ? GColorBlack : GColorWhite;
}

// Restore the persisted state at init — BEFORE the window
// exists, so the first frame is already correct.
FAMILY_API static void family_night_load(int pk_night, int pk_sunrise,
                                         int pk_sunset) {
  if (persist_exists(pk_night)) {
    family_night = persist_read_int(pk_night) != 0;
  }
  if (persist_exists(pk_sunrise)) {
    family_sunrise = persist_read_int(pk_sunrise);
    family_sunset = persist_read_int(pk_sunset);
    family_night_eval();
  }
}

// Apply an incoming message's sun/night tuples: sun epochs are
// stored, persisted and re-evaluated; a NIGHT tuple (the
// phone's own verdict) then wins for the moment, as it did in
// both apps. Returns a bitmask: 1 = sun epochs updated,
// 2 = NIGHT tuple applied. Refreshing stays the app's call.
FAMILY_API static int family_night_apply(DictionaryIterator *iter,
                                         int mk_sunrise, int mk_sunset,
                                         int mk_night, int pk_sunrise,
                                         int pk_sunset, int pk_night) {
  int flags = 0;
  Tuple *t_rise = dict_find(iter, mk_sunrise);
  Tuple *t_set = dict_find(iter, mk_sunset);
  if (t_rise && t_set) {
    family_sunrise = (int)t_rise->value->int32;
    family_sunset = (int)t_set->value->int32;
    persist_write_int(pk_sunrise, family_sunrise);
    persist_write_int(pk_sunset, family_sunset);
    family_night_eval();
    flags |= 1;
  }
  Tuple *t_night = dict_find(iter, mk_night);
  if (t_night) {
    family_night = t_night->value->int32 != 0;
    persist_write_int(pk_night, family_night ? 1 : 0);
    flags |= 2;
  }
  return flags;
}

// Restore persisted coordinates at init — call AFTER
// family_night_load, so the re-evaluation below already uses the
// computed pair for the first frame.
FAMILY_API static void family_place_load(int pk_lat, int pk_lon) {
  if (persist_exists(pk_lat) && persist_exists(pk_lon)) {
    family_lat_e6 = persist_read_int(pk_lat);
    family_lon_e6 = persist_read_int(pk_lon);
    family_have_place = family_lat_e6 >= -90000000 && family_lat_e6 <= 90000000 &&
                        family_lon_e6 >= -180000000 && family_lon_e6 <= 180000000;
    if (family_have_place) family_night_eval();
  }
}

// Apply an incoming message's LAT/LON tuples: stored, persisted,
// and the night state re-evaluated at once so the next refresh
// already uses the computed pair. Returns 1 when coordinates
// were updated.
FAMILY_API static int family_place_apply(DictionaryIterator *iter,
                                          int mk_lat, int mk_lon,
                                          int pk_lat, int pk_lon) {
  Tuple *t_lat = dict_find(iter, mk_lat);
  Tuple *t_lon = dict_find(iter, mk_lon);
  if (t_lat && t_lon) {
    family_lat_e6 = (int)t_lat->value->int32;
    family_lon_e6 = (int)t_lon->value->int32;
    persist_write_int(pk_lat, family_lat_e6);
    persist_write_int(pk_lon, family_lon_e6);
    family_have_place = family_lat_e6 >= -90000000 && family_lat_e6 <= 90000000 &&
                        family_lon_e6 >= -180000000 && family_lon_e6 <= 180000000;
    family_night_eval();
    return 1;
  }
  return 0;
}

// ---------- Corner earmarks ----------
// While the background is light, draw a concave fillet in all
// four screen corners
// (each corner box minus a disc of radius leg centred one leg
// inside its corner): full width along both edges, thinning
// along a circular arc toward the diagonal, so the mark hugs
// the corner. One shared implementation for the whole family —
// each app calls it at the end of its topmost full-screen draw,
// in screen space, with its own ink color.
// 25 px: from the user's side-by-side comparison
// (2026-10-27) of 19 / 24 (the original) / 25 / 29 — 29 and
// then 19 were each shipped and replaced the same day;
// 25 px is the final keeper. Reference
// points from the same session: the Pebble Time 2 glass
// corner measures R 8.5 mm = 68 px at 202 DPI (Core Devices'
// official CAD solid model); the display-concentric value is
// 44 px; 68, 44 and 35 were each shipped and each read
// slightly large on the wrist.
#define FAMILY_EARMARK_LEG 25
// The marks draw only on a LIGHT background (user's rule,
// 2026-10-27 — it replaces the earlier night-state gates: at
// night the background is dark, so they vanish there too, and
// a dark custom background in daytime behaves the same).
// Lightness from the 2-bit channels of the Pebble argb color.
FAMILY_API static bool family_color_is_light(GColor c) {
  int r = (c.argb >> 4) & 3, g = (c.argb >> 2) & 3, b = c.argb & 3;
  return 2 * r + 3 * g + b >= 9;
}

FAMILY_API static void family_earmarks(GContext *ctx, GRect bounds,
                                       GColor ink, GColor bg) {
  // NOT a Quiet Time mark (the user corrected that conflation):
  // the earmarks are pure corner dressing for light backgrounds.
  // Quiet Time has its own indicators (the face's two lines).
  if (!family_color_is_light(bg)) return;
  const int leg = FAMILY_EARMARK_LEG;
  int w = bounds.size.w, h = bounds.size.h;
  graphics_context_set_fill_color(ctx, ink);
  for (int y = 0; y < leg; y++) {
    int dy = leg - y;
    int dx = family_isqrt(leg * leg - dy * dy);
    int width = leg - dx;
    graphics_fill_rect(ctx, GRect(0, y, width, 1), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(w - leg + dx, y, width, 1), 0,
                       GCornerNone);
    graphics_fill_rect(ctx, GRect(0, h - 1 - y, width, 1), 0,
                       GCornerNone);
    graphics_fill_rect(ctx, GRect(w - leg + dx, h - 1 - y, width, 1),
                       0, GCornerNone);
  }
}

// ---------- Idle exit ----------
// _weather/_battery (user): after FAMILY_IDLE_TIMEOUT_MS with
// no interaction the app pops itself back to the watchface —
// the same pop the Up button does. The app re-arms on its own
// idea of interaction (Select, coming to the front); data
// arriving does not count.

FAMILY_API static AppTimer *s_family_exit_timer;

static void family_idle_cb(void *context) {
  s_family_exit_timer = NULL;
  window_stack_pop(true);
}

FAMILY_API static void family_idle_arm(void) {
  if (s_family_exit_timer) app_timer_cancel(s_family_exit_timer);
  s_family_exit_timer =
      app_timer_register(FAMILY_IDLE_TIMEOUT_MS, family_idle_cb, NULL);
}
