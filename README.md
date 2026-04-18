# homeassistant-vivla

Custom firmware for the [Home Assistant Voice Preview Edition](https://www.home-assistant.io/voice-pe/) (ESP32-S3 + AIC3204 + XMOS Voice Kit) that bypasses Home Assistant's Assist pipeline and talks directly to a **vivla.ai** realtime voice gateway running Gemini 3.x Live with VIVLA's Hermes orchestrator agent in the loop.

```
┌────────────────────┐       ┌──────────────────────────┐       ┌──────────────┐
│ Voice PE (ESP32-S3)│  WS   │ vivla-intelligence       │  WS   │ Google       │
│  okay_nabu wake    │──────▶│ apps/mastra/voice-bridge │──────▶│ Gemini Live  │
│  vivla_voice comp. │ PCM16 │  GeminiLiveVoice + tools │       │              │
│                    │◀──────│                          │◀──────│              │
└────────────────────┘ 24kHz │  ┌────────────────────┐  │       └──────────────┘
                             │  │ tool: delegateTo   │  │
                             │  │       Hermes(text) │  │
                             │  └─────────┬──────────┘  │
                             │            │ in-process   │
                             │            ▼              │
                             │   mastra.getAgent("hermes")
                             │   .generate(prompt)       │
                             └──────────────────────────┘
```

Wake word stays on-device. Once Gemini decides the user wants something domain-specific (sales, properties, legal, docs, …), it calls `delegateToHermes`, which runs Hermes in-process — full agent network, full memory.

## Repository layout

```
.
├── esphome/components/vivla_voice/   # ESPHome external component (this repo)
│   ├── __init__.py                   # Codegen + config schema
│   ├── vivla_voice.h / .cpp          # WebSocket client + state machine + audio plumbing
│   └── automation.h                  # Action/Condition/Trigger templates
├── vivla-voice-pe.yaml               # Minimal firmware config — flash this
├── secrets.yaml.example              # WiFi + bridge URL + device token
├── tools/probe-vivla-voice.mjs       # Pre-flash bridge validator (Node)
└── docs/                             # Hardware datasheet + upstream Voice PE config (read-only)
```

The voice gateway lives in the **`vivla-intelligence`** repo at `apps/mastra/voice-bridge/`. See its own README for what it does and how it boots alongside Mastra.

## Order of operations

The bridge is the unblock. Get it green before flashing anything.

### 1. Stand up the bridge (vivla-intelligence)

In the `vivla-intelligence` repo, on branch `feat/voice-bridge`:

```sh
# In apps/mastra/.env
GOOGLE_API_KEY=<from https://aistudio.google.com/apikey>
GEMINI_LIVE_MODEL=gemini-live-2.5-flash-preview   # or whatever 3.1 variant your key has
DEVICE_TOKEN=<long random string>

pnpm --filter mastra build
pnpm --filter mastra start
# → vivla voice bridge listening on ws://localhost:3002/voice
```

### 2. Probe the bridge end-to-end (this repo)

Make a 16 kHz mono PCM16 utterance:

```sh
ffmpeg -i my-question.m4a -ac 1 -ar 16000 -f s16le tools/sample-16k.pcm
```

Then:

```sh
node tools/probe-vivla-voice.mjs \
  --bridge ws://localhost:3002/voice \
  --token <DEVICE_TOKEN> \
  --in tools/sample-16k.pcm \
  --out /tmp/reply-24k.pcm

ffplay -f s16le -ar 24000 -ac 1 /tmp/reply-24k.pcm
```

You should see `session.ready`, transcripts streaming, optionally `tool.call delegateToHermes` if the question warrants it, and a reply audio file. **Don't flash hardware until this works.**

### 3. Flash the firmware

```sh
pip install esphome
cp secrets.yaml.example secrets.yaml   # fill in wifi + bridge url + device token
esphome compile vivla-voice-pe.yaml
esphome upload  vivla-voice-pe.yaml --device /dev/cu.usbmodem*
esphome logs    vivla-voice-pe.yaml --device /dev/cu.usbmodem*
```

ESP32-S3 native USB CDC shows up as `/dev/cu.usbmodem*` on macOS. No DTR/RTS quirks.

Boot sequence in the logs:
1. WiFi up
2. `vivla_voice` connects to the bridge URL → `session.ready` event
3. Say "Okay Nabu, …" → `wake: okay_nabu` → `listening` → audio streaming up → `tts start → replying` → audio out the speaker

## Status of the firmware (honest)

What's wired and reasoned-about:
- **WS client** using `esp_websocket_client` (ESP-IDF managed component) — connect, auto-reconnect, JSON event parsing
- **Microphone subscription** via the upstream `MicrophoneSource` helper (channel + bit-depth handling, auto-gated to LISTENING state)
- **Speaker output** — sets `AudioStreamInfo(16, 1, 24 kHz)` and pipes received PCM
- **State machine** with triggers mirroring `voice_assistant:`'s public surface (`on_start`, `on_listening`, `on_stt_vad_start/end`, `on_tts_start`, `on_end`, `on_error`, `on_transcript`, `on_intent_progress`)
- **Actions/conditions**: `vivla_voice.start`, `vivla_voice.stop`, `vivla_voice.is_running`

What needs hardware iteration:
- **First-flash debugging.** I haven't compiled this against actual ESPHome on hardware. Expect a small round of fixes (likely API drift in `MicrophoneSource`/`Speaker` between ESPHome versions, or the `esp_websocket_client` managed component version pin).
- **Backpressure handling** when `Speaker::play()` returns short — currently logged as a warning, audio is dropped. Fine for short replies, may need a ring buffer for long ones.
- **Barge-in.** The `interrupt` event from Gemini flips state back to `LISTENING`, but the current firmware doesn't actively halt the speaker. May need `speaker_->stop()` on interrupt.

What's intentionally out of scope for v0:
- The full LED/jack/mute/touch UX from `docs/home-assistant-voice-pe-dev/home-assistant-voice.yaml`. Once the core path works, copy that file, delete its `api:` and `voice_assistant:` blocks, and paste this `vivla_voice:` block in place — the trigger names line up so the LED scripts work unchanged (modulo the `voice_assistant::Timer` global, which has no equivalent here yet).
- Captive-portal token entry. v0 expects the device token in `secrets.yaml` at compile time.
- OTA from a vivla.ai-hosted manifest. v0 flashes via USB.
- Timer round-trip (timer tools through Hermes).

## Wire format

Documented in `vivla-intelligence/apps/mastra/voice-bridge/README.md`. Mirror summary:

| Direction | Frame | Content |
|---|---|---|
| device → bridge | binary | PCM16 LE mono **16 kHz** |
| device → bridge | text JSON | `{"type":"audio.end"}` on stop |
| bridge → device | binary | PCM16 LE mono **24 kHz** |
| bridge → device | text JSON | `session.ready`, `transcript`, `tool.call`, `turn.complete`, `interrupt`, `error`, `session.closed` |

Auth: `Authorization: Bearer <DEVICE_TOKEN>` header on the WS upgrade. The firmware sets this; the probe falls back to `?token=` query param because Node's WHATWG `WebSocket` can't set headers.
