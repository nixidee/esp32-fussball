# UI — screens, navigation, overlays

> Status: football screens, navigation and overlays are **planned**. The
> implemented C6 bring-up screen shows diagnostic rings, target/display text
> and firmware version; its three digital inputs currently log level changes.
> View sizes and the normal matchday window were confirmed 2026-10-08; final
> layout values still need the 240 px layout test.

## Screens
| Screen | Content | Notes |
|---|---|---|
| **LiveSingle** | Match minute (top), score very large (centre), team names below, recent highlights below that | Highlights depend on provider capabilities (goals only with OpenLigaDB) |
| **LiveMulti** (conference) | Own match centred and larger, other matches above and below | Max matches N (default 5), sorted by most recent score change |
| **Table** | Rows around the own club, window shifted at table edges | Window size default 5; side buttons scroll |
| **Crest / Slideshow** | Club crest (default) or up to 5 user images | Images change within this screen |

Each screen: separate background image (optional) and colours, configured in
the Web UI (Screens tab).

## Screen-mode resolver (pure logic, planned)
Inputs: current time, matchday window state, own match state, relevant
matches (leagues and cups), settings, number of inputs.
There is **no timed cycling between screens** (decided 2026-10-08). The device
always shows the default screen of the current situation; only the slideshow
changes images within its own screen.

| Situation | User choice (setting) | Default |
|---|---|---|
| No relevant matchday | Table, Slideshow (images) | **Table** |
| Matchday of a relevant competition (league round, DFB-Pokal), own club not playing | LiveMulti (other matches), Table, Slideshow | **LiveMulti** |
| Own club plays | LiveSingle (own match), LiveMulti (own match + others), Table, Slideshow (e.g. only the crest) | **LiveSingle** |

Normal matchday window: from 30 min before the first relevant kickoff
until 30 min after the last relevant match has finished; relevant = the own
club's competitions; both times configurable. Exceptional fixtures use the
bounded date horizon and state policy specified in [DATA_MODEL.md](DATA_MODEL.md):
earlier-round catch-up matches, delays, postponements, abandoned matches,
missing final states and season transitions need explicit handling before
resolver implementation. A missing final state must not keep the window open
indefinitely or be replaced with an invented final result. This exceptional
policy remains to be finalized; it does not change the normal window.

**No club / league configured** (no `secrets.h` values, nothing set in the
Web UI): the device shows a default image instead of football data
(decided 2026-10-08).

### Manual override and return to default
With inputs the user can switch away from the default screen. After a
configurable time without input (default 60 s) the device returns to the
default screen; the user can disable the return. Applies to every input
configuration (1, 2 or 3 inputs; decided 2026-10-08). With 0 inputs the device
always shows the default screen.

## Input and navigation (planned)
```
Drivers (digital inputs now: buttons or touch modules; touch screen, encoder later)
  → raw events (short press, long press, double press; gestures later)
  → InputMapper (by device input config + settings)
  → UiAction: NEXT_SCREEN, PREV_SCREEN, SCROLL_UP, SCROLL_DOWN, SELECT, BACK
  → NavigationController (screen list from resolver, scroll state)
```
Planned raw events per input: short press, long press, double press
(more combinations only if needed).

| Input config | Mapping |
|---|---|
| 3 inputs | input 1: NEXT_SCREEN; input 2: SCROLL_UP; input 3: SCROLL_DOWN |
| 2 inputs | input 1: NEXT_SCREEN; input 2 scrolls, see “Scrolling with one scroll input” |
| 1 input (no such target planned) | short: NEXT_SCREEN (manual override, see above); long: defined when a 1-input target exists |
| 0 inputs (e.g. Waveshare: BOOT button not reachable in the housing) | no navigation; default screen of the current situation |
| touch screen (later) | swipe left/right: screens; swipe up/down: scroll; tap: SELECT |

Views never read inputs directly.
The target's input order and wiring are documented in [HARDWARE.md](HARDWARE.md).

### Scrolling with one scroll input (2-input config)
Decided 2026-10-08.
- When a screen is entered, the scroll direction is **down**.
- **Holding** the scroll input (long press) scrolls in the current direction.
  A short tap does not scroll.
- **Every release flips the direction** — after a hold and after a short
  tap. So: hold = down, release, hold = up, release, hold = down, …
- To scroll up first, tap once briefly, release, then hold.
- After a configurable time without input (default 20 s) the direction
  resets to down, as if the screen were new.
- No double press is needed for this.
- While held, scrolling runs **continuously at a fixed rate** (decided
  2026-10-08). The rate is a setting: sensible default in `app_config.h`
  (chosen and tried on the device when input navigation is implemented),
  changeable in the Web UI.

## Round display rules
- Circle centre `(cx, cy)`, radius `r` (120 for 240×240).
- Content radius is `r − margin`; usable width at row `y` is
  `w(y) = 2·√((r − margin)² − (y − cy)²)` where the radicand is nonnegative.
- Safe margin scales with the shorter display side using the default in
  `app_config.h` (currently `/60`: 4 px at 240 px). The rule is applied in one
  place, `geometry::SafeArea::forDisplay(display profile, divisor)`; views take
  the display size from that SafeArea, not from a second source.
- Layout uses rows: each text row gets its width from the chord at its top
  and bottom edge (the smaller one).
- Implemented SafeArea uses integer outer pixel boundaries and checks all
  corners over the complete content height. Odd and non-square profiles are
  supported without floating-point arithmetic, heap allocation or a lookup
  table. Rectangular profiles use the inset rectangle.
- The boot view preserves its preferred text width when it fits. After LVGL
  layout/wrapping it reduces width to fit the complete measured text band.
  If no complete text layout fits, it keeps diagnostics and reports failure
  instead of silently truncating text. Native geometry checks do not replace
  visual device or rendered-pixel acceptance.
- Little space: avoid long names; use short names/codes depending on space.
- **Element classes:** readable/interactive content must fit the geometric
  safe area. Backgrounds and clipping containers may cover the full rectangular
  framebuffer; their rectangular bounding boxes are not content failures.
  Decoration is checked against its intended visible mask. Diagnostics have
  explicit test contracts: the accepted boot screen's red edge ring and green
  inner ring remain, including the edge ring's intentional use of the boundary.
- **Round-boundary acceptance:** test every screen on every round profile.
  Geometric tests check content at its full vertical extent; rendered pixel-mask
  tests check visibility and clipping, including decoration. Root/background/ring
  boxes are not required to fit wholly inside the safe circle. Correct the test
  contract instead of removing the accepted diagnostic rings to make it pass.

## Scaling
- Size classes by shorter side: S ≤ 260 px, M ≤ 400 px, L > 400 px
  (proposal). Fonts and spacing come from tokens per size class.
- No absolute pixel positions in views; relative to SafeArea.
- Rectangular displays use the same tokens with shape = rect.
- New resolutions need their own rendering, DMA-buffer and asset budgets;
  scaling geometry alone does not establish resource feasibility.

## Text and fonts (planned)
Production font roles and glyph subsets must be selected before football-screen
acceptance. The current built-in Montserrat 14 boot font does not establish
support for German umlauts or all names. Test real club/player names, umlauts,
long names, UTF-8 truncation at character boundaries and the chosen fallback
glyphs. Presenters select the bounded text/short-name fallback; views render it.
Measure actual font flash growth and text/LVGL memory peaks for each role and
size class before accepting the font set.

## Overlays (planned)
Overlay manager: layered above screens, priorities, timeouts, placement
inside the round area via chord width.

### IP badge (first overlay)
| Property | Value |
|---|---|
| Look | small rounded box, slight transparency, IP address (AP mode: SSID + IP) |
| Position | top or bottom (setting); placed at the row where its width fits the chord |
| STA mode default | shown for 60 s after boot/restart, then hidden; range 1–4000 s, or permanent |
| AP mode default | permanent; user can disable “permanent in AP mode” and set 60–4000 s |

These planned defaults and limits belong in `include/app_config.h` when the
settings schema is implemented, not in views.

Later overlays (backlog): goal popup, error/status hints.

## Night mode (planned)
Decided 2026-10-08. Active in a configurable night window (default
23:00–07:00), only when the time service reports a valid clock. The clock
starts invalid on every boot; SNTP will provide normal synchronisation with
the WiFi manager. The local civil window calculation is implemented and
offline device-tested, including midnight and both daylight-saving changes;
night-mode rendering and its runtime settings remain planned. Dark UI on
every target; targets with a backlight pin additionally dim or switch off
the backlight. The XIAO C6 display module has no backlight pin, so there the
backlight stays on.
