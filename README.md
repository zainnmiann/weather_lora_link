# weather_lora_link

Sends readings from one solar-powered weather station over a LoRa point-to-point link to an
internet-connected gateway, which publishes them as JSON over MQTT.

```
[met sensor] -> [station ESP32] --UART AT--> [LoRa-E5 mini]  ~ LoRa ~  [LoRa-E5 mini] --UART AT--> [gateway ESP32] --WiFi--> MQTT broker
```

Every radio is a **LoRa-E5 mini** board carrying a **Wio-E5-LE-HF** module (14 dBm max); see `DEVICES.md`.
All keep their **factory AT firmware** (TEST/P2P mode). Nothing on them is reflashed.
Meshtastic is not used: it does not support UART-attached radios on ESP32, and a mesh adds
nothing for a single station.

## Hardware per site

| Site | Parts |
|---|---|
| Weather station | the station's existing ESP32 + LoRa-E5 mini (Wio-E5-LE-HF) + antenna |
| Gateway | any ESP32 with WiFi + LoRa-E5 mini (Wio-E5-LE-HF) + antenna, mains/USB powered |
| Relay (only if the range test fails) | spare LoRa-E5 mini + small MCU at a high point between the two (not implemented yet) |

**Wiring** (3.3 V logic on both sides, common GND, cross TX/RX):

| ESP32 | LoRa-E5 mini header |
|---|---|
| GPIO17 (UART2 TX) | AT serial RX ("RX2" / PA3 in Seeed's LoRa-E5 mini pinout) |
| GPIO16 (UART2 RX) | AT serial TX ("TX2" / PA2) |
| optional GPIO -> `E5_RST_PIN` | RST (lets the ESP32 hard-reset a hung module) |
| 3V3 / GND | 3V3 / GND (when the board is powered from the ESP32 instead of USB-C) |

> **Confirm the pin labels on the board's silkscreen before wiring.** Don't drive these pins from the
> ESP32 while the board's USB-C is also plugged into a computer. **Never transmit without an antenna attached.**

## The real station: SparkFun MicroMod Weather Carrier Board

The weather station is a SparkFun MicroMod Weather Carrier Board with a MicroMod ESP32 processor
(no Raspberry Pi on board; identified from its original firmware; full flash backup and notes in
`~/Documents/original_firmware_backups/weather_station_esp32_34987a4cb200_original_20261012.*`).
Build target: `pio run -e station_micromod`. Sensor code: `src/station/sensor_micromod.cpp`.

| Sensor | Pin (MicroMod ESP32 map) | How it is read |
|---|---|---|
| BME280 temp/humidity/pressure | I2C 21/22, address 0x77 | every report |
| Anemometer | GPIO 14 (D0) | clicks counted for 5 s each report: mean + strongest 1-s gust |
| Wind vane | GPIO 35 (A1) | voltage matched to the 16 vane positions (10 kOhm pull-up) |
| Rain gauge | GPIO 27 (D1) | every tip counted, including during deep sleep (tip wakes the ESP32 briefly) |
| Soil moisture | GPIO 34 (A0) | % from the builder's calibration (raw ADC 0 = air, 648 = water) |
| AS3935 lightning | SPI 18/19/23, CS GPIO 12 | polled each report (latest strike + distance) |
| Battery | GPIO 39 (BATT_VIN/3) | every report |

Pins match the builder's own source (`~/Documents/weather_station_pi_extract/home_d7/WeatherStation/WeatherStation.ino`).
**Still to confirm on the hardware before flashing:** the radio's serial pins
(`E5_RX_PIN`/`E5_TX_PIN` = 32/33) are placeholders until we find two free pins on the carrier
(GPIO 16/17 are UART1, probably the SIM800L). The wind direction is relative to the boat's bow:
true wind direction needs a compass/GPS.

## Configure

1. `cp include/secrets.example.h include/secrets.h`, then fill in WiFi and MQTT and **generate a new
   HMAC key** (the command is in the file). Station and gateway must use the same key.
2. In `lib/wxlink/wx_config.h`, set `WX_FREQ_MHZ` to a frequency that is **legal in your country**,
   and check that `WX_TX_POWER_DBM` (14) is within the local limit.
3. In `platformio.ini`, under `[env:station]`:
   - `WX_STATION_ID`: one per station.
   - `BATT_ADC_PIN` / `BATT_DIVIDER`: if the battery voltage divider is wired.
   - `WX_FAKE_SENSOR=0`: once `src/station/sensor.cpp` reads the real sensor.

## Build / flash

```sh
pio run                              # build both
pio run -e gateway -t upload -t monitor
pio run -e station -t upload -t monitor
tools/run_tests.sh                   # host tests: codec, AT parsers, C++/Python parity (no hardware)
```
(`pio` is at `~/Library/Python/3.9/bin/pio` on this Mac.)

## Bench and range testing without the ESP32s

`tools/e5_bench.py` drives a module directly from the computer over the LoRa-E5 mini's USB-C port. It uses the RF settings from `wx_config.h`
and the key from `secrets.h`.

```sh
pip3 install pyserial paho-mqtt
python3 tools/e5_bench.py -p /dev/cu.usbserial-XXXX -v info                # first contact: version + RF config accepted
python3 tools/e5_bench.py -p PORT_A rx --ack --mqtt localhost              # act as the gateway
python3 tools/e5_bench.py -p PORT_B tx --count 50 --interval 5             # act as the station; prints loss %
tools/mqtt_watch.sh                                                        # watch weather/#
```

**Range test (decides whether you need a relay):**
1. Put `rx --ack` at the gateway site and `tx --count 0 --interval 10` at the station site, with
   both antennas mounted where they will finally be.
2. Read the RSSI/SNR printed on both ends:
   - SNR well above the SF floor (about −12.5 dB at SF9) and loss near 0%: no relay needed.
   - Marginal: try `--sf SF10`/`SF12` on **both** ends, then set `WX_SF` to match.
   - Nothing at SF12: add a relay.

`tools/fake_air.py` simulates two or more modules on pseudo-terminals, with optional packet loss.
With it, the whole flow (tx → ACK → MQTT, duplicates, stale replays, bad MACs) can be exercised
without hardware.

## Protocol

- **Data frame:** 33 bytes (version 2: adds soil moisture and lightning), about 0.27 s on air at SF9/125 kHz. **ACK:** 12 bytes. Both carry a 4-byte
  truncated HMAC-SHA256, which authenticates the sender (no encryption). The layout is in
  `lib/wxlink/wx_frame.h`.
- **Station cycle:**
  1. Deep-sleep wake.
  2. Read the sensor.
  3. TX, then wait 2.5 s for an ACK, up to 3 attempts with random backoff.
  4. `AT+LOWPOWER`.
  5. Deep sleep for `WX_INTERVAL_S` (×4 when the battery is below 3.5 V).
- **Duplicate handling:** `boot_id` (random per power-on) + `seq` (survives deep sleep).
  - Same seq again = the ACK was lost: the gateway re-ACKs but doesn't republish.
  - Older seq = stale: dropped.
- **Rain** is a cumulative total since `boot_id` changed, so a lost frame loses no rainfall. Consumers
  take differences and reset when `boot` changes.

### MQTT

`weather/<station_id>/data` (retained):
```json
{"station":1,"boot":59301,"seq":42,"time":"2026-10-07T12:23:07Z","temp_c":21.53,"rh_pct":55.2,
 "press_hpa":1013.2,"wind_ms":3.5,"gust_ms":7.2,"wind_dir_deg":270,"rain_total_mm":5.6,"batt_v":3.912,
 "sensor_fail":false,"fake":false,"low_batt":false,"cold_boot":false,"rssi":-87,"snr":7.0}
```
Fields the station does not measure are omitted. `time` is the gateway's NTP time at reception; it is
absent until NTP has synced.

`weather/gateway/status` (retained, every 60 s): counters (accepted / duplicate / stale / bad_frame /
queue), radio state and WiFi RSSI. Its last-will is `{"online":false}`.

## Status / not yet verified on hardware

- The firmware builds for ESP32, and the host tests and simulation pass. **Neither ESP32 firmware has
  run against real modules yet.** The expected AT replies (`+TEST: TX DONE`, `+TEST: RX "..."`,
  `+LOWPOWER: SLEEP`, …) follow Seeed's AT command spec. Confirm them with `e5_bench.py -v info` and
  a first `tx`/`rx` before flashing the ESP32s.
- `src/station/sensor.cpp` is a stub until the weather sensor's interface is known. If the station's
  existing ESP32 firmware already reads the sensor, move that code into `sensorRead()`.
