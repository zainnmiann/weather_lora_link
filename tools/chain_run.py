#!/usr/bin/env python3
"""Runs one station -> relay -> gateway scenario on real boards (or fake_air ports) and checks it.

  chain_run.py NAME --gw PORT --relay PORT --station PORT [--duration S] [--interval S]
               [--kill relay|gateway|station --kill-at S --restart-at S]
               [--relay-args "..."] [--gw-args "..."] [--station-args "..."] [--freq MHz]

Logs go to range_tests/chain_runs/<NAME>/. The station is stopped at --duration; relay and gateway
keep running --drain seconds more so queued readings can arrive, then chain_check.py judges it.
"""

import argparse
import os
import shlex
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "range_tests" / "chain_runs"


def start(role, port, extra, log, freq, gen_for=None):
    cmd = [sys.executable, "-u", str(HERE / "e5_node.py"), "-p", port, "--role", role, "--freq", freq]
    cmd += {"gateway": ["--id", "3"], "relay": ["--id", "2", "--next", "3"], "station": ["--id", "1", "--next", "2"]}[role]
    cmd += shlex.split(extra)
    if role == "station" and gen_for is not None:
        cmd += ["--gen-for", f"{max(1.0, gen_for):.0f}"]
    f = open(log, "a")
    return subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=HERE)


def stop(p):
    if p and p.poll() is None:
        p.send_signal(signal.SIGTERM)
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--gw", required=True)
    ap.add_argument("--relay", required=True)
    ap.add_argument("--station", required=True)
    ap.add_argument("--duration", type=float, default=240)
    ap.add_argument("--drain", type=float, default=90)
    ap.add_argument("--interval", type=float, default=8)
    ap.add_argument("--freq", default="869.525")
    ap.add_argument("--events", default="", help='timeline like "40:stop:gateway,100:stop:relay,110:start:relay"')
    ap.add_argument("--kill", choices=["relay", "gateway", "station"])
    ap.add_argument("--kill-at", type=float, default=60)
    ap.add_argument("--restart-at", type=float, default=150)
    ap.add_argument("--relay-args", default="")
    ap.add_argument("--gw-args", default="")
    ap.add_argument("--station-args", default="")
    a = ap.parse_args()

    d = OUT / a.name
    d.mkdir(parents=True, exist_ok=True)
    for f in d.glob("*.log"):
        f.unlink()
    logs = {r: d / f"{r}.log" for r in ("station", "relay", "gateway")}
    ports = {"station": a.station, "relay": a.relay, "gateway": a.gw}
    for f in d.glob("*.state*"):
        f.unlink()
    extra = {"station": f"--interval {a.interval} {a.station_args}",
             "relay": f"--state {d / 'relay.state'} {a.relay_args}",
             "gateway": f"--state {d / 'gateway.state'} {a.gw_args}"}

    procs = {}
    procs["gateway"] = start("gateway", ports["gateway"], extra["gateway"], logs["gateway"], a.freq)
    procs["relay"] = start("relay", ports["relay"], extra["relay"], logs["relay"], a.freq)
    time.sleep(4)
    procs["station"] = start("station", ports["station"], extra["station"], logs["station"], a.freq, a.duration)
    t0 = time.monotonic()
    killed = restarted = False
    events = []
    for ev in filter(None, a.events.split(",")):
        at, what, role = ev.split(":")
        events.append([float(at), what, role, False])
    down = set()
    print(f"[{a.name}] running {a.duration:.0f}s (+{a.drain:.0f}s drain), logs in {d}", flush=True)
    while time.monotonic() - t0 < a.duration:
        el = time.monotonic() - t0
        if a.kill and not killed and el >= a.kill_at:
            print(f"[{a.name}] t={el:.0f}s: stopping {a.kill}", flush=True)
            stop(procs[a.kill])
            killed = True
        if a.kill and killed and not restarted and el >= a.restart_at:
            print(f"[{a.name}] t={el:.0f}s: restarting {a.kill}", flush=True)
            procs[a.kill] = start(a.kill, ports[a.kill], extra[a.kill], logs[a.kill], a.freq, a.duration - el)
            restarted = True
        for ev in events:
            if not ev[3] and el >= ev[0]:
                ev[3] = True
                print(f"[{a.name}] t={el:.0f}s: {ev[1]} {ev[2]}", flush=True)
                if ev[1] == "stop":
                    stop(procs[ev[2]])
                    down.add(ev[2])
                else:
                    procs[ev[2]] = start(ev[2], ports[ev[2]], extra[ev[2]], logs[ev[2]], a.freq, a.duration - el)
                    down.discard(ev[2])
        for r, p in procs.items():
            if r in down:
                continue
            if p.poll() is not None and not (a.kill == r and killed and not restarted):
                print(f"[{a.name}] ABORT: {r} exited unexpectedly with {p.returncode}; see {logs[r]}", flush=True)
                for q in procs.values():
                    stop(q)
                sys.exit(3)
        time.sleep(1)
    time.sleep(a.drain)  # readings stopped at --duration; let every queue empty
    stop(procs["station"])
    stop(procs["relay"])
    stop(procs["gateway"])
    res = subprocess.run([sys.executable, str(HERE / "chain_check.py"), str(logs["station"]), str(logs["gateway"]),
                          str(logs["relay"])], capture_output=True, text=True)
    out = "\n".join(l for l in res.stdout.splitlines() if "NotOpenSSL" not in l)
    (d / "result.txt").write_text(out + "\n")
    print(out, flush=True)
    sys.exit(res.returncode)


if __name__ == "__main__":
    main()
