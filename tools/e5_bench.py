#!/usr/bin/env python3
"""Drive a Wio-E5 / LoRa-E5 (factory AT firmware) from a computer over USB-serial.

Bench tests and range tests without either ESP32:
  e5_bench.py -p PORT info                         firmware version / IDs, checks RF config is accepted
  e5_bench.py -p PORT at "AT+VER"                  send any AT command, print the reply
  e5_bench.py -p PORT rx                           print every packet with RSSI/SNR (sniffer)
  e5_bench.py -p PORT rx --ack [--mqtt localhost]  act as the gateway (ACK + optional MQTT publish)
  e5_bench.py -p PORT tx --count 20 --interval 5   act as the station: fake frames, wait for ACKs, report loss
  e5_bench.py -p PORT tx --hex 0100...             send exact bytes (replay a frame copied from an rx log)
  e5_bench.py -p PORT tx --corrupt                 send a frame with a broken MAC (gateway must reject it)

RF settings default to the values in lib/wxlink/wx_config.h; the HMAC key comes from include/secrets.h.
"""

import argparse
import json
import random
import re
import sys
import time
from datetime import datetime, timezone

import serial

import wxframe as wf

ACK_TIMEOUT_S = 2.5


def config_default(name, fallback):
    text = (wf.ROOT / "lib" / "wxlink" / "wx_config.h").read_text()
    m = re.search(rf"#define\s+{name}\s+(\"[^\"]*\"|\S+)", text)
    return m.group(1).strip('"') if m else fallback


class E5Error(RuntimeError):
    pass


class E5Disconnected(E5Error):
    """The serial port went away (USB unplugged, cable fault)."""


_RX_META = re.compile(r"\+TEST: LEN:\s*(\d+), RSSI:\s*(-?\d+), SNR:\s*(-?[\d.]+)")
_RX_DATA = re.compile(r'\+TEST: RX "((?:[0-9A-Fa-f]{2})*)"')


class E5:
    def __init__(self, port, baud=9600, verbose=False):
        self.port, self.baud = port, baud
        self.s = serial.Serial(port, baud, timeout=0.05)
        self.verbose = verbose
        self.buf = b""
        self.meta = None
        self.pending = []  # packets that arrived while a command was running; returned first by poll_packet

    def reopen(self):
        try:
            self.s.close()
        except Exception:
            pass
        self.s = serial.Serial(self.port, self.baud, timeout=0.05)
        self.buf, self.meta = b"", None

    def _read(self, n):
        try:
            return self.s.read(n)
        except (serial.SerialException, OSError) as err:
            raise E5Disconnected(f"serial read failed: {err}") from err

    def _write(self, data):
        try:
            self.s.write(data)
        except (serial.SerialException, OSError) as err:
            raise E5Disconnected(f"serial write failed: {err}") from err

    def _stash_if_packet(self, line):
        """Keeps received-packet lines that show up during a command instead of losing them."""
        m = _RX_META.match(line)
        if m:
            self.meta = (int(m.group(2)), float(m.group(3)))
            return True
        m = _RX_DATA.match(line)
        if m:
            rssi, snr = self.meta or (None, None)
            self.meta = None
            try:
                self.pending.append((bytes.fromhex(m.group(1)), rssi, snr))
            except ValueError:
                pass  # corrupted line: drop it, the sender will retry
            return True
        return False

    def _log(self, direction, text):
        if self.verbose:
            print(f"  {direction} {text}")

    def readline(self, deadline):
        while time.monotonic() < deadline:
            if b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.decode(errors="replace").strip()
                if text:
                    self._log("<", text)
                    return text
                continue
            self.buf += self._read(256)
        return None

    def command(self, cmd, expect, timeout=1.0):
        """Sends cmd and returns all reply lines up to the one starting with `expect`."""
        # Drain what is already buffered first, keeping any received packets.
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            self._stash_if_packet(line.decode(errors="replace").strip())
        self._log(">", cmd)
        self._write(cmd.encode() + b"\r\n")
        lines, deadline = [], time.monotonic() + timeout
        while (line := self.readline(deadline)) is not None:
            if self._stash_if_packet(line):
                continue
            lines.append(line)
            if line.startswith(expect):
                return lines
            if line.startswith("+") and "ERROR" in line:
                raise E5Error(f"{cmd} -> {line}")
        raise E5Error(f"timeout waiting for '{expect}' after {cmd} (got {lines})")

    def wake(self):
        for _ in range(3):
            self._write(b"\xff" * 4)
            time.sleep(0.01)
            try:
                return self.command("AT", "+AT: OK", 0.5)
            except E5Error:
                pass
        raise E5Error("module does not answer AT: check port, baud (9600), TX/RX crossing, power")

    def configure(self, freq, sf, power):
        self.wake()
        self.command("AT+MODE=TEST", "+MODE: TEST")
        return self.command(f"AT+TEST=RFCFG,{freq},{sf},125,12,15,{power},ON,OFF,OFF", "+TEST: RFCFG")

    def tx(self, payload: bytes, timeout=4.0):
        self.command(f'AT+TEST=TXLRPKT,"{payload.hex().upper()}"', "+TEST: TX DONE", timeout)

    def start_rx(self):
        self.meta = None
        self.command("AT+TEST=RXLRPKT", "+TEST: RXLRPKT")

    def poll_packet(self, timeout):
        """Returns (bytes, rssi, snr) for the next received packet, or None on timeout."""
        if self.pending:
            return self.pending.pop(0)
        deadline = time.monotonic() + timeout
        while (line := self.readline(deadline)) is not None:
            if self._stash_if_packet(line) and self.pending:
                return self.pending.pop(0)
        return None


def fake_reading(station_id, boot_id, seq, rain):
    return wf.Data(station_id, boot_id, seq,
                   temp_c_x100=2150 + random.randint(-300, 300), rh_x100=random.randint(3000, 9000),
                   press_hpa_x10=10132 + random.randint(-20, 20), wind_ms_x100=random.randint(0, 800),
                   gust_ms_x100=random.randint(800, 1200), wind_dir_deg=random.randint(0, 359),
                   rain_total_mm_x10=rain, batt_mv=random.randint(3700, 4150),
                   soil_pct_x10=random.randint(200, 600), lightning_total=0, flags=wf.FLAG_FAKE)


def cmd_info(e5, args):
    e5.wake()
    for cmd, expect in (("AT+VER", "+VER"), ("AT+ID", "+ID: AppEui")):
        for line in e5.command(cmd, expect):
            print(line)
    for line in e5.configure(args.freq, args.sf, args.power):
        print(line)
    print("RF config accepted")


def cmd_at(e5, args):
    e5._write(args.command.encode() + b"\r\n")
    deadline = time.monotonic() + args.wait
    while (line := e5.readline(deadline)) is not None:
        print(line)


def cmd_rx(e5, args):
    key = wf.load_key()
    mqtt = None
    if args.mqtt:
        import paho.mqtt.client as paho
        try:
            mqtt = paho.Client(paho.CallbackAPIVersion.VERSION2)
        except AttributeError:  # paho < 2
            mqtt = paho.Client()
        mqtt.connect(args.mqtt, args.mqtt_port)
        mqtt.loop_start()
    e5.configure(args.freq, args.sf, args.power)
    e5.start_rx()
    print(f"listening on {args.freq} MHz {args.sf} ({'gateway mode' if args.ack else 'sniffer'}), Ctrl-C to stop")
    stations = {}
    while True:
        pkt = e5.poll_packet(1.0)
        if not pkt:
            continue
        data, rssi, snr = pkt
        stamp = datetime.now(timezone.utc).strftime("%H:%M:%S")
        try:
            frame = wf.decode(data, key)
        except wf.DecodeError as err:
            print(f"{stamp} rssi={rssi} snr={snr} REJECTED {err}: {data.hex().upper()}")
            continue
        if isinstance(frame, wf.Ack):
            print(f"{stamp} rssi={rssi} snr={snr} ack {frame}")
            continue
        last = stations.get(frame.station_id)
        same_boot = last is not None and last[0] == frame.boot_id
        duplicate = same_boot and frame.seq == last[1]
        stale = same_boot and ((frame.seq - last[1]) & 0xFFFF) >= 0x8000
        status = "STALE (dropped)" if stale else "DUPLICATE (re-acked)" if duplicate else "accepted"
        print(f"{stamp} rssi={rssi} snr={snr} station={frame.station_id} boot={frame.boot_id:04X} "
              f"seq={frame.seq} {status}\n  hex {data.hex().upper()}")
        if stale:
            continue
        if args.ack:
            e5.tx(wf.encode_ack(wf.Ack(frame.station_id, frame.boot_id, frame.seq, int(duplicate)), key))
            e5.start_rx()
        if duplicate:
            continue
        stations[frame.station_id] = (frame.boot_id, frame.seq)
        payload = frame.to_json_dict()
        payload.update(time=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), rssi=rssi, snr=snr)
        print("  " + json.dumps(payload))
        if mqtt:
            mqtt.publish(f"weather/{frame.station_id}/data", json.dumps(payload), retain=True)


def cmd_tx(e5, args):
    key = wf.load_key()
    e5.configure(args.freq, args.sf, args.power)
    boot_id = random.randint(0, 0xFFFF)
    sent = acked = 0
    rain = 0
    try:
        for i in range(args.count if args.count > 0 else 10**9):
            if i:
                time.sleep(args.interval)
            if args.hex:
                frame_bytes = bytes.fromhex(args.hex)
                try:  # ACK matching needs the replayed frame's identity
                    d = wf.decode(frame_bytes, key)
                    ident = (d.station_id, d.boot_id, d.seq)
                except wf.DecodeError:
                    ident = None
            else:
                rain += random.choice((0, 0, 0, 2))
                ident = (args.station_id, boot_id, i + 1)
                frame_bytes = wf.encode_data(fake_reading(*ident, rain), key)
            if args.corrupt:
                frame_bytes = bytearray(frame_bytes)
                frame_bytes[8] ^= 0x01
                frame_bytes = bytes(frame_bytes)
            e5.tx(frame_bytes)
            sent += 1
            line = f"tx #{sent} {frame_bytes.hex().upper()}"
            if not args.no_ack:
                e5.start_rx()
                got = wait_for_ack(e5, key, ident) if ident else None
                if got:
                    acked += 1
                    line += f"  ACK rssi={got[0]} snr={got[1]}"
                else:
                    line += "  no ack"
            print(line)
    except KeyboardInterrupt:
        pass
    if not args.no_ack and sent:
        print(f"sent {sent}, acked {acked}, loss {100 * (sent - acked) / sent:.0f}%")


def wait_for_ack(e5, key, ident):
    deadline = time.monotonic() + ACK_TIMEOUT_S
    while (remaining := deadline - time.monotonic()) > 0:
        pkt = e5.poll_packet(remaining)
        if not pkt:
            return None
        try:
            frame = wf.decode(pkt[0], key)
        except wf.DecodeError:
            continue
        if isinstance(frame, wf.Ack) and (frame.station_id, frame.boot_id, frame.seq) == ident:
            return pkt[1], pkt[2]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", required=True, help="e.g. /dev/cu.usbserial-XXXX")
    ap.add_argument("--freq", default=config_default("WX_FREQ_MHZ", "868.1"))
    ap.add_argument("--sf", default=config_default("WX_SF", "SF9"))
    ap.add_argument("--power", type=int, default=int(config_default("WX_TX_POWER_DBM", "14")))
    ap.add_argument("-v", "--verbose", action="store_true", help="show raw AT traffic")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    p = sub.add_parser("at")
    p.add_argument("command")
    p.add_argument("--wait", type=float, default=1.0)
    p = sub.add_parser("rx")
    p.add_argument("--ack", action="store_true", help="ACK data frames like the gateway does")
    p.add_argument("--mqtt", metavar="HOST", help="also publish accepted readings to this MQTT broker")
    p.add_argument("--mqtt-port", type=int, default=1883)
    p = sub.add_parser("tx")
    p.add_argument("--count", type=int, default=1, help="frames to send, 0 = until Ctrl-C")
    p.add_argument("--interval", type=float, default=5.0, help="seconds between frames")
    p.add_argument("--station-id", type=int, default=99)
    p.add_argument("--hex", help="send these exact bytes instead of a fake reading")
    p.add_argument("--corrupt", action="store_true", help="flip one byte after signing")
    p.add_argument("--no-ack", action="store_true", help="do not wait for ACKs")
    args = ap.parse_args()

    e5 = E5(args.port, verbose=args.verbose)
    try:
        {"info": cmd_info, "at": cmd_at, "rx": cmd_rx, "tx": cmd_tx}[args.cmd](e5, args)
    except E5Error as err:
        sys.exit(f"error: {err}")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
