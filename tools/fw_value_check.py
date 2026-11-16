#!/usr/bin/env python3
"""Checks the values of a station_lakelink firmware run (usb_bridge.py station log + e5_node gateway log).

For every reading the gateway delivered, recomputes what the firmware should have sent from the TELEM
lines it printed before queuing that reading (same rules as src/station_lakelink: last sample for
temperature/humidity/pressure/rain/soil/vane, mean and max of the wind samples since the previous
reading) and compares. Usage: fw_value_check.py RUN_DIR
"""

import json
import math
import re
import sys

QUEUED = re.compile(r"^\d\d:\d\d:\d\d \[station \d+\] reading boot (\d+) seq (\d+) queued")
DELIV = re.compile(r"DELIVERED station \d+ boot (\d+) seq (\d+) via \d+ hop\(s\): (\{.*\})")
# One resolution step: TELEM prints rounded values, the firmware scales the unrounded float.
TOL = {"temp_c": 0.011, "rh_pct": 0.011, "press_hpa": 0.101, "rain_total_mm": 0.101, "soil_pct": 0.101,
       "wind_dir_deg": 1.01, "wind_ms": 0.011, "gust_ms": 0.011}


def rnd(x, nd=0):
    """Halves away from zero, like the firmware's lroundf (Python's round() rounds halves to even)."""
    k = 10 ** nd
    return math.copysign(math.floor(abs(x) * k + 0.5), x) / k


def expected(samples, window):
    d = samples[-1]
    e = {"temp_c": rnd(d["temperature"], 2), "rh_pct": rnd(d["humidity"], 2),
         "press_hpa": rnd(d["pressure"], 1), "rain_total_mm": rnd(d["rain_mm"], 1),
         "soil_pct": rnd(max(0.0, d["soil_pct"]), 1)}
    if 0 <= d["wind_dir"] < 360:
        e["wind_dir_deg"] = rnd(d["wind_dir"]) % 360
    w = [s["wind_speed"] for s in window if s["wind_speed"] >= 0]
    if w:
        e["wind_ms"] = rnd(sum(w) / len(w) / 3.6, 2)
        e["gust_ms"] = rnd(max(w) / 3.6, 2)
    return e


def main():
    run = sys.argv[1]
    want, samples, window = {}, [], []
    for line in open(f"{run}/station.log", encoding="utf-8", errors="replace"):
        if " TELEM " in line:
            try:
                d = json.loads(line.split(" TELEM ", 1)[1])
            except ValueError:
                continue
            samples.append(d)
            window.append(d)
        elif (m := QUEUED.match(line)):
            if samples:
                want[(m.group(1), int(m.group(2)))] = expected(samples, window)
            window = []
    problems = checked = 0
    for line in open(f"{run}/gateway.log", encoding="utf-8", errors="replace"):
        if not (m := DELIV.search(line)):
            continue
        key, got = (m.group(1), int(m.group(2))), json.loads(m.group(3))
        if key not in want:
            print(f"{key}: no TELEM before this reading (first reading after boot?)")
            continue
        checked += 1
        for f, v in want[key].items():
            if f not in got or abs(got[f] - v) > TOL[f]:
                problems += 1
                print(f"{key} {f}: expected {v}, delivered {got.get(f)}")
    print(f"readings checked: {checked}, value problems: {problems}")
    print("VALUES:", "OK" if checked and not problems else "PROBLEMS")
    sys.exit(0 if checked and not problems else 1)


if __name__ == "__main__":
    main()
