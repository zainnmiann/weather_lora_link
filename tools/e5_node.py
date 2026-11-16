#!/usr/bin/env python3
"""One node of the hop chain, driving a LoRa-E5 (factory AT firmware) from a computer.

  station: makes a (fake) signed reading every --interval s and hands it to --next
  relay:   acknowledges packets addressed to it and forwards them to --next
  gateway: acknowledges packets addressed to it, checks the station's signature, prints JSON
           (and optionally publishes it over MQTT)

Each hop is acknowledged separately (HACK). Packets that cannot be delivered stay queued and
are retried later (store-and-forward), so a broken hop delays readings instead of losing them.
Transmissions respect the EU868 duty-cycle limit of the chosen frequency (1 %, or 10 % in
869.4-869.65 MHz). Radio or serial failures are recovered from instead of exiting.

Example chain on one computer (three boards on USB):
  e5_node.py -p /dev/cu.usbserial-A --role gateway --id 3
  e5_node.py -p /dev/cu.usbserial-B --role relay   --id 2 --next 3
  e5_node.py -p /dev/cu.usbserial-C --role station --id 1 --next 2 --interval 30

--fault-* options inject failures for testing only.
"""

import argparse
import hashlib
import json
import math
import os
import random
import signal
import sys
import time
from collections import deque
from datetime import datetime, timezone

import hopnet as hn
import wxframe as wf
from e5_bench import E5, E5Disconnected, E5Error, config_default, fake_reading

ACK_TIMEOUT_S = 2.5
FAST_RETRIES = 3          # quick retries before falling back to store-and-forward
BACKOFF_S = (5, 10, 20, 40, 60)  # later waits while the next hop is unreachable; reset by any success
MAX_QUEUE = 500           # oldest readings are dropped beyond this
SEEN_TTL_S = 600
E2E_BOOTS_KEPT = 16       # gateway remembers the last seq of this many boots per station (replay protection)


def ts():
    return datetime.now().strftime("%H:%M:%S")


def lora_airtime_s(n_bytes, sf, bw_hz=125000, preamble=12, cr=1):
    """Semtech SX126x time-on-air: explicit header, CRC on, coding rate 4/(4+cr)."""
    tsym = (2 ** sf) / bw_hz
    de = 1 if (sf >= 11 and bw_hz == 125000) else 0
    n = 8 + max(math.ceil((8 * n_bytes - 4 * sf + 28 + 16) / (4 * (sf - 2 * de))) * (cr + 4), 0)
    return (preamble + 4.25) * tsym + n * tsym


# EU 863-870 MHz short-range-device sub-bands (ERC/REC 70-03 annex 1, ETSI EN 300 220): (low, high, duty)
SUB_BANDS = [(863.0, 865.0, 0.001), (865.0, 868.0, 0.01), (868.0, 868.6, 0.01), (868.7, 869.2, 0.001),
             (869.4, 869.65, 0.10), (869.7, 870.0, 0.01)]
HALF_BW_MHZ = 0.0625  # a 125 kHz channel must fit inside the sub-band


def duty_fraction(freq_mhz):
    f = float(freq_mhz)
    for lo, hi, duty in SUB_BANDS:
        if lo <= f - HALF_BW_MHZ and f + HALF_BW_MHZ <= hi:
            return duty
    raise ValueError(f"{f} MHz (125 kHz channel) does not fit inside an EU868 sub-band")


class DutyCycle:
    """Sliding one-hour airtime budget (ETSI EN 300 220 sub-band limits), with 10 % safety margin.
    Uses wall-clock time so the log can be saved and survives a restart."""

    def __init__(self, freq_mhz, budget_s=None):
        self.fraction = duty_fraction(freq_mhz)
        legal = 3600 * self.fraction * 0.9
        if budget_s is not None and not 0 <= budget_s <= legal:
            raise ValueError(f"budget {budget_s} s must be between 0 and the legal {legal:.1f} s per hour")
        self.budget = legal if budget_s is None else budget_s
        self.log = deque()  # (unix time, seconds)

    def used(self):
        now = time.time()
        while self.log and now - self.log[0][0] > 3600:
            self.log.popleft()
        return sum(s for _, s in self.log)

    def allows(self, seconds):
        return self.used() + seconds <= self.budget

    def record(self, seconds):
        self.log.append((time.time(), seconds))


class Node:
    def __init__(self, e5, key, args, mqtt=None):
        self.e5, self.key, self.mqtt = e5, key, mqtt
        self.role, self.me, self.nxt = args.role, args.id, args.next
        self.sf = int(args.sf.upper().lstrip("SF"))
        self.duty = DutyCycle(args.freq, args.duty_budget)
        self.faults = random.Random(args.seed)
        self.args = args
        self.lseq = random.randint(0, 0xFFFF)
        self.outq = deque()  # [payload, hops, lseq, attempts, next_try]
        self.seen = {}       # (src, lseq, payload digest) -> time; forward each packet once
        self.e2e_seen = {}   # gateway: station -> (boot, seq)
        self.duty_blocked = False
        self.inflight = None
        self.outbox = deque()  # gateway: JSON readings not yet confirmed by the MQTT broker
        self.next_publish = 0.0
        self.stats = dict(sent=0, acked=0, retries=0, fwd_in=0, dup_in=0, delivered=0, recoveries=0,
                          duty_waits=0, airtime_s=0.0, faults=0, dropped=0, refused_duty=0, published=0,
                          unexpected_errors=0)
        self.state_path = args.state
        self.load_state()

    # ---- persistence: queue, link sequence and duplicate memory survive a restart ----------
    def load_state(self):
        if not self.state_path or not os.path.exists(self.state_path):
            return
        with open(self.state_path) as f:
            s = json.load(f)
        self.lseq = s["lseq"]
        self.outq = deque([bytes.fromhex(p), h, l, 0, 0.0] for p, h, l in s["outq"])
        now = time.monotonic()
        self.seen = {(a, b, bytes.fromhex(c)): now for a, b, c in s["seen"]}
        self.e2e_seen = {int(k): {int(b): q for b, q in v} for k, v in s["e2e_seen"].items()}
        self.outbox = deque(s.get("outbox", []))
        self.duty.log = deque((a, b) for a, b in s.get("duty", []) if time.time() - a < 3600)
        self.log(f"restored state: {len(self.outq)} queued, {len(self.seen)} remembered packets")

    def save_state(self):
        if not self.state_path:
            return
        s = dict(lseq=self.lseq, outq=[[p.hex(), h, l] for p, h, l, *_ in self.outq],
                 seen=[[a, b, c.hex()] for a, b, c in self.seen],
                 e2e_seen={k: [[b, q] for b, q in v.items()] for k, v in self.e2e_seen.items()},
                 duty=list(self.duty.log), outbox=list(self.outbox))
        tmp = self.state_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(s, f)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.state_path)  # atomic: a crash leaves either the old or the new file

    def log(self, msg):
        print(f"{ts()} [{self.role} {self.me}] {msg}", flush=True)

    def fault(self, prob):
        hit = prob > 0 and self.faults.random() < prob
        if hit:
            self.stats["faults"] += 1
        return hit

    def transmit(self, frame):
        if self.fault(self.args.fault_radio_error):
            raise E5Error("injected radio error")
        air = lora_airtime_s(len(frame), self.sf)
        self.e5.tx(frame)
        self.duty.record(air)
        self.stats["airtime_s"] += air
        self.save_state()  # the airtime log must survive a restart (regulatory)
        self.e5.start_rx()

    # ---- sending side -----------------------------------------------------------------------
    def enqueue(self, payload, hops):
        if len(self.outq) >= MAX_QUEUE:
            # drop the oldest packet that is not currently in flight
            victim = next(i for i, it in enumerate(self.outq) if it is not self.inflight)
            del self.outq[victim]
            self.stats["dropped"] += 1
            self.log("queue full, dropped oldest waiting packet")
        self.lseq = (self.lseq + 1) & 0xFFFF
        self.outq.append([payload, hops, self.lseq, 0, 0.0])
        self.save_state()

    def pump(self):
        if not self.outq or self.nxt is None:
            return
        item = self.outq[0]
        payload, hops, lseq, attempts, next_try = item
        if time.monotonic() < next_try:
            return
        frame = hn.encode_fwd(self.me, self.nxt, lseq, hops, payload, self.key)
        if not self.duty.allows(lora_airtime_s(len(frame), self.sf)):
            if not self.duty_blocked:
                self.log(f"duty-cycle budget reached ({self.duty.used():.1f}/{self.duty.budget:.0f} s per hour); "
                         f"holding {len(self.outq)} packet(s)")
                self.stats["duty_waits"] += 1
            self.duty_blocked = True
            return
        self.duty_blocked = False
        self.inflight = item
        try:
            self.transmit(frame)
            self.stats["sent"] += 1
            got = self.wait_hack(lseq)
        finally:
            self.inflight = None
        if got:
            if any(it is item for it in self.outq):  # remove exactly the packet that was acknowledged
                for i, it in enumerate(self.outq):
                    if it is item:
                        del self.outq[i]
                        break
            self.save_state()
            self.stats["acked"] += 1
            self.log(f"-> node {self.nxt} ok (lseq {lseq}, hop ack rssi={got[0]} snr={got[1]}, queue {len(self.outq)})")
            return
        item[3] = attempts + 1
        self.stats["retries"] += 1
        if item[3] < FAST_RETRIES:
            item[4] = time.monotonic() + random.uniform(0.3, 1.0)
        else:
            step = BACKOFF_S[min(item[3] - FAST_RETRIES, len(BACKOFF_S) - 1)]
            wait = step * random.uniform(0.7, 1.3)
            item[4] = time.monotonic() + wait
            self.log(f"-> node {self.nxt} unreachable after {item[3]} tries; kept, retrying in {wait:.0f}s "
                     f"(queue {len(self.outq)})")

    def wait_hack(self, lseq):
        deadline = time.monotonic() + ACK_TIMEOUT_S
        while (remaining := deadline - time.monotonic()) > 0:
            pkt = self.e5.poll_packet(remaining)
            if not pkt:
                return None
            link = hn.decode(pkt[0], self.key)
            if link is None or link.dst != self.me:
                continue
            if link.type == hn.T_HACK and link.src == self.nxt and link.lseq == lseq:
                return pkt[1], pkt[2]
            if link.type == hn.T_FWD:
                self.on_fwd(link, pkt[1], pkt[2])
        return None

    # ---- receiving side ---------------------------------------------------------------------
    def on_fwd(self, link, rssi, snr):
        if self.fault(self.args.fault_deaf):
            self.log(f"[fault] ignored packet from node {link.src} lseq {link.lseq}")
            return
        if not self.duty.allows(lora_airtime_s(hn.HDR_LEN + wf.MAC_LEN, self.sf)):
            # Without airtime for the acknowledgement we must not take custody: the sender keeps the packet.
            self.stats["refused_duty"] += 1
            self.log(f"<- node {link.src} lseq {link.lseq} refused: duty-cycle budget used up (sender keeps it)")
            return
        # Take custody first (stored durably), acknowledge second: a crash in between can only
        # cause a harmless repeat, never a loss.
        now = time.monotonic()
        self.seen = {k: t for k, t in self.seen.items() if now - t < SEEN_TTL_S}
        key = (link.src, link.lseq, hashlib.sha256(link.payload).digest()[:8])
        repeat = key in self.seen
        if not repeat:
            self.seen[key] = now
            self.stats["fwd_in"] += 1
            if self.role == "relay":
                if link.hops >= hn.MAX_HOPS:
                    self.log(f"<- node {link.src} dropped: hop limit")
                else:
                    self.enqueue(link.payload, link.hops + 1)  # saves state
                    self.log(f"<- node {link.src} lseq {link.lseq} rssi={rssi} snr={snr}, forwarding to {self.nxt}")
            elif self.role == "gateway":
                self.deliver(link, rssi, snr)
                self.save_state()
        else:
            self.stats["dup_in"] += 1
            self.log(f"<- node {link.src} repeat lseq {link.lseq} (our ack was lost), re-acked")
        if self.fault(self.args.fault_drop_hack):
            self.log(f"[fault] not acknowledging node {link.src} lseq {link.lseq}")
        else:
            self.transmit(hn.encode_hack(self.me, link.src, link.lseq, self.key))

    def deliver(self, link, rssi, snr):
        try:
            d = wf.decode(link.payload, self.key)
        except wf.DecodeError as err:
            self.log(f"<- node {link.src}: payload REJECTED ({err})")
            return
        if not isinstance(d, wf.Data):
            return
        boots = self.e2e_seen.setdefault(d.station_id, {})  # boot_id -> highest seq delivered
        top = boots.get(d.boot_id)
        if top is not None and (d.seq == top or ((d.seq - top) & 0xFFFF) >= 0x8000):
            self.log(f"<- station {d.station_id} boot {d.boot_id} seq {d.seq}: already have it, ignored")
            return
        boots.pop(d.boot_id, None)
        boots[d.boot_id] = d.seq  # most recent boot last
        while len(boots) > E2E_BOOTS_KEPT:
            boots.pop(next(iter(boots)))
        self.stats["delivered"] += 1
        out = d.to_json_dict()
        out.update(time=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), hops=link.hops,
                   last_hop_from=link.src, rssi=rssi, snr=snr)
        self.log(f"DELIVERED station {d.station_id} boot {d.boot_id} seq {d.seq} via {link.hops} hop(s): {json.dumps(out)}")
        if self.mqtt:
            self.outbox.append([f"weather/{d.station_id}/data", json.dumps(out)])  # saved by the caller

    def flush_outbox(self):
        """Publishes queued readings with QoS 1; a reading leaves the outbox only once the broker confirmed it."""
        if not self.mqtt or not self.outbox or time.monotonic() < self.next_publish:
            return
        if not self.mqtt.is_connected():
            self.next_publish = time.monotonic() + 5
            return
        topic, payload = self.outbox[0]
        try:
            info = self.mqtt.publish(topic, payload, qos=1, retain=True)
            info.wait_for_publish(timeout=3)
            ok = info.is_published()
        except (RuntimeError, ValueError) as err:
            ok = False
            self.log(f"MQTT publish failed: {err}")
        if ok:
            self.outbox.popleft()
            self.stats["published"] += 1
            self.save_state()
        else:
            self.next_publish = time.monotonic() + 5
            self.log(f"MQTT not confirming; {len(self.outbox)} reading(s) kept for retry")

    def held_ids(self):
        """(boot, seq) of every reading still queued here, for the test checker."""
        ids = []
        for payload, *_ in self.outq:
            try:
                d = wf.decode(payload, self.key)
                ids.append(f"{d.boot_id}:{d.seq}")
            except wf.DecodeError:
                ids.append("?")
        return ids

    # ---- recovery ---------------------------------------------------------------------------
    def bring_up(self):
        """(Re)configures the radio; on a lost serial port keeps reopening until it is back."""
        delay = 1.0
        while True:
            try:
                self.e5.configure(self.args.freq, self.args.sf, self.args.power)
                self.e5.start_rx()
                return
            except E5Disconnected as err:
                self.log(f"serial port lost ({err}); reopening in {delay:.0f}s")
                time.sleep(delay)
                try:
                    self.e5.reopen()
                except Exception as e:  # port not back yet
                    self.log(f"reopen failed: {e}")
            except E5Error as err:
                self.log(f"radio not responding ({err}); retrying in {delay:.0f}s")
                time.sleep(delay)
            delay = min(delay * 2, 30)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", required=True, help="/dev/cu.* path, or dev:lora1 / sn:<USB serial>")
    ap.add_argument("--role", choices=["station", "relay", "gateway"], required=True)
    ap.add_argument("--id", type=int, required=True, help="this node's id (1-254)")
    ap.add_argument("--next", type=int, help="next hop's node id (station and relay)")
    ap.add_argument("--interval", type=float, default=30, help="station: seconds between readings")
    ap.add_argument("--station-id", type=int, default=1, help="station: id inside the signed reading")
    ap.add_argument("--telem-port", help="station: read real readings from the weather station's USB port (TELEM lines)")
    ap.add_argument("--gen-for", type=float, default=0, help="station TEST: stop making readings after S seconds (keeps sending the queue)")
    ap.add_argument("--mqtt", metavar="HOST", help="gateway: also publish to this MQTT broker")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--freq", default=config_default("WX_FREQ_MHZ", "868.1"))
    ap.add_argument("--sf", default=config_default("WX_SF", "SF9"))
    ap.add_argument("--power", type=int, default=int(config_default("WX_TX_POWER_DBM", "14")))
    ap.add_argument("--state", help="file that keeps queue and duplicate memory across restarts")
    ap.add_argument("--seed", type=int, default=None, help="seed for fault injection")
    ap.add_argument("--duty-budget", type=float, default=None, help="TEST: hourly airtime budget in seconds (only to make it smaller)")
    ap.add_argument("--fault-drop-hack", type=float, default=0.0, help="TEST: probability of not acknowledging")
    ap.add_argument("--fault-deaf", type=float, default=0.0, help="TEST: probability of ignoring a received packet")
    ap.add_argument("--fault-radio-error", type=float, default=0.0, help="TEST: probability of a fake radio error per transmit")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if args.role != "gateway" and args.next is None:
        ap.error("--next is required for station and relay")
    try:
        DutyCycle(args.freq, args.duty_budget)
    except ValueError as err:
        ap.error(str(err))
    if args.role == "gateway" and args.next is not None:
        ap.error("--next does not apply to the gateway")
    if not 1 <= args.id <= 254 or (args.next is not None and not 1 <= args.next <= 254) or args.next == args.id:
        ap.error("node ids must be 1-254 and differ from --next")

    mqtt = None
    if args.mqtt:
        import paho.mqtt.client as paho
        try:
            mqtt = paho.Client(paho.CallbackAPIVersion.VERSION2)
        except AttributeError:
            mqtt = paho.Client()
        mqtt.connect_async(args.mqtt, args.mqtt_port)  # does not fail if the broker is down; paho keeps retrying
        mqtt.loop_start()

    signal.signal(signal.SIGTERM, signal.default_int_handler)
    from ports import resolve
    args.port = resolve(args.port)
    if args.telem_port:
        args.telem_port = resolve(args.telem_port)
    key = wf.load_key()
    e5 = E5(args.port, verbose=args.verbose)
    node = Node(e5, key, args, mqtt)
    boot = random.randint(0, 0xFFFF)
    seq, rain = 0, 0
    telem, telem_restarts = None, 0
    if args.telem_port:
        if args.role != "station":
            ap.error("--telem-port applies to the station only")
        from telem_source import TelemSource
        telem = TelemSource(args.telem_port, log=lambda m: print(f"{ts()} [station {args.id}] {m}", flush=True))
    try:
        node.bring_up()
        node.log(f"up on {args.freq} MHz {args.sf} {args.power} dBm, duty budget {node.duty.budget:.0f} s/h"
                 + (f", next hop {args.next}" if args.next else ""))
        next_reading = started = time.monotonic()
        while True:
            try:
                generating = not args.gen_for or time.monotonic() - started < args.gen_for
                if telem and telem.latest is None and time.monotonic() - started < 30:
                    generating = False  # wait for the weather station's first line instead of sending an empty reading
                    next_reading = max(next_reading, time.monotonic())
                if args.role == "station" and generating and time.monotonic() >= next_reading:
                    if telem:
                        fields, flags, restarts, source = telem.take()  # source = the exact line these values came from
                        if source is not None:
                            node.log(f"source TELEM {json.dumps(source)}")
                        if restarts != telem_restarts:  # weather station rebooted: its rain total restarted
                            telem_restarts = restarts
                            boot, seq = random.randint(0, 0xFFFF), 0
                            node.log(f"weather station restarted: new boot {boot}")
                        seq = (seq + 1) & 0xFFFF
                        data = wf.Data(args.station_id, boot, seq, flags=flags, **fields)
                        node.enqueue(wf.encode_data(data, key), 1)
                        node.log(f"reading boot {boot} seq {seq} queued (queue {len(node.outq)}) "
                                 f"{json.dumps(data.to_json_dict())}")
                    else:
                        seq = (seq + 1) & 0xFFFF
                        rain += random.choice((0, 0, 0, 2))
                        node.enqueue(wf.encode_data(fake_reading(args.station_id, boot, seq, rain), key), 1)
                        node.log(f"reading boot {boot} seq {seq} queued (queue {len(node.outq)})")
                    next_reading += args.interval
                node.pump()
                node.flush_outbox()
                pkt = e5.poll_packet(0.2)
                if pkt:
                    link = hn.decode(pkt[0], key)
                    if link and link.dst == args.id and link.type == hn.T_FWD:
                        node.on_fwd(link, pkt[1], pkt[2])
            except E5Error as err:  # includes E5Disconnected
                node.stats["recoveries"] += 1
                node.log(f"RECOVERING from: {err}")
                node.bring_up()
                node.log("recovered, radio listening again")
            except (KeyboardInterrupt, SystemExit):
                raise
            except Exception as err:  # anything unforeseen: log it, re-initialise, keep the node alive
                import traceback
                node.stats["unexpected_errors"] += 1
                node.log(f"UNEXPECTED ERROR {type(err).__name__}: {err}\n{traceback.format_exc()}")
                time.sleep(1)
                node.bring_up()
    except KeyboardInterrupt:
        node.save_state()
        node.log(f"held {node.held_ids()}")
        node.log(f"stopped; queue {len(node.outq)}; stats {json.dumps({k: round(v, 2) if isinstance(v, float) else v for k, v in node.stats.items()})}")


if __name__ == "__main__":
    main()
