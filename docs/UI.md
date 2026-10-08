# UI — screens, navigation, overlays

> Status: **planned** (P5/P6). View sizes and matchday window confirmed
> 2026-10-08; final values after the 240 px layout test.

## Screens
| Screen | Content | Notes |
|---|---|---|
| **LiveSingle** | Match minute (top), score very large (centre), team names below, recent highlights below that | Highlights depend on provider capabilities (goals only with OpenLigaDB) |
| **LiveMulti** (conference) | Own match centred and larger, other matches above and below | Max matches N (default 5), sorted by most recent score change |
| **Table** | Rows around the own club, window shifted at table edges | Window size default 5; side buttons scroll |
| **Crest / Slideshow** | Club crest (default) or up to 5 user images | Images change within this screen |

Each screen: separate background image (optional) and colours, configured in
the Web UI (Screens tab).

## Screen-mode resolver (pure logic, P6.1)
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

Matchday window: from 30 min before the first relevant kickoff
until 30 min after the last relevant match has finished; relevant = the own
club's competitions; both times configurable.

**No club / league configured** (no `secrets.h` values, nothing set in the
Web UI): the device shows a default image instead of football data
(decided 2026-10-08).

### Manual override and return to default
With inputs the user can switch away from the default screen. After a
configurable time without input (default 60 s) the device returns to the
default screen; the user can disable the return. Applies to every input
configuration (1, 2 or 3 inputs; decided 2026-10-08). With 0 inputs the device
always shows the default screen.

## Input and navigation (P5.4)
```
Drivers (digital inputs now: buttons or touch modules; touch screen, encoder later)
  → raw events (short press, long press, double press; gestures later)
  → InputMapper (by device input config + settings)
  → UiAction: NEXT_SCREEN, PREV_SCREEN, SCROLL_UP, SCROLL_DOWN, SELECT, BACK
  → NavigationController (screen list from resolver, scroll state)
```
Prepared raw events per input: short press, long press, double press
(more combinations only if needed).

| Input config | Mapping |
|---|---|
| 3 inputs (C6: Touch 1/2/3 = D8/D9/D10) | Touch 1: NEXT_SCREEN; Touch 2: SCROLL_UP; Touch 3: SCROLL_DOWN |
| 2 inputs | input 1: NEXT_SCREEN; input 2 scrolls, see “Scrolling with one scroll input” |
| 1 input (no such target planned) | short: NEXT_SCREEN (manual override, see above); long: defined when a 1-input target exists |
| 0 inputs (e.g. Waveshare: BOOT button not reachable in the housing) | no navigation; default screen of the current situation |
| touch screen (later) | swipe left/right: screens; swipe up/down: scroll; tap: SELECT |

Views never read inputs directly.

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
  (chosen and tried on the device in P5.4), changeable in the Web UI.

## Round display rules
- Circle centre `(cx, cy)`, radius `r` (120 for 240×240).
- Usable width at row `y`: `w(y) = 2·√(r² − (y − cy)²)`, minus margin.
- Safe margin: proposal 4–6 px (decided with harness in P5.6).
- Layout uses rows: each text row gets its width from the chord at its top
  and bottom edge (the smaller one).
- Little space: avoid long names; use short names/codes depending on space.
- **Round-boundary test** (P5.6): every screen × every round profile; fails
  if anything renders outside the safe circle.

## Scaling
- Size classes by shorter side: S ≤ 260 px, M ≤ 400 px, L > 400 px
  (proposal). Fonts and spacing come from tokens per size class.
- No absolute pixel positions in views; relative to SafeArea.
- Rectangular displays use the same tokens with shape = rect.

## Overlays (P5.5)
Overlay manager: layered above screens, priorities, timeouts, placement
inside the round area via chord width.

### IP badge (first overlay)
| Property | Value |
|---|---|
| Look | small rounded box, slight transparency, IP address (AP mode: SSID + IP) |
| Position | top or bottom (setting); placed at the row where its width fits the chord |
| STA mode default | shown for 60 s after boot/restart, then hidden; range 1–4000 s, or permanent |
| AP mode default | permanent; user can disable “permanent in AP mode” and set 60–4000 s |

Values live in `include/app_config.h` (defaults, limits) — not in views.

Later overlays (backlog): goal popup, error/status hints.

## Night mode (P6.8)
Decided 2026-10-08. Active in a configurable night window (default
23:00–07:00), only when the time is valid (SNTP). Dark UI on every target;
targets with a backlight pin additionally dim or switch off the backlight. The XIAO C6
display module has no backlight pin, so there the backlight stays on.
