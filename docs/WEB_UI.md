# Web UI

> Status: **planned** (P3.6, P7, P8).

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
| **Debug** (planned) | Live debug output from the device's debug helper (P2.8): shown only while the tab is open, no log buffer on the device |

## Image editor (P7.4)
- Upload an image; resize, move, set transparency.
- Round mask shows the visible area.
- Option to overlay demo text (static demo data or live data) to judge
  readability on data screens.
- The browser produces the final image at device resolution and in the
  device's storage format; the device does no heavy image processing.
- Every pre-installed image can be reset to its default.
- Format: display size; uncompressed RGB565 or baseline JPEG (ADR-008).
- Crest source: own images first. Loading the provider's crest in
  the browser (render to display size, then upload) follows soon after.
- Default images are updated only over USB (`uploadfs`), not via the Web UI.

## API
The UI uses the REST API described in [NETWORK.md](NETWORK.md). Defaults and
limits come from the device's settings schema — never duplicated in
JavaScript.
