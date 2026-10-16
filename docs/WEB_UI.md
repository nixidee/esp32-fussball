# Web UI

The embedded page is a single offline HTML/CSS/JavaScript application with
German and English text. It uses no framework, CDN, web font or browser build
step. Firmware and page are updated together. Current C6 source/build evidence
and pending device acceptance are recorded separately below.

## Six pages

| Page | Content |
|---|---|
| Overview | Current screen/reason, own match or kickoff, global data age, next provider round and its reason, source/match count/data time/cause, action and error code per competition, warnings and refresh/next/IP actions |
| Competitions | Compact ordered list of active/paused entries with a Details expander (season, screen switches, move/edit/short names/remove), add/edit wizard, provider competition/club lists |
| Display | Situation defaults, five screen styles, background/image references, ordered slideshow (the crest is one possible slide), brightness/night/rotation, badges and expert scroll controls |
| Images | Revisioned thumbnails, usage, quota, upload/replace editor, confirmed real deletion, and the crest and standard background (replace, copy from the library, restore the factory image) |
| Settings | WiFi scan and credentials, hostname/antenna, provider keys/opt-in and request limits (daily cap, window limit) per provider, time/language/access and expert diagnostics |
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

The overview shows the current screen as a schematic SVG disc that follows the
display renderer: the same text roles and font steps for the display size and
text size, the safe-area width of every text band (round or rectangular,
device resolution and rotation), shortening with "…", the score, minute and
date pills, highlighted rows, page or slide dots, the IP chip at the top or
bottom, and the rim (match minute, scroll position, start steps or a coloured
state ring). Colours, text size and the background or slide image are the
saved values. Text widths are estimated from average glyph widths, so a name
may be shortened slightly earlier or later than on the device.

The overview binds the device status the way the display does: the start
screen with its three steps until the first data, the setup card, the
no-WiFi, waiting-for-time and no-data cards, the notice card of a failing
competition in the Matchday and Table cycles, the "Demo" line, the status
pill (no WiFi, waiting for time, the time of the last data when stale, the
error code of a failing competition or the number of failing ones) and the
IP chip with its configured duration. The device shows the status pill only
for 5 seconds after each change; the preview shows it for as long as the
state lasts and says so below the disc. The own match uses the device's
real data (teams, score, minute or kickoff date and countdown, phase). Values
the status API does not report — the matchday number, the event line,
conference and matchday rows, table rows and the setup network name — are drawn as example
values, and the caption names them as such. Times are shown in the browser's
time zone. The preview is explicitly a schematic, not a screenshot; it does
not show approximate-value markers or night dimming. Below it, one sentence
explains why this screen is shown (own match, matchday, idle with the next
own kickoff, manual choice with its return time, setup). While the first
provider request has not published data the overview says that data is
loading.

Theme follows the browser by default; a manual light/dark choice is stored only
in the browser. Form/navigation text follows the draft language choice. Status
polling updates its own nodes and does not replace unsaved form input.

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
club is optional (without a club the competition shows conference and table,
but no own match). Each competition has exactly one source; to follow the
same league at two providers, add it twice. At least one screen remains
enabled.

Passwords/keys have explicit keep/change/remove modes. Empty change input means
keep; only Remove sends an empty value. Unsaved summaries never reveal secret
contents. Secret inputs use new-password autocomplete. Editing is locked while
saving so input made during a login/network wait cannot be marked as saved.
Normal saves preserve the session unless the admin password changes.

Network-affecting changes include WiFi credentials, setup password, antenna
and hostname. The page explains reconnection and shows the new hostname/IP
addresses before saving. It does not relax origin checks or enable cross-origin
access: a changed .local hostname is reached by navigation/the displayed link.

## Competitions

Lists contain real entries without holes: at most eight competitions and
five slides. The order of the list is the order in which
the Matchday and Table screens show the competitions. The details of an entry
hold its four screen switches (Own match, Konferenz, Matchday, Table; a new
or imported entry has all on, an imported entry keeps the default for each
switch it does not name); Own match is disabled without a club. The closed
entry lists the screens it appears on.
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

## Display page

Situation defaults (outside matchdays, matchday, own match) list only enabled
screens. A stored choice whose screen has since been switched off stays
visible as "(off)", together with the note which screen the display uses
instead. Own match is not offered for the matchday situation, because the own
club does not play then; an imported backup with that value is changed to
Conference before validation.

The five screen cards follow the device's screen order (Own match,
Konferenz, Matchday, Table, Crest and slideshow; `screen_order` of the
schema). Each card has an enable switch, a live schematic preview
with example values that follows every draft change (colours, text size,
background, crest and slides), a background-image choice and a colour group.
The global background switch sits above the cards; while it is off, the
per-screen choices keep their values and say so. The conference card holds
the switch that pins the own match in the conference: when it is on, the own
match stands in the first row of every conference page; when it is off, it
takes its normal place in the order. "Matches visible at once" sits on the
conference card and also sets the matches per page of the Matchday screen;
it stays editable while either screen is on. The Matchday card holds
"Matchday shown" (current matchday, or the next matchday once the current
one is over) with its help text. The table card holds the table
rows and the table step (how long a table page or, with an own club, a
competition stays before the next one). Display, provider-budget and diagnostics settings whose
meaning is not obvious have a help text, including the device-specific
conditions (for example a provider's daily cap, 0 for no cap, and its reset
at 00:00 UTC, or the scroll reset that only applies to devices with exactly two
buttons).

The display page only selects images; it has no upload. Every screen except
the crest screen has a background switch (off) and a choice between the standard
background and a library image. The "Wappen und Diashow" section lists the
slides in play order; each slide is the crest or a library image, added from
the library list. The crest itself and the standard background are replaced
or restored on the image page. All choices are saved with the global save
like any other draft change.

## Image editor and storage

Uploaded JPEGs form a library of ids 1–10; id 0 means the stock image of the
role (crest or background). Backgrounds and slides can reuse one image
without spending quota twice. A slideshow id 0 means the crest. The crest is
not a display setting: it is always the crest role's image, the replacement
when one exists, else the factory crest. The gallery shows thumbnails using
per-image revisions and describes every current use.

The image page shows two fixed tiles beside the library: "Standard-Wappen"
(crest) and "Standard-Hintergrund" (standard background). Each shows the
image the display uses, its state ("eingebaut" for the factory image or
"Ersetzt" with the replacement's size) and where it appears (slides with id
0, or the crest screen when the slideshow is empty; screens with a
background but no library image). They cannot be deleted. Three actions:
"Ersetzen" opens the image editor and uploads the result as the role's
replacement; "Aus Bibliothek übernehmen" copies a chosen library image on the
device (the library image stays, the copy counts towards the quota and is
disabled while the library is empty); "Werksbild wiederherstellen" deletes the
replacement after a confirmation (disabled while the factory image is shown).
Replacements count towards the shared library quota but not towards the ten
library entries.

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
A decode that the request deadline stops is answered as `timeout` ("try
again"), not as a damaged file (`content`).
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

The overview and display-page previews follow the redesigned display layout;
whether a preview matches the physical display is part of the device
acceptance. On a display without adjustable backlight (C6 with GC9A01) the
night mode turns the background almost black, shows images at 20 % opacity
and dims all colours according to the night brightness; the day brightness is
stored but only takes effect with an adjustable backlight. See UI.md and
NETWORK.md and API.md for the display and server contracts.
