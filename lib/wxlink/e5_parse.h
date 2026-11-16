// Pure parsers for the Seeed Wio-E5 / LoRa-E5 AT firmware's TEST-mode receive output:
//   +TEST: LEN:28, RSSI:-87, SNR:7
//   +TEST: RX "0101..."
// No Arduino dependency, so they are unit-tested on the host (test/native).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace e5 {

struct RxMeta {
  int len = -1;
  int rssi = 0;    // dBm
  float snr = 0;   // dB
  bool valid = false;
};

// True if line is a "+TEST: LEN:.., RSSI:.., SNR:.." line.
bool parseRxMeta(const char* line, RxMeta& out);

// If line is a '+TEST: RX "<hex>"' line, decodes it into out and returns the byte count.
// Returns -1 if it is not an RX line or the hex is malformed / larger than cap.
int parseRxHex(const char* line, uint8_t* out, size_t cap);

// True if the line reports an error, e.g. "+TEST: ERROR(-1)" or "+AT: ERROR(-1)".
bool isError(const char* line);

}  // namespace e5
