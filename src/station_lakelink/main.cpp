// LakeLink station: the weather station's own MicroMod ESP32 reads the sensors (the builder's
// WeatherStation.ino: same pins, settings and serial TELEM/EVENT lines) and sends each report over LoRa
// itself, through a LoRa-E5 mini wired to two free GPIOs (factory AT firmware, TEST mode).
//
// Radio protocol = tools/e5_node.py (station role) / relay_fw: each signed 33-byte reading
// (lib/wxlink/wx_frame) is wrapped in a FWD link frame to NEXT_HOP and stays queued until that hop
// acknowledges it (HACK): 3 quick tries, then waits of 5, 10, 20, 40, 60 s (jittered). Every
// transmission is checked against the EU868 sub-band duty-cycle budget (90 % of the legal limit,
// sliding hour). Field scaling and range checks are identical to tools/telem_source.py.
//
// Not yet: queue / duty-log persistence (the carrier's microSD), sleep. E5_RX_PIN / E5_TX_PIN must be
// confirmed on the carrier before wiring.
//
// E5_VIA_USB (test only): the AT commands go out on the USB serial port instead of UART2, and
// tools/usb_bridge.py on the laptop passes them to a LoRa-E5 on another USB port and the replies back.
// Same code path as the wired radio, without soldering. WX_LOG_HELD logs the queue contents for
// tools/chain_check.py.
#include <Arduino.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <math.h>

#include "e5_radio.h"
#include "link.h"
#include "wx_config.h"
#include "wx_frame.h"
#include "wx_station.h"

#ifndef NODE_ID
#define NODE_ID 1
#endif
#ifndef NEXT_HOP
#define NEXT_HOP 2
#endif
#ifndef WX_STATION_ID
#define WX_STATION_ID 1
#endif

using wxstation::nowMs;

// ---- report state ------------------------------------------------------------------------------
static const uint8_t kKey[] = WX_HMAC_KEY;
static uint16_t bootId, seq = 0;

// ---- radio + hop sender ------------------------------------------------------------------------
#ifdef E5_VIA_USB
static E5Radio radio(Serial, -1, -1, -1);  // UART0 on its default pins (USB bridge)
static const uint32_t kRadioBaud = 115200;
#else
static HardwareSerial E5Serial(2);
static E5Radio radio(E5Serial, E5_RX_PIN, E5_TX_PIN, E5_RST_PIN);
static const uint32_t kRadioBaud = E5_BAUD;
#endif
static bool radioReady = false;

struct Item {
  uint8_t payload[wx::kDataLen];
  uint16_t boot, seq;  // copy of the ids inside payload, for the logs
  uint16_t lseq;
  uint8_t attempts;
  uint64_t nextTry;
};
static const int kQueueMax = 64;  // 64 x 5 min = 5 h 20 min of backlog
static Item queue[kQueueMax];
static int qHead = 0, qCount = 0;
static uint16_t myLseq;
static uint32_t statSent = 0, statAcked = 0, statDropped = 0, statDutyWaits = 0, statRadioErrors = 0;
static const uint16_t kBackoffS[] = {5, 10, 20, 40, 60};

// EU868 duty cycle, sub-band table identical to tools/e5_node.py. Airtime is kept in one-minute
// buckets; 61 of them are summed so the window always covers at least a full hour (errs towards
// transmitting less, never more).
struct DutyCycle {
  static const int kBuckets = 61;
  uint32_t budgetMs = 0;
  uint64_t minute[kBuckets] = {0};
  uint32_t usedMs[kBuckets] = {0};
  void begin(float f) {
    struct Band {
      float lo, hi, duty;
    };
    static const Band bands[] = {{863.0f, 865.0f, 0.001f}, {865.0f, 868.0f, 0.01f}, {868.0f, 868.6f, 0.01f},
                                 {868.7f, 869.2f, 0.001f}, {869.4f, 869.65f, 0.10f}, {869.7f, 870.0f, 0.01f}};
    for (auto& b : bands)
      if (b.lo <= f - 0.0625f && f + 0.0625f <= b.hi) budgetMs = uint32_t(3600000.0f * b.duty * 0.9f);
  }
  uint32_t used() {
    uint64_t now = nowMs() / 60000;
    uint32_t s = 0;
    for (int i = 0; i < kBuckets; i++)
      if (now - minute[i] < uint64_t(kBuckets)) s += usedMs[i];
    return s;
  }
  bool allows(uint32_t ms) { return used() + ms <= budgetMs; }  // budget 0 (frequency not in a sub-band) = never
  void record(uint32_t ms) {
    uint64_t now = nowMs() / 60000;
    int i = int(now % kBuckets);
    if (minute[i] != now) minute[i] = now, usedMs[i] = 0;
    usedMs[i] += ms;
  }
} duty;

// Semtech SX126x time-on-air, same formula as tools/e5_node.lora_airtime_s (BW 125 kHz, CR 4/5,
// explicit header, CRC on). Rounded up to whole milliseconds.
static uint32_t airtimeMs(size_t len) {
  const int sf = atoi(WX_SF + 2);
  const float tsym = float(1 << sf) / 125.0f;  // ms
  const int de = sf >= 11 ? 1 : 0;
  int n = 8 + max(int(ceilf((8.0f * len - 4 * sf + 28 + 16) / (4.0f * (sf - 2 * de)))) * 5, 0);
  return uint32_t(ceilf((WX_TX_PREAMBLE + 4.25f) * tsym + n * tsym));
}

static void logHeld() {
#ifdef WX_LOG_HELD
  Serial.print("[lora] held [");
  for (int i = 0; i < qCount; i++) {
    const Item& it = queue[(qHead + i) % kQueueMax];
    Serial.printf("%s%u:%u", i ? ", " : "", it.boot, it.seq);
  }
  Serial.println("]");
#endif
}

static void enqueueReading(const uint8_t* payload, uint16_t boot, uint16_t rseq) {
  if (qCount == kQueueMax) {  // over 5 h without the next hop: keep the newest, drop the oldest
    qHead = (qHead + 1) % kQueueMax;
    qCount--;
    statDropped++;
    Serial.println("[lora] queue full: dropped the oldest reading");
  }
  Item& it = queue[(qHead + qCount) % kQueueMax];
  memcpy(it.payload, payload, wx::kDataLen);
  it.boot = boot;
  it.seq = rseq;
  it.lseq = ++myLseq;
  it.attempts = 0;
  it.nextTry = 0;
  qCount++;
}

// Sends the head of the queue once and waits for its HACK. Blocks for at most the TX (~0.3 s at SF9,
// AT timeout 4 s) plus WX_ACK_TIMEOUT_MS. Readings are only removed after their HACK.
static void pumpQueue() {
  if (!qCount || !radioReady) return;
  Item& it = queue[qHead];
  if (nowMs() < it.nextTry) return;
  uint8_t frame[hoplink::kMaxFrame];
  size_t n = hoplink::encodeFwd(NODE_ID, NEXT_HOP, it.lseq, 1, it.payload, wx::kDataLen, kKey, sizeof(kKey), frame,
                                sizeof(frame));
  if (!n) {  // cannot happen with a 33-byte payload; never spin on it
    Serial.println("[lora] BUG: could not encode frame; reading dropped");
    qHead = (qHead + 1) % kQueueMax;
    qCount--;
    statDropped++;
    return;
  }
  const uint32_t air = airtimeMs(n);
  if (!duty.allows(air)) {
    static uint64_t lastNote = 0;
    if (!lastNote || nowMs() - lastNote > 60000) {
      lastNote = nowMs();
      Serial.printf("[lora] duty-cycle budget used (%lu/%lu ms this hour); waiting (queue %d)\n",
                    (unsigned long)duty.used(), (unsigned long)duty.budgetMs, qCount);
    }
    statDutyWaits++;
    it.nextTry = nowMs() + 5000;
    return;
  }
  duty.record(air);  // counted even if the AT command fails: the module may still have transmitted
  statSent++;
  bool acked = false;
  if (radio.transmit(frame, n) && radio.startReceive()) {
    const uint64_t deadline = nowMs() + WX_ACK_TIMEOUT_MS;
    uint8_t buf[hoplink::kMaxFrame];
    size_t len;
    e5::RxMeta meta;
    while (!acked && nowMs() < deadline) {
      if (radio.poll(buf, sizeof(buf), len, meta)) {
        hoplink::Frame f;
        if (hoplink::decode(buf, len, kKey, sizeof(kKey), f) && f.type == hoplink::kHack && f.dst == NODE_ID &&
            f.src == NEXT_HOP && f.lseq == it.lseq) {
          acked = true;
          Serial.printf("[lora] -> %d ok (lseq %u, try %u, rssi %d snr %.1f, queue %d)\n", NEXT_HOP, it.lseq,
                        it.attempts + 1, meta.rssi, meta.snr, qCount - 1);
        }
      }
      delay(2);
    }
  } else {
    statRadioErrors++;
    Serial.println("[lora] radio command failed; re-initialising the radio");
    radioReady = radio.configure();
  }
  if (acked) {
    statAcked++;
    qHead = (qHead + 1) % kQueueMax;
    qCount--;
    logHeld();
    return;
  }
  if (it.attempts < 255) it.attempts++;
  if (it.attempts < 3) {
    it.nextTry = nowMs() + 300 + esp_random() % 700;
  } else {
    const int last = int(sizeof(kBackoffS) / sizeof(kBackoffS[0])) - 1;
    const uint32_t waitMs = kBackoffS[min(int(it.attempts) - 3, last)] * (700 + esp_random() % 600);
    it.nextTry = nowMs() + waitMs;
    Serial.printf("[lora] next hop %d unreachable after %u tries; kept, retry in %lu s (queue %d)\n", NEXT_HOP, it.attempts,
                  (unsigned long)(waitMs / 1000), qCount);
  }
}

static void makeReport() {
  wx::DataFrame d;
  d.station_id = WX_STATION_ID;
  d.boot_id = bootId;
  d.seq = ++seq;
  if (seq == 1) d.r.flags |= wx::kFlagColdBoot;
  wxstation::fillReading(d);
  uint8_t payload[wx::kDataLen];
  if (wx::encodeData(d, kKey, sizeof(kKey), payload, sizeof(payload)) != wx::kDataLen) {
    Serial.println("[lora] BUG: could not encode reading");
    return;
  }
  enqueueReading(payload, bootId, seq);
  Serial.printf("[lora] reading boot %u seq %u queued%s (queue %d; sent %lu acked %lu dropped %lu duty-waits %lu "
                "radio-errors %lu; airtime %lu/%lu ms this hour)\n",
                bootId, seq, (d.r.flags & wx::kFlagSensorFail) ? " SENSOR_FAIL" : "", qCount, (unsigned long)statSent,
                (unsigned long)statAcked, (unsigned long)statDropped, (unsigned long)statDutyWaits,
                (unsigned long)statRadioErrors, (unsigned long)duty.used(), (unsigned long)duty.budgetMs);
  logHeld();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Hava Istasyonu baslatiliyor ===");  // tools/telem_source.py detects restarts by this banner
  Serial.printf("LakeLink station %d, node %d -> next hop %d\n", WX_STATION_ID, NODE_ID, NEXT_HOP);
  esp_task_wdt_init(30, true);  // reboot if loop() ever hangs for 30 s (longest normal block is ~7 s)
  esp_task_wdt_add(NULL);

  wxstation::begin();

  bootId = uint16_t(esp_random());
  myLseq = uint16_t(esp_random());
  duty.begin(atof(WX_FREQ_MHZ));
  if (!duty.budgetMs) Serial.printf("[lora] ERROR: %s MHz is not inside an EU868 sub-band; will never transmit\n", WX_FREQ_MHZ);
  radio.setLog(&Serial);
  radio.begin(kRadioBaud);
  radioReady = radio.configure();
  Serial.printf("[lora] boot %u, %s MHz %s %d dBm, radio %s, duty budget %lu ms/h, report every %d s\n", bootId,
                WX_FREQ_MHZ, WX_SF, WX_TX_POWER_DBM, radioReady ? "ready" : "NOT RESPONDING",
                (unsigned long)duty.budgetMs, WX_INTERVAL_S);
  Serial.println("--------------------------------------");
}

void loop() {
  esp_task_wdt_reset();

  wxstation::poll();

  static uint64_t nextReport = 10000;  // first report 10 s after boot, once the sensors have been read
  if (nowMs() >= nextReport) {
    nextReport = nowMs() + uint64_t(WX_INTERVAL_S) * 1000;
    makeReport();
  }

  if (!radioReady) {
    static uint64_t lastTry = 0;
    if (nowMs() - lastTry > 30000) {
      lastTry = nowMs();
      radioReady = radio.configure();
      Serial.printf("[lora] radio %s\n", radioReady ? "ready" : "still not responding");
    }
  }
  pumpQueue();
  delay(5);
}
