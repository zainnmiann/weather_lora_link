"""Hop-by-hop link layer for the station -> relay(s) -> gateway chain.

Every radio packet in the chain is a LINK frame wrapped around an end-to-end payload (the
33-byte signed data frame from wxframe.py, which only the gateway unwraps):

  FWD  (carries a payload): magic 'L' | type 0x10 | src | dst | link_seq (u16) | hops | payload | mac4
  HACK (hop acknowledgement): magic 'L' | type 0x11 | src | dst | link_seq (u16) | 0    | mac4

- src/dst are node ids. Routing is static: each node knows its next hop.
- link_seq is per sending node; the receiver re-acknowledges a repeated (src, link_seq) but
  forwards it only once.
- mac4 = first 4 bytes of HMAC-SHA256(key, b"LINK" + header + payload). It authenticates each
  hop; the payload keeps its own end-to-end signature from the station.
"""

import hmac
import struct
from dataclasses import dataclass

import wxframe as wf

MAGIC = 0x4C  # 'L', distinct from end-to-end frames (which start with their version byte, 2)
T_FWD, T_HACK = 0x10, 0x11
_HDR = struct.Struct("<BBBBHB")  # magic, type, src, dst, link_seq, hops
HDR_LEN = _HDR.size  # 7
MAX_HOPS = 8


@dataclass
class Link:
    type: int
    src: int
    dst: int
    lseq: int
    hops: int
    payload: bytes = b""


def _mac(key, body):
    return wf.mac4(key, b"LINK" + body)


def encode_fwd(src, dst, lseq, hops, payload, key):
    body = _HDR.pack(MAGIC, T_FWD, src, dst, lseq & 0xFFFF, hops) + payload
    return body + _mac(key, body)


def encode_hack(src, dst, lseq, key):
    body = _HDR.pack(MAGIC, T_HACK, src, dst, lseq & 0xFFFF, 0)
    return body + _mac(key, body)


def decode(buf, key):
    """Returns a Link, or None if the bytes are not a valid link frame for this key."""
    if len(buf) < HDR_LEN + wf.MAC_LEN or buf[0] != MAGIC:
        return None
    body, tag = buf[:-wf.MAC_LEN], buf[-wf.MAC_LEN:]
    if not hmac.compare_digest(_mac(key, body), tag):
        return None
    _, typ, src, dst, lseq, hops = _HDR.unpack(body[:HDR_LEN])
    if typ not in (T_FWD, T_HACK):
        return None
    return Link(typ, src, dst, lseq, hops, body[HDR_LEN:])
