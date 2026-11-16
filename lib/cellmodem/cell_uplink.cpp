#include "cell_uplink.h"

#include <esp_random.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "sim800.h"
#include "upload_batch.h"

namespace celluplink {

static uint64_t nowMs() { return uint64_t(esp_timer_get_time() / 1000); }

struct Item {
  uint32_t id;  // increases by one per reading; lets the task remove exactly what it uploaded
  uint8_t station;
  uint8_t frame[upload::kFrameLen];
  uint64_t measuredMs;
};
static const int kBatchMax = 16;  // ~1.2 KB per POST: small enough for a weak 2G link
static Item queue[kQueueMax];
static int qHead = 0, qCount = 0;
static uint32_t nextId = 1, dropped = 0;
static SemaphoreHandle_t qLock;
static Config cfg;

void enqueue(const uint8_t* frame, uint8_t station, uint32_t ageS) {
  xSemaphoreTake(qLock, portMAX_DELAY);
  if (qCount == kQueueMax) {  // nothing uploaded for a long time: keep the newest
    qHead = (qHead + 1) % kQueueMax;
    qCount--;
    dropped++;
  }
  Item& it = queue[(qHead + qCount) % kQueueMax];
  it.id = nextId++;
  it.station = station;
  memcpy(it.frame, frame, upload::kFrameLen);
  const uint64_t now = nowMs(), age = uint64_t(ageS) * 1000;
  it.measuredMs = age < now ? now - age : 0;
  qCount++;
  xSemaphoreGive(qLock);
}

// Copies up to kBatchMax oldest readings of one station (a batch carries one station id); ages are
// computed now. Returns how many.
static int snapshot(uint8_t (*frames)[upload::kFrameLen], uint32_t* ages, uint8_t& station, uint32_t& lastId) {
  xSemaphoreTake(qLock, portMAX_DELAY);
  const uint64_t now = nowMs();
  int n = 0;
  if (qCount) station = queue[qHead].station;
  for (; n < min(qCount, kBatchMax); n++) {
    const Item& it = queue[(qHead + n) % kQueueMax];
    if (it.station != station) break;
    memcpy(frames[n], it.frame, upload::kFrameLen);
    ages[n] = uint32_t((now - it.measuredMs) / 1000);
    lastId = it.id;
  }
  xSemaphoreGive(qLock);
  return n;
}

// Removes every reading with id <= lastId (some may already be gone if the queue overflowed meanwhile).
static void removeUpTo(uint32_t lastId) {
  xSemaphoreTake(qLock, portMAX_DELAY);
  while (qCount && int32_t(queue[qHead].id - lastId) <= 0) {
    qHead = (qHead + 1) % kQueueMax;
    qCount--;
  }
  xSemaphoreGive(qLock);
}

int queued() {
  xSemaphoreTake(qLock, portMAX_DELAY);
  int n = qCount;
  xSemaphoreGive(qLock);
  return n;
}

static Sim800* modem;
static volatile uint32_t upOk = 0, upFail = 0, readingsSent = 0;

static void feedWdt() { esp_task_wdt_reset(); }

static void pause(uint32_t ms) {
  const uint32_t until = millis() + ms;
  while (int32_t(until - millis()) > 0) {
    feedWdt();
    delay(200);
  }
}

static void task(void*) {
  esp_task_wdt_add(NULL);
  modem->setLog(&Serial);
  modem->setTick(feedWdt);
  static uint8_t frames[kBatchMax][upload::kFrameLen];
  static uint32_t ages[kBatchMax];
  static char body[upload::capacityFor(kBatchMax)];
  char resp[96];
  bool modemOk = false, bearer = false;
  int failures = 0;
  uint32_t backoffMs = 30000;
  uint64_t lastStatus = 0;

  for (;;) {
    feedWdt();
    if (!modemOk || modem->powerLost()) {
      modem->clearPowerLost();
      modemOk = modem->probe();
      bearer = false;
      if (!modemOk) {
        Serial.println("[cell] modem does not answer (power? wiring?); retrying in 60 s");
        pause(60000);
        continue;
      }
    }
    if (nowMs() - lastStatus > 60000 || !lastStatus) {
      lastStatus = nowMs();
      Serial.printf("[cell] sim %s, registration %d, signal %d/31, bearer %s, queue %d, uploads ok %lu fail %lu, "
                    "readings sent %lu, dropped %lu\n",
                    modem->simReady() ? "ready" : "NOT READY", modem->registration(), modem->signal(),
                    bearer ? "up" : "down", queued(), (unsigned long)upOk, (unsigned long)upFail,
                    (unsigned long)readingsSent, (unsigned long)dropped);
    }
    if (!queued() || strlen(cfg.url) == 0) {
      pause(1000);
      continue;
    }
    const int reg = modem->registration();
    if (reg != 1 && reg != 5) {
      Serial.printf("[cell] not registered on the network (%d); waiting\n", reg);
      pause(15000);
      continue;
    }
    if (!bearer && !(bearer = modem->bearerOpen(cfg.apn))) {
      failures++;
      Serial.printf("[cell] GPRS bearer failed (%d in a row); retry in %lu s\n", failures,
                    (unsigned long)(backoffMs / 1000));
    } else {
      uint32_t lastId = 0;
      uint8_t station = 0;
      const int n = snapshot(frames, ages, station, lastId);
      const size_t len = upload::build(station, frames, ages, n, cfg.key, cfg.keyLen, body, sizeof(body));
      const int status = len ? modem->post(cfg.url, "text/plain", body, len, resp, sizeof(resp)) : -1;
      if (status == 200 && strncmp(resp, "OK", 2) == 0) {
        removeUpTo(lastId);
        upOk++;
        readingsSent += n;
        failures = 0;
        backoffMs = 30000;
        Serial.printf("[cell] uploaded %d readings: %s", n, resp);
        continue;  // more queued? send the next batch right away
      }
      failures++;
      upFail++;
      bearer = false;  // re-check the bearer before the next try
      Serial.printf("[cell] upload of %d readings failed: HTTP %d %s (%d in a row); retry in %lu s\n", n, status,
                    resp, failures, (unsigned long)(backoffMs / 1000));
      if (status == 403) Serial.println("[cell] server rejected the signature: station key does not match the server");
    }
    if (failures % 3 == 0) modem->bearerClose();
    if (failures >= 6) {
      modemOk = modem->restart();
      failures = 0;
    }
    pause(backoffMs + esp_random() % 5000);
    backoffMs = min<uint32_t>(backoffMs * 2, 300000);
  }
}

void begin(const Config& c) {
  cfg = c;
  qLock = xSemaphoreCreateMutex();
  modem = new Sim800(Serial1, cfg.rxPin, cfg.txPin);
  xTaskCreatePinnedToCore(task, "cell", 8192, nullptr, 1, nullptr, cfg.core);
}

}  // namespace celluplink
