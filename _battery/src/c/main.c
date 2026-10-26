#include <pebble.h>
#include "../../shared/family.h"

// Battery Days — a deliberately tiny battery app.
// Shows exactly: current battery %, and a prediction of how many
// days are left in this charge: days = % / learned drain rate
// (%/day); until a rate is learned it assumes 15%/day.
//
// How the rate is learned (v2.8.0 — rewritten after the old
// method drifted): the old code timed each 1% step and blended
// the observations 2:1 into a stored rate. Percent steps are
// not evenly spaced — the battery's voltage curve is flat
// mid-discharge and the firmware smooths its readings — so
// step times kept stretching, the "observed" rate kept
// shrinking, and the prediction kept growing (77% read as 14
// days). Now the app anchors the START of each discharge
// stretch (charge % and time when the charger came off, or
// the first sample) plus the lowest % seen since, and
// computes the rate over the whole stretch — but only once
// the stretch is at least 12 hours old and at least 3% deep,
// so it rests on real signal. Within a stretch the window
// only grows, so estimates converge instead of drifting. If
// the charge ever rises 4% or more off its low without the
// app seeing a charger, a charge happened in between: that
// stretch is discarded and re-anchored rather than averaged
// in. (A top-up smaller than that can still slip through;
// the long window limits the damage.)

static Window *s_window;


static GFont s_f68, s_f48, s_f56, s_f40;  // LECO 1976 (custom font)
static GFont s_p34, s_p28, s_p24, s_p20;  // LECO 1976 %-only fonts at HALF the digit heights (user: full size was too large)
static Layer *s_pct_layer;
// Appear wobble is the family engine (shared/family.h): the
// readout splits around its decimal dot — left side slides
// left, right side slides right, dot stays (v2.6.0). While
// charging there is no dot, so the % readout keeps the plain
// sideways wobble. The battery body never moves.
static Layer *s_days_layer;

static int s_pct = -1;         // -1 = no reading yet
static bool s_charging = false;
static int s_rate_x10 = 0;     // drain rate in % per day x10, 0 = not learned
static int s_days_x10 = -1;    // predicted days left x10

#define PERSIST_RATE_X10 1
#define PERSIST_LAST_PCT 2    // now the stretch ANCHOR % (see header)
#define PERSIST_LAST_TIME 3   // now the stretch ANCHOR time
#define PERSIST_LOW_PCT 7     // lowest % seen in the current stretch
#define PERSIST_SCHEMA 8      // learning-schema version (1 = v2.8.0)
#define PERSIST_NIGHT 4
#define PERSIST_SUNRISE 5
#define PERSIST_SUNSET 6

// Night mode is the family engine (shared/family.h): a small
// JS companion fetches sunrise/sunset from Open-Meteo; the
// watch stores them and re-evaluates day/night on the tick.
static void refresh_display(void);

static void tick_handler(struct tm *tick_time, TimeUnits units) {
  if (family_night_tick()) refresh_display();
}
#define DEFAULT_RATE_X10 150   // 15%/day (~6-7 days) until a rate is learned

// ---------- Battery icon ----------

// The battery is no longer a separate icon: one BIG battery is
// drawn as the background behind the percentage digits (user:
// merge the numbers and the battery icon).
static void draw_battery_bg(GContext *ctx, GRect b) {
  bool big = b.size.w >= 180;
  int bw = big ? 172 : 126;
  int bh = big ? 100 : 78;
  int x = (b.size.w - bw) / 2;
  // Centered on screen, as before (the header-position trial was
  // reverted at the user's request: keep the center alignment).
  int y = (b.size.h - bh) / 2;
  int sw = 3;

  // Outline + terminal nub
  graphics_context_set_stroke_color(ctx, family_ink_color());
  graphics_context_set_stroke_width(ctx, sw);
  graphics_draw_rect(ctx, GRect(x, y, bw, bh));
  graphics_context_set_fill_color(ctx, family_ink_color());
  int nub_h = bh / 3;
  graphics_fill_rect(ctx, GRect(x + bw + 2, y + (bh - nub_h) / 2, 5, nub_h),
                     0, GCornerNone);

  // Fill level, inset from the outline
  if (s_pct > 0) {
    int inner_w = bw - 2 * sw - 4;
    int fill_w = inner_w * s_pct / 100;
    if (fill_w < 2) fill_w = 2;
#ifdef PBL_COLOR
    // Red alert when less than a day is left (charging stays green).
    bool low = !s_charging && s_days_x10 >= 0 && s_days_x10 < 10;
    // Lighter fills (user): pale greens behind the digits; the
    // low-battery red stays saturated so the alert still pops.
    // At night the fills go darker (user's night mode) so the
    // white digits stay readable over them.
    graphics_context_set_fill_color(
        ctx, low ? GColorRed
                 : (s_charging
                        ? (family_night ? GColorIslamicGreen : GColorMintGreen)
                        : (family_night ? GColorDarkGreen : GColorScreaminGreen)));
#else
    graphics_context_set_fill_color(ctx,
                                    family_night ? GColorDarkGray : GColorLightGray);
#endif
    graphics_fill_rect(ctx,
        GRect(x + sw + 2, y + sw + 2, fill_w, bh - 2 * sw - 4),
        0, GCornerNone);
  }
}

// ---------- Big % (drawn: LECO 1976 digits + Gothic % for style) ----

// The %-only companion for a digit font: half its height (the
// full-size % read too large). At half size it fits every budget.
static GFont pct_font_for(GFont df) {
  if (df == s_f56) return s_p28;
  if (df == s_f48) return s_p24;
  if (df == s_f40) return s_p20;
  return s_p34;  // df == s_f68
}

// The main readout (v2.5.0, user: simplify): no % — the big
// number IS the days left, with the battery fill behind it. The
// % survives only while charging (days can't be predicted then)
// and in the footer on the last day (see days_update_proc).
static void draw_days_big(GContext *ctx, GRect b) {
  bool big = b.size.w >= 180;
  int box_y = b.size.h / 2 - (big ? 44 : 31);
  int box_h = big ? 78 : 54;
  if (s_days_x10 < 0) {
    graphics_draw_text(ctx, "--", s_f48,
                       GRect(family_wobble_offset, box_y, b.size.w, box_h),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }
  char int_buf[8], frac_buf[8];
  snprintf(int_buf, sizeof(int_buf), "%d", s_days_x10 / 10);
  snprintf(frac_buf, sizeof(frac_buf), "%d", s_days_x10 % 10);
  GFont font = big ? s_f68 : s_f48;
  GFont word_font = fonts_get_system_font(
      big ? FONT_KEY_GOTHIC_28_BOLD : FONT_KEY_GOTHIC_24_BOLD);
  GRect measure = GRect(0, 0, 300, box_h);
  GSize i_sz = graphics_text_layout_get_content_size(
      int_buf, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
  GSize f_sz = graphics_text_layout_get_content_size(
      frac_buf, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
  GSize w_sz = graphics_text_layout_get_content_size(
      "days", word_font, measure, GTextOverflowModeFill,
      GTextAlignmentLeft);
  // The decimal dot is drawn by hand, a filled square on the
  // baseline (v2.5.1, user: LECO's own period glyph is too
  // small) — the same trick as _weather's degree marks.
  int dot = i_sz.h / 5;
  if (dot < 8) dot = 8;
  int dot_gap = 3;
  if (i_sz.w + dot_gap + dot + dot_gap + f_sz.w + 6 + w_sz.w >
      b.size.w - 6) {
    font = big ? s_f56 : s_f40;
    i_sz = graphics_text_layout_get_content_size(
        int_buf, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
    f_sz = graphics_text_layout_get_content_size(
        frac_buf, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
    dot = i_sz.h / 5;
    if (dot < 8) dot = 8;
  }
  int total = i_sz.w + dot_gap + dot + dot_gap + f_sz.w + 6 + w_sz.w;
  // The wobble splits the readout around the dot (user, v2.6.0):
  // the left side slides left, the right side slides right, the
  // dot itself stays exactly in place.
  int base = (b.size.w - total) / 2;
  int int_x = base - family_wobble_offset;
  int dot_x = base + i_sz.w + dot_gap;
  int frac_x = dot_x + dot + dot_gap + family_wobble_offset;
  graphics_draw_text(ctx, int_buf, font,
                     GRect(int_x, box_y, i_sz.w + 2, box_h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  graphics_context_set_fill_color(ctx, family_ink_color());
  graphics_fill_rect(ctx,
                     GRect(dot_x, box_y + i_sz.h - dot, dot, dot), 0,
                     GCornerNone);
  graphics_draw_text(ctx, frac_buf, font,
                     GRect(frac_x, box_y, f_sz.w + 2, box_h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  // "days" bottom-aligned with the digits, where the % used to sit.
  int word_y = box_y + i_sz.h - w_sz.h;
  if (word_y < box_y) word_y = box_y;
  graphics_draw_text(ctx, "days", word_font,
                     GRect(frac_x + f_sz.w + 6, word_y, w_sz.w + 2,
                           box_h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

static void pct_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  bool big = b.size.w >= 180;
  graphics_context_set_text_color(ctx, family_ink_color());

  // Merged layout: the big battery is the background; digits on top.
  draw_battery_bg(ctx, b);

  if (s_pct < 0) {
    GFont f = s_f48;
    graphics_draw_text(ctx, "--", f,
                       GRect(family_wobble_offset, 0, b.size.w, b.size.h),
                       GTextOverflowModeFill,
                       GTextAlignmentCenter, NULL);
    return;
  }
  if (!s_charging) {
    draw_days_big(ctx, b);
    return;
  }

  // Digit box placed so the digits' visual center is the screen's
  // vertical center (this layer is full-screen now).
  int box_y = b.size.h / 2 - (big ? 44 : 31);
  int box_h = big ? 78 : 54;

GFont font = big ? s_f68 : s_f48;
  GFont pct_font = pct_font_for(font);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d", s_pct);
  GRect measure = GRect(0, 0, 300, box_h);
  GSize d_sz = graphics_text_layout_get_content_size(
      buf, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
  GSize p_sz = graphics_text_layout_get_content_size(
      "%", pct_font, measure, GTextOverflowModeFill, GTextAlignmentLeft);

  if (d_sz.w + 4 + p_sz.w > b.size.w - 6) {
    font = big ? s_f56 : s_f40;
    pct_font = pct_font_for(font);
    d_sz = graphics_text_layout_get_content_size(
        buf, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
    p_sz = graphics_text_layout_get_content_size(
        "%", pct_font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
  }

  int total = d_sz.w + 4 + p_sz.w;
  int x = (b.size.w - total) / 2 + family_wobble_offset;
  graphics_draw_text(ctx, buf, font, GRect(x, box_y, d_sz.w + 2, box_h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  // The %'s box is bottom-aligned with the digits' box so the
  // baselines land together.
  int pct_y = box_y + d_sz.h - p_sz.h;
  if (pct_y < box_y) pct_y = box_y;
  graphics_draw_text(ctx, "%", pct_font,
                     GRect(x + d_sz.w + 4, pct_y, p_sz.w + 2, box_h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

// ---------- Prediction ----------

static void sample_battery(void) {
  BatteryChargeState bs = battery_state_service_peek();
  s_pct = bs.charge_percent;
  s_charging = bs.is_charging || bs.is_plugged;

  // One-time reset (v2.8.0): rates learned by the old per-step
  // method are poisoned (see header) — relearn from scratch.
  if (!persist_exists(PERSIST_SCHEMA) ||
      persist_read_int(PERSIST_SCHEMA) != 1) {
    persist_delete(PERSIST_RATE_X10);
    persist_delete(PERSIST_LAST_PCT);
    persist_delete(PERSIST_LAST_TIME);
    persist_delete(PERSIST_LOW_PCT);
    persist_write_int(PERSIST_SCHEMA, 1);
  }

  int rate_x10 = persist_exists(PERSIST_RATE_X10)
      ? persist_read_int(PERSIST_RATE_X10) : 0;
  int now = (int)time(NULL);

  bool have_anchor = persist_exists(PERSIST_LAST_PCT) &&
                     persist_exists(PERSIST_LAST_TIME);
  int anchor_pct = have_anchor ? persist_read_int(PERSIST_LAST_PCT) : s_pct;
  int anchor_time = have_anchor ? persist_read_int(PERSIST_LAST_TIME) : now;
  int low_pct = persist_exists(PERSIST_LOW_PCT)
      ? persist_read_int(PERSIST_LOW_PCT) : s_pct;

  if (s_charging) {
    // On the charger the stretch has not started: the anchor
    // follows the charge, so unplugging starts the stretch.
    anchor_pct = s_pct; anchor_time = now; low_pct = s_pct;
  } else if (!have_anchor) {
    anchor_pct = s_pct; anchor_time = now; low_pct = s_pct;
  } else if (s_pct >= low_pct + 4) {
    // The charge rose well off its low without the app seeing
    // a charger: a charge happened mid-stretch. Discard the
    // stretch (no rate learned from it) and re-anchor here.
    anchor_pct = s_pct; anchor_time = now; low_pct = s_pct;
  } else {
    if (s_pct < low_pct) low_pct = s_pct;
    int drop = anchor_pct - s_pct;
    int elapsed = now - anchor_time;
    if (drop >= 3 && elapsed >= 43200) {
      int observed_x10 = (int)((long)drop * 864000 / elapsed);
      if (observed_x10 >= 10 && observed_x10 <= 1000) {
        rate_x10 = observed_x10;
        persist_write_int(PERSIST_RATE_X10, rate_x10);
      }
    }
  }
  persist_write_int(PERSIST_LAST_PCT, anchor_pct);
  persist_write_int(PERSIST_LAST_TIME, anchor_time);
  persist_write_int(PERSIST_LOW_PCT, low_pct);

  s_rate_x10 = rate_x10;
  int eff_rate = rate_x10 > 0 ? rate_x10 : DEFAULT_RATE_X10;
  s_days_x10 = s_pct * 100 / eff_rate;
}

// The footer (v2.5.0): empty in normal use — the days are the
// big readout now. Two exceptions: "charging" while charging,
// and on the last day (1.0 days left or under) the charge %,
// the one moment the exact number beats the estimate.
static void days_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  bool big = b.size.w >= 180;
  GFont font = fonts_get_system_font(
      FONT_KEY_GOTHIC_28);

  if (s_charging) {
    graphics_context_set_text_color(ctx, family_ink_color());
    graphics_draw_text(ctx, "charging", font, b,
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }

  if (s_days_x10 >= 0 && s_days_x10 <= 10 && s_pct >= 0) {
    char pct_buf[8];
    snprintf(pct_buf, sizeof(pct_buf), "%d%%", s_pct);
    graphics_context_set_text_color(ctx, family_ink_color());
    graphics_draw_text(ctx, pct_buf, font, b,
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }

}

static void refresh_display(void) {
  if (s_window) window_set_background_color(s_window, family_bg_color());
  layer_mark_dirty(s_days_layer);
  layer_mark_dirty(s_pct_layer);  // draws digits + battery background
  layer_mark_dirty(s_pct_layer);
}

static void battery_handler(BatteryChargeState state) {
  sample_battery();
  refresh_display();
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  family_idle_arm();
  sample_battery();
  refresh_display();
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  // Top button returns to the watchface (user request).
  window_stack_pop(true);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
}

// ---------- Window ----------

// The appear wobble's hook: the offset moves the readout layer;
// the split application lives in the draw code, which reads
// family_wobble_offset.
static void wobble_hook(int offset) {
  if (s_pct_layer) layer_mark_dirty(s_pct_layer);
}

static void request_night(void);  // defined with the AppMessage code below

static void window_appear(Window *window) {
  family_wobble_start(wobble_hook);
  family_idle_arm();
  // Ask the phone for fresh sunrise/sunset on every appear,
  // not just at init (v2.8.1 — the same stale-epochs hole
  // _weather had: a missed reply left yesterday's sun times
  // deciding day/night).
  request_night();
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  int w = b.size.w;
  int h = b.size.h;
  window_set_background_color(window, family_bg_color());



  // Big current percentage over the big battery background —
  // one merged graphic now (the separate bottom icon is gone).
  // Full-screen layer: the background battery centers itself and
  // the proc places the digit box on the screen's vertical center.
  s_pct_layer = layer_create(GRect(0, 0, w, h));
  layer_set_update_proc(s_pct_layer, pct_update_proc);
  layer_add_child(root, s_pct_layer);

  // Predicted days left — the FOOTER (user: keep the centered
  // battery as is, but move the days to the footer).
  s_days_layer = layer_create(GRect(0, h - h * 6 / 100 - 36, w, 36));
  layer_set_update_proc(s_days_layer, days_update_proc);
  layer_add_child(root, s_days_layer);

  sample_battery();
  refresh_display();
}

static void window_unload(Window *window) {
  layer_destroy(s_pct_layer);
  layer_destroy(s_days_layer);
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  int flags = family_night_apply(iter, MESSAGE_KEY_SUNRISE,
                                 MESSAGE_KEY_SUNSET, MESSAGE_KEY_NIGHT,
                                 PERSIST_SUNRISE, PERSIST_SUNSET,
                                 PERSIST_NIGHT);
  if (flags & 2) refresh_display();
}

static void request_night(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_uint8(iter, MESSAGE_KEY_REQUEST, 1);
    app_message_outbox_send();
  }
}

static void init(void) {
  // Load the persisted night state FIRST, before the window
  // exists: loading it after the push (the v2.0.0 bug) painted
  // the first frame light and only flipped dark when the phone
  // replied.
  family_night_load(PERSIST_NIGHT, PERSIST_SUNRISE, PERSIST_SUNSET);
  s_f68 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_68));
  s_f48 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_48));
  s_f56 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_56));
  s_f40 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_40));
  s_p34 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_PCT_34));
  s_p28 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_PCT_28));
  s_p24 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_PCT_24));
  s_p20 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_PCT_20));
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .appear = window_appear,
    .unload = window_unload,
  });
  window_set_click_config_provider(s_window, click_config_provider);
  window_stack_push(s_window, true);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);
  app_message_register_inbox_received(inbox_received);
  app_message_open(256, 256);
  request_night();
}

static void deinit(void) {
  window_destroy(s_window);
  fonts_unload_custom_font(s_f68);
  fonts_unload_custom_font(s_f48);
  fonts_unload_custom_font(s_f56);
  fonts_unload_custom_font(s_f40);
  fonts_unload_custom_font(s_p34);
  fonts_unload_custom_font(s_p28);
  fonts_unload_custom_font(s_p24);
  fonts_unload_custom_font(s_p20);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
