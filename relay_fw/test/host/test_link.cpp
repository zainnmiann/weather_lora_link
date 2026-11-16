// Host test for the relay's crypto and link frames. Prints vectors that test_parity.py compares with hopnet.py.
#include <stdio.h>
#include <string.h>
#include "../../../lib/hoplink/hmac_sha256.h"
#include "../../../lib/hoplink/link.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void hex(const char* tag, const uint8_t* b, size_t n) { printf("%s ", tag); for (size_t i = 0; i < n; i++) printf("%02X", b[i]); printf("\n"); }

int main() {
  // RFC 4231 test case 2: key "Jefe", data "what do ya want for nothing?"
  const uint8_t key2[] = {'J', 'e', 'f', 'e'};
  const char* msg2 = "what do ya want for nothing?";
  const uint8_t* parts[1] = {(const uint8_t*)msg2};
  size_t lens[1] = {strlen(msg2)};
  uint8_t out[32];
  crypto::hmacSha256(key2, 4, parts, lens, 1, out);
  const uint8_t expect2[32] = {0x5b,0xdc,0xc1,0x46,0xbf,0x60,0x75,0x4e,0x6a,0x04,0x24,0x26,0x08,0x95,0x75,0xc7,
                               0x5a,0x00,0x3f,0x08,0x9d,0x27,0x39,0x83,0x9d,0xec,0x58,0xb9,0x64,0xec,0x38,0x43};
  CHECK(memcmp(out, expect2, 32) == 0);
  // RFC 4231 test case 6: 131-byte key (longer than a block) exercises the key-hash path
  uint8_t key6[131]; memset(key6, 0xaa, sizeof(key6));
  const char* msg6 = "Test Using Larger Than Block-Size Key - Hash Key First";
  parts[0] = (const uint8_t*)msg6; lens[0] = strlen(msg6);
  crypto::hmacSha256(key6, sizeof(key6), parts, lens, 1, out);
  const uint8_t expect6[32] = {0x60,0xe4,0x31,0x59,0x1e,0xe0,0xb6,0x7f,0x0d,0x8a,0x26,0xaa,0xcb,0xf5,0xb7,0x7f,
                               0x8e,0x0b,0xc6,0x21,0x37,0x28,0xc5,0x14,0x05,0x46,0x04,0x0f,0x0e,0xe3,0x7f,0x54};
  CHECK(memcmp(out, expect6, 32) == 0);

  const uint8_t key[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
  uint8_t payload[33]; for (int i = 0; i < 33; i++) payload[i] = uint8_t(i * 7 + 1);
  uint8_t f[hoplink::kMaxFrame];
  size_t n = hoplink::encodeFwd(1, 2, 0xBEEF, 1, payload, sizeof(payload), key, 16, f, sizeof(f));
  CHECK(n == 44);
  hex("VEC_FWD", f, n);
  hoplink::Frame fr;
  CHECK(hoplink::decode(f, n, key, 16, fr) && fr.type == hoplink::kFwd && fr.src == 1 && fr.dst == 2 && fr.lseq == 0xBEEF && fr.hops == 1 && fr.payloadLen == 33);
  for (size_t i = 0; i < n; i++) { uint8_t b[hoplink::kMaxFrame]; memcpy(b, f, n); b[i] ^= 1; CHECK(!hoplink::decode(b, n, key, 16, fr)); }
  uint8_t other[16] = {1}; CHECK(!hoplink::decode(f, n, other, 16, fr));
  size_t h = hoplink::encodeHack(2, 1, 0xBEEF, key, 16, f, sizeof(f));
  CHECK(h == 11);
  hex("VEC_HACK", f, h);
  uint8_t d8[8]; hoplink::digest8(payload, 33, d8); hex("VEC_DIGEST", d8, 8);
  printf(fails ? "%d FAILED\n" : "host checks passed\n", fails);
  return fails ? 1 : 0;
}
