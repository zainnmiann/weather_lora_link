"""Logs both hop nodes' serial output side by side (timestamped) into one file per port.
    python3 tools/hop_watch.py SECONDS OUTDIR PORT[=NAME] ...
Each board is reset when the run starts."""
import sys
import threading
import time
from pathlib import Path

import serial


def watch(port, name, out, until):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.dtr, s.rts = port, 115200, 0.2, False, False
    s.open()
    s.rts = True  # reset pulse: every run starts from a fresh boot
    time.sleep(0.1)
    s.rts = False
    with s, open(out / f"{name}.log", "w") as f:
        buf = b""
        while time.time() < until:
            buf += s.read(4096)
            *lines, buf = buf.split(b"\n")
            for line in lines:
                text = line.decode(errors="replace").rstrip()
                stamp = time.strftime("%H:%M:%S")
                f.write(f"{stamp} {text}\n")
                f.flush()
                print(f"{stamp} {name:8} {text}", flush=True)


secs, out = float(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
until = time.time() + secs
threads = []
for arg in sys.argv[3:]:
    port, _, name = arg.partition("=")
    t = threading.Thread(target=watch, args=(port, name or Path(port).name, out, until))
    t.start()
    threads.append(t)
for t in threads:
    t.join()
