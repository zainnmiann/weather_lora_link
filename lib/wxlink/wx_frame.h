// Over-the-air frame format. Explicit little-endian serialization (no struct casting), so the
// layout is identical on ESP32, the host-side tests and tools/wxframe.py.
//
// DATA (33 bytes, version 2)
//   0 ver   1 type=0   2 station_id   3-4 boot_id   5-6 seq
//   7-8 temp_c_x100 (int16)   9-10 rh_x100   11-12 press_hpa_x10   13-14 wind_ms_x100
//   15-16 gust_ms_x100   17-18 wind_dir_deg   19-20 rain_total_mm_x10   21-22 batt_mv
//   23-24 soil_pct_x10   25-26 lightning_total   27 lightning_km   28 flags   29-32 mac
// ACK (12 bytes)
//   0 ver   1 type=1   2 station_id   3-4 boot_id   5-6 seq   7 status   8-11 mac
//
// mac = first 4 bytes of HMAC-SHA256(key, all preceding bytes). It authenticates the sender;
// it is not encryption (readings are public weather data).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace wx {

constexpr uint8_t kVersion = 2;
constexpr size_t kMacLen = 4;
constexpr size_t kDataLen = 33;
constexpr size_t kAckLen = 12;
constexpr size_t kMaxFrameLen = kDataLen;

enum FrameType : uint8_t { kTypeData = 0, kTypeAck = 1 };

// Sentinels for "not measured". The gateway omits these fields from its JSON.
constexpr int16_t kNoI16 = 0x7FFF;
constexpr uint16_t kNoU16 = 0xFFFF;
constexpr uint8_t kNoU8 = 0xFF;

// Reading.flags
constexpr uint8_t kFlagSensorFail = 1 << 0;  // sensor could not be read this cycle
constexpr uint8_t kFlagFakeData = 1 << 1;    // built with WX_FAKE_SENSOR, not real weather
constexpr uint8_t kFlagLowBattery = 1 << 2;
constexpr uint8_t kFlagColdBoot = 1 << 3;    // first frame after power-on (not a deep-sleep wake)

struct Reading {
  int16_t temp_c_x100 = kNoI16;
  uint16_t rh_x100 = kNoU16;
  uint16_t press_hpa_x10 = kNoU16;
  uint16_t wind_ms_x100 = kNoU16;
  uint16_t gust_ms_x100 = kNoU16;
  uint16_t wind_dir_deg = kNoU16;
  uint16_t rain_total_mm_x10 = kNoU16;  // cumulative since boot_id changed; consumer takes deltas
  uint16_t batt_mv = kNoU16;
  uint16_t soil_pct_x10 = kNoU16;
  uint16_t lightning_total = kNoU16;  // strikes counted since boot_id changed (like rain)
  uint8_t lightning_km = kNoU8;       // distance estimate of the latest storm front
  uint8_t flags = 0;
};

struct DataFrame {
  uint8_t station_id = 0;
  uint16_t boot_id = 0;  // random per power-on; resets the gateway's duplicate tracking
  uint16_t seq = 0;
  Reading r;
};

struct AckFrame {
  uint8_t station_id = 0;
  uint16_t boot_id = 0;
  uint16_t seq = 0;
  uint8_t status = 0;  // 0 = stored; 1 = duplicate (already stored)
};

struct Frame {
  FrameType type;
  DataFrame data;
  AckFrame ack;
};

enum DecodeResult : uint8_t { kOk = 0, kBadLength, kBadVersion, kBadType, kBadMac };

// Encoders return the number of bytes written (kDataLen / kAckLen), or 0 if cap is too small.
size_t encodeData(const DataFrame& f, const uint8_t* key, size_t keyLen, uint8_t* out, size_t cap);
size_t encodeAck(const AckFrame& f, const uint8_t* key, size_t keyLen, uint8_t* out, size_t cap);
DecodeResult decode(const uint8_t* buf, size_t len, const uint8_t* key, size_t keyLen, Frame& out);
const char* decodeResultName(DecodeResult r);

// Truncated HMAC-SHA256 (mbedtls on ESP32, CommonCrypto on macOS for host tests).
void mac4(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t len, uint8_t out[kMacLen]);

// Upper-case hex without separators. out needs 2*len+1 bytes.
void toHex(const uint8_t* buf, size_t len, char* out);
// Returns the byte count, or -1 on odd length / bad digit / overflow.
int fromHex(const char* hex, size_t hexLen, uint8_t* out, size_t cap);

}  // namespace wx
