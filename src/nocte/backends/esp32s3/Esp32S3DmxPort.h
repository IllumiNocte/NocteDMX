#pragma once

#include <Arduino.h>
#include <atomic>
#include <driver/uart.h>
#include <esp_intr_alloc.h>
#include <hal/uart_ll.h>
#include "../../core/DmxReceiver.h"
#include "../../core/DmxFrame.h"
#include "../../core/RdmReceiver.h"
#include "../../core/Uid.h"

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

// Experimental S3 UART backend: DMX and unicast RDM controller GET/SET.
// Start/stop/configure from one foreground task. Callbacks are ISR-context.
class Esp32S3UartPort {
 public:
  explicit Esp32S3UartPort(uint8_t uartNumber = 1, int8_t txPin = 17, int8_t rxPin = 18);
  ~Esp32S3UartPort();
  Esp32S3UartPort(const Esp32S3UartPort&) = delete;
  Esp32S3UartPort& operator=(const Esp32S3UartPort&) = delete;
  // Legacy full RDM surface (discovery + responder) is not available yet.
  static constexpr bool supportsRdm = false;
  static constexpr bool supportsRdmController = true;
  static constexpr bool supportsRdmDiscovery = false;
  static constexpr bool supportsRdmResponder = false;

  bool setPins(int8_t txPin, int8_t rxPin);
  void setDirectionPin(uint8_t pin); // 255 means direct UART, no direction GPIO.
  void setDirectionPins(uint8_t driverEnablePin, uint8_t receiverEnableNotPin);
  void startOutput();
  void startInput();
  // Send mode only; responder/receive mode is deliberately unsupported.
  void startRDM(uint8_t pin, PortMode mode = PortMode::Send);
  void startRDM(uint8_t driverEnablePin, uint8_t receiverEnableNotPin,
                PortMode mode = PortMode::Send);
  void setUid(const core::Uid& uid) { uid_ = uid; }
  core::Uid uid() const { return uid_; }
  core::RdmCommandResult getRdmParameter(const core::Uid& target, uint16_t pid,
      uint8_t* destination, uint16_t capacity, const uint8_t* requestData = nullptr,
      uint16_t requestLength = 0, uint16_t subDevice = 0);
  core::RdmCommandResult setRdmParameter(const core::Uid& target, uint16_t pid,
      const uint8_t* data, uint16_t length, uint16_t subDevice = 0);
  uint16_t copyRdmResponse(uint8_t* destination, uint16_t capacity) const;
  core::RdmReceiveTiming rdmReceiveTiming() const { return rdmReceiver_.timing(); }
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
  bool pauseOutput();
  core::RdmCommandResult transactRdm(uint16_t length);
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
  std::atomic<bool> pauseRequested_{false};
  std::atomic<bool> outputPaused_{false};
  std::atomic<bool> rdmCapture_{false};
  std::atomic<bool> breakPending_{false};
  std::atomic<bool> lowErrorPending_{false};
  std::atomic<uint32_t> periodUs_{25000};
  bool receiving_ = false;
  bool rdmEnabled_ = false;
  core::Uid uid_{UINT64_C(0x7FF000000002)}; // Experimental MID; set your assigned UID.
  core::RdmReceiver rdmReceiver_;
  uint8_t rdmRequest_[rdm::kMaximumFrameSize] = {};
  uint8_t transaction_ = 0;
  uint32_t lastTransmitEndUs_ = 0;
  bool edgeAttached_ = false;
  uint32_t fallingUs_ = 0;
  bool lowSeen_ = false;
  void (*callback_)(int) = nullptr;
  uint8_t transmit_[kMaximumFrameSize] = {};
  uint16_t transmitLength_ = 0;
  uint16_t transmitIndex_ = 0;
};

} } }
