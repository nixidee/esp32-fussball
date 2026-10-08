# Configuration

> Status: **planned** (P2). Persistence: NVS (ADR-007, accepted 2026-10-08).

## Three sources, one place each
| What | Where | In Git |
|---|---|---|
| Hardware facts (pins, display, buttons) | `boards/`, `displays/`, `targets/` | yes |
| Software defaults, limits, enums (one structured file) | `include/app_config.h` | yes |
| Secrets and personal presets (WiFi, API keys, club, location) | `include/secrets.h` | **no** (template `include/secrets.h.example` is tracked) |

Runtime values changed in the Web UI are stored on the device in NVS
(ESP-IDF's key-value store in flash, ADR-007) and override the defaults.

## Precedence
1. Value stored on the device (set via Web UI)
2. `secrets.h` value (used on first boot and after factory reset)
3. Default from `app_config.h`

Example: mDNS hostname — default `fussball` in `app_config.h`, overridden by
`SECRET_HOSTNAME` if set, overridden by the Web UI value once saved.

## `secrets.h`
1. Copy `include/secrets.h.example` to `include/secrets.h`.
2. Fill in your values. Empty values mean “not set”.
3. Never commit `secrets.h` (it is in `.gitignore`).

Note: values from `secrets.h` are compiled into the firmware binary. Do not
share firmware files built with your personal `secrets.h`.

`secrets.h` is **optional** (ADR-007). Without it the build succeeds and
every value falls back to its default in `app_config.h`:
- no WiFi credentials → the device starts the setup AP;
- no club / league → a default image is shown instead of football data.
It only makes the first setup more comfortable.

## Factory reset
Erases stored settings (and optionally user images); the device restarts with
`secrets.h` values and defaults. Requirement (ADR-007): a reset to
defaults must **always** work and must leave **no stale value** behind —
neither after a reset nor after a firmware update that changes the settings
schema. Triggered from the Web UI (System) — a
button combination on the device is a possible later option.

## Settings groups (planned)
| Group | Examples |
|---|---|
| Club / data | provider, API keys, competitions, club |
| Screens | default screen per situation (no matchday / matchday / own match), return-to-default time (default 60 s, or off), scroll direction reset time (default 20 s), scroll rate while held (default set in P5.4), colours, backgrounds, live list size, table window |
| Overlays | IP badge position, duration, permanent in AP mode |
| WiFi | one network (SSID, password), optional AP password, hostname, external antenna (boards with antenna switch; default off) |
| Display | brightness (only targets with a backlight pin), rotation, night mode window (default 23:00–07:00) |
| Time | time zone, NTP server |
| System | optional admin password (also protects OTA; default off), factory reset |
| Debug | switches per debug output, e.g. periodic status log (today compile-time `cfg::kDebugStatusLog`, interval 30 s, in `app_config.h`) |

Each setting has a type, default, min/max (or allowed values) and a schema
version — all in `app_config.h`. The Web UI reads the schema from the device,
so defaults and limits exist only once.
