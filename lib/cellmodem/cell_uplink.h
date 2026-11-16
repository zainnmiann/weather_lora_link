// Cellular uplink: a queue of signed readings (33-byte wx frames) and a FreeRTOS task that uploads them
// through a SIM800L to the cloud server (POST WXB1 batches, lib/cellmodem/upload_batch.h).
// Used by the cellular station (its own readings) and by the LoRa gateway (readings heard from stations).
//
// The modem never blocks the caller: enqueue() only copies into RAM. A reading leaves the queue only after
// the server replied "200 OK ..."; failures back off 30 s -> 5 min, repeated failures reopen the bearer,
// then restart the modem. RAM only: a power loss loses what is still queued.
#pragma once
#include <Arduino.h>

namespace celluplink {

struct Config {
  const char* url = "";        // empty = the modem is exercised but nothing is sent
  const char* apn = "internet";
  const uint8_t* key = nullptr;  // station HMAC key (signs each batch)
  size_t keyLen = 0;
  int rxPin = 16, txPin = 17;  // ESP32 pins wired to the modem's TXD / RXD
  int core = 0;
};

static const int kQueueMax = 512;  // 42 h at one reading per 5 min

void begin(const Config& c);
// Queues one signed reading of `station`, measured `ageS` seconds ago.
void enqueue(const uint8_t* frame, uint8_t station, uint32_t ageS = 0);
int queued();

}  // namespace celluplink
