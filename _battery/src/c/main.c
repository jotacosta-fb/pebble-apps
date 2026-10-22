#include <pebble.h>

// Battery Days — a deliberately tiny battery app.
// Shows exactly: current battery %, and a prediction of how many days
// are left in this charge. The prediction learns this watch's real
// drain: each time the app opens (and on every battery event while it
// is open) it compares % and time against the last sample, blends the
// observed %/day into a stored rate, and divides the current charge
// by that rate. Until a rate is learned it assumes 15%/day.

static Window *s_window;
static GFont s_f68, s_f48, s_f56, s_f40;  // LECO 1976 (custom font)
static GFont s_p34, s_p28, s_p24, s_p20;  // LECO 1976 %-only fonts at HALF the digit heights (user: full size was too large)
static Layer *s_pct_layer;
static Layer *s_days_layer;

static int s_pct = -1;         // -1 = no reading yet
static bool s_charging = false;
static int s_rate_x10 = 0;     // drain rate in % per day x10, 0 = not learned
static int s_days_x10 = -1;    // predicted days left x10

#define PERSIST_RATE_X10 1
#define PERSIST_LAST_PCT 2
#define PERSIST_LAST_TIME 3
#define PERSIST_NIGHT 4

// Night mode (user): same inversion as _weather — after sunset,
// black background and white ink. A small JS companion fetches
// sunrise/sunset from Open-Meteo and sends NIGHT.
static bool s_night = false;
static GColor ink_color(void) { return s_night ? GColorWhite : GColorBlack; }
static GColor bg_color(void) { return s_night ? GColorBlack : GColorWhite; }
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
  graphics_context_set_stroke_color(ctx, ink_color());
  graphics_context_set_stroke_width(ctx, sw);
  graphics_draw_rect(ctx, GRect(x, y, bw, bh));
  graphics_context_set_fill_color(ctx, ink_color());
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
                        ? (s_night ? GColorIslamicGreen : GColorMintGreen)
                        : (s_night ? GColorDarkGreen : GColorScreaminGreen)));
#else
    graphics_context_set_fill_color(ctx,
                                    s_night ? GColorDarkGray : GColorLightGray);
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

static void pct_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  bool big = b.size.w >= 180;
  graphics_context_set_text_color(ctx, ink_color());

  // Merged layout: the big battery is the background; digits on top.
  draw_battery_bg(ctx, b);

  if (s_pct < 0) {
    GFont f = s_f48;
    graphics_draw_text(ctx, "--", f, b, GTextOverflowModeFill,
                       GTextAlignmentCenter, NULL);
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
  int x = (b.size.w - total) / 2;
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

  int rate_x10 = persist_exists(PERSIST_RATE_X10)
      ? persist_read_int(PERSIST_RATE_X10) : 0;
  int now = (int)time(NULL);

  if (!s_charging && persist_exists(PERSIST_LAST_PCT) &&
      persist_exists(PERSIST_LAST_TIME)) {
    int last_pct = persist_read_int(PERSIST_LAST_PCT);
    int elapsed = now - persist_read_int(PERSIST_LAST_TIME);
    int drop = last_pct - s_pct;
    if (drop > 0 && elapsed >= 3600) {
      int observed_x10 = (int)((long)drop * 864000 / elapsed);
      if (observed_x10 >= 10 && observed_x10 <= 1000) {
        rate_x10 = rate_x10 ? (rate_x10 * 2 + observed_x10) / 3 : observed_x10;
        persist_write_int(PERSIST_RATE_X10, rate_x10);
      }
      persist_write_int(PERSIST_LAST_PCT, s_pct);
      persist_write_int(PERSIST_LAST_TIME, now);
    } else if (drop < 0 || elapsed >= 86400) {
      persist_write_int(PERSIST_LAST_PCT, s_pct);
      persist_write_int(PERSIST_LAST_TIME, now);
    }
  } else {
    persist_write_int(PERSIST_LAST_PCT, s_pct);
    persist_write_int(PERSIST_LAST_TIME, now);
  }

  s_rate_x10 = rate_x10;
  int eff_rate = rate_x10 > 0 ? rate_x10 : DEFAULT_RATE_X10;
  s_days_x10 = s_pct * 100 / eff_rate;
}

// Days line is drawn, not a text layer, so the fractional part of the
// float can be gray while the rest stays black: "5" + ".3" (gray) +
// " days left". No tilde — user asked for plain float with gray fraction.
static void days_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  bool big = b.size.w >= 180;
  GFont font = fonts_get_system_font(
      FONT_KEY_GOTHIC_28);

  if (s_charging || s_days_x10 < 0) {
    graphics_context_set_text_color(ctx, ink_color());
    graphics_draw_text(ctx, s_charging ? "charging" : "-- days", font, b,
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }

  char int_buf[8], frac_buf[8];
  const char *suffix = " days";  // "left" removed at user request
  snprintf(int_buf, sizeof(int_buf), "%d", s_days_x10 / 10);
  snprintf(frac_buf, sizeof(frac_buf), ".%d", s_days_x10 % 10);

  GRect measure = GRect(0, 0, 300, b.size.h);
  GSize i_sz = graphics_text_layout_get_content_size(
      int_buf, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
  GSize f_sz = graphics_text_layout_get_content_size(
      frac_buf, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
  GSize s_sz = graphics_text_layout_get_content_size(
      suffix, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);

  if (i_sz.w + f_sz.w + s_sz.w > b.size.w - 4) {
    // Long prediction: drop the header one size so nothing clips.
    font = fonts_get_system_font(
        FONT_KEY_GOTHIC_24);
    i_sz = graphics_text_layout_get_content_size(
        int_buf, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
    f_sz = graphics_text_layout_get_content_size(
        frac_buf, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
    s_sz = graphics_text_layout_get_content_size(
        suffix, font, measure, GTextOverflowModeFill, GTextAlignmentLeft);
  }

  int x = (b.size.w - (i_sz.w + f_sz.w + s_sz.w)) / 2;
  graphics_context_set_text_color(ctx, ink_color());
  graphics_draw_text(ctx, int_buf, font, GRect(x, 0, i_sz.w + 2, b.size.h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  x += i_sz.w;
  graphics_context_set_text_color(ctx,
                             s_night ? GColorLightGray : GColorDarkGray);
  graphics_draw_text(ctx, frac_buf, font, GRect(x, 0, f_sz.w + 2, b.size.h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  x += f_sz.w;
  graphics_context_set_text_color(ctx, ink_color());
  graphics_draw_text(ctx, suffix, font, GRect(x, 0, s_sz.w + 2, b.size.h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

static void refresh_display(void) {
  if (s_window) window_set_background_color(s_window, bg_color());
  layer_mark_dirty(s_days_layer);
  layer_mark_dirty(s_pct_layer);  // draws digits + battery background
  layer_mark_dirty(s_pct_layer);
}

static void battery_handler(BatteryChargeState state) {
  sample_battery();
  refresh_display();
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
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

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  int w = b.size.w;
  int h = b.size.h;
  window_set_background_color(window, bg_color());

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
  Tuple *t = dict_find(iter, MESSAGE_KEY_NIGHT);
  if (t) {
    s_night = t->value->int32 != 0;
    persist_write_int(PERSIST_NIGHT, s_night ? 1 : 0);
    refresh_display();
  }
}

static void request_night(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_uint8(iter, MESSAGE_KEY_REQUEST, 1);
    app_message_outbox_send();
  }
}

static void init(void) {
  // Load the persisted night flag FIRST, before the window exists:
  // loading it after the push (the v2.0.0 bug) painted the first
  // frame light and only flipped dark when the phone replied.
  // _weather loads its persists up front — that's why it never
  // flashed.
  if (persist_exists(PERSIST_NIGHT)) {
    s_night = persist_read_int(PERSIST_NIGHT) != 0;
  }
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
    .unload = window_unload,
  });
  window_set_click_config_provider(s_window, click_config_provider);
  window_stack_push(s_window, true);
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
