// Minimal SX1276/77/78 (e.g. Ra-01, XL1278-SMT) LoRa driver that needs only SPI + NSS: no DIO0, no RESET.
// Everything is polled through the IRQ flags register, because neither of our boards wires DIO0.
// The SPI bus may be shared (the weather station's AS3935 is on the same bus): every access is a
// transaction and NSS is ours alone.
#pragma once
#include <Arduino.h>
#include <SPI.h>

class Sx1278 {
 public:
  struct Config {
    float freqMhz = 434.0f;
    uint8_t sf = 9;           // 7..12, BW 125 kHz, CR 4/5, explicit header, CRC on
    int8_t powerDbm = 10;     // 2..17 on PA_BOOST, 0..14 on RFO
    bool paBoost = true;      // which output pin the module's antenna is on (Ra-01: PA_BOOST)
    uint8_t syncWord = 0x12;  // private network
    uint16_t preamble = 12;
  };

  Sx1278(SPIClass& spi, int nss) : spi_(spi), nss_(nss) {}

  // Checks the chip (version 0x12) and applies the configuration. Leaves the radio in standby.
  bool begin(const Config& c);
  // True while the chip answers with its version and is still in LoRa mode (detects brown-outs).
  bool healthy();
  uint8_t version() { return rd(0x42); }
  uint8_t reg(uint8_t a) { return rd(a); }
  int rssiNow() { return -164 + rd(0x1B); }  // current channel level in RX, dBm

  // Blocking, polled TxDone (an SF9 frame of 44 bytes takes about 0.25 s). Returns false on timeout.
  bool transmit(const uint8_t* data, size_t len);
  bool startReceive();  // continuous receive
  // Non-blocking. Returns true for a packet that passed the radio CRC; rssi in dBm, snr in dB.
  bool poll(uint8_t* buf, size_t cap, size_t& len, int& rssi, float& snr);

  // Time on air in ms for a payload of len bytes with the current settings (Semtech AN1200.13).
  uint32_t airtimeMs(size_t len) const;

  uint32_t crcErrors = 0;
  int32_t lastFeiHz = 0;  // frequency error of the last packet (or CRC-failed packet): sender minus us

 private:
  uint8_t rd(uint8_t a);
  void wr(uint8_t a, uint8_t v);
  void mode(uint8_t m);
  uint8_t lf() const;

  SPIClass& spi_;
  int nss_;
  Config c_;
};
