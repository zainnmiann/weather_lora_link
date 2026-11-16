#include "e5_parse.h"

#include <stdlib.h>
#include <string.h>

#include "wx_frame.h"

namespace e5 {

namespace {

const char* skipSpaces(const char* p) {
  while (*p == ' ') p++;
  return p;
}

// Finds "<key>:" in line and returns a pointer just past the colon, or nullptr.
const char* findField(const char* line, const char* key) {
  const char* p = strstr(line, key);
  if (!p) return nullptr;
  p += strlen(key);
  if (*p != ':') return nullptr;
  return skipSpaces(p + 1);
}

}  // namespace

bool parseRxMeta(const char* line, RxMeta& out) {
  if (strncmp(line, "+TEST:", 6) != 0) return false;
  const char* len = findField(line, "LEN");
  const char* rssi = findField(line, "RSSI");
  const char* snr = findField(line, "SNR");
  if (!len || !rssi || !snr) return false;
  char* end;
  long l = strtol(len, &end, 10);
  if (end == len) return false;
  long r = strtol(rssi, &end, 10);
  if (end == rssi) return false;
  float s = strtof(snr, &end);
  if (end == snr) return false;
  out.len = int(l);
  out.rssi = int(r);
  out.snr = s;
  out.valid = true;
  return true;
}

int parseRxHex(const char* line, uint8_t* out, size_t cap) {
  static const char kPrefix[] = "+TEST: RX ";
  if (strncmp(line, kPrefix, sizeof(kPrefix) - 1) != 0) return -1;
  const char* p = skipSpaces(line + sizeof(kPrefix) - 1);
  if (*p != '"') return -1;
  p++;
  const char* close = strchr(p, '"');
  if (!close) return -1;
  return wx::fromHex(p, size_t(close - p), out, cap);
}

bool isError(const char* line) { return line[0] == '+' && strstr(line, "ERROR") != nullptr; }

}  // namespace e5
