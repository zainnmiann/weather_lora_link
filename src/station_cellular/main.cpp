// LakeLink station, cellular: the weather station's MicroMod ESP32 reads its sensors (lib/wxstation, the
// builder's code) and uploads signed readings itself through its SIM800L (2G) to the cloud server
// (lakelink_cloud, POST /api/v1/ingest, batch format in lib/cellmodem/upload_batch.h).
//
// Two tasks so the modem can never stall the sensors (the original SIM800L firmware stalled for minutes):
//   loop()      : sensors every 3 s (TELEM lines on USB), one signed reading every WX_INTERVAL_S -> queue
//   cell task   : modem bring-up, GPRS bearer, uploads the oldest queued readings in batches
// A reading leaves the queue only after the server replied "200 OK ...". Failures back off
// 30 s -> 5 min; repeated failures reopen the bearer, then restart the modem.
// Wiring: SIM TXD -> RX1 (GPIO16), SIM RXD -> TX1 (GPIO17). GPIO17 is also the AS3935 interrupt, so the
// lightning detector is polled.
//
// Not yet: queue persistence across power loss (RAM only: 512 readings = 42 h at 5 min).
#include <Arduino.h>
#include <SPI.h>
#include <esp_random.h>
#include <esp_task_wdt.h>

#include "cell_uplink.h"
#include "wx_config.h"
#include "wx_frame.h"
#include "wx_station.h"

#ifndef WX_STATION_ID
#define WX_STATION_ID 1
#endif
#ifndef CELL_URL
#define CELL_URL ""  // e.g. "http://1.2.3.4/api/v1/ingest"; empty = modem is exercised but nothing is sent
#endif
#ifndef CELL_APN
#define CELL_APN "internet"  // from the original firmware
#endif

using wxstation::nowMs;

static const uint8_t kKey[] = WX_HMAC_KEY;
static uint16_t bootId, seq = 0;

// ---- sensors + readings (loop task) -------------------------------------------------------------
static void makeReading() {
  wx::DataFrame d;
  d.station_id = WX_STATION_ID;
  d.boot_id = bootId;
  d.seq = ++seq;
  if (seq == 1) d.r.flags |= wx::kFlagColdBoot;
  wxstation::fillReading(d);
  uint8_t frame[wx::kDataLen];
  if (wx::encodeData(d, kKey, sizeof(kKey), frame, sizeof(frame)) != wx::kDataLen) {
    Serial.println("[cell] BUG: could not encode reading");
    return;
  }
  celluplink::enqueue(frame, WX_STATION_ID);
  // The same signed reading for a USB uploader (tools/station_uploader.py on a Pi or laptop).
  Serial.print("WXF ");
  for (uint8_t b : frame) Serial.printf("%02x", b);
  Serial.println();
  Serial.printf("[cell] reading boot %u seq %u queued%s (queue %d)\n", bootId, seq,
                (d.r.flags & wx::kFlagSensorFail) ? " SENSOR_FAIL" : "", celluplink::queued());
}

void setup() {
  // Less heat on the board, whose BME280 sits next to the ESP32: 80 MHz is plenty for a reading every 3 s
  // (APB stays 80 MHz, so UART/I2C/SPI timing is unchanged), and the radios stay off.
  setCpuFrequencyMhz(80);
  btStop();
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Hava Istasyonu baslatiliyor ===");  // tools/telem_source.py detects restarts by this banner
  Serial.printf("LakeLink station %d, cellular; CPU %u MHz; server %s, APN %s, reading every %d s\n", WX_STATION_ID,
                getCpuFrequencyMhz(),
                strlen(CELL_URL) ? CELL_URL : "(none configured)", CELL_APN, WX_INTERVAL_S);
  esp_task_wdt_init(30, true);  // reboot if either task hangs for 30 s
  esp_task_wdt_add(NULL);
#ifdef LORA_PARK_NSS
  // An SX1278 shares this SPI bus (unused here): deselect it and put it to sleep (LoRa sleep, ~0.2 uA)
  // so it never answers the lightning sensor's traffic.
  pinMode(LORA_PARK_NSS, OUTPUT);
  digitalWrite(LORA_PARK_NSS, HIGH);
  SPI.begin(18, 19, 23, -1);
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(LORA_PARK_NSS, LOW);
  SPI.transfer(0x01 | 0x80);  // RegOpMode <- sleep
  SPI.transfer(0x00);
  digitalWrite(LORA_PARK_NSS, HIGH);
  SPI.endTransaction();
#endif
  wxstation::Options opt;
  opt.lightningInterrupt = false;  // GPIO17 is the modem's TX line
  wxstation::begin(opt);
  bootId = uint16_t(esp_random());
  celluplink::Config cell;
  cell.url = CELL_URL;
  cell.apn = CELL_APN;
  cell.key = kKey;
  cell.keyLen = sizeof(kKey);
  celluplink::begin(cell);
  Serial.println("--------------------------------------");
}

void loop() {
  esp_task_wdt_reset();
  wxstation::poll();
  static uint64_t nextReading = 10000;  // first reading 10 s after boot, once the sensors have been read
  if (nowMs() >= nextReading) {
    nextReading = nowMs() + uint64_t(WX_INTERVAL_S) * 1000;
    makeReading();
  }
  delay(5);
}
