// Weather station node: wake -> read sensor -> send one frame over LoRa (with ACK + retries)
// -> put the Wio-E5 and the ESP32 into deep sleep. All work happens in setup(); loop() is unused.
#include <Arduino.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <sys/time.h>

#include "e5_radio.h"
#include "sensor.h"
#include "wx_config.h"
#include "wx_frame.h"

#ifndef WX_STATION_ID
#error "WX_STATION_ID must be set in platformio.ini"
#endif

static const uint8_t kKey[] = WX_HMAC_KEY;

// Survive deep sleep, reset on power loss.
RTC_DATA_ATTR static bool rtcValid = false;
RTC_DATA_ATTR static uint16_t rtcBootId;
RTC_DATA_ATTR static uint16_t rtcSeq;
RTC_DATA_ATTR static uint32_t rtcAcked;
RTC_DATA_ATTR static uint32_t rtcUnacked;
RTC_DATA_ATTR static int64_t rtcNextReportUs;  // system time of the next scheduled report

static E5Radio radio(Serial2, E5_RX_PIN, E5_TX_PIN, E5_RST_PIN);

static uint16_t readBatteryMv() {
#if BATT_ADC_PIN >= 0
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(BATT_ADC_PIN);
  return uint16_t(sum / 8 * BATT_DIVIDER);
#else
  return wx::kNoU16;
#endif
}

// System time keeps running through deep sleep (RTC timer), so it schedules the next report even
// when the sensor code wakes us early (e.g. to count a rain-gauge tip).
static int64_t nowUs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return int64_t(tv.tv_sec) * 1000000 + tv.tv_usec;
}

static void sleepUntilNextReport() {
  sensorBeforeSleep();
  int64_t remainingUs = rtcNextReportUs - nowUs();
  if (remainingUs < 1000000) remainingUs = 1000000;
  Serial.flush();
  esp_sleep_enable_timer_wakeup(uint64_t(remainingUs));
  esp_deep_sleep_start();
}

static bool waitForAck(uint16_t seq) {
  if (!radio.startReceive()) return false;
  uint32_t deadline = millis() + WX_ACK_TIMEOUT_MS;
  uint8_t buf[wx::kMaxFrameLen];
  size_t len;
  e5::RxMeta meta;
  while (int32_t(deadline - millis()) > 0) {
    if (radio.poll(buf, sizeof(buf), len, meta)) {
      wx::Frame f;
      wx::DecodeResult res = wx::decode(buf, len, kKey, sizeof(kKey), f);
      if (res == wx::kOk && f.type == wx::kTypeAck && f.ack.station_id == WX_STATION_ID &&
          f.ack.boot_id == rtcBootId && f.ack.seq == seq) {
        Serial.printf("ack seq=%u status=%u rssi=%d snr=%.1f\n", seq, f.ack.status, meta.rssi, meta.snr);
        return true;
      }
      Serial.printf("ignored packet (%s, %u bytes)\n", wx::decodeResultName(res), unsigned(len));
    }
    delay(2);
  }
  return false;
}

static bool sendWithRetries(const uint8_t* frame, size_t len, uint16_t seq) {
  for (int attempt = 1; attempt <= WX_MAX_TX_ATTEMPTS; attempt++) {
    if (radio.transmit(frame, len) && waitForAck(seq)) return true;
    Serial.printf("attempt %d/%d: no ack\n", attempt, WX_MAX_TX_ATTEMPTS);
    if (attempt < WX_MAX_TX_ATTEMPTS) delay(WX_RETRY_BACKOFF_MIN + esp_random() % (WX_RETRY_BACKOFF_MAX - WX_RETRY_BACKOFF_MIN));
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  if (rtcValid && sensorHandleWake()) sleepUntilNextReport();  // event-only wake, nothing to report yet
  bool coldBoot = !rtcValid;
  if (coldBoot) {
    rtcBootId = uint16_t(esp_random());
    rtcSeq = 0;
    rtcAcked = rtcUnacked = 0;
    rtcValid = true;
  }

  wx::DataFrame f;
  f.station_id = WX_STATION_ID;
  f.boot_id = rtcBootId;
  f.seq = ++rtcSeq;
  if (!(sensorBegin() && sensorRead(f.r))) f.r.flags |= wx::kFlagSensorFail;
  f.r.batt_mv = readBatteryMv();
  bool lowBatt = f.r.batt_mv != wx::kNoU16 && f.r.batt_mv < WX_LOW_BATT_MV;
  if (lowBatt) f.r.flags |= wx::kFlagLowBattery;
  if (coldBoot) f.r.flags |= wx::kFlagColdBoot;

  uint8_t frame[wx::kDataLen];
  size_t len = wx::encodeData(f, kKey, sizeof(kKey), frame, sizeof(frame));

  radio.setLog(&Serial);
  radio.begin(E5_BAUD);
  bool acked = radio.configure() && sendWithRetries(frame, len, f.seq);
  acked ? rtcAcked++ : rtcUnacked++;
  radio.sleep();

  uint64_t intervalS = WX_INTERVAL_S * (lowBatt ? WX_LOW_BATT_INTERVAL_FACTOR : 1);
  Serial.printf("boot=%04X seq=%u %s (acked %lu, unacked %lu), sleeping %llus\n", rtcBootId, f.seq,
                acked ? "delivered" : "NOT delivered", (unsigned long)rtcAcked, (unsigned long)rtcUnacked,
                (unsigned long long)intervalS);
  rtcNextReportUs = nowUs() + int64_t(intervalS) * 1000000;
  sleepUntilNextReport();
}

void loop() {}
