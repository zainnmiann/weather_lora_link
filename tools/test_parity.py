"""Checks that tools/wxframe.py produces byte-identical frames to the C++ codec.

Usage: python3 tools/test_parity.py <path-to-native-test-binary>
"""

import subprocess
import sys

import wxframe as wf

KEY = bytes.fromhex("00112233445566778899aabbccddeeff")  # same as test/native/test_link.cpp


def main():
    out = subprocess.run([sys.argv[1]], capture_output=True, text=True, check=True).stdout
    vectors = dict(line.split(" ", 1) for line in out.splitlines() if line.startswith("VECTOR_"))

    data = wf.Data(1, 0xBEEF, 42, -1234, 8765, 10132, 350, 720, 270, 56, 3912, 423, 17, 12, wf.FLAG_COLD_BOOT)
    ack = wf.Ack(1, 0xBEEF, 42, 0)
    py_data = wf.encode_data(data, KEY).hex().upper()
    py_ack = wf.encode_ack(ack, KEY).hex().upper()

    ok = True
    for name, c_hex, py_hex in (("data", vectors["VECTOR_DATA"], py_data), ("ack", vectors["VECTOR_ACK"], py_ack)):
        match = c_hex == py_hex
        ok &= match
        print(f"{name}: {'match' if match else 'MISMATCH'}\n  c++ {c_hex}\n  py  {py_hex}")

    # Decoding the C++ bytes in Python must give back the same values, and tampering must fail.
    ok &= wf.decode(bytes.fromhex(vectors["VECTOR_DATA"]), KEY) == data
    ok &= wf.decode(bytes.fromhex(vectors["VECTOR_ACK"]), KEY) == ack
    bad = bytearray(bytes.fromhex(vectors["VECTOR_DATA"]))
    bad[8] ^= 1
    try:
        wf.decode(bytes(bad), KEY)
        ok = False
        print("tampered frame was accepted")
    except wf.DecodeError as e:
        ok &= str(e) == "bad_mac"

    print("parity OK" if ok else "parity FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
