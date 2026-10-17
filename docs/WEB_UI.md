# Web UI

The embedded page is a single offline HTML/CSS/JavaScript application with
German and English text. It uses no framework, CDN, web font or browser build
step. Firmware and page are updated together. Current C6 source/build evidence
and pending device acceptance are recorded separately below.

The dedicated screen-policy options are available in the current source. They
share the existing draft/save flow with general settings and competitions.
The active controller now uses the same policy and common sequence. The
frame request-admission gate applies before the first HTTP write.
The [screen-system contract](UI.md#planned-screen-system-redesign) distinguishes
this staged implementation from final device behavior and layout acceptance.
The overview contains frame mode; Display contains phase rules, common order,
appearance and library-image sequence. Image files remain managed under Images.

## Six pages

| Page | Content |
|---|---|
| Overview | Frame mode, current screen/reason, own match or kickoff, global data age, next provider round and its reason, source/match count/data time/cause, action and error code per competition, warnings and refresh/next/IP actions |
| Competitions | Compact ordered list of active/paused entries with a Details expander (season, own special treatment, move/edit/short names/remove), add/edit wizard, provider competition/club lists |
| Display | Five phase tabs, six types, shared rules and retained per-competition overrides, common type order, visit/image times, separate crest and library images, appearance, brightness/night/rotation, badges and input roles |
| Images | Revisioned thumbnails, usage, quota, upload/replace editor, confirmed real deletion, and the crest and standard background (replace, copy from the library, restore the factory image) |
| Settings | WiFi scan and credentials, hostname/antenna, provider keys/opt-in and request limits (daily cap, window limit) per provider, time/language/access, expert Demo scenario/step controls and diagnostics |
| System | Firmware/update, backup/import, restart, separate image-storage repair, delete user images, factory reset and live diagnostics |

The page starts with session discovery. Without an admin password it obtains
its mutation token silently and shows no login/logout controls. With a password,
reads remain available and exactly one login/logout control is shown. Schema,
settings, status and images then load independently. A failed load has a visible
retry card and automatic retry; it never leaves an unexplained blank page.
Status failures mark the connection as unavailable even when older data exists.
Short notices appear at a fixed position near the bottom of the window, in the
browser's top layer so they stay visible above open dialogs; browsers without
that layer show them as an ordinary fixed element.
System diagnostics label provider request attempts as "since restart". The
daily caps are separate settings; the boot counter is not displayed as daily
consumption or as a fraction of a cap.

Local network-service errors appear in the existing overview warnings and
System diagnostics. A working WiFi connection remains shown as connected
when only setup DNS, local naming or HTTP advertisement failed. The error
names the failed phase, likely cause, automatic retry time, short `NET E..`
code and SDK error; naming/DNS faults with an active IP advise opening that
IP directly. Normal status polling clears recovered errors and preserves
unsaved form edits. Local-service readiness is listed with the device
measurements, separately from the WiFi address.

The overview reports the current screen and its reason and offers the existing
real-device screenshot download. It does not generate a simulated display for
the new screen policy. Ground layouts and rings are chosen from owner-flashed
device drafts; a future preview must consume the device's actual draw list.
Status polling preserves unsaved controls and shows actionable network/image
errors separately from the overall connection state.

Theme follows the browser by default; a manual light/dark choice is stored only
in the browser. Form/navigation text follows the draft language choice. Status
polling updates its own nodes and does not replace unsaved form input.

## Runtime Demo controls

Under Settings → Demo and logs, enable Demo and save first. Load the scenario
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
automatic sequence or the simulated match. A Demo-only automatic-sequence
advance is selected as a follow-up, not yet implemented. Normal timings remain;
shorter visits, clock acceleration and new stored Demo settings are excluded.

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
Screen-policy read/save can report busy while a provider/storage operation owns
the shared memory token. The failed part remains in the draft; retry after the
operation ends. A busy error never marks an unconfirmed write as saved.

Known feedback defect, reproduced2026-10-17: after an acknowledged successful
write, a busy follow-up read can still display the generic save error. With all
changes acknowledged the Save bar disappears, even though the message suggests
retrying the save. This must be distinguished from a rejected write, which
retains the draft. The planned correction preserves write/conflict checks and
separates confirmation, readback warning and failed-save error; it is not yet
implemented. Current toasts have neutral and red-error states only.

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
The competitions are saved through their own endpoint
(`/api/v1/competitions`) as part of the global save; the device assigns each
new competition its id, so moving a competition keeps its id and its kept
data. When the global save changes both
the settings and the competitions, the page writes first the part that the
other one needs (for example a newly entered provider key before a
competition that uses it). A change that both removes one keyed provider and
moves a competition to another keyed provider must be saved in two steps.
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

"Kurznamen" in the details of a saved, active competition lists the teams the
device holds for it (from its matches and table), each with the provider's
name as placeholder and the owner's short name, if any. "Übernehmen" saves
one name at once on the device, outside the global draft; an empty field
removes it. At most 26 bytes per name and 48 names over all competitions;
a full table or a full device is reported and keeps the previous name. A
competition without fetched data shows a hint to fetch first. The names
are part of the settings export; an import replaces all of them (a backup
without names keeps them). A settings reset deletes them.

Known editor defect: saving one row, or rejecting an overlong row, rebuilds the
dialog and loses other typed edits. Batch editing with retained drafts is
requested. Editable/localized full names and approximate lookup tables are also
requested but do not exist yet; short-name overrides do not translate full names.
The requested catalogue covers Bundesliga,2.Bundesliga,3.Liga and German
Regionalligen. Each future editor row should expose German/English/short-name
inputs, exact-match prefill and a catalogue dropdown. User mappings should
export/import in the catalogue's format. A readable JSON source beside
app_config.h in include/ is proposed. The selected shape uses direct de/en/short
fields per club and provider IDs under ids. Exact fallback/import behavior,
resource costs and integration remain design work, not current API support.

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
text scale and subpage dots. Help explains each non-obvious option. Complete
settings/phase-option reorganization remains planned after device layout work.

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

The editor decodes a selected local source image, preserves the old image on decode
failure, and crops at device resolution. It supports drag, numeric zoom, fit/
fill, mouse wheel, two-finger zoom, base colour, opacity and sample text. The
round mask follows the profile. Sample text is not stored. JPEG encoding tries
bounded quality levels before upload and reports the actual quality and bytes.
Upload has its own progress indicator and locks the intended source/target.
Dragging changes both X/Y offsets; explicit position inputs are requested.
Opening an existing image currently does not load its stored JPEG for editing;
it asks for a source file. Gallery/slide masks are still always round, which
does not match rectangular profiles. The reported mask/image offset on the
round C6 remains under investigation and has not been attributed to that defect.

Library quota and temporary filesystem reserve are separate checks. Replacing
an image subtracts its old size from the library quota, but the old file still
occupies filesystem space while its replacement is written. The firmware
performs its own baseline-JPEG, dimensions, complete-decode and quota checks.
An expired validation deadline, including time spent in file I/O, is
answered as `timeout` ("try again"). A confirmed read, seek or close failure
is an I/O error; rejected format, dimensions or JPEG content is `content`.
An already expired deadline stops before opening or decoding the file.
The kept football data and the short names share the partition (at most
48 KiB, typically one 4 KiB block per competition); the free-space check of
an upload counts them, the library quota does not. Images come first: the
device writes a football file only while the unused part of the library
quota plus the reserve stays free.

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

Separate storage repair requires confirmation. It erases uploads AND stock
files, preserves other settings, does not restart and reports a failed
reference save as a partial result. USB uploadfs restores stock files and can
itself erase uploads. Firmware OTA preserves shared image storage; trial
firmware blocks persistent image changes. Images are not part of settings
backup/import. Missing/damaged desired images use the appropriate protected role replacement,
then its factory file. A damaged replacement uses the unchanged factory file.
If every candidate fails, football content stays on the configured base colour
with a warning; the pure crest view shows a repair card. The desired settings
reference and every slideshow position remain intact. After ten seconds, or
immediately after a file revision changes, the device checks the desired file
again and returns on success. This never automatically deletes files or saves
changed settings. Explicit manual deletion still clears only the deleted id;
explicit storage repair/reset keeps its confirmed destructive semantics.

The overview and Images page show the selected reference, failed file kind,
actual fallback (or absence of a usable file), cause, action, short code and
next retry. System diagnostics retains the image state as well. Status polling
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
rules and the strict network-startup-error veto remain unchanged; firmware
rollback cannot repair image files shared by both slots.

Startup maintenance only removes legacy named image files outside an update
trial, once storage is available. It waits without blocking when another
expensive operation holds exclusive access. Failed legacy removal stays
pending and retries after 10 s;
the waiting period holds no exclusive-operation admission. Desired image
references/positions are retained by the independent content fallback path.

The shipped stock crest/background are generated project artwork, not downloaded
club crests. Normal firmware does not resize images; host stock generation uses
scripts/build_assets.py. Automatic provider-crest import is still deferred.

## System and diagnostics

Under **System → Diagnose**, **Bildschirmfoto herunterladen** downloads a
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

Export optionally includes secrets with an explicit notice; it carries the
settings, the competitions and the owner's short names.
Import sends them as up to three requests (settings, competitions, short
names), each at most the API body limit; it checks size,
JSON shape, types and the same validation rules before confirmation, the
short names included (provider, team id 1…4,294,967,295, a non-empty name
within the schema's byte bound without control characters, every team once,
at most the schema's count), so an invalid backup sends no request at all. The
size bound applies to the file (three request bodies) and to each request
part, not to the combined data (an old
matchday default of Own match is converted to Conference first, in the data
that is sent; a backup of earlier firmware with fewer screen styles sets
those, the later screens keep their values; the crest
choice of an older backup is dropped, and the result message says so,
because the crest is an image role now and images are not in the backup; the
API-Football daily budget of an older backup becomes API-Football's daily cap,
and the result message says so as well; the minimum request spacing of an
older backup is ignored with a message; competitions of an older backup
without ids get the ids 1, 2, 3 in list order; the fallback sources and
fixture pairs of an older backup are dropped, the rest is imported, and the
result message says that each competition has one source; the core-data
interval of an older backup is ignored, and the result message says that
team names now come with fixtures and table); successful
import refreshes the settings and session state instead of rewriting an old
draft. Restart and factory reset are separate confirmed actions. Factory reset
requires the device name and explains how to reconnect to setup afterwards.

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
draw list. On a display without adjustable backlight (C6 with GC9A01) the
night mode turns the background almost black, shows images at 20 % opacity
and dims all colours according to the night brightness; the day brightness is
stored but only takes effect with an adjustable backlight. See UI.md and
NETWORK.md and API.md for the display and server contracts.


## Screen-policy integration — 2026-10-17

Five phase tabs show six compact type rows. A sporting row has shared enable,
automatic and visit settings; its competition expander can override each field
independently. “Shared” inherits immediately, explicit No remains No, and
collapsing an override preserves its stored values. A manual-only entry retains
its visit time while the field is hidden. Image visits must be whole multiples
of the separate image time; invalid values are explained instead of rounded.

The browser uses one saved model and draft. General settings, competitions and
screen policy are separate bounded requests. Each successful part is acknowledged
before proceeding; a later failure leaves only unconfirmed edits pending. The
policy token and comparison of fresh competition/policy content prevent silent
overwrite by another client. New/imported competition bindings use the identities
returned by the service. Backup includes the complete policy; import validates
before writing and uses the same partial-acknowledgement/retry flow. Browser-only
binding keys never enter an API request or backup.

Image deletion/recovery may monotonically remove the explicitly affected image
references. Other policy or competition changes cause a visible conflict instead
of being silently merged. Confirmed file deletion and already saved reference
cleanup are reconciled separately after partial failures.

Targeted production-JavaScript checks: 27/27, existing screenshot checks: 22/22.
Local synthetic-API browser checks covered desktop 1440×1000 and phone390×844,
phase controls/inheritance/explicit No, shared saving, frame toggle, partial
save failure/retry, image reference/deletion and export. Neither width had
horizontal page overflow. This is browser evidence, not device/NVS acceptance;
full wizard/import/editor flows and final control/Demo behavior remain open.
The unchanged older UI suite reaches 25 passing cases before an obsolete
five-style/fixed-table-time expectation; it was not altered to conceal this.

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

The current Overview exposes screenshot download without an opt-in diagnostics
switch. The planned Debug tab keeps screenshot always available near the top,
with Demo just below and no duplicated screenshot inside Demo. One default-off
preference additionally shows screenshot on Overview; it does not gate Demo or
the browser log stream. A Demo-only automatic-sequence advance with normal
timings is requested before the full redesign.
Their settings/API are not yet implemented. The current Overview screenshot
button also lacks the progress text shown by the System button; both use the
same guarded capture operation. The reported Table-only Demo and round-image
mask offset still need state-specific device evidence, rather than an assumed
cause. Targeted script reproductions confirm the save-feedback and short-name
draft-loss defects; fixes and complete physical acceptance remain outstanding.
