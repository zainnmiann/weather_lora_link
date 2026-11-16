#include "upload_batch.h"

#include <stdio.h>

#include "hmac_sha256.h"

namespace upload {

static const char kHex[] = "0123456789abcdef";

size_t build(uint8_t station, const uint8_t (*frames)[kFrameLen], const uint32_t* ages, size_t n,
             const uint8_t* key, size_t keyLen, char* out, size_t cap) {
  if (cap < capacityFor(n)) return 0;
  size_t pos = size_t(snprintf(out, cap, "WXB1 %u\n", unsigned(station)));
  for (size_t i = 0; i < n; i++) {
    for (size_t b = 0; b < kFrameLen; b++) {
      out[pos++] = kHex[frames[i][b] >> 4];
      out[pos++] = kHex[frames[i][b] & 15];
    }
    pos += size_t(snprintf(out + pos, cap - pos, " %lu\n", (unsigned long)ages[i]));
  }
  uint8_t mac[32];
  const uint8_t* parts[] = {reinterpret_cast<const uint8_t*>(out)};
  const size_t lens[] = {pos};
  crypto::hmacSha256(key, keyLen, parts, lens, 1, mac);
  pos += size_t(snprintf(out + pos, cap - pos, "MAC "));
  for (int b = 0; b < 8; b++) {
    out[pos++] = kHex[mac[b] >> 4];
    out[pos++] = kHex[mac[b] & 15];
  }
  out[pos++] = '\n';
  out[pos] = '\0';
  return pos;
}

}  // namespace upload
