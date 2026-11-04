// _music — Spotify controller. Full-display album art (the whole screen),
// with the title as a footer overlay: a white box at the bottom with
// black Gothic 28 text, button bumps flushed to the right edge.
// When nothing is playing, the art layer
// hides and a grey Spotify logo (disc + knocked-out arcs) shows instead.
// Buttons: Select = play/pause, Up = previous track, Down = next track.
// Back is left at the system default (pops the app).
// The art arrives from the phone in 1024-byte chunks and is drawn progressively,
// top to bottom, as chunks land. No idle exit: a controller stays up while listening.
// family.h is included for the shared button slide (the Up/Down press
// feedback) and the night engine (day/night chrome: window background,
// slide and logo follow the on-watch sun times — the footer stays white
// with black text day and night).
// No wobble, no earmarks: art must not wobble, and earmarks would paint
// over pixels. Everything in the header is opt-in.
#include <pebble.h>
#include "../../shared/family.h"

#define ART_W 200
#define ART_H 228
#define ART_CHUNK 1024
#define ART_BYTES (ART_W * ART_H)
#define FOOTER_H 36
#define FOOTER_PAD_X 10
#define FOOTER_FADE_W 12
// Text layer top for the footer title, so the Gothic 28 ink centers in
// the 36-tall white box (box 192..228, center 210). Measured on the
// 1.1.30 pin: ink offset 9 from the layer top, ink 23 tall, so top 190
// puts ink at 199..221, center exactly 210.
#define FOOTER_TEXT_TOP 190
#define TRACK_MAX 127
#define PAUSE_BAR_W 18
#define PAUSE_BAR_H 78
#define PAUSE_BAR_GAP 18
#define PAUSE_BORDER 4
#define PKEY_LAT 1
#define PKEY_LON 2

static Window *s_window;
static BitmapLayer *s_art_layer;
static TextLayer *s_track_layer;
static Layer *s_chrome_layer;
static Layer *s_pause_layer;
static GBitmap *s_bmp;
static uint8_t *s_art_data;  // owned by s_bmp (from gbitmap_create_blank)
static int s_art_total_chunks = 0;
static int s_art_got_chunks = 0;
static char s_track[TRACK_MAX + 1];
static bool s_playing = false;
static bool s_art_seen = false;
static GRect s_art_rect;
static Layer *s_footer_layer;
static GRect s_footer_box;
static void footer_layout(void);

static void send_key(uint32_t key) {
  DictionaryIterator *iter = NULL;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_uint8(iter, key, 1);
  app_message_outbox_send();
}

static void select_click(ClickRecognizerRef recognizer, void *context) {
  // All three buttons animate the same way now: the shared family slide
  // at the pressed button's row — but only when art is showing. In the
  // empty state (Spotify logo, nothing playing) the buttons stay still;
  // the control message still goes out either way.
  if (s_art_seen) family_slide_start(2, s_chrome_layer, NULL);
  send_key(MESSAGE_KEY_PLAYPAUSE);
}

static void up_click(ClickRecognizerRef recognizer, void *context) {
  if (s_art_seen) family_slide_start(1, s_chrome_layer, NULL);
  send_key(MESSAGE_KEY_PREV);
}

static void down_click(ClickRecognizerRef recognizer, void *context) {
  if (s_art_seen) family_slide_start(3, s_chrome_layer, NULL);
  send_key(MESSAGE_KEY_NEXT);
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
}

// Official Spotify logo outline (Simple Icons spotify.svg, 24x24 viewBox,
// beziers flattened at 20 segments each, scaled 24 -> 100 px, origin at
// the disc center). Drawn as grey disc + white arcs (the single-colour
// logo treatment: arcs knocked out to the background).
static const GPoint s_logo_disc[] = { {0, -50}, {-4, -50}, {-8, -49}, {-12, -49}, {-16, -47}, {-19, -46}, {-23, -44}, {-26, -42}, {-29, -40}, {-32, -38}, {-35, -35}, {-38, -32}, {-40, -29}, {-42, -26}, {-44, -23}, {-46, -19}, {-47, -16}, {-49, -12}, {-49, -8}, {-50, -4}, {-50, 0}, {-50, 4}, {-49, 8}, {-49, 12}, {-47, 16}, {-46, 19}, {-44, 23}, {-42, 26}, {-40, 29}, {-38, 32}, {-35, 35}, {-32, 38}, {-29, 40}, {-26, 42}, {-23, 44}, {-19, 46}, {-16, 47}, {-12, 49}, {-8, 49}, {-4, 50}, {0, 50}, {4, 50}, {8, 49}, {12, 49}, {16, 47}, {19, 46}, {23, 44}, {26, 42}, {29, 40}, {32, 38}, {35, 35}, {38, 32}, {40, 29}, {42, 26}, {44, 23}, {46, 19}, {47, 16}, {49, 12}, {49, 8}, {50, 4}, {50, 0}, {50, -4}, {49, -8}, {49, -12}, {47, -16}, {46, -19}, {44, -23}, {43, -26}, {40, -29}, {38, -32}, {35, -35}, {33, -38}, {30, -40}, {26, -42}, {23, -44}, {20, -46}, {16, -47}, {12, -49}, {8, -49}, {4, -50}, {0, -50} };
static const GPoint s_logo_arc_bottom[] = { {23, 22}, {23, 23}, {22, 23}, {22, 24}, {21, 24}, {20, 24}, {19, 24}, {19, 23}, {17, 22}, {15, 21}, {13, 20}, {11, 20}, {9, 19}, {7, 18}, {5, 18}, {3, 17}, {1, 17}, {-1, 17}, {-3, 16}, {-6, 16}, {-8, 16}, {-10, 16}, {-13, 17}, {-15, 17}, {-18, 17}, {-20, 17}, {-23, 18}, {-25, 18}, {-26, 19}, {-27, 19}, {-27, 18}, {-28, 18}, {-28, 17}, {-29, 17}, {-29, 16}, {-29, 15}, {-29, 14}, {-28, 14}, {-28, 13}, {-27, 13}, {-27, 12}, {-24, 12}, {-21, 11}, {-18, 11}, {-16, 11}, {-13, 10}, {-10, 10}, {-8, 10}, {-5, 10}, {-3, 10}, {0, 11}, {2, 11}, {5, 11}, {7, 12}, {9, 12}, {11, 13}, {14, 14}, {16, 15}, {18, 16}, {20, 17}, {22, 18}, {23, 19}, {23, 20}, {23, 21}, {23, 22} };
static const GPoint s_logo_arc_mid[] = { {29, 8}, {29, 9}, {28, 9}, {28, 10}, {27, 10}, {26, 10}, {25, 10}, {24, 10}, {22, 9}, {19, 7}, {17, 6}, {15, 6}, {12, 5}, {10, 4}, {8, 3}, {5, 3}, {2, 2}, {0, 2}, {-3, 2}, {-6, 1}, {-8, 1}, {-11, 1}, {-13, 2}, {-16, 2}, {-19, 2}, {-21, 3}, {-24, 3}, {-26, 4}, {-27, 4}, {-28, 4}, {-29, 4}, {-29, 3}, {-30, 3}, {-30, 2}, {-31, 2}, {-31, 1}, {-31, 0}, {-31, -1}, {-30, -1}, {-30, -2}, {-30, -3}, {-29, -3}, {-28, -3}, {-25, -4}, {-23, -5}, {-20, -5}, {-17, -6}, {-14, -6}, {-11, -6}, {-8, -6}, {-5, -6}, {-2, -6}, {1, -5}, {4, -5}, {7, -5}, {10, -4}, {12, -3}, {15, -2}, {18, -1}, {21, 0}, {23, 1}, {26, 2}, {28, 3}, {28, 4}, {29, 4}, {29, 5}, {30, 6}, {30, 7}, {29, 8} };
static const GPoint s_logo_arc_top[] = { {30, -6}, {27, -7}, {24, -8}, {22, -9}, {19, -10}, {16, -11}, {13, -12}, {10, -12}, {7, -13}, {3, -13}, {0, -14}, {-3, -14}, {-6, -14}, {-9, -14}, {-12, -14}, {-15, -14}, {-18, -13}, {-21, -13}, {-24, -12}, {-26, -12}, {-28, -11}, {-29, -11}, {-30, -11}, {-31, -11}, {-32, -11}, {-32, -12}, {-33, -12}, {-33, -13}, {-34, -13}, {-34, -14}, {-34, -15}, {-34, -16}, {-34, -17}, {-34, -18}, {-33, -18}, {-33, -19}, {-32, -19}, {-32, -20}, {-31, -20}, {-29, -21}, {-26, -21}, {-23, -22}, {-19, -22}, {-16, -23}, {-13, -23}, {-9, -23}, {-6, -23}, {-2, -23}, {1, -23}, {5, -22}, {8, -22}, {12, -21}, {15, -21}, {19, -20}, {22, -19}, {25, -18}, {28, -16}, {31, -15}, {34, -13}, {35, -13}, {35, -12}, {36, -12}, {36, -11}, {36, -10}, {37, -10}, {37, -9}, {37, -8}, {36, -8}, {36, -7}, {36, -6}, {35, -6}, {34, -5}, {33, -5}, {32, -5}, {31, -5}, {30, -5}, {30, -6} };

static void draw_logo_poly(GContext *ctx, const GPoint *poly, int count, int ccx, int ccy) {
  GPoint points[160];
  for (int i = 0; i < count; i++) {
    points[i] = GPoint(ccx + poly[i].x, ccy + poly[i].y);
  }
  GPathInfo info = { .num_points = count, .points = points };
  GPath *path = gpath_create(&info);
  gpath_draw_filled(ctx, path);
  gpath_destroy(path);
}

static void draw_spotify_logo(GContext *ctx, GRect bounds) {
  // Centered on the screen: the art fills it, so art center = screen center.
  int ccx = bounds.size.w / 2;
  int ccy = bounds.size.h / 2;
  // Knocked out to the background: disc grey, arcs in the background
  // color, so the logo reads on white by day and black by night.
  graphics_context_set_fill_color(ctx, family_night ? GColorLightGray : GColorDarkGray);
  draw_logo_poly(ctx, s_logo_disc, ARRAY_LENGTH(s_logo_disc), ccx, ccy);
  graphics_context_set_fill_color(ctx, family_bg_color());
  draw_logo_poly(ctx, s_logo_arc_bottom, ARRAY_LENGTH(s_logo_arc_bottom), ccx, ccy);
  draw_logo_poly(ctx, s_logo_arc_mid, ARRAY_LENGTH(s_logo_arc_mid), ccx, ccy);
  draw_logo_poly(ctx, s_logo_arc_top, ARRAY_LENGTH(s_logo_arc_top), ccx, ccy);
}

// Chrome: the empty-state logo plus the shared slide. No frame anymore:
// the art bleeds to the edges. Chrome sits ABOVE the art layer, so the
// slide draws over the artwork (it used to sit under it, which was fine
// while the art was inset — full-bleed art would have hidden it).
// No middle mark anymore: all three buttons animate with the slide, and
// the paused state shows as black pause bars over the art (see
// pause_update_proc, topmost layer).
static void chrome_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, family_ink_color());
  graphics_context_set_stroke_color(ctx, family_ink_color());
  // Empty state (nothing playing, no art): the grey Spotify logo, centered
  // on the visible art area.
  if (!s_art_seen) draw_spotify_logo(ctx, bounds);
  // Button press feedback: the shared family slide, drawn last so it
  // stays visible over the logo and the art. The helper sets its own fill
  // color: the logo above leaves it white, which would make a bare rect
  // invisible on the white background.
  family_slide_draw(ctx, bounds, family_ink_color());
}

// Paused overlay: when the track is paused, bigger black pause bars
// centered over the album art, on a white plaque (the border) so they
// read on any artwork, on a topmost transparent layer (above the art).
// Nothing shows while playing, or when no art is displayed.
static void pause_update_proc(Layer *layer, GContext *ctx) {
  if (!s_art_seen || s_playing) return;
  GRect bounds = layer_get_bounds(layer);
  int cx = bounds.size.w / 2;
  int cy = bounds.size.h / 2;
  int half_w = PAUSE_BAR_W + PAUSE_BAR_GAP / 2;
  int half_h = PAUSE_BAR_H / 2;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(cx - half_w - PAUSE_BORDER, cy - half_h - PAUSE_BORDER,
                                (half_w + PAUSE_BORDER) * 2, (half_h + PAUSE_BORDER) * 2),
                     0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(cx - half_w, cy - half_h,
                                PAUSE_BAR_W, PAUSE_BAR_H), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(cx + PAUSE_BAR_GAP / 2, cy - half_h,
                                PAUSE_BAR_W, PAUSE_BAR_H), 0, GCornerNone);
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  // Location for the night engine: persisted on the watch, so one good
  // fix keeps day/night correct without the phone. A flip repaints the
  // chrome at once.
  if (family_place_apply(iter, MESSAGE_KEY_LAT, MESSAGE_KEY_LON, PKEY_LAT, PKEY_LON)) {
    window_set_background_color(s_window, family_bg_color());
    if (s_chrome_layer) layer_mark_dirty(s_chrome_layer);
    if (s_pause_layer) layer_mark_dirty(s_pause_layer);
  }
  Tuple *track_t = dict_find(iter, MESSAGE_KEY_TRACK);
  if (track_t) {
    const char *name = (track_t->length > 0) ? track_t->value->cstring : "";
    strncpy(s_track, name, TRACK_MAX);
    s_track[TRACK_MAX] = '\0';
    footer_layout();
  }
  Tuple *state_t = dict_find(iter, MESSAGE_KEY_STATE);
  if (state_t) {
    s_playing = (state_t->value->int32 != 0);
    if (s_chrome_layer) layer_mark_dirty(s_chrome_layer);
    if (s_pause_layer) layer_mark_dirty(s_pause_layer);
  }
  Tuple *w_t = dict_find(iter, MESSAGE_KEY_ART_W);
  Tuple *h_t = dict_find(iter, MESSAGE_KEY_ART_H);
  Tuple *idx_t = dict_find(iter, MESSAGE_KEY_ART_IDX);
  Tuple *total_t = dict_find(iter, MESSAGE_KEY_ART_TOTAL);
  Tuple *data_t = dict_find(iter, MESSAGE_KEY_ART_DATA);
  Tuple *start_t = dict_find(iter, MESSAGE_KEY_ART_START);
  if (start_t && w_t && h_t && total_t) {
    // New art starting: white buffer and frame on now; the chunks that follow
    // land in random order, so the reset lives here, NOT on idx 0.
    if (w_t->value->int32 != ART_W || h_t->value->int32 != ART_H) return;
    if (!s_art_data) return;
    uint32_t total = total_t->value->uint32;
    if (total == 0) return;
    memset(s_art_data, 0xFF, ART_BYTES);
    s_art_total_chunks = (int)total;
    s_art_got_chunks = 0;
    s_art_seen = true;
    layer_set_hidden(bitmap_layer_get_layer(s_art_layer), false);
    layer_mark_dirty(bitmap_layer_get_layer(s_art_layer));
    if (s_chrome_layer) layer_mark_dirty(s_chrome_layer);
    if (s_pause_layer) layer_mark_dirty(s_pause_layer);
    return;
  }
  if (!w_t || !h_t || !idx_t || !total_t || !data_t) return;
  if (w_t->value->int32 != ART_W || h_t->value->int32 != ART_H) return;
  if (!s_art_data) return;

  uint32_t idx = idx_t->value->uint32;
  uint32_t total = total_t->value->uint32;
  if (total == 0) {
    // Clear signal (nothing playing): hide the art layer so the grey logo
    // on the chrome layer shows through; no stale art, no frame.
    memset(s_art_data, 0xFF, ART_BYTES);
    s_art_total_chunks = 0;
    s_art_got_chunks = 0;
    s_art_seen = false;
    layer_set_hidden(bitmap_layer_get_layer(s_art_layer), true);
    layer_mark_dirty(bitmap_layer_get_layer(s_art_layer));
    if (s_chrome_layer) layer_mark_dirty(s_chrome_layer);
    if (s_pause_layer) layer_mark_dirty(s_pause_layer);
    return;
  }
  // No idx==0 reset anymore: chunks arrive shuffled, so a mid-stream idx 0
  // must NOT wipe the bands already received.
  if ((int)idx >= s_art_total_chunks) return;
  int offset = (int)idx * ART_CHUNK;
  int len = (int)data_t->length;
  if (offset + len > ART_BYTES) len = ART_BYTES - offset;
  if (len <= 0) return;
  memcpy(s_art_data + offset, data_t->value->data, len);
  s_art_got_chunks++;
  layer_mark_dirty(bitmap_layer_get_layer(s_art_layer));
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  // A night flip repaints the chrome: background and slide. The footer
  // stays white with black text, so it needs no repaint.
  if (family_night_tick()) {
    window_set_background_color(s_window, family_bg_color());
    if (s_chrome_layer) layer_mark_dirty(s_chrome_layer);
    if (s_pause_layer) layer_mark_dirty(s_pause_layer);
  }
}

// Position hash for the torn fade: deterministic, so the edge looks
// identical on every redraw (a re-randomized edge would shimmer), and
// it never touches rand(), so the slide's own per-press randomness is
// undisturbed.
static uint32_t footer_hash(int x, int y) {
  uint32_t h = ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u);
  h ^= h >> 16;
  return h;
}

// Footer box + torn fade, solid white always (day and night), under the
// title text. The box ends are not straight cuts: past each end, white
// pixels scatter into the art with a density that falls with distance,
// so the background dissolves like a tear. The footer layer spans the
// full width, so the fade columns (outside the box) are inside its
// bounds; coordinates here are layer-local, and the layer sits at x 0,
// so local x equals screen x and the box's local y is 0.
static void footer_update_proc(Layer *layer, GContext *ctx) {
  GRect lb = layer_get_bounds(layer);
  GRect box = GRect(s_footer_box.origin.x, 0, s_footer_box.size.w, s_footer_box.size.h);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, box, 0, GCornerNone);
  for (int side = 0; side < 2; side++) {
    for (int d = 1; d <= FOOTER_FADE_W; d++) {
      int x = (side == 0) ? box.origin.x - d : box.origin.x + box.size.w - 1 + d;
      if (x < 0 || x >= lb.size.w) continue;
      // Near-solid at the box, sparse at the outer edge.
      int threshold = (FOOTER_FADE_W - d + 1) * 100 / (FOOTER_FADE_W + 1);
      // Batch contiguous white runs in this column into single rects.
      int run_y0 = -1;
      for (int y = 0; y < box.size.h; y++) {
        int on = ((int)(footer_hash(x, y) % 100)) < threshold;
        if (on && run_y0 < 0) run_y0 = y;
        if (!on && run_y0 >= 0) {
          graphics_fill_rect(ctx, GRect(x, run_y0, 1, y - run_y0), 0, GCornerNone);
          run_y0 = -1;
        }
      }
      if (run_y0 >= 0) {
        graphics_fill_rect(ctx, GRect(x, run_y0, 1, box.size.h - run_y0), 0, GCornerNone);
      }
    }
  }
}

// The footer box shrink-wraps the title: a short title gets a compact
// white box centered over the art (text width + padding each side), not
// a bar end to end. A title that fills the width gets the full-width
// box, and an empty title hides the box entirely. The text layer keeps
// its tuned top (FOOTER_TEXT_TOP) so the ink stays centered in the box.
static void footer_layout(void) {
  if (!s_footer_layer || !s_track_layer) return;
  GRect bounds = layer_get_bounds(window_get_root_layer(s_window));
  int w = bounds.size.w;
  int box_y = bounds.size.h - FOOTER_H;
  if (s_track[0] == '\0') {
    layer_set_hidden(s_footer_layer, true);
    return;
  }
  layer_set_hidden(s_footer_layer, false);
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  GSize ts = graphics_text_layout_get_content_size(s_track, font,
      GRect(0, 0, w, FOOTER_H), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter);
  int box_w = ts.w + FOOTER_PAD_X * 2;
  if (box_w > w) box_w = w;
  int box_x = (w - box_w) / 2;
  // The footer layer spans the full width (set once in window_load); the
  // box itself lives in s_footer_box, which the update proc draws along
  // with the fade zones past its ends.
  s_footer_box = GRect(box_x, box_y, box_w, FOOTER_H);
  layer_set_frame(text_layer_get_layer(s_track_layer),
                  GRect(box_x, FOOTER_TEXT_TOP, box_w, FOOTER_H));
  layer_mark_dirty(s_footer_layer);
  layer_mark_dirty(text_layer_get_layer(s_track_layer));
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  // Night state before the first frame: restored coordinates (if any)
  // already decide day vs night here.
  family_place_load(PKEY_LAT, PKEY_LON);
  window_set_background_color(window, family_bg_color());
  // Full-display art, centered on every screen: exactly the screen on
  // emery, center-cropped on basalt. The title is a footer overlay, not
  // a caption below the art anymore.
  s_art_rect = GRect((bounds.size.w - ART_W) / 2, (bounds.size.h - ART_H) / 2,
                     ART_W, ART_H);
  s_chrome_layer = layer_create(bounds);
  layer_set_update_proc(s_chrome_layer, chrome_update_proc);
  // Chrome is added AFTER the art layer below, so it sits above the art
  // and the slide draws over the artwork (see chrome_update_proc).
#ifdef PBL_COLOR
  s_bmp = gbitmap_create_blank(GSize(ART_W, ART_H), GBitmapFormat8Bit);
  s_art_data = gbitmap_get_data(s_bmp);
  // 8-bit rows carry no stride restriction; the blank's rows are exactly ART_W
  // bytes wide, so chunk bytes land row-major in order. If a platform ever padded
  // rows, the art would shear visibly — the emulator pin test proves it on emery.
  memset(s_art_data, 0xFF, ART_BYTES);
  s_art_layer = bitmap_layer_create(s_art_rect);
  bitmap_layer_set_bitmap(s_art_layer, s_bmp);
  // Hidden until art arrives: the empty state shows the logo underneath.
  layer_set_hidden(bitmap_layer_get_layer(s_art_layer), true);
  layer_add_child(root, bitmap_layer_get_layer(s_art_layer));
#endif
  // Footer white box + torn fade, over the art. The layer spans the full
  // width; the box rect itself is set by footer_layout().
  s_footer_layer = layer_create(
      GRect(0, bounds.size.h - FOOTER_H, bounds.size.w, FOOTER_H));
  layer_set_update_proc(s_footer_layer, footer_update_proc);
  layer_add_child(root, s_footer_layer);
  // Title text, transparent over the white box, top set so the ink
  // centers in the box (FOOTER_TEXT_TOP). Back to Gothic 28 Bold — the
  // Roboto 30 step is over. Black on white, day and night.
  s_track_layer = text_layer_create(
      GRect(0, FOOTER_TEXT_TOP, bounds.size.w, FOOTER_H));
  text_layer_set_font(s_track_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(s_track_layer, GTextAlignmentCenter);
  text_layer_set_text_color(s_track_layer, GColorBlack);
  text_layer_set_background_color(s_track_layer, GColorClear);
  text_layer_set_text(s_track_layer, s_track);
  layer_add_child(root, text_layer_get_layer(s_track_layer));
  footer_layout();
  // Chrome above the footer, so the slide draws over the white box too.
  layer_add_child(root, s_chrome_layer);
  // Paused overlay, topmost: transparent, draws only when paused on art.
  s_pause_layer = layer_create(bounds);
  layer_set_update_proc(s_pause_layer, pause_update_proc);
  layer_add_child(root, s_pause_layer);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void window_appear(Window *window) {
  send_key(MESSAGE_KEY_REQ);
}

static void window_unload(Window *window) {
  tick_timer_service_unsubscribe();
  family_slide_teardown();
  if (s_track_layer) text_layer_destroy(s_track_layer);
  layer_destroy(s_footer_layer);
  layer_destroy(s_chrome_layer);
  if (s_pause_layer) layer_destroy(s_pause_layer);
#ifdef PBL_COLOR
  bitmap_layer_destroy(s_art_layer);
  gbitmap_destroy(s_bmp);
  s_art_data = NULL;
#endif
}

static void init(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .appear = window_appear,
    .unload = window_unload,
  });
  window_set_click_config_provider(s_window, click_config);
  app_message_register_inbox_received(inbox_received);
  // Inbox must fit one 1024-byte art chunk plus dict overhead, and a track name.
  app_message_open(2048, 256);
  window_stack_push(s_window, true);
}

static void deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
