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

// ---------- Day/night engine ----------
// Shared by _weather and _battery: after sunset the display
// inverts (black background, white ink). The phone sends
// SUNRISE/SUNSET epochs (and its own NIGHT verdict) with each
// fetch; the watch persists them and re-evaluates on a minute
// tick, so the flip happens on time even with no new message.
// Persist keys and message keys stay per-app (their own
// numbering); the app passes them in.

FAMILY_API static bool family_night;
FAMILY_API static int family_sunrise;  // epoch, 0 = unknown
FAMILY_API static int family_sunset;   // epoch, 0 = unknown

FAMILY_API static void family_night_eval(void) {
  if (family_sunrise > 0 && family_sunset > family_sunrise) {
    time_t now = time(NULL);
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

// ---------- Quiet Time earmarks ----------
// When Quiet Time is active (and it is not night — at night the
// marks come off, the face is all black anyway; user,
// 2026-10-26), draw a concave fillet in all four screen corners
// (each corner box minus a disc of radius leg centred one leg
// inside its corner): full width along both edges, thinning
// along a circular arc toward the diagonal, so the mark hugs
// the corner. One shared implementation for the whole family —
// each app calls it at the end of its topmost full-screen draw,
// in screen space, with its own ink color.
#define FAMILY_QT_EARMARK_LEG 24
FAMILY_API static void family_qt_earmarks_ex(GContext *ctx,
                                               GRect bounds,
                                               GColor ink, bool night) {
  if (!quiet_time_is_active() || night) return;
  const int leg = FAMILY_QT_EARMARK_LEG;
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

// Plain form: gates on the family night state. Apps whose night
// lives elsewhere (the watchface computes its own) call the _ex
// form with their own night flag instead — the plain gate read
// an always-false family_night there and the marks showed at
// night (user report, fixed v1.4.11 of the face).
FAMILY_API static void family_qt_earmarks(GContext *ctx, GRect bounds,
                                          GColor ink) {
  family_qt_earmarks_ex(ctx, bounds, ink, family_night);
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
