#pragma once
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "usb/usb_host.h"
#include <cstdint>

namespace esphome::streamdeck {

class StreamDeck : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 private:
  static void task_entry_(void *arg);
  void task_();
  static void client_event_(const usb_host_client_event_msg_t *msg, void *arg);
  static void transfer_done_(usb_transfer_t *transfer);
  void on_transfer_done_(usb_transfer_t *transfer);
  void on_device_(uint8_t address);
  void close_device_();
  void queue_input_();
  void queue_image_chunk_();
  void prepare_image_(bool enabled);

  usb_host_client_handle_t client_{nullptr};
  usb_device_handle_t device_{nullptr};
  usb_transfer_t *input_{nullptr};
  usb_transfer_t *output_{nullptr};
  uint8_t interface_{0};
  uint8_t ep_in_{0};
  uint8_t ep_out_{0};
  uint16_t in_packet_{0};
  uint16_t out_packet_{0};
  bool connected_{false};
  bool pressed_{false};
  bool image_on_{false};
  bool image_pending_{false};
  bool output_busy_{false};
  size_t image_offset_{0};
  uint8_t chunk_index_{0};
  uint8_t image_[19254]{};  // 54-byte BMP header + 80*80*3
};

}  // namespace esphome::streamdeck
