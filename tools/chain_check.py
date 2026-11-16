#!/usr/bin/env python3
"""Checks a station -> relay(s) -> gateway run from the e5_node.py logs.

Usage: chain_check.py STATION_LOG GATEWAY_LOG [RELAY_LOG ...]
Exit status 0 only if every reading the station made was delivered exactly once, in order, with
nothing delivered that the station did not make. Readings still queued when the run stopped are
reported separately (they were not lost, the run just ended).
"""

import re
import sys
from datetime import datetime

QUEUED = re.compile(r"^(\d\d:\d\d:\d\d) \[station \d+\] reading boot (\d+) seq (\d+) queued")
DELIV = re.compile(r"^(\d\d:\d\d:\d\d) \[gateway \d+\] DELIVERED station \d+ boot (\d+) seq (\d+) via (\d+) hop")
STOPPED_Q = re.compile(r"stopped; queue (\d+)")
HELD = re.compile(r"\] held \[(.*)\]")
PROBLEMS = [("Traceback", "crash"), ("Error", "error text"), ("REJECTED", "rejected payload"),
            ("RECOVERING", "radio/serial recovery"), ("duty-cycle", "duty-cycle hold"),
            ("unreachable", "next hop unreachable"), ("repeat lseq", "duplicate re-acked"),
            ("already have it", "end-to-end duplicate ignored"), ("[fault]", "injected fault"),
            ("queue full", "queue overflow (DATA LOSS)"), ("hop limit", "hop limit drop")]


def t(s):
    return datetime.strptime(s, "%H:%M:%S")


def scan(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    return [l for l in lines if "NotOpenSSL" not in l and "warnings.warn" not in l]


def main():
    st_lines, gw_lines = scan(sys.argv[1]), scan(sys.argv[2])
    relay_logs = [(p, scan(p)) for p in sys.argv[3:]]
    made = [(m.group(2), int(m.group(3)), t(m.group(1))) for l in st_lines if (m := QUEUED.match(l))]
    got = [(m.group(2), int(m.group(3)), t(m.group(1)), int(m.group(4))) for l in gw_lines if (m := DELIV.match(l))]
    made_keys = [(b, s) for b, s, _ in made]
    got_keys = [(b, s) for b, s, _, _ in got]
    made_at = {(b, s): when for b, s, when in made}

    still_queued = 0
    for l in st_lines:
        if (m := STOPPED_Q.search(l)):
            still_queued = int(m.group(1))
    relay_queued = 0
    for _, ls in relay_logs:
        last = 0  # a restarted node writes one "stopped" line per run; only its final one is what remains
        for l in ls:
            if (m := STOPPED_Q.search(l)):
                last = int(m.group(1))
        relay_queued += last

    def final_held(lines):
        held = set()
        for l in lines:  # a restarted node logs once per run; only its final run's queue remains
            if (m := HELD.search(l)):
                held = {tuple(x.strip(" '").split(":")) for x in m.group(1).split(",") if ":" in x}
        return {(b, int(s)) for b, s in held}
    held_ids = final_held(st_lines)
    for _, ls in relay_logs:
        held_ids |= final_held(ls)
    dup = sorted({k for k in got_keys if got_keys.count(k) > 1})
    phantom = [k for k in got_keys if k not in made_at]
    missing = [k for k in made_keys if k not in got_keys]
    # order: deliveries must follow the order readings were made
    order_ok = [k for k in made_keys if k in got_keys] == [k for k in got_keys if k in made_at][:len([k for k in made_keys if k in got_keys])]
    in_flight = still_queued + relay_queued
    unexplained = [k for k in missing if k not in held_ids]  # identity match, not a count
    unexplained_missing = len(unexplained)
    lat = [(when - made_at[(b, s)]).total_seconds() % 86400 for b, s, when, _ in got if (b, s) in made_at]  # survives midnight
    hops = sorted({h for *_, h in got})

    print(f"readings made           : {len(made)}")
    print(f"delivered (unique)      : {len(set(got_keys))}   hop counts seen: {hops}")
    print(f"still queued at stop    : station {still_queued}, relays {relay_queued}")
    print(f"missing                 : {len(missing)} {missing[:10]}")
    print(f"  of which unexplained  : {unexplained_missing} {unexplained[:10]}   (missing and not held by any node)")
    print(f"duplicates delivered    : {len(dup)} {dup[:10]}")
    print(f"phantom deliveries      : {len(phantom)} {phantom[:10]}")
    print(f"delivery order preserved: {order_ok}")
    if lat:
        lat_sorted = sorted(lat)
        print(f"latency s (min/median/max): {lat_sorted[0]:.0f} / {lat_sorted[len(lat_sorted)//2]:.0f} / {lat_sorted[-1]:.0f}")
    print("log events:")
    for name, ls in [("station", st_lines), ("gateway", gw_lines)] + [(f"relay({p.split('/')[-1]})", ls) for p, ls in relay_logs]:
        counts = {label: sum(1 for l in ls if pat in l) for pat, label in PROBLEMS}
        shown = {k: v for k, v in counts.items() if v}
        print(f"  {name:22}: {shown or 'none'}")
        for l in ls:
            if "stopped;" in l:
                print(f"  {'':22}  {l.split('stopped; ', 1)[1]}")
    ok = not dup and not phantom and order_ok and unexplained_missing == 0 and not any(
        "Traceback" in l or "queue full" in l for _, ls in [("s", st_lines), ("g", gw_lines)] + relay_logs for l in ls)
    if not ok:
        print("RESULT: FAIL")
        sys.exit(1)
    if in_flight or missing:
        print(f"RESULT: PASS-UNDRAINED (correct, but {len(missing)} reading(s) had not arrived when the run ended)")
        sys.exit(2)
    print("RESULT: PASS")
    sys.exit(0)


if __name__ == "__main__":
    main()
