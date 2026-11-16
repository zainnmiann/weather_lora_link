#include "sx1278.h"

#include <math.h>

namespace {
constexpr uint8_t kFifo = 0x00, kOpMode = 0x01, kFrfMsb = 0x06, kPaConfig = 0x09, kOcp = 0x0B, kLna = 0x0C,
                  kFifoAddrPtr = 0x0D, kFifoTxBase = 0x0E, kFifoRxBase = 0x0F, kFifoRxCurrent = 0x10,
                  kIrqFlags = 0x12, kRxNbBytes = 0x13, kPktSnr = 0x19, kPktRssi = 0x1A, kHopChannel = 0x1C,
                  kModemConfig1 = 0x1D, kModemConfig2 = 0x1E, kPreambleMsb = 0x20, kPreambleLsb = 0x21,
                  kPayloadLength = 0x22, kModemConfig3 = 0x26, kDetectOptimize = 0x31, kDetectThreshold = 0x37,
                  kSyncWord = 0x39, kDioMapping1 = 0x40, kVersion = 0x42, kPaDac = 0x4D;
constexpr uint8_t kLongRange = 0x80, kSleep = 0x00, kStandby = 0x01, kTx = 0x03, kRxContinuous = 0x05;
constexpr uint8_t kIrqRxDone = 0x40, kIrqCrcError = 0x20, kIrqTxDone = 0x08;
const SPISettings kSpi(1000000, MSBFIRST, SPI_MODE0);
}  // namespace

uint8_t Sx1278::rd(uint8_t a) {
  spi_.beginTransaction(kSpi);
  digitalWrite(nss_, LOW);
  spi_.transfer(a & 0x7F);
  uint8_t v = spi_.transfer(0x00);
  digitalWrite(nss_, HIGH);
  spi_.endTransaction();
  return v;
}

void Sx1278::wr(uint8_t a, uint8_t v) {
  spi_.beginTransaction(kSpi);
  digitalWrite(nss_, LOW);
  spi_.transfer(a | 0x80);
  spi_.transfer(v);
  digitalWrite(nss_, HIGH);
  spi_.endTransaction();
}

// LowFrequencyModeOn (bit 3) must stay set below 525 MHz (its reset value); clearing it selects the HF
// settings and the receiver no longer detects packets.
uint8_t Sx1278::lf() const { return c_.freqMhz < 525.0f ? 0x08 : 0x00; }

void Sx1278::mode(uint8_t m) { wr(kOpMode, kLongRange | lf() | m); }

bool Sx1278::begin(const Config& c) {
  c_ = c;
  pinMode(nss_, OUTPUT);
  digitalWrite(nss_, HIGH);
  if (rd(kVersion) != 0x12) return false;
  wr(kOpMode, lf() | kSleep);  // LoRa mode can only be selected in sleep
  delay(5);
  wr(kOpMode, kLongRange | lf() | kSleep);
  delay(5);
  const uint64_t frf = uint64_t(double(c.freqMhz) * 524288.0 / 32.0 + 0.5);  // Fstep = 32 MHz / 2^19
  wr(kFrfMsb, frf >> 16);
  wr(kFrfMsb + 1, frf >> 8);
  wr(kFrfMsb + 2, frf);
  if (c.paBoost) {
    int8_t p = constrain(c.powerDbm, 2, 17);
    wr(kPaConfig, 0x80 | (p - 2));  // PA_BOOST: Pout = 2 + OutputPower dBm
  } else {
    int8_t p = constrain(c.powerDbm, 0, 14);
    wr(kPaConfig, 0x70 | p);        // RFO, MaxPower 7 (15 dBm): Pout = OutputPower dBm
  }
  wr(kPaDac, 0x84);               // normal PA (no +20 dBm mode)
  wr(kOcp, 0x2B);                 // over-current limit 100 mA
  wr(kLna, 0x23);                 // max gain, LNA boost
  wr(kModemConfig1, 0x72);        // BW 125 kHz, CR 4/5, explicit header
  wr(kModemConfig2, uint8_t(c.sf << 4) | 0x04);  // SF, CRC on
  wr(kModemConfig3, (c.sf >= 11 ? 0x08 : 0x00) | 0x04);  // low data rate optimise for SF11/12, AGC on
  wr(kPreambleMsb, c.preamble >> 8);
  wr(kPreambleLsb, c.preamble & 0xFF);
  wr(kDetectOptimize, 0xC3);
  wr(kDetectThreshold, 0x0A);
  wr(kSyncWord, c.syncWord);
  wr(kFifoTxBase, 0);
  wr(kFifoRxBase, 0);
  wr(kDioMapping1, 0x00);
  mode(kStandby);
  delay(2);
  return rd(kModemConfig2) == (uint8_t(c.sf << 4) | 0x04) && rd(kOpMode) == (kLongRange | lf() | kStandby);
}

bool Sx1278::healthy() { return rd(kVersion) == 0x12 && (rd(kOpMode) & kLongRange); }

bool Sx1278::transmit(const uint8_t* data, size_t len) {
  if (len == 0 || len > 255) return false;
  mode(kStandby);
  wr(kFifoAddrPtr, 0);
  for (size_t i = 0; i < len; i++) wr(kFifo, data[i]);
  wr(kPayloadLength, len);
  wr(kIrqFlags, 0xFF);
  mode(kTx);
  const uint32_t limit = airtimeMs(len) * 2 + 100, t0 = millis();
  while (!(rd(kIrqFlags) & kIrqTxDone)) {
    if (millis() - t0 > limit) {
      mode(kStandby);
      return false;
    }
    delay(1);
  }
  wr(kIrqFlags, 0xFF);
  return true;  // the chip returns to standby by itself
}

bool Sx1278::startReceive() {
  mode(kStandby);
  wr(kFifoAddrPtr, 0);
  wr(kIrqFlags, 0xFF);
  mode(kRxContinuous);
  for (int i = 0; i < 10; i++) {  // passes through FSRX (frequency synthesis) first
    if ((rd(kOpMode) & 0x07) == kRxContinuous) return true;
    delay(1);
  }
  return false;
}

bool Sx1278::poll(uint8_t* buf, size_t cap, size_t& len, int& rssi, float& snr) {
  const uint8_t irq = rd(kIrqFlags);
  if (!(irq & kIrqRxDone)) return false;
  wr(kIrqFlags, 0xFF);
  int32_t fei = (int32_t(rd(0x28) & 0x0F) << 16) | (int32_t(rd(0x29)) << 8) | rd(0x2A);
  if (fei & 0x80000) fei -= 0x100000;
  lastFeiHz = int32_t(fei * (16777216.0f / 32e6f) * (125.0f / 500.0f));  // SX1276 datasheet 4.1.5
  if ((irq & kIrqCrcError) || !(rd(kHopChannel) & 0x40)) {  // bad CRC, or a packet sent without one
    crcErrors++;
    return false;
  }
  len = rd(kRxNbBytes);
  if (len > cap) len = cap;
  wr(kFifoAddrPtr, rd(kFifoRxCurrent));
  for (size_t i = 0; i < len; i++) buf[i] = rd(kFifo);
  snr = int8_t(rd(kPktSnr)) / 4.0f;
  rssi = -164 + rd(kPktRssi);  // LF port (below 525 MHz)
  return true;
}

uint32_t Sx1278::airtimeMs(size_t len) const {
  const float tsym = float(1 << c_.sf) / 125.0f;  // ms at 125 kHz
  const int de = c_.sf >= 11 ? 1 : 0;
  const int n = 8 + max(int(ceilf((8.0f * len - 4 * c_.sf + 28 + 16) / (4.0f * (c_.sf - 2 * de)))) * 5, 0);
  return uint32_t(ceilf((c_.preamble + 4.25f) * tsym + n * tsym));
}
