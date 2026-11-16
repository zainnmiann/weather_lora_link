// SIM800L (2G) driver over a UART, using the modem's own HTTP stack (plain HTTP; the SIM800L cannot do
// modern TLS, so the payload is HMAC-signed instead). Every call has a deadline and returns; nothing
// waits forever. Meant to run in its own task: `tick` is called while waiting (feed the watchdog).
#pragma once
#include <Arduino.h>

class Sim800 {
 public:
  using Tick = void (*)();

  Sim800(HardwareSerial& serial, int rxPin, int txPin) : serial_(serial), rxPin_(rxPin), txPin_(txPin) {}
  void setLog(Print* log) { log_ = log; }
  void setTick(Tick tick) { tick_ = tick; }

  // Finds the modem's baud rate (autobaud at 9600 first), turns echo off. False if it never answers.
  bool probe();
  uint32_t baud() const { return baud_; }
  bool simReady();                 // AT+CPIN? -> READY
  int registration();              // AT+CREG? stat: 1 home, 5 roaming, 2 searching, 3 denied, 0 idle; -1 no answer
  int signal();                    // AT+CSQ rssi: 0..31 (higher is better), 99 unknown; -1 no answer
  bool bearerIsOpen();
  bool bearerOpen(const char* apn);  // GPRS context 1 (takes up to ~85 s)
  void bearerClose();
  // HTTP POST. Returns the HTTP status (200...), 6xx for SIM800 network errors, or -1 on a modem/AT
  // failure. Up to cap-1 bytes of the reply body are copied to resp (NUL-terminated).
  int post(const char* url, const char* contentType, const char* body, size_t len, char* resp, size_t cap);
  bool restart();                  // AT+CFUN=1,1 (full functionality, reboot), then probe again
  bool powerLost() const { return powerLost_; }  // the modem announced a power-down (brown-out)
  void clearPowerLost() { powerLost_ = false; }

 private:
  bool cmd(const char* c, const char* ok, uint32_t timeoutMs, const char* want = nullptr, char* got = nullptr,
           size_t gotCap = 0);
  bool readLine(uint32_t deadline);
  bool waitFor(const char* prefix, uint32_t timeoutMs, char* got = nullptr, size_t gotCap = 0);
  void drain();
  void note(const char* line);

  HardwareSerial& serial_;
  int rxPin_, txPin_;
  uint32_t baud_ = 0;
  Print* log_ = nullptr;
  Tick tick_ = nullptr;
  char line_[160];
  size_t len_ = 0;
  bool powerLost_ = false;
};
