#include <pebble.h>

// _life — an MTG life counter. The total in LECO 60, centered on
// screen over a background that tints green (20) to red (0). A + at
// top center and a - at bottom center mirror the Up/Down buttons.
// Starts at 20. Up/Down adjust by 1 (hold to repeat), holding Select
// resets to 20 — same controls language as _countdown. The total
// floors at 0, shown as a plain 0 (an earlier build showed a skull
// there; removed). The total persists.

#define START_LIFE 20
#define MAX_LIFE 999
#define PK_LIFE 1

static Window *s_window;
static Layer *s_main_layer;
// Pre-rendered LECO 1976 digits (bitmaps): a real font can't reach
// size 120 — a 120-digit's glyph bitmap is ~617 bytes, over even
// emery's 512-byte glyph cap — so the glyphs ship as images.
static GBitmap *s_d120[10], *s_d88[10], *s_d86[10], *s_d64[10];
static const uint32_t RES_D120[10] = {
    RESOURCE_ID_D120_0, RESOURCE_ID_D120_1, RESOURCE_ID_D120_2,
    RESOURCE_ID_D120_3, RESOURCE_ID_D120_4, RESOURCE_ID_D120_5,
    RESOURCE_ID_D120_6, RESOURCE_ID_D120_7, RESOURCE_ID_D120_8,
    RESOURCE_ID_D120_9};
static const uint32_t RES_D88[10] = {
    RESOURCE_ID_D88_0, RESOURCE_ID_D88_1, RESOURCE_ID_D88_2,
    RESOURCE_ID_D88_3, RESOURCE_ID_D88_4, RESOURCE_ID_D88_5,
    RESOURCE_ID_D88_6, RESOURCE_ID_D88_7, RESOURCE_ID_D88_8,
    RESOURCE_ID_D88_9};
static const uint32_t RES_D86[10] = {
    RESOURCE_ID_D86_0, RESOURCE_ID_D86_1, RESOURCE_ID_D86_2,
    RESOURCE_ID_D86_3, RESOURCE_ID_D86_4, RESOURCE_ID_D86_5,
    RESOURCE_ID_D86_6, RESOURCE_ID_D86_7, RESOURCE_ID_D86_8,
    RESOURCE_ID_D86_9};
static const uint32_t RES_D64[10] = {
    RESOURCE_ID_D64_0, RESOURCE_ID_D64_1, RESOURCE_ID_D64_2,
    RESOURCE_ID_D64_3, RESOURCE_ID_D64_4, RESOURCE_ID_D64_5,
    RESOURCE_ID_D64_6, RESOURCE_ID_D64_7, RESOURCE_ID_D64_8,
    RESOURCE_ID_D64_9};

static int s_life = START_LIFE;

// Appear wobble (user, v1.10.0 — the family motion from
// _weather/_battery): when the app comes to the front, the total
// swings sideways and settles — one full sine swing with a
// linear decay, starting and ending at 0, so the resting display
// is unchanged. The +/- markers stay put (like _battery's
// battery body). s_wobble is the current offset in px. Any
// button press stops it, and the reset animation cancels it too.
static int s_wobble;
#define WOBBLE_AMP 10
static void wobble_stop(void);  // defined with the animation code below

// Green at 20+, red at 0 and below; the tint routes through yellow
// (20→10 green→yellow, 10→0 yellow→red) so the middle reads as a
// warning instead of a muddy brown.
static GColor life_color(int life) {
#ifdef PBL_COLOR
  if (life >= START_LIFE) return GColorFromRGB(64, 199, 99);
  if (life <= 0) return GColorFromRGB(235, 64, 52);
  int r, g, b;
  if (life >= START_LIFE / 2) {
    int t = (life - START_LIFE / 2) * 255 / (START_LIFE / 2);  // 0..255
    r = 255 + (64 - 255) * t / 255;
    g = 200 + (199 - 200) * t / 255;
    b = 0 + (99 - 0) * t / 255;
  } else {
    int t = life * 255 / (START_LIFE / 2);
    r = 235 + (255 - 235) * t / 255;
    g = 64 + (200 - 64) * t / 255;
    b = 52 + (0 - 52) * t / 255;
  }
  return GColorFromRGB(r, g, b);
#else
  return GColorWhite;
#endif
}

static void refresh_display(void) {
  window_set_background_color(s_window, life_color(s_life));
  layer_mark_dirty(s_main_layer);
}

// The counter floors at 0, displayed as a plain 0 (the skull death
// state was removed at the user's request).
static void anim_cancel(void);  // defined with the animation code below

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  anim_cancel();
  wobble_stop();
  if (s_life < MAX_LIFE) {
    s_life++;
  } else {
    return;
  }
  persist_write_int(PK_LIFE, s_life);
  refresh_display();
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  anim_cancel();
  wobble_stop();
  // Floor is 0 (user): it shows as a plain "0" and goes no lower.
  if (s_life > 0) {
    s_life--;
  } else {
    return;
  }
  persist_write_int(PK_LIFE, s_life);
  refresh_display();
}

static void anim_start(void);  // defined with the animation code below

static void select_long_handler(ClickRecognizerRef recognizer,
                                void *context) {
  if (s_life == START_LIFE) {
    vibes_short_pulse();
    return;  // already at 20: nothing to refresh
  }
  anim_start();
}

static void click_config_provider(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 120,
                                          up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 120,
                                          down_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long_handler,
                              NULL);
}

// ---------- Value drawing (shared by the normal view and the
// reset animation) ----------

static void draw_markers(GContext *ctx, GRect b) {
  int cx = b.size.w / 2;
  int arm = b.size.w >= 180 ? 9 : 7;
  int thick = b.size.w >= 180 ? 4 : 3;
  int plus_cy = b.size.h * 9 / 100;
  int minus_cy = b.size.h * 91 / 100;
  graphics_fill_rect(ctx, GRect(cx - arm, plus_cy - thick / 2, arm * 2, thick),
                     0, GCornerNone);
  graphics_fill_rect(ctx, GRect(cx - thick / 2, plus_cy - arm, thick, arm * 2),
                     0, GCornerNone);
  graphics_fill_rect(ctx, GRect(cx - arm, minus_cy - thick / 2, arm * 2, thick),
                     0, GCornerNone);
}

// Half-diagonal of a value's ink, so the animation knows when the
// refresh wave has covered it.
static int isqrt_int(int v) {
  int r = 0;
  while ((r + 1) * (r + 1) <= v) r++;
  return r;
}

static int value_extent(GRect b, int value) {
  bool big = b.size.w >= 180;
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", value);
  int n = strlen(buf);
  int S = big ? (n >= 3 ? 88 : 120) : (n >= 3 ? 64 : 86);
  int hw = n * (S * 63 / 100) / 2;
  int hh = (S * 70 / 100) / 2;
  return isqrt_int(hw * hw + hh * hh) + 2;
}

// The total from pre-rendered LECO 1976 digit bitmaps: size 120 on
// the big watches (digits 84px tall), 86 on the small ones; a
// 3-digit total steps down (88 / 64) so it still fits the width.
// The bitmaps are the glyphs' ink boxes, so centering the ink
// centers the digits exactly. 0 is a normal value now.
static void draw_value(GContext *ctx, GRect b, int value) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", value);
  int n = strlen(buf);
  bool big = b.size.w >= 180;
  int S = big ? (n >= 3 ? 88 : 120) : (n >= 3 ? 64 : 86);
  GBitmap **set = big ? (n >= 3 ? s_d88 : s_d120)
                      : (n >= 3 ? s_d64 : s_d86);
  int adv = S * 63 / 100;
  int ink_h = S * 70 / 100;
  int x = (b.size.w - n * adv) / 2 + S * 7 / 100 + s_wobble;
  int y = (b.size.h - ink_h) / 2;
  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  for (int i = 0; i < n; i++) {
    GBitmap *bmp = set[buf[i] - '0'];
    GSize bs = gbitmap_get_bounds(bmp).size;
    graphics_draw_bitmap_in_rect(ctx, bmp, GRect(x + i * adv, y, bs.w, bs.h));
  }
}

// ---------- Appear wobble ----------
// One full sine swing with a linear decay: the offset starts at
// 0 (the app is already on screen), swings one way, back past
// centre smaller, and settles at 0. Delayed 250 ms so the
// system's launch slide finishes first — the recipe the user
// approved on _watchface and the sibling apps use. The stopped
// handler zeroes the offset, so the resting display is
// unchanged.

static bool s_wobble_running;
static Animation *s_wobble_anim;

static void wobble_update(Animation *animation,
                          const AnimationProgress progress) {
  int32_t t = (int32_t)progress;            // 0..65535
  int32_t angle = (int32_t)(((int64_t)t * TRIG_MAX_ANGLE) >> 16);
  int32_t sn = cos_lookup(angle - TRIG_MAX_ANGLE / 4);  // sin, Q16
  s_wobble = (int)((int64_t)WOBBLE_AMP * sn * (65536 - t) /
                   ((int64_t)65536 * 65536));
  if (s_main_layer) layer_mark_dirty(s_main_layer);
}

// File scope: animation_set_implementation keeps the pointer.
static const AnimationImplementation s_wobble_impl = {
    .update = wobble_update};

static void wobble_stopped(Animation *animation, bool finished,
                           void *context) {
  s_wobble_running = false;
  s_wobble_anim = NULL;
  s_wobble = 0;
  if (s_main_layer) layer_mark_dirty(s_main_layer);
}

static void wobble_stop(void) {
  if (!s_wobble_running) return;
  Animation *a = s_wobble_anim;
  s_wobble_anim = NULL;
  s_wobble_running = false;
  s_wobble = 0;
  if (a) animation_unschedule(a);  // may re-enter wobble_stopped; state already zeroed
  if (s_main_layer) layer_mark_dirty(s_main_layer);
}

static void start_wobble(void) {
  if (s_wobble_running) return;
  s_wobble_running = true;
  Animation *a = animation_create();
  s_wobble_anim = a;
  animation_set_delay(a, 250);     // user: wobble delay 250 ms
  animation_set_duration(a, 400);  // user: wobble length 400 ms
  animation_set_implementation(a, &s_wobble_impl);
  animation_set_handlers(
      a, (AnimationHandlers){.stopped = wobble_stopped}, NULL);
  animation_schedule(a);
}

static void window_appear(Window *window) { start_wobble(); }

// ---------- Reset animation: black out, then white ----------
// Holding Select runs a two-phase refresh (user spec): a BLACK disc
// expands from the screen's center until the whole screen is black;
// then the new tint (green) expands from the center again, revealing
// the new total as it goes. It lands directly on the normal view:
// tint background and the +/- markers maintained.
// Each phase takes ~0.65s (tuned with the user: v1.9.0's 0.9s
// phases were still a touch slow; the original was too fast).

#define ANIM_PHASE_STEPS 15  // 15 x 33ms ≈ 0.5s per phase (user: faster again)

static bool s_anim = false;
static int s_anim_from = START_LIFE;
static int s_anim_phase = 1;  // 1 = black out, 2 = white in
static int s_anim_step = 0;
static AppTimer *s_anim_timer = NULL;

static int anim_radius(GRect b) {
  int cx = b.size.w / 2, cy = b.size.h / 2;
  int r_max = isqrt_int(cx * cx + cy * cy) + 2;
  int t = s_anim_step * 255 / ANIM_PHASE_STEPS;
  int e;  // ease-in-out
  if (t < 128) {
    e = 2 * t * t / 255;
  } else {
    int u = 255 - t;
    e = 255 - 2 * u * u / 255;
  }
  return r_max * e / 255;
}

static void anim_frame(void *context) {
  s_anim_timer = NULL;
  s_anim_step++;
  if (s_anim_step >= ANIM_PHASE_STEPS) {
    if (s_anim_phase == 1) {
      s_anim_phase = 2;
      s_anim_step = 0;
    } else {
      // Animation ends: settle into the normal view — the tint and
      // the +/- markers come back (user: maintain them at the end).
      s_anim = false;
      window_set_background_color(s_window, life_color(s_life));
      layer_mark_dirty(s_main_layer);
      return;
    }
  }
  s_anim_timer = app_timer_register(33, anim_frame, NULL);
  layer_mark_dirty(s_main_layer);
}

static void anim_start(void) {
  wobble_stop();
  s_anim_from = s_life;
  s_life = START_LIFE;
  persist_write_int(PK_LIFE, s_life);
  vibes_short_pulse();
  s_anim = true;
  s_anim_phase = 1;
  s_anim_step = 0;
  if (s_anim_timer) app_timer_cancel(s_anim_timer);
  s_anim_timer = app_timer_register(33, anim_frame, NULL);
  layer_mark_dirty(s_main_layer);
}

static void anim_cancel(void) {
  if (!s_anim) return;
  s_anim = false;
  if (s_anim_timer) {
    app_timer_cancel(s_anim_timer);
    s_anim_timer = NULL;
  }
  window_set_background_color(s_window, life_color(s_life));
}

// Full-screen layer: + at top center, the total centered,
// - at bottom.
static void main_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_context_set_text_color(ctx, GColorBlack);

  if (s_anim) {
    int cx = b.size.w / 2, cy = b.size.h / 2;
    int r = anim_radius(b);
    if (s_anim_phase == 1) {
      // Old view first; the black disc consumes it from the center.
      draw_markers(ctx, b);
      draw_value(ctx, b, s_anim_from);
      graphics_context_set_fill_color(ctx, GColorBlack);
      if (r > 0) graphics_fill_circle(ctx, GPoint(cx, cy), r);
    } else {
      // All black, then the GREEN tint blooms from the center
      // directly (no white step). The new total is drawn over
      // everything: black digits are only visible where the tint
      // has reached, so they emerge with the wave.
      graphics_context_set_fill_color(ctx, GColorBlack);
      graphics_fill_rect(ctx, b, 0, GCornerNone);
      graphics_context_set_fill_color(ctx, life_color(START_LIFE));
      if (r > 0) graphics_fill_circle(ctx, GPoint(cx, cy), r);
      graphics_context_set_fill_color(ctx, GColorBlack);
      draw_value(ctx, b, START_LIFE);
    }
    return;
  }

  draw_markers(ctx, b);
  draw_value(ctx, b, s_life);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_main_layer = layer_create(bounds);
  layer_set_update_proc(s_main_layer, main_update_proc);
  layer_add_child(root, s_main_layer);

  refresh_display();
}

static void window_unload(Window *window) {
  layer_destroy(s_main_layer);
}

static void init(void) {
  for (int i = 0; i < 10; i++) {
    s_d120[i] = gbitmap_create_with_resource(RES_D120[i]);
    s_d88[i] = gbitmap_create_with_resource(RES_D88[i]);
    s_d86[i] = gbitmap_create_with_resource(RES_D86[i]);
    s_d64[i] = gbitmap_create_with_resource(RES_D64[i]);
  }
  if (persist_exists(PK_LIFE)) {
    s_life = persist_read_int(PK_LIFE);
    if (s_life < 0) s_life = 0;  // legacy negatives read as dead
    if (s_life > MAX_LIFE) s_life = START_LIFE;
  }
  s_window = window_create();
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers){
                                           .load = window_load,
                                           .appear = window_appear,
                                           .unload = window_unload,
                                       });
  window_stack_push(s_window, true);
}

static void deinit(void) {
  window_destroy(s_window);
  for (int i = 0; i < 10; i++) {
    gbitmap_destroy(s_d120[i]);
    gbitmap_destroy(s_d88[i]);
    gbitmap_destroy(s_d86[i]);
    gbitmap_destroy(s_d64[i]);
  }
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
