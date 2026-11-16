#!/usr/bin/env python3
"""Phase 1 value check: does each reading arrive at the gateway with exactly the values the weather
station printed?

Usage: value_check.py RUN_DIR   (needs station.log written with --telem-port, and gateway.log)
For each reading: raw TELEM (source) -> station's encoded reading -> gateway's JSON.
"""

import json
import re
import sys
from pathlib import Path

SRC = re.compile(r"\] source TELEM (\{.*\})$")
QUEUED = re.compile(r"\] reading boot (\d+) seq (\d+) queued \(queue \d+\) (\{.*\})$")
DELIV = re.compile(r"\] DELIVERED station \d+ boot (\d+) seq (\d+) via \d+ hop\(s\): (\{.*\})$")
FIELDS = ["temp_c", "rh_pct", "press_hpa", "wind_ms", "gust_ms", "wind_dir_deg", "rain_total_mm", "soil_pct",
          "lightning_total", "lightning_km", "sensor_fail"]


def main():
    d = Path(sys.argv[1])
    st = d.joinpath("station.log").read_text(errors="replace").splitlines()
    gw = d.joinpath("gateway.log").read_text(errors="replace").splitlines()
    readings, src = [], None
    for line in st:
        if (m := SRC.search(line)):
            src = json.loads(m.group(1))
        elif (m := QUEUED.search(line)):
            readings.append(((m.group(1), int(m.group(2))), src, json.loads(m.group(3))))
            src = None
    delivered = {}
    for line in gw:
        if (m := DELIV.search(line)):
            delivered[(m.group(1), int(m.group(2)))] = json.loads(m.group(3))

    problems = []
    for key, source, sent in readings:
        got = delivered.get(key)
        if got is None:
            problems.append(f"{key}: not delivered")
            continue
        for f in FIELDS:  # radio path must not change anything
            if sent.get(f) != got.get(f):
                problems.append(f"{key}: {f} sent {sent.get(f)} but gateway has {got.get(f)}")
        if not source:
            if not sent.get("sensor_fail"):
                problems.append(f"{key}: no TELEM source but not marked sensor_fail")
            continue
        # encoding must reproduce the station's own numbers (to the frame's resolution)
        checks = [("temp_c", source.get("temperature"), 0.01), ("rh_pct", source.get("humidity"), 0.01),
                  ("press_hpa", source.get("pressure"), 0.1), ("rain_total_mm", source.get("rain_mm"), 0.1),
                  ("soil_pct", source.get("soil_pct"), 0.1)]
        for f, raw, res in checks:
            if raw is None:
                continue
            if sent.get(f) is None or abs(sent[f] - float(raw)) > res / 2 + 1e-9:
                problems.append(f"{key}: {f} = {sent.get(f)} but station printed {raw}")
        wd = source.get("wind_dir")
        if wd is not None and float(wd) < 0 and "wind_dir_deg" in sent:
            problems.append(f"{key}: wind direction -1 (not read) was sent as {sent['wind_dir_deg']}")
        if "wind_ms" in sent and "gust_ms" in sent and sent["gust_ms"] + 1e-9 < sent["wind_ms"]:
            problems.append(f"{key}: gust {sent['gust_ms']} below mean wind {sent['wind_ms']}")

    print(f"readings: {len(readings)}, delivered: {sum(1 for k, *_ in readings if k in delivered)}")
    if readings:
        first, last = readings[0][2], readings[-1][2]
        print(f"first: {json.dumps({f: first.get(f) for f in FIELDS if f in first})}")
        print(f"last : {json.dumps({f: last.get(f) for f in FIELDS if f in last})}")
    print(f"value problems: {len(problems)}")
    for p in problems[:20]:
        print("  ", p)
    print("VALUES:", "OK" if not problems else "MISMATCH")
    sys.exit(0 if not problems else 1)


if __name__ == "__main__":
    main()
