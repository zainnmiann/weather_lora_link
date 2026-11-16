// Sensors of the SparkFun MicroMod Weather Carrier Board with the MicroMod ESP32 processor:
// BME280 (I2C 0x77), AS3935 lightning detector (SPI, polled), Weather Meter Kit (anemometer,
// wind vane, tipping-bucket rain gauge) and an analog soil moisture probe.
//
// Default pins match the station builder's own WeatherStation.ino (recovered from the old Pi card):
// wind speed 14, vane 35, rain 27, soil 34, AS3935 CS 12. SPI/I2C follow SparkFun's MicroMod ESP32
// map (SPI 18/19/23, I2C 21/22).
#if WX_SENSOR_MICROMOD

#include <Arduino.h>
#include <SPI.h>
#include <SparkFunBME280.h>
#include <SparkFun_AS3935.h>
#include <Wire.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "sensor.h"

#ifndef PIN_WIND_SPEED
#define PIN_WIND_SPEED 14  // D0, anemometer reed switch to GND
#endif
#ifndef PIN_RAIN
#define PIN_RAIN 27  // D1, rain-gauge reed switch to GND; must be an RTC GPIO (deep-sleep wake)
#endif
#ifndef PIN_WIND_DIR
#define PIN_WIND_DIR 35  // A1, vane resistor network with 10 kOhm pull-up to 3.3 V
#endif
#ifndef PIN_SOIL
#define PIN_SOIL 34  // A0
#endif
#ifndef PIN_SOIL_POWER
#define PIN_SOIL_POWER -1  // the builder powers the probe permanently; set to 15 (G0) to switch it
#endif
#ifndef PIN_LIGHTNING_CS
#define PIN_LIGHTNING_CS 12  // as wired by the builder (SparkFun's map says G1 = 25)
#endif
#ifndef PIN_SPI_SCK
#define PIN_SPI_SCK 18
#endif
#ifndef PIN_SPI_CIPO
#define PIN_SPI_CIPO 19
#endif
#ifndef PIN_SPI_COPI
#define PIN_SPI_COPI 23
#endif
#ifndef PIN_I2C_SDA
#define PIN_I2C_SDA 21
#endif
#ifndef PIN_I2C_SCL
#define PIN_I2C_SCL 22
#endif

#ifndef WX_WIND_SAMPLE_S
#define WX_WIND_SAMPLE_S 5  // anemometer is sampled for this long on every report
#endif
// Soil probe calibration in raw 12-bit ADC counts, as measured by the builder: in air / in water.
#ifndef SOIL_ADC_DRY
#define SOIL_ADC_DRY 0
#endif
#ifndef SOIL_ADC_WET
#define SOIL_ADC_WET 648
#endif

// Weather Meter Kit datasheet: 1 switch closure per second = 2.4 km/h; 1 bucket tip = 0.2794 mm.
static constexpr float kMsPerClickPerS = 2.4f / 3.6f;
static constexpr float kMmPerTip = 0.2794f;
static constexpr uint32_t kWindDebounceUs = 5000;
static constexpr uint32_t kRainDebounceUs = 100000;

// AS3935 register values (from SparkFun's examples).
static constexpr uint8_t kAsOutdoor = 0x0E;
static constexpr uint8_t kAsLightningInt = 0x08;
static constexpr uint8_t kAsDistanceOutOfRange = 0x3F;

// Survive deep sleep.
RTC_DATA_ATTR static uint32_t rtcRainTips = 0;
RTC_DATA_ATTR static uint16_t rtcLightningTotal = 0;
RTC_DATA_ATTR static uint8_t rtcLightningKm = wx::kNoU8;
RTC_DATA_ATTR static bool rtcRainArmed = false;

static BME280 bme;
static SparkFun_AS3935 lightning;
static bool bmeOk = false, lightningOk = false;

static volatile uint32_t windClicks = 0;
static volatile uint32_t lastWindUs = 0;
static volatile uint32_t lastRainUs = 0;

static void IRAM_ATTR onWindClick() {
  uint32_t now = micros();
  if (now - lastWindUs < kWindDebounceUs) return;
  lastWindUs = now;
  windClicks++;
}

static void IRAM_ATTR onRainTip() {
  uint32_t now = micros();
  if (now - lastRainUs < kRainDebounceUs) return;
  lastRainUs = now;
  rtcRainTips++;
}

bool sensorHandleWake() {
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT0) {
    if (rtcRainArmed) rtc_gpio_deinit(gpio_num_t(PIN_RAIN));
    rtcRainArmed = false;
    return false;
  }
  // Woken by a rain-gauge tip: count it, release the pin, and let the caller go back to sleep.
  rtc_gpio_deinit(gpio_num_t(PIN_RAIN));
  rtcRainArmed = false;
  rtcRainTips++;
  pinMode(PIN_RAIN, INPUT_PULLUP);
  uint32_t start = millis();
  while (digitalRead(PIN_RAIN) == LOW && millis() - start < 200) delay(5);  // wait for the reed to open
  return true;
}

void sensorBeforeSleep() {
  // Arm the rain gauge as a wake source, unless its switch is stuck closed (that would wake us in
  // a loop); the timer wake then still delivers the next report.
  pinMode(PIN_RAIN, INPUT_PULLUP);
  if (digitalRead(PIN_RAIN) == LOW) return;
  rtc_gpio_init(gpio_num_t(PIN_RAIN));
  rtc_gpio_set_direction(gpio_num_t(PIN_RAIN), RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(gpio_num_t(PIN_RAIN));
  rtc_gpio_pulldown_dis(gpio_num_t(PIN_RAIN));
  esp_sleep_enable_ext0_wakeup(gpio_num_t(PIN_RAIN), 0);
  rtcRainArmed = true;
}

bool sensorBegin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  bme.setI2CAddress(0x77);
  bmeOk = bme.beginI2C(Wire);
  if (!bmeOk) Serial.println("sensor: BME280 not found at 0x77");

  SPI.begin(PIN_SPI_SCK, PIN_SPI_CIPO, PIN_SPI_COPI);
  lightningOk = lightning.beginSPI(PIN_LIGHTNING_CS, 2000000);
  if (lightningOk) {
    lightning.setIndoorOutdoor(kAsOutdoor);  // keeps its settings while powered; cheap to repeat
  } else {
    Serial.println("sensor: AS3935 not responding");
  }

  pinMode(PIN_WIND_SPEED, INPUT_PULLUP);
  pinMode(PIN_RAIN, INPUT_PULLUP);
  if (PIN_SOIL_POWER >= 0) {
    pinMode(PIN_SOIL_POWER, OUTPUT);
    digitalWrite(PIN_SOIL_POWER, LOW);
  }
  analogSetPinAttenuation(PIN_WIND_DIR, ADC_11db);
  analogSetPinAttenuation(PIN_SOIL, ADC_11db);
  return bmeOk;  // the BME280 is the core sensor; the others are reported as "not measured" if absent
}

// Nearest of the 16 vane positions, comparing the measured voltage with the voltage each position's
// resistor gives against the 10 kOhm pull-up. Returns kNoU16 if the vane looks disconnected.
static uint16_t readWindDirection() {
  static const struct { uint16_t deg10; uint32_t ohms; } kVane[16] = {
      {0, 33000},     {225, 6570},  {450, 8200},   {675, 891},    {900, 1000},   {1125, 688},
      {1350, 2200},   {1575, 1410}, {1800, 3900},  {2025, 3140},  {2250, 16000}, {2475, 14120},
      {2700, 120000}, {2925, 42120}, {3150, 64900}, {3375, 21880},
  };
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_WIND_DIR);
  mv /= 8;
  if (mv > 3200) return wx::kNoU16;  // open circuit: pulled all the way up
  int best = -1;
  float bestErr = 1e9f;
  for (int i = 0; i < 16; i++) {
    float expect = 3300.0f * kVane[i].ohms / (kVane[i].ohms + 10000.0f);
    float err = fabsf(logf((mv + 1.0f) / (expect + 1.0f)));
    if (err < bestErr) {
      bestErr = err;
      best = i;
    }
  }
  if (bestErr > 0.25f) return wx::kNoU16;  // more than ~25 % away from any position
  return uint16_t((kVane[best].deg10 + 5) / 10);
}

static uint16_t readSoilPct10() {
  if (PIN_SOIL_POWER >= 0) {
    digitalWrite(PIN_SOIL_POWER, HIGH);
    delay(10);
  }
  uint32_t raw = 0;
  for (int i = 0; i < 8; i++) raw += analogRead(PIN_SOIL);
  raw /= 8;
  if (PIN_SOIL_POWER >= 0) digitalWrite(PIN_SOIL_POWER, LOW);
  float pct = 100.0f * (float(raw) - SOIL_ADC_DRY) / float(SOIL_ADC_WET - SOIL_ADC_DRY);
  pct = constrain(pct, 0.0f, 100.0f);
  return uint16_t(pct * 10 + 0.5f);
}

// Counts anemometer clicks for WX_WIND_SAMPLE_S seconds; mean and strongest 1-second gust.
static void readWind(uint16_t& meanX100, uint16_t& gustX100) {
  windClicks = 0;
  attachInterrupt(digitalPinToInterrupt(PIN_WIND_SPEED), onWindClick, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_RAIN), onRainTip, FALLING);  // count tips while awake
  uint32_t maxPerSecond = 0, prev = 0;
  for (int s = 0; s < WX_WIND_SAMPLE_S; s++) {
    delay(1000);
    uint32_t now = windClicks;
    maxPerSecond = max(maxPerSecond, now - prev);
    prev = now;
  }
  detachInterrupt(digitalPinToInterrupt(PIN_WIND_SPEED));
  detachInterrupt(digitalPinToInterrupt(PIN_RAIN));
  meanX100 = uint16_t(100.0f * kMsPerClickPerS * prev / WX_WIND_SAMPLE_S + 0.5f);
  gustX100 = uint16_t(100.0f * kMsPerClickPerS * maxPerSecond + 0.5f);
}

bool sensorRead(wx::Reading& r) {
  if (bmeOk) {
    r.temp_c_x100 = int16_t(lroundf(bme.readTempC() * 100));
    r.rh_x100 = uint16_t(lroundf(bme.readFloatHumidity() * 100));
    r.press_hpa_x10 = uint16_t(lroundf(bme.readFloatPressure() / 10.0f));  // Pa -> hPa x10
  }
  readWind(r.wind_ms_x100, r.gust_ms_x100);
  r.wind_dir_deg = readWindDirection();
  r.rain_total_mm_x10 = uint16_t(lroundf(rtcRainTips * kMmPerTip * 10));
  r.soil_pct_x10 = readSoilPct10();

  if (lightningOk) {
    // Polled (interrupt line not wired): only the latest event since the previous report is seen.
    if (lightning.readInterruptReg() == kAsLightningInt) {
      rtcLightningTotal++;
      uint8_t km = lightning.distanceToStorm();
      rtcLightningKm = (km == kAsDistanceOutOfRange) ? wx::kNoU8 : km;
    }
    r.lightning_total = rtcLightningTotal;
    r.lightning_km = rtcLightningKm;
  }
  if (bmeOk) bme.setMode(MODE_SLEEP);
  return bmeOk;
}

#endif  // WX_SENSOR_MICROMOD
