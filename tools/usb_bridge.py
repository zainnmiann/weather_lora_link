#!/usr/bin/env python3
"""Stands in for the UART wires between the weather station's ESP32 and its LoRa-E5 (office test).

The ESP32 runs the station_lakelink_usb build: it writes its AT commands to its USB serial port, mixed
with its normal log lines. This script passes every line that starts with "AT" to the LoRa-E5 on
another USB port and passes everything the E5 says back to the ESP32 unchanged. All other ESP32 lines
are written to the log with a timestamp; "[lora]" lines get the "[station 1]" prefix that
tools/chain_check.py expects. On stop it writes "stopped; queue N" (N = the firmware's last reported
queue length); the firmware's own "held [...]" lines name the readings still queued.

Usage: usb_bridge.py --esp dev:weather --e5 dev:lora2 [--seconds S] [--no-reset] > station.log
"""

import argparse
import re
import signal
import sys
import threading
import time

import serial

from ports import resolve

QUEUE = re.compile(r"\(queue (\d+)")


def stamp():
    return time.strftime("%H:%M:%S")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--esp", default="dev:weather")
    ap.add_argument("--e5", default="dev:lora2")
    ap.add_argument("--station", type=int, default=1, help="node id used in the log prefix")
    ap.add_argument("--seconds", type=float, default=0, help="stop after S seconds (0 = until Ctrl-C / SIGTERM)")
    ap.add_argument("--no-reset", action="store_true", help="do not restart the ESP32 when starting")
    ap.add_argument("-v", "--verbose", action="store_true", help="also log the AT traffic")
    args = ap.parse_args()

    esp = serial.Serial()
    esp.port, esp.baudrate, esp.timeout = resolve(args.esp), 115200, 0.05
    esp.dtr = esp.rts = False  # RTS = EN, DTR = IO0 on this board: both released = no reset / no bootloader
    esp.open()
    e5 = serial.Serial(resolve(args.e5), 9600, timeout=0.05)
    out_lock = threading.Lock()
    stop = threading.Event()
    state = {"queue": None, "at_out": 0, "e5_in": 0}
    prefix = f"[station {args.station}] "

    def log(line):
        with out_lock:
            print(f"{stamp()} {line}", flush=True)

    def fail(msg):
        log(f"[bridge] UNEXPECTED {msg}")
        stop.set()

    def esp_to_e5():
        buf = b""
        while not stop.is_set():
            try:
                chunk = esp.read(256)
            except (serial.SerialException, OSError) as err:
                return fail(f"ESP32 port error: {err}")
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                raw = raw.rstrip(b"\r").lstrip(b"\xff")  # the firmware's wake bytes precede an "AT"
                if raw.startswith(b"AT"):
                    try:
                        e5.write(raw + b"\r\n")
                    except (serial.SerialException, OSError) as err:
                        return fail(f"LoRa-E5 port error: {err}")
                    state["at_out"] += 1
                    if args.verbose:
                        log(f"[bridge] -> {raw.decode(errors='replace')}")
                    continue
                line = raw.decode(errors="replace")
                if line.startswith("[lora] "):
                    line = prefix + line[7:]
                    if (m := QUEUE.search(line)):
                        state["queue"] = int(m.group(1))
                    if " held [" in line:
                        state["queue"] = line.count(":")  # exact: one boot:seq per held reading
                if line:
                    log(line)

    def e5_to_esp():
        pending = b""
        while not stop.is_set():
            try:
                chunk = e5.read(256)
            except (serial.SerialException, OSError) as err:
                return fail(f"LoRa-E5 port error: {err}")
            if not chunk:
                continue
            try:
                esp.write(chunk)
            except (serial.SerialException, OSError) as err:
                return fail(f"ESP32 port error: {err}")
            state["e5_in"] += len(chunk)
            if args.verbose:
                pending += chunk
                while b"\n" in pending:
                    raw, pending = pending.split(b"\n", 1)
                    log(f"[bridge] <- {raw.rstrip(chr(13).encode()).decode(errors='replace')}")

    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    if not args.no_reset:
        esp.rts = True  # pull EN low: restart the ESP32 so the run starts from a fresh boot
        time.sleep(0.2)
        esp.rts = False
        log("[bridge] ESP32 restarted")
    threads = [threading.Thread(target=f, daemon=True) for f in (esp_to_e5, e5_to_esp)]
    for th in threads:
        th.start()
    log(f"[bridge] {esp.port} <-> {e5.port}")
    t_end = time.monotonic() + args.seconds if args.seconds else None
    try:
        while not stop.is_set() and (t_end is None or time.monotonic() < t_end):
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    stop.set()
    for th in threads:
        th.join(1)
    q = state["queue"]
    log(f"{prefix}stopped; queue {q if q is not None else 'unknown'}")
    log(f"[bridge] AT lines passed: {state['at_out']}, bytes from E5: {state['e5_in']}")
    if state["at_out"] == 0:
        log("[bridge] UNEXPECTED: the ESP32 sent no AT commands (is the station_lakelink_usb build flashed?)")
    esp.close()
    e5.close()


if __name__ == "__main__":
    main()
