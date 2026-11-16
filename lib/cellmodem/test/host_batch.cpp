// Host check: builds a batch and prints it; test_batch_parity.py feeds it to the server's parser.
#include <stdio.h>
#include <string.h>

#include "upload_batch.h"

int main() {
  uint8_t key[16];
  for (int i = 0; i < 16; i++) key[i] = uint8_t(i);
  uint8_t frames[3][upload::kFrameLen];
  for (int f = 0; f < 3; f++)
    for (size_t b = 0; b < upload::kFrameLen; b++) frames[f][b] = uint8_t(f * 40 + b);
  uint32_t ages[3] = {0, 300, 14 * 24 * 3600};
  char out[upload::capacityFor(3)];
  size_t n = upload::build(7, frames, ages, 3, key, sizeof(key), out, sizeof(out));
  if (!n || n != strlen(out)) return 1;
  char tiny[50];
  if (upload::build(7, frames, ages, 3, key, sizeof(key), tiny, sizeof(tiny)) != 0) return 2;  // must refuse
  fputs(out, stdout);
  return 0;
}
