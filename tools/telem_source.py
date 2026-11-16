"""Reads the weather station's own TELEM lines (MicroMod ESP32, original firmware) over USB serial.

The original firmware prints, every ~3 s:
  TELEM {"temperature":25.89,"humidity":57.33,"pressure":1001.00,"altitude":102.50,
         "wind_speed":0.00,"wind_dir":-1.0,"rain_mm":0.00,"soil_pct":100.0,"soil_adc":23}
and, when the lightning detector fires (some firmware versions):
  EVENT {"type":"lightning","distance_km":12,"energy":34567}

TelemSource keeps the latest values and aggregates wind over each reporting interval:
mean of the samples -> wind, strongest sample -> gust. The port is opened with DTR/RTS low so
the ESP32 is not reset. If the ESP32 restarts (banner line, or the cumulative rain total going
down), restarts is incremented so the station can start a new boot number: downstream rain
differences must not span an ESP32 reset.
"""

import json
import threading
import time

import serial

import wxframe as wf

STALE_S = 30           # no TELEM for this long -> the reading is marked sensor_fail
BANNER = "Hava Istasyonu"


class TelemSource:
    def __init__(self, port, baud=115200, log=print, start=True):
        self.port, self.baud, self.log = port, baud, log
        self.lock = threading.Lock()
        self.latest = None        # last TELEM dict
        self.latest_at = 0.0      # monotonic time of last TELEM
        self.wind = []            # km/h samples since the last take()
        self.lightning_total = 0
        self.lightning_km = None
        self.restarts = 0
        self.last_rain = None
        self.lines = 0
        self.bad_lines = 0
        self.thread = threading.Thread(target=self._run, daemon=True)
        if start:
            self.thread.start()

    def _open(self):
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = self.port, self.baud, 1.0
        s.dtr, s.rts = False, False  # keep EN/IO0 untouched: do not reset the station
        s.open()
        return s

    def _run(self):
        s, buf = None, b""
        while True:
            try:
                if s is None:
                    s = self._open()
                    self.log(f"telemetry: listening on {self.port}")
                chunk = s.read(512)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self._handle(line.decode(errors="replace").strip())
            except (serial.SerialException, OSError) as err:
                self.log(f"telemetry: port problem ({err}); retrying in 3 s")
                try:
                    if s:
                        s.close()
                except Exception:
                    pass
                s, buf = None, b""
                time.sleep(3)

    def _handle(self, line):
        if BANNER in line:
            with self.lock:
                if self.latest is not None:
                    self.restarts += 1
                    self.log("telemetry: weather station restarted (banner seen)")
                    self.lightning_total, self.lightning_km = 0, None  # counts restart with the new boot, like rain
                self.last_rain = None  # the rain total restarts from 0; don't count the drop as a second restart
            return
        if line.startswith("TELEM "):
            try:
                d = json.loads(line[6:])
                float(d["temperature"])
            except (ValueError, KeyError, TypeError):
                self.bad_lines += 1
                return
            with self.lock:
                rain = d.get("rain_mm")
                if rain is not None and self.last_rain is not None and rain + 0.05 < self.last_rain:
                    self.restarts += 1
                    self.log(f"telemetry: rain total went down ({self.last_rain} -> {rain}): station restart assumed")
                    self.lightning_total, self.lightning_km = 0, None
                if rain is not None:
                    self.last_rain = rain
                self.latest, self.latest_at = d, time.monotonic()
                if d.get("wind_speed") is not None:
                    self.wind.append(float(d["wind_speed"]))
                self.lines += 1
        elif line.startswith("EVENT "):
            try:
                ev = json.loads(line[6:])
            except ValueError:
                self.bad_lines += 1
                return
            if ev.get("type") == "lightning":
                with self.lock:
                    self.lightning_total += 1
                    if ev.get("distance_km") is not None:
                        self.lightning_km = int(ev["distance_km"])

    def take(self):
        """Returns (fields for wx Data, flags, restarts, source TELEM dict) and starts a new wind window."""
        with self.lock:
            d, age = self.latest, time.monotonic() - self.latest_at
            wind, self.wind = self.wind, []
            restarts = self.restarts
            lt, lk = self.lightning_total, self.lightning_km
        if d is None or age > STALE_S:
            return {}, wf.FLAG_SENSOR_FAIL, restarts, None

        def scaled(key, factor, lo, hi):
            v = d.get(key)
            if v is None:
                return None
            v = float(v)
            if not lo <= v <= hi:
                return None
            return int(round(v * factor))
        f = {}
        t = scaled("temperature", 100, -60, 80)
        if t is not None:
            f["temp_c_x100"] = t
        for key, field, factor, lo, hi in (("humidity", "rh_x100", 100, 0, 100), ("pressure", "press_hpa_x10", 10, 300, 1100),
                                           ("rain_mm", "rain_total_mm_x10", 10, 0, 6553), ("soil_pct", "soil_pct_x10", 10, 0, 100)):
            v = scaled(key, factor, lo, hi)
            if v is not None:
                f[field] = v
        wd = d.get("wind_dir")
        if wd is not None and 0 <= float(wd) < 360:
            f["wind_dir_deg"] = int(round(float(wd))) % 360
        if wind:
            kmh_to_ms100 = 100 / 3.6
            f["wind_ms_x100"] = int(round(sum(wind) / len(wind) * kmh_to_ms100))
            f["gust_ms_x100"] = int(round(max(wind) * kmh_to_ms100))
        f["lightning_total"] = lt & 0xFFFF
        if lk is not None:
            f["lightning_km"] = max(0, min(lk, 254))
        return f, 0, restarts, dict(d)
