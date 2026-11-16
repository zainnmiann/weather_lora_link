// Hop link frames, byte-identical to tools/hopnet.py:
//   FWD : 'L' | 0x10 | src | dst | lseq (u16 LE) | hops | payload | mac4
//   HACK: 'L' | 0x11 | src | dst | lseq (u16 LE) | 0    | mac4
// mac4 = first 4 bytes of HMAC-SHA256(key, "LINK" + header + payload).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hoplink {

constexpr uint8_t kMagic = 0x4C;
constexpr uint8_t kFwd = 0x10;
constexpr uint8_t kHack = 0x11;
constexpr size_t kHdr = 7;
constexpr size_t kMac = 4;
constexpr size_t kMaxPayload = 64;  // end-to-end reading is 33 bytes; room for future fields
constexpr size_t kMaxFrame = kHdr + kMaxPayload + kMac;
constexpr uint8_t kMaxHops = 8;

struct Frame {
  uint8_t type, src, dst, hops;
  uint16_t lseq;
  uint8_t payload[kMaxPayload];
  size_t payloadLen;
};

size_t encodeFwd(uint8_t src, uint8_t dst, uint16_t lseq, uint8_t hops, const uint8_t* payload, size_t payloadLen,
                 const uint8_t* key, size_t keyLen, uint8_t* out, size_t cap);
size_t encodeHack(uint8_t src, uint8_t dst, uint16_t lseq, const uint8_t* key, size_t keyLen, uint8_t* out, size_t cap);
// Returns false for anything that is not a valid link frame for this key (wrong magic, type, length or MAC).
bool decode(const uint8_t* buf, size_t len, const uint8_t* key, size_t keyLen, Frame& out);

// First 8 bytes of SHA-256(payload), used to recognise repeats.
void digest8(const uint8_t* payload, size_t len, uint8_t out[8]);

}  // namespace hoplink
