# `boards/ha_voice_pe`

Board-support layer for the Home Assistant Voice Preview Edition, shaped to fit airplay-esp32's `iot_board_*` contract (`components/boards/board_common.h`).

## Responsibilities

- **Declare the hardware**: pin numbers for I²C, I²S, the LED ring VCC rail, the amp-enable line, etc., in `iot_board.h`.
- **Bring up I²C** (GPIO5 SDA, GPIO6 SCL, 400 kHz) and expose the bus handle via `iot_board_get_handle(BOARD_I2C_DAC_ID)` so the DAC driver can attach.
- **Drive the TPA6211A internal-amp enable line** (GPIO47): high at boot today; Phase 2 will flip low when the 3.5 mm jack is detected.
- **Seed WiFi credentials** from the compile-time `wifi_config.h` into NVS. Runs on every boot, so re-flashing with new credentials is the source of truth (matches the PRD's "no runtime provisioning" rule).
- **Supervise the WiFi fallback**: if `WIFI_SSID_1` doesn't associate within `WIFI_CONNECT_TIMEOUT_MS` (default 25 s, overridable in `wifi_config.h`), switch the live STA config to `WIFI_SSID_2` and kick a reconnect. Round-robins as long as the ring is alive.
- **Register the AIC3204 DAC** (`dac_register(&dac_tlv320aic3204_ops)`) before main calls `dac_init()`.

## What this board does *not* do

- **Does not touch GPIO17 (jack detect)** — reserved for Phase 2 auto-switch between internal speaker and the jack.
- **Does not touch GPIO45 (LED rail) or GPIO3 (mute slide)** — those are handled by `components/denair_leds/` and `components/denair_ui/led_switch.c` respectively, because they're LED-UX concerns, not board infra.
- **Does not initialise the LED ring or UI components**. Those `_init` calls live in `main/main.c` for link-order reasons (see [../../../docs/architecture.md](../../../docs/architecture.md) "Link order trick").

## Why the pin defines are baked in, not `CONFIG_…`

The upstream airplay-esp32 board abstraction reads `CONFIG_I2S_BCK_IO` / `CONFIG_DAC_I2C_SDA` / etc. via Kconfig so one binary can target different pinouts. HA Voice PE is fixed hardware — exposing the pins as Kconfig knobs would just let someone accidentally change them. `iot_board.h` defines them as `#define BOARD_I2S_BCK_GPIO 8` and mirrors the same values into the Kconfig knobs via `sdkconfig.defaults.ha_voice_pe` for the parts of the upstream codebase that still read `CONFIG_*`.

## Key files

- `iot_board.h` — pin map + the `BOARD_*` macros the rest of the firmware uses. Authoritative.
- `board.c` — `iot_board_init` / `iot_board_deinit` / `iot_board_get_handle`, plus the WiFi credential seeder and fallback supervisor (~50 lines).
- `../Kconfig.projbuild` — adds the `BOARD_HA_VOICE_PE` choice option and sets `BOARD_TARGET_PATH="ha_voice_pe"`.
- `../partitions.csv` — 16 MB layout with 3 MB OTA slots and a 1.9 MB SPIFFS storage partition (inherited from upstream, unmodified).

## Testing this module in isolation

Bring-up check after any edit:

```bash
tools/denair-build.sh build
tools/denair-build.sh flash
tools/denair-build.sh monitor
```

Expected boot log (abridged):

```
I (1082) main: Board: HA Voice PE (DenAir)
I (1092) HA-Voice-PE: seeded WiFi credentials for SSID='…' from wifi_config.h
I (1102) HA-Voice-PE: WiFi fallback: armed (swap SSID after 25000 ms if not connected)
I (1112) gpio: GPIO[47]| InputEn: 0| OutputEn: 1| …
I (1122) HA-Voice-PE: I2C bus 0 up (sda=5 scl=6)
I (3632) AIC3204: AIC3204 online @ 0x18
I (3632) HA-Voice-PE: HA Voice PE initialized (DenAir)
```

`AIC3204 online @ 0x18` proves both the I²C bus *and* the DAC are reachable on their pins. If it doesn't appear, the fault is on this module or the DAC driver.
