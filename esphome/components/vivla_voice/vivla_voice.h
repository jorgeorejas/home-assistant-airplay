#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

#ifdef USE_ESP_IDF

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/micro_wake_word/micro_wake_word.h"

#include <esp_websocket_client.h>

#include <string>
#include <vector>

namespace esphome {
namespace vivla_voice {

enum class State : uint8_t {
  NOT_READY = 0,
  CONNECTING,
  IDLE,            // session ready, waiting for start()
  LISTENING,       // mic streaming up
  THINKING,        // mic stopped, waiting for first reply
  REPLYING,        // receiving / playing tts
  ERROR_STATE,
};

class VivlaVoice : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // ----- config setters (called from generated codegen) -----
  void set_url(const std::string &url) { this->url_ = url; }
  void set_token(const std::string &token) { this->token_ = token; }
  void set_reconnect_interval(uint32_t ms) { this->reconnect_interval_ms_ = ms; }
  void set_microphone_source(microphone::MicrophoneSource *mic_source) {
    this->mic_source_ = mic_source;
  }
  void set_speaker(speaker::Speaker *spkr) { this->speaker_ = spkr; }
  void set_micro_wake_word(micro_wake_word::MicroWakeWord *mww) { this->mww_ = mww; }

  // ----- runtime API used by actions/conditions -----
  void start();
  void stop();
  bool is_running() const {
    return this->state_ == State::LISTENING || this->state_ == State::THINKING ||
           this->state_ == State::REPLYING;
  }

  State state() const { return this->state_; }

  // ----- trigger accessors (codegen wires them up) -----
  Trigger<> *get_client_connected_trigger() { return &this->on_client_connected_; }
  Trigger<> *get_client_disconnected_trigger() { return &this->on_client_disconnected_; }
  Trigger<> *get_start_trigger() { return &this->on_start_; }
  Trigger<> *get_listening_trigger() { return &this->on_listening_; }
  Trigger<> *get_stt_vad_start_trigger() { return &this->on_stt_vad_start_; }
  Trigger<> *get_stt_vad_end_trigger() { return &this->on_stt_vad_end_; }
  Trigger<> *get_tts_start_trigger() { return &this->on_tts_start_; }
  Trigger<> *get_end_trigger() { return &this->on_end_; }
  Trigger<std::string> *get_error_trigger() { return &this->on_error_; }
  Trigger<std::string> *get_intent_progress_trigger() { return &this->on_intent_progress_; }
  Trigger<std::string, std::string> *get_transcript_trigger() {
    return &this->on_transcript_;
  }

 protected:
  // WS lifecycle
  void connect_ws_();
  void disconnect_ws_();
  static void ws_event_handler_(void *handler_args, esp_event_base_t base, int32_t id,
                                void *event_data);
  void on_ws_connected_();
  void on_ws_disconnected_();
  void on_ws_text_(const char *data, size_t len);
  void on_ws_binary_(const uint8_t *data, size_t len);
  void on_ws_error_(const std::string &reason);

  // App-level protocol parsing
  void handle_event_(const std::string &type, const std::string &raw_json);

  // Audio plumbing
  void start_microphone_();
  void stop_microphone_();
  void send_mic_chunk_(const uint8_t *pcm, size_t len);
  void play_tts_chunk_(const uint8_t *pcm24k, size_t len);

  // State machine
  void set_state_(State new_state);
  static const char *state_name_(State s);

  // Config
  std::string url_;
  std::string token_;
  uint32_t reconnect_interval_ms_{5000};
  microphone::MicrophoneSource *mic_source_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  micro_wake_word::MicroWakeWord *mww_{nullptr};

  // Runtime
  esp_websocket_client_handle_t ws_{nullptr};
  bool ws_connected_{false};
  uint32_t last_reconnect_attempt_ms_{0};
  State state_{State::NOT_READY};

  // Triggers
  Trigger<> on_client_connected_;
  Trigger<> on_client_disconnected_;
  Trigger<> on_start_;
  Trigger<> on_listening_;
  Trigger<> on_stt_vad_start_;
  Trigger<> on_stt_vad_end_;
  Trigger<> on_tts_start_;
  Trigger<> on_end_;
  Trigger<std::string> on_error_;
  Trigger<std::string> on_intent_progress_;
  Trigger<std::string, std::string> on_transcript_;
};

}  // namespace vivla_voice
}  // namespace esphome

#endif  // USE_ESP_IDF
