# `dac_tlv320aic3204`

Driver for the TI TLV320AIC3204 codec/DAC sitting on the I²C bus the board brings up on GPIO5/6. The chip receives I²S audio from the ESP32-S3 master on GPIO7/8/10 and drives both the 3.5 mm jack (HP path) and the internal speaker amplifier (LO path).

## Why this exists

Upstream airplay-esp32 supplies drivers for the TI TAS57xx (SqueezeAMP) and TAS58xx (Esparagus) families through its `dac_ops_t` vtable. The HA Voice PE uses an AIC3204 instead, which is a different chip family. This component fills in the missing DAC driver so the board can register it at `iot_board_init` time and the audio pipeline Just Works.

## What it does

- Implements the full `dac_ops_t` contract from `components/dac/include/dac.h`:
  - `init(i2c_bus)` — attaches to the bus, runs soft reset, configures PLL/NDAC/MDAC/DOSR, sets the I²S interface to 32-bit, routes **both** HPL/R and LOL/R outputs simultaneously (so both the jack and the internal amp get the signal), waits 2.5 s for HP soft-step, powers up the DAC channels, sets initial volume to the configured ceiling.
  - `deinit` — mutes, powers down, removes from the bus.
  - `set_volume(db)` — maps AirPlay's [-30, 0] dB range onto [`CONFIG_TLV320AIC3204_MAX_VOLUME_DB`, -63.5] dB, writes 0.5 dB-step codes to page 0 regs 65/66 (DACL_VOL_D / DACR_VOL_D).
  - `set_power_mode(mode)` — ON unmute; STANDBY mute (channels stay powered); OFF mute + channel power-down.
  - `enable_speaker(bool)` / `enable_line_out(bool)` — both no-ops in DenAir. The internal-amp enable is gated at the TPA6211A (GPIO47, board layer), not at the codec. The HP/LO paths in the codec are permanently routed.

## Register sequence

Ported from [ESPHome's `aic3204` component](https://github.com/esphome/esphome/tree/dev/esphome/components/aic3204), which is known-working on this exact hardware (the pre-pivot `vivla-voice-pe.yaml` at the `v0.1-voice` tag used it). See `dac_tlv320aic3204.c` for the full sequence — it's heavily commented.

Key values:

| Register (addr) | Page | Value | Meaning |
|---|---:|---:|---|
| `SW_RST` (0x01) | 0 | 0x01 | software reset |
| `NDAC`, `MDAC` (0x0B, 0x0C) | 0 | 0x82 | power-up bit + divider = 2 |
| `DOSR` (0x0E) | 0 | 0x80 | DOSR = 128 |
| `CODEC_IF` (0x1B) | 0 | 0x30 | I²S, 32-bit word length |
| `LDO_CTRL` (0x02) | 1 | 0x09 → 0x01 | enable AVDD_LDO, then master analog power |
| `CM_CTRL` (0x0A) | 1 | 0x40 | common mode 0.75 V |
| `HP_START` (0x14) | 1 | 0x25 | HP soft-step: 6 kΩ, N=6 |
| `HPL/R_ROUTE`, `LOL/R_ROUTE` (0x0C-0x0F) | 1 | 0x08 | route L→HPL+LOL, R→HPR+LOR |
| `HPL/R_GAIN` (0x10, 0x11) | 1 | 0x3E | unmute, −2 dB |
| `LOL/R_DRV_GAIN` (0x12, 0x13) | 1 | 0x00 | unmute, 0 dB |
| `OP_PWR_CTRL` (0x09) | 1 | 0x3C | power HPL/HPR/LOL/LOR |
| `DAC_CH_SET1` (0x3F) | 0 | 0xD4 | power L+R DACs, soft step 1x |
| `DAC_CH_SET2` (0x40) | 0 | 0x00 | unmute |
| `DACL/R_VOL_D` (0x41, 0x42) | 0 | varies | volume, 0.5 dB step, signed |

## Clocking

**No external MCLK is wired on HA Voice PE.** The AIC3204 derives its audio clocks from the BCLK our ESP32-S3 produces. NDAC/MDAC/DOSR = 2/2/128 gives an effective 512× overclock factor — at 44.1 kHz BCLK that's 22.5 MHz CODEC_CLKIN, which matches the datasheet recommendation for the DAC PLL to lock.

If BCLK timing ever drifts (e.g. if the I²S peripheral is reconfigured mid-stream), the PLL can unlock and audio goes silent even though I²S is still writing. In practice this hasn't happened; if it does, the fix is in `components/dac_tlv320aic3204/dac_tlv320aic3204.c:setup_codec()` — add a PLL-source write to page 0 reg 4 to force BCLK-as-PLL-input.

## Volume mapping

`aic3204_set_volume(volume_db)` takes AirPlay's −30..0 dB range and maps linearly onto [`-6`, `-63.5`] dB (default ceiling `CONFIG_TLV320AIC3204_MAX_VOLUME_DB = -6`). Why a ceiling at −6 instead of 0:

- AirPlay peak-level tracks can be full-scale, which at 0 dB DAC is clippy on the TPA6211A amp.
- −6 dB gives ~2× headroom for the amp without the user perceiving a quiet system.

Override the ceiling via `idf.py menuconfig → Component config → DAC Configuration → Maximum DAC volume`.

## Config

Kconfig options in `components/dac_tlv320aic3204/Kconfig`:

| Option | Default | Notes |
|---|---|---|
| `TLV320AIC3204_I2C_ADDR` | `0x18` | 7-bit, don't change unless your wiring differs |
| `TLV320AIC3204_SAMPLE_RATE` | `44100` | informational; the codec clocks off BCLK |
| `TLV320AIC3204_MAX_VOLUME_DB` | `-6` | AirPlay 0 dB maps to this level |

`CONFIG_DAC_TLV320AIC3204=y` is set automatically by `sdkconfig.defaults.ha_voice_pe` when the board is selected.

## Known TODOs

- Hardware-validate the clocking for 48 kHz sources. Most AirPlay content is 44.1 kHz; 48 kHz negotiations have been tested on bench with Apple Music but not measured for jitter.
- Implement the DAC PLL registers (page 0 regs 4-6) instead of relying on direct NDAC/MDAC clocking. Nice-to-have; not blocking.
