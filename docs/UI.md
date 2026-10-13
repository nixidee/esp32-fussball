# UI — screens, navigation and overlays

The start screen, all four football screens, the state cards, navigation,
the IP chip and night rendering follow the approved display design in
0.1.0-dev. Current C6 source compiles. Rendered boundary checks and physical
input/display acceptance remain open. The earlier boot-screen and geometry
acceptance covers that earlier workload.

## Screens and automatic selection

| Screen | Content |
|---|---|
| Own match | principal score, supplied/estimated minute or kickoff, teams, separate shootout tally, bounded recent events |
| Conference | matches of the current kickoff blocks of every active competition; up to the configured rows per page (profile maximum four on 240 round), the own match included; mode All pages or One page; own match pinned as the first row or shown as a normal accent row |
| Table | profile-limited rows (seven maximum on 240 round) around the club, shifted at edges; all 32 accepted rows reachable by scrolling |
| Crest / slideshow | the crest role's image (replacement or factory crest); up to five slides (the crest or library images) rotate within this screen |

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

The relevant-matchday default applies while the Konferenz has at least one
member; membership follows kickoff blocks of every active competition,
including overruns (rules in [DATA_MODEL.md](DATA_MODEL.md#72-konferenz)). Own cup/catch-up
windows are independent. A missing final status cannot leave a window open
indefinitely.

### Konferenz pages

- **All pages** (default): every member in kickoff order. With more members
  than rows, pages change every 8 seconds; the title shows "Konferenz · n/m"
  and page dots replace the status line.
- **One page**: a single page. Running matches with goals come first, the
  latest goal first; then goalless running matches, finished matches and
  matches not yet started, each in kickoff order.
- **Pin own match** (default on): the own match is the first row of every
  page while it is a member and counts towards the rows per page. Off: it is
  a normal row in kickoff or activity order, drawn in the accent colour.
- **Show own match for the whole matchday** (default off): see
  [DATA_MODEL.md](DATA_MODEL.md#72-konferenz).
- Up/Down move one page in kickoff order, also in One page mode, and stop
  automatic paging until the manual return; with no Up/Down or Scroll input
  there is no manual paging.
- Rows keep fixed slots while paging, so a shorter last page does not move
  the rows up.

## Layout rules

Every position is derived from the shorter display side `s` and the
SafeArea; nothing is placed at a fixed pixel position.

- **Size class**: small up to 260 px shorter side, medium up to 400 px,
  large above. Each class has font role sizes for caption, body, title and
  score. The screen's text scale multiplies them; the largest compiled font
  at or below the result is used (caption font as the minimum).
- **Gap unit** `g` = max(2, s/60); every spacing is a multiple of it. The
  rim stroke is max(2, s/80) px.
- **Fitting**: a text line takes the chord of its band on a round panel (the
  full inset width on a rectangle). A line with a font list uses the first
  font that fits; if none fits, the text is shortened with "…" in the last
  font. A line that still leaves the SafeArea is drawn, but the render is
  reported as failed (UI health), so the overflow is visible in the status.
- **Content blocks** are centred vertically as a whole; the status band sits
  directly below the block, the caption band directly above.

### Screens in detail

- **Own match**: caption "competition · round" (dim; without a round label
  "N. Spieltag"), home name, centre, away name, a status pill, the
  highlight line and the status band. The centre is the score in the 48 px
  score font, before kickoff the local kickoff time. The pill shows the
  minute ("67'", estimated "~67'"), "Halbzeit", "Elfmeterschießen",
  "Beendet", or the shootout tally ("i.E. 4 : 2"), dim once finished;
  before kickoff it is a dim date pill ("Sa 01.11.")
  and the highlight line counts down ("Anstoß in 3 Tagen"). The own club's
  name is in the accent colour. The highlight line rotates through the
  retained events ("67' Tor  Name").
- **Konferenz**: caption "Konferenz · n/m" in the accent colour, then one
  row per match: home name right-aligned, centre, away name left-aligned.
  All names of a page share one font (body, or caption when a name does not
  fit). The centre is the score, the kickoff time (dim) or "--:--". The own
  match sits on a dark row, the own club's name in the accent colour; for
  120 seconds after a goal a row is filled with the accent colour. Below
  the rows: page dots while paging, else the status band. Without members
  a card "Konferenz / Keine Spiele im Zeitfenster" names the next kickoff.
- **Table**: a title row ("#", competition name, Pkt/Diff/Sp) and the rows
  around the own club; the own club's row is filled with the accent
  colour. Columns are dropped from the right in the order played,
  difference, points while fewer than ten letters of the name would fit;
  column titles are shown only when the whole title row fits the chord,
  otherwise only the competition name. Up to five rows use a wider row
  pitch. A competition without a table shows a card.
- **Crest / slideshow**: the image, page dots near the bottom rim while
  several slides rotate, and the status band above them; a bottom IP chip
  ends on the status line, so it never covers the dots.

### Start screen

From boot the device shows a start card: the club's display name as the
title ("Fussball" while no club name is stored), the running step as the
line ("Verbinde WLAN …", "Hole Uhrzeit …", "Lade Daten …") and as the hint
the firmware version, while loading data the name of the first active
competition's provider. It uses the crest screen's colours, without an image
or IP chip. The rim shows the three steps WLAN, time and data as segments:
finished steps in the accent colour, the running one half-tinted, the rest
as track.

The start screen ends for good when the first data arrive, a step fails (the
WLAN gives up its current attempt, or the provider answers with an error),
an input is pressed, the setup network is on, the demo is on or no active
competition is configured. The state cards below then explain a failure.
The first frame is drawn right after the display starts.

### Rim (round panels)

Rectangular panels draw no rim.

- **Own match**: a track ring and a progress arc of 4° per minute (90
  minutes = full circle), dim once finished; extra time as a second inner
  lap of 12° per minute; a small gap at the bottom marks half time.
- **Konferenz**: the same progress for the own match while it is a member,
  capped at 90 minutes.
- **Table**: a scroll indicator on the right (40°–140°); the accent part
  marks the visible rows among all rows.
- **Start screen**: three segments of 112° (WLAN, time, data) with small
  gaps, filled as the steps finish.
- **State cards**: a full ring in the card's colour.

### State cards

A card replaces a screen's content with a title, up to two lines and two
hints, and a coloured ring:

| Situation | Card | Ring |
|---|---|---|
| Not configured, setup network on | "Einrichtung", "Mit WLAN verbinden:", "» network name", "dann im Browser öffnen:", setup IP | accent |
| No data loaded yet, no WiFi | "Kein WLAN" with the reason: "Netz nicht gefunden" (saved network not seen), "Passwort falsch?" (authentication retry), "Setup-Netz aktiv" (setup network only) or "Verbinde …" (scanning, connecting or retry); hint "Neuer Versuch läuft" (not with the setup network only) and "Setup-Netz aktiv" while the setup network runs alongside | warning |
| No data loaded yet, no valid time | "Warte auf Uhrzeit", "Zeitserver wird gefragt" | warning |
| No data loaded yet | "Keine Daten" with the provider error and "Neuer Versuch HH:MM" (next attempt), or "Lade Daten" before the first answer | error / neutral |
| Own match screen without an own match | "Eigenes Spiel", "Kein Spiel" | neutral |

The setup card already names the setup network and IP, so no IP chip is
shown with it. A device without configuration and without the setup
network shows the crest screen.

### Status band

One caption line below the content, by priority: "Demo" (dim), "Kein WLAN",
"Warte auf Uhrzeit", stale data as "Stand HH:MM" (time of the oldest
data shown: a competition whose fetch failed keeps its earlier data; "Daten
veraltet" without any) — these three in the
warning colour — and finally a provider error in the error colour.
Otherwise the band is empty. The Konferenz shows page dots instead while it
pages.

### Drawing

One full-screen draw object paints every shape and text from a fixed list of
72 items with a 1,280-byte text store. A render compares the new list with
the previous one and invalidates only the boxes that changed; a growing
progress arc invalidates only its new part. The image behind the content is
therefore redrawn stripe by stripe, never as a whole. Shapes are drawn
first, all texts above them. A list or store overflow is reported as a
render failure.

## Presenter and event display

Missing values use `--` rather than fabricated zero. Supplied minutes are plain;
estimates show `~`. Half-time, full-time and shootout have explicit labels.
A separate shootout tally does not change ordinary goals. Kickoff is converted
to selected local civil time; retries and freshness use monotonic time.

The event pool retains up to 12 records per match and 32 across the snapshot.
The Own match highlight band rotates through that bounded list every five
seconds. It displays the event type and player; supplied substitution in/out
names appear as "out > in", a substitution without direction (ESPN) as
"A / B". A one-line rotating band keeps every retained
highlight accessible without a larger object tree. Removed records are
excluded; truncation and reconciliation behaviour is defined in the model.

The One page order uses recent principal-value changes, rather than repeated
initial fetches or source-tag changes; a change not seen since the last
restart sorts after seen ones. The table defaults to five rows around the own
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
press. Repetition defaults to 250 ms and is configurable. The mandatory
input's short press waits for the double-press window; the other roles do not
need that delay.

Each input of the profile has a role, set in the Web UI under Anzeige →
Tasten; exactly one of the device's inputs is Mandatory.

| Role | Behaviour |
|---|---|
| Mandatory | short: next screen; double: previous screen; long: back to the automatic default |
| Up | Konferenz: previous page; table: scroll up; repeats while held |
| Down | Konferenz: next page; table: scroll down; repeats while held |
| Scroll | while held: Up or Down in the current direction, repeating; every release flips the direction, so a short tap only flips |
| Off | ignored |

Defaults: three inputs Mandatory, Up, Down; two inputs Mandatory, Scroll;
one input Mandatory; further inputs Up, except the third, which is Down.
With zero inputs the current automatic default remains selected.

The Scroll direction starts down and resets after 20 seconds without a
direction change; 0 disables that reset. The setting matters only while an
input has the Scroll role. Every accepted row remains reachable.

Actions cross the event bus and a four-entry static controller queue. Callbacks
do not block or call LVGL. Overflow is counted/logged and drops excess input
rather than accumulating stale actions. Touch screens and encoders remain
future hardware input types.

There is no input gesture that resets settings; the reset is available in
the Web UI only.

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
visible mask. The diagnostic colour test (`cfg::kDisplayTest`) keeps its
explicit edge exception for its rings; normal builds show the start screen
instead.

The product keeps one persistent LVGL object tree: the image and one draw
object (see "Drawing"), reused across screens. Image source revision forces
reopening after a replacement. No full-image cache or new framebuffer is
added.

## Fonts and readability

Montserrat weight 500 is included under SIL Open Font License 1.1.
The pinned source and regeneration instructions are in `assets/fonts/`.
Committed 4-bit glyph subsets provide Latin text at 12, 14 and 18 pixels and
a number-only score role at 48 pixels. Text subsets contain 228 glyphs,
including German umlauts/ß and common Latin player-name accents; the score
subset has 14 characters. Unsupported glyphs use `?`. The 14 pixel text role
is also LVGL's default font (`CONFIG_LV_USE_CUSTOM_FONT_DEFAULT` with the
header `components/ui/lvgl_font/ui_font_default.h`), so no built-in LVGL font
is compiled.

Role sizes (caption/body/title/score) are 12/14/18/48 px for the small
class (shorter side ≤260), 16/20/26/72 for medium and 20/26/34/96 for large.
Only 12, 14, 18 and 48 px are compiled, so medium and large currently use
the largest compiled font at or below each role; geometry still scales
separately. Text scale selects among compiled fonts; it does not rasterize
arbitrary font sizes on the device. Large-profile fonts and actual
names/boundary acceptance are still required. Bitmap payloads
are 8165/10960/17846/5718 bytes (12/14/18/48); descriptors/maps add flash
beyond those numbers.

## IP chip

The IP chip is a dark pill with one or two caption lines, top or bottom by
setting. At the top it takes the place of the caption band (on the table,
of the title row); at the bottom it takes the place of the status band.
Setup shows the setup network name and 192.168.4.1 and is permanent by
default. Station IP is shown for 60 seconds after a successful connection,
including recovery. Duration 0 means permanent. Each line is fitted to its
SafeArea chord.

## Colours and night

Each screen has background, text and accent colours. Derived from them:
a dim text colour (60 % of the way from background to text) for captions,
kickoff times and hints, and a track colour (16 %) for the empty rim and
inactive dots. Fixed semantic colours mark warnings, errors, neutral
states, the IP chip/own Konferenz row and slideshow dots.

Night mode defaults to 23:00–07:00 local civil time and waits for valid time.
The existing time service handles midnight and daylight-saving transitions.
At night the background becomes the night background and images are shown
at 20 % opacity. A profile with a backlight pin dims with PWM brightness and
keeps the colours. Without a backlight pin (the current C6) every text,
pill, rim and dot colour is mixed towards the night background; the night
brightness setting is applied as an amount of light (colour value =
level^(1/2.2), so 10 % keeps about 35 % of each colour). S3 dimming/network
behaviour is untested.

## Acceptance boundary

C6 firmware build and fixed sizes are verified. Visual alignment, real names,
text scaling, overlay overlap, all input counts, bottom-row reachability,
JPEG redraw responsiveness, night brightness and the full rendered safe-area
matrix remain polish/testing. No screenshot is represented as device evidence,
and the boundary harness has not been implemented/run for these new screens.
