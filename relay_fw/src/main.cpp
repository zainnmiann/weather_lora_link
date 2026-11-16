// LakeLink relay: runs on the LoRa-E5 mini's STM32WLE5JC. Receives FWD frames addressed to NODE_ID,
// acknowledges them (HACK), queues them, and forwards them to NEXT_HOP with retries and backoff.
// Same protocol and rules as tools/e5_node.py --role relay:
//   - every hop is acknowledged; a repeat (same sender, link seq, payload digest) is re-acked, forwarded once
//   - store-and-forward: 3 quick retries, then waits of 5, 10, 20, 40, 60 s; the queue is never dropped
//     except the oldest waiting packet when full
//   - EU868 duty-cycle budget per sub-band (90 % of the legal limit), also for acknowledgements: with no
//     airtime left the relay refuses new packets (no ack) so the sender keeps them
//   - hop limit 8; watchdog resets the chip if the main loop ever hangs
// Not yet implemented (see README): queue persistence in flash (a power loss loses queued packets) and
// scheduled wake-up (the radio listens continuously, about 6-7 mA).
#include <Arduino.h>
#include <IWatchdog.h>
#include <RadioLib.h>

#include "link.h"
#include "wx_config.h"  // WX_HMAC_KEY (from include/secrets.h or the placeholder)

#ifndef NODE_ID
#error "NODE_ID must be set"
#endif
#ifndef NEXT_HOP
#error "NEXT_HOP must be set"
#endif

static const uint8_t kKey[] = WX_HMAC_KEY;

// ---- radio ------------------------------------------------------------------------------------
STM32WLx radio = new STM32WLx_Module();

// Antenna switch on the LoRa-E5 mini: PA4/PA5. Which TX path the Wio-E5-LE uses is NOT YET VERIFIED.
static const uint32_t kRfSwitchPins[] = {PA4, PA5, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC};
static const Module::RfSwitchMode_t kRfSwitchTable[] = {
    {STM32WLx::MODE_IDLE, {LOW, LOW}},
    {STM32WLx::MODE_RX, {HIGH, LOW}},
#if RFSW_USE_LP
    {STM32WLx::MODE_TX_LP, {HIGH, HIGH}},
#else
    {STM32WLx::MODE_TX_HP, {LOW, HIGH}},
#endif
    END_OF_MODE_TABLE,
};

static volatile bool rxFlag = false;
static void onRadioIrq() { rxFlag = true; }

// ---- timing / limits --------------------------------------------------------------------------
static const uint32_t kAckTimeoutMs = 2500;
static const uint8_t kFastRetries = 3;
static const uint16_t kBackoffS[] = {5, 10, 20, 40, 60};
static const uint32_t kSeenTtlMs = 600000;

// ---- duty cycle: 60 one-minute buckets ---------------------------------------------------------
struct DutyCycle {
  uint32_t budgetMs = 0;
  uint32_t minute[60] = {0};
  uint32_t usedMs[60] = {0};
  void begin(float freqMhz) {
    struct Band { float lo, hi, duty; };
    static const Band bands[] = {{863.0f, 865.0f, 0.001f}, {865.0f, 868.0f, 0.01f}, {868.0f, 868.6f, 0.01f},
                                 {868.7f, 869.2f, 0.001f}, {869.4f, 869.65f, 0.10f}, {869.7f, 870.0f, 0.01f}};
    for (const Band& b : bands) {
      if (b.lo <= freqMhz - 0.0625f && freqMhz + 0.0625f <= b.hi) budgetMs = uint32_t(3600000.0f * b.duty * 0.9f);
    }
  }
  uint32_t used() {
    uint32_t now = millis() / 60000, sum = 0;
    for (int i = 0; i < 60; i++)
      if (now - minute[i] < 60) sum += usedMs[i];
    return sum;
  }
  bool allows(uint32_t ms) { return budgetMs && used() + ms <= budgetMs; }
  void record(uint32_t ms) {
    uint32_t now = millis() / 60000;
    int i = now % 60;
    if (minute[i] != now) {
      minute[i] = now;
      usedMs[i] = 0;
    }
    usedMs[i] += ms;
  }
};
static DutyCycle duty;

// ---- queue and duplicate memory ----------------------------------------------------------------
struct Item {
  uint8_t payload[hoplink::kMaxPayload];
  uint8_t len, hops, attempts;
  uint16_t lseq;
  uint32_t nextTry;
};
static const int kQueueMax = 48;
static Item queue[kQueueMax];
static int qHead = 0, qCount = 0;
static uint16_t myLseq;

struct Seen {
  uint8_t src;
  uint16_t lseq;
  uint8_t digest[8];
  uint32_t at;
  bool used;
};
static const int kSeenMax = 64;
static Seen seen[kSeenMax];
static int seenNext = 0;

static bool inflight = false;
static bool acked = false;
static uint32_t ackDeadline = 0;

struct Stats {
  uint32_t fwdIn, dupIn, sent, ackedOut, retries, refusedDuty, dropped, badFrames, radioErrors;
} stats;

static void logf(const char* fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(millis() / 1000);
  Serial.print("s [relay ");
  Serial.print(NODE_ID);
  Serial.print("] ");
  Serial.println(buf);
}

static uint32_t airMs(size_t len) { return uint32_t(radio.getTimeOnAir(len) / 1000) + 1; }

static void startRx() {
  int st = radio.startReceive();
  if (st != RADIOLIB_ERR_NONE) {
    stats.radioErrors++;
    logf("startReceive failed (%d)", st);
  }
}

static bool send(const uint8_t* frame, size_t len) {
  int st = radio.transmit(frame, len);
  duty.record(airMs(len));
  startRx();
  if (st != RADIOLIB_ERR_NONE) {
    stats.radioErrors++;
    logf("transmit failed (%d)", st);
    return false;
  }
  return true;
}

static bool seenBefore(const hoplink::Frame& f, const uint8_t d[8]) {
  uint32_t now = millis();
  for (const Seen& s : seen)
    if (s.used && now - s.at < kSeenTtlMs && s.src == f.src && s.lseq == f.lseq && memcmp(s.digest, d, 8) == 0)
      return true;
  return false;
}

static void remember(const hoplink::Frame& f, const uint8_t d[8]) {
  Seen& s = seen[seenNext];
  seenNext = (seenNext + 1) % kSeenMax;
  s.src = f.src;
  s.lseq = f.lseq;
  memcpy(s.digest, d, 8);
  s.at = millis();
  s.used = true;
}

static void enqueue(const uint8_t* payload, size_t len, uint8_t hops) {
  if (qCount == kQueueMax) {
    // drop the oldest packet that is not currently in flight
    int victim = (inflight && qCount > 1) ? 1 : 0;
    for (int i = victim; i < qCount - 1; i++) queue[(qHead + i) % kQueueMax] = queue[(qHead + i + 1) % kQueueMax];
    qCount--;
    stats.dropped++;
    logf("queue full, dropped oldest waiting packet");
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

static void onFrame(const uint8_t* buf, size_t len, float rssi, float snr) {
  hoplink::Frame f;
  if (!hoplink::decode(buf, len, kKey, sizeof(kKey), f)) {
    stats.badFrames++;
    return;
  }
  if (f.dst != NODE_ID) return;
  if (f.type == hoplink::kHack) {
    if (inflight && f.src == NEXT_HOP && qCount && f.lseq == queue[qHead].lseq) acked = true;
    return;
  }
  // FWD addressed to us
  if (!duty.allows(airMs(hoplink::kHdr + hoplink::kMac))) {
    stats.refusedDuty++;
    logf("<- %u lseq %u refused: duty budget used up (sender keeps it)", f.src, f.lseq);
    return;
  }
  uint8_t d[8];
  hoplink::digest8(f.payload, f.payloadLen, d);
  if (seenBefore(f, d)) {
    stats.dupIn++;
    logf("<- %u repeat lseq %u, re-acked", f.src, f.lseq);
  } else {
    remember(f, d);
    stats.fwdIn++;
    if (f.hops >= hoplink::kMaxHops) {
      logf("<- %u dropped: hop limit", f.src);
    } else {
      enqueue(f.payload, f.payloadLen, f.hops + 1);
      logf("<- %u lseq %u rssi=%.0f snr=%.1f, queued (%d)", f.src, f.lseq, rssi, snr, qCount);
    }
  }
  uint8_t hack[hoplink::kHdr + hoplink::kMac];
  size_t n = hoplink::encodeHack(NODE_ID, f.src, f.lseq, kKey, sizeof(kKey), hack, sizeof(hack));
  send(hack, n);
}

static void pump() {
  uint32_t now = millis();
  if (inflight) {
    if (acked) {
      inflight = acked = false;
      Item& it = queue[qHead];
      logf("-> %u ok (lseq %u, queue %d)", NEXT_HOP, it.lseq, qCount - 1);
      qHead = (qHead + 1) % kQueueMax;
      qCount--;
      stats.ackedOut++;
    } else if (int32_t(now - ackDeadline) >= 0) {
      inflight = false;
      Item& it = queue[qHead];
      it.attempts++;
      stats.retries++;
      if (it.attempts < kFastRetries) {
        it.nextTry = now + 300 + random(700);
      } else {
        uint8_t step = it.attempts - kFastRetries;
        if (step >= sizeof(kBackoffS) / sizeof(kBackoffS[0])) step = sizeof(kBackoffS) / sizeof(kBackoffS[0]) - 1;
        uint32_t wait = uint32_t(kBackoffS[step]) * (700 + random(600));  // ms, +-30 %
        it.nextTry = now + wait;
        logf("-> %u unreachable after %u tries; kept, retry in %lus (queue %d)", NEXT_HOP, it.attempts,
             (unsigned long)(wait / 1000), qCount);
      }
    }
    return;
  }
  if (!qCount) return;
  Item& it = queue[qHead];
  if (it.nextTry && int32_t(now - it.nextTry) < 0) return;
  uint8_t frame[hoplink::kMaxFrame];
  size_t n = hoplink::encodeFwd(NODE_ID, NEXT_HOP, it.lseq, it.hops, it.payload, it.len, kKey, sizeof(kKey), frame,
                             sizeof(frame));
  if (!duty.allows(airMs(n))) return;  // wait for airtime
  acked = false;
  inflight = true;
  ackDeadline = millis() + kAckTimeoutMs;
  stats.sent++;
  send(frame, n);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  logf("LakeLink relay starting: node %d -> next hop %d, %.3f MHz SF%d %d dBm, TX path %s", NODE_ID, NEXT_HOP,
       RADIO_FREQ_MHZ, RADIO_SF, RADIO_POWER_DBM, RFSW_USE_LP ? "LP" : "HP");
  duty.begin(RADIO_FREQ_MHZ);
  if (!duty.budgetMs) {
    logf("FATAL: %.3f MHz is not inside an EU868 sub-band; refusing to transmit", RADIO_FREQ_MHZ);
    while (true) delay(1000);
  }
  radio.setRfSwitchTable(kRfSwitchPins, kRfSwitchTable);
  // 125 kHz, CR 4/5, private sync word 0x12 (matches the AT firmware's NET OFF), preamble 12, TCXO 1.7 V
  int st = radio.begin(RADIO_FREQ_MHZ, 125.0, RADIO_SF, 5, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, RADIO_POWER_DBM, 12, 1.7);
  if (st != RADIOLIB_ERR_NONE) {
    logf("radio init failed (%d); retrying without TCXO", st);
    st = radio.begin(RADIO_FREQ_MHZ, 125.0, RADIO_SF, 5, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, RADIO_POWER_DBM, 12, 0);
  }
  if (st != RADIOLIB_ERR_NONE) {
    logf("FATAL: radio init failed (%d)", st);
    while (true) delay(1000);
  }
  radio.setCRC(2);
  radio.setDio1Action(onRadioIrq);
  randomSeed(radio.randomByte() << 8 | radio.randomByte());
  myLseq = random(65536);
  startRx();
  IWatchdog.begin(8000000);  // 8 s: resets the chip if the loop ever hangs
  logf("listening; duty budget %lu ms/h", (unsigned long)duty.budgetMs);
}

void loop() {
  IWatchdog.reload();
  if (rxFlag) {
    rxFlag = false;
    uint8_t buf[hoplink::kMaxFrame + 8];
    size_t len = radio.getPacketLength();
    if (len > sizeof(buf)) len = sizeof(buf);
    int st = radio.readData(buf, len);
    float rssi = radio.getRSSI(), snr = radio.getSNR();
    startRx();
    if (st == RADIOLIB_ERR_NONE) onFrame(buf, len, rssi, snr);
  }
  pump();
  static uint32_t lastStatus = 0;
  if (millis() - lastStatus > 60000) {
    lastStatus = millis();
    logf("status: queue %d, in %lu (dup %lu), out %lu, retries %lu, refused %lu, dropped %lu, bad %lu, radio errors %lu, airtime %lu/%lu ms/h",
         qCount, (unsigned long)stats.fwdIn, (unsigned long)stats.dupIn, (unsigned long)stats.ackedOut,
         (unsigned long)stats.retries, (unsigned long)stats.refusedDuty, (unsigned long)stats.dropped,
         (unsigned long)stats.badFrames, (unsigned long)stats.radioErrors, (unsigned long)duty.used(),
         (unsigned long)duty.budgetMs);
  }
}
