# Web UI

The embedded page is a single offline HTML/CSS/JavaScript application with
German and English text. It uses no framework, CDN, web font or browser build
step. Firmware and page are updated together. Current C6 source/build evidence
and pending device acceptance are recorded separately below.

## Six pages

| Page | Content |
|---|---|
| Overview | Current screen/reason, own match or kickoff, global data age, source/match count/error per competition, warnings and refresh/next/IP actions |
| Competitions | Ordered active/paused entries, add/edit wizard, provider competition/club lists, fallback explanation, move/remove, saved-competition fixture picker |
| Display | Situation defaults, four screen styles, background/image references, crest and ordered slideshow, brightness/night/rotation, badges and expert scroll controls |
| Images | Revisioned thumbnails, usage, quota, upload/replace editor and confirmed real deletion |
| Settings | WiFi scan and credentials, hostname/antenna, provider keys/opt-in/budget, time/language/access and expert diagnostics |
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
API-Football daily budget is a separate setting; the boot counter is not
displayed as daily consumption or as a fraction of that budget.

The overview shows the current screen as a schematic disc drawn with the
renderer's geometry (round or rectangular, device resolution), the saved
colours, text size and background/slide image, the status line and the IP
badge as the display would show them. The own match uses the device's real
data (teams, score, minute or kickoff, phase). Values the status API does not
report — conference rows, table rows and the event line — are drawn as example
values, and the caption names them as such. The kickoff is shown in the
browser's time zone. The preview is explicitly a schematic, not a screenshot;
it does not show approximate-value markers or night dimming. Below it, one
sentence explains why this screen is shown (own match, matchday, idle with the
next own kickoff, manual choice with its return time, setup). While the first
provider cycle has not published a snapshot the overview says that data is
loading.

Theme follows the browser by default; a manual light/dark choice is stored only
in the browser. Form/navigation text follows the draft language choice. Status
polling updates its own nodes and does not replace unsaved form input.

## One global draft and save

Loaded settings and the editable draft are separate. Switching pages preserves
the draft; a global save bar lists differences and supports discard/save. The
client validates all fields against schema byte capacities, numeric bounds,
list maxima and the firmware's source/pair/image relationships before sending.
An empty number is an error, not zero. Server errors open the relevant page and
identify the field. The server remains authoritative.

A control that cannot be used is disabled with a visible reason next to it
(for example a full list, a missing save, a full image library or an
incomplete wizard step), so a greyed-out button is never left unexplained.

A listed competition always needs competition/club ids, including when paused.
An active fallback needs a complete source from another provider. Active pairs
must not repeat an id on the same provider side within their competition; ids
from different providers may be equal. At least one screen remains enabled.

Passwords/keys have explicit keep/change/remove modes. Empty change input means
keep; only Remove sends an empty value. Unsaved summaries never reveal secret
contents. Secret inputs use new-password autocomplete. Editing is locked while
saving so input made during a login/network wait cannot be marked as saved.
Normal saves preserve the session unless the admin password changes.

Network-affecting changes include WiFi credentials, setup password, antenna
and hostname. The page explains reconnection and shows the new hostname/IP
addresses before saving. It does not relax origin checks or enable cross-origin
access: a changed .local hostname is reached by navigation/the displayed link.

## Competitions and fixture pairs

Lists contain real entries without holes: at most three competitions, eight
fixture pairs and five slides. The first active competition supplies the table.
Chosen provider names are stored for display; ids remain the identity. Names
exceeding the storage byte capacity can be edited before accepting the wizard.
Changing a competition clears its previous club selection.

Provider lists are asynchronous, bounded pages. Only successful retrieval
advances paging; busy jobs are waited for and their errors are translated.
Search/selection context cannot change underneath a running list request.
For OpenLigaDB the wizard shows a season field above the competition list.
Empty loads the current season; a picked competition then stores no season,
so the device follows the season automatically. Another typed season loads
that season's list and is stored with the picked competition. Only one list
entry is marked as the current choice.
API-Football still requires the chosen season while its source is used; the
wizard takes it from the provider result and permits an expert override. It
does not promise automatic empty-season resolution before that service exists.
Provider keys and ESPN opt-in must be saved before their provider can be used.

Competition entries that are new, moved or changed show a notice that the
device fetches them, and fixture pairs can be created for them, only after the
global save. Removing a competition asks for confirmation, names it and the
number of fixture pairs removed with it, and takes effect on save. The wizard
asks for a country filter only for API-Football, whose competition list
supports it; the Next button shows why it is not yet available.

Fixture picking uses unchanged saved competitions at their saved indices.
New, edited or reordered competitions require the global save first. It loads
primary and fallback fixtures with ids, labels and kickoff, clears stale picks
on reload and requires current valid selections before accepting. A home/away
swap suggestion remains explicitly reviewable; the firmware never equates
provider identities by similar names. Moving/removing competitions updates
pair indices in the draft.

## Display page

Situation defaults (outside matchdays, matchday, own match) list only enabled
screens. A stored choice whose screen has since been switched off stays
visible as "(off)", together with the note which screen the display uses
instead. Own match is not offered for the matchday situation, because the own
club does not play then; an imported backup with that value is changed to
Conference before validation.

Each of the four screen cards has an enable switch, a live schematic preview
with example values that follows every draft change (colours, text size,
background, crest and slides), a background-image choice and a colour group.
The global background switch sits above the cards; while it is off, the
per-screen choices keep their values and say so. The conference card holds
the switch that pins the own match in the conference. It is stored, but the
current display always keeps the own match in the middle row, so it has no
visible effect yet. Display, provider-budget and diagnostics settings whose
meaning is not obvious have a help text, including the device-specific
conditions (for example the API-Football daily budget and its reset at 00:00
UTC, or the scroll reset that only applies to devices with exactly two
buttons).

The crest field and each screen's background field offer "Upload new image"
next to their choice (slides are added from the library list). It opens the image editor for that field; after a successful upload
the new library image is assigned to the field in the draft and the page
returns to it. "Back to display" leaves the editor without changes. The
assignment is saved with the global save like any other draft change.

## Image editor and storage

Uploaded JPEGs form a library of ids 1–10; id 0 means the stock image. Crest,
backgrounds and slides can reuse one image without spending quota twice. A
slideshow id 0 means the selected crest. The gallery shows thumbnails using
per-image revisions and describes every current use.

The editor decodes a local or stored image, preserves the old image on decode
failure, and crops at device resolution. It supports drag, numeric zoom, fit/
fill, mouse wheel, two-finger zoom, base colour, opacity and sample text. The
round mask follows the profile. Sample text is not stored. JPEG encoding tries
bounded quality levels before upload and reports the actual quality and bytes.
Upload has its own progress indicator and locks the intended source/target.

Library quota and temporary filesystem reserve are separate checks. Replacing
an image subtracts its old size from the library quota, but the old file still
occupies filesystem space while its replacement is written. The firmware
performs its own baseline-JPEG, dimensions, complete-decode and quota checks.

| Bound | Current C6 |
|---|---|
| One uploaded file | 32,768 B |
| Unique library-file quota | 327,680 B |
| Filesystem replacement reserve | 40,960 B |
| LittleFS partition | 458,752 B including filesystem metadata |
| Library entries | 10 |

Single deletion is confirmed with the image's saved/draft uses. Delete-all uses
individual deletes and reports each result, including partial failure. After
image mutations the page reloads inventory/settings, removes missing image
references and preserves unrelated unsaved edits. Refresh failure must not
hide an earlier destructive-operation error.

Separate storage repair requires confirmation. It erases uploads AND stock
files, preserves other settings, does not restart and reports a failed
reference save as a partial result. USB uploadfs restores stock files and can
itself erase uploads. Firmware OTA preserves shared image storage; trial
firmware blocks persistent image changes. Images are not part of settings
backup/import. Missing image references use the stock role or skip the slide.

The shipped stock crest/background are generated project artwork, not downloaded
club crests. Normal firmware does not resize images; host stock generation uses
scripts/build_assets.py. Automatic provider-crest import is still deferred.

## System and diagnostics

Export optionally includes secrets with an explicit notice. Import checks size,
JSON shape, types and the same validation rules before confirmation (an old
matchday default of Own match is converted to Conference first); successful
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
filesystem use, request counters, trial and task stack minima. The optional
WebSocket sends bounded current state, not the SDK console. It has one client,
no retained history, and closes when its page/session ends. Admission is checked
before upgrade; a browser can only report a failed WebSocket connection, not
its refusal status. Console health logging is a separate setting.

## Acceptance boundary

The rewritten source is checked with targeted host logic/render tests. These
are not a simulated-device acceptance. The actual page was loaded locally with
no device API to verify its visible retry/start failure state. Full device
behaviour, phone gestures, provider lists, actual uploads, recovery, sessions
and live WebSocket operation remain owner/device acceptance after USB upload.
No OTA/reset/storage-format action has been executed on owner data.

The display redesign is not included in this step. The existing display still
has its old fonts/layout/input behaviour. On the C6 without backlight PWM,
current night rendering darkens background/images; the stored night-brightness
value does not yet implement foreground colour dimming. See UI.md and
NETWORK.md for the display and server contracts.
