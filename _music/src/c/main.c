// _music — Spotify controller. White background, thick-framed album art
// centered both ways, song name as a tight caption below the art, button
// bumps flushed to the right edge. When nothing is playing, the art layer
// hides and a grey Spotify logo (disc + knocked-out arcs) shows instead.
// Buttons: Select = play/pause, Up = previous track, Down = next track.
// Back is left at the system default (pops the app).
// The art arrives from the phone in 1024-byte chunks and is drawn progressively,
// top to bottom, as chunks land. No idle exit: a controller stays up while listening.
// family.h is included ONLY for the shared button slide (the Up/Down
// press feedback). No wobble, no earmarks, no night engine: art must not
// wobble, and earmarks would paint over pixels. Including the header adds
// no behavior by itself — everything in it is opt-in.
#include <pebble.h>
#include "../../shared/family.h"

#define ART_SIZE 144
#define ART_CHUNK 1024
#define ART_BYTES (ART_SIZE * ART_SIZE)
#define FRAME 4
#define FOOTER_H 28
#define FOOTER_GAP 4
#define TRACK_MAX 127
#define ICON_W 14
#define ICON_H 22

static Window *s_window;
static BitmapLayer *s_art_layer;
static TextLayer *s_track_layer;
static Layer *s_chrome_layer;
static GBitmap *s_bmp;
static uint8_t *s_art_data;  // owned by s_bmp (from gbitmap_create_blank)
static int s_art_total_chunks = 0;
static int s_art_got_chunks = 0;
static char s_track[TRACK_MAX + 1];
static bool s_playing = false;
static bool s_art_seen = false;
static GRect s_art_rect;
static bool s_show_footer = false;

static void send_key(uint32_t key) {
  DictionaryIterator *iter = NULL;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_uint8(iter, key, 1);
  app_message_outbox_send();
}

// Tap pulse: the Select mark grows to 1.5x mid-pulse and returns to normal
// over 350 ms. One Animation object, recreated per tap. Up/Down use the
// shared family slide instead (see family_slide_start); only Select runs
// this pulse.
static Animation *s_button_anim = NULL;
static int s_anim_button = 0;  // 0 = none, 2 = select pulsing
static int s_anim_scale_x10 = 10;

static void button_anim_update(Animation *animation, AnimationProgress progress) {
  // progress 0..ANIMATION_NORMALIZED_MAX maps to a 0..180° sine bump.
  int32_t theta = (progress * (TRIG_MAX_ANGLE / 2)) / ANIMATION_NORMALIZED_MAX;
  int32_t sine = sin_lookup(theta);  // 0..TRIG_MAX_RATIO across 0..180°
  s_anim_scale_x10 = 10 + (5 * sine) / TRIG_MAX_RATIO;
  if (s_chrome_layer) layer_mark_dirty(s_chrome_layer);
}

static void button_anim_stopped(Animation *animation, bool finished, void *context) {
  s_anim_button = 0;
  s_anim_scale_x10 = 10;
  if (s_chrome_layer) layer_mark_dirty(s_chrome_layer);
}

// The AnimationImplementation must be file-scope static const: the animation
// keeps the pointer.
static const AnimationImplementation s_button_anim_impl = {
  .update = button_anim_update,
};
static const AnimationHandlers s_button_anim_handlers = {
  .stopped = button_anim_stopped,
};

static void start_button_anim(int button) {
  if (s_button_anim) {
    animation_unschedule(s_button_anim);
    animation_destroy(s_button_anim);
    s_button_anim = NULL;
  }
  // Set after the old animation is gone: unschedule may fire its stopped
  // handler, which resets s_anim_button.
  s_anim_button = button;
  s_button_anim = animation_create();
  animation_set_duration(s_button_anim, 350);
  animation_set_implementation(s_button_anim, &s_button_anim_impl);
  animation_set_handlers(s_button_anim, s_button_anim_handlers, NULL);
  animation_schedule(s_button_anim);
}

static void select_click(ClickRecognizerRef recognizer, void *context) {
  start_button_anim(2);
  send_key(MESSAGE_KEY_PLAYPAUSE);
}

static void up_click(ClickRecognizerRef recognizer, void *context) {
  family_slide_start(1, s_chrome_layer, NULL);
  send_key(MESSAGE_KEY_PREV);
}

static void down_click(ClickRecognizerRef recognizer, void *context) {
  family_slide_start(3, s_chrome_layer, NULL);
  send_key(MESSAGE_KEY_NEXT);
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
}

// Integer square root (floor) for the half-circle arc points.
static int isqrt_int(int n) {
  int result = 0;
  int bit = 1 << 30;
  while (bit > n) bit >>= 2;
  while (bit != 0) {
    if (n >= result + bit) {
      n -= result + bit;
      result = (result >> 1) + bit;
    } else {
      result >>= 1;
    }
    bit >>= 2;
  }
  return result;
}

// Filled left semicircle with center (cx, cy) and radius r. Arc points are
// computed with the integer sqrt, so no trig-angle convention is involved;
// the polygon's closing edge is the vertical diameter. (graphics_fill_radial
// was tried first and drew nothing with inset_thickness 0, so this fan
// replaces it.)
static void draw_half_circle(GContext *ctx, int cx, int cy, int r) {
  GPoint points[64];  // 2r+1 vertices; r is 11 here, so 23
  int n = 0;
  for (int dy = -r; dy <= r; dy++) {
    points[n++] = GPoint(cx - isqrt_int(r * r - dy * dy), cy + dy);
  }
  GPathInfo info = { .num_points = n, .points = points };
  GPath *path = gpath_create(&info);
  gpath_draw_filled(ctx, path);
  gpath_destroy(path);
}

// Scale a rect around a center by scale_x10/10 (position and size).
static GRect scale_rect(GRect r, GPoint c, int scale_x10) {
  return GRect(c.x + (r.origin.x - c.x) * scale_x10 / 10,
               c.y + (r.origin.y - c.y) * scale_x10 / 10,
               r.size.w * scale_x10 / 10, r.size.h * scale_x10 / 10);
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

static void draw_spotify_logo(GContext *ctx) {
  int ccx = s_art_rect.origin.x + ART_SIZE / 2;
  int ccy = s_art_rect.origin.y + ART_SIZE / 2;
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  draw_logo_poly(ctx, s_logo_disc, ARRAY_LENGTH(s_logo_disc), ccx, ccy);
  graphics_context_set_fill_color(ctx, GColorWhite);
  draw_logo_poly(ctx, s_logo_arc_bottom, ARRAY_LENGTH(s_logo_arc_bottom), ccx, ccy);
  draw_logo_poly(ctx, s_logo_arc_mid, ARRAY_LENGTH(s_logo_arc_mid), ccx, ccy);
  draw_logo_poly(ctx, s_logo_arc_top, ARRAY_LENGTH(s_logo_arc_top), ccx, ccy);
}

// Middle button mark only, flushed to the very rightmost pixel column and
// aligned with the physical Select button at 50% of the screen height. The
// top/bottom symbols are gone (the buttons themselves still work). The mark
// shows the state from the phone's STATE: pause bars while playing, the half
// circle while paused — so the paused state is visible at a glance.
static void chrome_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_context_set_stroke_color(ctx, GColorBlack);
#ifdef PBL_COLOR
  if (s_art_seen) {
    for (int i = 0; i < FRAME; i++) {
      graphics_draw_rect(ctx, GRect(s_art_rect.origin.x - FRAME + i, s_art_rect.origin.y - FRAME + i,
                                    ART_SIZE + FRAME * 2 - i * 2, ART_SIZE + FRAME * 2 - i * 2));
    }
  }
#endif
  int ix = bounds.size.w - ICON_W;
  // rx is the screen edge itself: a polygon edge exactly at x=200 inks column
  // 199 (a vertex at 199 would leave that column empty under the pixel-square
  // fill rule), so the mark truly touches the rightmost pixel.
  int rx = ix + ICON_W;
  int sel_y = bounds.size.h / 2 - ICON_H / 2;
  // The pulse scales whichever middle mark is shown around the box center;
  // for the half circle the flat side stays on the screen edge.
  int sscale = (s_anim_button == 2) ? s_anim_scale_x10 : 10;
  if (s_playing) {
    GPoint mc = GPoint(ix + ICON_W / 2, sel_y + ICON_H / 2);
    graphics_fill_rect(ctx, scale_rect(GRect(ix + 1, sel_y, 5, ICON_H), mc, sscale), 0, GCornerNone);
    graphics_fill_rect(ctx, scale_rect(GRect(ix + 9, sel_y, 5, ICON_H), mc, sscale), 0, GCornerNone);
  } else {
    draw_half_circle(ctx, rx, sel_y + ICON_H / 2, (ICON_H / 2) * sscale / 10);
  }
  // Empty state (nothing playing, no art): the grey Spotify logo, centered
  // where the art goes. It never coexists with the frame, and the mark sits
  // outside the disc.
  if (!s_art_seen) draw_spotify_logo(ctx);
  // Up/Down press feedback (no icons): the shared family slide, drawn last
  // so it stays visible over the logo (it still sits under the art layer,
  // which is above chrome). The helper sets its own fill color: the logo
  // above leaves it white, which would make a bare rect invisible on the
  // white background.
  family_slide_draw(ctx, bounds, GColorBlack);
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *track_t = dict_find(iter, MESSAGE_KEY_TRACK);
  if (track_t) {
    const char *name = (track_t->length > 0) ? track_t->value->cstring : "";
    strncpy(s_track, name, TRACK_MAX);
    s_track[TRACK_MAX] = '\0';
    if (s_track_layer) layer_mark_dirty(text_layer_get_layer(s_track_layer));
  }
  Tuple *state_t = dict_find(iter, MESSAGE_KEY_STATE);
  if (state_t) {
    s_playing = (state_t->value->int32 != 0);
    if (s_chrome_layer) layer_mark_dirty(s_chrome_layer);
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
    if (w_t->value->int32 != ART_SIZE || h_t->value->int32 != ART_SIZE) return;
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
    return;
  }
  if (!w_t || !h_t || !idx_t || !total_t || !data_t) return;
  if (w_t->value->int32 != ART_SIZE || h_t->value->int32 != ART_SIZE) return;
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

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  window_set_background_color(window, GColorWhite);
  // The art itself is centered both ways; the caption hangs tight below the
  // frame only when it fits (emery yes, basalt no).
  int ax = (bounds.size.w - ART_SIZE) / 2;
  int ay = (bounds.size.h - ART_SIZE) / 2;
  int caption_top = ay + ART_SIZE + FRAME + FOOTER_GAP;
  s_show_footer = (caption_top + FOOTER_H <= bounds.size.h);
  s_art_rect = GRect(ax, ay, ART_SIZE, ART_SIZE);
  s_chrome_layer = layer_create(bounds);
  layer_set_update_proc(s_chrome_layer, chrome_update_proc);
  layer_add_child(root, s_chrome_layer);
#ifdef PBL_COLOR
  s_bmp = gbitmap_create_blank(GSize(ART_SIZE, ART_SIZE), GBitmapFormat8Bit);
  s_art_data = gbitmap_get_data(s_bmp);
  // 8-bit rows carry no stride restriction; the blank's rows are exactly ART_SIZE
  // bytes wide, so chunk bytes land row-major in order. If a platform ever padded
  // rows, the art would shear visibly — the emulator pin test proves it on emery.
  memset(s_art_data, 0xFF, ART_BYTES);
  s_art_layer = bitmap_layer_create(s_art_rect);
  bitmap_layer_set_bitmap(s_art_layer, s_bmp);
  // Hidden until art arrives: the empty state shows the logo underneath.
  layer_set_hidden(bitmap_layer_get_layer(s_art_layer), true);
  layer_add_child(root, bitmap_layer_get_layer(s_art_layer));
#endif
  if (s_show_footer) {
    s_track_layer = text_layer_create(
        GRect(0, caption_top, bounds.size.w, FOOTER_H));
    text_layer_set_font(s_track_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    text_layer_set_text_alignment(s_track_layer, GTextAlignmentCenter);
    text_layer_set_text_color(s_track_layer, GColorBlack);
    text_layer_set_background_color(s_track_layer, GColorClear);
    text_layer_set_text(s_track_layer, s_track);
    layer_add_child(root, text_layer_get_layer(s_track_layer));
  }
}

static void window_appear(Window *window) {
  send_key(MESSAGE_KEY_REQ);
}

static void window_unload(Window *window) {
  if (s_button_anim) {
    animation_unschedule(s_button_anim);
    animation_destroy(s_button_anim);
    s_button_anim = NULL;
  }
  family_slide_teardown();
  if (s_track_layer) text_layer_destroy(s_track_layer);
  layer_destroy(s_chrome_layer);
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
