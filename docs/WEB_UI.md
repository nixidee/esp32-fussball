# Web UI

> Status: **planned**, not implemented.

Modern, simple, dark design with tabs. Served by the device (embedded in the
firmware, no internet/CDN needed). Reachable in normal WiFi mode
(`http://<ip>` or `http://fussball.local`) and in setup AP mode.

## Tabs
| Tab | Content |
|---|---|
| **Dashboard** (start page) | Quick club selection, basic options (e.g. use backgrounds), status: WiFi, provider, last update |
| **Screens** | Sub-tab per screen (Live single, Live multi, Table, Crest/Slideshow): enable, colours, background, sizes, timings |
| **Images** | Crest, slideshow (up to 5 images initially, limited by free space), backgrounds per screen; editor; reset to default |
| **Settings** | Groups: Data/API, WiFi, Display, Overlays, Time, System |
| **Update** | Firmware upload (OTA), current version and profile |
| **Debug** (planned) | Selected live application diagnostics while the tab is open, without retained history; bounded connection/formatting overhead is measured before transport implementation |

## Image editor (P7.4)
- Upload an image; resize, move, set transparency.
- Round mask shows the visible area.
- Option to overlay demo text (built-in static demo data; the Web UI shows no
  live scores or tables) to judge readability on data screens.
- The browser produces the final image at device resolution and in the
  device's storage format; the device does no heavy image processing.
- Every pre-installed image can be reset to its default.
- Format: display size; uncompressed RGB565 or baseline JPEG (ADR-008).
- Crest source: own images first. Loading the provider's crest in
  the browser (render to display size, then upload) follows soon after.
- Default images are updated only over USB (`uploadfs`), not via the Web UI.

## Image storage and updates

Default and user images share LittleFS. The maximum image count is subject to
the measured per-resolution quota, including defaults, metadata, filesystem
overhead and temporary space for replacement. Five images are an initial UI
limit, not a promise that five raw full-screen images fit every target.

Uploads validate total bytes, dimensions and the supported format before an
image becomes selectable. An interrupted or invalid replacement preserves
the previous usable file. Image readers and update/delete operations need an
explicit lifetime protocol: an image being read by LVGL cannot be replaced
or deleted until its reader has finished. The chosen mechanism writes the new
image to one bounded temporary file inside a reserved part of the filesystem
and publishes it only when no reader uses the old file (ADR-016). Specify the
reserve, quotas and cost before implementation. Test full storage, interrupted uploads, corrupt
files and concurrent readers. A failed mount is reported, without automatic
formatting.

USB `uploadfs` is a **development-only filesystem-image replacement**. It may
erase images previously uploaded through the Web UI; no backup/restore tools
or separate image partition are planned for this operation. Routine firmware
OTA preserves the shared LittleFS images. Trial firmware must also retain
files readable by the previous image until acceptance, as described in
[CONFIGURATION.md](CONFIGURATION.md).

## Live debug output

Console health reports exist today and use the stored `debug_status_log`
and `debug_status_interval_s` settings. Browser output is a separate planned
live push stream of controlled application diagnostics: selected module
status, state changes and errors (ADR-021, refining ADR-016). It does not
mirror arbitrary SDK/vendor console text. Producers omit or redact secrets
before enqueueing a record. No log hook or network transport is installed
by the health service now; transport follows in P8.7.

The device retains no log history and offers no replay after reconnecting.
Socket/protocol state and transient formatting/sending buffers must still
be bounded. Choose the protocol, client count, message size, rate and explicit
drop/disconnect policy before transport implementation, using measurements
of both inactive and active heap peaks, stack, fragmentation and flash.

A producer must never block on a browser, including a slow or disconnected
client; no queue may grow without a bound. Keep live output switchable and
stop streaming when the tab disconnects. Required acceptance covers inactive
and active budgets, slow/disconnected clients, reconnects and operation
alongside allowed provider, image-upload and OTA workloads.

## API
The UI uses the REST API described in [NETWORK.md](NETWORK.md). Defaults and
limits come from the device's settings schema — never duplicated in
JavaScript.
