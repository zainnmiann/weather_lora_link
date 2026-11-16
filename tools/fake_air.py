#!/usr/bin/env python3
"""Simulates N Wio-E5 modules (TEST-mode AT subset) on pseudo-terminals sharing one radio channel.

Lets e5_bench.py (or anything speaking the AT protocol over a serial port) be tested without
hardware. A TXLRPKT on one module is delivered to every other module that is in RXLRPKT mode
with the same RFCFG, unless dropped by --loss.

  python3 tools/fake_air.py --modules 2 --loss 0.2
  -> prints one /dev/ttysNNN path per module; pass them to e5_bench.py -p
"""

import argparse
import heapq
import itertools
import os
import random
import re
import select
import time
import tty

AIRTIME_S = 0.27  # ~SF9/125 kHz for a 33-byte frame


class Module:
    def __init__(self, idx):
        self.idx = idx
        self.master, slave = os.openpty()
        tty.setraw(slave)  # no echo, no CR/LF translation: behave like a real UART
        self.path = os.ttyname(slave)
        self.slave = slave  # keep open so the pty survives client reconnects
        self.buf = b""
        self.mode = None
        self.rfcfg = None
        self.rx = False

    def send(self, line):
        os.write(self.master, line.encode() + b"\r\n")


class Air:
    def __init__(self, n, loss, seed):
        self.modules = [Module(i) for i in range(n)]
        self.loss = loss
        self.rng = random.Random(seed)
        self.events = []  # (time, counter, fn)
        self.counter = itertools.count()

    def later(self, delay, fn):
        heapq.heappush(self.events, (time.monotonic() + delay, next(self.counter), fn))

    def handle(self, m, cmd):
        if cmd == "AT":
            m.send("+AT: OK")
        elif cmd == "AT+VER":
            m.send("+VER: 4.0.11-sim")
        elif cmd == "AT+ID":
            m.send(f"+ID: DevAddr, 00:00:00:{m.idx:02X}")
            m.send(f"+ID: DevEui, 2C:F7:F1:20:00:00:00:{m.idx:02X}")
            m.send("+ID: AppEui, 80:00:00:00:00:00:00:06")
        elif cmd == "AT+MODE=TEST":
            m.mode, m.rx = "TEST", False
            m.send("+MODE: TEST")
        elif cmd == "AT+LOWPOWER":
            m.rx = False
            m.send("+LOWPOWER: SLEEP")
        elif cmd.startswith("AT+TEST=RFCFG,") and m.mode == "TEST":
            m.rfcfg, m.rx = cmd.split(",", 1)[1], False
            f, sf, bw, txpr, rxpr, pw, crc, iq, net = m.rfcfg.split(",")
            m.send(f"+TEST: RFCFG F:{int(float(f) * 1e6)}, {sf}, BW{bw}K, TXPR:{txpr}, RXPR:{rxpr}, "
                   f"POW:{pw}dBm, CRC:{crc}, IQ:{iq}, NET:{net}")
        elif cmd == "AT+TEST=RXLRPKT" and m.mode == "TEST":
            m.rx = True
            m.send("+TEST: RXLRPKT")
        elif (tx := re.fullmatch(r'AT\+TEST=TXLRPKT,"([0-9A-Fa-f]*)"', cmd)) and m.mode == "TEST":
            m.rx = False
            payload = tx.group(1).upper()
            m.send(f'+TEST: TXLRPKT "{payload}"')
            self.later(AIRTIME_S, lambda: self.deliver(m, payload))
        else:
            m.send(f"+{cmd[3:].split('=')[0] or 'AT'}: ERROR(-1)" if cmd.startswith("AT") else "+AT: ERROR(-1)")

    def deliver(self, sender, payload):
        sender.send("+TEST: TX DONE")
        for m in self.modules:
            if m is sender or not m.rx or m.rfcfg != sender.rfcfg:
                continue
            if self.rng.random() < self.loss:
                print(f"[air] module {sender.idx} -> {m.idx}: DROPPED")
                continue
            m.send(f"+TEST: LEN:{len(payload) // 2}, RSSI:{self.rng.randint(-110, -60)}, "
                   f"SNR:{self.rng.uniform(-5, 10):.1f}")
            m.send(f'+TEST: RX "{payload}"')
            print(f"[air] module {sender.idx} -> {m.idx}: {len(payload) // 2} bytes")

    def run(self):
        by_fd = {m.master: m for m in self.modules}
        while True:
            timeout = max(0.0, self.events[0][0] - time.monotonic()) if self.events else 0.5
            ready, _, _ = select.select(list(by_fd), [], [], timeout)
            for fd in ready:
                m = by_fd[fd]
                try:
                    m.buf += os.read(fd, 4096)
                except OSError:
                    continue
                m.buf = m.buf.replace(b"\xff", b"")  # wake bytes
                while b"\n" in m.buf:
                    line, m.buf = m.buf.split(b"\n", 1)
                    cmd = line.decode(errors="replace").strip()
                    if cmd:
                        self.handle(m, cmd)
            while self.events and self.events[0][0] <= time.monotonic():
                heapq.heappop(self.events)[2]()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--modules", type=int, default=2)
    ap.add_argument("--loss", type=float, default=0.0, help="probability each delivery is dropped")
    ap.add_argument("--seed", type=int, default=None)
    args = ap.parse_args()
    air = Air(args.modules, args.loss, args.seed)
    for m in air.modules:
        print(f"module {m.idx}: {m.path}", flush=True)
    try:
        air.run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
