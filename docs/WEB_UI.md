# Web UI

The embedded page is a single offline HTML/CSS/JavaScript application with
German and English text. It uses no framework, CDN, web font or browser build
step. Firmware and page are updated together. Current C6 source/build evidence
and pending device acceptance are recorded separately below.

The dedicated screen-policy options are available in the current source. They
share the existing draft/save flow with general settings and competitions.
The active controller now uses the same policy and common sequence. The
frame request-admission gate applies before the first HTTP write.
The [screen-system contract](UI.md#current-screens-and-automatic-sequence) distinguishes
this staged implementation from final device behavior and layout acceptance.
The overview contains frame mode; Display contains phase rules, common order,
appearance and library-image sequence. Image files remain managed under Images.

## Source organization and offline bundle

`web/index.html` contains the document shell and the ordered list of local
stylesheet/script sources. `web/style.css` owns the shared layout and design
tokens. The plain JavaScript files in `web/js/` share the application's existing
lexical scope; they do not create additional state owners or a module runtime.

| Source | Responsibility |
|---|---|
| `i18n.js`, `state.js`, `messages.js` | Translations, the single application state, field metadata, errors and diagnostic text |
| `controls.js`, `pages.js` | Shared fields/dialogs and composition/navigation/status updates for the seven pages |
| `api.js` | HTTP/session admission, uploads and device snapshot loading |
| `draft.js` | The shared settings draft, validation, save receipts and baseline reconciliation |
| `screens.js`, `competitions.js` | Screen-policy options and the competition/provider-selection editor |
| `names.js`, `images.js`, `debug.js`, `system.js` | Name drafts, image editing, Demo/screenshots/logging and system actions |
| `app.js` | User-action wiring, theme and autonomous polling |

The existing `scripts/embed_web.py` pre-build step inlines these local sources
in their HTML order into one script and stylesheet, then emits the same single
deterministic gzip firmware asset. The device still serves one offline document
without additional requests, LittleFS files, framework or dependencies. Changing
any included source updates that asset; unchanged input preserves the generated
header. The script's `--document` option prints the bundled HTML without writing
files. Client checks use `scripts/tests/web_source.cjs`, which calls this same
bundler instead of maintaining a second source list.

## Lossless WiFi names

WiFi names retain their original bytes, up to 32 bytes. Ordinary valid UTF-8
uses a JSON string. A name containing other bytes uses the explicit wire value
`{"base64":"..."}`. The editor labels that representation **Base64**, explains
its original-byte limit and edits it directly in the shared draft. NUL and line
ending bytes also use this editable presentation because an HTML text input
cannot retain them verbatim. Their wire value remains a valid JSON string when
they are UTF-8. A scan selection replaces the same draft field.

Validation rejects malformed or noncanonical base64 and names exceeding 32
original bytes before a write. Encoding metadata is derived from the draft;
there is no separate hidden WiFi-name state. A decoded UTF-8 value uses the same
wire representation as the device's subsequent readback.

## Seven pages

| Page | Content |
|---|---|
| Overview | Frame mode, current screen/reason, own match or kickoff, global data age, next provider round and its reason, source/match count/data time/cause, action and error code per competition, warnings and refresh/next/IP actions |
| Competitions | Compact ordered list of active/paused entries with a Details expander (season, own special treatment, move/edit/short names/remove), add/edit wizard, provider competition/club lists |
| Display | Five phase tabs, six types, shared rules and retained per-competition overrides, common type order, visit/image times, separate crest and library images, appearance, rotation, badges and input roles; brightness/night controls only with a controllable backlight pin |
| Images | Revisioned thumbnails, usage, quota, upload/replace editor, confirmed real deletion, and the crest and standard background (replace, copy from the library, restore the factory image) |
| Settings | WiFi scan and credentials, hostname/antenna, provider keys/opt-in and request limits (daily cap, window limit) per provider, time/language/access |
| Debug | Screenshot at the top, optional Overview screenshot preference, Demo enable/scenarios/automatic-next, expert logs and device diagnostics |
| System | Firmware/update, backup/import, restart, reset-gated storage formatting, delete user images, factory reset |

The page starts with session discovery. Without an admin password it obtains
its mutation token silently and shows no login/logout controls. With a password,
reads remain available and exactly one login/logout control is shown. Schema,
settings, status and images then load independently. A failed load has a visible
retry card and automatic retry; it never leaves an unexplained blank page.
Status failures mark the connection as unavailable even when older data exists.
Short notices appear at a fixed position near the bottom of the window, in the
browser's top layer so they stay visible above open dialogs; browsers without
that layer show them as an ordinary fixed element.
Debug device diagnostics label provider request attempts as "since restart". The
daily caps are separate settings; the boot counter is not displayed as daily
consumption or as a fraction of a cap.

Local network-service errors appear in the existing overview warnings and
Debug device diagnostics. A working WiFi connection remains shown as connected
when only setup DNS, local naming or HTTP advertisement failed. The error
names the failed phase, likely cause, automatic retry time, short `NET E..`
code and SDK error; naming/DNS faults with an active IP advise opening that
IP directly. Normal status polling clears recovered errors and preserves
unsaved form edits. Local-service readiness is listed with the device
measurements, separately from the WiFi address.

The overview reports the current screen and its reason. Screenshot download is
available when its additional Overview preference is saved; Debug always offers
the same real-device capture operation. It does not generate a simulated display for
the new screen policy. Ground layouts and rings are chosen from owner-flashed
device drafts; a future preview must consume the device's actual draw list.
Status polling preserves unsaved controls and shows actionable network/image
errors separately from the overall connection state.

Theme follows the browser by default; a manual light/dark choice is stored only
in the browser. Form/navigation text follows the draft language choice. Status
polling updates its own nodes and does not replace unsaved form input.

## Runtime Demo controls

Under Debug → Demo, enable Demo and save first. Load the scenario
catalogue, select a scenario/step, or move to the previous/next step. Controls
wait for the worker publication, show busy/pending state and report an
unconfirmed step with an actionable retry message. They do not change or save
the global draft. Seven scenarios cover own/ordinary/conference games, cup
shootout, sporting rounds, break proof and confirmed empty data.

The first two saved competitions supply order and per-competition display
rules, including special own treatment and overrides. The panel shows each
runtime association; missing saved associations use common rules. Overview
status uses published canonical ids and labels simulated datasets separately.
Demo controls do not write real competition files, save synthetic provider
sources or set system time. Provider identities/names/events come from recorded
public replies, while timings, transitions and tables are simulated. This
supports repeatable device checks without replacing real-provider evidence.

Each selected Demo step fixes its simulated data and match time until another
step is applied. Display visits still use the saved durations and normal
manual/automatic rules. A manually held Table can therefore remain across
steps within the same phase; a zero manual-return time keeps it held.
"Nächster Bildschirm" enters manual navigation. It does not advance the
automatic sequence or the simulated match. Debug's Demo-only "Automatik
weiterschalten" requests the next automatic page/image boundary; the next screen
kind or competition follows only at the end of its visit. While a manual view
is held, "Zur Automatik zurückkehren" first resumes the first eligible automatic
entry (the current frame image in frame mode). Following clicks advance normally.
Phase, type, manual/frame state and automatic entry count explain the last
controller-bound state, not a successful physical redraw. Commands are disabled
while Demo is off or preparing. Normal timings remain; shorter visits, clock
acceleration and new stored Demo fields are excluded.

## Provider-selection ownership

The competition/club wizard reads only its acknowledged opaque query ID.
Another browser's result, a changed configuration or an expired query leaves
its draft/previous rows intact and reports a retryable error. Header Close,
Cancel and Escape stop polling and cancel only that query; late start replies
are cancelled by their returned ID. Closing/reopening cannot revive an old
editor. An unacknowledged start is not retried; only explicit busy rejection
has bounded retry. A lost browser/start response expires through the device's
30-second idle-read lease. Further pages reuse the loaded list's filters but
receive their own query identity.

## One global draft and save

Loaded settings and the editable draft are separate. Switching pages preserves
the draft; a global save bar lists differences and supports discard/save. The
client validates all fields against schema byte capacities, numeric bounds,
list maxima and the firmware's source/image relationships before sending.
An empty number is an error, not zero. Server errors open the relevant page and
identify the field. The server remains authoritative.

A control that cannot be used is disabled with a visible reason next to it
(for example a full list, a missing save, a full image library or an
incomplete wizard step), so a greyed-out button is never left unexplained.

A listed competition always needs a competition id, including when paused; the
club is optional. Each competition has exactly one source; to follow the
same league at two providers, add it twice. Screen-policy enable and automatic
flags are independent; an empty automatic selection is allowed and must not
silently reactivate a disabled entry.

Passwords/keys have explicit keep/change/remove modes. Empty change input means
keep; only Remove sends an empty value. Unsaved summaries never reveal secret
contents. Secret inputs use new-password autocomplete. Editing is locked while
saving so input made during a login/network wait cannot be marked as saved.
Normal saves preserve the session unless the admin password changes.
Save sends the complete desired configuration and optional names in one Apply
with the expected revision and a unique64-bit ID. The device validates the whole
candidate, stages changed names and publishes one NVS reference. No part ordering
or two-step provider-key change is needed. A rejection retains the whole draft.
A received success receipt establishes the new baseline; later edits remain
unsaved. A failed follow-up GET shows an amber verification warning and a
read-only **Check status again** action. It never resends an acknowledged write.

After a lost response or uncertain NVS outcome, the client retains the Apply ID
and blocks another save. GET-only reconciliation accepts a matching receipt.
Reading the old revision does not prove the write absent; the page requires
reboot/recovery to establish durable state. A newer different revision establishes
a conflict; the draft is retained for reconciliation. Errors and warnings remain
visible above dialogs. No uncertain operation silently becomes a success.

Network-affecting changes include WiFi credentials, setup password, antenna
and hostname. The page explains reconnection and shows the new hostname/IP
addresses before saving. It does not relax origin checks or enable cross-origin
access: a changed .local hostname is reached by navigation/the displayed link.

## Competitions

Lists contain real entries without holes: at most eight competitions and
five library slides. Competition order sets own-match priority and order within
each sporting type. Entry details expose own special handling separately from
active state; phase/type eligibility and overrides live in Display. Without a
club there is no own focus or static club Single. The current display controller
uses the same effective screen policy exposed by the Display page.
Competitions, provider credentials and screen preferences are saved together
through the common Apply. The device assigns new identities once; moving an
entry keeps its identity and dataset. Switching only the preferred club keeps
the source's match/table/coverage facts and reclassifies their own-club view.
Chosen provider names are stored for display; ids remain the identity. Names
exceeding the storage byte capacity can be edited before accepting the wizard.
Changing a competition clears its previous club selection.

Provider lists are asynchronous, bounded pages. Only successful retrieval
advances paging; busy jobs are waited for and their errors are translated.
While the provider's window limit or a "too many requests" pause holds the
list, the wizard shows "Waiting for the request limit until hh:mm:ss (Code
limit)" (code `retry` for the pause) under the load button, in the
browser's local time; the device keeps serving the other providers
meanwhile. The page waits 150 seconds for a list, extended by the reported
wait.
Search/selection context cannot change underneath a running list request.
Typing in the search field only filters the entries already loaded, without
a request, and the load button then reads "Search"; "No results." appears when
no loaded entry contains the text. Enter or "Search" asks the device to search
the provider's whole list. The device search is a case-insensitive substring
match on the provider name (ASCII and accented Latin capitals such as Ä, Ö,
Ü); names are the provider's own, so ESPN leagues are found by English names
("German Bundesliga"). "Next page" requests the same list as the last load,
with an offset, because the device does not keep whole lists in RAM; a search
text typed since then keeps filtering the loaded entries. The competition
step and the club step each keep their own search text: the club step
starts empty, "Back" shows the competition search again, and choosing
another provider clears both.
For OpenLigaDB the wizard shows a season field above the competition list.
Empty loads the current season; a picked competition then stores no season,
so the device follows the season automatically. Another typed season loads
that season's list and is stored with the picked competition. Only one list
entry is marked as the current choice.
API-Football also stores no season for a picked competition, so the device
follows its current season automatically; the season shown in the list is only
passed to the club list request. An expert season override is stored as typed.
Provider keys and ESPN opt-in must be saved before their provider can be used.
Each provider card on the settings page also holds that provider's request
limits: the maximum requests per day (0 = no daily cap; when the rest will
not last until 00:00 UTC, the device requests live table, table,
fixtures and live scores less often or not at all, the own match last) and a
window limit,
at most N requests per window of W seconds (both 0 = no window limit; the
device never exceeds N in any W seconds). The change list and error messages
name the provider of a limit. The intervals per data type and the global
limit over all providers are stored settings, but the page does not show
them yet; they keep their values on every save.
When the device runs on initial values because its stored settings were
invalid, damaged, unreadable or not storable, the overview shows a warning;
for an invalid or damaged record the first save asks before it overwrites
the stored settings. Settings or competitions written by an older firmware
format are not read: the overview says they were discarded and asks to set
them up again; the old record stays until the next save replaces it. While the device is not connected to the home
network the overview shows "Kein WLAN" with the reason the display card
shows (network not found, wrong password, setup network on, connecting).
Each competition row of the overview shows its provider, match count and
the time of its last successful request, and a state tag (current, stale,
loading, "Gestört" after a failed request, "Nicht gepflegt" when the
provider leaves past matches without a result, "Unvollständig" when not
all data of the competition fit the device's store). A failed competition
adds the cause, what to do and the error code ("Fehlercode: OLG E10·503"); an
unmaintained one names the other enabled providers, or asks to enable one
under Settings; an incomplete one names what is missing (match list,
table, team names, match events) and asks for fewer or smaller
competitions ("Fehlercode: ESPN E19"). The two notices use the warning
tag and offer no retry. Every error message of the Web UI follows that pattern —
what failed, what to do, a short error code — without a serial console.
Below the data age the overview shows when the next provider round is due
("in 45 s" up to two minutes ahead, otherwise the browser's clock time) and
why: match live, before kickoff, after the final whistle, no match window
open, match window opens, daily limit tight (requests less often or skipped),
daily limit reached, retry after an error, waiting for WiFi and time, update test
running.
The device refuses a save in which an active competition uses a provider
without its key or opt-in, including a save
that removes a key still in use; the page then marks that source's provider.

Competition entries that are new or changed show a notice that the device
fetches them only after the global save. Removing a competition asks for
confirmation, names it and takes effect on save. The wizard
asks for a country filter only for API-Football, whose competition list
supports it; the Next button shows why it is not yet available.

The competition's names action opens one collection editor in the common
application draft. German, English and short fields distinguish inherited
(absent) values from explicit empty values. The UTF-8 adoption preview shows
26B names and3B shorts; full input is checked before clipping. Closing invalidates
pending reads, and late responses cannot replace another dialog or draft.
Changes become durable only through the global Save/Common Apply action.

The maintained [catalogue](../include/team_names.json) contains all147 supplied
clubs from Bundesliga,2.Bundesliga,3.Liga and the German regional groups.
Loading it, selecting a proposal and previewing an import change only the draft.
Confirmed provider IDs remain identity; coincident short/name texts never group
competitions or connect clubs automatically.71 preferred English names match
checked ESPN fields verbatim;76 do not have that verbatim evidence, including40
with confirmed club identity. Provider aliases do not silently replace a
preferred translation. Full provider-original acquisition remains a separate
explicit provider operation; clipped fallback labels cannot recover originals.

Collection import replaces the complete names draft after confirmation and
clipping/removal preview. It retains every provider-ID array, alias, league and
explicit exception. It does not impose a48-row exception limit. Source JSON
version1 uses full text values. Adopted export and Apply use version2 with a
shared `texts` dictionary and numeric references, so repeated long metadata is
sent once. The browser expands references for editing and deduplicates/clips
only at the confirmed adoption boundary. Names export includes the entire
adopted collection, not only fetched teams or the currently visible page.

The firmware enforces16KiB for the complete binary collection, including
identity/index/CRC bytes. A transport-body limit is separate and does not
allocate an equally large RAM buffer. Names use two immutable versions selected
by the exact reference in NVS AppConfig. One Common Apply saves configuration
and that reference together. If the result is uncertain, the browser checks
state without repeating the mutation and preserves the draft. A referenced
missing/corrupt file reports an error without discarding other settings.

Ordinary restart/power loss preserves adopted names. The existing confirmed
factory reset deletes them; the immutable firmware catalogue survives. Full
storage formatting is part of that reset. There is no names backup in RAM/NVS,
automatic repair rewrite, football-cache eviction or original-name archive.
Image-only deletion remains separate. Device transfer timing, heap/stack and
physical display acceptance remain open.

Original provider labels can be fetched explicitly in the names editor for a
saved competition. The query uses the existing worker, provider budgets and
absolute operation deadline; closing the dialog cancels its ownership. Complete
original full/short strings are shown, with full names and aliases used for
unambiguous matching before display clipping. An existing provider ID binding
takes precedence. Ambiguous equal names require an explicit target choice.
Adopting into an existing or new club changes only the shared names draft.
Other metadata, bindings and competition grouping remain intact; Save adopts
labels within the normalized16KiB collection envelope and26/3 display bounds.

## Display page

Five phase tabs expose six screen types with independent enable/automatic
flags and visit durations. Sporting rules inherit shared values unless a
competition explicitly overrides the field. One common type order applies;
competition order supplies the order within each sporting type. Empty automatic
selections stay empty. The page describes effective total/page times rather
than offering an independent table-page clock.

Timing/sports options include the shared minimum-two maximum visible matches
for Conference/Matchday, balanced pinned pages, table rows, conference mode,
shown matchday and manual return. Appearance options hold colours, backgrounds,
text scale and subpage dots. Help explains each non-obvious option. The common draft and phase/appearance/timing sections are implemented;
physical screen readability remains a separate device check.

Crest and library Images are separate screen types. The image sequence holds
only library ids; frame mode can additionally include the crest. Display selects
images but does not upload them. Replace/reset the protected crest/background
on Images. All policy choices share the global draft/save flow. The current
Overview reports screen/reason as text; it does not render a simulated LCD.
See [screen-policy integration](#screen-policy-integration--2026-10-17).

## Image editor and storage

Uploaded JPEGs form a library of ids 1–10; id 0 means the stock image of the
role (crest or background). Backgrounds and slides can reuse one image
without spending quota twice. The library sequence excludes id0; crest is a
separate type and an optional frame image. The crest is
not a display setting: it is always the crest role's image, the replacement
when one exists, else the factory crest. The gallery shows thumbnails using
per-image revisions and describes every current use. A referenced missing
file stays in the gallery as "Datei fehlt" / "File missing", with the same
Replace and Delete actions. Uses include saved settings and the current
unsaved draft, including disabled screens whose image choice is retained.
Replace uploads to the same id, preserving screen and slideshow selection;
successful validation/revision makes the display return automatically.
The current missing background choice remains visible in its selector, but
missing files are not offered as new slideshow entries or role-copy sources.
Explicit Delete clears that id's saved/draft references; Delete all includes
referenced missing ids and clears only successful deletions on partial failure.
New image allocation also reserves selected missing ids: saved uses on the
device and unsaved uses in this browser. The existing capacity text
distinguishes actual files from free ids; a missing reserved file uses no
image bytes but still occupies its wanted id. When all ten ids contain files
or references, remove an image/reference before adding unrelated content.
Replace can still restore the intended id directly.

The image page shows two fixed tiles beside the library: "Standard-Wappen"
(crest) and "Standard-Hintergrund" (standard background). Each shows the
stored role file (replacement or factory), its state ("eingebaut" for the factory image or
"Ersetzt" with the replacement's size) and where it appears (the crest type/
optional frame image, or screens using the standard background). They cannot
be deleted. Three actions:
"Ersetzen" opens the image editor and uploads the result as the role's
replacement; "Aus Bibliothek übernehmen" copies a chosen library image on the
device (the library image stays, the copy counts towards the quota and is
disabled while the library is empty); "Werksbild wiederherstellen" deletes the
replacement after a confirmation (disabled while the factory image is shown).
Replacements count towards the shared library quota but not towards the ten
library entries.

The editor loads a stored library/role JPEG when opened, or decodes a selected
local source file. Late loads cannot replace a newer file or a closed/reopened
editor. A missing/corrupt stored source keeps the stored file and offers local
replacement. Stored JPEG pixels do not recover the original source/crop history;
further edits re-encode those pixels.

Cropping uses the device profile resolution. Drag and horizontal/vertical numeric
inputs share the same offsets in device pixels:0 centers, positive is right/down,
negative left/up. Decimal values are retained; an empty/invalid value restores
the previous position with a warning. Numeric zoom, fit/fill, mouse wheel,
two-finger zoom, base colour, opacity and sample text remain available. Fit/fill
recenter and update both inputs. The editor mask/canvas share the profile;
sample text is not stored. Bounded JPEG encoding reports actual quality/bytes;
upload locks its intended source/target and has a progress indicator.

Gallery/role/slide images render as blocks, removing the reproduced inline
baseline space between image and round frame. Desktop/phone browser bounds now
align; this does not establish pixel agreement with the physical panel. The
existing gallery/slide masks still assume round images, a separate limitation
for rectangular profiles. No guessed device crop shift was introduced.

Library quota and temporary filesystem workspace are separate checks. Replacing
an image subtracts its old size from the library quota, but the old file still
occupies filesystem space while its replacement is written. Admission requires
available space of at least the larger of the candidate size and the 40 KiB
workspace; writing consumes that workspace. It is not charged a second time or
reserved for unused future library capacity. Actual filesystem allocation can
still fail; every write, synchronization, close and final rename is checked.
Upload and library-to-role copy use this same temporary-file pipeline. Both
validate the actual copied bytes for baseline JPEG, dimensions and complete
decode before replacing the published file. A failure preserves that file.
An expired validation deadline, including time spent in file I/O, is
answered as `timeout` ("try again"). A confirmed read, seek or close failure
is an I/O error; rejected format, dimensions or JPEG content is `content`.
An already expired deadline stops before opening or decoding the file.
Adopted names and stock images also occupy the partition and are included in
actual used space, but not the image-library quota. Football data is RAM-only.

| Bound | Current C6 |
|---|---|
| One uploaded file | 32,768 B |
| Unique library-file quota | 327,680 B |
| Filesystem replacement reserve | 40,960 B |
| LittleFS partition | 458,752 B including filesystem metadata |
| Library entries | 10 |

Single deletion is confirmed with the image's saved/draft uses. Delete-all uses
individual deletes for the library, then resets the crest and the standard
background to their factory images, and reports each result, including
partial failure. After
image mutations the page reloads inventory/settings, clears only explicitly
deleted image references and preserves other missing references and unrelated
unsaved edits. Refresh failure must not
hide an earlier destructive-operation error.
Deletion reports saved-reference cleanup and file removal separately. If
references were saved but unlink failed, the page explains that the file remains
and offers a later explicit retry. If the settings write has an unknown outcome,
delete-all stops, sends no further mutations and asks for a restart/state check.

The repair action opens the existing factory-reset dialog with full formatting
selected. It explicitly warns that setup, names, uploads and stock files are
removed, and retains the existing hostname confirmation. Settings and request
counters reset before formatting; failure reports the failed stage and completed
steps and permits retry. Only complete success restarts. Reconfiguration or an
existing backup restores setup; the firmware name preset stays intact. There is no standalone repair endpoint
or older-format alias.
USB uploadfs restores stock files and can itself erase uploads. Firmware OTA
preserves shared image storage; trial firmware blocks persistent image changes.
Images are not part of settings backup/import. Missing/damaged desired images
use the protected role replacement, then its factory file. A damaged replacement
uses the unchanged factory file.

If every candidate fails, football content stays on the configured base colour
with a warning; the pure crest view shows a repair card. The desired settings
reference and every slideshow position remain intact. After ten seconds, or
immediately after a file revision changes, the device checks the desired file
again and returns on success. This never automatically deletes files or saves
changed settings. Explicit manual deletion still clears only the deleted id;
explicit storage repair/reset keeps its confirmed destructive semantics.

The overview and Images page show the selected reference, failed file kind,
actual fallback (or absence of a usable file), cause, action, short code and
next retry. Debug device diagnostics retains the image state as well. Status polling
clears recovered errors while preserving unrelated unsaved edits. The live
schematic preview reads the actual selected file, including factory fallback
with a damaged replacement still present. Its status pill remains visible
while the state lasts; the device uses its existing five-second rule. A
simultaneous network/provider and image warning combines their codes plus a
Web hint, for example "NET E11 · IMG E03 · Web". Only then does the individual
action text yield. A crest repair card already carries the image code, so a
different network/provider pill can retain its own action.

Image file faults are content errors: a working UI with this fallback/error
presentation may confirm firmware during the existing ten-second OTA trial.
Confirmation does not guarantee successful JPEG output. Hardware/LVGL fatal
rules remain independent; network availability is not an OTA verdict. Firmware
rollback cannot repair image files shared by both slots. Startup no longer runs
a legacy-image cleanup mechanism.

The shipped stock crest/background are generated project artwork, not downloaded
club crests. Normal firmware does not resize images; host stock generation uses
scripts/build_assets.py. Automatic provider-crest import is still deferred.

## Debug and screenshot capture

Next, IP and Demo automatic-advance commands use the revision of the last-read
saved configuration. Draft edits do not change that revision. If another
configuration was activated, the command fails without being applied; reload
the configuration before sending a new command. The page never silently retries
that rejected action against a newer revision.

Under **Debug → Bildschirmfoto**, **Bildschirmfoto herunterladen** downloads a
real redraw as a PNG, independently of the schematic overview preview and
the optional live log. The adjacent `?` explains the five-second maximum
display pause, login, orientation and capture limits. The existing session
policy applies: login with the configured admin password, otherwise a silent
session. The live-log switch need not be enabled.

The PNG keeps the display's current reading orientation, with transparent
pixels outside a round display and a local date/time in its filename. The
device streams copied strips rather than retaining a full frame or encoding
PNG. No screenshot is saved in device storage. A forced redraw captures
LVGL's new output, not existing LCD RAM or physical panel faults.

An incomplete or malformed transfer never becomes a download. The action is
available again after an error; messages give an action and API/HTTP code or
local `SCREEN E01` (timeout), `E02` (connection), `E03` (incomplete/invalid
format), `E04` (browser PNG creation). A busy provider/upload asks the owner
to retry, while a RAM shortage asks them to let other work finish.

Export optionally includes secrets after an explicit notice. One protected
request returns the current-version desired settings, competitions, screens
and complete adopted names collection. Images remain separately managed.
Import accepts that current version, validates its complete shape and loads it
into the shared draft after confirmation. It sends no configuration write.
Only Save publishes one common Apply. Old development formats are rejected;
there are no legacy backup transforms or partial section commits.

Restart and factory reset are separate confirmed actions. Factory reset
requires the device name and explains reconnection to setup afterwards.

Firmware upload checks the file's descriptor, target/layout, settings format
and actual OTA capacity before transmission; firmware validates the image
again. The page remembers the expected full ELF build identity and previous
identity, waits for the device, and distinguishes a running trial, confirmed
new build, return to the previous build and an unresolved result. Equal version
strings alone are not treated as proof of an update.

Diagnostics show clock/network/signal/reset reason, free/minimum/largest heap,
filesystem use, request counters, trial and task stack minima. The heap-minimum
label distinguishes "since boot" from a measurement interval; an unavailable
minimum is shown as unavailable. Each task shows its minimum free bytes,
usable stack bytes and percentage when the usable capacity is known. This
capacity comes from the kernel's aligned bounds, including SDK tasks, rather
than a second list of configured allocations. Unknown capacity, including the
current SMP case, is shown as unavailable. Firmware storage shows the running
image's parsed bytes against the existing OTA-slot capacity and its percentage;
missing metadata is shown as unavailable. The current targets use unsigned
images, so the reported length includes the appended image hash but does not
promise accounting for signature blocks.

Integration acceptance uses 40,960 B minimum free heap, 32,768 B largest free
block, at least 25% free usable stack per task and at most 85% app-slot
occupancy. The displayed point values and percentages are diagnostic evidence;
they do not establish that every required workload or unobserved memory dip
meets those limits. The optional
WebSocket sends bounded current state, not the SDK console. It has one client,
no retained history, and closes when its page/session ends. Admission is checked
before upgrade; a browser can only report a failed WebSocket connection, not
its refusal status. Console health logging is a separate setting.

## Acceptance boundary

The screenshot client has production-page browser evidence at 1440×900 and
375×812 against synthetic RGB565 records: native PNG downloads, exact colors,
all-pixel round transparency, dated names, German/English help and recoverable
format/busy errors. This verifies browser composition and controls, not LCD
pixels, radio transfer time or device memory reserves. The owner must flash
the C6 candidate and compare captures with each real screen and rotation.

The rewritten source is checked with targeted host logic/render tests. These
are not a simulated-device acceptance. The actual page was loaded locally with
no device API to verify its visible retry/start failure state. Full device
behaviour, phone gestures, provider lists, actual uploads, recovery, sessions
and live WebSocket operation remain owner/device acceptance after USB upload.
No OTA/reset/storage-format action has been executed on owner data.

Overview currently reports screen/reason as text. Physical layout acceptance
uses device screenshots; a future graphical preview must use the actual device
draw list. Brightness and night controls appear only on a target with a
controllable backlight pin. The current C6 with GC9A01 has no such pin and
offers neither control. On a capable target, night mode changes only the
backlight; colours, background and image opacity stay unchanged. See UI.md and
NETWORK.md and API.md for the display and server contracts.


## Screen-policy integration — 2026-10-17

Five phase tabs show six compact type rows. A sporting row has shared enable,
automatic and visit settings; its competition expander can override each field
independently. “Shared” inherits immediately, explicit No remains No, and
collapsing an override preserves its stored values. A manual-only entry retains
its visit time while the field is hidden. Image visits must be whole multiples
of the separate image time; invalid values are explained instead of rounded.

The browser uses one saved aggregate and one draft. Settings, competitions,
screen rules and edited names apply in one revision-checked request. Each
competition carries its own fieldwise display overrides. Browser-only draft
keys are removed from requests and exports. Failed/stale writes preserve edits;
an uncertain outcome is checked by Apply ID using GET only, with no mutation
retry. An uncertain NVS result keeps writes disabled until restart.

The competition page explicitly selects group members, shows the derived club
heading, and offers edit/dissolve actions. Names and matching labels never create
groups. The highest member priority places the group; members retain their list
order. The club wizard visibly retains an existing membership during a club
change and offers removal. “Only one own single match” defaults on; switching
it off admits further eligible own singles in the same type/competition sequence.

Image deletion/recovery may monotonically remove the explicitly affected image
references. Other policy or competition changes cause a visible conflict instead
of being silently merged. Confirmed file deletion and already saved reference
cleanup are reconciled separately after partial failures.

Client timing, polling and retry budgets have one owner in the central firmware
configuration. The offline generator embeds that exact table for bootstrap;
the published schema exposes the same values. A common bounded busy retry
preserves query cancellation and never repeats an unacknowledged write. Inputs
continue to use the published schema's field bounds.

Current production-function tests cover explicit grouping, group edits without
source changes, numeric club boundaries, independent nullable overrides and
common Apply outcomes. Earlier browser evidence below describes the previous
wire format. No current desktop/phone browser or physical-display acceptance is
claimed for this model cutover; those remain separate checks.

## Current control/Demo verification — 2026-10-17

Twelve production-script scenarios use the actual current C6 schema and
settings/competition/policy codecs plus the canonical Demo catalogue, with a
synthetic local service. They cover shared validation, false/inherited overrides,
draft/save acknowledgements, busy retries, competition identity reorder,
backup/import shape, Demo steps and exact/stale/unbound status labels. A browser
at1440×1000 and390×844 checked scenario/step application, displayed bindings,
expert section persistence, rejection of minimum1 and restoration to4. Neither
viewport had horizontal overflow or console errors. These checks do not run
device HTTP/NVS/provider scheduling or image decoding; complete wizard/import/
image-editor browser acceptance remains separate. The existing screenshot flow
and owner-flashed device checks remain the display evidence boundary.

## Owner feedback follow-up — 2026-10-17

Focused fixes are implemented before the full settings redesign: one Debug
screenshot, a default-off additional Overview screenshot, Demo automatic boundary
advance with manual return, truthful acknowledged-save verification, retained
name drafts/closed-dialog fences, stored-JPEG editing and explicit X/Y controls.
The Web thumbnail baseline gap is reproduced and corrected; the physical
mask comparison and the owner's Table-only device path still require device
state/screenshots. A fixed Demo step and a manually held Table can legitimately
remain while the normal saved rules apply; no guessed selector cause is claimed.

Current scoped host checks:50 native cases (sequence/general record/input),22 save
feedback,22 screenshot,16 Debug controls,23 image-editor,6 name drafts and7 name
close/race cases. The earlier12 current-codec scenarios remain separately
recorded. Production-page browser checks cover desktop1440 and phone390 placement,
save warning/recovery, Overview opt-in, stored JPEG loading, explicit positioning
and exact image/frame bounds. Services/data are synthetic; they do not establish
NVS, provider scheduling, device rendering or physical runtime reserves. Complete
wizard/import, physical capture and upload/reload acceptance remain open.

### Shared field help

General settings, secrets and screen controls use one contextual `?` button
implementation. Complete explanations remain available in the help dialog;
short allowed ranges and text limits stay next to their controls. Long provider
limit/password descriptions no longer repeat as always-visible paragraphs on
the Settings page. Provider configuration uses the same expandable sections as
other infrequent settings; all four providers remain visible by name and
capability, with credentials and limits inside their own section. Open state
survives normal page redraws. This changes no option, saved value or API contract.

JSON mutations, binary screenshot requests and XHR image/OTA uploads use one
session-admission function. Only an explicit expired-session rejection renews
authentication and repeats the exact operation once. Password cancellation,
other rejection codes, timeouts and lost connections never replay a mutation.
The current nested screen-mode fields render readable old/new choices in the
shared change list. Legacy top-level mode labels/routing have been removed.

Club selection accepts only numeric positive IDs in current list responses.
Malformed items cannot replace an existing list, while an explicitly empty
array is a valid empty result. Selection, name import and explicit-original
validation share the firmware-published team-ID maximum; the browser has no
independent uint32 ceiling. The expert field explains that positive range and
the separate No club choice. Existing numeric selections survive list reload.
