#include "wx_frame.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "mbedtls/md.h"
#elif defined(__APPLE__)
#include <CommonCrypto/CommonHMAC.h>
#else
#error "wx_frame: no HMAC-SHA256 backend for this platform"
#endif

namespace wx {

namespace {

void put16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}

uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

void putHeader(uint8_t* p, FrameType type, uint8_t stationId, uint16_t bootId, uint16_t seq) {
  p[0] = kVersion;
  p[1] = type;
  p[2] = stationId;
  put16(p + 3, bootId);
  put16(p + 5, seq);
}

bool macMatches(const uint8_t* key, size_t keyLen, const uint8_t* buf, size_t len) {
  uint8_t expect[kMacLen];
  mac4(key, keyLen, buf, len - kMacLen, expect);
  uint8_t diff = 0;  // constant-time compare
  for (size_t i = 0; i < kMacLen; i++) diff |= expect[i] ^ buf[len - kMacLen + i];
  return diff == 0;
}

}  // namespace

void mac4(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t len, uint8_t out[kMacLen]) {
  uint8_t full[32];
#if defined(ESP_PLATFORM)
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, keyLen, msg, len, full);
#else
  CCHmac(kCCHmacAlgSHA256, key, keyLen, msg, len, full);
#endif
  memcpy(out, full, kMacLen);
}

size_t encodeData(const DataFrame& f, const uint8_t* key, size_t keyLen, uint8_t* out, size_t cap) {
  if (cap < kDataLen) return 0;
  putHeader(out, kTypeData, f.station_id, f.boot_id, f.seq);
  put16(out + 7, uint16_t(f.r.temp_c_x100));
  put16(out + 9, f.r.rh_x100);
  put16(out + 11, f.r.press_hpa_x10);
  put16(out + 13, f.r.wind_ms_x100);
  put16(out + 15, f.r.gust_ms_x100);
  put16(out + 17, f.r.wind_dir_deg);
  put16(out + 19, f.r.rain_total_mm_x10);
  put16(out + 21, f.r.batt_mv);
  put16(out + 23, f.r.soil_pct_x10);
  put16(out + 25, f.r.lightning_total);
  out[27] = f.r.lightning_km;
  out[28] = f.r.flags;
  mac4(key, keyLen, out, kDataLen - kMacLen, out + kDataLen - kMacLen);
  return kDataLen;
}

size_t encodeAck(const AckFrame& f, const uint8_t* key, size_t keyLen, uint8_t* out, size_t cap) {
  if (cap < kAckLen) return 0;
  putHeader(out, kTypeAck, f.station_id, f.boot_id, f.seq);
  out[7] = f.status;
  mac4(key, keyLen, out, kAckLen - kMacLen, out + kAckLen - kMacLen);
  return kAckLen;
}

DecodeResult decode(const uint8_t* buf, size_t len, const uint8_t* key, size_t keyLen, Frame& out) {
  if (len < 2) return kBadLength;
  if (buf[0] != kVersion) return kBadVersion;
  if (buf[1] == kTypeData) {
    if (len != kDataLen) return kBadLength;
    if (!macMatches(key, keyLen, buf, len)) return kBadMac;
    out.type = kTypeData;
    DataFrame& d = out.data;
    d.station_id = buf[2];
    d.boot_id = get16(buf + 3);
    d.seq = get16(buf + 5);
    d.r.temp_c_x100 = int16_t(get16(buf + 7));
    d.r.rh_x100 = get16(buf + 9);
    d.r.press_hpa_x10 = get16(buf + 11);
    d.r.wind_ms_x100 = get16(buf + 13);
    d.r.gust_ms_x100 = get16(buf + 15);
    d.r.wind_dir_deg = get16(buf + 17);
    d.r.rain_total_mm_x10 = get16(buf + 19);
    d.r.batt_mv = get16(buf + 21);
    d.r.soil_pct_x10 = get16(buf + 23);
    d.r.lightning_total = get16(buf + 25);
    d.r.lightning_km = buf[27];
    d.r.flags = buf[28];
    return kOk;
  }
  if (buf[1] == kTypeAck) {
    if (len != kAckLen) return kBadLength;
    if (!macMatches(key, keyLen, buf, len)) return kBadMac;
    out.type = kTypeAck;
    AckFrame& a = out.ack;
    a.station_id = buf[2];
    a.boot_id = get16(buf + 3);
    a.seq = get16(buf + 5);
    a.status = buf[7];
    return kOk;
  }
  return kBadType;
}

const char* decodeResultName(DecodeResult r) {
  switch (r) {
    case kOk: return "ok";
    case kBadLength: return "bad_length";
    case kBadVersion: return "bad_version";
    case kBadType: return "bad_type";
    case kBadMac: return "bad_mac";
  }
  return "unknown";
}

void toHex(const uint8_t* buf, size_t len, char* out) {
  static const char kDigits[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = kDigits[buf[i] >> 4];
    out[2 * i + 1] = kDigits[buf[i] & 0x0F];
  }
  out[2 * len] = '\0';
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

int fromHex(const char* hex, size_t hexLen, uint8_t* out, size_t cap) {
  if (hexLen % 2 != 0 || hexLen / 2 > cap) return -1;
  for (size_t i = 0; i < hexLen / 2; i++) {
    int hi = hexNibble(hex[2 * i]);
    int lo = hexNibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = uint8_t((hi << 4) | lo);
  }
  return int(hexLen / 2);
}

}  // namespace wx
