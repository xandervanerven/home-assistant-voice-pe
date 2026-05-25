#pragma once

#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"

#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace va_client {

class OnPhaseTrigger;

class VaClient : public Component {
 public:
  void set_url(const std::string &url) { url_ = url; }
  void set_token(const std::string &token) { token_ = token; }
  void set_microphone(microphone::Microphone *m) { mic_ = m; }
  void set_mic_channel(uint8_t c) { mic_channel_ = c; }
  void set_speaker(speaker::Speaker *s) { speaker_ = s; }
  void add_on_phase_trigger(OnPhaseTrigger *t) { phase_triggers_.push_back(t); }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // YAML-callable actions.
  void start_session();
  void send_interrupt();

  // Called from the static esp-idf event handler trampoline.
  void on_ws_event(int32_t event_id, void *event_data);

 protected:
  void connect_();
  void schedule_reconnect_();
  void on_mic_data_(const std::vector<uint8_t> &samples);
  void handle_text_(const char *data, size_t len);
  void handle_binary_(const uint8_t *data, size_t len);
  void set_phase_(const std::string &phase);

  std::string url_;
  std::string token_;
  // Lifetime-stable storage referenced by esp_websocket_client_config_t.headers.
  std::string auth_header_;
  uint8_t mic_channel_{0};

  microphone::Microphone *mic_{nullptr};
  speaker::Speaker *speaker_{nullptr};

  // esp_websocket_client_handle_t kept opaque to avoid leaking esp-idf into the header.
  void *ws_handle_{nullptr};
  bool ws_connected_{false};

  uint32_t reconnect_delay_ms_{1000};
  // Set when a reconnect timer is in flight. esp_websocket_client emits both
  // DISCONNECTED and CLOSED (and sometimes ERROR) per failure; without this
  // guard we'd double-bump the backoff delay and double-log.
  bool reconnect_pending_{false};

  std::string current_phase_{"idle"};
  std::vector<OnPhaseTrigger *> phase_triggers_;

  // Scratch buffers reused on the hot path to avoid per-callback heap allocation.
  std::vector<int16_t> mono_buf_;
  std::vector<int16_t> stereo_48k_buf_;
};

}  // namespace va_client
}  // namespace esphome
