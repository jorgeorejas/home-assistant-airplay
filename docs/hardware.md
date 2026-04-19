# Hardware

Target: **[Home Assistant Voice Preview Edition](https://www.home-assistant.io/voice-pe/)** (NC-VK-9727). No hardware modifications are required or expected.

## Board at a glance

| Component | Part | Note |
|---|---|---|
| MCU | ESP32-S3R8 | dual Xtensa LX7 @ 240 MHz, 8 MB octal PSRAM @ 80 MHz, 16 MB flash |
| Mic pre-processor | XMOS XU316 | unused in DenAir; idle on the I²S bus |
| Audio codec / DAC | TI TLV320AIC3204 | I²S slave, I²C control on 0x18 |
| Speaker amp | TPA6211A | internal 8 Ω speaker driver, enable on GPIO47 |
| Output | 3.5 mm stereo jack | switched output; detect line on GPIO17 (Phase 2) |
| LED ring | 12 × WS2812B | GPIO21 data, GPIO45 gated VCC rail |
| User input | Rotary encoder + push button | encoder A/B on GPIO16/18, button on GPIO0 |
| User input | Side slide switch | GPIO3, active-low. Physical mute in stock firmware; **repurposed as LED on/off in DenAir** |

## Pin map

Authoritative source: [docs/home-assistant-voice-pe-dev/home-assistant-voice.yaml](home-assistant-voice-pe-dev/home-assistant-voice.yaml) (upstream ESPHome reference). Pre-pivot `vivla-voice-pe.yaml` at the `v0.1-voice` tag has a different GPIO47 mapping that appears to have been a typo; trust the upstream yaml.

| Function | GPIO | Direction | Detail |
|---|---:|---|---|
| I²S BCLK | 8 | out | to AIC3204, S3 is master |
| I²S LRCLK | 7 | out | 44.1 or 48 kHz |
| I²S DOUT | 10 | out | 16-bit stereo |
| I²C SDA | 5 | in/out | AIC3204 control, 400 kHz |
| I²C SCL | 6 | out | |
| AIC3204 I²C address | 0x18 | — | 7-bit |
| Jack detect | 17 | in | **unused in Phase 1**; hook planned for Phase 2 (200 ms debounce) |
| Internal amp enable (TPA6211A) | 47 | out | driven HIGH at boot; Phase 2 flips LOW when jack is inserted |
| LED ring data | 21 | out | WS2812B 12 LEDs, 10 MHz RMT, GRB byte order |
| LED ring VCC enable | 45 | out | **driven by the mute slide** (GPIO3 LOW = rail HIGH = ring powered) |
| Rotary encoder A | 16 | in, pull-up | quadrature, 4 edges = 1 detent |
| Rotary encoder B | 18 | in, pull-up | |
| Center button | 0 | in, pull-up | active-low, strapping pin |
| Mute slide switch | 3 | in, pull-up | active-low = LEDs on (DenAir), mic mute in stock |
| USB CDC | internal | in/out | native ESP32-S3 USB; primary console via USB_SERIAL_JTAG secondary |

## Power & thermals

- Powered over USB-C (5 V / up to 2 A).
- PRD target: < 1.5 W active.
- No active cooling; the enclosure runs cool during AirPlay playback at typical room temperature.

## Enclosure

The stock 3D-printed case is preserved — see `voice_preview_edition_enclosure_all_parts.stl` if you want to print a replacement. The DenAir firmware does not require any physical modification to the enclosure.

## Schematic & datasheet

- [home_assistant_voice_pe_schematic_v1.0_241009.pdf](home_assistant_voice_pe_schematic_v1.0_241009.pdf)
- [home_assistant_voice_preview_edition_datasheet_v1_1.pdf](home_assistant_voice_preview_edition_datasheet_v1_1.pdf)
