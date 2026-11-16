// Minimal driver for a Seeed Wio-E5 / LoRa-E5 running the factory AT firmware, used in
// TEST (raw LoRa point-to-point) mode over a UART.
#pragma once
#include <Arduino.h>

#include "e5_parse.h"

class E5Radio {
 public:
  static constexpr size_t kMaxTxLen = 128;  // largest packet transmit() accepts (hop frames are <= 75 B)

  E5Radio(HardwareSerial& serial, int rxPin, int txPin, int rstPin = -1);

  void begin(uint32_t baud);
  void setLog(Stream* log) { log_ = log; }

  // Wakes the module, enters TEST mode and applies the RF settings from wx_config.h.
  bool configure();
  // Blocks until "+TEST: TX DONE" (or timeout / error).
  bool transmit(const uint8_t* data, size_t len, uint32_t timeoutMs = 4000);
  // Puts the radio into continuous receive; packets are then collected with poll().
  bool startReceive();
  // Non-blocking. Returns true when a complete packet has been received.
  bool poll(uint8_t* buf, size_t cap, size_t& len, e5::RxMeta& meta);
  // Deep low-power mode (AT+LOWPOWER). Any UART traffic wakes it; configure() handles that.
  bool sleep();
  // Pulses nRST if wired. Returns false if no reset pin is configured.
  bool hardReset();

 private:
  bool wake();
  bool command(const char* cmd, const char* expectPrefix, uint32_t timeoutMs);
  bool readLineUntil(uint32_t deadline);
  bool feed(char c);  // true when line_ holds a complete line
  void drainInput();

  HardwareSerial& serial_;
  int rxPin_, txPin_, rstPin_;
  Stream* log_ = nullptr;
  char line_[600];  // fits a 255-byte packet as hex plus the "+TEST: RX" framing
  size_t lineLen_ = 0;
  bool overflow_ = false;
  e5::RxMeta pendingMeta_;
};
