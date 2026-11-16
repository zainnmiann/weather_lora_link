// Upload batch for the cloud server (format defined in lakelink_cloud/lakelink/batch.py):
//   "WXB1 <station>\n" + "<66 hex> <age_s>\n" per reading + "MAC <16 hex>\n"
// MAC = first 8 bytes of HMAC-SHA256(key, everything before the "MAC " line).
// Plain C++ (no Arduino) so it is unit-tested on the host against the server's parser.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace upload {

constexpr size_t kFrameLen = 33;
constexpr size_t kMaxLine = 2 * kFrameLen + 1 + 10 + 1;  // hex, space, age (<= 10 digits), newline
constexpr size_t kOverhead = 16 + 4 + 16 + 1;            // header + "MAC " + mac + newline

inline constexpr size_t capacityFor(size_t n) { return kOverhead + n * kMaxLine + 1; }

// Returns the batch length (without the terminating NUL it also writes), or 0 if `cap` is too small.
size_t build(uint8_t station, const uint8_t (*frames)[kFrameLen], const uint32_t* ages, size_t n,
             const uint8_t* key, size_t keyLen, char* out, size_t cap);

}  // namespace upload
