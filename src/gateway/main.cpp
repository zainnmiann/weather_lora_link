// Gateway: LoRa-E5 mini in continuous receive -> verify frame -> ACK -> publish JSON over MQTT.
//
// Topics
//   weather/<station_id>/data   one JSON per accepted reading (retained = latest value)
//   weather/gateway/status      retained; {"online":false} is the MQTT last-will
#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <time.h>

#include "e5_radio.h"
#include "wx_config.h"
#include "wx_frame.h"

static const uint8_t kKey[] = WX_HMAC_KEY;
static const char kStatusTopic[] = "weather/gateway/status";
static const uint32_t kStatusPeriodMs = 60 * 1000;
static const uint32_t kRadioRearmPeriodMs = 30 * 60 * 1000;  // re-init the E5 periodically, in case it hung
static const uint32_t kMqttRetryMs = 5000;
static const time_t kValidEpoch = 1700000000;  // anything earlier means NTP has not synced yet

static E5Radio radio(Serial2, E5_RX_PIN, E5_TX_PIN, E5_RST_PIN);
static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

// Duplicate / replay tracking per station.
struct StationState {
  bool seen = false;
  uint16_t boot_id = 0;
  uint16_t seq = 0;
};
static StationState stations[256];

// Accepted readings waiting for MQTT (kept while WiFi/broker is down; oldest dropped when full).
struct Pending {
  wx::DataFrame f;
  int rssi;
  float snr;
  time_t rxTime;  // 0 if NTP had not synced at reception
};
static const size_t kQueueLen = 32;
static Pending queue[kQueueLen];
static size_t qHead = 0, qCount = 0;

static struct {
  uint32_t accepted, duplicate, stale, badFrame, queueDropped, radioErrors;
} stats;

static bool radioReady = false;
static uint32_t lastRadioInit = 0, lastStatus = 0, lastMqttAttempt = 0;

static void enqueue(const Pending& p) {
  if (qCount == kQueueLen) {
    qHead = (qHead + 1) % kQueueLen;
    qCount--;
    stats.queueDropped++;
  }
  queue[(qHead + qCount) % kQueueLen] = p;
  qCount++;
}

static bool initRadio() {
  lastRadioInit = millis();
  radioReady = radio.configure() && radio.startReceive();
  if (!radioReady) stats.radioErrors++;
  Serial.printf("radio %s\n", radioReady ? "listening" : "init FAILED");
  return radioReady;
}

// Appends ,"key":value with the given scale, skipping "not measured" sentinels.
static void addU16(char*& p, char* end, const char* key, uint16_t v, float scale, int decimals) {
  if (v == wx::kNoU16) return;
  p += snprintf(p, end - p, ",\"%s\":%.*f", key, decimals, v / scale);
}

static size_t toJson(const Pending& p, char* out, size_t cap) {
  const wx::Reading& r = p.f.r;
  char* s = out;
  char* end = out + cap;
  s += snprintf(s, end - s, "{\"station\":%u,\"boot\":%u,\"seq\":%u", p.f.station_id, p.f.boot_id, p.f.seq);
  if (p.rxTime) {
    struct tm tm;
    gmtime_r(&p.rxTime, &tm);
    s += strftime(s, end - s, ",\"time\":\"%Y-%m-%dT%H:%M:%SZ\"", &tm);
  }
  if (r.temp_c_x100 != wx::kNoI16) s += snprintf(s, end - s, ",\"temp_c\":%.2f", r.temp_c_x100 / 100.0f);
  addU16(s, end, "rh_pct", r.rh_x100, 100, 2);
  addU16(s, end, "press_hpa", r.press_hpa_x10, 10, 1);
  addU16(s, end, "wind_ms", r.wind_ms_x100, 100, 2);
  addU16(s, end, "gust_ms", r.gust_ms_x100, 100, 2);
  addU16(s, end, "wind_dir_deg", r.wind_dir_deg, 1, 0);
  addU16(s, end, "rain_total_mm", r.rain_total_mm_x10, 10, 1);
  addU16(s, end, "batt_v", r.batt_mv, 1000, 3);
  addU16(s, end, "soil_pct", r.soil_pct_x10, 10, 1);
  addU16(s, end, "lightning_total", r.lightning_total, 1, 0);
  if (r.lightning_km != wx::kNoU8) s += snprintf(s, end - s, ",\"lightning_km\":%u", r.lightning_km);
  s += snprintf(s, end - s,
                ",\"sensor_fail\":%s,\"fake\":%s,\"low_batt\":%s,\"cold_boot\":%s,\"rssi\":%d,\"snr\":%.1f}",
                (r.flags & wx::kFlagSensorFail) ? "true" : "false", (r.flags & wx::kFlagFakeData) ? "true" : "false",
                (r.flags & wx::kFlagLowBattery) ? "true" : "false", (r.flags & wx::kFlagColdBoot) ? "true" : "false",
                p.rssi, p.snr);
  return s - out;
}

static void handlePacket(const uint8_t* buf, size_t len, const e5::RxMeta& meta) {
  wx::Frame f;
  wx::DecodeResult res = wx::decode(buf, len, kKey, sizeof(kKey), f);
  if (res != wx::kOk || f.type != wx::kTypeData) {
    stats.badFrame++;
    Serial.printf("rejected %u-byte packet: %s (rssi=%d)\n", unsigned(len),
                  res == wx::kOk ? "not a data frame" : wx::decodeResultName(res), meta.rssi);
    return;  // never ACK what we cannot authenticate
  }
  const wx::DataFrame& d = f.data;
  StationState& st = stations[d.station_id];
  bool sameBoot = st.seen && st.boot_id == d.boot_id;
  bool duplicate = sameBoot && d.seq == st.seq;           // our previous ACK was lost: re-ACK only
  bool stale = sameBoot && int16_t(d.seq - st.seq) < 0;   // older than what we already have
  if (stale) {
    stats.stale++;
    Serial.printf("station %u: stale seq %u (have %u), dropped\n", d.station_id, d.seq, st.seq);
    return;
  }

  wx::AckFrame ack;
  ack.station_id = d.station_id;
  ack.boot_id = d.boot_id;
  ack.seq = d.seq;
  ack.status = duplicate ? 1 : 0;
  uint8_t ackBuf[wx::kAckLen];
  size_t ackLen = wx::encodeAck(ack, kKey, sizeof(kKey), ackBuf, sizeof(ackBuf));
  if (!radio.transmit(ackBuf, ackLen)) stats.radioErrors++;
  if (!radio.startReceive()) initRadio();

  if (duplicate) {
    stats.duplicate++;
    Serial.printf("station %u: duplicate seq %u, re-acked\n", d.station_id, d.seq);
    return;
  }
  st.seen = true;
  st.boot_id = d.boot_id;
  st.seq = d.seq;
  stats.accepted++;
  time_t now = time(nullptr);
  enqueue({d, meta.rssi, meta.snr, now > kValidEpoch ? now : 0});
  Serial.printf("station %u: seq %u accepted (rssi=%d snr=%.1f)\n", d.station_id, d.seq, meta.rssi, meta.snr);
}

static void publishStatus() {
  char json[320];
  snprintf(json, sizeof(json),
           "{\"online\":true,\"uptime_s\":%lu,\"radio_ok\":%s,\"accepted\":%lu,\"duplicate\":%lu,\"stale\":%lu,"
           "\"bad_frame\":%lu,\"queue\":%u,\"queue_dropped\":%lu,\"radio_errors\":%lu,\"wifi_rssi\":%d}",
           (unsigned long)(millis() / 1000), radioReady ? "true" : "false", (unsigned long)stats.accepted,
           (unsigned long)stats.duplicate, (unsigned long)stats.stale, (unsigned long)stats.badFrame,
           unsigned(qCount), (unsigned long)stats.queueDropped, (unsigned long)stats.radioErrors, WiFi.RSSI());
  mqtt.publish(kStatusTopic, json, true);
}

static void maintainMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (millis() - lastMqttAttempt < kMqttRetryMs) return;
  lastMqttAttempt = millis();
  char clientId[32];
  snprintf(clientId, sizeof(clientId), "wx-gateway-%06lX", (unsigned long)(ESP.getEfuseMac() & 0xFFFFFF));
  const char* user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
  const char* pass = strlen(MQTT_USER) ? MQTT_PASSWORD : nullptr;
  if (mqtt.connect(clientId, user, pass, kStatusTopic, 1, true, "{\"online\":false}")) {
    Serial.println("mqtt connected");
    publishStatus();
  } else {
    Serial.printf("mqtt connect failed, state=%d\n", mqtt.state());
  }
}

static void flushQueue() {
  char topic[32], json[384];
  while (qCount && mqtt.connected()) {
    const Pending& p = queue[qHead];
    snprintf(topic, sizeof(topic), "weather/%u/data", p.f.station_id);
    toJson(p, json, sizeof(json));
    if (!mqtt.publish(topic, json, true)) return;  // keep it queued, retry next loop
    Serial.printf("%s %s\n", topic, json);
    qHead = (qHead + 1) % kQueueLen;
    qCount--;
  }
}

void setup() {
  Serial.begin(115200);
  radio.setLog(&Serial);
  radio.begin(E5_BAUD);
  initRadio();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
}

void loop() {
  if (radioReady) {
    uint8_t buf[wx::kMaxFrameLen];
    size_t len;
    e5::RxMeta meta;
    if (radio.poll(buf, sizeof(buf), len, meta)) handlePacket(buf, len, meta);
  }
  if (millis() - lastRadioInit > (radioReady ? kRadioRearmPeriodMs : 10000)) initRadio();

  maintainMqtt();
  mqtt.loop();
  flushQueue();
  if (mqtt.connected() && millis() - lastStatus > kStatusPeriodMs) {
    lastStatus = millis();
    publishStatus();
  }
  delay(1);
}
