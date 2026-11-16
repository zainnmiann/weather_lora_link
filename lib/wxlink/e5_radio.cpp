#include "e5_radio.h"

#include "wx_config.h"
#include "wx_frame.h"

E5Radio::E5Radio(HardwareSerial& serial, int rxPin, int txPin, int rstPin)
    : serial_(serial), rxPin_(rxPin), txPin_(txPin), rstPin_(rstPin) {}

void E5Radio::begin(uint32_t baud) {
  serial_.begin(baud, SERIAL_8N1, rxPin_, txPin_);
  if (rstPin_ >= 0) pinMode(rstPin_, INPUT);  // nRST has an internal pull-up; only ever pull it low
}

bool E5Radio::hardReset() {
  if (rstPin_ < 0) return false;
  pinMode(rstPin_, OUTPUT);
  digitalWrite(rstPin_, LOW);
  delay(20);
  pinMode(rstPin_, INPUT);
  delay(500);  // AT firmware boot banner
  drainInput();
  return true;
}

bool E5Radio::wake() {
  for (int attempt = 0; attempt < 3; attempt++) {
    // Per the AT spec, any UART byte wakes the module from AT+LOWPOWER; 0xFF is ignored by the parser.
    for (int i = 0; i < 4; i++) serial_.write(uint8_t(0xFF));
    serial_.flush();
    delay(10);
    if (command("AT", "+AT: OK", 500)) return true;
  }
  if (hardReset() && command("AT", "+AT: OK", 500)) return true;
  if (log_) log_->println("[e5] no response to AT");
  return false;
}

bool E5Radio::configure() {
  if (!wake()) return false;
  if (!command("AT+MODE=TEST", "+MODE: TEST", 1000)) return false;
  char cmd[96];
  snprintf(cmd, sizeof(cmd), "AT+TEST=RFCFG,%s,%s,%d,%d,%d,%d,ON,OFF,OFF", WX_FREQ_MHZ, WX_SF, WX_BW_KHZ,
           WX_TX_PREAMBLE, WX_RX_PREAMBLE, WX_TX_POWER_DBM);
  return command(cmd, "+TEST: RFCFG", 1000);
}

bool E5Radio::transmit(const uint8_t* data, size_t len, uint32_t timeoutMs) {
  if (len > kMaxTxLen) return false;
  char cmd[2 * kMaxTxLen + 32];
  int n = snprintf(cmd, sizeof(cmd), "AT+TEST=TXLRPKT,\"");
  wx::toHex(data, len, cmd + n);
  strcat(cmd, "\"");
  return command(cmd, "+TEST: TX DONE", timeoutMs);
}

bool E5Radio::startReceive() {
  pendingMeta_ = e5::RxMeta();
  return command("AT+TEST=RXLRPKT", "+TEST: RXLRPKT", 1000);
}

bool E5Radio::sleep() { return command("AT+LOWPOWER", "+LOWPOWER: SLEEP", 1000); }

bool E5Radio::poll(uint8_t* buf, size_t cap, size_t& len, e5::RxMeta& meta) {
  while (serial_.available()) {
    if (!feed(char(serial_.read()))) continue;
    e5::RxMeta m;
    if (e5::parseRxMeta(line_, m)) {
      pendingMeta_ = m;
      continue;
    }
    int n = e5::parseRxHex(line_, buf, cap);
    if (n >= 0) {
      len = size_t(n);
      meta = pendingMeta_;
      pendingMeta_ = e5::RxMeta();
      return true;
    }
    if (log_ && line_[0] != '\0') {
      log_->print("[e5] ");
      log_->println(line_);  // includes RX lines too large for buf (foreign traffic)
    }
  }
  return false;
}

bool E5Radio::command(const char* cmd, const char* expectPrefix, uint32_t timeoutMs) {
  drainInput();
  serial_.print(cmd);
  serial_.print("\r\n");
  uint32_t deadline = millis() + timeoutMs;
  size_t expectLen = strlen(expectPrefix);
  while (readLineUntil(deadline)) {
    if (strncmp(line_, expectPrefix, expectLen) == 0) return true;
    if (e5::isError(line_)) {
      if (log_) {
        log_->printf("[e5] %s -> %s\n", cmd, line_);
      }
      return false;
    }
  }
  if (log_) log_->printf("[e5] timeout waiting for '%s' after %s\n", expectPrefix, cmd);
  return false;
}

bool E5Radio::readLineUntil(uint32_t deadline) {
  while (int32_t(deadline - millis()) > 0) {
    while (serial_.available()) {
      if (feed(char(serial_.read()))) return true;
    }
    delay(1);
  }
  return false;
}

bool E5Radio::feed(char c) {
  if (c == '\r') return false;
  if (c == '\n') {
    bool complete = !overflow_;
    line_[lineLen_] = '\0';
    lineLen_ = 0;
    overflow_ = false;
    return complete;
  }
  if (overflow_) return false;
  if (lineLen_ >= sizeof(line_) - 1) {
    overflow_ = true;  // drop the rest of an over-long line
    lineLen_ = 0;
    return false;
  }
  line_[lineLen_++] = c;
  return false;
}

void E5Radio::drainInput() {
  while (serial_.available()) serial_.read();
  lineLen_ = 0;
  overflow_ = false;
}
