#if defined(ARDUINO_ARCH_ESP32)
#include <sdkconfig.h>
#if defined(CONFIG_IDF_TARGET_ESP32S3)

#include "Esp32S3DmxPort.h"
#include <driver/gpio.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>
#include <soc/uart_periph.h>
#include <rdm/rdm_utility.h>
#include "../../core/UartBreak.h"

namespace nocte { namespace dmx { namespace backends {
namespace {
portMUX_TYPE ownershipLock = portMUX_INITIALIZER_UNLOCKED;
Esp32S3UartPort* owners[SOC_UART_NUM] = {};
constexpr uint32_t kRxInterrupts = UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT
    | UART_INTR_BRK_DET | UART_INTR_FRAM_ERR | UART_INTR_PARITY_ERR | UART_INTR_RXFIFO_OVF;
constexpr uint32_t kRxMinimumBreakUs = 88;
constexpr uint32_t kTxBreakUs = 176;
constexpr uint32_t kTxMabUs = 16;
}

Esp32S3UartPort::Esp32S3UartPort(uint8_t uartNumber, int8_t txPin, int8_t rxPin)
    : uartNumber_(static_cast<uart_port_t>(uartNumber)), txPin_(txPin), rxPin_(rxPin),
      receiver_(frames_) {}
Esp32S3UartPort::~Esp32S3UartPort() { stop(); }

bool Esp32S3UartPort::setPins(int8_t txPin, int8_t rxPin) {
  if (isActive() || txPin == rxPin || !GPIO_IS_VALID_OUTPUT_GPIO(txPin)
      || !GPIO_IS_VALID_GPIO(rxPin)) return false;
  txPin_ = txPin;
  rxPin_ = rxPin;
  return true;
}

void Esp32S3UartPort::setDirectionPin(uint8_t pin) {
  setDirectionPins(pin, 255);
}
void Esp32S3UartPort::setDirectionPins(uint8_t de, uint8_t reNot) {
  if (isActive()) return;
  if ((de != 255 && !GPIO_IS_VALID_OUTPUT_GPIO(de))
      || (reNot != 255 && !GPIO_IS_VALID_OUTPUT_GPIO(reNot))) {
    error_ = S3PortError::InvalidConfiguration;
    return;
  }
  dePin_ = de == 255 ? -1 : static_cast<int8_t>(de);
  reNotPin_ = reNot == 255 ? -1 : static_cast<int8_t>(reNot);
}
void Esp32S3UartPort::setDirection(bool transmit) {
  // Pin modes are established during initialize(). Register-only writes keep
  // the RDM TX_DONE -> receive transition IRAM-safe and independent of tasks.
  if (transmit) {
    if (reNotPin_ >= 0) gpio_ll_set_level(&GPIO, reNotPin_, 1);
    if (dePin_ >= 0) gpio_ll_set_level(&GPIO, dePin_, 1);
  } else {
    if (dePin_ >= 0) gpio_ll_set_level(&GPIO, dePin_, 0);
    if (reNotPin_ >= 0) gpio_ll_set_level(&GPIO, reNotPin_, 0);
  }
}

void Esp32S3UartPort::beginTransmit() {
  // A short critical section prevents RTOS preemption extending BREAK/MAB.
  // Interrupts are restored as soon as data starts; NEVER mask a whole packet.
  uart_ll_inverse_signal(hardware_, UART_SIGNAL_TXD_INV);
  esp_rom_delay_us(kTxBreakUs);
  uart_ll_inverse_signal(hardware_, 0);
  esp_rom_delay_us(kTxMabUs);
  fillTxFifo();
}

void Esp32S3UartPort::finishRdmTransmit() {
  // TX_DONE can precede the FSM's IDLE register update by a few peripheral
  // clocks. Espressif's driver also allows 2 us before inspecting that state.
  esp_rom_delay_us(2);
  if (!rdmTxPending_.load() || transmitIndex_ != transmitLength_
      || !uart_ll_is_tx_idle(hardware_)) return;
  const uint32_t end = static_cast<uint32_t>(esp_timer_get_time());
  uart_ll_disable_intr_mask(hardware_, UART_INTR_TXFIFO_EMPTY | UART_INTR_TX_DONE);
  rdmRequestEndUs_ = end;
  lastTransmitEndUs_ = end;
  rdmReceiver_.begin(end);
  discoveryReceiver_.begin(end);
  lowSeen_ = false;
  breakPending_.store(false);
  lowErrorPending_.store(false);
  uart_ll_rxfifo_rst(hardware_);
  uart_ll_clr_intsts_mask(hardware_, kRxInterrupts);
  const bool response = rdmTransaction_ != RdmTransaction::Broadcast;
  rdmCapture_.store(response);
  if (response) {
    uart_ll_ena_intr_mask(hardware_, kRxInterrupts);
    setDirection(false);
  } // Broadcast requests keep driving MARK; there is no responder turn.
  rdmTxPending_.store(false);
  rdmTxDone_.store(true); // Publish only after timing/receive/direction are ready.
}

bool Esp32S3UartPort::initialize(bool receive) {
  error_ = S3PortError::None;
  // UART0 belongs to the console/bootloader. The first backend leases UART1/2.
  if (uartNumber_ < UART_NUM_1 || uartNumber_ >= UART_NUM_MAX || txPin_ == rxPin_
      || !GPIO_IS_VALID_OUTPUT_GPIO(txPin_) || !GPIO_IS_VALID_GPIO(rxPin_)
      || (dePin_ >= 0 && (!GPIO_IS_VALID_OUTPUT_GPIO(dePin_) || dePin_ == txPin_ || dePin_ == rxPin_))
      || (reNotPin_ >= 0 && (!GPIO_IS_VALID_OUTPUT_GPIO(reNotPin_) || reNotPin_ == txPin_ || reNotPin_ == rxPin_))) {
    error_ = S3PortError::InvalidConfiguration;
    return false;
  }
  if (!esp_ptr_internal(this)) {
    error_ = S3PortError::NotInternalRam;
    return false;
  }
  portENTER_CRITICAL(&ownershipLock);
  bool busy = owners[uartNumber_] || uart_is_driver_installed(uartNumber_);
  const int8_t pins[] = {txPin_, rxPin_, dePin_, reNotPin_};
  for (auto* owner : owners) {
    if (!owner) continue;
    const int8_t occupied[] = {owner->txPin_, owner->rxPin_, owner->dePin_, owner->reNotPin_};
    for (const auto pin : pins)
      for (const auto other : occupied)
        if (pin >= 0 && pin == other) busy = true;
  }
  if (!busy) owners[uartNumber_] = this;
  portEXIT_CRITICAL(&ownershipLock);
  if (busy) { error_ = S3PortError::UartBusy; return false; }

  uart_config_t config = {};
  config.baud_rate = 250000;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_2;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_XTAL;
  if (uart_param_config(uartNumber_, &config) != ESP_OK
      || uart_set_pin(uartNumber_, txPin_, rxPin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    error_ = S3PortError::HardwareSetup;
    releaseHardware();
    return false;
  }
  hardware_ = UART_LL_GET_HW(uartNumber_);
  uart_ll_disable_intr_mask(hardware_, UINT32_MAX);
  uart_ll_clr_intsts_mask(hardware_, UINT32_MAX);
  uart_ll_txfifo_rst(hardware_);
  uart_ll_rxfifo_rst(hardware_);
  uart_ll_inverse_signal(hardware_, 0);
  uart_ll_set_tx_idle_num(hardware_, 0);
  uart_ll_set_txfifo_empty_thr(hardware_, 64);
  uart_ll_set_rxfifo_full_thr(hardware_, 1);
  uart_ll_set_rx_tout(hardware_, 44); // Drain idle RX; not a DMX frame delimiter.
  if (esp_intr_alloc(uart_periph_signal[uartNumber_].irq,
                     ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1,
                     uartInterrupt, this, &interrupt_) != ESP_OK) {
    error_ = S3PortError::InterruptSetup;
    releaseHardware();
    return false;
  }
  if (dePin_ >= 0) pinMode(dePin_, OUTPUT);
  if (reNotPin_ >= 0) pinMode(reNotPin_, OUTPUT);
  setDirection(!receive);
  receiving_ = receive;
  receiver_.reset();
  breakPending_.store(false);
  lowErrorPending_.store(false);
  lowSeen_ = false;
  stopRequested_.store(false);
  pauseRequested_.store(false);
  outputPaused_.store(false);
  rdmCapture_.store(false);
  rdmTxPending_.store(false);
  rdmTxDone_.store(false);
  active_.store(true);
  if (receive || rdmEnabled_) {
    if (receive) frames_.slots = 0;
    gpio_pullup_en(static_cast<gpio_num_t>(rxPin_));
    // UART BREAK alone does not qualify the physical low duration. The GPIO
    // edge ISR measures it; UART IRQs still handle all character data.
    attachInterruptArg(rxPin_, rxEdgeInterrupt, this, CHANGE);
    edgeAttached_ = true;
    if (receive) uart_ll_ena_intr_mask(hardware_, kRxInterrupts);
  }
  return true;
}

void Esp32S3UartPort::releaseHardware() {
  active_.store(false);
  portENTER_CRITICAL(&lock_);
  rdmTxPending_.store(false);
  rdmTxDone_.store(false);
  if (hardware_) uart_ll_disable_intr_mask(hardware_, UINT32_MAX);
  portEXIT_CRITICAL(&lock_);
  if (edgeAttached_) { detachInterrupt(rxPin_); edgeAttached_ = false; }
  if (interrupt_) { esp_intr_free(interrupt_); interrupt_ = nullptr; }
  if (hardware_) {
    uart_ll_inverse_signal(hardware_, 0);
    uart_ll_txfifo_rst(hardware_);
    uart_ll_rxfifo_rst(hardware_);
    uart_ll_clr_intsts_mask(hardware_, UINT32_MAX);
  }
  setDirection(false);
  active_.store(false);
  portENTER_CRITICAL(&ownershipLock);
  if (uartNumber_ >= UART_NUM_1 && uartNumber_ < UART_NUM_MAX && owners[uartNumber_] == this)
    owners[uartNumber_] = nullptr;
  portEXIT_CRITICAL(&ownershipLock);
  hardware_ = nullptr;
}

void Esp32S3UartPort::startInput() {
  if (isActive() && receiving_) return;
  stop();
  rdmEnabled_ = false;
  initialize(true);
}
void Esp32S3UartPort::startOutput() {
  if (isActive() && !receiving_ && !rdmEnabled_) return;
  stop();
  rdmEnabled_ = false;
  if (!initialize(false)) return;
  if (!core::isValidOutputSlotCount(frames_.slots)) frames_.slots = kMaximumSlots;
  if (xTaskCreatePinnedToCore(outputTask, "NocteDMX TX", 3072, this, 4, &task_,
                              xPortGetCoreID()) != pdPASS) {
    error_ = S3PortError::TaskSetup;
    task_ = nullptr;
    releaseHardware();
  }
}
void Esp32S3UartPort::startRDM(uint8_t pin, PortMode mode) {
  startRDM(pin, 255, mode);
}
void Esp32S3UartPort::startRDM(uint8_t de, uint8_t reNot, PortMode mode) {
  stop();
  rdmEnabled_ = false;
  if (mode != PortMode::Send) { error_ = S3PortError::InvalidConfiguration; return; }
  error_ = S3PortError::None;
  setDirectionPins(de, reNot);
  if (error_ != S3PortError::None) return;
  rdmEnabled_ = true;
  if (!initialize(false)) { rdmEnabled_ = false; return; }
  if (!core::isValidOutputSlotCount(frames_.slots)) frames_.slots = kMaximumSlots;
  if (xTaskCreatePinnedToCore(outputTask, "NocteDMX TX", 3072, this, 4, &task_,
                              xPortGetCoreID()) != pdPASS) {
    error_ = S3PortError::TaskSetup;
    task_ = nullptr;
    releaseHardware();
  }
}
void Esp32S3UartPort::stop() {
  if (!isActive()) return;
  stopRequested_.store(true);
  pauseRequested_.store(false);
  TaskHandle_t worker;
  portENTER_CRITICAL(&lock_);
  worker = task_;
  // Keep the worker from clearing/deleting its handle between lookup and wake.
  if (worker) xTaskNotifyGive(worker);
  portEXIT_CRITICAL(&lock_);
  // Cooperative shutdown: never delete a task while it owns a spinlock/FIFO.
  while (worker) {
    vTaskDelay(1);
    portENTER_CRITICAL(&lock_);
    worker = task_;
    portEXIT_CRITICAL(&lock_);
  }
  releaseHardware();
}

bool Esp32S3UartPort::transmitFrame() {
  const uint32_t started = static_cast<uint32_t>(esp_timer_get_time());
  // Snapshot before BREAK so channel count/copy time cannot extend MAB.
  portENTER_CRITICAL(&lock_);
  transmitLength_ = frames_.slots + 1;
  for (uint16_t i = 0; i < transmitLength_; ++i) transmit_[i] = frames_.dmx[i];
  transmit_[0] = 0;
  transmitIndex_ = 0;
  portEXIT_CRITICAL(&lock_);
  // At MARK/idle, inverting the UART's idle TX signal produces a real BREAK
  // without switching baud rate or emitting a dummy channel/start code.
  portENTER_CRITICAL(&lock_);
  beginTransmit();
  portEXIT_CRITICAL(&lock_);
  for (;;) {
    portENTER_CRITICAL(&lock_);
    const bool complete = transmitIndex_ == transmitLength_ && uart_ll_is_tx_idle(hardware_);
    portEXIT_CRITICAL(&lock_);
    if (complete) break;
    if (static_cast<uint32_t>(esp_timer_get_time()) - started > 100000) {
      portENTER_CRITICAL(&lock_);
      uart_ll_disable_intr_mask(hardware_, UART_INTR_TXFIFO_EMPTY);
      uart_ll_txfifo_rst(hardware_);
      statistics_.transmitTimeouts++;
      portEXIT_CRITICAL(&lock_);
      return false;
    }
    vTaskDelay(1);
  }
  lastTransmitEndUs_ = static_cast<uint32_t>(esp_timer_get_time());
  portENTER_CRITICAL(&lock_);
  statistics_.transmittedFrames++;
  portEXIT_CRITICAL(&lock_);
  return true;
}

void Esp32S3UartPort::outputTask(void* argument) {
  auto* port = static_cast<Esp32S3UartPort*>(argument);
  while (!port->stopRequested_.load()) {
    if (port->pauseRequested_.load()) {
      port->outputPaused_.store(true);
      while (port->pauseRequested_.load() && !port->stopRequested_.load())
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      port->outputPaused_.store(false);
      continue;
    }
    const uint32_t started = static_cast<uint32_t>(esp_timer_get_time());
    port->transmitFrame();
    const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time()) - started;
    const uint32_t period = port->periodUs_.load();
    if (period > elapsed)
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((period - elapsed + 999) / 1000));
  }
  portENTER_CRITICAL(&port->lock_);
  port->task_ = nullptr;
  portEXIT_CRITICAL(&port->lock_);
  vTaskDelete(nullptr); // No access to port storage after releasing its lock.
}

bool Esp32S3UartPort::pauseOutput() {
  pauseRequested_.store(true);
  portENTER_CRITICAL(&lock_);
  if (task_) xTaskNotifyGive(task_);
  portEXIT_CRITICAL(&lock_);
  const uint32_t started = millis();
  while (!outputPaused_.load()) {
    if (millis() - started > 150) {
      pauseRequested_.store(false);
      portENTER_CRITICAL(&lock_);
      if (task_) xTaskNotifyGive(task_);
      portEXIT_CRITICAL(&lock_);
      return false;
    }
    vTaskDelay(1);
  }
  return true;
}

core::RdmCommandResult Esp32S3UartPort::transactRdm(uint16_t length, RdmTransaction kind) {
  using namespace core;
  discoveryCapture_.store(kind == RdmTransaction::Discovery);
  discoveryReceiver_.begin(0);
  rdmReceiver_.begin(0);
  // A failed pause must not expose an earlier successful discovery response.
  discoveryReceiver_.onError();
  if (!pauseOutput()) return {RdmCommandStatus::InvalidResponse, 0, 0, kRdmReceiveError};
  // One foreground caller, one persistent output task; no per-command allocation.
  while (static_cast<uint32_t>(esp_timer_get_time()) - lastTransmitEndUs_ < 176) {}
  portENTER_CRITICAL(&lock_);
  uart_ll_disable_intr_mask(hardware_, kRxInterrupts);
  uart_ll_disable_intr_mask(hardware_, UART_INTR_TX_DONE);
  uart_ll_clr_intsts_mask(hardware_, UART_INTR_TX_DONE);
  uart_ll_rxfifo_rst(hardware_);
  rdmCapture_.store(false);
  rdmTxDone_.store(false);
  rdmTransaction_ = kind;
  transmitLength_ = length;
  transmitIndex_ = 0;
  for (uint16_t i = 0; i < length; ++i) transmit_[i] = rdmRequest_[i];
  setDirection(true);
  rdmTxPending_.store(true);
  beginTransmit();
  uart_ll_ena_intr_mask(hardware_, UART_INTR_TX_DONE);
  portEXIT_CRITICAL(&lock_);
  const uint32_t started = static_cast<uint32_t>(esp_timer_get_time());
  // The foreground may be preempted: the ISR has already armed capture and
  // released DE at wire completion. A tick-based wait is safe for this caller.
  while (!rdmTxDone_.load()
      && static_cast<uint32_t>(esp_timer_get_time()) - started < 35000) {
    vTaskDelay(1);
  }
  const bool sent = rdmTxDone_.load();
  portENTER_CRITICAL(&lock_);
  const uint32_t requestEnd = sent ? rdmRequestEndUs_ : static_cast<uint32_t>(esp_timer_get_time());
  if (!sent) {
    rdmTxPending_.store(false);
    uart_ll_disable_intr_mask(hardware_, UART_INTR_TXFIFO_EMPTY | UART_INTR_TX_DONE | kRxInterrupts);
    uart_ll_txfifo_rst(hardware_);
    statistics_.transmitTimeouts++;
    rdmReceiver_.begin(requestEnd);
    discoveryReceiver_.begin(requestEnd);
    rdmReceiver_.onError();
    discoveryReceiver_.onError();
    rdmCapture_.store(false);
    if (kind != RdmTransaction::Broadcast) setDirection(false);
  }
  portEXIT_CRITICAL(&lock_);
  bool complete = false;
  while (!complete) {
    portENTER_CRITICAL(&lock_);
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
    complete = kind == RdmTransaction::Discovery ? discoveryReceiver_.poll(now)
        : kind == RdmTransaction::Broadcast ? now - requestEnd >= 176
        : rdmReceiver_.poll(now);
    portEXIT_CRITICAL(&lock_);
    if (!complete) delayMicroseconds(50);
  }
  portENTER_CRITICAL(&lock_);
  rdmCapture_.store(false);
  uart_ll_disable_intr_mask(hardware_, kRxInterrupts);
  uart_ll_rxfifo_rst(hardware_);
  uart_ll_clr_intsts_mask(hardware_, kRxInterrupts);
  portEXIT_CRITICAL(&lock_);
  const auto result = kind == RdmTransaction::Normal
      ? classifyRdmResponse(rdmReceiver_.data(), rdmReceiver_.length(),
                           rdmReceiver_.validate(rdmRequest_, length))
      : RdmCommandResult{sent ? RdmCommandStatus::Sent : RdmCommandStatus::InvalidResponse,
                         0, 0, static_cast<uint16_t>(sent ? 0 : kRdmReceiveError)};
  // Quiet-window closure already exceeds 176 us after a received last slot;
  // missing-response closure waits at least 3 ms after request EOP.
  setDirection(true);
  pauseRequested_.store(false);
  portENTER_CRITICAL(&lock_);
  if (task_) xTaskNotifyGive(task_);
  portEXIT_CRITICAL(&lock_);
  // Do not let a following command reuse the previous pause acknowledgement
  // while the worker is leaving its wait loop. Each command needs a fresh
  // frame-boundary acknowledgement, including back-to-back foreground calls.
  while (outputPaused_.load()) vTaskDelay(1);
  return result;
}

core::RdmCommandResult Esp32S3UartPort::transactRawRdm(const uint8_t* request, uint16_t length) {
  using namespace core;
  if (!isActive() || !rdmEnabled_ || receiving_ || !isRdmControllerRequest(request, length))
    return {RdmCommandStatus::InvalidArgument, 0, 0, 0};
  memcpy(rdmRequest_, request, length);
  const bool broadcast = Uid(request + RDM_IDX_DESTINATION_UID).isBroadcast();
  return transactRdm(length, broadcast ? RdmTransaction::Broadcast : RdmTransaction::Normal);
}

core::RdmCommandResult Esp32S3UartPort::getRdmParameter(const core::Uid& target,
    uint16_t pid, uint8_t* destination, uint16_t capacity, const uint8_t* requestData,
    uint16_t requestLength, uint16_t subDevice) {
  using namespace core;
  if (!isActive() || !rdmEnabled_ || receiving_ || target.isBroadcast()
      || (capacity && !destination) || (requestLength && !requestData)
      || requestLength > rdm::kMaximumParameterDataLength)
    return {RdmCommandStatus::InvalidArgument, 0, 0, 0};
  const uint16_t length = buildRdmRequest(rdmRequest_, uid_.data(), target.data(),
      transaction_++, RDM_GET_COMMAND, pid, requestData, requestLength, subDevice);
  auto result = transactRdm(length);
  if (result.status == RdmCommandStatus::Ack || result.status == RdmCommandStatus::Overflow)
    result.copiedLength = copyRdmParameterData(rdmReceiver_.data(), rdmReceiver_.length(),
                                             destination, capacity);
  return result;
}
core::RdmCommandResult Esp32S3UartPort::setRdmParameter(const core::Uid& target,
    uint16_t pid, const uint8_t* data, uint16_t length, uint16_t subDevice) {
  using namespace core;
  if (!isActive() || !rdmEnabled_ || receiving_ || target.isBroadcast()
      || length > rdm::kMaximumParameterDataLength || (length && !data))
    return {RdmCommandStatus::InvalidArgument, 0, 0, 0};
  const uint16_t wireLength = buildRdmRequest(rdmRequest_, uid_.data(), target.data(),
      transaction_++, RDM_SET_COMMAND, pid, data, length, subDevice);
  return transactRdm(wireLength);
}
core::RdmDiscoveryResult Esp32S3UartPort::discoverRdmBranch(const core::Uid& lower,
    const core::Uid& upper, core::Uid* discovered) {
  using namespace core;
  if (!isActive() || !rdmEnabled_ || receiving_ || upper < lower)
    return RdmDiscoveryResult::CollisionOrMalformed;
  uint8_t bounds[12];
  memcpy(bounds, lower.data(), 6);
  memcpy(bounds + 6, upper.data(), 6);
  const Uid broadcast(UINT64_C(0xFFFFFFFFFFFF));
  const uint16_t length = buildRdmRequest(rdmRequest_, uid_.data(), broadcast.data(),
      transaction_++, RDM_DISCOVERY_COMMAND, 0x0001, bounds, sizeof(bounds));
  if (transactRdm(length, RdmTransaction::Discovery).status != RdmCommandStatus::Sent)
    return RdmDiscoveryResult::CollisionOrMalformed;
  uint8_t bytes[6];
  const auto result = discoveryReceiver_.result(bytes);
  if (result != RdmDiscoveryResult::SingleDevice) return result;
  const Uid candidate(bytes);
  if (candidate < lower || upper < candidate || candidate.isBroadcast())
    return RdmDiscoveryResult::CollisionOrMalformed;
  if (discovered) *discovered = candidate;
  return result;
}
core::RdmCommandResult Esp32S3UartPort::setRdmDiscoveryMute(const core::Uid& target, bool mute) {
  using namespace core;
  if (!isActive() || !rdmEnabled_ || receiving_)
    return {RdmCommandStatus::InvalidArgument, 0, 0, 0};
  const uint16_t length = buildRdmRequest(rdmRequest_, uid_.data(), target.data(),
      transaction_++, RDM_DISCOVERY_COMMAND, mute ? 0x0002 : 0x0003, nullptr, 0);
  return transactRdm(length,
      target.isBroadcast() ? RdmTransaction::Broadcast : RdmTransaction::Normal);
}
uint16_t Esp32S3UartPort::copyRdmResponse(uint8_t* destination, uint16_t capacity) const {
  if (!destination) return 0;
  const uint16_t available = discoveryCapture_.load() ? discoveryReceiver_.length() : rdmReceiver_.length();
  const uint16_t length = available < capacity ? available : capacity;
  memcpy(destination, discoveryCapture_.load() ? discoveryReceiver_.data() : rdmReceiver_.data(), length);
  return length;
}

void Esp32S3UartPort::fillTxFifo() {
  const uint16_t remaining = transmitLength_ - transmitIndex_;
  const uint32_t free = uart_ll_get_txfifo_len(hardware_);
  const uint16_t count = remaining < free ? remaining : static_cast<uint16_t>(free);
  uart_ll_write_txfifo(hardware_, transmit_ + transmitIndex_, count);
  transmitIndex_ += count;
  if (transmitIndex_ < transmitLength_)
    uart_ll_ena_intr_mask(hardware_, UART_INTR_TXFIFO_EMPTY);
  else
    uart_ll_disable_intr_mask(hardware_, UART_INTR_TXFIFO_EMPTY);
}

bool Esp32S3UartPort::rxLevel() const {
  return rxPin_ < 32 ? ((GPIO.in >> rxPin_) & 1u)
                     : ((GPIO.in1.val >> (rxPin_ - 32)) & 1u);
}

void Esp32S3UartPort::rxEdgeInterrupt(void* argument) {
  auto* port = static_cast<Esp32S3UartPort*>(argument);
  if (!port->active_.load() || (!port->receiving_ && !port->rdmCapture_.load())) return;
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
  if (!port->rxLevel()) {
    if (port->rdmCapture_.load()) {
      portENTER_CRITICAL_ISR(&port->lock_);
      if (port->discoveryCapture_.load()) port->discoveryReceiver_.onActivity(now);
      else {
        port->rdmReceiver_.onLow(now);
        port->rdmReceiver_.onStartBit(now);
      }
      portEXIT_CRITICAL_ISR(&port->lock_);
    }
    port->fallingUs_ = now;
    port->lowSeen_ = true;
    return;
  }
  if (!port->lowSeen_) return;
  // A normal 250-kbaud character can remain low for at most 36 us. Capture
  // shorter-than-legal BREAK candidates too, so RDM diagnostics/validation
  // retain the physical duration instead of confusing them with data bytes.
  const uint32_t minimumLow = port->rdmCapture_.load() ? 44 : kRxMinimumBreakUs;
  const bool uartEvidence = port->breakPending_.load() || port->lowErrorPending_.load()
      || (uart_ll_get_intsts_mask(port->hardware_) & (UART_INTR_BRK_DET | UART_INTR_FRAM_ERR));
  const bool responseStarted = port->rdmCapture_.load()
      && (port->discoveryCapture_.load() || port->rdmReceiver_.hasBreak());
  const bool qualified = core::qualifyUartLowPulse(
      now - port->fallingUs_, minimumLow, responseStarted, uartEvidence);
  port->lowSeen_ = false;
  if (!qualified && !port->breakPending_.load() && !port->lowErrorPending_.load()) {
    if (port->rdmCapture_.load()) {
      portENTER_CRITICAL_ISR(&port->lock_);
      if (!port->discoveryCapture_.load()) port->rdmReceiver_.onHigh();
      portEXIT_CRITICAL_ISR(&port->lock_);
    }
    return;
  }
  portENTER_CRITICAL_ISR(&port->lock_);
  uart_ll_rxfifo_rst(port->hardware_); // Remove UART's artificial BREAK byte.
  uart_ll_clr_intsts_mask(port->hardware_, kRxInterrupts);
  port->breakPending_.store(false);
  port->lowErrorPending_.store(false);
  uint16_t completed = 0;
  if (port->rdmCapture_.load()) {
    if (port->discoveryCapture_.load()) port->discoveryReceiver_.onError(); // DUB has no BREAK.
    else {
      port->rdmReceiver_.onHigh();
      if (qualified) port->rdmReceiver_.onBreak(port->fallingUs_, now);
      else port->rdmReceiver_.onError();
    }
  } else {
    completed = qualified ? port->receiver_.onBreak() : 0;
    if (!qualified) { port->receiver_.onError(); port->statistics_.receiveErrors++; }
  }
  if (completed) port->statistics_.receivedFrames++;
  const auto callback = port->callback_;
  portEXIT_CRITICAL_ISR(&port->lock_);
  if (completed && callback) callback(completed);
}

void Esp32S3UartPort::uartInterrupt(void* argument) {
  auto* port = static_cast<Esp32S3UartPort*>(argument);
  if (!port->active_.load()) return;
  portENTER_CRITICAL_ISR(&port->lock_);
  const uint32_t status = uart_ll_get_intsts_mask(port->hardware_);
  if (status & UART_INTR_TXFIFO_EMPTY) port->fillTxFifo();
  if ((status & UART_INTR_TX_DONE) && port->rdmTxPending_.load()) port->finishRdmTransmit();
  if (status & UART_INTR_BRK_DET) {
    if (port->rdmCapture_.load() && port->discoveryCapture_.load()) port->discoveryReceiver_.onError();
    port->breakPending_.store(true);
    uart_ll_rxfifo_rst(port->hardware_);
  }
  if (status & UART_INTR_RXFIFO_OVF) {
    if (port->rdmCapture_.load()) {
      if (port->discoveryCapture_.load()) port->discoveryReceiver_.onError();
      else port->rdmReceiver_.onError();
    }
    port->receiver_.onError();
    port->statistics_.receiveErrors++;
    uart_ll_rxfifo_rst(port->hardware_);
  } else if (!(status & UART_INTR_BRK_DET)
             && (status & (UART_INTR_FRAM_ERR | UART_INTR_PARITY_ERR))) {
    if (port->rdmCapture_.load() && port->discoveryCapture_.load()) port->discoveryReceiver_.onError();
    // A BREAK can first raise a framing error while the line is still low.
    // Qualify that low period at its rising edge rather than discarding the
    // preceding good frame before knowing whether this is its closing BREAK.
    if (!port->rxLevel()) port->lowErrorPending_.store(true);
    else {
      if (port->rdmCapture_.load()) port->rdmReceiver_.onError();
      port->receiver_.onError(); port->statistics_.receiveErrors++;
    }
    uart_ll_rxfifo_rst(port->hardware_);
  } else if (status & (UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT)) {
    uint8_t bytes[128];
    const uint32_t count = uart_ll_get_rxfifo_len(port->hardware_);
    uart_ll_read_rxfifo(port->hardware_, bytes, count);
    if (!port->breakPending_.load() && !port->lowErrorPending_.load())
      for (uint32_t i = 0; i < count; ++i) {
        if (port->rdmCapture_.load()) {
          const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
          if (port->discoveryCapture_.load()) port->discoveryReceiver_.onByte(bytes[i], now);
          else port->rdmReceiver_.onByte(bytes[i], now);
        } else port->receiver_.onByte(bytes[i]);
      }
  }
  uart_ll_clr_intsts_mask(port->hardware_, status);
  portEXIT_CRITICAL_ISR(&port->lock_);
}

S3PortStatistics Esp32S3UartPort::statistics() {
  portENTER_CRITICAL(&lock_);
  const auto result = statistics_;
  portEXIT_CRITICAL(&lock_);
  return result;
}
bool Esp32S3UartPort::setRefreshRate(uint16_t fps) {
  if (fps < 1 || fps > 44) return false;
  periodUs_.store(1000000 / fps);
  return true;
}
uint16_t Esp32S3UartPort::numberOfSlots() {
  portENTER_CRITICAL(&lock_);
  const uint16_t slots = frames_.slots;
  portEXIT_CRITICAL(&lock_);
  return slots;
}
void Esp32S3UartPort::setMaxSlots(int slots) {
  portENTER_CRITICAL(&lock_);
  frames_.slots = core::clampOutputSlotCount(slots);
  portEXIT_CRITICAL(&lock_);
}
void Esp32S3UartPort::setSlot(int slot, uint8_t value) {
  if (slot < 0 || slot > kMaximumSlots) return;
  portENTER_CRITICAL(&lock_);
  frames_.dmx[slot] = value;
  portEXIT_CRITICAL(&lock_);
}
uint8_t Esp32S3UartPort::getSlot(int slot) {
  if (slot < 0 || slot > kMaximumSlots) return 0;
  portENTER_CRITICAL(&lock_);
  const uint8_t value = frames_.dmx[slot];
  portEXIT_CRITICAL(&lock_);
  return value;
}
bool Esp32S3UartPort::setFrame(const uint8_t* data, uint16_t slots) {
  if (!data || !core::isValidOutputSlotCount(slots)) return false;
  portENTER_CRITICAL(&lock_);
  core::replaceChannelData(frames_.dmx, data, slots);
  frames_.slots = slots;
  portEXIT_CRITICAL(&lock_);
  return true;
}
uint16_t Esp32S3UartPort::copyFrame(uint8_t* destination, uint16_t capacity) {
  if (!destination || !capacity) return 0;
  portENTER_CRITICAL(&lock_);
  const uint16_t copied = core::copyChannelData(frames_.dmx, frames_.slots, destination, capacity);
  portEXIT_CRITICAL(&lock_);
  return copied;
}
void Esp32S3UartPort::clearSlots() {
  portENTER_CRITICAL(&lock_);
  memset(frames_.dmx, 0, sizeof(frames_.dmx));
  portEXIT_CRITICAL(&lock_);
}
void Esp32S3UartPort::setDataReceivedCallback(void (*callback)(int)) {
  portENTER_CRITICAL(&lock_);
  callback_ = callback;
  portEXIT_CRITICAL(&lock_);
}

} } }
#endif
#endif
