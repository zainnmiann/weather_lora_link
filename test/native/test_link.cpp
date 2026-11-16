// Host-side tests for the frame codec and the AT output parsers (no ESP32 needed).
// Run with tools/run_tests.sh. Prints test vectors that tools/test_parity.py checks against
// the Python implementation, so firmware and bench tools can never disagree on the format.
#include <stdio.h>
#include <string.h>

#include "e5_parse.h"
#include "wx_frame.h"

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                  \
    }                                                              \
  } while (0)

static const uint8_t kKey[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

static wx::DataFrame sampleData() {
  wx::DataFrame d;
  d.station_id = 1;
  d.boot_id = 0xBEEF;
  d.seq = 42;
  d.r.temp_c_x100 = -1234;  // -12.34 C
  d.r.rh_x100 = 8765;
  d.r.press_hpa_x10 = 10132;
  d.r.wind_ms_x100 = 350;
  d.r.gust_ms_x100 = 720;
  d.r.wind_dir_deg = 270;
  d.r.rain_total_mm_x10 = 56;
  d.r.batt_mv = 3912;
  d.r.soil_pct_x10 = 423;
  d.r.lightning_total = 17;
  d.r.lightning_km = 12;
  d.r.flags = wx::kFlagColdBoot;
  return d;
}

static void testDataRoundTrip() {
  wx::DataFrame d = sampleData();
  uint8_t buf[wx::kDataLen];
  CHECK(wx::encodeData(d, kKey, sizeof(kKey), buf, sizeof(buf)) == wx::kDataLen);
  wx::Frame f;
  CHECK(wx::decode(buf, sizeof(buf), kKey, sizeof(kKey), f) == wx::kOk);
  CHECK(f.type == wx::kTypeData);
  CHECK(f.data.station_id == 1 && f.data.boot_id == 0xBEEF && f.data.seq == 42);
  CHECK(f.data.r.temp_c_x100 == -1234);
  CHECK(f.data.r.rh_x100 == 8765 && f.data.r.press_hpa_x10 == 10132);
  CHECK(f.data.r.wind_ms_x100 == 350 && f.data.r.gust_ms_x100 == 720 && f.data.r.wind_dir_deg == 270);
  CHECK(f.data.r.rain_total_mm_x10 == 56 && f.data.r.batt_mv == 3912);
  CHECK(f.data.r.soil_pct_x10 == 423 && f.data.r.lightning_total == 17 && f.data.r.lightning_km == 12);
  CHECK(f.data.r.flags == wx::kFlagColdBoot);
  CHECK(wx::encodeData(d, kKey, sizeof(kKey), buf, wx::kDataLen - 1) == 0);
}

static void testSentinels() {
  wx::DataFrame d;  // all fields "not measured"
  uint8_t buf[wx::kDataLen];
  wx::encodeData(d, kKey, sizeof(kKey), buf, sizeof(buf));
  wx::Frame f;
  CHECK(wx::decode(buf, sizeof(buf), kKey, sizeof(kKey), f) == wx::kOk);
  CHECK(f.data.r.temp_c_x100 == wx::kNoI16);
  CHECK(f.data.r.rh_x100 == wx::kNoU16 && f.data.r.batt_mv == wx::kNoU16);
  CHECK(f.data.r.soil_pct_x10 == wx::kNoU16 && f.data.r.lightning_km == wx::kNoU8);
}

static void testAckRoundTrip() {
  wx::AckFrame a;
  a.station_id = 7;
  a.boot_id = 0x1234;
  a.seq = 65535;
  a.status = 1;
  uint8_t buf[wx::kAckLen];
  CHECK(wx::encodeAck(a, kKey, sizeof(kKey), buf, sizeof(buf)) == wx::kAckLen);
  wx::Frame f;
  CHECK(wx::decode(buf, sizeof(buf), kKey, sizeof(kKey), f) == wx::kOk);
  CHECK(f.type == wx::kTypeAck && f.ack.station_id == 7 && f.ack.boot_id == 0x1234);
  CHECK(f.ack.seq == 65535 && f.ack.status == 1);
}

static void testRejections() {
  wx::DataFrame d = sampleData();
  uint8_t buf[wx::kDataLen];
  wx::encodeData(d, kKey, sizeof(kKey), buf, sizeof(buf));
  wx::Frame f;

  uint8_t tampered[wx::kDataLen];
  for (size_t i = 0; i < wx::kDataLen; i++) {  // flipping any single byte must be detected
    memcpy(tampered, buf, sizeof(buf));
    tampered[i] ^= 0x01;
    wx::DecodeResult r = wx::decode(tampered, sizeof(tampered), kKey, sizeof(kKey), f);
    CHECK(r != wx::kOk);
  }

  uint8_t otherKey[sizeof(kKey)];
  memcpy(otherKey, kKey, sizeof(kKey));
  otherKey[0] ^= 0x80;
  CHECK(wx::decode(buf, sizeof(buf), otherKey, sizeof(otherKey), f) == wx::kBadMac);
  CHECK(wx::decode(buf, sizeof(buf) - 1, kKey, sizeof(kKey), f) == wx::kBadLength);
  CHECK(wx::decode(buf, 1, kKey, sizeof(kKey), f) == wx::kBadLength);

  memcpy(tampered, buf, sizeof(buf));
  tampered[0] = 9;
  CHECK(wx::decode(tampered, sizeof(tampered), kKey, sizeof(kKey), f) == wx::kBadVersion);
  tampered[0] = wx::kVersion;
  tampered[1] = 5;
  CHECK(wx::decode(tampered, sizeof(tampered), kKey, sizeof(kKey), f) == wx::kBadType);
}

static void testHex() {
  uint8_t in[] = {0x00, 0x7F, 0xA5, 0xFF};
  char hex[9];
  wx::toHex(in, sizeof(in), hex);
  CHECK(strcmp(hex, "007FA5FF") == 0);
  uint8_t out[4];
  CHECK(wx::fromHex("007fa5ff", 8, out, sizeof(out)) == 4 && memcmp(in, out, 4) == 0);
  CHECK(wx::fromHex("007", 3, out, sizeof(out)) == -1);
  CHECK(wx::fromHex("0G", 2, out, sizeof(out)) == -1);
  CHECK(wx::fromHex("0011223344", 10, out, sizeof(out)) == -1);
}

static void testParsers() {
  e5::RxMeta m;
  CHECK(e5::parseRxMeta("+TEST: LEN:28, RSSI:-87, SNR:7", m));
  CHECK(m.valid && m.len == 28 && m.rssi == -87 && m.snr == 7.0f);
  CHECK(e5::parseRxMeta("+TEST: LEN:12, RSSI:-121, SNR:-12.5", m));
  CHECK(m.rssi == -121 && m.snr == -12.5f);
  CHECK(!e5::parseRxMeta("+TEST: RXLRPKT", m));
  CHECK(!e5::parseRxMeta("+TEST: TX DONE", m));

  uint8_t buf[8];
  CHECK(e5::parseRxHex("+TEST: RX \"0A0B0C\"", buf, sizeof(buf)) == 3 && buf[2] == 0x0C);
  CHECK(e5::parseRxHex("+TEST: RXLRPKT", buf, sizeof(buf)) == -1);
  CHECK(e5::parseRxHex("+TEST: RX \"0A0B0", buf, sizeof(buf)) == -1);              // no closing quote
  CHECK(e5::parseRxHex("+TEST: RX \"000102030405060708\"", buf, sizeof(buf)) == -1);  // > cap
  CHECK(e5::parseRxHex("+TEST: RX \"\"", buf, sizeof(buf)) == 0);

  CHECK(e5::isError("+TEST: ERROR(-1)"));
  CHECK(e5::isError("+AT: ERROR(-10)"));
  CHECK(!e5::isError("+TEST: TX DONE"));
}

static void printVectors() {
  uint8_t buf[wx::kDataLen];
  char hex[2 * wx::kDataLen + 1];
  wx::encodeData(sampleData(), kKey, sizeof(kKey), buf, sizeof(buf));
  wx::toHex(buf, wx::kDataLen, hex);
  printf("VECTOR_DATA %s\n", hex);

  wx::AckFrame a;
  a.station_id = 1;
  a.boot_id = 0xBEEF;
  a.seq = 42;
  wx::encodeAck(a, kKey, sizeof(kKey), buf, sizeof(buf));
  wx::toHex(buf, wx::kAckLen, hex);
  printf("VECTOR_ACK %s\n", hex);
}

int main() {
  testDataRoundTrip();
  testSentinels();
  testAckRoundTrip();
  testRejections();
  testHex();
  testParsers();
  printVectors();
  if (failures) {
    printf("%d check(s) FAILED\n", failures);
    return 1;
  }
  printf("all native checks passed\n");
  return 0;
}
