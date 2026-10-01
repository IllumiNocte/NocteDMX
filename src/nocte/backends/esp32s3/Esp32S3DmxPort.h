#pragma once

#include <Arduino.h>
#include <atomic>
#include <driver/uart.h>
#include <esp_intr_alloc.h>
#include <hal/uart_ll.h>
#include "../../core/DmxReceiver.h"
#include "../../core/DmxFrame.h"

namespace nocte { namespace dmx { namespace backends {

enum class S3PortError : uint8_t {
  None, InvalidConfiguration, UartBusy, NotInternalRam, HardwareSetup,
  InterruptSetup, TaskSetup,
};

struct S3PortStatistics {
  uint32_t receivedFrames = 0;
  uint32_t receiveErrors = 0;
  uint32_t transmittedFrames = 0;
  uint32_t transmitTimeouts = 0;
};

// Experimental S3 UART backend. DMX only in this milestone; no RDM API yet.
// Start/stop/configure from one foreground task. Callbacks are ISR-context.
class Esp32S3UartPort {
 public:
  explicit Esp32S3UartPort(uint8_t uartNumber = 1, int8_t txPin = 17, int8_t rxPin = 18);
  ~Esp32S3UartPort();
  Esp32S3UartPort(const Esp32S3UartPort&) = delete;
  Esp32S3UartPort& operator=(const Esp32S3UartPort&) = delete;
  static constexpr bool supportsRdm = false;

  bool setPins(int8_t txPin, int8_t rxPin);
  void setDirectionPin(uint8_t pin); // 255 means direct UART, no direction GPIO.
  void setDirectionPins(uint8_t driverEnablePin, uint8_t receiverEnableNotPin);
  void startOutput();
  void startInput();
  void stop();
  bool isActive() const { return active_.load(); }
  S3PortError lastError() const { return error_; }
  S3PortStatistics statistics();
  bool setRefreshRate(uint16_t framesPerSecond); // 1-44, default 40 Hz.
  uint16_t numberOfSlots();
  void setMaxSlots(int slots);
  void setSlot(int slot, uint8_t value);
  uint8_t getSlot(int slot);
  bool setFrame(const uint8_t* data, uint16_t slots = kMaximumSlots);
  uint16_t copyFrame(uint8_t* destination, uint16_t capacity = kMaximumSlots);
  void clearSlots();
  void setDataReceivedCallback(void (*callback)(int));

 private:
  bool initialize(bool receive);
  void releaseHardware();
  void setDirection(bool transmit);
  bool transmitFrame();
  static void outputTask(void* argument);
  static void IRAM_ATTR uartInterrupt(void* argument);
  static void IRAM_ATTR rxEdgeInterrupt(void* argument);
  void IRAM_ATTR fillTxFifo();
  bool IRAM_ATTR rxLevel() const;

  uart_port_t uartNumber_;
  int8_t txPin_;
  int8_t rxPin_;
  int8_t dePin_ = -1;
  int8_t reNotPin_ = -1;
  uart_dev_t* hardware_ = nullptr;
  intr_handle_t interrupt_ = nullptr;
  TaskHandle_t task_ = nullptr;
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  core::FrameStorage frames_;
  core::DmxReceiver receiver_;
  S3PortStatistics statistics_;
  S3PortError error_ = S3PortError::None;
  std::atomic<bool> active_{false};
  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> breakPending_{false};
  std::atomic<bool> lowErrorPending_{false};
  std::atomic<uint32_t> periodUs_{25000};
  bool receiving_ = false;
  bool edgeAttached_ = false;
  uint32_t fallingUs_ = 0;
  bool lowSeen_ = false;
  void (*callback_)(int) = nullptr;
  uint8_t transmit_[kMaximumFrameSize] = {};
  uint16_t transmitLength_ = 0;
  uint16_t transmitIndex_ = 0;
};

} } }
