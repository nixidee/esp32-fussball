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
- Settings format 2 with read-only import of legacy format-1 core records;
  explicit save after trial acceptance, matching-format OTA gate.
- Offline German/English Web UI, typed settings, redacted export/import,
  session/CSRF protection, image editor, baseline-JPEG uploads and live debug.
- Whole-operation provider DNS/TLS/header/body and HTTP receive/send bounds;
  asynchronous DNS and incremental TLS retain certificate hostname/SNI checks.
- Streamed OTA with chip/project/target/layout identity and local trial/rollback.
- Shared stock images, asset/font builders and an opt-in release source guard.

Versions follow MAJOR.MINOR.PATCH. Development candidates add `-dev`; the
release guard accepts plain versions only. `VERSION` is authoritative. Settings
record format and stable hardware/layout identity are versioned independently:
an application version change alone does not change their compatibility.

The C6 candidate fits its OTA slot at 87.6% but exceeds the 85% reserve target.
Runtime reserves, browser/device acceptance and visual polish are still pending.
