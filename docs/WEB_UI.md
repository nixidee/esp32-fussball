# Web UI

The embedded Web UI is implemented in the compiled C6 development candidate.
Browser, phone and device acceptance are pending. It runs locally in German or
English and needs no external scripts, fonts or CDN.

## Tabs

| Tab | Implemented content |
|---|---|
| Dashboard | Club/provider selection, refresh and network/data/resource status |
| Screens | Live single, conference, table and crest/slideshow; enable, colours, text scale, background and timing controls |
| Images | Crest, five slideshow slots and four screen backgrounds; browser editor, upload, reset and storage repair |
| Settings | Data routes and explicit fallback mappings, WiFi, antenna, display, overlays, time, system and admin password |
| Update | Current version/profile and compatible firmware upload |
| Debug | Controlled live status during the connection, without retained history |

The status distinguishes invalid time, missing club/competition, stale data,
provider errors and fallback use. Provider selection uses asynchronous,
bounded result pages. A selected team/competition is saved by its provider ID;
the firmware does not guess cross-provider identity from a name. API-Football
requires a selected season. Optional fallback routes require explicit paired
fixture IDs, with an explicit home/away swap flag when needed.

Every configurable default and limit is supplied by the device schema.
Display dimensions and shape come from the active hardware profile. The browser
preview uses static demo content; it does not display live scores or a live table.

## Image editor

The editor accepts a local image, places it on a canvas at device resolution
and supports drag, zoom and opacity. Round profiles show the round visible mask.
Optional static demo text helps check contrast. The browser encodes the final
image as baseline JPEG and reduces encoding quality within the device's byte
limit. The firmware does not resize uploaded images.

The supplied stock source PNGs are generated project artwork: a football crest
and a pitch background. They contain no downloaded club crest. The build script
`scripts/build_assets.py` makes resolution-specific stock JPEGs from
`assets/src/`. Pillow 12.3.0 was checked on PyPI on 2026-10-10; no runtime image
processing dependency is installed on the device.

Automatic provider-crest loading in the browser remains a deferred enhancement.
The current editor accepts owner-supplied files. This distinction is tracked
with the remaining scope, rather than treating a provider image URL as an
already implemented import workflow.

## Image storage and lifetime

There are ten logical slots: one crest, five slideshow images and four screen
backgrounds. All use baseline JPEG at exactly the active display dimensions.
Progressive JPEG, invalid markers, invalid dimensions and incomplete/corrupt
decode are rejected before publication.

| Bound | C6 candidate |
|---|---|
| One uploaded image | 32,768 bytes |
| Total unique image-file quota | 327,680 bytes |
| Reserved replacement space | 40,960 bytes |
| LittleFS partition | 458,752 bytes |
| Stock files | Two aliases shared by default slots; 7,862 logical bytes |

The partition also needs filesystem metadata. These caps leave a reserve but
do not substitute for a measured full-storage test. Larger/other profiles need
their own image-quality, quota and decode-memory acceptance.

One bounded temporary file receives the upload. A full streamed JPEG decode
uses a fixed 4 KB validation workspace; the existing LVGL JPEG pool is separate.
Before atomic replacement/deletion, the HTTP operation acquires the LVGL lock
and drops the old image cache entry. This keeps a reader from using a replaced
file. An interrupted replacement preserves the old published image. Leftover
unpublished temporary storage is reclaimed when the next upload begins.

The filesystem preserves populated/corrupt storage after a mount failure;
only a partition verified entirely erased is initialized at first boot. The Images
tab provides explicit full-storage formatting, including when display startup
failed and the web repair path is available. Full formatting erases defaults
as well as uploads; USB `uploadfs` restores the stock filesystem image.
Resetting a single uploaded slot removes its override and selects its stock
alias if that alias still exists.

Routine firmware OTA preserves this shared storage. Trial firmware blocks
persistent image mutation until local health confirmation. USB `uploadfs`
is a development filesystem-image replacement and may erase user uploads.
There is no automatic backup/restore operation.

## Session and update behaviour

The optional admin password and session contract apply consistently to
settings, images, exports, reset and OTA. Even without a password, mutations
require the browser's nonce session. Ordinary settings reads redact stored
passwords and API keys. Uploads stream bounded chunks; an admitted provider
operation can cause a temporary HTTP 409 on image/OTA operations.

Firmware upload checks target/profile/layout compatibility and final image
integrity. Trial confirmation depends on local services, not an internet
connection. See [NETWORK.md](NETWORK.md) for the endpoint, timeout and rollback
contracts and [CONFIGURATION.md](CONFIGURATION.md) for settings reset semantics.

## Live Debug

The debug producer sends selected current application state, not arbitrary
SDK console text. Fields cover monotonic time, WiFi, free/largest heap block,
data revision/staleness, heavy-operation status, a bounded error field and
provider request counters. Credentials, API keys and full provider payloads
are absent.

One WebSocket client and one in-flight record are permitted. A record is bounded
to 256 bytes and produced once per second by a static 4 KB task. The send
buffer remains owned until the asynchronous completion callback. A busy or
slow client causes a record drop/disconnect instead of a growing queue.
Incoming frames are bounded to 128 bytes. Closing the tab closes its stream.

There is no history or replay. The existing console health interval is separate
from this live browser stream. Inactive/active heap peaks, stack, slow clients,
reconnects and simultaneous permitted workloads are still acceptance work.

## Remaining acceptance and polish

Browser form behaviour, narrow phone layouts, canvas gestures, translations,
screenshots, slow/disconnected clients and hardware upload/recovery remain
unverified. Stock artwork and typography are functional defaults; final visual
polish is deferred. A successful firmware/filesystem build does not establish
these behaviours.
