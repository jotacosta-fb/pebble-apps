# _music (v1.1.18) — Spotify controller

White background, framed album art, the song name in the footer, and button
icons on the right edge.

## Buttons

- Select = play/pause (icon swaps: pause bars while playing, play triangle
  while paused, from the phone's STATE)
- Up = previous track
- Down = next track
- Back = system default (exits the app)

## Layout (emery, 200×228)

Art 144×144 with a 4 px black frame, song name (Gothic 24 Bold, centered)
as a tight caption 4 px below the frame. The art itself is centered both
ways: frame 24–175 × 38–189, art 28–171 × 42–185, caption 193–221. Icons
14×22, flushed to the rightmost pixel column (186–199), aligned with the
physical buttons at 25/50/75% of screen height: filled < (previous),
filled half circle flat side on the edge (Select), filled > (next). On
short screens (basalt) the caption is dropped and the art alone stays
centered. The frame draws only on color platforms and only once art has
arrived; icons and caption draw everywhere.

No idle exit: a controller stays up while listening. No wobble, no earmarks —
both would fight the artwork.

## How the art gets there

The watch has no internet and no image decoder, so the phone does the work:

1. PKJS polls Spotify's currently-playing endpoint (every 10 s, plus on open
   and after every button press).
2. On a track change it downloads the album art (the ≤320 px copy), decodes
   the JPEG with a bundled pure-JS decoder (jpeg-js), box-averages it down to
   144×144, and quantizes each pixel to Pebble's 64 colors (GColor8: 2 bits
   per channel, opaque alpha).
3. The 20,736 bytes travel to the watch in 21 chunks of 1024 bytes via
   AppMessage, paced by send acknowledgements. The watch draws progressively,
   top to bottom, as chunks land. Nothing playing → the screen clears to black.

The watch side holds the art in the blank bitmap's own buffer
(`gbitmap_create_blank` + `gbitmap_get_data`), so there is no second copy and
no leak. Art is color-platform only (`PBL_COLOR`); on black-and-white watches
the buttons still control playback, the screen just stays black.

## Status

- v1.0.0 builds clean. Host-verified: a real JPEG decodes, downscales and
  quantizes to exactly 20,736 bytes with correct corner colors.
  Emulator-verified: the art square draws exactly centered (28,42)–(171,185)
  with the right channel order.
- v1.0.0 confirmed working on the real watch by the user (2026-11-03):
  login, art, and controls all work.
- v1.1.0: white background, 2 px frame, song-name footer (TRACK key),
  button icons (STATE key drives the Select icon). Emulator-verified with a
  pinned gradient + track name: white corners, frame exactly at (26,22),
  footer text present, all three icons present on the right edge.
- Phone→watch messages: REQ / PLAYPAUSE / NEXT / PREV (watch→phone);
  ART_W / ART_H / ART_IDX / ART_TOTAL / ART_DATA, TRACK (name, ≤127 chars),
  STATE (1 = playing) (phone→watch).
- v1.1.1: Select while nothing is playing starts playback on the PHONE
  specifically (looks up Spotify Connect devices, picks the Smartphone,
  passes its device_id to the play endpoint; falls back to a plain play call
  if no phone is listed). Opening the app alone does not auto-play.
- v1.1.2: 4 px frame, art centered both ways, song name as a tight caption
  below the frame, simplified right-edge bumps: filled <, thick ( arc,
  filled >. The middle mark no longer shows play vs pause. Emulator-verified
  with a pinned gradient + track name (frame exactly 24–175 × 38–189, all
  three icons present).
- v1.1.3: bumps shrunk to 14×22 and flushed to the rightmost pixel column
  (polygon edges at the screen edge, so column 199 actually inks); middle
  mark is now a filled half circle (flat side on the edge, bulge left),
  drawn as a polygon fan with an integer sqrt — graphics_fill_radial drew
  nothing with inset_thickness 0, so it was dropped. Emulator-verified:
  column 199 inked in all three bands, nothing left of x=186.
- v1.1.4: tap pulse — the tapped button grows to 1.5x mid-pulse (sine bump)
  and returns over 350 ms, via the Animation API (file-scope static const
  implementation; animation recreated per tap). Emulator-verified with a
  slowed-down test pulse: only the tapped button grows, others untouched,
  back to baseline after the pulse ends.
- v1.1.5: random art reveal — the phone shuffles the 21 chunks (Fisher-Yates,
  fresh per track) and sends a new ART_START message first; the watch resets
  its buffer to white and shows the frame on START, then paints each band as
  it lands in random order. The old idx==0 reset is gone (a mid-stream idx 0
  must not wipe bands already received). Host-verified message sequence
  (START first, permutation of 0..20, retry keeps the same idx); the on-watch
  reveal itself needs the real watch, as the emulator cannot inject AppMessages.
- v1.1.6: empty state — when nothing is playing, the art layer hides and a
  grey Spotify logo shows instead: dark-grey disc (r=50, centered on the art
  rect) with the three arcs knocked out in white (quadratic-bezier bands,
  bowed up, sloping down to the right, longest on top). Emulator-verified:
  the empty state shows the logo with no art and no frame; START/clear swap
  art and logo visibility.
- v1.1.7: logo redrawn from the official outline (Simple Icons spotify.svg,
  24x24, beziers flattened at 20 segments, scaled 24 -> 100 px, drawn as grey
  disc + white arc polygons). Pixel comparison vs a reference raster of the
  same outline: IoU 0.76 (bezier approximation) -> 0.93; every remaining
  differing pixel sits within 1 px of the boundary (PIL vs Pebble fill-rule
  edge effect), centroids within 0.23 px — the shapes are identical.
- v1.1.8: top/bottom button symbols removed (Up/Down buttons still work, and
  their tap pulses are still wired — just nothing visible to scale). The
  middle mark now shows the phone's STATE: pause bars while playing, the half
  circle while paused. Emulator pin-verified: playing state draws two 5x22
  bars (x 187-191 and 195-199) with zero ink in the top/bottom bands; the
  paused state is the same half circle as 1.1.7.
- v1.1.9: Up/Down press feedback — a small 14x22 rectangle popping into view
  from the left at the pressed button's row (slides in from off the left
  edge to the right-edge slot, lingers a beat, slides back out, 350 ms),
  via the shared `family_pop_rect()` in shared/family.h (pure function of
  animation progress, so the other apps can reuse it for any button-row
  feedback). Emulator-verified with the animation stretched: Up and Down
  rects caught mid-slide at their rows, gone once the animation ends.
  Gotcha fixed during this build: the logo draw leaves the fill color
  white, which made the rect invisible on white — the pop draw now resets
  fill to black first. SUPERSEDED by 1.1.10: the user rejected the
  cross-screen slide — the feedback must stay close to the button.
- v1.1.10: Up/Down feedback redone as a slot-local squeeze (the
  iPhone-volume-button feel): the rect grows from nothing centered on the
  slot, squashes flat at the press moment (1.3x wider, 0.55x as tall at
  peak), and shrinks back to nothing, all in 350 ms, still via the shared
  `family_pop_rect()` (same sine-bump character as the tap pulse, 0..1.5x
  size). Emulator-verified stretched: Up and Down rects caught at their
  slots (12x13 at ~20% progress, x 187-198), nothing mid-screen, gone once
  the animation ends. SUPERSEDED by 1.1.11: the wide-flat squash got the
  button's shape wrong — the rect must stay taller than wide.
- v1.1.11: squeeze axis flipped — the rect now compresses horizontally at
  the press moment (0.55x width, height follows the pulse), so it mirrors
  the tall button: a 14x22 slot peaks at 11x33 and is taller than wide at
  every point. Emulator-verified stretched: Up and Down rects caught at
  8x17 at ~20% progress (x 189-196), nothing mid-screen, gone once the
  animation ends.
- v1.1.12: three changes. (1) Growth is now right-to-left: the rect's
  right edge stays pinned to the screen edge (the button side) and the
  rect grows leftward out of the button, retracting back into it.
  (2) Button-sized slot: 20x56 px (about 2.5x7 mm at 202 DPI), peaking at
  11x56 squeezed — no more 1.5x overshoot, the peak IS the button size.
  (3) Rows are a first example for the user to judge (no published
  Time 2 button pixel positions exist; case 43x36.04 mm, display
  200x228 at 202 DPI, buttons angled not straight): centers at 25%/75%
  of height (57/171), tops 29/143. Also fixed in family_pop_rect: the
  sine peak truncates (sin_lookup tops at RATIO-1, so 10xS/R gave 9) —
  sizes now round, so the peak truly hits slot size (measured 11x56
  after the fix, 10x50 before). Emulator-verified: alignment example
  with both rects at peak, plus a mid-flight catch proving right-edge
  pinning (x 191-199 at ~20% progress); gone once the animation ends.
- v1.1.13: two changes per the user's read of the 1.1.12 example.
  (1) Slide, not grow: family_pop_rect is now a constant-size slide —
  the same 11x56 rect translates right-to-left in from the screen edge
  and back out (x = slot_x + w - w*in/10, sine-bumped so it lingers at
  the slot); no size changes at all. Mid-flight catch proves it: 7 wide
  but full 56 tall (x 193-199, y 14-69). (2) Rows moved toward the
  corners: centers 57/171 -> 42/186 (tops 14/158), approved by the user
  ("perfect", 2026-11-03).
- v1.1.14: refactor, behavior identical: Up/Down now run the shared
  family slide helper (family_slide_start/draw in shared/family.h —
  animation, progress, button, layer, and finish callback all in one
  place, no per-app copies). Select keeps its own tap pulse. The slide
  geometry (family_pop_rect) is unchanged; emulator-verified mid-flight
  Up rect x 194-199, y 14-69, same as 1.1.13.
- v1.1.15: no middle mark anymore. All three buttons animate the same
  way (the shared slide at the pressed button's row — Select included,
  so the tap pulse and its scale code are gone). The paused state now
  shows as black pause bars centered over the album art, on a topmost
  transparent layer; nothing shows while playing or when no art is
  displayed. The half-circle fan moved to shared/family.h as
  family_half_circle (now _battery's center mark); the emulator-forced
  overlay preview measures bars x 82-93 / 106-117, y 88-139 on emery.
- NOT yet built: the Spotify login. It needs a Spotify developer app (client
  ID) plus a small config page doing the PKCE flow; tokens then live in
  PKJS localStorage. The placeholders are `CLIENT_ID` and `CONFIG_URL` at the
  top of `src/pkjs/index.js`.
- Requires Spotify Premium: playback control endpoints refuse free accounts.
