#include <pebble.h>

// _timer — a countdown timer. Just the time, nothing else. The
// state is marked on the time itself: a thick underline under
// the minutes only means ready to start; a giant light-grey
// pause symbol behind the time means paused; a full black band
// (blinking) means done; no mark means running. Defaults to 45:00. Up/Down adjust by one minute (hold to
// repeat), Select starts/stops, holding Select resets to the set time.
// Remaining time is computed from a stored end timestamp, so it's
// drift-free, and a wakeup is scheduled so the timer still fires if the
// app is closed.

#define DEFAULT_SECONDS (45 * 60)
#define MAX_SECONDS (99 * 60)
#define MIN_SET_SECONDS 60

#define PK_SET 1
#define PK_END 2
#define PK_STATE 3
#define PK_REMAINING 4

enum { ST_EDIT, ST_RUNNING, ST_PAUSED, ST_DONE };

static Window *s_window;
static GFont s_f66, s_f47, s_f56, s_f40;  // LECO 1976 (custom font)
static GFont s_f50;
#if defined(PBL_PLATFORM_EMERY)
static GFont s_f70;  // emery-only (a 70 digit exceeds the 256B glyph cap elsewhere)
#endif
static Layer *s_main_layer;
static Layer *s_back_layer;

static int s_state = ST_EDIT;
static int s_set_seconds = DEFAULT_SECONDS;
static int s_remaining = DEFAULT_SECONDS;
static time_t s_end_time = 0;
static WakeupId s_wakeup_id = -1;
static bool s_blink_on = true;

// Underline wobble (user, v1.5.0; re-scoped v1.5.1): the family
// motion, but ONLY the ready underline moves — the time itself
// sits still. It runs when the app is shown and on reset, the
// two moments the underline (re)appears; if no underline is
// visible (restored running/paused/done), nothing animates.
// One full sine swing with a linear decay, starting and ending
// at 0, so the resting display is unchanged. s_wobble is the
// current offset in px. Any button press stops it.
static int s_wobble;
#define WOBBLE_AMP 10
static void wobble_stop(void);   // defined with the animation code below
static void start_wobble(void);  // defined with the animation code below

static int display_seconds(void) {
  if (s_state == ST_RUNNING) {
    int left = (int)(s_end_time - time(NULL));
    return left > 0 ? left : 0;
  }
  return s_remaining;
}

static void save_state(void) {
  persist_write_int(PK_SET, s_set_seconds);
  persist_write_int(PK_END, (int)s_end_time);
  persist_write_int(PK_STATE, s_state);
  persist_write_int(PK_REMAINING, s_remaining);
}

static void refresh_display(void) {
  layer_mark_dirty(s_main_layer);
  if (s_back_layer) layer_mark_dirty(s_back_layer);
}

static void cancel_wakeup(void) {
  if (s_wakeup_id >= 0) {
    wakeup_cancel(s_wakeup_id);
    s_wakeup_id = -1;
  }
}

static void finish_timer(void) {
  s_state = ST_DONE;
  s_remaining = 0;
  cancel_wakeup();
  static const uint32_t segments[] = {200, 100, 200, 100, 500};
  VibePattern pattern = {
      .durations = segments,
      .num_segments = ARRAY_LENGTH(segments),
  };
  vibes_enqueue_custom_pattern(pattern);
  save_state();
  refresh_display();
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  if (s_state == ST_RUNNING && display_seconds() <= 0) {
    finish_timer();
    return;
  }
  if (s_state == ST_DONE) {
    s_blink_on = !s_blink_on;
    layer_mark_dirty(s_main_layer);
  } else if (s_state == ST_RUNNING) {
    layer_mark_dirty(s_main_layer);
  }
}

static void start_timer(void) {
  if (s_remaining <= 0) s_remaining = s_set_seconds;
  s_end_time = time(NULL) + s_remaining;
  s_state = ST_RUNNING;
  cancel_wakeup();
  s_wakeup_id = wakeup_schedule(s_end_time, 0, true);
  save_state();
  refresh_display();
}

static void pause_timer(void) {
  s_remaining = display_seconds();
  s_state = ST_PAUSED;
  cancel_wakeup();
  save_state();
  refresh_display();
}

static void reset_timer(void) {
  // Per spec: holding Select resets back to 45:00, the default — this
  // also restores the configured start value to 45.
  s_state = ST_EDIT;
  s_set_seconds = DEFAULT_SECONDS;
  s_remaining = DEFAULT_SECONDS;
  cancel_wakeup();
  save_state();
  refresh_display();
  start_wobble();  // the underline is back: it wobbles (user, v1.5.1)
}

static void select_click_handler(ClickRecognizerRef recognizer,
                                 void *context) {
  wobble_stop();
  if (s_state == ST_RUNNING) {
    pause_timer();
  } else if (s_state == ST_DONE) {
    reset_timer();
  } else {
    start_timer();
  }
}

static void select_long_handler(ClickRecognizerRef recognizer,
                                void *context) {
  wobble_stop();
  reset_timer();
  vibes_short_pulse();
}

static void adjust_minutes(int delta) {
  wobble_stop();
  if (s_state == ST_RUNNING || s_state == ST_DONE) return;
  int value = s_remaining + delta * 60;
  if (value < MIN_SET_SECONDS) value = MIN_SET_SECONDS;
  if (value > MAX_SECONDS) value = MAX_SECONDS;
  s_remaining = value;
  if (s_state == ST_EDIT) s_set_seconds = value;
  save_state();
  refresh_display();
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  adjust_minutes(1);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  adjust_minutes(-1);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long_handler,
                              NULL);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 150,
                                          up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150,
                                          down_click_handler);
}

// Main: "MM:SS" in LECO, colon drawn as two small squares (LECO-style).

static void main_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  bool big = b.size.w >= 180;
  if (s_state == ST_DONE && !s_blink_on) return;  // blink while done
// LECO 1976, stepped up a size (user): emery leads at 70, small
  // watches at 50 — the fit chain below keeps it on one line.
#if defined(PBL_PLATFORM_EMERY)
  GFont font = big ? s_f70 : s_f50;
#else
  GFont font = big ? s_f66 : s_f50;
#endif
  int total_s = display_seconds();
  char min_buf[8], sec_buf[8];
  snprintf(min_buf, sizeof(min_buf), "%d", total_s / 60);
  snprintf(sec_buf, sizeof(sec_buf), "%02d", total_s % 60);

  GRect measure = GRect(0, 0, 300, b.size.h);
  // The font is chosen from the WIDEST strings the timer can show
  // ("44:44"), never from the digits currently on screen: measuring
  // the actual string let narrow digits keep a bigger font (user
  // bug: 41:00 rendered larger than 45:00). One size for all times.
  int colon_w = big ? 24 : 18;  // widened: the minutes box crowded the colon
  for (;;) {
    GSize pmin = graphics_text_layout_get_content_size(
        "44", font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
    GSize psec = graphics_text_layout_get_content_size(
        "44", font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
    if (pmin.w + colon_w + psec.w <= b.size.w - 6) break;
    GFont next = font;
#if defined(PBL_PLATFORM_EMERY)
    if (font == s_f70) next = big ? s_f56 : s_f47;
    else
#endif
    if (font == s_f66) next = big ? s_f56 : s_f47;
    else if (font == s_f50) next = s_f47;
    else if (font == s_f47) next = s_f40;
    if (next == font) break;
    font = next;
  }
  GSize min_sz = graphics_text_layout_get_content_size(
      min_buf, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
  GSize sec_sz = graphics_text_layout_get_content_size(
      sec_buf, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
  int total = min_sz.w + colon_w + sec_sz.w;
  (void)total_s;

  int x = (b.size.w - total) / 2;
  // Decorations hug the glyph box (LECO glyphs span ~30%-98% of
  // their line height) with a little padding. Padding shrinks if
  // the (wide) time would push a box to the screen edges — boxes
  // must always hug, never bleed.
  int pad_x = (b.size.w - total) / 2 - 3;
  if (pad_x > 10) pad_x = 10;
  if (pad_x < 3) pad_x = 3;
  int pad_y = big ? 8 : 6;
  int glyph_top = min_sz.h * 30 / 100;
  int glyph_bottom = min_sz.h * 98 / 100;

  // State marks (user): ready boxes the minutes only — [45]:00;
  // done keeps the full black band; running has no mark. Paused is
  // marked by the backdrop layer behind everything (see below).
  bool underline_minutes = (s_state == ST_EDIT);
  bool band_full = (s_state == ST_DONE);
  if (band_full) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx,
                       GRect(x - pad_x, glyph_top - pad_y, total + 2 * pad_x,
                             (glyph_bottom - glyph_top) + 2 * pad_y),
                       0, GCornerNone);
  } else if (underline_minutes) {
    // Thick underline under the minutes only (user, replacing the
    // minutes box): a few px below the glyph box (lowered 2px
    // more in v1.5.0, user). Bigger and dark grey (user, v1.5.2):
    // wider than the minutes on both sides and thicker, in both
    // size tiers, so the wobble reads.
    int u_ext = big ? 6 : 4;
    graphics_context_set_fill_color(ctx, GColorDarkGray);
    graphics_fill_rect(ctx,
                       GRect(x + s_wobble - u_ext, glyph_bottom + 5,
                             min_sz.w + 2 * u_ext, big ? 8 : 6),
                       0, GCornerNone);
  }

  // Digits are white only inside the done band; everything else
  // is black on the plain screen.
  GColor min_fg = band_full ? GColorWhite : GColorBlack;
  GColor rest_fg = min_fg;
  graphics_context_set_text_color(ctx, min_fg);
  graphics_draw_text(ctx, min_buf, font, GRect(x, 0, min_sz.w + 2, b.size.h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, rest_fg);
  graphics_context_set_fill_color(ctx, rest_fg);
  int sq = big ? 6 : 4;
  int colon_cx = x + min_sz.w + colon_w / 2;
  int cy1 = min_sz.h * 54 / 100;
  int cy2 = min_sz.h * 76 / 100;
  graphics_fill_rect(ctx, GRect(colon_cx - sq / 2, cy1 - sq / 2, sq, sq), 0,
                     GCornerNone);
  graphics_fill_rect(ctx, GRect(colon_cx - sq / 2, cy2 - sq / 2, sq, sq), 0,
                     GCornerNone);
  graphics_draw_text(ctx, sec_buf, font,
                     GRect(x + min_sz.w + colon_w, 0, sec_sz.w + 2, b.size.h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
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

static void window_appear(Window *window) {
  // Shown with no underline (restored running/paused/done):
  // don't animate (user, v1.5.1).
  if (s_state == ST_EDIT) start_wobble();
}

// ---------- Backdrop: the paused mark ----------
// Full-screen layer behind the time: a giant light-grey pause
// symbol (two bars), centered on the screen.
static void back_update_proc(Layer *layer, GContext *ctx) {
  if (s_state != ST_PAUSED) return;
  GRect b = layer_get_bounds(layer);
  bool big = b.size.w >= 180;
  // Giant light-grey pause bars behind the time (user's pick).
  int bw = big ? 34 : 22;
  int bh = big ? 130 : 92;
  int gap = big ? 26 : 18;
  int bx = (b.size.w - (2 * bw + gap)) / 2;
  int by = (b.size.h - bh) / 2;
  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_fill_rect(ctx, GRect(bx, by, bw, bh), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(bx + bw + gap, by, bw, bh), 0, GCornerNone);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  int w = bounds.size.w;
  int h = bounds.size.h;
  int main_h = w >= 180 ? 78 : 54;

  // Layer centered, then nudged up 2-3px: the digits sit a touch low
  // in their line box, so the box center isn't the visual center.
  s_back_layer = layer_create(bounds);
  layer_set_update_proc(s_back_layer, back_update_proc);
  layer_add_child(root, s_back_layer);

  s_main_layer = layer_create(
      GRect(0, (h - main_h) / 2 - (w >= 180 ? 2 : 3), w, main_h));
  layer_set_update_proc(s_main_layer, main_update_proc);
  layer_add_child(root, s_main_layer);

  refresh_display();
}

static void window_unload(Window *window) {
  layer_destroy(s_main_layer);
  layer_destroy(s_back_layer);
}

static void wakeup_handler(WakeupId id, int32_t reason) {
  if (s_state == ST_RUNNING) finish_timer();
}

static void init(void) {
  s_f66 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_66));
  s_f50 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_50));
#if defined(PBL_PLATFORM_EMERY)
  s_f70 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_70));
#endif
  s_f47 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_47));
  s_f56 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_56));
  s_f40 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_40));
  // Restore state
  if (persist_exists(PK_SET)) {
    s_set_seconds = persist_read_int(PK_SET);
    if (s_set_seconds < MIN_SET_SECONDS || s_set_seconds > MAX_SECONDS) {
      s_set_seconds = DEFAULT_SECONDS;
    }
  }
  s_remaining = s_set_seconds;
  bool launched_by_wakeup = (launch_reason() == APP_LAUNCH_WAKEUP);
  if (persist_exists(PK_STATE)) {
    int st = persist_read_int(PK_STATE);
    if (st == ST_RUNNING && persist_exists(PK_END)) {
      s_end_time = (time_t)persist_read_int(PK_END);
      if (s_end_time > time(NULL)) {
        s_state = ST_RUNNING;
        s_wakeup_id = wakeup_schedule(s_end_time, 0, true);
      } else {
        s_state = ST_DONE;
        s_remaining = 0;
      }
    } else if (st == ST_PAUSED && persist_exists(PK_REMAINING)) {
      s_remaining = persist_read_int(PK_REMAINING);
      s_state = ST_PAUSED;
    }
  }
  if (launched_by_wakeup) {
    s_state = ST_DONE;
    s_remaining = 0;
  }

  s_window = window_create();
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers){
                                           .load = window_load,
                                           .appear = window_appear,
                                           .unload = window_unload,
                                       });
  window_stack_push(s_window, true);
  tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
  wakeup_service_subscribe(wakeup_handler);
  if (launched_by_wakeup) {
    static const uint32_t segments[] = {200, 100, 200, 100, 500};
    VibePattern pattern = {
        .durations = segments,
        .num_segments = ARRAY_LENGTH(segments),
    };
    vibes_enqueue_custom_pattern(pattern);
  }
  refresh_display();
}

static void deinit(void) {
  save_state();
  window_destroy(s_window);
  fonts_unload_custom_font(s_f66);
  fonts_unload_custom_font(s_f50);
#if defined(PBL_PLATFORM_EMERY)
  fonts_unload_custom_font(s_f70);
#endif
  fonts_unload_custom_font(s_f47);
  fonts_unload_custom_font(s_f56);
  fonts_unload_custom_font(s_f40);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
