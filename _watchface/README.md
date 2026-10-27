# _watchface (fork)

A fork of Lean 1.4.0 (MIT, by Axel Kark / Kieselleger) for the
_app family, renamed **_watchface**. Two changes vs. upstream:

1. **Day/night awareness, the low-battery way.** The phone sends
   the location once (persisted on the watch); the watch then
   computes sunrise and sunset itself in integer fixed point
   (NOAA approximation, within a few minutes), once a day. The
   minute tick only compares the time against the two stored
   instants — no radio, no GPS, no extra wakeups at runtime.
   While night mode is on (a settings toggle), the face draws
   white on black after sunset; the user's colours return at
   sunrise.
2. **Settings:** a new *Display* section adds a **Font size**
   slider (60–140% in steps of 2; 100% is the upstream size —
   past 100% the digits grow only as far as the tilted block
   still fits) and the Night mode toggle. The colour settings
   are unchanged.

The original README follows.

---

# Lean

*The time meets your eye at the angle your wrist comes to rest.*

<img src="screenshots/shot_emery_mono.png" alt="shot_emery_mono.png"> <img src="screenshots/shot_emery_amber.png" alt="shot_emery_amber.png"> <img src="screenshots/shot_emery_mint.png" alt="shot_emery_mint.png"> <img src="screenshots/shot_gabbro_mono.png" alt="shot_gabbro_mono.png"> <img src="screenshots/shot_gabbro_teal.png" alt="shot_gabbro_teal.png"> <img src="screenshots/shot_gabbro_orange.png" alt="shot_gabbro_orange.png">

A Pebble watchface (v1.4.16).

Every watch assumes you can turn your arm until the face lies straight in
front of you. Not everyone can. Lean turns the time instead of your arm:
raise your wrist the way you normally would, see how far the numbers are
off, and set that angle. From then on the time sits level when you glance
at it.

Worn on the left wrist the time turns clockwise, worn on the right it turns
counter-clockwise, anything from 0 to 90 degrees. One switch adds another
180 degrees for wearing the watch the other way round.

An optional date - weekday and day - turns along with the time and goes
wherever the most space is left around the turned digits.

The time is set in LECO 1976, two lines, hours above minutes, drawn far
larger than the built-in version allows. There is no animation; the face is
drawn once a minute and then left alone.

## How the turning works

The Pebble SDK cannot draw rotated text. Lean draws the time upright as
usual, reads the digit pixels from the frame buffer into a 1-bit mask
(clearing them on the way), and writes them back rotated about the centre of
the screen. Each target pixel samples the mask at four points (2x2
supersampling) and is blended between digit and background colour, so the
edges stay smooth at any angle. On the rectangular screen the block is
scaled down if its turned corners would otherwise touch the edge.

## Where the date goes

The date is drawn upright into a second mask and placed in the reading frame
of the time, the coordinate system in which the digits stand upright. There
the screen edge is a tilted rectangle (or a circle) and the time block an
obstacle. A grid search with a pixel-exact refinement finds the spot with the
largest distance to both, for three arrangements: side by side, stacked, and
split (weekday and day placed separately). Splitting only wins if it leaves
noticeably more room. If nothing fits at 28 pt, the search repeats at 22 pt.
The result is cached and only recomputed when the angle, the date or the
settings change.

## Settings

- Worn on - left wrist (clockwise) or right wrist (counter-clockwise).
- Tilt - 0 to 90 degrees in steps of 5.
- Worn upside down - adds 180 degrees. Leave it off if your watch already
  turns the whole display.
- Show the date - weekday and day, off by default.
- Weekday language - the watch language, or English, German, French,
  Spanish, Italian, Portuguese, Dutch.
- Background - the face colour.
- Digits - the colour of the numerals.

12 or 24 hour format follows the watch setting.

## Platforms

- Pebble Time 2 (`emery`)
- Pebble Round 2 (`gabbro`)

## Building

With the [Pebble SDK](https://developer.repebble.com/sdk/):

```bash
pebble build
pebble install --emulator emery
```

The repository can also be imported into CloudPebble as is.

## Release notes

### 1.4.16

Earmark radius hand-tuned down once more: 35 px. (The
measured glass curve is 68 px, the concentric value 44 —
both read slightly large on the real watch.)


### 1.4.15

Earmark radius settled at the display-concentric value:
44 px (the 68 px straight glass copy from 1.4.14 read too
big on screen; 44 px is the glass curve offset inward by
the ~3 mm bezel inset).


### 1.4.14

The earmarks' curvature is now the watch's own: the fillet
radius is 68 px — the Pebble Time 2 glass corner radius
(8.5 mm, measured from Core Devices' official CAD solid
model) at the display's 202 DPI. (A display-concentric 44 px
was the alternative; the user picked the true glass curve.)


### 1.4.13

Correction of a conflation: the corner earmarks have nothing
to do with Quiet Time — they are corner dressing that appears
whenever the background is light (the helper no longer checks
Quiet Time at all, and is renamed family_earmarks). Quiet
Time's own mark on this face remains the two small lines.


### 1.4.12

The Quiet Time earmarks follow a simpler rule now (the user's):
they draw when the background is light, and not when it is
dark — whatever the mode or custom colors say. The helper
takes the caller's background color and tests its lightness,
replacing the night-flag gates entirely.


### 1.4.11

Bug fix: the Quiet Time earmarks showed at night after all.
The shared helper's night gate read the family night state,
which the watchface never drives — the face keeps its own
(s_night_active, from its on-watch solar math). The helper now
has an _ex form taking the caller's night flag, and the face
passes its own. A/B-verified in the emulator: night pinned →
no marks, day pinned → marks. (_weather was unaffected; its
family night state is real.)


### 1.4.10

The Quiet Time line now also appears at the top center,
mirroring the bottom one — the user loved the bottom line and
asked for its twin.


### 1.4.9

The Quiet Time dot between the digit rows is gone (the user
didn't like it). In its place, Quiet Time draws a thin line at
the bottom center of the screen — 40 px wide, 2 px tall, at
half ink strength — deliberately very subtle.


### 1.4.8

Three Quiet Time refinements: the earmarks no longer draw at
night (the shared helper now also checks the family night
state); the date's dark gray — introduced as the Quiet Time
color — is now its permanent color; and while Quiet Time is
active, a dot sits on the seam between the hour and minute
rows, drawn into the time mask so it leans and wobbles with
the digits.


### 1.4.7

The Quiet Time earmarks now sit in all four corners, drawn by a
shared helper in family.h (`family_qt_earmarks`) that every app
in the family calls. And while Quiet Time is active, the date
renders in dark gray instead of the digit color — the renderer
now counts date ink separately from time ink to color it.


### 1.4.6

The Quiet Time earmark's curvature is flipped: instead of a
quarter disc bulging from the corner, it is now a concave
fillet — full width along both edges, thinning along a circular
arc — so it hugs the corner.


### 1.4.5

The Quiet Time earmark is curved now — a quarter disc (radius
24 px) around the corner instead of a straight-edged triangle,
so it melts into the corner — and it appears in both the
top-right and the bottom-left corners.


### 1.4.4

The Quiet Time corner triangle is larger (24 px legs, up from 15)
and is now filled row by row: the path-based fill dropped the
rightmost pixel column at the screen edge, which the row fill
reaches exactly — pixel-verified in the emulator.


### 1.4.3

The date no longer disappears when the time is set large (e.g. font
size 114 %): if the weekday/day pair fits nowhere at the two large
date sizes, it now steps down through two smaller ones (16 and 12)
until it fits — smaller, but present and together.

Also: when Quiet Time is active, a small filled triangle sits in
the top-right corner of the screen, so the state is visible at a
glance.

### 1.4.2

The date's weekday and day of the month now always stay together —
side by side or stacked as one block. The old third arrangement,
which placed each half in its own corner when that scored better,
is gone; when the pair doesn't fit together at the large date
size, the smaller date font is used instead.


### 1.4.0

Optional date: weekday and day, turned with the time and placed in the
largest free space around it. The weekday follows the watch language, or the
one you pick.

### 1.0.0

First release.

## License

MIT License, see [LICENSE](LICENSE). The bundled fonts are not covered by it and
remain under their own licenses.
