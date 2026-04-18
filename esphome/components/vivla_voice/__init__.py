"""ESPHome external component: vivla_voice.

Mirrors a useful subset of the built-in voice_assistant: API so the existing
LED/UX YAML transplants 1:1, but talks to the vivla.ai voice gateway over
WebSocket instead of Home Assistant's native API.

Wire format (matches apps/mastra/voice-bridge/README.md):
- Out: PCM16 LE mono 16 kHz binary frames; {"type":"audio.end"} text frame on stop.
- In:  PCM16 LE mono 24 kHz binary frames + JSON control events.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.automation import maybe_simple_id
from esphome.components import microphone, speaker, micro_wake_word
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_URL, CONF_TOKEN

CODEOWNERS = ["@vivla"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["json"]

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

NoPayloadTrigger = vivla_voice_ns.class_("NoPayloadTrigger", automation.Trigger.template())
ErrorTrigger = vivla_voice_ns.class_(
    "ErrorTrigger", automation.Trigger.template(cg.std_string)
)
IntentProgressTrigger = vivla_voice_ns.class_(
    "IntentProgressTrigger", automation.Trigger.template(cg.std_string)
)
TranscriptTrigger = vivla_voice_ns.class_(
    "TranscriptTrigger", automation.Trigger.template(cg.std_string, cg.std_string)
)


def _no_payload_validator(value):
    return automation.validate_automation(
        {cv.GenerateID(CONF_ID): cv.declare_id(NoPayloadTrigger)}
    )(value)


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
        cv.Optional(CONF_ON_CLIENT_CONNECTED): _no_payload_validator,
        cv.Optional(CONF_ON_CLIENT_DISCONNECTED): _no_payload_validator,
        cv.Optional(CONF_ON_START): _no_payload_validator,
        cv.Optional(CONF_ON_LISTENING): _no_payload_validator,
        cv.Optional(CONF_ON_STT_VAD_START): _no_payload_validator,
        cv.Optional(CONF_ON_STT_VAD_END): _no_payload_validator,
        cv.Optional(CONF_ON_TTS_START): _no_payload_validator,
        cv.Optional(CONF_ON_END): _no_payload_validator,
        cv.Optional(CONF_ON_ERROR): automation.validate_automation(
            {cv.GenerateID(CONF_ID): cv.declare_id(ErrorTrigger)}
        ),
        cv.Optional(CONF_ON_INTENT_PROGRESS): automation.validate_automation(
            {cv.GenerateID(CONF_ID): cv.declare_id(IntentProgressTrigger)}
        ),
        cv.Optional(CONF_ON_TRANSCRIPT): automation.validate_automation(
            {cv.GenerateID(CONF_ID): cv.declare_id(TranscriptTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


FINAL_VALIDATE_SCHEMA = microphone.final_validate_microphone_source_schema(
    "vivla_voice", sample_rate=MIC_SAMPLE_RATE
)


_NO_PAYLOAD_TRIGGER_KEYS = (
    CONF_ON_CLIENT_CONNECTED,
    CONF_ON_CLIENT_DISCONNECTED,
    CONF_ON_START,
    CONF_ON_LISTENING,
    CONF_ON_STT_VAD_START,
    CONF_ON_STT_VAD_END,
    CONF_ON_TTS_START,
    CONF_ON_END,
)

_TRIGGER_GETTERS = {
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

    for key in _NO_PAYLOAD_TRIGGER_KEYS:
        for conf in config.get(key, []):
            getter = getattr(var, _TRIGGER_GETTERS[key])
            trig = cg.new_Pvariable(conf[CONF_ID], getter())
            await automation.build_automation(trig, [], conf)

    for conf in config.get(CONF_ON_ERROR, []):
        trig = cg.new_Pvariable(conf[CONF_ID], var.get_error_trigger())
        await automation.build_automation(trig, [(cg.std_string, "code")], conf)

    for conf in config.get(CONF_ON_INTENT_PROGRESS, []):
        trig = cg.new_Pvariable(conf[CONF_ID], var.get_intent_progress_trigger())
        await automation.build_automation(trig, [(cg.std_string, "x")], conf)

    for conf in config.get(CONF_ON_TRANSCRIPT, []):
        trig = cg.new_Pvariable(conf[CONF_ID], var.get_transcript_trigger())
        await automation.build_automation(
            trig, [(cg.std_string, "role"), (cg.std_string, "text")], conf
        )

    # ESP-IDF managed component for the WebSocket client.
    cg.add_library("espressif/esp_websocket_client", "1.5.0")


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
