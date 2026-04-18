"""ESPHome external component: vivla_voice.

Mirrors a useful subset of the built-in voice_assistant: API so the existing
LED/UX YAML transplants 1:1, but talks to the vivla.ai voice gateway over
WebSocket instead of Home Assistant's native API.

Wire format (matches apps/mastra/voice-bridge/README.md):
- Out: PCM16 LE mono 16 kHz binary frames; {"type":"audio.end"} text frame on stop.
- In:  PCM16 LE mono 24 kHz binary frames + JSON control events.
"""

AUTO_LOAD = ["json"]

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.automation import maybe_simple_id
from esphome.components import microphone, speaker, micro_wake_word
from esphome.components.esp32 import add_idf_component
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_URL

CONF_TOKEN = "token"
CONF_SPEAKER = "speaker"
CONF_MICRO_WAKE_WORD = "micro_wake_word"
CONF_RECONNECT_INTERVAL = "reconnect_interval"
CONF_ON_CLIENT_CONNECTED = "on_client_connected"
CONF_ON_CLIENT_DISCONNECTED = "on_client_disconnected"
CONF_ON_START = "on_start"
CONF_ON_LISTENING = "on_listening"
CONF_ON_STT_VAD_START = "on_stt_vad_start"
CONF_ON_STT_VAD_END = "on_stt_vad_end"
CONF_ON_INTENT_PROGRESS = "on_intent_progress"
CONF_ON_TTS_START = "on_tts_start"
CONF_ON_END = "on_end"
CONF_ON_ERROR = "on_error"
CONF_ON_TRANSCRIPT = "on_transcript"

# Mic input format we require: 16-bit mono @ 16 kHz. The MicrophoneSource
# helper handles bit-depth conversion + channel selection; sample rate must
# match the underlying microphone (final-validated below).
MIC_SAMPLE_RATE = 16000

vivla_voice_ns = cg.esphome_ns.namespace("vivla_voice")
VivlaVoice = vivla_voice_ns.class_("VivlaVoice", cg.Component)

StartAction = vivla_voice_ns.class_("StartAction", automation.Action)
StopAction = vivla_voice_ns.class_("StopAction", automation.Action)
IsRunningCondition = vivla_voice_ns.class_("IsRunningCondition", automation.Condition)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VivlaVoice),
        cv.Required(CONF_URL): cv.string_strict,
        cv.Optional(CONF_TOKEN, default=""): cv.string,
        cv.Optional(CONF_RECONNECT_INTERVAL, default="5s"): cv.positive_time_period_milliseconds,
        cv.Required(CONF_MICROPHONE): microphone.microphone_source_schema(
            min_bits_per_sample=16,
            max_bits_per_sample=16,
            min_channels=1,
            max_channels=1,
        ),
        cv.Optional(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_MICRO_WAKE_WORD): cv.use_id(micro_wake_word.MicroWakeWord),
        cv.Optional(CONF_ON_CLIENT_CONNECTED): automation.validate_automation(single=False),
        cv.Optional(CONF_ON_CLIENT_DISCONNECTED): automation.validate_automation(single=False),
        cv.Optional(CONF_ON_START): automation.validate_automation(single=False),
        cv.Optional(CONF_ON_LISTENING): automation.validate_automation(single=False),
        cv.Optional(CONF_ON_STT_VAD_START): automation.validate_automation(single=False),
        cv.Optional(CONF_ON_STT_VAD_END): automation.validate_automation(single=False),
        cv.Optional(CONF_ON_TTS_START): automation.validate_automation(single=False),
        cv.Optional(CONF_ON_END): automation.validate_automation(single=False),
        cv.Optional(CONF_ON_ERROR): automation.validate_automation(single=False),
        cv.Optional(CONF_ON_INTENT_PROGRESS): automation.validate_automation(single=False),
        cv.Optional(CONF_ON_TRANSCRIPT): automation.validate_automation(single=False),
    }
).extend(cv.COMPONENT_SCHEMA)


FINAL_VALIDATE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_MICROPHONE): microphone.final_validate_microphone_source_schema(
                "vivla_voice", sample_rate=MIC_SAMPLE_RATE
            ),
        },
        extra=cv.ALLOW_EXTRA,
    ),
)


_NO_PAYLOAD_TRIGGERS = {
    CONF_ON_CLIENT_CONNECTED: "get_client_connected_trigger",
    CONF_ON_CLIENT_DISCONNECTED: "get_client_disconnected_trigger",
    CONF_ON_START: "get_start_trigger",
    CONF_ON_LISTENING: "get_listening_trigger",
    CONF_ON_STT_VAD_START: "get_stt_vad_start_trigger",
    CONF_ON_STT_VAD_END: "get_stt_vad_end_trigger",
    CONF_ON_TTS_START: "get_tts_start_trigger",
    CONF_ON_END: "get_end_trigger",
}


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_url(config[CONF_URL]))
    if config[CONF_TOKEN]:
        cg.add(var.set_token(config[CONF_TOKEN]))
    cg.add(var.set_reconnect_interval(config[CONF_RECONNECT_INTERVAL]))

    mic_source = await microphone.microphone_source_to_code(config[CONF_MICROPHONE])
    cg.add(var.set_microphone_source(mic_source))

    if CONF_SPEAKER in config:
        spkr = await cg.get_variable(config[CONF_SPEAKER])
        cg.add(var.set_speaker(spkr))

    if CONF_MICRO_WAKE_WORD in config:
        mww = await cg.get_variable(config[CONF_MICRO_WAKE_WORD])
        cg.add(var.set_micro_wake_word(mww))

    for key, getter in _NO_PAYLOAD_TRIGGERS.items():
        for conf in config.get(key, []):
            await automation.build_automation(getattr(var, getter)(), [], conf)

    for conf in config.get(CONF_ON_ERROR, []):
        await automation.build_automation(
            var.get_error_trigger(), [(cg.std_string, "code")], conf
        )

    for conf in config.get(CONF_ON_INTENT_PROGRESS, []):
        await automation.build_automation(
            var.get_intent_progress_trigger(), [(cg.std_string, "x")], conf
        )

    for conf in config.get(CONF_ON_TRANSCRIPT, []):
        await automation.build_automation(
            var.get_transcript_trigger(),
            [(cg.std_string, "role"), (cg.std_string, "text")],
            conf,
        )

    add_idf_component(
        name="esp_websocket_client",
        repo="https://github.com/espressif/esp-protocols.git",
        ref="websocket-v1.6.1",
        path="components/esp_websocket_client",
    )


VIVLA_VOICE_ACTION_SCHEMA = maybe_simple_id(
    {cv.GenerateID(): cv.use_id(VivlaVoice)}
)


@automation.register_action("vivla_voice.start", StartAction, VIVLA_VOICE_ACTION_SCHEMA)
async def start_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action("vivla_voice.stop", StopAction, VIVLA_VOICE_ACTION_SCHEMA)
async def stop_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_condition(
    "vivla_voice.is_running", IsRunningCondition, VIVLA_VOICE_ACTION_SCHEMA
)
async def is_running_to_code(config, condition_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, parent)
