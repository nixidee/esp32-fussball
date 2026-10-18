# UI — screens, navigation and overlays

The active controller uses one five-phase policy and one common sequence for
six types: Crest, Images, Single, Matchday, Conference and Table. Settings,
provider demand and navigation use the same competition identities and order.
The current content and layout contracts are described below.
[Earlier design requirements](#earlier-screen-system-design-requirements) remain
as historical context. Controller, ring and layout source changes have focused
host/geometry evidence; the latest integrated build evidence is recorded
separately. Physical input, round-screen layout and runtime reserves still need
device checks.

## Current screens and automatic sequence

| Type | Content |
|---|---|
| Single | The focused own match, a unique ordinary active match, or the selected club's held result/next preview; the same full names and available event presentation |
| Conference | One competition's applicable kickoff blocks; All pages or One page; optional own pin and independent own highlighting |
| Matchday | One competition's complete known sporting round, including rescheduled fixtures outside the phase's local-date cluster |
| Table | One competition's table, automatically centred around the selected club or paginated without one; complete manual row access |
| Crest | The crest role's replacement/factory image |
| Images | Configured library images, without the crest; visible-time rotation and continuation from the next actually shown image |

One global phase wins: Own, Other games, Matchday, Idle, optional Break. Actual
running own matches beat own buffers; competition order resolves own focus and
within-type order. Own special treatment can be disabled without losing club
highlighting or ordinary unique-match eligibility. Unknown kickoff and justified
interruption retain only their bounded canonical time basis. A failed fetch
alone does not invent a final whistle or erase retained content.

Each phase enables entries as automatic, manual-only or disabled. One common
type order is used throughout. Per-competition overrides inherit each shared
field until explicitly changed. Crest/Images are global automatic entries when
enabled. Confirmed empty entries are skipped; unloaded/failed content remains an
actionable card, while available content keeps its warning. No implicit sports
exception bypasses the configured first entry.

Every phase change exits manual mode and starts the first eligible automatic
entry. Ordinary data updates preserve the current eligible visit and its time;
a changed own focus updates the Single immediately. A save cancels pending
click/double-click/held gestures and old queued actions. Fresh commands wait for
the matching data generation and retain FIFO order.

Manual Next/Previous follows enabled sporting type-by-competition entries,
skipping images. Up/Down holds an internal page: all table row positions and
held/current/next round pages remain reachable. Before that first page action,
a manually selected entry still rotates its automatic pages. Every input
renews the configurable return timer; timeout/Default starts the first
eligible automatic entry. A held key must be released after settings change.

Visit time is divided equally across pages, with effective total at least
15 seconds and five seconds per page. Extra incoming pages join the next visit;
shrinking content removes invalid pages immediately. Time is credited only
after successful display output, so lock/render/image failures consume no unseen
image or page. There are no separate Table, Conference or Matchday page clocks.

Conference pinning keeps the own match first while it is a member. Automatic
One page ranks activity; manual paging uses complete kickoff order and anchors
from the visible automatic match. Conference and Matchday share a maximum row count, minimum2. The pin
uses one place; only remaining actual fixtures are split. Page count stays
unchanged, with up to one donated row per earlier page (>2pages) or two
(2pages), taking from the back first. Larger pages stay first. One actual
fixture displays alone without paging. The old whole-matchday own
retention switch is replaced by the separate own/other pre/post windows.

Picture frame overrides normal profiles without clearing them or the football
store. It shows the permitted crest/library sequence, lets navigation browse
images, hides football faults and stops automatic football demand. Setup queries
retain their normal admission limits. Reconnects do not repeatedly restore a
permanent setup-IP overlay in frame mode. Night mode, image recovery and settings
cancellation remain active.

Appearance comes from the screen policy: colours, text scale, background, image
and dots. The renderer places bounded page indicators, score-centred content
and prepared ring progress within the SafeArea. Physical readability remains
device acceptance. Views render fixed view models; they do not fetch data,
resolve policy or read GPIO.

## Layout rules

Every position is derived from the shorter display side `s` and the
SafeArea; nothing is placed at a fixed pixel position.

- **Size class**: small up to 260 px shorter side, medium up to 400 px,
  large above. Each class has font role sizes for caption, body, title and
  score. Caption, body and title select the largest compiled text font at or
  below their scaled role size (caption font as the minimum). The current
  score starts with the nominal 48 px font (35 px generated line height).
  Complete content height first reduces text roles/gaps, then the central
  font only when necessary to keep all content within the SafeArea. Larger
  compiled fonts remain a separate resource and physical-readability choice.
- **Gap unit** `g` starts at max(2, s/60); an overfull text layout may halve
  it with a two-pixel minimum. Dense tables use the actual font line height
  without extra row padding. The
  rim stroke is max(2, s/80) px.
- **Fitting**: a text line takes the chord of its band on a round panel (the
  full inset width on a rectangle). A line with a font list uses the first
  font that fits; if none fits, the text is shortened with "…" in the last
  font. A line that still leaves the SafeArea is drawn, but the render is
  reported as failed (UI health), so the overflow is visible in the status.
- **Content blocks** are centred vertically as a whole; the status line
  ("Demo") sits directly below the block, the caption band directly above.
  Sporting layouts reserve the full padded footer (see "Status pill").

### Screens in detail

- **Own match**: caption "competition · round" (dim; without a round label
  "N. Spieltag"), home name, centre, away name, a status pill, the
  highlight line and the status line. The centre is the score in the 48 px
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
  the rows: a page-position band while paging, followed by the status/IP band.
  A confirmed empty Conference is skipped. Missing/unconfirmed or failed fixture
  data use the common loading/error cards; no whole-store next-match search
  runs in an unreachable empty-Conference view.
- **Table**: a separate competition/group caption, all numeric column headers
  and the rows around the own club; the own club's row is filled with the accent
  colour. Rank, name, played, difference and points remain present. Numeric
  widths include the actual visible values and headers, including every
  canonical points/difference value from −10,000 to 10,000. If necessary,
  existing smaller fonts fit the complete row; no column is dropped. The
  common row positions fit their complete round-screen band. Headers align
  with the columns when their own chord fits, otherwise a complete compact
  legend keeps the same left-to-right order. The group leads the competition
  caption so its context survives a narrow band. Up to five
  rows use a wider row pitch. A competition without a table shows a card; one whose request
  failed without a table shows its notice card (State cards). The rim shows the
  position of the visible rows in the whole table.
- **Matchday**: the Konferenz layout. Caption "competition · round" of the
  first match (round label or "N. Spieltag", else its date), rows as in the Konferenz
  with result, kickoff time or "--:--"; the own match on a dark row. Below
  the rows a separate page-position band while paging, then status/IP; the rim shows the
  position of the page in the matchday. A competition whose request failed
  without matches to show gets its notice card in the cycle.
- **Crest / slideshow**: the image, page dots near the bottom rim while
  several slides rotate, and the status line above them; a bottom IP chip
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
| Local network initialization failed, no data loaded yet | "Netzwerkstart gestört", failed phase, automatic retry or "IP im Browser öffnen" when usable for a naming/DNS fault, and short code `NET E01`…`NET E13`. Ends the initial startup card, including on the crest screen | warning |
| Not configured, setup network on, local service failed | retains both setup WiFi instructions, network name and IP; title identifies the failed phase, first hint says to open the IP plus the short code | warning |
| No data loaded yet, no WiFi | "Kein WLAN" with the reason: "Netz nicht gefunden" (saved network not seen), "Passwort falsch?" (authentication retry), "Setup-Netz aktiv" (setup network only) or "Verbinde …" (scanning, connecting or retry); hint "Neuer Versuch läuft" (not with the setup network only) and "Setup-Netz aktiv" while the setup network runs alongside | warning |
| No data loaded yet, no valid time | "Warte auf Uhrzeit", "Zeitserver wird gefragt" | warning |
| No data loaded yet | "Lade Daten" before the first answer; after a failed request "Keine Daten", the name of the first failing competition, its short cause, "Neuer Versuch HH:MM" (next attempt) and its error code with the HTTP status ("OLG E10·503") | neutral / cause colour |
| Matchday or Table: a competition in the cycle failed and has nothing to show | the competition name, its short cause, "Neuer Versuch HH:MM" and the error code with the HTTP status | cause colour |
| Own match screen without an own match | "Eigenes Spiel", "Kein Spiel" | neutral |

The cause colour is the error colour, for the two data notices the
warning colour, which also has no "Neuer Versuch": "Anbieter pflegt nicht"
(the provider leaves past matches without a result for more than 24 hours)
and "Unvollständig" (E19: not all data of the competition fit the device's
store; the data that fit stay visible). The short causes are
listed in the firmware's cause texts; the Web UI explains each cause and its
action in full (codes: [API.md](API.md#error-causes-and-codes)).

The setup card already names the setup network and IP, so no IP chip is
shown with it. A device without configuration and without the setup
network shows the crest screen unless a local initialization error needs
its failure card.

### Status line and status pill

"Demo" is a permanent dim line below the content while the demo runs; the
demo shows no ordinary status pill; a new image-content fault briefly uses
the existing pill before the permanent Demo line returns.

Every other status is a status pill: a slightly transparent dark pill
(background opacity 204 of 255) at the bottom for 5 seconds (`kStatusPillS`) after a
change of the status state. The state is the WiFi connection, local initialization phase/error, valid
time, stale data and, for each active competition, its aggregate cause, capacity
notices and independent fixture/table/event failures, plus the desired image
role/id, failure and actual fallback. Changing the screen does not show it again;
successful-fetch timestamps alone do not restart it. While it is
visible it shows what the current screen reports. Sporting layouts and cards
reserve the complete padded status/IP bands. With a bottom IP chip the pill
sits directly above the chip; Conference/Matchday page positions have their own
band. Crest/Images retain overlays over the image. Content by priority:

- local initialization failure: code first and short action, for example
  "NET E11 · IP nutzen" or "NET E06 · neuer Versuch" (warning colour).
  The full phase remains on the failure card and in the Web UI. Loaded
  football/image content stays visible; retry time alone does not restart
  the five-second pill;
- "Kein WLAN", "Warte auf Uhrzeit" (warning colour);
- on Matchday and Table, the matching dataset of the competition on screen:
  after a failed request its error code without the HTTP status and
  "Stand HH:MM" of that dataset's last successful request
  ("OLG E10 · Stand 14:05", cause colour); with stale data "Stand HH:MM"
  (warning colour). Fixtures success does not clear a table failure or replace
  its timestamp; Fixtures failure does not downgrade a fresh table. Capacity,
  unmaintained-result and live-age rules come from the canonical dataset owner;
  unrelated dataset/competition failures are not attached to this view;
- elsewhere one failing competition as "code · name" ("APF E08 · Premier
  League", cause colour), several as "N Wettbewerbe gestört" (error
  colour, also when some of them only have a data notice); else stale
  data as "Stand HH:MM" of the oldest data shown, "Daten veraltet" without
  any (warning colour).

The pill is not shown on a card that already says the same: the local
initialization failure card, setup,
"Kein WLAN" and "Warte auf Uhrzeit" cards for the network and time states,
and a competition's notice card for that competition's failure. The start
screen has no status. A network initialization error ends it; recovery
continues through the ordinary state/content cards rather than re-entering
the initial three-step card. The existing packed display status exposes
this startup flag for the Web preview, without a second state machine. The Web UI preview shows the pill for as long as the
state lasts and notes that the device shows it only for 5 seconds.

### Current content and completion ownership

The controller compiles the current situation's effective programme from shared
rules and competition-attached overrides. Manual and automatic navigation use
the same enabled entries; manual-only sporting entries retain their durations.
Explicit club groups form priority blocks, independent of names. With “Only one
own single match” on, the focused own game is the only own-phase Single; off
allows further eligible own Singles in that order. A games overview with one
match uses the Single layout; several use Conference. An identical automatic
focused Single is not repeated by a one-match Conference; manual access remains.

Tables retain provider group boundaries: automatic paging without an own row
visits each group; a selected own group is centred automatically. Complete manual
paging spans every row window in every group. The caption identifies the current
group, and ranks restart with that table. No group is flattened into another.

One prepared page supplies both sequence eligibility/page count and the Presenter
with the selected match, Conference rows, table rows or sporting round. Its
lifetime is one canonical-store read. The Presenter composes labels and semantic
progress; the renderer measures fonts and round-screen geometry. The controller
prepares immutable calendar offsets for the page, current time, independent
last successes and retry. Each instant uses its own timezone offset; no time
service is called during presentation. Missing calendar facts remain unknown.
Names use the
consciously adopted 26/3-byte display envelope. Full provider originals are
transient editor data; import/adoption previews any reduction before confirmation
and storage. The renderer tries the complete adopted display label in all
allowed fonts before using its custom short name. An explicitly empty short
name keeps the adopted label, with a UTF-8 ellipsis if it cannot fit the band.
This does not promise rendering the unshortened provider-original string.

Page indicators use at most seven dots. More pages show the exact current/total
position, including the last page of a512-match projection. Pills reserve their
padding inside the whole band's round-screen chord. Normal added time never
creates an extra-time lap: only confirmed extra-time/shootout phase or period
can do so. Top/bottom markers follow the reported half, and minute text includes
reported added minutes. The renderer receives progress angles, not football-time
rules.

Single shows only reported shootout attempts, in provider order, in ten-attempt
windows rotating with the existing event interval. Home, away and unknown actor
remain distinct; a score total never creates fictional scored/missed attempts.
The attempt caption identifies the reported range and incomplete/missing data;
regular highlights remain available. Long shootouts use the shared retained
event pool independently of the twelve regular highlight references.

Layout height includes all actual font line heights, the attempt rows and
complete padded IP/status bands. When the content is too tall, existing smaller
text fonts and gaps are selected. The central score font is reduced only when
the remaining complete content still requires it. Two-line setup IP chips,
long shootouts and larger requested text therefore do not place captions or
highlights outside the content area. This is a source contract; physical
readability still requires the screenshots listed below.

Scene selection, LVGL source acceptance, JPEG decode and LCD transfer completion
are separate. A visit starts only after the matching scene's final SPI stripe
completes; an image additionally requires the matching successful decode. Failed
or stale transfers earn no time. A settled image-error card earns a finite visit,
so automatic navigation continues without consuming the ordinary image
continuation cursor. Screenshot capture retains the same normal flush ownership.
These source/host contracts still require screenshots from owner-flashed round
hardware for layout and actual transfer acceptance.

### Drawing

One full-screen draw object paints every shape and text from a fixed list of
72 items (2,016 bytes) with a 1,280-byte text store. A render compares the
new list with the previous one and invalidates added, removed and changed
item bounds; a growing progress arc normally invalidates only its new part.
Unchanged items add no invalidation. LVGL can merge those areas, while the
initial frame, screen transitions and image/style/rotation changes can
repaint the whole display. LCD transfers use partial buffers stripe by
stripe even for a full repaint; the JPEG decoder can restart from the top
for each stripe, so partial repainting does not mean that only dirty image
pixels are decoded. The two persistent child objects (image and draw object,
under the active screen), fixed item/text storage, temporary LVGL draw tasks,
DMA buffers and decoder work state have separate memory costs. Shapes are
drawn first, all texts above them. A list or store overflow is reported as a
render failure.

## Presenter and event display

Missing values use `--` rather than fabricated zero. Supplied minutes are plain;
estimates show `~`. Half-time, full-time and shootout have explicit labels.
A separate shootout tally does not change ordinary goals. Kickoff is converted
to selected local civil time; retries and freshness use monotonic time.

The match store keeps up to 12 events per own match and 48 in total.
The Own match highlight band rotates through that bounded list every five
seconds. It displays the event type and player; supplied substitution in/out
names appear as "out > in", a substitution without direction (ESPN) as
"A / B". A one-line rotating band keeps every retained
highlight accessible without a larger object tree. Removed records are
excluded; truncation and reconciliation behaviour is defined in the model.

The One page order uses recent principal-value changes, rather than repeated
initial fetches or source-tag changes; a change not seen since the last
restart sorts after seen ones. The table defaults to five rows around the own
club, with its initial window clamped to the stored rows; manual Up/Down
continues across competition boundaries. Long names use UTF-8-safe prefixes and
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
| Up | Konferenz and Matchday: previous page; table: scroll up; repeats while held |
| Down | Konferenz and Matchday: next page; table: scroll down; repeats while held |
| Scroll | while held: Up or Down in the current direction, repeating; every release flips the direction, so a short tap only flips |
| Off | ignored |

Defaults: three inputs Mandatory, Up, Down; two inputs Mandatory, Scroll;
one input Mandatory; further inputs Up, except the third, which is Down.
With zero inputs the current automatic default remains selected.

The Scroll direction starts down and resets after 20 seconds without a
direction change; 0 disables that reset. The setting matters only while an
input has the Scroll role. Every accepted row remains reachable.

Physical input and admitted Web actions carry the full settings generation
directly into the same four-entry static controller FIFO. Web commands use the
caller's last-read configuration revision; stale requests are rejected before
queueing, and a later configuration change also discards already queued old
commands. Every actual revision change cancels pending/double/held/repeating
physical gestures independently of navigation changes; held keys must be
released before another action. Cosmetic changes preserve the manual visit,
and a same-revision phase projection retains the current gesture. Old versions
have no navigation, start-screen or IP timer effect. The 20-ms application poll
applies accepted actions in order, using the current store bounds for each row/page movement; the 500-ms display refresh does not
delay action consumption. Callbacks do not block or call LVGL. Overflow is
counted/logged and drops excess input
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

### Missing and damaged image content

The image service keeps one desired id/role and selects a fully validated
file. Library images fall back to the appropriate role replacement, then its
unchanged factory image; a damaged role replacement falls back to factory.
Missing desired files do not remove settings references or slideshow positions.
The desired file is retried after ten seconds or a new file revision; a
successful check returns to it. Checks share the existing heavy-operation
admission and JPEG workspace, outside the LVGL and football-data locks.

With no usable candidate, football remains on its configured base colour with
an error pill. Only a pure crest/image view becomes the repair card; setup,
network and football notice cards remain. The card has cause, image code,
Web repair/diagnosis hint and retry indication. A usable fallback is reported
through the same temporary pill and permanent Web diagnostics. Simultaneous
network/provider and image warnings share both codes plus a Web hint in the
existing five-second pill. The image code is not duplicated when the crest
repair card already shows it. Demo returns to its permanent line after the
temporary image warning. Retrying or advancing an attempt alone does not
restart the pill.

A file source is applied only after the LVGL getter confirms the new owned
source, including same-path/new-revision changes. A rejected change hides the
old image instead of presenting it as the requested replacement. The installed
JPEG callbacks and POSIX-open callback report matching source attempts. A
successful open/close or end-of-image return alone is not decoded-image proof:
only delivery of the final image block marks decode completion. Later faults
start the service's fallback path. These are content observations, never an
additional UI/hardware-health or OTA-confirmation gate. No whole-image buffer,
second decoder, rendering task or automatic file deletion is added. Actual
round-screen pixels, clipping and late-fault recovery still need device checks.

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
the largest compiled text font at or below each caption/body/title role;
the score starts with the nominal 48 px font (35 px generated line height).
The complete content fit may reduce its font after the text roles and gaps
have been reduced. Geometry scales separately. Text scale selects among
compiled text fonts; it does not rasterize arbitrary font sizes on the device.
Larger compiled fonts need their own resource decision. Actual names and
boundary readability still require physical acceptance.
The same 4-bit pixels are stored with lossless run-length encoding: bitmap
payloads are 7224/9285/14036/3396 bytes (12/14/18/48), instead of
8165/10960/17846/5718. Descriptors/maps and the single decoder add flash.
The public LVGL font callback writes alpha pixels directly into LVGL's existing
glyph target. It adds no heap allocation, line buffer or mutable font cache;
missing/undersized targets and malformed/truncated streams return failure
without publishing partial output. The native compressed-font decoder stays
disabled. Actual LVGL comparisons preserve all 691 nonempty original bitmaps,
all 698 character metrics and the four existing question-mark fallbacks.

Decoding costs CPU: a pixel performs at most three bounded bit reads and one
alpha-byte write. A host sanitizer replay took about 4.6 times the native plain
reader's CPU time for 100 complete font sets; this is not C6 timing or a physical
readability measurement. The isolated C6 callback's own stack frame is 80 B.
Device rendering latency and runtime stack/heap peaks still require measurement.

## IP chip

The IP chip is a dark pill with one or two caption lines, top or bottom by
setting. At the top it takes the place of the caption band (on the table,
of the title row); at the bottom it takes the place of the status line, and
the status pill sits above it.
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

Night mode is available only on a target with a controllable backlight pin.
It defaults to 23:00–07:00 local civil time and waits for valid time. The
existing time service handles midnight and daylight-saving transitions.
Night mode changes only the PWM backlight brightness; background, text,
semantic colours and image opacity stay unchanged. The current C6 has no
backlight pin and offers neither brightness controls nor night mode. The
PWM setter remembers only the last successfully applied hardware duty.
Repeated equal duties make no driver writes; initial zero and a new wiring
owner still receive setup and their first duty. Failed setup/set/update calls
never publish a successful receipt; the existing fatal SDK-error/restart
contract remains. The paused S3 profile's PWM behaviour remains untested on
hardware.

## Acceptance boundary

Debug → Bildschirmfoto can download the next real LVGL redraw in the display's
current reading orientation. This is a separate screenshot function, not
the schematic Web preview; see [WEB_UI.md](WEB_UI.md). A full-frame device
buffer and a host LCD renderer are not used. Device acceptance must compare
the PNG with Own match, Konferenz, Matchday, Table, crest/slideshow, notice
and startup/setup cards and status/IP overlays at all rotations. On a target
with backlight control, compare the physical day/night brightness separately:
the screenshot contains the same colours and images in both modes.

C6 firmware build and fixed sizes are verified. Visual alignment, real names,
text scaling, overlay overlap, all input counts, bottom-row reachability,
JPEG redraw responsiveness, hardware night brightness and the full rendered safe-area
matrix remain device acceptance. The bounded geometry fixture executes the
actual layout/fit functions, actual font-source glyph advances and SafeArea:
216 cases cover240/466px round geometry,100/130% requested text, one/two-line
IP at either edge, nine-row signed-value tables, long/missing shootout attempts,
Conference/Matchday page growth and511-page positions, cards and Crest/Images.
It records strings/rectangles and checks complete numeric columns, attempt
symbols, padded containment and text overlap. It draws no pixels and does not
prove LCD legibility, glyph overhang or transfer behavior.

Required screenshots include Single before kickoff, live,90+added time,
confirmed extra time and an18-attempt/missing-attempt shootout; Conference and
Matchday with four/five rows and large page counts; each table group with
long/short names, nine rows and signed extreme values; Crest/Images including
decode failure/retry; setup/loading/empty/provider/network/storage cards. Repeat
the applicable views with100/130% requested text, one/two-line top/bottom IP,
temporary status and all rotations. No screenshot is represented as device
evidence until captured from the owner-flashed target.

## Earlier screen-system design requirements

The following section preserves the earlier design discussion. Its requirements
remain visible for acceptance, but future-tense descriptions of canonical
models, name provenance, controller ownership or Web source organization are
historical planning. The implemented contracts and separate evidence limits
above describe the current source.

Historical product direction, 2026-10-17, reviewed against the clean `efae218`
candidate. At that checkpoint these requirements awaited the model/API/storage
design and implementation. Subsequent approved consolidation implemented the
current contracts above; this retained text does not reopen that design gate; [ADR-042](DECISIONS.md#adr-042--situation-based-screen-sequences--accepted-direction-2026-10-17)
records that boundary. Setup/startup and fault handling remain part of the
complete screen inventory, not extra normal sequence entries.

### Situations and match periods

The device uses one global situation, taking the highest applicable priority:

| Priority | Situation | Meaning |
|---|---|---|
| Highest | Own game | An own fixture is inside its before/live/after period, with a club selected and separate own-game handling enabled for that competition |
| Next | Games in a competition | Relevant fixtures are inside their before/live/after periods, including own fixtures whose separate handling is disabled |
| Next | Matchday | Inside a related matchday's local-date span, without a higher situation |
| Otherwise | No game / optional season break | Normal quiet-time profile, or its separately enabled break profile when the known fixture gap qualifies |

Each situation has its own screen choices, automatic/manual eligibility and
durations. Separate own-game handling can be disabled per competition; that
fixture then contributes to Games in a competition, and stays highlighted in
its Conference. This switch does not erase the club identity or turn off the
competition's other selected screens.

A confirmed running report may establish the live situation even without a
usable kickoff. Use its first confirmed observation as a bounded 240-minute
selection anchor, visibly retain the unknown kickoff, and return to the normal
kickoff basis when supplied. Do not invent a kickoff or minute. An explicitly
interrupted game retains its already justified active slot within the same
bound, with an interruption label and no clock advancement; later provider
reports determine resumption or abandonment.

A matchday period extends continuously from 00:00 on its first local date
through the end of its last local date, including an empty day in between.
Use the actual sporting round where the provider supplies a reliable mapping;
split distant fixtures of that round into date clusters with at most one fully
empty intermediate local day. Two empty days split the phase periods. Without
a reliable round, use explicit temporal fallback groups: at most one empty
intermediate day and at most four calendar days from the group's start. This
fallback must not be presented as a verified sporting round. Date clusters
determine the device situation; the Matchday screen still shows the full known
sporting round, including separated catch-up fixtures. Without a real round it
shows the documented fallback group.

Optional season break means a gap of more than the configurable threshold
between the previous and next fixtures, default 21 days, across **all active
competitions**. A competition without a club still counts; a configured World
Cup or European Championship can interrupt a league's summer break. Known
fixture coverage is required: missing, stale, not-yet-published or failed
schedule data cannot establish that no game exists. A higher situation wins.
The break profile can differ from the ordinary no-game profile without
altering its settings.
The threshold uses elapsed time between kickoffs (days times 24 hours), not
local date subtraction. Its range is 1–366 days. Compact source/season/interval
coverage evidence remains separate from retained match rows. Discovery may
inspect two adjacent seasons with up to 16 additional requests per device-wide
run, at most one new run daily; ordinary live/due work and provider limits take
priority. Reaching a bound leaves coverage unknown.

### Own-game focus and individual-match content

Only one own game has individual-match focus at a time. A genuinely running
own match outranks another own match's before/after buffer. Concurrent running
own games follow the ordered competition list; this same list supplies the
competition order within every screen type. When the focused game finishes,
another running own game can take over immediately without waiting for the
first game's after-buffer.

Use four global buffer settings: before/after for own games, and before/after
for other games. For overlapping own-game buffers, hand over near the midpoint
between the actual finish and the next kickoff, clamped to the configured
windows: shorten buffers as needed, never lengthen them. A 30-minute gap can
therefore give 15 minutes to the result and 15 minutes to the next preview.
Actual running own games continue to outrank buffers. The old option to retain
the own game for the entire matchday is replaced by these rules, rather than
kept as a competing rule.

In Own game, at most the focused own match contributes an Individual match
entry. Other competitions can contribute their other enabled screen types,
but no extra static individual-match entries during that situation.

Outside Own game, the static Individual match screen requires a selected club.
It keeps the latest result through the end of the local calendar day following
the game's finish date, then switches to the next fixture's preview. A game
finishing after midnight uses that later date; an estimated finish remains
identifiable as estimated. The fresh result remains
for that period even if no next fixture is known; after the hold, no next
fixture makes the entry empty. A competition without a club has no static
result/preview entry.

In Games in a competition, Individual match can instead show an eligible
game: one actual running candidate wins over other games' before/after
buffers; without a running game there must be exactly one candidate in the
window. If no unique eligible active candidate exists, the selected club's
static result/preview remains available under the same hold/empty rules.
Disabling separate own-game handling removes its special situation and focus
priority, not its eligibility for this generic candidate rule: one own live
game, separate handling off and the general Individual match entry enabled
means Games in a competition with that game shown as the ordinary unique
candidate, still highlighted as own. Do not filter it out merely for being
own, or restore the special Own game situation/priority through this path. The
active other-game presentation has the **same available information scope**
as an own game, including events and shootout attempts where the provider
supplies them; it must be labelled as the actual game, not as the owner's
game. Missing provider detail must be explicit, not silently filled in.

### One sequence model, shared order and overrides

There are six screen types: Crest, Images, Individual match, Matchday,
Conference and Table. Crest and Images are global. The four football types
can have entries per competition. One configurable **type order applies to
every situation**; entries within a type follow competition order. There is no
second priority list or arbitrary per-entry ordering.

New development settings start with the order Crest, Images, Individual match,
Matchday, Conference, Table. All football types are enabled for manual access;
the initial automatic choice per situation is Individual match for Own game,
Conference for Games in a competition, Matchday for Matchday, and Table for
No game and optional Break. Crest/Images start disabled. Initial visits are
15 s for Crest, 60 s for Images/Table and 30 s for Individual match/Matchday/
Conference, configurable from 15 s to 12 hours. Picture frame and automatic
Break start off; frame includes Crest when enabled. During development settings
may start fresh; migration is reconsidered after alpha/beta, while image files
need not be deleted.

Each situation has shared enabled/automatic/duration values. Its optional
competition-specific controls provide live overrides of those shared values,
not one-time copies. The UI identifies overrides and can return a field to its
shared value. Switching competition-specific mode off retains overrides but
temporarily ignores them; switching it back on restores them.

- Disabled: absent from automatic and manual navigation; hide its options.
- Enabled with Automatic: present in the automatic sequence and manually
  reachable for football types; show its duration.
- Enabled without Automatic: reachable only manually; hide its duration.
- Crest and Images are always automatic when enabled and are skipped by normal
  manual football navigation.

For example, Crest 15 s and Images 60 s can precede Individual match 3. Liga
30 s, Matchday Bundesliga 30 s, Table Bundesliga 60 s and Table 3. Liga 60 s.
Additional manual-only Matchday/Conference entries appear in the manual
sequence in the same type/competition order, without entering that automatic
sequence. Next/Previous starts relative to the currently visible automatic
type/competition position, skips Crest/Images, and wraps through eligible
manual football entries. An inactivity timeout or long Default returns to the
first eligible automatic entry; successful saves also use a coherent automatic state and
must not revive queued actions or gestures from the old configuration.

Every situation change, higher or lower, ends manual mode and starts at the
first eligible automatic entry of the shared type order, even if that entry
is a long Crest/Images visit. A focus change inside Own game applies
immediately. Ordinary updates preserve a still-valid current entry and visit
time; an entry that ceases to be valid cannot remain selected.

Skip entries known to be empty, such as a cup table that does not exist or an
empty image list. A failed fetch is not confirmed emptiness: retain older
data with a warning, or present the competition's actionable error card when
no data exists. If the automatic sequence has no eligible entries, show an
actionable neutral notice instead of re-enabling a disabled or manual-only
screen. No eligible manual entry means no hidden football fallback.

An unsuccessful match update alone does not change the situation. Keep the
last justified data with a warning while evaluating the existing bounded
match windows. At their limit release focus without inventing a final score
or confirmed whistle. New data enters the ordinary selection rules again;
a failed table update alone does not make match information stale.

### Pagination, images and Conference

Paginated football screens divide one configured total visit time equally
between their pages. There is no separately adjustable page duration. Their
effective total is the largest of the configured total, 15 seconds and
five seconds times the page count. Thus eight pages need at least 40 seconds,
even when the configured total is 15 seconds. Help and the UI must explain the
effective duration. The old fixed eight-second Conference timer and separate
table-step setting must not survive as another timing mechanism.

If incoming data add pages during a timed visit, keep that visit's current
page count, elapsed time and total duration. Show the additional pages when
the next visit begins and calculate its duration from the new page count.
This also applies while a manually selected entry rotates its pages
automatically. Explicit held manual paging continues to expose all available
content.

A manually selected football entry continues its automatic internal page
rotation using that computed timing until the user pages with Up/Down.
For a manual-only entry the retained visit-time value still supplies this
calculation even though its duration field is hidden; help explains that use.
After that, hold the manually chosen page. Every input refreshes the manual
return deadline. The outer entry remains manual until timeout, Default or a
situation change; internal page rotation does not advance to the next outer
entry on its own.

Separate Conferences per competition replace the mixed Conference. Preserve
All pages versus One page/activity-ranked behavior, own-game highlighting
whether pinned or not, and fixed chronological manual paging. Optional own
pinning retains shown own games at the top while the other rows change.
Page partition is bounded and keeps the original page count. Unpinned
counts5,5,5,1 become4,4,4,4;5,5,5,2 become5,4,4,4;5,5,1 become4,4,3;
4,4,1 become3,3,3;5,1 become3,3. A pin is added to every nonempty page
after partitioning the other fixtures. The shared minimum row maximum is2;
actual content can be one. Readable fonts and final geometry require device
screenshots.

Normal Images contains library images only; Crest has its own screen and is
no longer a normal slideshow item. One image duration applies to all images;
the Images visit is a multiple of that duration. Continue with the next image
on the next visit instead of advancing invisibly while football is shown.
Four images at 15 s each with a 30 s Images visit show 1+2, then 3+4 on the
next cycle. Development settings may restart rather than convert old crest-slide
references; library and protected image files are retained.

### Picture frame mode

Picture frame mode temporarily overrides the normal profiles and restores
them on exit. It shows library images, optionally including Crest, with one
common per-image/crest duration. It stops competition data demand and hides
the corresponding football errors. A single HTTP request already sent may
finish and publish its actual outcome and request cost; no further autonomous
football request or continuation starts while frame mode is active.
Explicit user-started competition-wizard queries are permitted within existing
limits so setup remains usable. On exit, resume
due work within the existing request limits, retaining data and profiles.
It does not remove network/setup access, image
validation, fallback, repair or retry behavior. IP appears only for the
configured time after start, rather than indefinitely because football data
are absent. With no usable frame content, show the applicable neutral or
image-repair state, never a hidden football screen.

Next/Previous and Up/Down select and hold frame images using the normal manual
return timer, renewing it on input; zero disables timed return. Timeout or long
Default resumes frame automation at the currently viewed image with its full
image duration. No football entry is accessible in this mode.

### Ring roles and subpage dots

Navigation and sport rings are independent functions. The navigation switch
controls automatic position plus the manual-return countdown. The sport
switch permits match-time or shootout information in automatic mode. Manual
mode always gives priority to its return countdown over sport information;
without a return deadline the manual ring is off. Sport information must
still be considered in the screen content; if it cannot fit legibly, report
that conflict for a decision instead of removing it silently.

The automatic position ring counts automatic entries only. It groups entries
by screen type, or optionally separates the football types by competition.
Crest and Images form one global image category. Optional remaining segments
are shown subdued. The image category still counts when enabled, even if the
ring is hidden while Crest/Images are visible: with Images and Table enabled,
Table can show both segments. The image-hide option also applies to the
manual/sport display rules where relevant; hiding a ring does not remove its
category from the automatic sequence.

Dots continue to mean subpages of the current screen, not screen types. They
default on from two pages, can be disabled per screen and must be larger and
anchored consistently. When the navigation ring replaces the current
Table/Matchday scroll arc, preserve the information about the visible part of
the list in the accepted layout rather than silently discarding it.

The sport ring replaces automatic sequence position on Individual match or
Conference during an active game when enabled. Regulation time has 18
five-minute segments, nine per half, plus top and bottom markers. The top
marker is yellow before the first kickoff and clears on kickoff. The bottom
marker is yellow for first-half added time, red in the halftime interval and
clears for the second half. The top marker handles second-half added time
through the final whistle, then clears. Played segments fill in the green
progress color. Extra time switches to six five-minute segments, three per
half, with equivalent markers at 105/120 minutes; the top marker indicates
waiting for extra time to start.

A Conference uses its earliest relevant kickoff slot, then the next after
that slot finishes. Progress follows that slot's first kickoff. Its halftime
marker turns red when the last first half of the slot ends and clears when
the last game has resumed; similarly, the final marker turns yellow when the
first game reaches the second-half endpoint and clears at the last final
whistle. Extra-time behavior follows the same half-based rules. Provider
clocks may be estimated only with a visible estimate indication; estimates
must not be presented as provider-confirmed whistles, extra time or attempts.

For an Individual match with supplied shootout attempts, show `11m` at the
bottom center. Attempts grow upward from that point on both sides: home on
the left, away on the right, green for scored and red for missed. A tally
alone cannot reconstruct attempts or misses. Provider completeness, ordering,
corrections, longer shootouts and bounded storage require the upcoming model
and feasibility design; no attempt cap or fabricated history is approved here.

### Device layouts, testability and website

Planned club-name input to the layouts is one selected-language name of at
most26 UTF-8 bytes and a short code of at most3 bytes per working team. Both
adopted languages remain durable/editable, not a global display RAM collection.
Full original preset texts stay in the immutable source, without an additional
original-user-text archive. Layout design will choose full/short use per
screen; a possible user selector is not yet selected. Adoption uses UTF-8-safe
prefix clipping, including provider-fallback labels; complete input is matched
first and identities are never shortened. Two central target-selected compile
limits drive storage and Web preview; S3 may use larger values after review.
Views consume prepared team values without file I/O.

Inventory every affected screen/state and retain its information before
redesigning. The single-match score must be central and prominent, with the
other information arranged around it; test short names as well as long names
and avoid solving fit solely with smaller fonts. IP and status/error overlays
need fixed, separate opposite anchors independent of content. The reported
slow visible construction of images/text still needs measurement of the real
draw/decode/flush path; composition cadence and button recognition delays do
not establish its cause.

The extended device Demo provides controllable, provider-faithful
match/event scenario playback through the canonical model and production
screens. Use a separate test time without changing the system clock, redirecting
provider endpoints or adding a host screen renderer. Its scenario flash and
runtime costs must be included in the proposal. Synthetic playback supports
repeatability; current real provider responses still need checks through the
real parsers for affected states and limits.

The owner selected a Demo-only advance through the automatic screen sequence
with normal timings and no new stored Demo settings. Shortened test visits
or clock acceleration are excluded. The screenshot remains always available
near the top of the intended Debug tab, with Demo controls just below where
practical; one default-off preference additionally shows screenshot on Overview.
These controls are implemented in source before layout acceptance and complete
Web redesign. The automatic-next command follows each page/image boundary,
then the next screen/competition. From manual output its first click returns to
automatic selection; it does not advance scenario time or change saved durations.
The current Demo uses fixed selected scenario steps, saved eligibility rules
and normal visit/manual-return times. A
scenario step does not necessarily change the shown screen. Keep diagnostic
state sufficient to explain the active phase, manual/frame state and missing
content before attributing an observed Table-only sequence to a selector fault.

Agree the selection/control model and costs first; then implement that model,
its options and testability. Present alternative device drafts of the main
screens before deciding them. Only after those layouts are sufficiently final
design the time/shootout ring variants on them, choose the presentation and
implement its final logic. The owner flashes; screenshots from the real
240 px round device supply layout evidence. No build or host logic test
substitutes for those screen checks.

Restructure the website after the device model and layouts are stable. Keep
one consistent design system, frequent sequence controls on Overview, per-screen
appearance under Display and file management under Images. Put the picture-frame
mode control at the top; keep general navigation-ring options together under
general settings, distinct from per-screen colors/appearance and subpage-dot
options. Phase screen controls
can align horizontally on desktop and wrap on phones; disabled options collapse,
and non-obvious options have code-checked `?` help. Preserve actionable errors
and save/session safeguards. The agreed target is no whole-page desktop scroll
at 1440×900 except long lists inside their areas; at 375 px phone width there is
no horizontal scroll and pages stay around two screen heights. Use the device
draw list for the actual preview and on-demand sequence thumbnails, with one
layout implementation. Browser and physical-device acceptance remain separate.


### Historical interim team-label selection — 2026-10-17

Every existing team text uses the working team's prepared26-byte name field.
The independent3-byte short code was already prepared and stored; its
screen placement was still pending at that checkpoint. The current full/short
fit rule is described above. Views and
presenters perform no durable name lookup. Available raw match-detail names
remain the fallback when the working directory cannot hold an identity. The
name cutover still requires owner screenshots of Single, Conference, Matchday,
Table, Overview and start/next-match content; host checks do not establish round
LCD layout acceptance.
