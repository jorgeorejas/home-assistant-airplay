#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "vivla_voice.h"

#ifdef USE_ESP_IDF

#include <string>

namespace esphome {
namespace vivla_voice {

template<typename... Ts> class StartAction : public Action<Ts...> {
 public:
  explicit StartAction(VivlaVoice *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->start(); }

 protected:
  VivlaVoice *parent_;
};

template<typename... Ts> class StopAction : public Action<Ts...> {
 public:
  explicit StopAction(VivlaVoice *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->stop(); }

 protected:
  VivlaVoice *parent_;
};

template<typename... Ts> class IsRunningCondition : public Condition<Ts...> {
 public:
  explicit IsRunningCondition(VivlaVoice *parent) : parent_(parent) {}
  bool check(Ts... x) override { return this->parent_->is_running(); }

 protected:
  VivlaVoice *parent_;
};

}  // namespace vivla_voice
}  // namespace esphome

#endif  // USE_ESP_IDF
