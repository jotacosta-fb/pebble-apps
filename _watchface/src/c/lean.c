// _watchface -- a fork of Lean (MIT, by Axel Kark / Kieselleger) for
// the _app family. Changes vs. upstream: a font-size setting, and
// day/night awareness computed ON the watch from a once-sent
// location (see the Sonne section). Original header follows.
//
// Lean -- grosse Ziffern wie bei Ebb, aber ohne Ueberblendung und mit
// einstellbarem Anzeigewinkel. Wer den Arm nicht so weit drehen kann, dass
// die Uhr gerade vor dem Auge liegt, dreht stattdessen die Zeit.
//
// Pebble kann Text nicht gedreht zeichnen. Deshalb wird die Zeit zuerst ganz
// normal gezeichnet, danach aus dem Framebuffer in eine 1-Bit-Maske gelesen
// und dabei geloescht, und zum Schluss gedreht wieder hineingeschrieben. Jedes
// Zielpixel tastet die Maske an vier Stellen ab (2x2-Supersampling), daraus
// ergibt sich eine Mischfarbe zwischen Ziffer und Hintergrund -- die Kanten
// bleiben so auch bei schiefen Winkeln glatt.
//
// Gedreht wird um die Mitte des Ziffernblocks, die dabei auf die
// Displaymitte faellt. Passt der gedrehte Block nicht mehr aufs Display (nur
// auf Emery um 45 Grad herum knapp moeglich), wird er beim Drehen ein wenig
// verkleinert.
//
// Das Datum (Wochentag und Tag) dreht mit. Es wird vorab in eine eigene Maske
// gelesen und im "Leseraum" platziert, also im ungedrehten Koordinatensystem
// des Zeitblocks. Dort ist der Bildschirmrand ein gekipptes Rechteck bzw. ein
// Kreis, und gesucht wird die Stelle mit dem groessten Abstand zu Rand und
// Zeitblock -- nebeneinander, gestapelt oder getrennt, je nachdem, was mehr
// Luft laesst.
//
// Gezeichnet wird nur einmal pro Minute und bei geaenderten Einstellungen.
//
// Nur Emery und Gabbro: die 104er-LECO braucht das 512-Byte-Glyph-Limit, und
// der Framebuffer ist auf beiden 8 Bit je Pixel.

#include <pebble.h>
#include "../../shared/family.h"

// --------------------------------------------------------------- Diagnose

// Nur fuer Store-Screenshots: haelt die Anzeige auf Mittwoch, 14., 10:08.
// pebble emu-set-time greift in dieser Umgebung nicht. Vor dem Release
// wieder auskommentieren.
// #define LEAN_DEMO_TIME

// --------------------------------------------------------------- Farben

#define COLOR_BG_DEFAULT   GColorBlack
#define COLOR_FG_DEFAULT   GColorWhite

// --------------------------------------------------------------- Layout

// Ungedrehte Lage wie bei Ebb, relativ zur Displaymitte. Die genaue Lage ist
// unkritisch: gedreht wird ohnehin um die Mitte der tatsaechlich gezeichneten
// Pixel. Sie muss nur vollstaendig aufs Display passen, damit nichts
// abgeschnitten in die Maske geraet.
#define DIGIT_H      80
#define TEXT_LIFT    22
#define HOUR_TOP    -84     // Oberkante der Stundenzeile relativ zur Mitte
#define MIN_TOP       4     // Oberkante der Minutenzeile relativ zur Mitte

// Mindestabstand des gedrehten Blocks zum Displayrand
#if defined(PBL_PLATFORM_EMERY)
  #define FIT_MARGIN  4
#else                                      // gabbro
  #define FIT_MARGIN  8
#endif

// Groesster Framebuffer der Zielplattformen (gabbro 260 x 260)
#define FB_MAX_W    260
#define FB_MAX_H    260

// --------------------------------------------------------------- Datum

#define DATE_BOX_TOP      -25   // Textrahmen beim ungedrehten Vorzeichnen
#define DATE_BOX_H         50
#define DATE_CAPTURE_GAP    6   // Abstand der beiden Teile beim Vorzeichnen

#define DATE_GAP            8   // Mindestabstand zum Zeitblock
#define DATE_ITEM_GAP       9   // Wochentag und Tag nebeneinander
#define DATE_LINE_GAP       6   // Wochentag ueber Tag
// So viel mehr Luft (px) muss eine Trennung bringen, sonst bleibt das Datum
// zusammen -- getrennt liest es sich schlechter.
// Platzsuche: Rasterweite und Suchradius im Leseraum
#define DATE_STEP           2
#define DATE_RANGE        150

// Sprachen der Wochentagskuerzel. LANG_AUTO folgt der Uhr.
#define LANG_AUTO   0
#define LANG_COUNT  7

static const char *const LANG_CODES[LANG_COUNT] = {
  "en", "de", "fr", "es", "it", "pt", "nl",
};

// Kuerzel ab Sonntag, in Grossbuchstaben wie die Ziffern
static const char *const WDAY_NAMES[LANG_COUNT][7] = {
  { "SU",  "MO",  "TU",  "WE",  "TH",  "FR",  "SA"   },   // en
  { "SO",  "MO",  "DI",  "MI",  "DO",  "FR",  "SA"   },   // de
  { "DI",  "LU",  "MA",  "ME",  "JE",  "VE",  "SA"   },   // fr
  { "DO",  "LU",  "MA",  "MI",  "JU",  "VI",  "SA"   },   // es
  { "DO",  "LU",  "MA",  "ME",  "GI",  "VE",  "SA"   },   // it
  { "DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SÁB"  },   // pt
  { "ZO",  "MA",  "DI",  "WO",  "DO",  "VR",  "ZA"   },   // nl
};

// --------------------------------------------------------------- Einstellungen

#define WRIST_LEFT     0
#define WRIST_RIGHT    1

#define TILT_DEFAULT  20
#define TILT_MAX      90

#define PKEY_COLOR_BG   101
#define PKEY_COLOR_FG   102
#define PKEY_WRIST      103
#define PKEY_TILT       104
#define PKEY_FLIP       105
#define PKEY_DATE       106
#define PKEY_LANG       107
#define PKEY_FONTSIZE   108
#define PKEY_NIGHTMODE  109
#define PKEY_LAT        110
#define PKEY_LON        111
#define PKEY_ANIMDELAY  112
#define PKEY_WOBBLELEN  113

// s_color_bg/fg are the colours actually drawn with. By day they
// mirror the user's picks (s_user_bg/fg); at night they are the
// fixed black/white pair (see refresh_colors).
static GColor s_color_bg;
static GColor s_color_fg;
static GColor s_user_bg;
static GColor s_user_fg;

// Font size preference (_watchface): a percentage (slider in
// the settings, 60-140 in steps of 2; 100 = upstream size),
// applied on top of the angle fit -- below 100 it caps the fit,
// above 100 it may exceed 1.0 only as far as the block fits.
// (v1.0.0 shipped a 3-way Compact/Standard/Large switch; its
// persisted values 0/1/2 are migrated in load_settings.)
#define FONTSIZE_MIN_PCT   60
#define FONTSIZE_MAX_PCT   140
static const int FONTSIZE_LEGACY_PCT[3] = { 80, 100, 125 };
static int  s_fontpercent = 100;
static int  s_anim_delay = 400;   // ms before the appear wobble
static int  s_wobble_len = 200;   // ms the wobble lasts (user default)
static bool s_nightmode  = true;
static bool s_night_active = false;

// The place, microdegrees, sent once by the phone and persisted.
// Sunrise/sunset are computed on the watch from it (Sonne below).
static bool s_have_place = false;
static int  s_lat_e6 = 0;
static int  s_lon_e6 = 0;
static time_t s_sunrise = 0;
static time_t s_sunset  = 0;
static time_t s_sun_day = -1;
static int    s_wrist = WRIST_LEFT;
static int    s_tilt  = TILT_DEFAULT;
static bool   s_flip  = false;
static bool   s_date  = false;
static int    s_lang  = LANG_AUTO;

// --------------------------------------------------------------- Zustand

static Window *s_window;
static Layer  *s_canvas;
static GFont   s_digit_font;
static int     s_ref_w;      // Breite von "00", fuer eine stabile Skalierung

static char s_hour_text[4] = "";
static char s_min_text[4]  = "";
static char s_wday_text[8] = "";
static char s_mday_text[4] = "";

// Masken, 1 Bit je Pixel, Zeilenlaenge = Framebufferbreite
static uint8_t s_mask[(FB_MAX_W * FB_MAX_H + 7) / 8];
// _watchface fly-in: while the appear animation runs, the time
// mask is sampled with a horizontal source offset — the hour row
// (above the block centre) flies in from the left, the minute row
// from the right. Both are 0 at rest, rendering is then original.
static int s_fly_off_l, s_fly_off_r;
#define WOBBLE_AMP 14  // source pixels of sideways wobble
static uint8_t s_date_mask[(FB_MAX_W * FB_MAX_H + 7) / 8];

// Umriss der Tinte, Grenzen inklusive. Leer, solange x1 < x0.
typedef struct {
  int x0, y0, x1, y1;
} Ink;

// Rechteck im Leseraum, relativ zur Mitte des Zeitblocks, x1/y1 exklusiv
typedef struct {
  int x0, y0, x1, y1;
} Box;

// Ein Teil des Datums: wo seine Tinte in der Datumsmaske liegt und wohin er
// im Leseraum kommt
typedef struct {
  Ink ink;
  int x, y;          // Platzierung oben links, relativ zur Mitte des Zeitblocks
} DateItem;

#define ITEM_WDAY  0
#define ITEM_MDAY  1

static DateItem s_items[2];
static bool     s_date_placed = false;

// Datumsschriften: die grosse, und eine kleinere fuer Winkel, bei denen die
// grosse nirgends Platz findet (etwa 90 Grad auf Emery)
#define DATE_FONTS  4
static GFont s_date_fonts[DATE_FONTS];

// Merkt sich, wofuer die Platzierung zuletzt berechnet wurde. Die Suche ist
// teuer, die Eingaben aendern sich hoechstens einmal am Tag.
typedef struct {
  int deg;
  int32_t k;
  int obst_w, obst_h;
  char wday[8];
  char mday[4];
} LayoutKey;

static LayoutKey s_layout_key;
static bool      s_layout_valid = false;
static int       s_date_font_idx = 0;    // Ergebnis: gewaehlte Datumsschrift

// --------------------------------------------------------------- Zeittext

static int current_lang(void) {
  if (s_lang > LANG_AUTO && s_lang <= LANG_COUNT) return s_lang - 1;
  const char *locale = i18n_get_system_locale();
  if (locale) {
    for (int i = 0; i < LANG_COUNT; i++) {
      if (strncmp(locale, LANG_CODES[i], 2) == 0) return i;
    }
  }
  return 0;                                // Englisch
}

static void update_time_text(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
#ifdef LEAN_DEMO_TIME
  struct tm demo = *t;
  demo.tm_hour = 10;
  demo.tm_min = 8;
  demo.tm_mday = 14;
  demo.tm_wday = 3;
  t = &demo;
#endif
  // Klemmung gegen -Wformat-truncation, die Werte liegen ohnehin im Bereich
  int hour = t->tm_hour % 24;
  int min  = t->tm_min % 60;
  int mday = t->tm_mday % 32;
  int wday = t->tm_wday % 7;
  if (hour < 0) hour = 0;
  if (min < 0) min = 0;
  if (mday < 1) mday = 1;
  if (wday < 0) wday = 0;

  if (!clock_is_24h_style()) {
    hour = hour % 12;
    if (hour == 0) hour = 12;
    snprintf(s_hour_text, sizeof(s_hour_text), "%d", hour);
  } else {
    snprintf(s_hour_text, sizeof(s_hour_text), "%02d", hour);
  }
  snprintf(s_min_text, sizeof(s_min_text), "%02d", min);

  snprintf(s_wday_text, sizeof(s_wday_text), "%s", WDAY_NAMES[current_lang()][wday]);
  snprintf(s_mday_text, sizeof(s_mday_text), "%d", mday);
}

// --------------------------------------------------------------- Rechenhilfen

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iabs(int a) { return a < 0 ? -a : a; }

// --------------------------------------------------------------- Drehung

// Drehwinkel in Grad, im Uhrzeigersinn positiv. Am linken Handgelenk liegt
// die Uhr beim Hinsehen gegen den Uhrzeigersinn verdreht, die Zeit muss also
// im Uhrzeigersinn gegensteuern -- rechts genau umgekehrt.
static int rotation_deg(void) {
  int deg = (s_wrist == WRIST_LEFT) ? s_tilt : -s_tilt;
  if (s_flip) deg += 180;
  return ((deg % 360) + 360) % 360;
}

// Skalierung (16.16, hoechstens 1.0), mit der ein w x h grosser Block um den
// Winkel mit |cos| = ac und |sin| = as noch aufs Display passt.
static int32_t fit_scale(int w, int h, int32_t ac, int32_t as,
                         int fb_w, int fb_h) {
  int64_t k = 1 << 16;
#if defined(PBL_ROUND)
  // Auf dem runden Display zaehlt nur die Diagonale, der Winkel ist egal
  (void)ac; (void)as; (void)fb_h;
  int64_t need = family_isqrt((uint32_t)(w * w + h * h));
  int64_t avail = fb_w - 2 * FIT_MARGIN;
  if (need > avail) k = (avail << 16) / need;
#else
  int64_t need_w = ((int64_t)w * ac + (int64_t)h * as) >> 16;
  int64_t need_h = ((int64_t)w * as + (int64_t)h * ac) >> 16;
  int64_t avail_w = fb_w - 2 * FIT_MARGIN;
  int64_t avail_h = fb_h - 2 * FIT_MARGIN;
  if (need_w > avail_w) k = (avail_w << 16) / need_w;
  if (need_h > avail_h && (avail_h << 16) / need_h < k) k = (avail_h << 16) / need_h;
#endif
  return (int32_t)k;
}

static inline int bit_get(const uint8_t *mask, int x, int y, int w, int h) {
  if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return 0;
  int i = y * w + x;
  return (mask[i >> 3] >> (i & 7)) & 1;
}

// Liest alle Nicht-Hintergrund-Pixel in die Maske und loescht sie im
// Framebuffer. Links und rechts von split_x wird der Umriss getrennt
// vermessen; mit split_x = 0 landet alles rechts.
static void capture_ink(GBitmap *fb, uint8_t *mask, uint8_t bg, int split_x,
                        Ink *left, Ink *right) {
  GRect fb_bounds = gbitmap_get_bounds(fb);
  int w = fb_bounds.size.w;
  int h = fb_bounds.size.h;
  *left  = (Ink){ w, h, -1, -1 };
  *right = (Ink){ w, h, -1, -1 };

  memset(mask, 0, (w * h + 7) / 8);
  for (int y = 0; y < h; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    for (int x = row.min_x; x <= row.max_x; x++) {
      if (row.data[x] == bg) continue;
      int i = y * w + x;
      mask[i >> 3] |= (uint8_t)(1 << (i & 7));
      row.data[x] = bg;
      Ink *ink = (x < split_x) ? left : right;
      if (x < ink->x0) ink->x0 = x;
      if (x > ink->x1) ink->x1 = x;
      if (y < ink->y0) ink->y0 = y;
      if (y > ink->y1) ink->y1 = y;
    }
  }
}

static inline bool ink_empty(const Ink *ink) { return ink->x1 < ink->x0; }
static inline int  ink_w(const Ink *ink) { return ink->x1 - ink->x0 + 1; }
static inline int  ink_h(const Ink *ink) { return ink->y1 - ink->y0 + 1; }

// --------------------------------------------------------------- Platzsuche

// Alles, was die Platzsuche ueber Display und Zeitblock wissen muss
typedef struct {
  int32_t cs, sn;    // Drehung, 16.16
  int32_t k;         // Skalierung, 16.16
  int half_w, half_h;  // Display ohne Rand, halbe Masse (eckig)
  int radius;          // Display ohne Rand (rund)
  Box obstacles[2];
  int n_obstacles;
} Geo;

// Abstand eines Rechtecks im Leseraum zum Displayrand und zu allen
// Hindernissen, jeweils abzueglich des geforderten Mindestabstands.
// Negativ heisst: passt nicht.
static int clearance(const Geo *g, const Box *b) {
  int best = 0x7FFF;

  for (int i = 0; i < g->n_obstacles; i++) {
    const Box *o = &g->obstacles[i];
    int gx = imax(o->x0 - b->x1, b->x0 - o->x1);
    int gy = imax(o->y0 - b->y1, b->y0 - o->y1);
    if (gx < 0 && gy < 0) return -1;      // ueberlappt
    int dist = (gx > 0 && gy > 0) ? (int)family_isqrt((uint32_t)(gx * gx + gy * gy))
                                  : imax(gx, gy);
    best = imin(best, dist - DATE_GAP);
    if (best < 0) return -1;
  }

  // Die Ecken genuegen: Display und Rechteck sind beide konvex
  for (int i = 0; i < 4; i++) {
    int tx = (i & 1) ? b->x1 : b->x0;
    int ty = (i & 2) ? b->y1 : b->y0;
    int64_t rx = (int64_t)g->cs * tx - (int64_t)g->sn * ty;
    int64_t ry = (int64_t)g->sn * tx + (int64_t)g->cs * ty;
    int dx = (int)((rx * g->k) >> 32);
    int dy = (int)((ry * g->k) >> 32);
#if defined(PBL_ROUND)
    int c = g->radius - (int)family_isqrt((uint32_t)(dx * dx + dy * dy));
#else
    int c = imin(g->half_w - iabs(dx), g->half_h - iabs(dy));
#endif
    best = imin(best, c);
    if (best < 0) return -1;
  }
  return best;
}

// Sucht fuer ein w x h grosses Rechteck die Stelle mit dem meisten Platz.
// Bei Gleichstand gewinnt die Stelle naeher an der Mittelachse, dann die
// weiter unten. Liefert den Abstand oder -1, wenn es nirgends passt.
typedef struct {
  int c, ax, cx, cy;
  Box box;
} Candidate;

// Bei gleichem Platz: naeher an der Mittelachse, dann rechts der Zeit, dann
// darunter -- so steht das Datum, wo man es von Papier und Uhr gewohnt ist.
static void try_candidate(const Geo *g, int cx, int cy, int w, int h, Candidate *best) {
  Box b = { cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h };
  int c = clearance(g, &b);
  if (c < 0) return;
  int ax = iabs(cx);
  bool better = c > best->c ||
      (c == best->c && (ax < best->ax ||
      (ax == best->ax && (cx > best->cx ||
      (cx == best->cx && cy > best->cy)))));
  if (better) *best = (Candidate){ c, ax, cx, cy, b };
}

// Erst grob im Raster, dann pixelgenau um den besten Treffer herum. Das
// Raster darf nicht groeber sein als der knappste Spielraum, den man noch
// finden will: bei 90 Grad auf Emery passt der Wochentag mit nur 2 px Luft
// neben die Zeit.
static int place_box(const Geo *g, int w, int h, Box *out) {
  Candidate best = { -1, 0, 0, 0, { 0, 0, 0, 0 } };
  for (int cy = -DATE_RANGE; cy <= DATE_RANGE; cy += DATE_STEP) {
    for (int cx = -DATE_RANGE; cx <= DATE_RANGE; cx += DATE_STEP) {
      try_candidate(g, cx, cy, w, h, &best);
    }
  }
  if (best.c < 0) return -1;

  int bx = best.cx;
  int by = best.cy;
  for (int cy = by - DATE_STEP; cy <= by + DATE_STEP; cy++) {
    for (int cx = bx - DATE_STEP; cx <= bx + DATE_STEP; cx++) {
      try_candidate(g, cx, cy, w, h, &best);
    }
  }
  *out = best.box;
  return best.c;
}

// Ordnet Wochentag und Tag an: nebeneinander oder uebereinander, je
// nachdem, was den meisten Abstand zu Rand und Zeitblock laesst. Sie
// bleiben immer zusammen (user, v1.4.2): die fruehere dritte Anordnung
// "getrennt" — jede Haelfte an ihrer eigenen besten Stelle — gibt es
// nicht mehr; passt das Paar in der grossen Datumsschrift nirgends
// zusammen, faellt prepare_date auf die kleinere Schrift zurueck.
static void layout_date(Geo *g) {
  int ww = ink_w(&s_items[ITEM_WDAY].ink), hw = ink_h(&s_items[ITEM_WDAY].ink);
  int wd = ink_w(&s_items[ITEM_MDAY].ink), hd = ink_h(&s_items[ITEM_MDAY].ink);
  int best = -1;
  Box b;

  // Nebeneinander, unten buendig
  int row_h = imax(hw, hd);
  if ((best = place_box(g, ww + DATE_ITEM_GAP + wd, row_h, &b)) >= 0) {
    s_items[ITEM_WDAY].x = b.x0;
    s_items[ITEM_WDAY].y = b.y1 - hw;
    s_items[ITEM_MDAY].x = b.x0 + ww + DATE_ITEM_GAP;
    s_items[ITEM_MDAY].y = b.y1 - hd;
  }

  // Uebereinander, mittig
  int col_w = imax(ww, wd);
  int stacked = place_box(g, col_w, hw + DATE_LINE_GAP + hd, &b);
  if (stacked > best) {
    best = stacked;
    s_items[ITEM_WDAY].x = (b.x0 + b.x1 - ww) / 2;
    s_items[ITEM_WDAY].y = b.y0;
    s_items[ITEM_MDAY].x = (b.x0 + b.x1 - wd) / 2;
    s_items[ITEM_MDAY].y = b.y1 - hd;
  }

  s_date_placed = best >= 0;
}

// --------------------------------------------------------------- Rendern

// Lage und Drehung des Zeitblocks fuer das aktuelle Bild
typedef struct {
  int w, h;              // Framebuffer
  int src_cx, src_cy;    // Mitte des Zeitblocks in der Maske
  int tw, th;            // Tintenumriss des Zeitblocks
  int deg;
  int32_t cs, sn;        // Drehung, 16.16
  int32_t k;             // Skalierung, 16.16
} Frame;

// Wie fit_scale, aber ohne die 1.0-Grenze: die groesste
// Skalierung, mit der der Block noch passt (fuer die
// Schriftgroessen-Einstellung "Gross").
static int32_t fit_scale_uncapped(int w, int h, int32_t ac, int32_t as,
                                  int fb_w, int fb_h) {
#if defined(PBL_ROUND)
  (void)ac; (void)as; (void)fb_h;
  int64_t need = family_isqrt((uint32_t)(w * w + h * h));
  int64_t avail = fb_w - 2 * FIT_MARGIN;
  return (int32_t)((avail << 16) / need);
#else
  int64_t need_w = ((int64_t)w * ac + (int64_t)h * as) >> 16;
  int64_t need_h = ((int64_t)w * as + (int64_t)h * ac) >> 16;
  int64_t avail_w = fb_w - 2 * FIT_MARGIN;
  int64_t avail_h = fb_h - 2 * FIT_MARGIN;
  int64_t kw = (avail_w << 16) / need_w;
  int64_t kh = (avail_h << 16) / need_h;
  return (int32_t)(kw < kh ? kw : kh);
#endif
}

static void frame_setup(Frame *f, int w, int h, const Ink *time_ink) {
  f->w = w;
  f->h = h;
  f->tw = ink_w(time_ink);
  f->th = ink_h(time_ink);
  // Ganzzahlige Mittelpunkte: bei 0, 90 und 180 Grad treffen die Abtastpunkte
  // dann genau die Quellpixel, und die Kanten bleiben scharf.
  f->src_cx = (time_ink->x0 + time_ink->x1 + 1) / 2;
  f->src_cy = (time_ink->y0 + time_ink->y1 + 1) / 2;
  f->deg = rotation_deg();
  int32_t angle = DEG_TO_TRIGANGLE(f->deg);
  f->cs = cos_lookup(angle);
  f->sn = sin_lookup(angle);
  // Fuer die Skalierung die Breite von "00": sonst wuerde der Block je nach
  // Ziffern minimal pumpen.
  f->k = fit_scale(imax(f->tw, s_ref_w), f->th, iabs(f->cs), iabs(f->sn), w, h);
  // Schriftgroesse (_watchface): "Kompakt" deckelt die Pass-
  // Skalierung, "Gross" darf ueber 1.0 hinaus, soweit der Block
  // noch aufs Display passt.
  int32_t pref = (int32_t)(((int64_t)s_fontpercent << 16) / 100);
  if (pref <= (1 << 16)) {
    if (f->k > pref) f->k = pref;
  } else {
    int32_t fit_max = fit_scale_uncapped(imax(f->tw, s_ref_w), f->th,
                                         iabs(f->cs), iabs(f->sn), w, h);
    f->k = pref < fit_max ? pref : fit_max;
  }
}

// Datumsbit an einer Stelle des Leseraums (relativ zur Mitte des Zeitblocks)
static inline int date_get(int rx, int ry, int w, int h) {
  for (int i = 0; i < 2; i++) {
    const DateItem *it = &s_items[i];
    int ix = rx - it->x;
    int iy = ry - it->y;
    if ((unsigned)ix < (unsigned)ink_w(&it->ink) &&
        (unsigned)iy < (unsigned)ink_h(&it->ink)) {
      return bit_get(s_date_mask, it->ink.x0 + ix, it->ink.y0 + iy, w, h);
    }
  }
  return 0;
}

// Zeichnet Zeit- und Datumsmaske gedreht in den Framebuffer
static void render_rotated(GBitmap *fb, const Frame *f, bool with_date) {
  int w = f->w;
  int h = f->h;
  uint8_t bg = s_color_bg.argb;
  int dst_cx = w / 2;
  int dst_cy = h / 2;
  int64_t inv_k = ((int64_t)1 << 32) / f->k;

  // Rueckwaertsabbildung Ziel -> Quelle: Schritt je Zielpixel in x (u) und y (v)
  int32_t ux = (int32_t)(( (int64_t)f->cs * inv_k) >> 16);
  int32_t uy = (int32_t)((-(int64_t)f->sn * inv_k) >> 16);
  int32_t vx = (int32_t)(( (int64_t)f->sn * inv_k) >> 16);
  int32_t vy = (int32_t)(( (int64_t)f->cs * inv_k) >> 16);

  // Vier Abtastpunkte je Zielpixel, um +-1/4 Pixel gegen die Pixelmitte versetzt
  int32_t off_x[4], off_y[4];
  for (int i = 0; i < 4; i++) {
    int a = (i & 1) ? 1 : -1;
    int b = (i & 2) ? 1 : -1;
    off_x[i] = (a * ux + b * vx) / 4;
    off_y[i] = (a * uy + b * vy) / 4;
  }

  uint8_t lut[5];
  for (int cov = 0; cov <= 4; cov++) lut[cov] = family_blend(s_color_fg.argb, bg, cov);
  // Das Datum wird immer grau statt in der Ziffernfarbe
  // geschrieben (user: das Grau der Ruhezeit gefiel ihm so gut,
  // dass es v1.4.8 zum Dauerzustand wurde) — eigene LUT, weil
  // Zeit- und Datumsink unten getrennt gezaehlt werden.
  uint8_t dlut[5];
  uint8_t date_ink = GColorDarkGray.argb;
  for (int cov = 0; cov <= 4; cov++) dlut[cov] = family_blend(date_ink, bg, cov);

  // Ohne Datum genuegt der Kreis, den der gedrehte Zeitblock ueberstreichen
  // kann; mit Datum wird das ganze Display abgefahren.
  int radius = (int)(((int64_t)(family_isqrt((uint32_t)(f->tw * f->tw + f->th * f->th)) / 2 + 2) * f->k) >> 16) + 2;
  if (with_date) radius = imax(w, h);
  int y0 = imax(dst_cy - radius, 0);
  int y1 = imin(dst_cy + radius, h - 1);

  for (int y = y0; y <= y1; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int x0 = imax(dst_cx - radius, row.min_x);
    int x1 = imin(dst_cx + radius, row.max_x);
    if (x0 > x1) continue;

    int64_t dx = ((int64_t)(x0 - dst_cx) << 16) + (1 << 15);
    int64_t dy = ((int64_t)(y - dst_cy) << 16) + (1 << 15);
    int32_t sx = (f->src_cx << 16) + (int32_t)((dx * ux + dy * vx) >> 16);
    int32_t sy = (f->src_cy << 16) + (int32_t)((dx * uy + dy * vy) >> 16);

    for (int x = x0; x <= x1; x++) {
      int cov = 0;
      int dcov = 0;
      for (int i = 0; i < 4; i++) {
        int px = (sx + off_x[i]) >> 16;
        int py = (sy + off_y[i]) >> 16;
        int ox = (s_fly_off_l | s_fly_off_r)
                     ? ((py < f->src_cy) ? s_fly_off_l : s_fly_off_r) : 0;
        if (bit_get(s_mask, px + ox, py, w, h)) cov++;
        else if (with_date &&
                 date_get(px - f->src_cx, py - f->src_cy, w, h)) dcov++;
      }
      if (cov) row.data[x] = lut[cov];
      else if (dcov) row.data[x] = dlut[dcov];
      sx += ux;
      sy += uy;
    }
  }
}

// --------------------------------------------------------------- Zeichnen

static void draw_row(GContext *ctx, GRect bounds, int y, const char *text) {
  GRect box = GRect(bounds.origin.x, y - TEXT_LIFT,
                    bounds.size.w, DIGIT_H + TEXT_LIFT);
  graphics_draw_text(ctx, text, s_digit_font, box,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     NULL);
}

// Zeichnet das Datum ungedreht, liest es in die Datumsmaske und loescht es
// wieder. Wochentag links, Tag rechts der Mitte -- so lassen sich beide beim
// Einlesen an der Mitte auseinanderhalten.
static bool capture_date(GContext *ctx, GRect bounds, GFont font) {
  int cx = bounds.size.w / 2;
  int top = bounds.size.h / 2 + DATE_BOX_TOP;
  graphics_draw_text(ctx, s_wday_text, font,
                     GRect(0, top, cx - DATE_CAPTURE_GAP, DATE_BOX_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  graphics_draw_text(ctx, s_mday_text, font,
                     GRect(cx + DATE_CAPTURE_GAP, top, bounds.size.w - cx - DATE_CAPTURE_GAP, DATE_BOX_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return false;
  capture_ink(fb, s_date_mask, s_color_bg.argb, gbitmap_get_bounds(fb).size.w / 2,
              &s_items[ITEM_WDAY].ink, &s_items[ITEM_MDAY].ink);
  graphics_release_frame_buffer(ctx, fb);
  return !ink_empty(&s_items[ITEM_WDAY].ink) && !ink_empty(&s_items[ITEM_MDAY].ink);
}

// Liest das Datum ein und sorgt fuer eine Platzierung. Die Suche laeuft nur,
// wenn sich Winkel, Zeitblock oder Datumstext geaendert haben; dann die
// Datumsschriften der Reihe nach von gross nach klein (28/22/16/12), bis
// das Paar irgendwo zusammen passt. Seit v1.4.3 (user) verschwindet das
// Datum nicht mehr, wenn es bei grosser Zeiteinstellung (z. B. 114 %) in
// den zwei grossen Schriften keinen Platz hat — es wird einfach kleiner.
// Liefert false, wenn das Datum nirgends Platz hat.
static bool prepare_date(GContext *ctx, GRect bounds, const Frame *f) {
  LayoutKey key;
  memset(&key, 0, sizeof(key));
  key.deg = f->deg;
  key.k = f->k;
  key.obst_w = f->tw;
  key.obst_h = f->th;
  strncpy(key.wday, s_wday_text, sizeof(key.wday) - 1);
  strncpy(key.mday, s_mday_text, sizeof(key.mday) - 1);

  if (s_layout_valid && memcmp(&key, &s_layout_key, sizeof(key)) == 0) {
    if (!s_date_placed) return false;
    // Positionen stehen schon, nur die Maske muss neu gefuellt werden
    return capture_date(ctx, bounds, s_date_fonts[s_date_font_idx]);
  }

  s_date_placed = false;
  for (int i = 0; i < DATE_FONTS && !s_date_placed; i++) {
    if (!capture_date(ctx, bounds, s_date_fonts[i])) continue;
    Geo g = {
      .cs = f->cs, .sn = f->sn, .k = f->k,
      .half_w = f->w / 2 - FIT_MARGIN, .half_h = f->h / 2 - FIT_MARGIN,
      .radius = f->w / 2 - FIT_MARGIN,
      .n_obstacles = 1,
    };
    // Der echte Tintenumriss, nicht die Breite von "00": neben der Zeit ist
    // jeder Pixel wertvoll
    g.obstacles[0] = (Box){ -f->tw / 2, -f->th / 2, f->tw - f->tw / 2, f->th - f->th / 2 };
    layout_date(&g);
    s_date_font_idx = i;
  }
  s_layout_key = key;
  s_layout_valid = true;
  return s_date_placed;
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int cy = bounds.size.h / 2;

  graphics_context_set_fill_color(ctx, s_color_bg);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, s_color_fg);

  // 1. Zeit zeichnen und in die Maske lesen
  draw_row(ctx, bounds, cy + HOUR_TOP, s_hour_text);
  draw_row(ctx, bounds, cy + MIN_TOP, s_min_text);


  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  GRect fb_bounds = gbitmap_get_bounds(fb);
  Ink unused, time_ink;
  bool ok = fb_bounds.size.w <= FB_MAX_W && fb_bounds.size.h <= FB_MAX_H;
  if (ok) {
    capture_ink(fb, s_mask, s_color_bg.argb, 0, &unused, &time_ink);
    ok = !ink_empty(&time_ink);
  }
  graphics_release_frame_buffer(ctx, fb);
  if (!ok) return;

  Frame f;
  frame_setup(&f, fb_bounds.size.w, fb_bounds.size.h, &time_ink);

  // 2. Datum einlesen und platzieren
  bool with_date = s_date && prepare_date(ctx, bounds, &f);

  // 3. Alles gedreht zurueckschreiben. Auch bei 0 Grad: der Block landet
  // dann exakt mittig.
  fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  render_rotated(fb, &f, with_date);
  graphics_release_frame_buffer(ctx, fb);

  // 4. Quiet-Time-Marken (user): konkave Ecken in allen vier
  // BildschirmeEcken — geteilte Implementierung in family.h.
  family_qt_earmarks(ctx, bounds, s_color_fg);

  // Dazu (v1.4.9) statt des Punkts aus v1.4.8 — der gefiel dem
  // user nicht — eine duenne, zentrierte Linie am unteren
  // Bildschirmrand, in halber Ink-Staerke: sehr dezent.
  if (quiet_time_is_active()) {
    GColor half = (GColor){.argb = family_blend(s_color_fg.argb,
                                                s_color_bg.argb, 2)};
    graphics_context_set_fill_color(ctx, half);
    graphics_fill_rect(ctx, GRect((bounds.size.w - 40) / 2,
                                  bounds.size.h - 10, 40, 2),
                       0, GCornerNone);
  }
}

// --------------------------------------------------------------- Sonne
// _watchface: day/night awareness the least battery-intensive
// way. The phone sends the location ONCE (LAT/LON, persisted on
// the watch); from then on the watch computes sunrise and sunset
// itself, in integer fixed point (NOAA approximation, accurate
// to a few minutes), once per day. The minute tick only compares
// the current time against the two stored instants: no radio, no
// GPS, no extra wakeups at runtime.

// acos for a Q16 cosine via binary search on cos_lookup
// (monotonic over 0..180 degrees). Returns a TRIGANGLE.
static int32_t acos_trig(int32_t cos_q16) {
  int32_t lo = 0, hi = TRIG_MAX_ANGLE / 2;
  for (int i = 0; i < 18; i++) {
    int32_t mid = (lo + hi) / 2;
    if (cos_lookup(mid) > cos_q16) lo = mid; else hi = mid;
  }
  return (lo + hi) / 2;
}

static void sun_recompute(time_t now) {
  s_sun_day = now - (now % 86400);
  s_sunrise = 0;
  s_sunset = 0;
  if (!s_have_place) return;
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
  int32_t lat = (int32_t)(((int64_t)s_lat_e6 * TRIG_MAX_ANGLE) / 360000000);
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
    s_sunrise = s_sun_day + 2 * 86400;
    s_sunset = s_sun_day - 86400;
    return;
  }
  if (cos_h <= -65536) {         // the sun never sets today
    s_sunrise = s_sun_day - 86400;
    s_sunset = s_sun_day + 2 * 86400;
    return;
  }
  int32_t h_trig = acos_trig((int32_t)cos_h);
  int64_t h_deg_q16 = (int64_t)h_trig * 360;   // degrees, Q16
  int64_t lon_deg_q16 = ((int64_t)s_lon_e6 * 65536) / 1000000;
  int64_t rise_q16 = ((int64_t)720 << 16) - 4 * (lon_deg_q16 + h_deg_q16)
                     - eq_q16;
  int64_t set_q16 = ((int64_t)720 << 16) - 4 * (lon_deg_q16 - h_deg_q16)
                    - eq_q16;
  s_sunrise = s_sun_day + (time_t)((rise_q16 * 60) >> 16);
  s_sunset = s_sun_day + (time_t)((set_q16 * 60) >> 16);
}

static void refresh_colors(void) {
  if (s_night_active) {
    s_color_bg = GColorBlack;
    s_color_fg = GColorWhite;
  } else {
    s_color_bg = s_user_bg;
    s_color_fg = s_user_fg;
  }
}

// Runs on the minute tick and after settings/location changes.
// Only the day rollover costs a recomputation; otherwise this is
// two comparisons and a colour assignment.
static void night_update(time_t now) {
  if (now - (now % 86400) != s_sun_day) sun_recompute(now);
  s_night_active = s_nightmode && s_have_place &&
                   (now < s_sunrise || now >= s_sunset);
  refresh_colors();
  if (s_window) window_set_background_color(s_window, s_color_bg);
}

// --------------------------------------------------------------- Zeit

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  night_update(time(NULL));
  update_time_text();
  layer_mark_dirty(s_canvas);
}

// --------------------------------------------------------------- Einstellungen

// Faengt die Farbwahl FG == BG ab, die das Watchface unlesbar machen wuerde.
// Clay kann das nicht verhindern, also tut es die Uhr.
static void enforce_contrast(void) {
  if (gcolor_equal(s_user_fg, s_user_bg)) {
    s_user_fg = gcolor_legible_over(s_user_bg);
  }
}

// Clay-Werte kommen je nach Element als String oder als Zahl an, Zahlen
// zudem in 1, 2 oder 4 Byte (der Toggle etwa als einzelnes Byte). Alles
// annehmen -- ein falsch geratener Typ wuerde stumm Muell liefern.
static int tuple_int(Tuple *t) {
  if (t->type == TUPLE_CSTRING) return atoi(t->value->cstring);
  bool is_signed = (t->type == TUPLE_INT);
  switch (t->length) {
    case 1:  return is_signed ? t->value->int8  : t->value->uint8;
    case 2:  return is_signed ? t->value->int16 : t->value->uint16;
    default: return is_signed ? t->value->int32 : (int)t->value->uint32;
  }
}

static bool valid_wrist(int v) { return v == WRIST_LEFT || v == WRIST_RIGHT; }
static bool valid_tilt(int v)  { return v >= 0 && v <= TILT_MAX; }
static bool valid_lang(int v)  { return v >= LANG_AUTO && v <= LANG_COUNT; }

static bool valid_animdelay(int v) { return v >= 0 && v <= 5000; }
static bool valid_wobblelen(int v) { return v >= 50 && v <= 5000; }

static bool valid_fontsize(int v) {
  return v >= FONTSIZE_MIN_PCT && v <= FONTSIZE_MAX_PCT;
}

static void load_settings(void) {
  s_user_bg = COLOR_BG_DEFAULT;
  s_user_fg = COLOR_FG_DEFAULT;

  if (persist_exists(PKEY_COLOR_BG)) {
    s_user_bg = GColorFromHEX(persist_read_int(PKEY_COLOR_BG));
  }
  if (persist_exists(PKEY_COLOR_FG)) {
    s_user_fg = GColorFromHEX(persist_read_int(PKEY_COLOR_FG));
  }
  if (persist_exists(PKEY_FONTSIZE)) {
    int v = persist_read_int(PKEY_FONTSIZE);
    if (v >= 0 && v <= 2) {
      s_fontpercent = FONTSIZE_LEGACY_PCT[v];  // v1.0.0 switch
    } else if (valid_fontsize(v)) {
      s_fontpercent = v;
    }
  }
  if (persist_exists(PKEY_NIGHTMODE)) {
    s_nightmode = persist_read_bool(PKEY_NIGHTMODE);
  }
  if (persist_exists(PKEY_ANIMDELAY)) {
    int v = persist_read_int(PKEY_ANIMDELAY);
    if (valid_animdelay(v)) s_anim_delay = v;
  }
  if (persist_exists(PKEY_WOBBLELEN)) {
    int v = persist_read_int(PKEY_WOBBLELEN);
    if (valid_wobblelen(v)) s_wobble_len = v;
  }
  if (persist_exists(PKEY_LAT) && persist_exists(PKEY_LON)) {
    s_lat_e6 = persist_read_int(PKEY_LAT);
    s_lon_e6 = persist_read_int(PKEY_LON);
    s_have_place = s_lat_e6 >= -90000000 && s_lat_e6 <= 90000000 &&
                   s_lon_e6 >= -180000000 && s_lon_e6 <= 180000000;
  }
  if (persist_exists(PKEY_WRIST)) {
    int v = persist_read_int(PKEY_WRIST);
    if (valid_wrist(v)) s_wrist = v;
  }
  if (persist_exists(PKEY_TILT)) {
    int v = persist_read_int(PKEY_TILT);
    if (valid_tilt(v)) s_tilt = v;
  }
  if (persist_exists(PKEY_FLIP)) {
    s_flip = persist_read_bool(PKEY_FLIP);
  }
  if (persist_exists(PKEY_DATE)) {
    s_date = persist_read_bool(PKEY_DATE);
  }
  if (persist_exists(PKEY_LANG)) {
    int v = persist_read_int(PKEY_LANG);
    if (valid_lang(v)) s_lang = v;
  }
  enforce_contrast();
  refresh_colors();
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;

  if ((t = dict_find(iter, MESSAGE_KEY_COLOR_BG)) != NULL) {
    int v = (int)t->value->int32;
    s_user_bg = GColorFromHEX(v);
    persist_write_int(PKEY_COLOR_BG, v);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_COLOR_FG)) != NULL) {
    int v = (int)t->value->int32;
    s_user_fg = GColorFromHEX(v);
    persist_write_int(PKEY_COLOR_FG, v);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_FONTSIZE)) != NULL) {
    int v = tuple_int(t);
    if (valid_fontsize(v)) {
      s_fontpercent = v;
      persist_write_int(PKEY_FONTSIZE, v);
    }
  }
  if ((t = dict_find(iter, MESSAGE_KEY_NIGHTMODE)) != NULL) {
    s_nightmode = tuple_int(t) != 0;
    persist_write_bool(PKEY_NIGHTMODE, s_nightmode);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ANIMDELAY)) != NULL) {
    int v = tuple_int(t);
    if (valid_animdelay(v)) {
      s_anim_delay = v;
      persist_write_int(PKEY_ANIMDELAY, v);
    }
  }
  if ((t = dict_find(iter, MESSAGE_KEY_WOBBLELEN)) != NULL) {
    int v = tuple_int(t);
    if (valid_wobblelen(v)) {
      s_wobble_len = v;
      persist_write_int(PKEY_WOBBLELEN, v);
    }
  }
  if ((t = dict_find(iter, MESSAGE_KEY_LAT)) != NULL) {
    int v = tuple_int(t);
    if (v >= -90000000 && v <= 90000000) {
      s_lat_e6 = v;
      persist_write_int(PKEY_LAT, v);
      if (persist_exists(PKEY_LON)) s_have_place = true;
    }
  }
  if ((t = dict_find(iter, MESSAGE_KEY_LON)) != NULL) {
    int v = tuple_int(t);
    if (v >= -180000000 && v <= 180000000) {
      s_lon_e6 = v;
      persist_write_int(PKEY_LON, v);
      if (persist_exists(PKEY_LAT)) s_have_place = true;
    }
  }
  if ((t = dict_find(iter, MESSAGE_KEY_WRIST)) != NULL) {
    int v = tuple_int(t);
    if (valid_wrist(v)) {
      s_wrist = v;
      persist_write_int(PKEY_WRIST, v);
    }
  }
  if ((t = dict_find(iter, MESSAGE_KEY_TILT)) != NULL) {
    int v = tuple_int(t);
    if (valid_tilt(v)) {
      s_tilt = v;
      persist_write_int(PKEY_TILT, v);
    }
  }
  if ((t = dict_find(iter, MESSAGE_KEY_FLIP)) != NULL) {
    s_flip = tuple_int(t) != 0;
    persist_write_bool(PKEY_FLIP, s_flip);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_DATE)) != NULL) {
    s_date = tuple_int(t) != 0;
    persist_write_bool(PKEY_DATE, s_date);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_LANG)) != NULL) {
    int v = tuple_int(t);
    if (valid_lang(v)) {
      s_lang = v;
      persist_write_int(PKEY_LANG, v);
    }
  }

  enforce_contrast();
  if (s_have_place) {          // a fresh place: recompute today
    s_sun_day = -1;
  }
  night_update(time(NULL));
  update_time_text();
  layer_mark_dirty(s_canvas);
}

// --------------------------------------------------------------- Einflug
// Appear animation (user): when the face comes to the front, the
// two number rows wobble sideways in opposite directions and
// settle back home (v1.2.0 re-spec). The motion itself is the
// family engine (shared/family.h); the hook below turns the
// engine's offset into the two row offsets render_rotated
// samples with. The offsets only exist while this runs; at rest
// the face renders exactly as before. Delay and length are user
// settings, so the engine is retuned on every start.

static void fly_hook(int offset) {
  s_fly_off_l = offset;
  s_fly_off_r = -offset;
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void start_fly(void) {
  // On app-return the face restarts under the system's exit
  // transition (~300 ms): an immediate flight plays out hidden
  // underneath it (v1.1.0/v1.1.1 — the user saw it only at face
  // start). The configured delay lets the flight begin once the
  // face is actually on screen.
  family_wobble_configure(WOBBLE_AMP, s_anim_delay, s_wobble_len);
  family_wobble_start(fly_hook);
}

static void window_appear(Window *window) { start_fly(); }

// On real hardware the face does NOT get window_appear when an
// app above it closes (it is restored under the system's exit
// transition) — the user only saw the fly-in at face start. The
// focus service is the hook that fires when the face actually
// becomes visible again, so the flight hangs on it too.
static void focus_handler(bool in_focus) {
  if (in_focus) start_fly();
}

static void fly_kick(void *context) { start_fly(); }

// --------------------------------------------------------------- Fenster

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
}

// --------------------------------------------------------------- Start

static void init(void) {
  load_settings();
  night_update(time(NULL));  // sets the effective colours pre-window
  s_digit_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_104));
  s_date_fonts[0] = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_28));
  s_date_fonts[1] = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_22));
  s_date_fonts[2] = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_16));
  s_date_fonts[3] = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_12));
  s_ref_w = graphics_text_layout_get_content_size(
      "00", s_digit_font, GRect(0, 0, FB_MAX_W, FB_MAX_H),
      GTextOverflowModeWordWrap, GTextAlignmentLeft).w;

  update_time_text();

  s_window = window_create();
  window_set_background_color(s_window, s_color_bg);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .appear = window_appear,
    .unload = window_unload,
  });
  window_stack_push(s_window, false);
  app_focus_service_subscribe_handlers((AppFocusHandlers){
      .did_focus = focus_handler});
  app_timer_register(500, fly_kick, NULL);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  app_message_register_inbox_received(inbox_received);
  app_message_open(256, 64);
}

static void deinit(void) {
  app_focus_service_unsubscribe();
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
  fonts_unload_custom_font(s_digit_font);
  for (int i = 0; i < DATE_FONTS; i++) fonts_unload_custom_font(s_date_fonts[i]);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
