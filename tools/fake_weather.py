#!/usr/bin/env python3
"""Pretends to be the weather station's ESP32 on a pseudo-terminal, for testing telem_source.py.

Prints the same human-readable and TELEM lines as the original firmware every --period s, with
known values. --restart-after N prints the boot banner and resets the rain total after N lines;
--lightning-every N emits an EVENT lightning line every N lines. Prints the device path first.
"""

import argparse
import os
import time
import tty


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--period", type=float, default=3.0)
    ap.add_argument("--restart-after", type=int, default=0)
    ap.add_argument("--lightning-every", type=int, default=0)
    a = ap.parse_args()
    master, slave = os.openpty()
    tty.setraw(slave)
    print(f"weather: {os.ttyname(slave)}", flush=True)

    def out(s):
        os.write(master, (s + "\r\n").encode())

    out("=== Hava Istasyonu (SIM800L) baslatiliyor ===")
    n, rain = 0, 0.0
    while True:
        n += 1
        if a.restart_after and n == a.restart_after:
            out("ets Jul 29 2019 12:21:46")
            out("=== Hava Istasyonu (SIM800L) baslatiliyor ===")
            rain = 0.0
        rain += 0.28 if n % 4 == 0 else 0.0
        t = 20.0 + (n % 10) * 0.1
        wind = (n % 5) * 3.6  # km/h: 0, 3.6, 7.2, 10.8, 14.4 -> 0..4 m/s
        out(f"Sicaklik: {t:.1f} C  |  Nem: 55.0 %  |  Basinc: 1001.5 hPa  |  Yukseklik: 102.0 m")
        out(f'TELEM {{"temperature":{t:.2f},"humidity":55.00,"pressure":1001.50,"altitude":102.00,'
            f'"wind_speed":{wind:.2f},"wind_dir":{(n * 45) % 360:.1f},"rain_mm":{rain:.2f},"soil_pct":42.5,"soil_adc":300}}')
        if a.lightning_every and n % a.lightning_every == 0:
            out('EVENT {"type":"lightning","distance_km":12,"energy":34567}')
        out("--------------------------------------")
        time.sleep(a.period)


if __name__ == "__main__":
    main()
