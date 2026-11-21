#include "streamdeck_poc.h"
#include "esphome/core/helpers.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include <cstring>
#include <algorithm>

namespace esphome::streamdeck_poc {
static const char *const TAG = "streamdeck_poc";
static constexpr uint16_t VID = 0x0FD9;
static constexpr uint16_t PID = 0x00B8;
static constexpr size_t BMP_BYTES = 54 + 80 * 80 * 3;
static constexpr size_t HID_REPORT_BYTES = 1024;
static constexpr size_t CHUNK_BYTES = 1008;

void StreamDeckPoC::setup() {
  ESP_LOGI(TAG, "Starting USB host (experimental Stream Deck 6-Key Module driver)");
  xTaskCreatePinnedToCore(task_entry_, "deck_usb", 8192, this, 5, nullptr, 0);
}
void StreamDeckPoC::loop() {}
void StreamDeckPoC::task_entry_(void *arg) { static_cast<StreamDeckPoC *>(arg)->task_(); }

void StreamDeckPoC::task_() {
  usb_host_config_t host_cfg = {};
  host_cfg.skip_phy_setup = false;
  host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  esp_err_t err = usb_host_install(&host_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
    vTaskDelete(nullptr);
    return;
  }
  usb_host_client_config_t cfg = {};
  cfg.is_synchronous = false;
  cfg.max_num_event_msg = 5;
  cfg.async.client_event_callback = client_event_;
  cfg.async.callback_arg = this;
  err = usb_host_client_register(&cfg, &client_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "usb_host_client_register failed: %s", esp_err_to_name(err));
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI(TAG, "USB host ready; connect Stream Deck to native USB OTG port (5V VBUS required)");
  while (true) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(0, &flags);
    usb_host_client_handle_events(client_, pdMS_TO_TICKS(10));
    if (connected_ && image_pending_ && !output_busy_) queue_image_chunk_();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void StreamDeckPoC::client_event_(const usb_host_client_event_msg_t *msg, void *arg) {
  auto *self = static_cast<StreamDeckPoC *>(arg);
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) self->on_device_(msg->new_dev.address);
  else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) self->close_device_();
}

void StreamDeckPoC::on_device_(uint8_t address) {
  if (device_) return;
  if (usb_host_device_open(client_, address, &device_) != ESP_OK) return;
  const usb_device_desc_t *dev = nullptr;
  if (usb_host_get_device_descriptor(device_, &dev) != ESP_OK || !dev) { close_device_(); return; }
  ESP_LOGI(TAG, "USB device VID=%04X PID=%04X", dev->idVendor, dev->idProduct);
  if (dev->idVendor != VID || dev->idProduct != PID) {
    ESP_LOGW(TAG, "Not the 6-Key Module; ignoring device");
    close_device_();
    return;
  }
  const usb_config_desc_t *config = nullptr;
  if (usb_host_get_active_config_descriptor(device_, &config) != ESP_OK || !config) {
    ESP_LOGE(TAG, "No configuration descriptor"); close_device_(); return;
  }
  // Walk raw descriptors to find HID interface and interrupt endpoints.
  const uint8_t *raw = reinterpret_cast<const uint8_t *>(config);
  size_t offset = 0;
  bool hid_found = false;
  bool in_hid = false;
  while (offset + 2 <= config->wTotalLength) {
    uint8_t length = raw[offset];
    uint8_t type = raw[offset + 1];
    if (length < 2 || offset + length > config->wTotalLength) break;
    if (type == 4 && length >= 9) {
      in_hid = raw[offset + 5] == 3;
      if (in_hid && !hid_found) {
        hid_found = true;
        interface_ = raw[offset + 2];
        ESP_LOGI(TAG, "HID interface %u", interface_);
      }
    } else if (type == 5 && length >= 7 && in_hid) {
      uint8_t endpoint = raw[offset + 2];
      uint8_t attributes = raw[offset + 3];
      uint16_t packet = raw[offset + 4] | (raw[offset + 5] << 8);
      if ((attributes & 3) == 3) {
        if ((endpoint & 0x80) && !ep_in_) { ep_in_ = endpoint; in_packet_ = packet; }
        if (!(endpoint & 0x80) && !ep_out_) { ep_out_ = endpoint; out_packet_ = packet; }
      }
    }
    offset += length;
  }
  if (!hid_found || !ep_in_) {
    ESP_LOGE(TAG, "HID interrupt IN endpoint missing"); close_device_(); return;
  }
  if (usb_host_interface_claim(client_, device_, interface_, 0) != ESP_OK) {
    ESP_LOGE(TAG, "Cannot claim HID interface"); close_device_(); return;
  }
  ESP_LOGI(TAG, "HID endpoints IN=0x%02X (%u), OUT=0x%02X (%u)", ep_in_, in_packet_, ep_out_, out_packet_);
  if (usb_host_transfer_alloc(std::max<size_t>(in_packet_, 65), 0, &input_) != ESP_OK ||
      usb_host_transfer_alloc(8 + HID_REPORT_BYTES, 0, &output_) != ESP_OK) {
    ESP_LOGE(TAG, "Transfer allocation failed"); close_device_(); return;
  }
  input_->device_handle = device_;
  input_->bEndpointAddress = ep_in_;
  input_->callback = transfer_done_;
  input_->context = this;
  input_->num_bytes = std::max<size_t>(in_packet_, 65);
  output_->device_handle = device_;
  output_->bEndpointAddress = 0;  // HID SET_REPORT on control endpoint
  output_->callback = transfer_done_;
  output_->context = this;
  connected_ = true;
  pressed_ = false;
  prepare_image_(false);
  queue_input_();
}

void StreamDeckPoC::queue_input_() {
  if (!connected_ || !input_) return;
  input_->num_bytes = std::max<size_t>(in_packet_, 65);
  esp_err_t err = usb_host_transfer_submit(input_);
  if (err != ESP_OK) ESP_LOGE(TAG, "Input submit: %s", esp_err_to_name(err));
}

void StreamDeckPoC::prepare_image_(bool enabled) {
  image_on_ = enabled;
  // Windows BMP, 80x80, bottom-up BGR24; rotate 90 degrees clockwise in the pixel map.
  std::memset(image_, 0, sizeof(image_));
  auto w16 = [this](int pos, uint16_t v) { image_[pos] = v & 255; image_[pos+1] = v >> 8; };
  auto w32 = [this](int pos, uint32_t v) { for (int i=0;i<4;i++) image_[pos+i] = (v >> (8*i)) & 255; };
  image_[0] = 'B'; image_[1] = 'M';
  w32(2, BMP_BYTES); w32(10, 54); w32(14, 40);
  w32(18, 80); w32(22, 80); w16(26, 1); w16(28, 24);
  w32(34, 80*80*3);
  // Simple icon: dark background, filled circle and vertical line (power symbol).
  for (int y = 0; y < 80; y++) {
    for (int x = 0; x < 80; x++) {
      // Source icon coordinates (before rotation)
      int sx = y;
      int sy = 79 - x;
      int dx = sx - 40, dy = sy - 40;
      int d2 = dx*dx + dy*dy;
      bool ring = d2 >= 23*23 && d2 <= 28*28 && sy >= 24;
      bool stem = sx >= 37 && sx <= 43 && sy >= 11 && sy <= 39;
      bool fg = ring || stem;
      size_t p = 54 + (y*80+x)*3;
      image_[p+0] = fg ? (enabled ? 35 : 125) : 20;  // blue
      image_[p+1] = fg ? (enabled ? 205 : 125) : 22; // green
      image_[p+2] = fg ? (enabled ? 45 : 125) : 25;  // red
    }
  }
  image_offset_ = 0;
  chunk_index_ = 0;
  image_pending_ = true;
  ESP_LOGI(TAG, "Key 1 image requested: %s", enabled ? "ON" : "OFF");
}

void StreamDeckPoC::queue_image_chunk_() {
  if (!connected_ || !output_ || !image_pending_ || output_busy_) return;
  const size_t remaining = BMP_BYTES - image_offset_;
  const size_t count = std::min(remaining, CHUNK_BYTES);
  uint8_t *buffer = output_->data_buffer;
  std::memset(buffer, 0, 8 + HID_REPORT_BYTES);
  // USB setup packet: HID class SET_REPORT, Output report ID 2, interface index.
  buffer[0] = 0x21; buffer[1] = 0x09;
  buffer[2] = 0x02; buffer[3] = 0x02;
  buffer[4] = interface_; buffer[5] = 0;
  buffer[6] = 0x00; buffer[7] = 0x04;
  uint8_t *report = buffer + 8;
  report[0] = 0x02; report[1] = 0x01;
  report[2] = chunk_index_;
  report[3] = 0;
  report[4] = 1;  // show image
  report[5] = 0;  // key index 0
  std::memcpy(report + 16, image_ + image_offset_, count);
  output_->num_bytes = 8 + HID_REPORT_BYTES;
  output_busy_ = true;
  esp_err_t err = usb_host_transfer_submit_control(client_, output_);
  if (err != ESP_OK) {
    output_busy_ = false;
    ESP_LOGE(TAG, "Image chunk %u submit failed: %s", chunk_index_, esp_err_to_name(err));
    image_pending_ = false;
  } else {
    image_offset_ += count;
    chunk_index_++;
  }
}

void StreamDeckPoC::transfer_done_(usb_transfer_t *transfer) {
  static_cast<StreamDeckPoC *>(transfer->context)->on_transfer_done_(transfer);
}
void StreamDeckPoC::on_transfer_done_(usb_transfer_t *transfer) {
  if (transfer == input_) {
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes >= 7) {
      const uint8_t *r = transfer->data_buffer;
      if (r[0] == 0x01) {
        bool now = r[1] != 0;
        if (now && !pressed_) {
          ESP_LOGI(TAG, "KEY 1 PRESSED -> toggle image");
          prepare_image_(!image_on_);
        }
        pressed_ = now;
      }
    }
    if (connected_) queue_input_();
  } else if (transfer == output_) {
    output_busy_ = false;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
      ESP_LOGE(TAG, "Image transfer failed (status=%d)", transfer->status);
      image_pending_ = false;
    } else if (image_offset_ >= BMP_BYTES) {
      image_pending_ = false;
      ESP_LOGI(TAG, "Image upload complete (%u chunks)", chunk_index_);
    }
  }
}

void StreamDeckPoC::close_device_() {
  connected_ = false;
  image_pending_ = false;
  ESP_LOGW(TAG, "Stream Deck disconnected");
  // Transfers can still be pending when a device is unplugged; avoid freeing
  // their buffers in this early experimental PoC. A later revision will add
  // cancellation, endpoint halt and clean reconnect handling.
  if (device_) {
    if (ep_in_) usb_host_endpoint_halt(device_, ep_in_);
    if (ep_in_) usb_host_endpoint_flush(device_, ep_in_);
    if (ep_in_) usb_host_endpoint_clear(device_, ep_in_);
    usb_host_interface_release(client_, device_, interface_);
    usb_host_device_close(client_, device_);
    device_ = nullptr;
  }
  ep_in_ = ep_out_ = 0;
  input_ = output_ = nullptr;
}
}  // namespace esphome::streamdeck_poc
