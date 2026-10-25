#include <pebble.h>
#include <math.h>

// Family overrides (shared/family.h): the wobble runs longer
// and wider here than the family norm (user-tuned).
#define FAMILY_WOBBLE_LENGTH_MS 600
#define FAMILY_WOBBLE_AMP 12
#include "../../shared/family.h"

// MinMax Weather — a deliberately tiny weather app.
// Shows exactly: today's min-max temperature, chance of precipitation,
// and a condition icon. Data comes from the PebbleKit JS companion
// (Open-Meteo, no API key).

static Window *s_window;


static GFont s_f64, s_f46, s_f56, s_f40;  // LECO 1976 (custom font)
static GFont s_f50, s_f48;
static GFont s_f62, s_f54;  // (the -2 chain, no longer picked)
static GFont s_f60, s_f52, s_f44, s_f42, s_f38, s_f36;  // the min's chain
#if defined(PBL_PLATFORM_EMERY)
static GFont s_f80, s_f72;  // emery-only resources (glyph cap)
static GFont s_f78, s_f70;  // (unused since the -4 step, kept loaded-free)
static GFont s_f76, s_f68;  // the min's -4 chain, emery tiers
#endif

static int font_height_of(GFont f) {
#if defined(PBL_PLATFORM_EMERY)
  if (f == s_f80) return 80;
  if (f == s_f78) return 78;
  if (f == s_f72) return 72;
  if (f == s_f70) return 70;
  if (f == s_f76) return 76;
  if (f == s_f68) return 68;
#endif
  if (f == s_f62) return 62;
  if (f == s_f60) return 60;
  if (f == s_f54) return 54;
  if (f == s_f52) return 52;
  if (f == s_f42) return 42;
  if (f == s_f44) return 44;
  if (f == s_f38) return 38;
  if (f == s_f64) return 64;
  if (f == s_f56) return 56;
  if (f == s_f50) return 50;
  if (f == s_f48) return 48;
  if (f == s_f46) return 46;
  return 40;
}
static Layer *s_temps_layer;
static Layer *s_header_layer;
static Layer *s_icon_layer;

// Weather state
static int s_temp_max = 0;
static int s_temp_min = 0;
static int s_precip = -1;      // -1 = no data yet
static int s_code = -1;        // WMO weather code, -1 = no data yet
static bool s_have_data = false;

#define PERSIST_TEMP_MAX 1
#define PERSIST_TEMP_MIN 2
#define PERSIST_PRECIP 3
#define PERSIST_CODE 5
#define PERSIST_NIGHT 6
#define PERSIST_SUNRISE 7
#define PERSIST_SUNSET 8

// Night mode is the family engine (shared/family.h): the JS
// sends sunrise/sunset epochs with each fetch; the watch stores
// them and re-evaluates day/night on the minute tick.
static void refresh_display(void);

static void tick_handler(struct tm *tick_time, TimeUnits units) {
  if (family_night_tick()) refresh_display();
}

// ---------- Icon drawing ----------

// Icon geometry is authored for a ~78px-tall layer and scaled to fit.
static int s_k = 100;
#define SC(v) ((v) * s_k / 100)

static void draw_cloud(GContext *ctx, GPoint c) {
  // Cloud outline built by stroking the bumps, then erasing the interior
  // strokes with white fills, leaving only the outer outline.
  GPoint a = GPoint(c.x - SC(19), c.y + SC(6));
  GPoint b = GPoint(c.x + SC(4), c.y - SC(7));
  GPoint d = GPoint(c.x + SC(24), c.y + SC(7));
  int ra = SC(16), rb = SC(22), rd = SC(13);
  int sw = s_k >= 85 ? 3 : 2;

  graphics_context_set_stroke_color(ctx, family_ink_color());
  graphics_context_set_stroke_width(ctx, sw);
  graphics_draw_circle(ctx, a, ra);
  graphics_draw_circle(ctx, b, rb);
  graphics_draw_circle(ctx, d, rd);

  // Interior: light gray (user: clouds grey inside). The r-2 fills
  // both erase the inner bump strokes and lay the gray in.
  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_fill_circle(ctx, a, ra - 2);
  graphics_fill_circle(ctx, b, rb - 2);
  graphics_fill_circle(ctx, d, rd - 2);
  // Flatten the base in the same gray — but only across the body
  // (±34, where the side lines run)...
  graphics_fill_rect(ctx, GRect(c.x - SC(34), c.y - SC(1), SC(68), SC(22)), 0, GCornerNone);
  // ...while the strips outside the sides stay background-white so
  // the outermost bump arcs (left -35, right +37) don't bulge out.
  graphics_context_set_fill_color(ctx, family_bg_color());
  graphics_fill_rect(ctx, GRect(c.x - SC(41), c.y - SC(1), SC(9), SC(22)), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(c.x + SC(32), c.y - SC(1), SC(9), SC(22)), 0, GCornerNone);

  graphics_context_set_stroke_color(ctx, family_ink_color());
  graphics_context_set_stroke_width(ctx, sw);
  graphics_draw_line(ctx, GPoint(c.x - SC(34), c.y + SC(21)), GPoint(c.x + SC(34), c.y + SC(21)));
  // Sides run all the way up to the band's top edge, where the bump
  // arcs exit it — so the outline is one continuous line (they used
  // to start ~11px lower, leaving a gap on each side).
  graphics_draw_line(ctx, GPoint(c.x - SC(34), c.y - SC(1)), GPoint(c.x - SC(34), c.y + SC(21)));
  graphics_draw_line(ctx, GPoint(c.x + SC(34), c.y - SC(1)), GPoint(c.x + SC(34), c.y + SC(21)));
}

static void draw_sun(GContext *ctx, GPoint c, int r) {
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite));
  graphics_fill_circle(ctx, c, r);
  graphics_context_set_stroke_color(ctx, family_ink_color());
  graphics_context_set_stroke_width(ctx, 3);
  graphics_draw_circle(ctx, c, r);
  for (int i = 0; i < 8; i++) {
    int32_t ang = DEG_TO_TRIGANGLE(i * 45);
    GPoint p1 = gpoint_from_polar(GRect(c.x - r - 4, c.y - r - 4, (r + 4) * 2, (r + 4) * 2),
                                  GOvalScaleModeFitCircle, ang);
    GPoint p2 = gpoint_from_polar(GRect(c.x - r - 10, c.y - r - 10, (r + 10) * 2, (r + 10) * 2),
                                  GOvalScaleModeFitCircle, ang);
    graphics_draw_line(ctx, p1, p2);
  }
}

static void icon_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  // Center sits where it always did on screen: the taller frame
  // (see window_load) shifts the local center down by the extra
  // headroom, so b.size.h/2 lands on the old -4 position.
  // Art rides 6px high in its layer (the layer has 12px of
  // headroom): the laddered max now reaches up toward the icon
  // and the cloud's base must clear its top.
  GPoint c = GPoint(b.size.w / 2, b.size.h / 2 - 6);
  s_k = (b.size.h - 12) * 65 / 78;  // scale off the original height, x0.65 (user: icon still too big at x0.85)
  if (s_k < 40) s_k = 40;
  if (s_k > 110) s_k = 110;

  if (s_code < 0) {
    draw_cloud(ctx, c);
    return;
  }

  if (s_code == 0) {
    draw_sun(ctx, c, SC(20));
  } else if (s_code == 1 || s_code == 2) {
    draw_sun(ctx, GPoint(c.x - SC(26), c.y - SC(18)), SC(14));
    draw_cloud(ctx, GPoint(c.x + SC(5), c.y + SC(8)));
  } else if (s_code == 3) {
    draw_cloud(ctx, c);
  } else if (s_code == 45 || s_code == 48) {
    draw_cloud(ctx, GPoint(c.x, c.y - SC(10)));
    graphics_context_set_stroke_color(ctx, family_ink_color());
    graphics_context_set_stroke_width(ctx, 3);
    for (int i = 0; i < 3; i++) {
      int y = c.y + SC(18) + i * SC(7);
      graphics_draw_line(ctx, GPoint(c.x - SC(25), y), GPoint(c.x + SC(25), y));
    }
  } else if ((s_code >= 71 && s_code <= 77) || s_code == 85 || s_code == 86) {
    draw_cloud(ctx, GPoint(c.x, c.y - SC(10)));
    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorBlueMoon, family_ink_color()));
    graphics_context_set_stroke_width(ctx, 2);
    for (int i = -1; i <= 1; i++) {
      GPoint p = GPoint(c.x + i * SC(20), c.y + SC(20));
      graphics_draw_line(ctx, GPoint(p.x - SC(5), p.y), GPoint(p.x + SC(5), p.y));
      graphics_draw_line(ctx, GPoint(p.x, p.y - SC(5)), GPoint(p.x, p.y + SC(5)));
      graphics_draw_line(ctx, GPoint(p.x - SC(4), p.y - SC(4)), GPoint(p.x + SC(4), p.y + SC(4)));
      graphics_draw_line(ctx, GPoint(p.x - SC(4), p.y + SC(4)), GPoint(p.x + SC(4), p.y - SC(4)));
    }
  } else if (s_code >= 95) {
    draw_cloud(ctx, GPoint(c.x, c.y - SC(10)));
    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorChromeYellow, family_ink_color()));
    graphics_context_set_stroke_width(ctx, 4);
    graphics_draw_line(ctx, GPoint(c.x + SC(3), c.y + SC(13)), GPoint(c.x - SC(6), c.y + SC(27)));
    graphics_draw_line(ctx, GPoint(c.x - SC(6), c.y + SC(27)), GPoint(c.x + SC(3), c.y + SC(27)));
    graphics_draw_line(ctx, GPoint(c.x + SC(3), c.y + SC(27)), GPoint(c.x - SC(5), c.y + SC(40)));
  } else {
    // Rain / drizzle / showers
    draw_cloud(ctx, GPoint(c.x, c.y - SC(10)));
    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorBlueMoon, family_ink_color()));
    graphics_context_set_stroke_width(ctx, 3);
    for (int i = -1; i <= 1; i++) {
      int x = c.x + i * SC(20);
      graphics_draw_line(ctx, GPoint(x - SC(3), c.y + SC(15)), GPoint(x - SC(7), c.y + SC(28)));
    }
  }
}

// ---------- Refreshing the screen ----------

static bool code_is_snow(int code) {
  return (code >= 71 && code <= 77) || code == 85 || code == 86;
}

static bool code_is_rain(int code) {
  return (code >= 51 && code <= 67) || (code >= 80 && code <= 82) ||
         code >= 95;
}

// Paired chains: the MAX rides the main chain; the MIN runs
// exactly 4 points under it at every tier (user: -2, then -2 more). The pair steps
// down together until the combined width fits.
static void pick_temps_font(bool big, const char *min_buf,
                            const char *max_buf, int layer_w,
                            GFont *f_min, GFont *f_max, GSize *min_sz,
                            GSize *max_sz, int *total) {
  int pad = big ? 4 : 2;
  int sep_w = big ? 10 : 6;
  int deg_w = big ? 15 : 12;
#if defined(PBL_PLATFORM_EMERY)
  GFont big_max[4] = {s_f80, s_f72, s_f64, s_f56};
  GFont big_min[4] = {s_f76, s_f68, s_f60, s_f52};
  int big_n = 4;
#else
  GFont big_max[2] = {s_f64, s_f56};
  GFont big_min[2] = {s_f60, s_f52};
  int big_n = 2;
#endif
  GFont small_max[4] = {s_f50, s_f48, s_f46, s_f40};
  GFont small_min[4] = {s_f46, s_f44, s_f42, s_f36};
  GFont *cmax = big ? big_max : small_max;
  GFont *cmin = big ? big_min : small_min;
  int n = big ? big_n : 4;
  GRect measure = GRect(0, 0, 300, 100);
  for (int i = 0; i < n; i++) {
    *f_max = cmax[i];
    *f_min = cmin[i];
    *min_sz = graphics_text_layout_get_content_size(
        min_buf, *f_min, measure, GTextOverflowModeFill, GTextAlignmentLeft);
    *max_sz = graphics_text_layout_get_content_size(
        max_buf, *f_max, measure, GTextOverflowModeFill, GTextAlignmentLeft);
    *total = min_sz->w + pad * 2 + sep_w + max_sz->w + deg_w;
    if (*total <= layer_w - 6 || i == n - 1) break;
  }
}


// ---------- Lean the temps (user, v2.1.0) ----------
// The temperature pair is tilted 5 degrees ANTICLOCKWISE, the
// Lean watchface's trick: the temps are drawn upright as usual,
// then this pass reads their ink from the framebuffer into a
// mask, erases it, and writes it back rotated about the ink's
// own center, 2x2 supersampled so the edges stay smooth. The
// temps layer is created FIRST (before icon and header), so
// when this runs, the framebuffer under the temps holds only
// background and the mask is purely the temps' own ink. The
// precip line and the condition icon stay upright.
#define LEAN_TEMPS_DEG 5.0
static uint8_t s_lean_mask[(264 * 264 + 7) / 8];

// v2.1.2: rewritten in integer fixed point — no floating point
// anywhere (v2.1.0's double-precision pass ran in the emulator
// but crashed on hardware; the bisect build proved the fault
// was inside this function). Same shape as Lean's own renderer:
// 16.16 fixed-point steps, sin_lookup/cos_lookup for the angle,
// 2x2 supersampling, blend LUT between background and ink.
static void lean_pass(Layer *layer, GContext *ctx) {
#if defined(PBL_COLOR)
  GRect fr = layer_get_frame(layer);
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  GRect fbb = gbitmap_get_bounds(fb);
  int W = fbb.size.w, H = fbb.size.h;
  if ((W * H + 7) / 8 > (int)sizeof(s_lean_mask)) {
    graphics_release_frame_buffer(ctx, fb);
    return;
  }
  uint8_t bg = family_bg_color().argb;
  int x0 = fr.origin.x, y0 = fr.origin.y;
  int x1 = x0 + fr.size.w - 1, y1 = y0 + fr.size.h - 1;
  memset(s_lean_mask, 0, (W * H + 7) / 8);
  int ix0 = W, iy0 = H, ix1 = -1, iy1 = -1;
  for (int y = y0; y <= y1 && y < H; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int rx0 = x0 > row.min_x ? x0 : row.min_x;
    int rx1 = x1 < row.max_x ? x1 : row.max_x;
    for (int x = rx0; x <= rx1; x++) {
      if (row.data[x] == bg) continue;
      int i = y * W + x;
      s_lean_mask[i >> 3] |= (uint8_t)(1 << (i & 7));
      row.data[x] = bg;
      if (x < ix0) ix0 = x;
      if (x > ix1) ix1 = x;
      if (y < iy0) iy0 = y;
      if (y > iy1) iy1 = y;
    }
  }
  if (ix1 < ix0) {
    graphics_release_frame_buffer(ctx, fb);
    return;
  }
  // Ink center and sweep radius, all integers.
  int cx16 = (ix0 + ix1) << 15;  // center * 65536
  int cy16 = (iy0 + iy1) << 15;
  int iw = ix1 - ix0 + 1, ih = iy1 - iy0 + 1;
  int rad = family_isqrt(iw * iw + ih * ih) / 2 + 2;
  // Anticlockwise: source = C + R(+5 deg) * (dest - C), with
  // cs/sn as 16.16 fixed point from the SDK trig lookups.
  int32_t ang = (int32_t)(TRIG_MAX_ANGLE * LEAN_TEMPS_DEG / 360.0);
  int32_t cs = cos_lookup(ang);
  int32_t sn = sin_lookup(ang);
  uint8_t lut[5];
  for (int cov = 0; cov <= 4; cov++) {
    lut[cov] = family_blend(family_ink_color().argb, bg, cov);
  }
  int dy0 = (cy16 >> 16) - rad;
  if (dy0 < y0) dy0 = y0;
  int dy1 = (cy16 >> 16) + rad;
  if (dy1 > y1) dy1 = y1;
  for (int y = dy0; y <= dy1 && y < H; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int dx0 = (cx16 >> 16) - rad;
    if (dx0 < row.min_x) dx0 = row.min_x;
    int dx1 = (cx16 >> 16) + rad;
    if (dx1 > row.max_x) dx1 = row.max_x;
    for (int x = dx0; x <= dx1; x++) {
      int cov = 0;
      for (int s2 = 0; s2 < 4; s2++) {
        int32_t ox = (s2 & 1) ? 16384 : -16384;  // +/- 0.25 px
        int32_t oy = (s2 & 2) ? 16384 : -16384;
        int64_t ddx = ((int64_t)(x << 16) - cx16) + ox;
        int64_t ddy = ((int64_t)(y << 16) - cy16) + oy;
        int px = (int)((cx16 + ((ddx * cs - ddy * sn) >> 16)) >> 16);
        int py = (int)((cy16 + ((ddx * sn + ddy * cs) >> 16)) >> 16);
        if (px >= 0 && py >= 0 && px < W && py < H) {
          int i = py * W + px;
          cov += (s_lean_mask[i >> 3] >> (i & 7)) & 1;
        }
      }
      if (cov) row.data[x] = lut[cov];
    }
  }
  graphics_release_frame_buffer(ctx, fb);
#endif
}

static void temps_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  bool big = b.size.w >= 180;
  // Clear the layer first (v2.1.0): the Lean pass below reads the
  // temps' ink from the framebuffer, so the upright draw must
  // start from clean background every refresh — otherwise the
  // previous frame's rotated ink joins the mask and the tilt
  // compounds. The layer was transparent over this same color,
  // and the icon draws after, so nothing visual changes.
  graphics_context_set_fill_color(ctx, family_bg_color());
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  // LECO 1976 custom font (from the Lean watchface).
  graphics_context_set_text_color(ctx, family_ink_color());
  graphics_context_set_fill_color(ctx, family_ink_color());

  if (!s_have_data) {
    graphics_draw_text(ctx, "-----\xC2\xB0", big ? s_f64 : s_f46, b,
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    lean_pass(layer, ctx);
    return;
  }

  char min_buf[8], max_buf[16];
  snprintf(min_buf, sizeof(min_buf), "%d", s_temp_min);
  snprintf(max_buf, sizeof(max_buf), "%d", s_temp_max);

  int pad = big ? 4 : 2;
  int sep_w = big ? 10 : 6;
  int deg_w = big ? 15 : 12;  // hand-drawn degree ring after the max
  GFont f_min, f_max;
  GSize min_sz, max_sz;
  int total;
  pick_temps_font(big, min_buf, max_buf, b.size.w, &f_min, &f_max,
                  &min_sz, &max_sz, &total);

  // The ladder (user): an imaginary line runs through the screen's
  // center (the layer is framed centered, so it's the layer's
  // middle too). The MAX sits ABOVE the line, bottom-aligned to it
  // — its ink bottom rests on the line. The MIN sits BELOW the
  // line, top-aligned — its ink top starts at the line. Same font
  // size for both. (LECO ink spans ~30%..100% of the line height.)
  int fh = max_sz.h;
  int min_h = min_sz.h;
  int cy = b.size.h / 2;
  // Quarter ladder (user): each number moved toward the other
  // until the drop is a quarter of the full split. The shifts are
  // symmetric, so the group's ink span stays centered on the
  // screen's center — max above it, min below it.
  int shift = fh * 21 / 80;
  // Min stays exactly where it is; the max rides slightly
  // higher (user).
  int max_box_y = cy - fh + shift - (big ? 15 : 13);
  int min_box_y = cy - min_h * 30 / 100 - shift;
  int x = (b.size.w - total) / 2;
  int min_x = x;
  int max_x = x + min_sz.w + pad * 2 + sep_w;

  // The wobble is diagonal (user, v2.6.0): the max slides up
  // and right, the min down and left — along the ladder. The
  // min's filled circle stays in place (the anchor); the max's
  // ring follows its number (user correction, v2.6.1 — the
  // ring's position below gets the same offsets as the max).
  graphics_draw_text(ctx, max_buf, f_max,
                     GRect(max_x + family_wobble_offset, max_box_y - family_wobble_offset,
                           max_sz.w + 2, b.size.h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, min_buf, f_min,
                     GRect(min_x - family_wobble_offset, min_box_y + family_wobble_offset,
                           min_sz.w + 2, b.size.h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);

  // The min's degree is a FILLED circle (user, v2.7.3 — a dot
  // where a filled square used to sit) — same size and spot as
  // the max's ring, but solid; the max keeps the hollow square.
  {
    int mdeg_sz = big ? 11 : 9;
    int mdeg_x = min_x + min_sz.w + (big ? 4 : 3);
    int mdeg_y = min_box_y + min_h * 30 / 100 + 1;
    graphics_context_set_fill_color(ctx, family_ink_color());
    graphics_fill_circle(
        ctx, GPoint(mdeg_x + mdeg_sz / 2, mdeg_y + mdeg_sz / 2),
        mdeg_sz / 2);
  }

  // No separator at all (user: remove the divider).
  // Degree as a hand-drawn hollow square at the max's top.
  int deg_sz = big ? 11 : 9;
  int deg_x = max_x + max_sz.w + (big ? 4 : 3) + family_wobble_offset;
  int deg_y = max_box_y + fh * 30 / 100 + 1 - family_wobble_offset;
  graphics_context_set_fill_color(ctx, family_ink_color());
  graphics_fill_rect(ctx, GRect(deg_x, deg_y, deg_sz, deg_sz), 0,
                     GCornerNone);
  graphics_context_set_fill_color(ctx, family_bg_color());
  graphics_fill_rect(ctx,
                     GRect(deg_x + deg_sz / 3, deg_y + deg_sz / 3,
                           deg_sz - 2 * (deg_sz / 3),
                           deg_sz - 2 * (deg_sz / 3)),
                     0, GCornerNone);
  graphics_context_set_fill_color(ctx, family_ink_color());
  lean_pass(layer, ctx);
}

// ---------- Header: precip chance as icon + % ----------
// The word (Rain/Snow/Precip) is gone: a small droplet or snowflake
// icon sits beside the percentage. At 0% — or before the first
// reading — the header draws nothing at all (user request).

static void draw_droplet(GContext *ctx, int cx, int cy) {
  // Teardrop: a triangle tip over a round bottom, ~16px tall.
  static const GPoint drop_points[] = {{0, -8}, {-5, 2}, {5, 2}};
  static const GPathInfo drop_info = {.num_points = 3,
                                      .points = (GPoint *)drop_points};
  static GPath *path = NULL;
  if (!path) path = gpath_create(&drop_info);
  gpath_move_to(path, GPoint(cx, cy));
  graphics_context_set_fill_color(
      ctx, PBL_IF_COLOR_ELSE(GColorBlueMoon, family_ink_color()));
  gpath_draw_filled(ctx, path);
  graphics_fill_circle(ctx, GPoint(cx, cy + 2), 5);
}

static void draw_snowflake(GContext *ctx, int cx, int cy) {
  graphics_context_set_stroke_color(
      ctx, PBL_IF_COLOR_ELSE(GColorBlueMoon, family_ink_color()));
  graphics_context_set_stroke_width(ctx, 2);
  // Three spokes at 0/60/120 degrees — a small asterisk flake.
  graphics_draw_line(ctx, GPoint(cx - 7, cy), GPoint(cx + 7, cy));
  graphics_draw_line(ctx, GPoint(cx - 4, cy - 6), GPoint(cx + 4, cy + 6));
  graphics_draw_line(ctx, GPoint(cx - 4, cy + 6), GPoint(cx + 4, cy - 6));
}

static void header_update_proc(Layer *layer, GContext *ctx) {
  if (!s_have_data || s_precip <= 0) return;  // no header at 0%
  GRect b = layer_get_bounds(layer);
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_28);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", s_precip);
  GSize t_sz = graphics_text_layout_get_content_size(
      buf, font, GRect(0, 0, 300, b.size.h), GTextOverflowModeFill,
      GTextAlignmentLeft);

  int icon_w = 16, gap = 6;
  int total = icon_w + gap + t_sz.w;
  int x = (b.size.w - total) / 2;
  int cy = b.size.h / 2;

  if (code_is_snow(s_code)) {
    draw_snowflake(ctx, x + icon_w / 2, cy);
  } else {
    draw_droplet(ctx, x + icon_w / 2, cy);
  }
  graphics_context_set_text_color(ctx, family_ink_color());
  graphics_draw_text(ctx, buf, font,
                     GRect(x + icon_w + gap, 0, t_sz.w + 2, b.size.h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

static void refresh_display(void) {
  if (s_window) window_set_background_color(s_window, family_bg_color());
  // Re-frame the temps layer for the font size the picker will
  // choose, so the digits' visual center stays on the screen's
  // vertical center at every size tier (LECO ink centers at ~64%
  // of the line height).
  if (s_temps_layer) {
    GRect rb = layer_get_bounds(window_get_root_layer(s_window));
    bool big = rb.size.w >= 180;
    // Center-line framing: the temps layer is exactly two line
    // heights tall and centered on screen, so the layer's middle
    // IS the screen's center line the temps split around.
    int font_h;
    if (s_have_data) {
      char mn[8], mx[16];
      snprintf(mn, sizeof(mn), "%d", s_temp_min);
      snprintf(mx, sizeof(mx), "%d", s_temp_max);
      GFont fmn, fmx;
      GSize a, b;
      int t;
      pick_temps_font(big, mn, mx, rb.size.w, &fmn, &fmx, &a, &b, &t);
      font_h = font_height_of(fmx);
    } else {
#if defined(PBL_PLATFORM_EMERY)
      font_h = big ? 80 : 50;
#else
      font_h = big ? 64 : 50;
#endif
    }
    int frame_h = 2 * font_h + 4;
    // The pair sits LOWER as a group (user: centered felt too
    // busy up top) — dropped by a fifth of the font size.
    // Raised 6px (user, v2.1.3) from the lowered group position.
    layer_set_frame(s_temps_layer,
                    GRect(0, rb.size.h / 2 - frame_h / 2 + font_h / 5 - 6,
                          rb.size.w, frame_h));
  }
  layer_mark_dirty(s_header_layer);
  layer_mark_dirty(s_temps_layer);
  layer_mark_dirty(s_icon_layer);
}

// ---------- AppMessage ----------

static void request_update(void) {
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (iter) {
    dict_write_uint8(iter, MESSAGE_KEY_REQUEST, 1);
    app_message_outbox_send();
  }
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t_max = dict_find(iter, MESSAGE_KEY_TEMP_MAX);
  Tuple *t_min = dict_find(iter, MESSAGE_KEY_TEMP_MIN);
  Tuple *t_precip = dict_find(iter, MESSAGE_KEY_PRECIP);
  Tuple *t_code = dict_find(iter, MESSAGE_KEY_CODE);
  family_night_apply(iter, MESSAGE_KEY_SUNRISE, MESSAGE_KEY_SUNSET,
                       MESSAGE_KEY_NIGHT, PERSIST_SUNRISE, PERSIST_SUNSET,
                       PERSIST_NIGHT);

  if (t_max && t_min && t_precip && t_code) {
    s_temp_max = t_max->value->int32;
    s_temp_min = t_min->value->int32;
    s_precip = t_precip->value->int32;
    s_code = t_code->value->int32;
    s_have_data = true;

    persist_write_int(PERSIST_TEMP_MAX, s_temp_max);
    persist_write_int(PERSIST_TEMP_MIN, s_temp_min);
    persist_write_int(PERSIST_PRECIP, s_precip);
    persist_write_int(PERSIST_CODE, s_code);
    refresh_display();
  }
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  family_idle_arm();
  request_update();
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

// The appear wobble's hook: the offset moves the temps layer;
// the diagonal application lives in temps_update_proc, which
// reads family_wobble_offset.
static void wobble_hook(int offset) {
  if (s_temps_layer) layer_mark_dirty(s_temps_layer);
}

static void window_appear(Window *window) {
  family_wobble_start(wobble_hook);
  family_idle_arm();
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  int w = b.size.w;
  int h = b.size.h;
  window_set_background_color(window, family_bg_color());



  // Temps layer FIRST (v2.1.0): the Lean pass in its
  // proc needs a framebuffer holding only background
  // beneath the temps, so it must draw before the
  // icon and header. Ink never overlaps (19px gap),
  // so the stacking change is invisible.
  // Big min-max temperature — LECO numbers, drawn underscore separator
  // Main layer placed so the DIGITS' visual center lands on the
  // screen's vertical center (digits sit low in their line box, and
  // the old 42% slot left them ~24px under center on emery).
  s_temps_layer = layer_create(
      GRect(0, h / 2 - (w >= 180 ? 42 : 30), w, w >= 180 ? 78 : 54));
  layer_set_update_proc(s_temps_layer, temps_update_proc);
  layer_add_child(root, s_temps_layer);

  // Condition icon at the BOTTOM now (user) — the frame keeps its
  // 12px of extra headroom; the art center lands ~17% of the
  // screen height up from the bottom edge.
  s_icon_layer = layer_create(
      GRect(0, h - h * 17 / 100 - (h * 34 / 100 + 12) / 2 + 6, w,
            h * 34 / 100 + 12));
  layer_set_update_proc(s_icon_layer, icon_update_proc);
  layer_add_child(root, s_icon_layer);

  // Precipitation chance is now the FOOTER: small icon + %,
  // hidden at 0% (same 6% margin, mirrored to the bottom).
  // Precipitation line back up top (it traded places with the icon).
  s_header_layer = layer_create(GRect(0, h * 6 / 100, w, 36));
  layer_set_update_proc(s_header_layer, header_update_proc);
  layer_add_child(root, s_header_layer);

  refresh_display();
}

static void window_unload(Window *window) {
  layer_destroy(s_temps_layer);
  layer_destroy(s_header_layer);
  layer_destroy(s_icon_layer);
}

static void init(void) {
  s_f64 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_64));
  s_f50 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_50));
  s_f62 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_62));
  s_f54 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_54));
  s_f60 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_60));
  s_f52 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_52));
  s_f42 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_42));
  s_f36 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_36));
  s_f44 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_44));
  s_f38 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_38));
#if defined(PBL_PLATFORM_EMERY)
  s_f78 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_78));
  s_f70 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_70));
  s_f76 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_76));
  s_f68 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_68));
#endif
  s_f48 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_48));
#if defined(PBL_PLATFORM_EMERY)
  s_f80 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_80));
  s_f72 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_72));
#endif
  s_f46 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_46));
  s_f56 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_56));
  s_f40 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_40));
  family_night_load(PERSIST_NIGHT, PERSIST_SUNRISE, PERSIST_SUNSET);
  if (persist_exists(PERSIST_TEMP_MAX)) {
    s_temp_max = persist_read_int(PERSIST_TEMP_MAX);
    s_temp_min = persist_read_int(PERSIST_TEMP_MIN);
    s_precip = persist_read_int(PERSIST_PRECIP);
    s_code = persist_read_int(PERSIST_CODE);
    s_have_data = true;
  }

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .appear = window_appear,
    .unload = window_unload,
  });
  window_set_click_config_provider(s_window, click_config_provider);
  window_stack_push(s_window, true);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  app_message_register_inbox_received(inbox_received);
  app_message_open(256, 64);
  request_update();
}

static void deinit(void) {
  window_destroy(s_window);
  fonts_unload_custom_font(s_f64);
  fonts_unload_custom_font(s_f50);
  fonts_unload_custom_font(s_f62);
  fonts_unload_custom_font(s_f54);
  fonts_unload_custom_font(s_f60);
  fonts_unload_custom_font(s_f52);
  fonts_unload_custom_font(s_f42);
  fonts_unload_custom_font(s_f36);
  fonts_unload_custom_font(s_f44);
  fonts_unload_custom_font(s_f38);
#if defined(PBL_PLATFORM_EMERY)
  fonts_unload_custom_font(s_f78);
  fonts_unload_custom_font(s_f70);
  fonts_unload_custom_font(s_f76);
  fonts_unload_custom_font(s_f68);
#endif
  fonts_unload_custom_font(s_f48);
#if defined(PBL_PLATFORM_EMERY)
  fonts_unload_custom_font(s_f80);
  fonts_unload_custom_font(s_f72);
#endif
  fonts_unload_custom_font(s_f46);
  fonts_unload_custom_font(s_f56);
  fonts_unload_custom_font(s_f40);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
