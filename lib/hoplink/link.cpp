#include "link.h"

#include <string.h>

#include "hmac_sha256.h"

namespace hoplink {

namespace {

void mac4(const uint8_t* key, size_t keyLen, const uint8_t* body, size_t len, uint8_t out[kMac]) {
  static const uint8_t kDomain[4] = {'L', 'I', 'N', 'K'};
  const uint8_t* parts[2] = {kDomain, body};
  size_t lens[2] = {sizeof(kDomain), len};
  uint8_t full[32];
  crypto::hmacSha256(key, keyLen, parts, lens, 2, full);
  memcpy(out, full, kMac);
}

size_t header(uint8_t type, uint8_t src, uint8_t dst, uint16_t lseq, uint8_t hops, uint8_t* out) {
  out[0] = kMagic;
  out[1] = type;
  out[2] = src;
  out[3] = dst;
  out[4] = lseq & 0xFF;
  out[5] = lseq >> 8;
  out[6] = hops;
  return kHdr;
}

}  // namespace

size_t encodeFwd(uint8_t src, uint8_t dst, uint16_t lseq, uint8_t hops, const uint8_t* payload, size_t payloadLen,
                 const uint8_t* key, size_t keyLen, uint8_t* out, size_t cap) {
  if (payloadLen > kMaxPayload || cap < kHdr + payloadLen + kMac) return 0;
  size_t n = header(kFwd, src, dst, lseq, hops, out);
  memcpy(out + n, payload, payloadLen);
  n += payloadLen;
  mac4(key, keyLen, out, n, out + n);
  return n + kMac;
}

size_t encodeHack(uint8_t src, uint8_t dst, uint16_t lseq, const uint8_t* key, size_t keyLen, uint8_t* out, size_t cap) {
  if (cap < kHdr + kMac) return 0;
  size_t n = header(kHack, src, dst, lseq, 0, out);
  mac4(key, keyLen, out, n, out + n);
  return n + kMac;
}

bool decode(const uint8_t* buf, size_t len, const uint8_t* key, size_t keyLen, Frame& out) {
  if (len < kHdr + kMac || len > kMaxFrame || buf[0] != kMagic) return false;
  uint8_t expect[kMac];
  mac4(key, keyLen, buf, len - kMac, expect);
  uint8_t diff = 0;
  for (size_t i = 0; i < kMac; i++) diff |= expect[i] ^ buf[len - kMac + i];
  if (diff) return false;
  if (buf[1] != kFwd && buf[1] != kHack) return false;
  out.type = buf[1];
  out.src = buf[2];
  out.dst = buf[3];
  out.lseq = uint16_t(buf[4] | (buf[5] << 8));
  out.hops = buf[6];
  out.payloadLen = len - kHdr - kMac;
  memcpy(out.payload, buf + kHdr, out.payloadLen);
  return true;
}

void digest8(const uint8_t* payload, size_t len, uint8_t out[8]) {
  crypto::Sha256 s;
  s.init();
  s.update(payload, len);
  uint8_t full[32];
  s.final(full);
  memcpy(out, full, 8);
}

}  // namespace hoplink
