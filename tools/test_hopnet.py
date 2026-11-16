"""Unit tests for the hop link layer, airtime and duty-cycle maths. Run: python3 tools/test_hopnet.py"""

import sys
import unittest

import hopnet as hn
import wxframe as wf
from e5_node import DutyCycle, lora_airtime_s

KEY = bytes(range(16))
OTHER = bytes(range(1, 17))


class LinkFrames(unittest.TestCase):
    def setUp(self):
        self.payload = wf.encode_data(wf.Data(1, 0xBEEF, 7, temp_c_x100=1234), KEY)

    def test_fwd_roundtrip(self):
        buf = hn.encode_fwd(1, 2, 0xFFFF, 3, self.payload, KEY)
        self.assertEqual(len(buf), hn.HDR_LEN + len(self.payload) + wf.MAC_LEN)
        l = hn.decode(buf, KEY)
        self.assertEqual((l.type, l.src, l.dst, l.lseq, l.hops, l.payload), (hn.T_FWD, 1, 2, 0xFFFF, 3, self.payload))

    def test_hack_roundtrip(self):
        l = hn.decode(hn.encode_hack(2, 1, 42, KEY), KEY)
        self.assertEqual((l.type, l.src, l.dst, l.lseq, l.payload), (hn.T_HACK, 2, 1, 42, b""))

    def test_lseq_wraps(self):
        self.assertEqual(hn.decode(hn.encode_fwd(1, 2, 0x10001, 1, b"x", KEY), KEY).lseq, 1)

    def test_every_byte_flip_rejected(self):
        buf = hn.encode_fwd(1, 2, 9, 1, self.payload, KEY)
        for i in range(len(buf)):
            bad = bytearray(buf)
            bad[i] ^= 0x01
            self.assertIsNone(hn.decode(bytes(bad), KEY), f"flip at byte {i} accepted")

    def test_wrong_key_truncated_and_foreign(self):
        buf = hn.encode_fwd(1, 2, 9, 1, self.payload, KEY)
        self.assertIsNone(hn.decode(buf, OTHER))
        for n in range(len(buf)):
            self.assertIsNone(hn.decode(buf[:n], KEY))
        self.assertIsNone(hn.decode(self.payload, KEY))  # a bare end-to-end frame is not a link frame
        self.assertIsNone(hn.decode(b"", KEY))

    def test_link_mac_is_domain_separated(self):
        # A link MAC must not be reusable as an end-to-end MAC or the other way round.
        body = b"\x02\x00" + bytes(22)
        self.assertNotEqual(hn._mac(KEY, body), wf.mac4(KEY, body))


class Airtime(unittest.TestCase):
    def test_against_semtech_formula(self):
        # SF9/125 kHz, CR 4/5, preamble 12, explicit header, CRC on (hand-computed: 74.25 symbols x 4.096 ms)
        self.assertAlmostEqual(lora_airtime_s(44, 9), 74.25 * 0.004096, places=6)
        self.assertGreater(lora_airtime_s(44, 12), 1.0)  # SF12 enables low-data-rate optimisation
        self.assertLess(lora_airtime_s(11, 9), lora_airtime_s(44, 9))


class Duty(unittest.TestCase):
    def test_sub_bands(self):
        self.assertAlmostEqual(DutyCycle("868.1").budget, 3600 * 0.01 * 0.9)
        self.assertAlmostEqual(DutyCycle("869.525").budget, 3600 * 0.10 * 0.9)

    def test_budget_enforced(self):
        d = DutyCycle("868.1")
        air = lora_airtime_s(44, 9)
        n = 0
        while d.allows(air):
            d.record(air)
            n += 1
        self.assertLessEqual(n * air, d.budget)
        self.assertEqual(n, int(d.budget // air))


class SubBands(unittest.TestCase):
    def test_tenth_percent_band_and_gaps(self):
        from e5_node import duty_fraction
        self.assertEqual(duty_fraction("869.0"), 0.001)   # 868.7-869.2 MHz: 0.1 %
        self.assertEqual(duty_fraction("864.0"), 0.001)   # 863-865 MHz: 0.1 %
        self.assertEqual(duty_fraction("868.1"), 0.01)
        self.assertEqual(duty_fraction("869.525"), 0.10)
        for f in ("868.65", "869.3", "868.58", "870.5", "433.0"):  # gaps, channel edge overlap, out of band
            with self.assertRaises(ValueError, msg=f):
                duty_fraction(f)

    def test_budget_zero_means_zero(self):
        d = DutyCycle("868.1", 0)
        self.assertEqual(d.budget, 0)
        self.assertFalse(d.allows(0.1))
        with self.assertRaises(ValueError):
            DutyCycle("868.1", 1000)  # cannot raise above the legal budget


class FakeE5:
    def __init__(self):
        self.sent, self.inbox = [], []
    def tx(self, b): self.sent.append(b)
    def start_rx(self): pass
    def poll_packet(self, timeout): return self.inbox.pop(0) if self.inbox else None


class FakeArgs:
    role, id, next, sf, freq, state, seed = "relay", 2, 3, "SF9", "869.525", None, 1
    duty_budget = None
    fault_deaf = fault_drop_hack = fault_radio_error = 0.0


class NodeLogic(unittest.TestCase):
    def make(self, role="relay"):
        import e5_node
        args = FakeArgs()
        args.role = role
        args.next = None if role == "gateway" else 3
        n = e5_node.Node(FakeE5(), KEY, args)
        n.log = lambda msg: None
        return n, e5_node

    def test_full_queue_never_drops_inflight_or_wrong_item(self):
        n, m = self.make()
        old = m.MAX_QUEUE
        m.MAX_QUEUE = 3
        try:
            for i in range(3):
                n.enqueue(bytes([i]), 1)
            head = n.outq[0]
            # while head is in flight, a new packet arrives and the queue is full
            def fake_wait(lseq):
                n.enqueue(b"new", 1)
                return (-10, 9.0)
            n.wait_hack = fake_wait
            n.pump()
            remaining = [it[0] for it in n.outq]
            self.assertNotIn(head[0], remaining)        # the acknowledged packet is gone
            self.assertEqual(remaining, [bytes([2]), b"new"])  # oldest *waiting* one was dropped, not the next unsent
            self.assertEqual(n.stats["dropped"], 1)
        finally:
            m.MAX_QUEUE = old

    def test_gateway_rejects_replay_of_old_boot(self):
        n, _ = self.make("gateway")
        def link_for(boot, seq):
            payload = wf.encode_data(wf.Data(1, boot, seq), KEY)
            return hn.decode(hn.encode_fwd(2, 3, seq, 2, payload, KEY), KEY)
        for boot, seq in ((100, 1), (100, 2), (200, 1)):  # station rebooted: boot 100 -> 200
            n.deliver(link_for(boot, seq), -50, 5)
        n.deliver(link_for(100, 2), -50, 5)   # replay from the old boot
        n.deliver(link_for(200, 1), -50, 5)   # duplicate in current boot
        n.deliver(link_for(100, 3), -50, 5)   # late but genuinely new reading of the old boot
        self.assertEqual(n.stats["delivered"], 4)
        self.assertEqual(n.e2e_seen[1][200], 1)

    def test_no_ack_without_airtime(self):
        n, _ = self.make()
        n.duty.budget = 0
        payload = wf.encode_data(wf.Data(1, 5, 1), KEY)
        n.on_fwd(hn.decode(hn.encode_fwd(1, 2, 7, 1, payload, KEY), KEY), -40, 9)
        self.assertEqual(n.e5.sent, [])      # no acknowledgement transmitted
        self.assertEqual(len(n.outq), 0)     # and no custody taken: the sender keeps it
        self.assertEqual(n.stats["refused_duty"], 1)


class SerialParsing(unittest.TestCase):
    def test_odd_hex_line_is_dropped_not_raised(self):
        from e5_bench import E5
        e = E5.__new__(E5)
        e.meta, e.pending = None, []
        self.assertTrue(e._stash_if_packet('+TEST: LEN:2, RSSI:-50, SNR:7'))
        self.assertFalse(e._stash_if_packet('+TEST: RX "ABC"'))  # odd length: not a packet line
        self.assertTrue(e._stash_if_packet('+TEST: RX "ABCD"'))
        self.assertEqual(e.pending[0][0], bytes.fromhex("ABCD"))


class Telemetry(unittest.TestCase):
    def src(self):
        from telem_source import TelemSource
        return TelemSource("/dev/null", log=lambda m: None, start=False)

    def test_mapping_and_wind_window(self):
        s = self.src()
        for w in (0.0, 3.6, 7.2):
            s._handle('TELEM {"temperature":20.40,"humidity":55.00,"pressure":1001.50,"altitude":102.00,'
                      f'"wind_speed":{w},"wind_dir":-1.0,"rain_mm":1.40,"soil_pct":42.5,"soil_adc":300}}')
        s._handle('EVENT {"type":"lightning","distance_km":12,"energy":34567}')
        f, flags, restarts, src = s.take()
        self.assertEqual(flags, 0)
        self.assertEqual((f["temp_c_x100"], f["rh_x100"], f["press_hpa_x10"]), (2040, 5500, 10015))
        self.assertEqual((f["wind_ms_x100"], f["gust_ms_x100"]), (100, 200))   # mean 3.6 km/h, max 7.2 km/h
        self.assertNotIn("wind_dir_deg", f)                                    # -1 = vane not read
        self.assertEqual((f["rain_total_mm_x10"], f["soil_pct_x10"]), (14, 425))
        self.assertEqual((f["lightning_total"], f["lightning_km"]), (1, 12))
        f2, _, _, _ = s.take()
        self.assertNotIn("wind_ms_x100", f2)                                   # new window, no samples yet
        # the result must fit the radio frame
        wf.encode_data(wf.Data(1, 1, 1, **f), KEY)

    def test_stale_and_restart(self):
        s = self.src()
        self.assertEqual(s.take()[1], wf.FLAG_SENSOR_FAIL)                     # nothing received yet
        s._handle('TELEM {"temperature":20,"rain_mm":3.0}')
        s._handle('EVENT {"type":"lightning","distance_km":5}')
        s._handle("=== Hava Istasyonu (SIM800L) baslatiliyor ===")
        s._handle('TELEM {"temperature":20,"rain_mm":0.0}')                    # rain reset after reboot
        f, _, restarts, _ = s.take()
        self.assertEqual(restarts, 1)                                          # counted once, not twice
        self.assertEqual(f["lightning_total"], 0)                              # lightning count restarts with the boot
        s._handle('TELEM {"temperature":20,"rain_mm":0.0}')
        s._handle('TELEM garbage')
        self.assertEqual(s.bad_lines, 1)
        s.latest_at -= 60
        self.assertEqual(s.take()[1], wf.FLAG_SENSOR_FAIL)                     # older than 30 s


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False, verbosity=1).result.wasSuccessful() else 1)
