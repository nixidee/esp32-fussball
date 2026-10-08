# Configuration

> Status: **planned**, apart from the compile-time boot-layout/debug defaults in
> `include/app_config.h`. NVS persistence and the full runtime settings model
> are not implemented. NVS was selected in ADR-007; the consistency and OTA
> storage contract is refined by ADR-011.

## Three sources, one place each
| What | Where | In Git |
|---|---|---|
| Hardware facts (pins, display, buttons) | `boards/`, `displays/`, `targets/` | yes |
| Software defaults, limits, enums (one structured file) | `include/app_config.h` | yes |
| Secrets and personal presets (WiFi, API keys, club, location) | `include/secrets.h` | **no** (template `include/secrets.h.example` is tracked) |

Planned runtime values changed in the Web UI are stored on the device in NVS
(ESP-IDF's key-value store in flash, ADR-007) and override the defaults.

## Precedence
The current boot layout uses compile-time tokens from `app_config.h`: content
margin, diagnostic ring width/gap and preferred text width. These are not stored
settings or Web controls. The runtime precedence below applies when the settings
model is implemented.

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

`secrets.h` is **optional** (ADR-007). It is not required by the current
bring-up build. Once runtime settings and networking are implemented, missing
values fall back to their defaults in `app_config.h`:
- no WiFi credentials → the device starts the setup AP;
- no club / league → a default image is shown instead of football data.
It only makes the first setup more comfortable.

## Factory reset
Planned reset erases the complete stored settings model, including obsolete
keys and migration generations, then restarts with `secrets.h` values and
defaults. No stale setting may reappear after reset or schema migration.
Deletion of user images is an explicit optional reset choice. A mount fault
must never be interpreted as permission to format LittleFS or discard images.
Reset is triggered from the Web UI (System); a device trigger must also remain
possible, with its exact input gesture to be specified before implementation.

## Consistent saves, migration and OTA

A saved settings model must appear as one complete, validated generation:
after a failed write or power loss, boot loads the previous complete model or
the new complete model, never a mixture. NVS's per-key persistence does not
alone provide this whole-model contract. Choose the transaction, versioning,
capacity and recovery mechanism before implementing the store; keep software
defaults and limits in `app_config.h`.

Both firmware slots share NVS and LittleFS. A trial OTA image must retain
settings and files readable by the previous firmware until the local boot
health check accepts the new image. Any migration must support booting the
previous image after a rejected trial; firmware rollback does not restore
shared storage. An accepted migration removes obsolete active values without
letting them leak into the current model. Retention/cleanup details and their
flash, heap and quota costs are decided before the store and OTA modules.

Save and reset report failure rather than claiming success after a partial
operation. Required tests include interruption at each write/commit stage,
invalid or full NVS, corrupt/unknown schema, reset, and older-firmware boot
after a trial migration.

## Settings groups (planned)
| Group | Examples |
|---|---|
| Club / data | provider, API keys, competitions, club |
| Screens | default screen per situation (no matchday / matchday / own match), return-to-default time (default 60 s, or off), scroll direction reset time (default 20 s), scroll rate while held (default set in P5.4), colours, backgrounds, live list size, table window |
| Overlays | IP badge position, duration, permanent in AP mode |
| WiFi | one network (SSID, password), optional AP password, hostname, external antenna (boards with antenna switch; default off) |
| Display | brightness (only targets with a backlight pin), rotation, night mode window (default 23:00–07:00) |
| Time | time-zone location label mapped to a POSIX rule, NTP server; clock use is defined in [NETWORK.md](NETWORK.md) |
| System | optional admin password (also protects OTA; default off), factory reset |
| Debug | switches per debug output, e.g. periodic status log (today compile-time `cfg::kDebugStatusLog`, interval 30 s, in `app_config.h`) |

Each setting has a type, default, min/max (or allowed values) and a schema
version — all in `app_config.h`. The Web UI reads the schema from the device,
so defaults and limits exist only once.
