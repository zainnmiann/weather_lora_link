"""Compares the relay's C++ link frames with tools/hopnet.py byte for byte."""
import hashlib, subprocess, sys
sys.path.insert(0, sys.argv[2])
import hopnet as hn
out = subprocess.run([sys.argv[1]], capture_output=True, text=True).stdout
v = dict(l.split(" ", 1) for l in out.splitlines() if l.startswith("VEC_"))
key = bytes(range(16)); payload = bytes((i * 7 + 1) & 0xFF for i in range(33))
checks = {"VEC_FWD": hn.encode_fwd(1, 2, 0xBEEF, 1, payload, key).hex().upper(),
          "VEC_HACK": hn.encode_hack(2, 1, 0xBEEF, key).hex().upper(),
          "VEC_DIGEST": hashlib.sha256(payload).digest()[:8].hex().upper()}
ok = True
for k, py in checks.items():
    same = v.get(k) == py; ok &= same
    print(f"{k}: {'match' if same else 'MISMATCH'}\n  c++ {v.get(k)}\n  py  {py}")
print("parity OK" if ok else "parity FAILED"); sys.exit(0 if ok else 1)
