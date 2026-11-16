#include "sim800.h"

void Sim800::note(const char* line) {
  // Unsolicited messages that matter: the modem shuts itself down on low supply voltage.
  if (strstr(line, "POWER DOWN")) powerLost_ = true;
  if (log_ && (strstr(line, "POWER") || strstr(line, "UNDER-VOLTAGE") || strstr(line, "OVER-VOLTAGE")))
    log_->printf("[cell] modem says: %s\n", line);
}

void Sim800::drain() {
  while (serial_.available()) serial_.read();
  len_ = 0;
}

bool Sim800::readLine(uint32_t deadline) {
  while (int32_t(deadline - millis()) > 0) {
    while (serial_.available()) {
      char c = char(serial_.read());
      if (c == '\r') continue;
      if (c == '\n') {
        if (len_ == 0) continue;  // blank line
        line_[len_] = '\0';
        len_ = 0;
        note(line_);
        return true;
      }
      if (len_ < sizeof(line_) - 1) line_[len_++] = c;  // over-long lines are truncated
    }
    if (tick_) tick_();
    delay(2);
  }
  return false;
}

bool Sim800::cmd(const char* c, const char* ok, uint32_t timeoutMs, const char* want, char* got, size_t gotCap) {
  drain();
  serial_.print(c);
  serial_.print("\r\n");
  const uint32_t deadline = millis() + timeoutMs;
  bool matched = (want == nullptr);
  while (readLine(deadline)) {
    if (want && strncmp(line_, want, strlen(want)) == 0) {
      if (got) strlcpy(got, line_, gotCap);
      matched = true;
      if (!ok) return true;
      continue;
    }
    if (ok && strncmp(line_, ok, strlen(ok)) == 0) return matched;
    if (strncmp(line_, "ERROR", 5) == 0 || strncmp(line_, "+CME ERROR", 10) == 0) {
      if (log_) log_->printf("[cell] %s -> %s\n", c, line_);
      return false;
    }
  }
  if (log_) log_->printf("[cell] %s -> timeout\n", c);
  return false;
}

bool Sim800::waitFor(const char* prefix, uint32_t timeoutMs, char* got, size_t gotCap) {
  const uint32_t deadline = millis() + timeoutMs;
  while (readLine(deadline)) {
    if (strncmp(line_, prefix, strlen(prefix)) == 0) {
      if (got) strlcpy(got, line_, gotCap);
      return true;
    }
  }
  return false;
}

bool Sim800::probe() {
  static const uint32_t bauds[] = {9600, 115200, 57600, 38400, 19200};
  Print* log = log_;
  log_ = nullptr;  // 15 "AT -> timeout" lines per round say nothing the caller does not report
  struct Restore {
    Print*& dst;
    Print* v;
    ~Restore() { dst = v; }
  } restore{log_, log};
  for (uint32_t b : bauds) {
    serial_.end();
    serial_.begin(b, SERIAL_8N1, rxPin_, txPin_);
    delay(50);
    for (int i = 0; i < 3; i++) {
      if (cmd("AT", "OK", 600)) {
        baud_ = b;
        cmd("ATE0", "OK", 1000);       // no echo: replies are easier to parse
        cmd("AT+CMEE=2", "OK", 1000);  // readable error texts
        if (log_) log_->printf("[cell] modem answers at %lu baud\n", (unsigned long)b);
        return true;
      }
    }
  }
  baud_ = 0;
  return false;
}

bool Sim800::simReady() {
  char got[40];
  return cmd("AT+CPIN?", "OK", 5000, "+CPIN:", got, sizeof(got)) && strstr(got, "READY");
}

int Sim800::registration() {
  char got[40];
  if (!cmd("AT+CREG?", "OK", 2000, "+CREG:", got, sizeof(got))) return -1;
  const char* comma = strchr(got, ',');
  return comma ? atoi(comma + 1) : -1;
}

int Sim800::signal() {
  char got[40];
  if (!cmd("AT+CSQ", "OK", 2000, "+CSQ:", got, sizeof(got))) return -1;
  return atoi(got + 5);
}

bool Sim800::bearerIsOpen() {
  char got[64];
  // +SAPBR: 1,<status>,"ip"   status 1 = connected
  return cmd("AT+SAPBR=2,1", "OK", 3000, "+SAPBR:", got, sizeof(got)) && strncmp(got, "+SAPBR: 1,1,", 12) == 0;
}

bool Sim800::bearerOpen(const char* apn) {
  if (bearerIsOpen()) return true;
  char c[96];
  if (!cmd("AT+SAPBR=3,1,\"Contype\",\"GPRS\"", "OK", 3000)) return false;
  snprintf(c, sizeof(c), "AT+SAPBR=3,1,\"APN\",\"%s\"", apn);
  if (!cmd(c, "OK", 3000)) return false;
  cmd("AT+CGATT=1", "OK", 10000);          // attach to GPRS (may already be attached)
  if (!cmd("AT+SAPBR=1,1", "OK", 85000)) {  // the SIM800 manual allows up to 85 s
    return bearerIsOpen();                  // "ERROR" also comes back when it was already open
  }
  return bearerIsOpen();
}

void Sim800::bearerClose() { cmd("AT+SAPBR=0,1", "OK", 65000); }

int Sim800::post(const char* url, const char* contentType, const char* body, size_t len, char* resp, size_t cap) {
  if (resp && cap) resp[0] = '\0';
  char c[200];
  cmd("AT+HTTPTERM", "OK", 2000);  // a previous session may still be open; an error here is normal
  if (!cmd("AT+HTTPINIT", "OK", 5000)) return -1;
  int status = -1;
  do {
    if (!cmd("AT+HTTPPARA=\"CID\",1", "OK", 2000)) break;
    snprintf(c, sizeof(c), "AT+HTTPPARA=\"URL\",\"%s\"", url);
    if (!cmd(c, "OK", 2000)) break;
    snprintf(c, sizeof(c), "AT+HTTPPARA=\"CONTENT\",\"%s\"", contentType);
    if (!cmd(c, "OK", 2000)) break;
    snprintf(c, sizeof(c), "AT+HTTPDATA=%u,20000", unsigned(len));
    if (!cmd(c, "DOWNLOAD", 5000)) break;
    serial_.write(reinterpret_cast<const uint8_t*>(body), len);
    if (!waitFor("OK", 25000)) break;
    if (!cmd("AT+HTTPACTION=1", "OK", 5000)) break;
    char got[64];
    if (!waitFor("+HTTPACTION:", 90000, got, sizeof(got))) break;  // +HTTPACTION: 1,<status>,<length>
    const char* p = strchr(got, ',');
    if (!p) break;
    status = atoi(p + 1);
    const char* q = strchr(p + 1, ',');
    const int bodyLen = q ? atoi(q + 1) : 0;
    if (resp && cap > 1 && bodyLen > 0) {
      drain();
      serial_.print("AT+HTTPREAD\r\n");
      if (waitFor("+HTTPREAD:", 5000)) {  // then exactly n raw bytes, then OK
        const int n = min(bodyLen, int(cap) - 1);
        int got_n = 0;
        const uint32_t deadline = millis() + 5000;
        while (got_n < bodyLen && int32_t(deadline - millis()) > 0) {
          if (serial_.available()) {
            char ch = char(serial_.read());
            if (got_n < n) resp[got_n] = ch;
            got_n++;
          } else {
            if (tick_) tick_();
            delay(1);
          }
        }
        resp[min(got_n, n)] = '\0';
        waitFor("OK", 2000);
      }
    }
  } while (false);
  cmd("AT+HTTPTERM", "OK", 2000);
  return status;
}

bool Sim800::restart() {
  if (log_) log_->println("[cell] restarting the modem (AT+CFUN=1,1)");
  cmd("AT+CFUN=1,1", "OK", 10000);
  const uint32_t until = millis() + 15000;  // it reboots and needs a few seconds before answering
  while (int32_t(until - millis()) > 0) {
    if (tick_) tick_();
    delay(10);
  }
  powerLost_ = false;
  return probe();
}
