#include "wx_station.h"

#include <SPI.h>
#include <SparkFunBME280.h>
#include <SparkFun_AS3935.h>
#include <SparkFun_Weather_Meter_Kit_Arduino_Library.h>
#include <Wire.h>
#include <esp_timer.h>
#include <math.h>

namespace wxstation {

// ---- the builder's sensor configuration (WeatherStation.ino) ---------------------------------------
#define BME280_ADDR 0x77
#ifndef AS3935_CS
#define AS3935_CS 25  // MicroMod ESP32 G1; probed 2026-11-05 (reg 0x02 = 0xC2). GPIO12 is the Ra-01's NSS.
#endif
#define AS3935_INT_PIN 17
#define AS3935_SPI_HZ 2000000
#define WDIR_PIN 35
#define WSPEED_PIN 14
#define RAIN_PIN 27
#define AS3935_TUNE_CAP 6
#define AS3935_NOISE_FLOOR 2
#define AS3935_WATCHDOG 2
#define AS3935_SPIKE_REJECT 2
#define AS3935_LIGHTNING_THRESH 1
#define SOIL_PIN 34
#define SOIL_ADC_DRY 0
#define SOIL_ADC_WET 648

static BME280 bme;
static SparkFun_AS3935 lightning;
static SFEWeatherMeterKit weatherMeterKit(WDIR_PIN, WSPEED_PIN, RAIN_PIN);
static volatile bool lightningFlag = false;
static bool as3935Ready = false, bmeReady = false, useInterrupt = true;
static void IRAM_ATTR onLightning() { lightningFlag = true; }

// Wind speed: our own counter instead of the library's. The library counts every change of the reed
// switch with no filtering, so a cup resting with its magnet at the switch chatters and reports fake
// gusts (seen 2026-11-05: lone 15-17 km/h samples between 0 and 1.2 km/h, odd edge counts).
// Here only closures count, at most one per WIND_DEBOUNCE_US (20 ms = 50 closures/s = 120 km/h), and
// the speed is averaged over the whole 3 s between samples (the WMO definition of a gust).
#define WIND_DEBOUNCE_US 20000
#define KMH_PER_CLOSURE_PER_S 2.4f  // SparkFun anemometer: one closure per second = 2.4 km/h
static volatile uint32_t windClosures = 0, lastClosureUs = 0;
static void IRAM_ATTR onWindClosure() {
  uint32_t now = micros();
  if (now - lastClosureUs >= WIND_DEBOUNCE_US) {
    windClosures++;
    lastClosureUs = now;
  }
}

static float windSum = 0, windMax = 0;  // km/h samples since the last reading
static int windN = 0;
static uint16_t lightningTotal = 0;  // strikes since boot (a new boot_id restarts it, like the rain total)
static int lightningKm = -1;
static uint64_t latestAt = 0;  // when `latest` was read; 0 = never
static struct {
  float t, h, p, windDir, rain, soilPct;
} latest;
static const uint32_t kStaleMs = 30000;  // same as telem_source.STALE_S

uint64_t nowMs() { return uint64_t(esp_timer_get_time() / 1000); }

static float soilMoisturePercent(int raw) {
  float pct = float(SOIL_ADC_DRY - raw) * 100.0f / float(SOIL_ADC_DRY - SOIL_ADC_WET);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

// Bosch's "weather monitoring" setting: 1x oversampling, no filter, one forced measurement per
// reading. The library default (normal mode, 0.5 ms standby) measures nonstop and warms the sensor.
static bool beginBme() {
  bme.setI2CAddress(BME280_ADDR);
  bool ok = bme.beginI2C();
  if (!ok) {
    bme.setI2CAddress(0x76);
    ok = bme.beginI2C();
  }
  if (ok) bme.setMode(MODE_SLEEP);
  return ok;
}

static void measureBme() {
  if (!bmeReady) return;
  bme.setMode(MODE_FORCED);              // one T/P/H conversion (~10 ms), then the sensor sleeps
  const uint64_t start = nowMs();
  delay(2);
  while (bme.isMeasuring() && nowMs() - start < 50) delay(1);
}

void begin(const Options& opt) {
  useInterrupt = opt.lightningInterrupt;
  Wire.begin();
  Wire.setClock(400000);
  bmeReady = beginBme();
  Serial.println(bmeReady ? "BME280 hazir." : "HATA: BME280 bulunamadi!");

  SPI.begin();
  if (lightning.beginSPI(AS3935_CS, AS3935_SPI_HZ)) {
    as3935Ready = true;
    lightning.setIndoorOutdoor(OUTDOOR);
    lightning.tuneCap(AS3935_TUNE_CAP);
    lightning.setNoiseLevel(AS3935_NOISE_FLOOR);
    lightning.watchdogThreshold(AS3935_WATCHDOG);
    lightning.spikeRejection(AS3935_SPIKE_REJECT);
    lightning.lightningThreshold(AS3935_LIGHTNING_THRESH);
    lightning.maskDisturber(false);
    if (useInterrupt) {
      pinMode(AS3935_INT_PIN, INPUT);
      attachInterrupt(digitalPinToInterrupt(AS3935_INT_PIN), onLightning, RISING);
      Serial.println("AS3935 hazir (SPI, CS=GPIO" + String(AS3935_CS) + ").");
    } else {
      Serial.println("AS3935 hazir (SPI, CS=GPIO" + String(AS3935_CS) + ", INT pini kullanilmiyor - polling).");
    }
  } else {
    Serial.println("UYARI: AS3935 SPI'da baslatilamadi (CS=GPIO" + String(AS3935_CS) + ").");
  }

  weatherMeterKit.begin();
  detachInterrupt(digitalPinToInterrupt(WSPEED_PIN));  // replace the library's unfiltered counter
  attachInterrupt(digitalPinToInterrupt(WSPEED_PIN), onWindClosure, FALLING);
  Serial.println("Weather Meter Kit hazir (ruzgar/yagmur).");
  pinMode(SOIL_PIN, INPUT);
  analogReadResolution(12);
  Serial.println("Toprak nemi sensoru hazir (analog).");
}

static void handleLightning() {
  int intVal = lightning.readInterruptReg();  // reading clears it; 0 = nothing happened
  if (intVal == LIGHTNING) {
    byte dist = lightning.distanceToStorm();
    long energy = lightning.lightningEnergy();
    if (lightningTotal < 0xFFFE) lightningTotal++;  // 0xFFFF is the "no value" sentinel
    lightningKm = dist;
    Serial.printf("EVENT {\"type\":\"lightning\",\"distance_km\":%u,\"energy\":%ld}\n", dist, energy);
  } else if (intVal == DISTURBER_DETECT) {
    Serial.println("EVENT {\"type\":\"disturber\",\"distance_km\":null,\"energy\":null}");
  } else if (intVal == NOISE_TO_HIGH) {
    Serial.println("EVENT {\"type\":\"noise\",\"distance_km\":null,\"energy\":null}");
  }
}

void poll() {
  if (as3935Ready && useInterrupt && lightningFlag) {
    lightningFlag = false;
    delay(2);
    handleLightning();
  }

  static uint64_t lastRead = 0;
  if (lastRead && nowMs() - lastRead < 3000) return;
  lastRead = nowMs();
  if (as3935Ready && !useInterrupt) handleLightning();  // polled every 3 s; the register holds the event
  if (!bmeReady) {
    static uint64_t lastBmeTry = 0;
    if (nowMs() - lastBmeTry > 60000) {  // a loose Qwiic cable may come back
      lastBmeTry = nowMs();
      if ((bmeReady = beginBme())) Serial.println("BME280 hazir.");
    }
  }
  measureBme();
  float t = bme.readTempC();
  float h = bme.readFloatHumidity();
  float p = bme.readFloatPressure() / 100.0f;
  float alt = bme.readFloatAltitudeMeters();
  static uint32_t prevClosures = 0;
  static uint64_t prevWindAt = 0;
  const uint32_t closures = windClosures;
  float windSpeed = NAN;  // km/h over the time since the previous sample (~3 s); none for the first sample
  if (prevWindAt && nowMs() > prevWindAt)
    windSpeed = (closures - prevClosures) * KMH_PER_CLOSURE_PER_S * 1000.0f / float(nowMs() - prevWindAt);
  prevClosures = closures;
  prevWindAt = nowMs();
  float windDir = weatherMeterKit.getWindDirection();  // degrees, -1 if the vane reading is invalid
  float rain = weatherMeterKit.getTotalRainfall();     // mm since boot
  int soilRaw = analogRead(SOIL_PIN);
  float soilPct = soilMoisturePercent(soilRaw);
  latest = {t, h, p, windDir, rain, soilPct};
  latestAt = nowMs();
  if (!isnan(windSpeed) && windSpeed >= 0) {
    windSum += windSpeed;
    windMax = max(windMax, windSpeed);
    windN++;
  }
  Serial.printf("TELEM {\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f,\"altitude\":%.2f,"
                "\"wind_speed\":%.2f,\"wind_dir\":%.1f,\"rain_mm\":%.2f,\"soil_pct\":%.1f,\"soil_adc\":%d}\n",
                t, h, p, alt, isnan(windSpeed) ? 0.0f : windSpeed, windDir, rain, soilPct, soilRaw);  // JSON has no nan
}

// Same scaling and plausibility ranges as tools/telem_source.take(); an out-of-range value is left out.
static bool scaled(float v, float factor, float lo, float hi, int32_t& out) {
  if (isnan(v) || v < lo || v > hi) return false;
  out = lroundf(v * factor);
  return true;
}

void fillReading(wx::DataFrame& d) {
  const bool fresh = latestAt && nowMs() - latestAt <= kStaleMs;
  if (!fresh) {
    d.r.flags |= wx::kFlagSensorFail;
  } else {
    int32_t v;
    if (!bmeReady) d.r.flags |= wx::kFlagSensorFail;
    if (bmeReady && scaled(latest.t, 100, -60, 80, v)) d.r.temp_c_x100 = int16_t(v);
    if (bmeReady && scaled(latest.h, 100, 0, 100, v)) d.r.rh_x100 = uint16_t(v);
    if (bmeReady && scaled(latest.p, 10, 300, 1100, v)) d.r.press_hpa_x10 = uint16_t(v);
    if (scaled(latest.rain, 10, 0, 6553, v)) d.r.rain_total_mm_x10 = uint16_t(v);
    if (scaled(latest.soilPct, 10, 0, 100, v)) d.r.soil_pct_x10 = uint16_t(v);
    if (!isnan(latest.windDir) && latest.windDir >= 0 && latest.windDir < 360)
      d.r.wind_dir_deg = uint16_t(lroundf(latest.windDir) % 360);
    if (windN) {
      d.r.wind_ms_x100 = uint16_t(lroundf(windSum / windN * (100.0f / 3.6f)));
      d.r.gust_ms_x100 = uint16_t(lroundf(windMax * (100.0f / 3.6f)));
    }
    d.r.lightning_total = lightningTotal;
    if (lightningKm >= 0) d.r.lightning_km = uint8_t(min(lightningKm, 254));
  }
  windSum = windMax = 0;
  windN = 0;
}

}  // namespace wxstation
