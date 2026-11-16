#include "sensor.h"

#include <Arduino.h>
#include <math.h>

#if WX_FAKE_SENSOR

// Plausible synthetic weather for bench tests. Frames carry kFlagFakeData so they can never be
// mistaken for real observations downstream.
RTC_DATA_ATTR static uint16_t fakeRainX10 = 0;

bool sensorBegin() { return true; }

bool sensorHandleWake() { return false; }

void sensorBeforeSleep() {}

bool sensorRead(wx::Reading& r) {
  float t = millis() / 1000.0f + esp_random() % 1000;
  r.temp_c_x100 = int16_t(2150 + 300 * sinf(t / 600.0f));
  r.rh_x100 = uint16_t(5500 + 1500 * cosf(t / 900.0f));
  r.press_hpa_x10 = uint16_t(10132 + esp_random() % 20);
  r.wind_ms_x100 = uint16_t(esp_random() % 800);
  r.gust_ms_x100 = uint16_t(r.wind_ms_x100 + esp_random() % 400);
  r.wind_dir_deg = uint16_t(esp_random() % 360);
  if (esp_random() % 4 == 0) fakeRainX10 += 2;  // one 0.2 mm bucket tip now and then
  r.rain_total_mm_x10 = fakeRainX10;
  r.soil_pct_x10 = uint16_t(300 + esp_random() % 200);
  r.lightning_total = 0;
  r.flags |= wx::kFlagFakeData;
  return true;
}

#elif WX_SENSOR_MICROMOD

// Real sensors: implemented in sensor_micromod.cpp (SparkFun MicroMod Weather Carrier Board).

#else

#error "Pick a sensor implementation: WX_FAKE_SENSOR=1 or WX_SENSOR_MICROMOD=1"

#endif
