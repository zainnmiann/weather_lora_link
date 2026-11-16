// Weather station sensors, shared by every station firmware (LoRa, cellular).
// The builder's WeatherStation.ino configuration: same pins and settings, same serial TELEM/EVENT lines
// (tools/telem_source.py parses them), same field scaling and range checks as tools/telem_source.py.
#pragma once
#include <Arduino.h>

#include "wx_frame.h"

namespace wxstation {

struct Options {
  // The AS3935 interrupt is hard-wired to GPIO17, which is also UART1 TX (RX1/TX1 on the carrier).
  // When TX1 is used (cellular modem), the lightning detector is polled instead of using its interrupt.
  bool lightningInterrupt = true;
};

// Sets up BME280, AS3935, wind/rain meter and soil sensor; prints the builder's start-up lines.
void begin(const Options& opt = Options());
// Call from loop() often: handles lightning events and samples every 3 s (prints a TELEM line).
void poll();
// Fills the reading part of `d` from the latest samples and starts a new wind window.
// Sets kFlagSensorFail when the samples are stale or the BME280 is missing.
void fillReading(wx::DataFrame& d);

uint64_t nowMs();  // milliseconds since boot, 64-bit (never wraps, unlike millis())

}  // namespace wxstation
