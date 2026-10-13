# Changelog

## 0.1.0-dev — 2026-10-10

Implementation candidate; device, provider-fixture, browser and maximum-load
acceptance remain pending. No release or device upload is implied.

- WiFi recovery policy, setup AP/captive DNS, antenna switching, mDNS and SNTP.
- Correct AP DHCP initialization before assigning the configured setup address;
  the earlier sequence aborted before WiFi started, regardless of SSID availability.
- Bounded canonical football data, four provider adapters, polling, budgets,
  provider-scoped fallback and explicit verified fixture composition.
- Four device screens, input navigation, night mode, table scrolling, IP badge,
  offline demo and fixed Latin font subsets.
- Settings format 3 with compact competition/fixture/slide lists, image
  references and read-only import of formats 1/2; explicit save after trial
  acceptance, OTA accepts equal/newer settings formats.
- Offline German/English Web UI, typed settings, redacted export/import,
  session/CSRF protection, image editor, baseline-JPEG uploads and live debug.
- Whole-operation provider DNS/TLS/header/body and HTTP receive/send bounds;
  asynchronous DNS and incremental TLS retain certificate hostname/SNI checks.
- Streamed OTA with chip/project/target/layout identity and local trial/rollback.
- Shared stock images, asset/font builders and an opt-in release source guard.

### Implementation update — 2026-10-12

- Six-page offline Web UI with global draft/save, inline validation, explicit
  secret modes, independent startup/retries and optional-password sessions.
- Competition wizard and saved-only fixture picker, ordered slides, revisioned
  image library/editor, separate repair, per-image delete results and full
  build-identity follow-up after firmware upload.
- Stable provider-operation error keys, actual WiFi scan completion/error,
  schema upload/search limits and explicit partial image-repair results.
- Thirty-two reproducible host browser-logic checks; device acceptance next.
  Display redesign is not included; existing display fonts/layout/input remain.
- OpenLigaDB minimum request spacing reduced from 30 to 5 seconds. Its
  published upstream limit is 60 requests per minute per IP; the local spacing
  also covers selections and multiple competitions. No cache/pipeline redesign
  or new runtime interval setting is included in this change.
- Provider counters correctly labelled as attempts since restart, without a
  misleading comparison to API-Football's daily budget.
- OpenLigaDB full refresh requests the current round and the own club's
  fixtures instead of the whole season (about 26 KB instead of about 300 KB).
- OpenLigaDB competition list loads one season (current or typed) and only
  football entries; picking keeps a single marked entry and the correct name.
- WiFi modem sleep is off; it was the unchosen ESP-IDF default.
- Page notices are readable in both themes and stay visible above dialogs.
- Settings format 4: new switch "pin own match in the Konferenz" (default on;
  display effect follows with the display redesign). The matchday default no
  longer offers Own match; a stored or imported value becomes Konferenz.
- Web UI completed to the approved design: schematic round/rectangular
  previews of the current screen and of every screen card, a reason line for
  the shown screen, a visible reason on every disabled control, upload of a
  new crest/background image directly from its field, situation choices
  limited to enabled screens, explained competition save/delete flow, country
  filter only for API-Football and help texts for non-obvious settings.

Versions follow MAJOR.MINOR.PATCH. Development candidates add `-dev`; the
release guard accepts plain versions only. `VERSION` is authoritative. Settings
record format and stable hardware/layout identity are versioned independently:
an application version change alone does not change their compatibility.

The current C6 candidate fits its OTA slot at 85.0% (1,559,248 B reported flash),
with unchanged 197,380 B static RAM. The embedded UI is 48,481 B gzip.
Runtime reserves, browser/device acceptance and visual polish are still pending.
