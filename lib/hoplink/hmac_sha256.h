// Minimal SHA-256 and HMAC-SHA256 (FIPS 180-4 / RFC 2104) for the STM32 relay.
// Self-contained so the same code is unit-tested on the host (test/host).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace crypto {

struct Sha256 {
  uint32_t h[8];
  uint64_t bits;
  uint8_t buf[64];
  size_t used;
  void init();
  void update(const uint8_t* data, size_t len);
  void final(uint8_t out[32]);
};

void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* const* parts, const size_t* lens, size_t nParts,
                uint8_t out[32]);

}  // namespace crypto
