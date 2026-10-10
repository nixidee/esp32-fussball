# UI — screens, navigation and overlays

All four football screens, navigation, overlays and night rendering are
implemented in 0.1.0-dev. Current C6 source compiles. Font/layout polish,
rendered boundary checks and physical input/display acceptance remain open.
The earlier boot-screen and geometry acceptance covers that earlier workload.

## Screens and automatic selection

| Screen | Content |
|---|---|
| Own match | principal score, supplied/estimated minute or kickoff, teams, separate shootout tally, bounded recent events |
| Conference | tracked club centred with a larger row; up to five fixtures of the selected competition, sorted by recent score change |
| Table | up to nine rows around the club, shifted at edges; all 32 accepted rows reachable by scrolling |
| Crest / slideshow | uploaded crest or stock image; up to five uploaded slides rotate within this screen |

Each screen has an enable switch, colour/text/accent, background switch and
text-scale setting. No view fetches data or reads GPIO. The controller copies
canonical data into a fixed view model, then the view renders it.

| Situation | Default |
|---|---|
| No configured competition | stock image |
| Outside relevant windows | Table |
| Relevant matchday | Conference |
| Own fixture window | Own match |

Defaults are configurable. Disabled or unavailable screens are skipped.
With zero inputs the current situation's default remains selected; there is
no automatic cycling between screens. Manual navigation returns after
60 seconds without input by default; 0 disables the return.

Round windows span the first through last relevant fixture, including gaps.
Own cup/catch-up windows are independent. A conference chooses the active
own fixture's competition, otherwise the first active configured competition.
Its bounded nearby fixtures and own fixture stay together. Exceptional/stale
limits are documented in [DATA_MODEL.md](DATA_MODEL.md); a missing final
status cannot leave a window open indefinitely.

## Presenter and event display

Missing values use `--` rather than fabricated zero. Supplied minutes are plain;
estimates show `~`. Half-time, full-time and shootout have explicit labels.
A separate shootout tally does not change ordinary goals. Kickoff is converted
to selected local civil time; retries and freshness use monotonic time.

The event pool retains up to 12 records per match and 32 across the snapshot.
The Own match highlight band rotates through that bounded list every five
seconds. It displays the event type and player; supplied substitution in/out
names can appear together. A one-line rotating band keeps every retained
highlight accessible without a larger object tree. Removed records are
excluded; truncation and reconciliation behaviour is defined in the model.

Conference uses recent principal-value changes, rather than repeated initial
fetches or source-tag changes. The table defaults to five rows around the own
club and clamps scrolling at either end. Long names use UTF-8-safe prefixes and
LVGL ellipsis. Empty fixture/table, no configuration, no WiFi, no valid time,
provider error, stale data and demo status have explicit text.

## Inputs

The target declares pin order and active level in its hardware profile.
The current C6 has three TTP223B digital inputs (see [HARDWARE.md](HARDWARE.md)).

```
digital levels → debounce / duration → raw short, long, double, release, repeat
              → input-count mapping → semantic action → navigation
```

Pure `InputPolicy` uses 40 ms debounce, 600 ms long press and 300 ms double
press. Repetition defaults to 250 ms and is configurable. Screen input short
press waits for the double-press window; scroll inputs do not need that delay.

| Inputs | Behaviour |
|---|---|
| 3 | first: short next, double previous, long default; second up; third down, including hold/repeat |
| 2 | first selects screens; second hold scrolls, every release flips direction; short tap only flips |
| 1 | short next, double previous, long default |
| 0 | current automatic default only |

Two-input scroll direction starts down and resets after 20 seconds without
scroll activity; 0 disables that reset. Every accepted row remains reachable.

Actions cross the event bus and a four-entry static controller queue. Callbacks
do not block or call LVGL. Overflow is counted/logged and drops excess input
rather than accumulating stale actions. Touch screens and encoders remain
future hardware input types.

Holding all three current C6 inputs for eight seconds shows a countdown and
resets settings/restarts. Release cancels it; it does not erase images.
The gesture is unavailable during an OTA trial.

## SafeArea and scaling

All positions derive from the profile and `geometry::SafeArea`. At a round
display row, width comes from the circle chord at both vertical band edges;
the smaller span is used. The content margin scales with the shorter side
(default /60, or four pixels at 240 pixels). Rectangular displays use the
inset rectangle. No display resolution or pin appears in a view.

The existing integer SafeArea checks the complete band, including odd and
non-square profiles. Text bands use fixed relative placement and bounded
font roles. Background/clipping containers can cover the framebuffer; they
are not readable-content boundary failures. Decoration is checked against its
visible mask. The accepted diagnostic boot rings keep their explicit edge
exception and remain the initial bring-up screen.

The product keeps one persistent LVGL object tree: one image and 16 text
objects, reused across screens. Unchanged text is not rewritten. Image source
revision forces reopening after a replacement. No full-image cache or new
framebuffer is added.

## Fonts and readability

Montserrat weight 500 is included under SIL Open Font License 1.1.
The pinned source and regeneration instructions are in `assets/fonts/`.
Committed 4-bit glyph subsets provide Latin text at 10, 14 and 18 pixels and
a number-only score role at 28 pixels. Text subsets contain 228 glyphs,
including German umlauts/ß and common Latin player-name accents; the score
subset has 14 characters. Unsupported glyphs use `?`.

Default body role is 14 for the small class (shorter side ≤260), otherwise 18.
Medium and large currently share the 18 role; geometry still scales separately.
Text scale selects among compiled roles and clamps to the available band;
it does not rasterize arbitrary font sizes on the device. Large-profile font
polish and actual names/boundary acceptance are still required. Bitmap payloads
are 5938/10960/17846/1969 bytes; descriptors/maps add flash beyond those numbers.

## Overlays and night mode

The IP badge is above screen content, top or bottom by setting. Setup shows
SSID and 192.168.4.1 and is permanent by default. Station IP is shown for
60 seconds after successful connection, including recovery. Duration 0 means
permanent. Reset countdown has higher priority and replaces the IP badge.
Each complete two-line band is fitted to the SafeArea chord.

Night mode defaults to 23:00–07:00 local civil time and waits for valid time.
The existing time service handles midnight and daylight-saving transitions.
Night rendering darkens the background and image opacity. A profile with a
backlight pin additionally uses PWM brightness; the current C6 has no such pin,
so its physical backlight remains on. S3 dimming/network behaviour is untested.

## Acceptance boundary

C6 firmware build and fixed sizes are verified. Visual alignment, real names,
text scaling, overlay overlap, all input counts, bottom-row reachability,
JPEG redraw responsiveness, night brightness and the full rendered safe-area
matrix remain polish/testing. No screenshot is represented as device evidence,
and the boundary harness has not been implemented/run for these new screens.
