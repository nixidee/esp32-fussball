# Hardware

> Status: profile system implemented. XIAO ESP32-C6 + GC9A01 verified on
> the device; Waveshare profile exists, display code not
> built for it yet (paused); XIAO ESP32-S3 later. Pins marked ⛔ are not
> confirmed and must not be used until they are verified.

## Supported / planned profiles
| Profile (env, planned) | Board | Display | Status |
|---|---|---|---|
| `xiao_esp32c6_gc9a01` | Seeed XIAO ESP32-C6 | 1.28" GC9A01 240×240 round (external module, no BL pin) | first target; display and touch inputs verified on the device |
| `waveshare_esp32s3_lcd128` | Waveshare ESP32-S3-LCD-1.28 (non-touch) | built-in GC9A01A 240×240 round | paused; boot log verified; display code not built for it yet; pins from an earlier project on this board; no inputs |
| `xiao_esp32s3_gc9a01` | Seeed XIAO ESP32-S3 | 1.28" GC9A01 240×240 round | later (fallback if C6 RAM is insufficient) |

## Hardware profile system (ADR-004, implemented)
Hardware facts are kept separate and combined per target (compile-time
`constexpr` facts; retained strings or lookup data may occupy flash):
- `include/hw_profile.h` — profile types (`BoardProfile`, `DisplayProfile`,
  `DisplayWiring`, `InputPin`, `TargetProfile`) and the check functions.
  `hw::kNoPin` (-1) marks an unconnected signal.
- `boards/<board>.h` — `hw::kBoard`: name, reserved pins (never used for
  wiring or inputs), antenna switch. Fails the build if the env's chip does
  not match (`CONFIG_IDF_TARGET_*`).
- `displays/<display>.h` — `hw::kDisplay`: controller, shape (round/rect),
  resolution, SPI mode, colour order (`bgr_order` = MADCTL BGR bit) and
  colour inversion (`invert_colors` = INVON). The controller init sequence
  lives in the display component (`components/display/gc9a01_panel.cpp`).
  Rotation is not modelled yet.
- `targets/<target>.h` — `hw::kTarget`: includes one board + one display and
  defines the wiring (which board pin drives which display signal, SPI clock,
  backlight) and the inputs (input 1, 2, 3 … in order).
- `targets/<target>.sdkconfig.defaults` — board-dependent ESP-IDF settings
  (ADR-010).
- `include/hw_target.h` — the only header code includes. The env's build flag
  `-D FUSSBALL_TARGET_<NAME>` (`platformio.ini`) selects the target header;
  none or two flags fail the build.
- Compile-time checks (`static_assert` in `hw_target.h`): SCLK/MOSI/DC
  present; every pin exists on the chip (`SOC_GPIO_VALID_*_MASK` from
  ESP-IDF); no GPIO used twice; no reserved GPIO used. Verified 2026-10-08
  with deliberate wrong assignments (duplicate, reserved, non-existent GPIO,
  missing SCLK) — each fails the build with its message.
- The current guards check chip-valid pins and the board's reserved list;
  they do not yet prove that every allowed pin is usable on the board header.
  A board-usable-pin rule is planned before integration acceptance. On the
  XIAO C6 it must reject flash GPIO24–30 even though the SoC masks permit them.
  The confirmed wiring below does not use those GPIOs and is not being remapped.
- `app_main` logs the selected profile at boot.

New target: board/display header if new, target header, sdkconfig file,
`#elif` line in `hw_target.h`, env in `platformio.ini`.
A new controller additionally requires a driver. Each new resolution needs
separate draw/DMA-buffer, font, scene and asset measurements before acceptance;
adding a profile does not establish that it fits the existing memory budget.
For example, the current two partial RGB565 buffer formula needs 19,200 B at
240×240 but 70,832 B at 466×466, all internal DMA-capable RAM.

## Seeed XIAO ESP32-C6
- ESP32-C6 (RISC-V, single core 160 MHz), 512 KB SRAM, **no PSRAM**, 4 MB flash.
- USB-C (native USB-Serial/JTAG).

### Header map
| Pad | GPIO | Notes |
|---|---|---|
| D0 | 0 | |
| D1 | 1 | |
| D2 | 2 | |
| D3 | 21 | |
| D4 | 22 | SDA |
| D5 | 23 | SCL |
| D6 | 16 | TX |
| D7 | 17 | RX |
| D8 | 19 | SCK |
| D9 | 20 | MISO |
| D10 | 18 | MOSI |

### Reserved (not on header, never use)
| GPIO | Use |
|---|---|
| 3 | RF switch control enable (see “External antenna”) |
| 14 | Antenna select (see “External antenna”) |
| 15 | User LED, strapping pin |
| 4, 5, 8, 9 | Strapping pins |
| 12, 13 | USB D-/D+ (USB-Serial/JTAG: upload and console). Source: ESP-IDF 6.1 `soc/esp32c6/register/soc/io_mux_reg.h` |

### Wiring to GC9A01 (verified on the device)
Same XIAO pads (= same breadboard holes, standard XIAO footprint) as the
XIAO S3 build of an earlier project (S3: SCLK 6, MOSI 5, DC 3, CS 4, RST 2).
| Display pin | XIAO pad | GPIO |
|---|---|---|
| VCC | 3V3 | – |
| GND | GND | – |
| SCL/SCLK | D5 | 23 |
| SDA/MOSI | D4 | 22 |
| CS | D3 | 21 |
| DC | D2 | 2 |
| RST | D1 | 1 |

The display module has **no backlight (BL) pin**: the backlight is
always on at full brightness and cannot be dimmed or switched off by the
firmware on this target. Night mode here is software-only (dark UI), see
[UI.md](UI.md) → “Night mode”.
SPI clock on the C6: 40 MHz works (LVGL boot screen verified on the device
2026-10-08). Recorded runtime after display + LVGL init (device status log
2026-10-08):
internal heap free 399 488 B, largest block 376 832 B (no PSRAM). Touch inputs: active high, momentary — confirmed on the device.
This is an idle boot-screen observation, not a new measurement or a WiFi/TLS,
production-screen or long-run acceptance result.
Backlight driver: a target with a BL pin gets a plain on/off GPIO (off
during panel init, on after the panel is cleared to black); PWM dimming
(LEDC) is not implemented yet. The C6 target has no BL pin → no-op.

### Inputs (verified on the device)
The ESP32-C6 has **no built-in capacitive touch sensor** (unlike ESP32 /
S2 / S3). This target uses TTP223B touch modules with a digital output; they
work like buttons on any free pad.
| Input | XIAO pad | GPIO |
|---|---|---|
| Touch 1 | D8 | 19 |
| Touch 2 | D9 | 20 |
| Touch 3 | D10 | 18 |
Action per input: see [UI.md](UI.md). Free pads left: D0, D6, D7 (D6/D7 = UART0 TX/RX).

Modules: TTP223B single-channel (2.5–5.5 V, power-saving after 12 s idle),
jumpers A/B in factory state; momentary, active-high behaviour and all three
inputs were confirmed on the device. **Power them from 3V3** — the output
follows the supply voltage; 5 V on a C6 GPIO damages the chip.

### External antenna
The XIAO ESP32-C6 has an on-board ceramic antenna and a U.FL connector. An RF
switch selects between them:
| GPIO | Function |
|---|---|
| 3 | RF switch control enable — LOW = enabled |
| 14 | Antenna select — LOW = on-board ceramic (default), HIGH = external U.FL |

Firmware option “external antenna” (default off) — set in the project config,
changeable in the Web UI (WiFi settings). Use only with an antenna connected.

Status: **planned, not implemented yet**. The profile
fields exist in `boards/xiao_esp32c6.h`; the firmware currently does not
touch GPIO3/14. The driver is deferred during display development and must be
implemented before the production WiFi manager. The earlier diagnostic WiFi
budget may run without it, with the undriven antenna state recorded as unverified.

## Waveshare ESP32-S3-LCD-1.28 (non-touch)
- ESP32-S3R2: 2 MB PSRAM, 16 MB flash; USB-C via CH343 USB-UART.
- Upload speed 921600. On macOS the CH343 driver extension must be approved.

| Function | GPIO |
|---|---|
| LCD SCLK | 10 |
| LCD MOSI | 11 |
| LCD DC | 8 |
| LCD CS | 9 |
| LCD RST | 12 |
| LCD backlight | 40 (active high, PWM-dimmable) |
| BOOT button | 0 |
| Battery ADC | 1 |
| IMU QMI8658 SDA / SCL | 6 / 7 |

Source: board profile of an earlier project on this board (verified on the
device). SPI 40 MHz. **Inputs: none** — the BOOT button (GPIO0) is not
reachable in the housing used and is not used as an input.

Known quirks (from earlier projects on this board): use UART0 console instead of
USB-CDC; disable WiFi power save.

Build setup (`platformio.ini` + `targets/waveshare_esp32s3_lcd128.sdkconfig.defaults`):
- PlatformIO has no manifest for this board; the generic
  `esp32-s3-devkitc-1` manifest is used with `board_upload.flash_size = 16MB`.
  ESP-IDF builds take flash size, PSRAM and console from the sdkconfig
  defaults, not from the manifest.
- PSRAM: `CONFIG_SPIRAM` quad mode, 40 MHz (IDF default), added to the heap.
- Console: UART0 via the CH343 bridge, no secondary console.
- Monitor: `monitor_rts = 0`, `monitor_dtr = 0` (from an earlier project) so
  opening the monitor does not reset the board or hold it in download mode.
  If the first boot lines are missing after an upload, press RST.
- Verified on the device 2026-10-08 (boot log): flash 16 MB DIO
  80 MHz, PSRAM 2 MB found at 40 MHz, memory test OK (takes ≈ 420 ms at
  boot), console on GPIO43/44, CPU 160 MHz (IDF default), internal heap
  free 400 711 B, largest block 303 104 B.

## GC9A01 display notes
- 240×240, round visible area (diameter 240 px); corners of the frame buffer
  are not visible.
- SPI, RGB565. Init: custom command sequence from an earlier
  hardware-verified driver, 40 MHz SPI (verified on S3 and C6). `COLMOD`
  0x05 and 0x55 both select 16 bit/pixel for the SPI (MCU) interface; this
  project sends 0x05.
- Init order in `gc9a01_panel.cpp` / `display_port.cpp`: reset → vendor,
  power, gamma, COLMOD, TEON table → SLPOUT, 120 ms → MADCTL → INVON/INVOFF
  (profile) → whole panel written black → DISPON → backlight on.
- Usable width shrinks towards top/bottom: chord width
  `w(y) = 2·√(r² − (y − cy)²)`. At 20 px from the edge only ≈ 133 px remain.
  See [UI.md](UI.md) → “Round display rules”.
