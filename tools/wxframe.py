"""Python mirror of lib/wxlink/wx_frame.{h,cpp}. Layout must stay identical (see test_parity.py)."""

import hashlib
import hmac
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path

VERSION = 2
TYPE_DATA, TYPE_ACK = 0, 1
MAC_LEN = 4
NO_I16, NO_U16, NO_U8 = 0x7FFF, 0xFFFF, 0xFF

FLAG_SENSOR_FAIL, FLAG_FAKE, FLAG_LOW_BATT, FLAG_COLD_BOOT = 1, 2, 4, 8

# ver, type, station_id, boot_id, seq, temp, rh, press, wind, gust, dir, rain, batt, soil, lightning_total,
# lightning_km, flags
_DATA = struct.Struct("<BBBHHhHHHHHHHHHBB")
# ver, type, station_id, boot_id, seq, status
_ACK = struct.Struct("<BBBHHB")
DATA_LEN = _DATA.size + MAC_LEN  # 33
ACK_LEN = _ACK.size + MAC_LEN    # 12

ROOT = Path(__file__).resolve().parent.parent


def load_key(path=None):
    """Reads WX_HMAC_KEY from include/secrets.h (or secrets.example.h), like the firmware does."""
    if path is None:
        path = ROOT / "include" / "secrets.h"
        if not path.exists():
            path = ROOT / "include" / "secrets.example.h"
    text = Path(path).read_text()
    m = re.search(r"#define\s+WX_HMAC_KEY\s*\{(.*?)\}", text, re.S)
    if not m:
        raise ValueError(f"WX_HMAC_KEY not found in {path}")
    return bytes(int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+", m.group(1)))


def mac4(key, msg):
    return hmac.new(key, msg, hashlib.sha256).digest()[:MAC_LEN]


@dataclass
class Data:
    station_id: int
    boot_id: int
    seq: int
    temp_c_x100: int = NO_I16
    rh_x100: int = NO_U16
    press_hpa_x10: int = NO_U16
    wind_ms_x100: int = NO_U16
    gust_ms_x100: int = NO_U16
    wind_dir_deg: int = NO_U16
    rain_total_mm_x10: int = NO_U16
    batt_mv: int = NO_U16
    soil_pct_x10: int = NO_U16
    lightning_total: int = NO_U16
    lightning_km: int = NO_U8
    flags: int = 0

    def to_json_dict(self):
        """Same field names and scaling as the gateway's JSON."""
        out = {"station": self.station_id, "boot": self.boot_id, "seq": self.seq}
        if self.temp_c_x100 != NO_I16:
            out["temp_c"] = self.temp_c_x100 / 100
        for key, val, scale in (("rh_pct", self.rh_x100, 100), ("press_hpa", self.press_hpa_x10, 10),
                                ("wind_ms", self.wind_ms_x100, 100), ("gust_ms", self.gust_ms_x100, 100),
                                ("wind_dir_deg", self.wind_dir_deg, 1),
                                ("rain_total_mm", self.rain_total_mm_x10, 10), ("batt_v", self.batt_mv, 1000),
                                ("soil_pct", self.soil_pct_x10, 10), ("lightning_total", self.lightning_total, 1)):
            if val != NO_U16:
                out[key] = val / scale
        if self.lightning_km != NO_U8:
            out["lightning_km"] = self.lightning_km
        out.update(sensor_fail=bool(self.flags & FLAG_SENSOR_FAIL), fake=bool(self.flags & FLAG_FAKE),
                   low_batt=bool(self.flags & FLAG_LOW_BATT), cold_boot=bool(self.flags & FLAG_COLD_BOOT))
        return out


@dataclass
class Ack:
    station_id: int
    boot_id: int
    seq: int
    status: int = 0


def encode_data(d: Data, key: bytes) -> bytes:
    body = _DATA.pack(VERSION, TYPE_DATA, d.station_id, d.boot_id, d.seq, d.temp_c_x100, d.rh_x100,
                      d.press_hpa_x10, d.wind_ms_x100, d.gust_ms_x100, d.wind_dir_deg, d.rain_total_mm_x10,
                      d.batt_mv, d.soil_pct_x10, d.lightning_total, d.lightning_km, d.flags)
    return body + mac4(key, body)


def encode_ack(a: Ack, key: bytes) -> bytes:
    body = _ACK.pack(VERSION, TYPE_ACK, a.station_id, a.boot_id, a.seq, a.status)
    return body + mac4(key, body)


class DecodeError(ValueError):
    pass


def decode(buf: bytes, key: bytes):
    """Returns Data or Ack; raises DecodeError(reason) with the firmware's reason names."""
    if len(buf) < 2:
        raise DecodeError("bad_length")
    if buf[0] != VERSION:
        raise DecodeError("bad_version")
    if buf[1] == TYPE_DATA:
        struct_, cls = _DATA, Data
    elif buf[1] == TYPE_ACK:
        struct_, cls = _ACK, Ack
    else:
        raise DecodeError("bad_type")
    if len(buf) != struct_.size + MAC_LEN:
        raise DecodeError("bad_length")
    body, tag = buf[:-MAC_LEN], buf[-MAC_LEN:]
    if not hmac.compare_digest(mac4(key, body), tag):
        raise DecodeError("bad_mac")
    fields = struct_.unpack(body)[2:]
    return cls(*fields)
