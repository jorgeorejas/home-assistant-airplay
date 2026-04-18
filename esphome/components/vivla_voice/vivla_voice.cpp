#include "vivla_voice.h"

#ifdef USE_ESP_IDF

#include "esphome/core/log.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/json/json_util.h"

#include <ArduinoJson.h>
#include <cstring>

namespace esphome {
namespace vivla_voice {

static const char *const TAG = "vivla_voice";

static constexpr size_t MIC_SAMPLE_RATE = 16000;
static constexpr size_t TTS_SAMPLE_RATE = 24000;
static constexpr size_t MIC_CHUNK_SAMPLES = 320;  // 20 ms @ 16 kHz mono
static constexpr size_t MIC_CHUNK_BYTES = MIC_CHUNK_SAMPLES * sizeof(int16_t);

const char *VivlaVoice::state_name_(State s) {
  switch (s) {
    case State::NOT_READY:    return "NOT_READY";
    case State::CONNECTING:   return "CONNECTING";
    case State::IDLE:         return "IDLE";
    case State::LISTENING:    return "LISTENING";
    case State::THINKING:     return "THINKING";
    case State::REPLYING:     return "REPLYING";
    case State::ERROR_STATE:  return "ERROR";
  }
  return "?";
}

void VivlaVoice::setup() {
  ESP_LOGCONFIG(TAG, "Setting up vivla_voice (url=%s)", this->url_.c_str());
  if (this->mic_source_ != nullptr) {
    // The MicrophoneSource only fires this callback while it is started, so
    // chunks are auto-gated to LISTENING state — no extra check needed.
    this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
      if (data.empty()) return;
      this->send_mic_chunk_(data.data(), data.size());
    });
  }
  if (this->speaker_ != nullptr) {
    // Tell the (resampler) speaker what we're going to feed it: PCM16 mono 24 kHz.
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, TTS_SAMPLE_RATE));
  }
  this->set_state_(State::NOT_READY);
}

void VivlaVoice::dump_config() {
  ESP_LOGCONFIG(TAG, "Vivla Voice:");
  ESP_LOGCONFIG(TAG, "  URL: %s", this->url_.c_str());
  ESP_LOGCONFIG(TAG, "  Token: %s", this->token_.empty() ? "<unset>" : "<set>");
  ESP_LOGCONFIG(TAG, "  Reconnect: %u ms", this->reconnect_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Microphone source: %s", this->mic_source_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Speaker: %s", this->speaker_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  micro_wake_word: %s", this->mww_ ? "yes" : "no");
}

void VivlaVoice::loop() {
  // Reconnect logic — only attempt if WiFi is up (handled by AFTER_WIFI priority).
  if (!this->ws_connected_ && this->state_ != State::CONNECTING) {
    const uint32_t now = millis();
    if (now - this->last_reconnect_attempt_ms_ >= this->reconnect_interval_ms_) {
      this->last_reconnect_attempt_ms_ = now;
      this->connect_ws_();
    }
  }
}

void VivlaVoice::connect_ws_() {
  if (this->ws_ != nullptr) {
    esp_websocket_client_destroy(this->ws_);
    this->ws_ = nullptr;
  }

  esp_websocket_client_config_t cfg = {};
  cfg.uri = this->url_.c_str();
  cfg.disable_auto_reconnect = true;          // we handle reconnect in loop()
  cfg.network_timeout_ms = 10000;
  cfg.reconnect_timeout_ms = 5000;

  // Bearer token via Authorization header on upgrade. Falls back to ?token= if
  // the URL already encodes it; both are accepted by the bridge.
  std::string headers;
  if (!this->token_.empty()) {
    headers = "Authorization: Bearer " + this->token_ + "\r\n";
    cfg.headers = headers.c_str();
  }

  this->ws_ = esp_websocket_client_init(&cfg);
  if (this->ws_ == nullptr) {
    ESP_LOGE(TAG, "esp_websocket_client_init failed");
    this->set_state_(State::ERROR_STATE);
    return;
  }

  esp_websocket_register_events(this->ws_, WEBSOCKET_EVENT_ANY,
                                &VivlaVoice::ws_event_handler_, this);

  this->set_state_(State::CONNECTING);
  ESP_LOGI(TAG, "connecting to %s", this->url_.c_str());
  if (esp_websocket_client_start(this->ws_) != ESP_OK) {
    ESP_LOGE(TAG, "esp_websocket_client_start failed");
    this->on_ws_error_("client_start_failed");
  }
}

void VivlaVoice::disconnect_ws_() {
  if (this->ws_ == nullptr) return;
  esp_websocket_client_close(this->ws_, portMAX_DELAY);
  esp_websocket_client_destroy(this->ws_);
  this->ws_ = nullptr;
  this->ws_connected_ = false;
}

void VivlaVoice::ws_event_handler_(void *handler_args, esp_event_base_t base,
                                   int32_t event_id, void *event_data) {
  auto *self = static_cast<VivlaVoice *>(handler_args);
  auto *evt = static_cast<esp_websocket_event_data_t *>(event_data);

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      self->on_ws_connected_();
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      self->on_ws_disconnected_();
      break;
    case WEBSOCKET_EVENT_DATA:
      if (evt == nullptr || evt->data_ptr == nullptr || evt->data_len == 0) break;
      // op_code 0x1 = text, 0x2 = binary, 0x8 = close, 0x9 = ping, 0xA = pong
      if (evt->op_code == 0x1) {
        self->on_ws_text_(evt->data_ptr, evt->data_len);
      } else if (evt->op_code == 0x2) {
        self->on_ws_binary_(reinterpret_cast<const uint8_t *>(evt->data_ptr),
                            evt->data_len);
      }
      break;
    case WEBSOCKET_EVENT_ERROR:
      self->on_ws_error_("transport_error");
      break;
    default:
      break;
  }
}

void VivlaVoice::on_ws_connected_() {
  ESP_LOGI(TAG, "ws connected");
  this->ws_connected_ = true;
  // Don't fire on_client_connected yet — wait for "session.ready" so the LED
  // phase only flips to idle once Gemini Live is actually serving requests.
}

void VivlaVoice::on_ws_disconnected_() {
  ESP_LOGW(TAG, "ws disconnected");
  this->ws_connected_ = false;
  bool was_running = this->is_running();
  this->set_state_(State::NOT_READY);
  this->on_client_disconnected_.trigger();
  if (was_running) {
    this->stop_microphone_();
    this->on_end_.trigger();
  }
}

void VivlaVoice::on_ws_error_(const std::string &reason) {
  ESP_LOGW(TAG, "ws error: %s", reason.c_str());
  this->set_state_(State::ERROR_STATE);
  this->on_error_.trigger(reason);
}

void VivlaVoice::on_ws_text_(const char *data, size_t len) {
  // Parse the type field, dispatch to handler.
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if (err) {
    ESP_LOGW(TAG, "bad json (%zu bytes): %s", len, err.c_str());
    return;
  }
  const char *type = doc["type"] | "";
  this->handle_event_(type, std::string(data, len));
}

void VivlaVoice::handle_event_(const std::string &type, const std::string &raw_json) {
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, raw_json) != DeserializationError::Ok) return;

  if (type == "session.ready") {
    this->set_state_(State::IDLE);
    this->on_client_connected_.trigger();
  } else if (type == "transcript") {
    std::string role = doc["role"] | "";
    std::string text = doc["text"] | "";
    this->on_transcript_.trigger(role, text);
    if (role == "assistant" && !text.empty()) {
      this->on_intent_progress_.trigger(text);
    }
  } else if (type == "tool.call") {
    std::string name = doc["name"] | "";
    ESP_LOGD(TAG, "tool call: %s", name.c_str());
    // Tool calls execute server-side; nothing for the device to do here.
  } else if (type == "interrupt") {
    // The model was interrupted by the user (barge-in). Treat as a fresh turn.
    if (this->state_ == State::REPLYING) this->set_state_(State::LISTENING);
  } else if (type == "turn.complete") {
    this->stop_microphone_();
    this->set_state_(State::IDLE);
    this->on_end_.trigger();
  } else if (type == "error") {
    std::string msg = doc["message"] | "unknown";
    std::string code = doc["code"] | "";
    if (code.empty()) code = msg;
    this->set_state_(State::ERROR_STATE);
    this->on_error_.trigger(code);
  } else if (type == "session.closed") {
    this->set_state_(State::NOT_READY);
  }
}

void VivlaVoice::on_ws_binary_(const uint8_t *data, size_t len) {
  if (this->state_ == State::THINKING || this->state_ == State::IDLE) {
    // First TTS chunk after a turn — flip to REPLYING and fire on_tts_start.
    this->set_state_(State::REPLYING);
    this->on_tts_start_.trigger();
  }
  this->play_tts_chunk_(data, len);
}

// ============================================================================
// Audio plumbing — the spots that need hardware iteration.
// ============================================================================

void VivlaVoice::start() {
  if (!this->ws_connected_ || this->state_ == State::ERROR_STATE) {
    ESP_LOGW(TAG, "start() called while not connected (state=%s)",
             state_name_(this->state_));
    return;
  }
  if (this->is_running()) {
    ESP_LOGD(TAG, "start() ignored: already running");
    return;
  }
  this->set_state_(State::LISTENING);
  this->on_start_.trigger();
  this->on_listening_.trigger();
  this->on_stt_vad_start_.trigger();   // fired immediately; Gemini does VAD server-side
  this->start_microphone_();
}

void VivlaVoice::stop() {
  if (!this->is_running()) return;
  this->stop_microphone_();
  // Tell the bridge we're done speaking; Gemini will commit the input buffer.
  if (this->ws_connected_ && this->ws_ != nullptr) {
    static const char *END = R"({"type":"audio.end"})";
    esp_websocket_client_send_text(this->ws_, END, std::strlen(END), portMAX_DELAY);
  }
  this->on_stt_vad_end_.trigger();
  this->set_state_(State::THINKING);
}

void VivlaVoice::start_microphone_() {
  if (this->mic_source_ == nullptr) return;
  this->mic_source_->start();
}

void VivlaVoice::stop_microphone_() {
  if (this->mic_source_ == nullptr) return;
  this->mic_source_->stop();
}

void VivlaVoice::send_mic_chunk_(const uint8_t *pcm, size_t len) {
  if (!this->ws_connected_ || this->ws_ == nullptr) return;
  if (this->state_ != State::LISTENING) return;  // safety: drop late callbacks
  esp_websocket_client_send_bin(this->ws_, reinterpret_cast<const char *>(pcm), len,
                                portMAX_DELAY);
}

void VivlaVoice::play_tts_chunk_(const uint8_t *pcm24k, size_t len) {
  if (this->speaker_ == nullptr) {
    ESP_LOGV(TAG, "tts chunk dropped: no speaker (%zu bytes)", len);
    return;
  }
  if (this->speaker_->is_stopped()) this->speaker_->start();
  size_t written = this->speaker_->play(pcm24k, len);
  if (written < len) {
    ESP_LOGW(TAG, "speaker dropped %zu/%zu bytes (buffer full)", len - written, len);
  }
}

void VivlaVoice::set_state_(State new_state) {
  if (this->state_ == new_state) return;
  ESP_LOGD(TAG, "state %s → %s", state_name_(this->state_), state_name_(new_state));
  this->state_ = new_state;
}

}  // namespace vivla_voice
}  // namespace esphome

#endif  // USE_ESP_IDF
