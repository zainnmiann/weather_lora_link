#!/usr/bin/env python3
"""USB uploader: the weather station on this computer's USB -> the LakeLink server.

The station firmware (station_cellular) prints every signed reading as a line `WXF <66 hex chars>`.
This program keeps each one in a queue file on disk (it survives restarts and network outages) and
POSTs them in signed batches to /api/v1/ingest, the same format the station's own SIM800L would use
(lakelink_cloud/lakelink/batch.py). A reading leaves the queue only after the server answered OK.

    python3 tools/station_uploader.py --port /dev/ttyUSB0 --key-file ~/lakelink/station.key
    (--key-file: one line, the station's 16-byte HMAC key as 32 hex chars; keep it mode 600)

Works on a Raspberry Pi or a laptop: needs python3 and pyserial only.
"""
import argparse
import hashlib
import hmac
import json
import os
import time
import urllib.error
import urllib.request
from pathlib import Path

import serial

MAX_BATCH = 64
POST_EVERY_S = 60


def log(msg):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), msg, flush=True)


def build_batch(station, items, key, now):
    lines = [f"WXB1 {station}"] + [f"{it['frame']} {max(0, int(now - it['at']))}" for it in items]
    signed = ("\n".join(lines) + "\n").encode()
    return signed + f"MAC {hmac.new(key, signed, hashlib.sha256).digest()[:8].hex()}\n".encode()


def frame_ok(frame, key):
    """The reading's own 4-byte HMAC (wx_frame v2 DATA, 33 bytes)."""
    return len(frame) == 33 and frame[1] == 0 and \
        hmac.compare_digest(hmac.new(key, frame[:29], hashlib.sha256).digest()[:4], frame[29:])


class Queue:
    def __init__(self, path):
        self.path = Path(path)
        self.items = []
        if self.path.exists():
            for line in self.path.read_text().splitlines():
                try:
                    self.items.append(json.loads(line))
                except ValueError:
                    pass

    def save(self):
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text("".join(json.dumps(it) + "\n" for it in self.items))
        os.replace(tmp, self.path)

    def add(self, frame_hex):
        self.items.append({"frame": frame_hex, "at": time.time()})
        self.save()

    def drop(self, n):
        del self.items[:n]
        self.save()


def post(url, body, timeout=30):
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "text/plain"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode(errors="replace").strip()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace").strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--key-file", required=True)
    ap.add_argument("--station", type=int, default=1)
    ap.add_argument("--url", default="https://api.d7tech.net/api/v1/ingest")
    ap.add_argument("--queue", default=str(Path.home() / "lakelink" / "upload_queue.jsonl"))
    ap.add_argument("--echo", action="store_true", help="print every station line")
    args = ap.parse_args()
    key = bytes.fromhex(Path(args.key_file).expanduser().read_text().strip())
    Path(args.queue).parent.mkdir(parents=True, exist_ok=True)
    q = Queue(args.queue)
    log(f"uploader: station {args.station} on {args.port} -> {args.url}; {len(q.items)} readings waiting")
    ser, buf, next_post, backoff = None, b"", 0.0, POST_EVERY_S
    while True:
        if ser is None:
            try:
                ser = serial.Serial()
                ser.port, ser.baudrate, ser.timeout, ser.dtr, ser.rts = args.port, 115200, 1, False, False
                ser.open()
                log(f"serial {args.port} open")
            except (serial.SerialException, OSError) as e:
                log(f"serial {args.port}: {e}; retrying in 10 s")
                ser = None
                time.sleep(10)
                continue
        try:
            buf += ser.read(4096)
        except (serial.SerialException, OSError) as e:
            log(f"serial lost ({e}); reopening")
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            continue
        *lines, buf = buf.split(b"\n")
        for raw in lines:
            line = raw.decode(errors="replace").strip()
            if args.echo and line:
                log(f"station: {line}")
            if line.startswith("WXF "):
                hexs = line[4:].strip().lower()
                try:
                    frame = bytes.fromhex(hexs)
                except ValueError:
                    continue
                if frame_ok(frame, key):
                    q.add(hexs)
                    log(f"reading queued (boot {int.from_bytes(frame[3:5], 'little')} seq "
                        f"{int.from_bytes(frame[5:7], 'little')}); {len(q.items)} waiting")
                else:
                    log("reading with a bad signature ignored (wrong key file?)")
        if q.items and time.time() >= next_post:
            items = q.items[:MAX_BATCH]
            try:
                status, text = post(args.url, build_batch(args.station, items, key, time.time()))
            except (urllib.error.URLError, OSError) as e:
                status, text = 0, str(e)
            if status == 200 and text.startswith("OK"):
                q.drop(len(items))
                log(f"uploaded {len(items)}: {text}; {len(q.items)} waiting")
                backoff, next_post = POST_EVERY_S, (0 if q.items else time.time() + POST_EVERY_S)
            else:
                log(f"upload failed ({status} {text}); {len(q.items)} kept, retry in {backoff} s")
                next_post = time.time() + backoff
                backoff = min(backoff * 2, 900)


if __name__ == "__main__":
    main()
