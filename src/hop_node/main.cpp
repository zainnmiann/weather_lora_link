// LakeLink hop node on an ESP32 with an SX1278 (Ra-01 / XL1278-SMT, 433 MHz). One firmware, three roles:
//
//   station (ROLE 1): reads the weather sensors (lib/wxstation), signs each reading (lib/wxlink, 33 bytes)
//                     and sends it to NEXT_HOP.
//   relay   (ROLE 2): accepts readings addressed to it, acknowledges them and forwards them to NEXT_HOP.
//   gateway (ROLE 3): accepts readings, checks the station's signature, drops repeats and prints
//                     one `DELIVERED {json}` line per reading on USB serial (the uploader reads these).
//                     Built with CELL_URL, it also uploads each new reading itself through a SIM800L
//                     (lib/cellmodem/cell_uplink; modem TXD -> GPIO16, RXD -> GPIO17).
// The station also prints each signed reading as `WXF <hex>` (tools/station_uploader.py), so a USB uploader
// can run in parallel; the server drops readings it already has.
//
// Same protocol as tools/hopnet.py / e5_node.py / relay_fw (lib/hoplink): every hop is acknowledged
// (HACK) and a node keeps a packet until the next hop has acknowledged it: 3 quick tries, then waits of
// 5, 10, 20, 40, 60 s (jittered, never given up). Repeats (same sender, link seq and payload) are
// re-acknowledged but forwarded once. Every transmission, acknowledgements included, is checked against
// the duty-cycle budget (90 % of the legal limit, sliding hour); with no airtime left a node does not
// acknowledge, so the sender keeps the packet.
// The radio is polled (no DIO0 / RESET wired); a radio that stops answering is re-initialised.
#include <Arduino.h>
#include <SPI.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

#include "link.h"
#include "sx1278.h"
#include "wx_config.h"
#include "wx_frame.h"
#if ROLE == 1
#include "wx_station.h"
#endif
#if ROLE == 3 && defined(CELL_URL)
#include "cell_uplink.h"
#ifndef CELL_APN
#define CELL_APN "internet"
#endif
#endif

#ifndef ROLE
#error "ROLE must be 1 (station), 2 (relay) or 3 (gateway)"
#endif
#ifndef NODE_ID
#error "NODE_ID must be set"
#endif
#if ROLE != 3 && !defined(NEXT_HOP)
#error "NEXT_HOP must be set for a station or relay"
#endif
#ifndef LORA_NSS
#error "LORA_NSS must be set"
#endif
#ifndef HOP_FREQ_MHZ
#define HOP_FREQ_MHZ 434.0
#endif
#ifndef HOP_SF
#define HOP_SF 9
#endif
#ifndef HOP_POWER_DBM
#define HOP_POWER_DBM 10  // 433 MHz SRD limit is 10 mW e.r.p.
#endif
#ifndef WX_STATION_ID
#define WX_STATION_ID 1
#endif

static const char* kRoleName = ROLE == 1 ? "station" : ROLE == 2 ? "relay" : "gateway";
static const uint8_t kKey[] = WX_HMAC_KEY;
static uint64_t nowMs() { return uint64_t(esp_timer_get_time() / 1000); }

static void logf(const char* fmt, ...) {
  char buf[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.printf("%lus [%s %d] %s\n", (unsigned long)(nowMs() / 1000), kRoleName, NODE_ID, buf);
}

// ---- radio --------------------------------------------------------------------------------------
static Sx1278 radio(SPI, LORA_NSS);
static bool radioReady = false;

static bool radioInit() {
  Sx1278::Config c;
  c.freqMhz = HOP_FREQ_MHZ;
  c.sf = HOP_SF;
  c.powerDbm = HOP_POWER_DBM;
  c.preamble = WX_TX_PREAMBLE;
  return radio.begin(c) && radio.startReceive();
}

// ---- duty cycle ---------------------------------------------------------------------------------
// One-minute buckets; 61 are summed so the window always covers at least a full hour.
struct DutyCycle {
  static const int kBuckets = 61;
  uint32_t budgetMs = 0;
  uint64_t minute[kBuckets] = {0};
  uint32_t usedMs[kBuckets] = {0};
  void begin(float f) {
    struct Band {
      float lo, hi, duty;
    };
    static const Band bands[] = {{433.05f, 434.79f, 0.10f},  // ERC 70-03 annex 1 h1.4: 10 mW, 10 %
                                 {863.0f, 865.0f, 0.001f}, {865.0f, 868.0f, 0.01f}, {868.0f, 868.6f, 0.01f},
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
  bool allows(uint32_t ms) { return used() + ms <= budgetMs; }
  void record(uint32_t ms) {
    uint64_t now = nowMs() / 60000;
    int i = int(now % kBuckets);
    if (minute[i] != now) minute[i] = now, usedMs[i] = 0;
    usedMs[i] += ms;
  }
} duty;

struct Stats {
  uint32_t in, dupIn, sent, acked, retries, refusedDuty, dropped, bad, radioErrors, delivered, rejected;
} stats;

static bool send(const uint8_t* frame, size_t len) {
  duty.record(radio.airtimeMs(len));  // counted even on failure: the chip may still have transmitted
  bool ok = radio.transmit(frame, len);
  if (!ok) {
    stats.radioErrors++;
    logf("transmit timed out; re-initialising the radio");
    radioReady = radioInit();
    return false;
  }
  if (!radio.startReceive()) {
    stats.radioErrors++;
    radioReady = radioInit();
  }
  return true;
}

// ---- outgoing queue (station, relay) ------------------------------------------------------------
#if ROLE != 3
struct Item {
  uint8_t payload[hoplink::kMaxPayload];
  uint8_t len, hops, attempts;
  uint16_t lseq;
  uint64_t nextTry;
};
static const int kQueueMax = 64;
static Item queue[kQueueMax];
static int qHead = 0, qCount = 0;
static uint16_t myLseq;
static bool inflight = false, acked = false;
static uint64_t ackDeadline = 0;
static const uint16_t kBackoffS[] = {5, 10, 20, 40, 60};

static void enqueue(const uint8_t* payload, size_t len, uint8_t hops) {
  if (qCount == kQueueMax) {  // keep the newest: drop the oldest packet that is not in flight
    int victim = (inflight && qCount > 1) ? 1 : 0;
    for (int i = victim; i < qCount - 1; i++) queue[(qHead + i) % kQueueMax] = queue[(qHead + i + 1) % kQueueMax];
    qCount--;
    stats.dropped++;
    logf("queue full: dropped the oldest waiting packet");
  }
  Item& it = queue[(qHead + qCount) % kQueueMax];
  memcpy(it.payload, payload, len);
  it.len = len;
  it.hops = hops;
  it.attempts = 0;
  it.lseq = ++myLseq;
  it.nextTry = 0;
  qCount++;
}

static void pump() {
  const uint64_t now = nowMs();
  if (inflight) {
    Item& it = queue[qHead];
    if (acked) {
      inflight = acked = false;
      stats.acked++;
      logf("-> %d ok (lseq %u, try %u, queue %d)", NEXT_HOP, it.lseq, it.attempts + 1, qCount - 1);
      qHead = (qHead + 1) % kQueueMax;
      qCount--;
    } else if (now >= ackDeadline) {
      inflight = false;
      if (it.attempts < 255) it.attempts++;
      stats.retries++;
      if (it.attempts < 3) {
        it.nextTry = now + 300 + esp_random() % 700;
      } else {
        const int last = int(sizeof(kBackoffS) / sizeof(kBackoffS[0])) - 1;
        const uint32_t wait = kBackoffS[min(int(it.attempts) - 3, last)] * (700 + esp_random() % 600);
        it.nextTry = now + wait;
        logf("-> %d unreachable after %u tries; kept, retry in %lu s (queue %d)", NEXT_HOP, it.attempts,
             (unsigned long)(wait / 1000), qCount);
      }
    }
    return;
  }
  if (!qCount || !radioReady) return;
  Item& it = queue[qHead];
  if (now < it.nextTry) return;
  uint8_t frame[hoplink::kMaxFrame];
  size_t n = hoplink::encodeFwd(NODE_ID, NEXT_HOP, it.lseq, it.hops, it.payload, it.len, kKey, sizeof(kKey), frame,
                                sizeof(frame));
  if (!n) {
    logf("BUG: could not encode a frame; dropped");
    qHead = (qHead + 1) % kQueueMax;
    qCount--;
    stats.dropped++;
    return;
  }
  if (!duty.allows(radio.airtimeMs(n))) {
    it.nextTry = now + 5000;
    return;
  }
  stats.sent++;
  acked = false;
  if (send(frame, n)) {
    inflight = true;
    ackDeadline = nowMs() + WX_ACK_TIMEOUT_MS;
  } else {
    it.nextTry = nowMs() + 1000;
  }
}
#endif

// ---- incoming (relay, gateway) ------------------------------------------------------------------
#if ROLE != 1
struct Seen {
  uint8_t src;
  uint16_t lseq;
  uint8_t digest[8];
  uint64_t at;
  bool used;
};
static const int kSeenMax = 64;
static const uint64_t kSeenTtlMs = 600000;
static Seen seen[kSeenMax];
static int seenNext = 0;

static bool seenBefore(const hoplink::Frame& f, const uint8_t d[8]) {
  for (const Seen& s : seen)
    if (s.used && nowMs() - s.at < kSeenTtlMs && s.src == f.src && s.lseq == f.lseq && !memcmp(s.digest, d, 8))
      return true;
  return false;
}

static void remember(const hoplink::Frame& f, const uint8_t d[8]) {
  Seen& s = seen[seenNext];
  seenNext = (seenNext + 1) % kSeenMax;
  s = {f.src, f.lseq, {0}, nowMs(), true};
  memcpy(s.digest, d, 8);
}
#endif

#if ROLE == 3
// End-to-end repeats: the highest seq delivered per (station, boot), for the last 16 boots.
struct BootSeen {
  uint8_t station;
  uint16_t boot, top;
  uint32_t order;
  bool used;
};
static BootSeen boots[16];
static uint32_t bootOrder = 0;

static bool alreadyDelivered(const wx::DataFrame& d) {
  for (BootSeen& b : boots)
    if (b.used && b.station == d.station_id && b.boot == d.boot_id) {
      if (d.seq == b.top || uint16_t(d.seq - b.top) >= 0x8000) return true;
      b.top = d.seq;
      b.order = ++bootOrder;
      return false;
    }
  BootSeen* slot = &boots[0];  // a free slot, else the boot heard from least recently
  for (BootSeen& b : boots) {
    if (!b.used) {
      slot = &b;
      break;
    }
    if (b.order < slot->order) slot = &b;
  }
  *slot = {d.station_id, d.boot_id, d.seq, ++bootOrder, true};
  return false;
}

static void deliver(const hoplink::Frame& f, int rssi, float snr) {
  wx::Frame w;
  wx::DecodeResult r = wx::decode(f.payload, f.payloadLen, kKey, sizeof(kKey), w);
  if (r != wx::kOk || w.type != wx::kTypeData) {
    stats.rejected++;
    logf("<- %d payload REJECTED (%s)", f.src, wx::decodeResultName(r));
    return;
  }
  const wx::DataFrame& d = w.data;
  if (alreadyDelivered(d)) {
    logf("<- station %u boot %u seq %u: already have it, ignored", d.station_id, d.boot_id, d.seq);
    return;
  }
  stats.delivered++;
#if defined(CELL_URL)
  // Age 0: the reading is a few seconds old when it arrives, more only after link retries.
  celluplink::enqueue(f.payload, d.station_id, 0);
#endif
  char hex[2 * hoplink::kMaxPayload + 1];
  for (size_t i = 0; i < f.payloadLen; i++) sprintf(hex + 2 * i, "%02x", f.payload[i]);
  const wx::Reading& x = d.r;
  Serial.printf("DELIVERED {\"station\":%u,\"boot\":%u,\"seq\":%u,\"hops\":%u,\"last_hop_from\":%u,\"rssi\":%d,"
                "\"snr\":%.1f,\"temp_c\":%.2f,\"wind_ms\":%.2f,\"gust_ms\":%.2f,\"wind_dir_deg\":%u,"
                "\"rain_total_mm\":%.1f,\"flags\":%u,\"frame\":\"%s\"}\n",
                d.station_id, d.boot_id, d.seq, f.hops, f.src, rssi, snr,
                x.temp_c_x100 == wx::kNoI16 ? -999.0 : x.temp_c_x100 / 100.0,
                x.wind_ms_x100 == wx::kNoU16 ? -1.0 : x.wind_ms_x100 / 100.0,
                x.gust_ms_x100 == wx::kNoU16 ? -1.0 : x.gust_ms_x100 / 100.0, x.wind_dir_deg,
                x.rain_total_mm_x10 == wx::kNoU16 ? -1.0 : x.rain_total_mm_x10 / 10.0, x.flags, hex);
}
#endif

static void onFrame(const uint8_t* buf, size_t len, int rssi, float snr) {
  hoplink::Frame f;
  if (!hoplink::decode(buf, len, kKey, sizeof(kKey), f)) {
    stats.bad++;
    return;
  }
  if (f.dst != NODE_ID) return;
  if (f.type == hoplink::kHack) {
#if ROLE != 3
    if (inflight && f.src == NEXT_HOP && qCount && f.lseq == queue[qHead].lseq) acked = true;
#endif
    return;
  }
#if ROLE == 1
  return;  // a station does not accept packets
#else
  if (!duty.allows(radio.airtimeMs(hoplink::kHdr + hoplink::kMac))) {
    stats.refusedDuty++;
    logf("<- %d lseq %u refused: duty budget used up (sender keeps it)", f.src, f.lseq);
    return;
  }
  uint8_t d[8];
  hoplink::digest8(f.payload, f.payloadLen, d);
  if (seenBefore(f, d)) {
    stats.dupIn++;
    logf("<- %d repeat lseq %u, re-acked", f.src, f.lseq);
  } else {
    remember(f, d);
    stats.in++;
#if ROLE == 2
    if (f.hops >= hoplink::kMaxHops) {
      logf("<- %d dropped: hop limit", f.src);
    } else {
      enqueue(f.payload, f.payloadLen, f.hops + 1);
      logf("<- %d lseq %u rssi %d snr %.1f, queued (%d)", f.src, f.lseq, rssi, snr, qCount);
    }
#else
    logf("<- %d lseq %u hops %u rssi %d snr %.1f fei %ld Hz", f.src, f.lseq, f.hops, rssi, snr, (long)radio.lastFeiHz);
    deliver(f, rssi, snr);
#endif
  }
  uint8_t hack[hoplink::kHdr + hoplink::kMac];
  size_t n = hoplink::encodeHack(NODE_ID, f.src, f.lseq, kKey, sizeof(kKey), hack, sizeof(hack));
  send(hack, n);
#endif
}

// ---- station readings ---------------------------------------------------------------------------
#if ROLE == 1
static uint16_t bootId, seq = 0;

static void makeReport() {
  wx::DataFrame d;
  d.station_id = WX_STATION_ID;
  d.boot_id = bootId;
  d.seq = ++seq;
  if (seq == 1) d.r.flags |= wx::kFlagColdBoot;
  wxstation::fillReading(d);
  uint8_t payload[wx::kDataLen];
  if (wx::encodeData(d, kKey, sizeof(kKey), payload, sizeof(payload)) != wx::kDataLen) {
    logf("BUG: could not encode reading");
    return;
  }
  enqueue(payload, wx::kDataLen, 1);
  Serial.print("WXF ");
  for (uint8_t b : payload) Serial.printf("%02x", b);
  Serial.println();
  logf("reading boot %u seq %u queued%s (queue %d)", bootId, seq,
       (d.r.flags & wx::kFlagSensorFail) ? " SENSOR_FAIL" : "", qCount);
}
#endif

void setup() {
#if ROLE == 1
  // Less heat on the board, whose BME280 sits next to the ESP32 (SPI/UART/I2C clocks are unchanged).
  setCpuFrequencyMhz(80);
  btStop();
#endif
  Serial.begin(115200);
  delay(300);
  Serial.println();
#if ROLE == 1
  Serial.println("=== Hava Istasyonu baslatiliyor ===");  // tools/telem_source.py detects restarts by this banner
#endif
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);
  SPI.begin(18, 19, 23, -1);
#if ROLE == 1
  pinMode(LORA_NSS, OUTPUT);
  digitalWrite(LORA_NSS, HIGH);  // keep the radio off the bus while the lightning sensor starts
  wxstation::begin();
  bootId = uint16_t(esp_random());
#endif
#if ROLE != 3
  myLseq = uint16_t(esp_random());
#endif
#if ROLE == 3 && defined(CELL_URL)
  celluplink::Config cell;
  cell.url = CELL_URL;
  cell.apn = CELL_APN;
  cell.key = kKey;
  cell.keyLen = sizeof(kKey);
  celluplink::begin(cell);
  logf("uploading to %s over the SIM800L (APN %s)", CELL_URL, CELL_APN);
#endif
  duty.begin(HOP_FREQ_MHZ);
  radioReady = radioInit();
#if ROLE == 3
  logf("LakeLink hop node: gateway, %.3f MHz SF%d %d dBm, radio %s, duty budget %lu ms/h", HOP_FREQ_MHZ, HOP_SF,
       HOP_POWER_DBM, radioReady ? "ready" : "NOT RESPONDING", (unsigned long)duty.budgetMs);
#else
  logf("LakeLink hop node: %s -> next hop %d, %.3f MHz SF%d %d dBm, radio %s, duty budget %lu ms/h", kRoleName,
       NEXT_HOP, HOP_FREQ_MHZ, HOP_SF, HOP_POWER_DBM, radioReady ? "ready" : "NOT RESPONDING",
       (unsigned long)duty.budgetMs);
#endif
  if (!duty.budgetMs) logf("ERROR: %.3f MHz is outside every allowed band; will never transmit", HOP_FREQ_MHZ);
}

void loop() {
  esp_task_wdt_reset();
#if ROLE == 1
  wxstation::poll();
  static uint64_t nextReport = 10000;
  if (nowMs() >= nextReport) {
    nextReport = nowMs() + uint64_t(WX_INTERVAL_S) * 1000;
    makeReport();
  }
#endif
  if (radioReady) {
    uint8_t buf[hoplink::kMaxFrame + 8];
    size_t len;
    int rssi;
    float snr;
    const uint32_t crcBefore = radio.crcErrors;
    if (radio.poll(buf, sizeof(buf), len, rssi, snr)) onFrame(buf, len, rssi, snr);
    else if (radio.crcErrors != crcBefore) logf("packet with CRC error (fei %ld Hz)", (long)radio.lastFeiHz);
  }
#if ROLE != 3
  pump();
#endif
  static uint64_t lastCheck = 0, lastStatus = 0;
  if (nowMs() - lastCheck > 5000) {  // a brown-out or loose wire resets the chip to FSK standby
    lastCheck = nowMs();
    if (!radioReady || !radio.healthy()) {
      if (radioReady) {
        stats.radioErrors++;
        logf("radio lost its settings (version 0x%02X opmode 0x%02X): reset or brown-out", radio.version(),
             radio.reg(0x01));
      }
      radioReady = radioInit();
      logf("radio %s (version 0x%02X opmode 0x%02X)", radioReady ? "re-initialised" : "not responding", radio.version(), radio.reg(0x01));
    }
  }
  if (nowMs() - lastStatus > 60000) {
    lastStatus = nowMs();
    logf("status: in %lu (dup %lu), sent %lu, acked %lu, retries %lu, delivered %lu, rejected %lu, refused %lu, "
         "dropped %lu, bad %lu, crc %lu, radio errors %lu, airtime %lu/%lu ms/h"
#if ROLE != 3
         ", queue %d", (unsigned long)stats.in, (unsigned long)stats.dupIn, (unsigned long)stats.sent,
         (unsigned long)stats.acked, (unsigned long)stats.retries, (unsigned long)stats.delivered,
         (unsigned long)stats.rejected, (unsigned long)stats.refusedDuty, (unsigned long)stats.dropped,
         (unsigned long)stats.bad, (unsigned long)radio.crcErrors, (unsigned long)stats.radioErrors,
         (unsigned long)duty.used(), (unsigned long)duty.budgetMs, qCount);
#else
         , (unsigned long)stats.in, (unsigned long)stats.dupIn, (unsigned long)stats.sent,
         (unsigned long)stats.acked, (unsigned long)stats.retries, (unsigned long)stats.delivered,
         (unsigned long)stats.rejected, (unsigned long)stats.refusedDuty, (unsigned long)stats.dropped,
         (unsigned long)stats.bad, (unsigned long)radio.crcErrors, (unsigned long)stats.radioErrors,
         (unsigned long)duty.used(), (unsigned long)duty.budgetMs);
#endif
  }
  delay(2);
}
