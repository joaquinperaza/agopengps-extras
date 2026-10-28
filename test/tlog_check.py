#!/usr/bin/env python3
"""Check the flash image written by test/tlog_ring_test.cpp with the real decoder.
  python3 -I test/tlog_check.py /tmp/tlog.bin"""
import importlib.util, os, struct, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("tlog_decode", os.path.join(here, "..", "tools", "tlog_decode.py"))
dec = importlib.util.module_from_spec(spec); spec.loader.exec_module(dec)

path = sys.argv[1]
total, newest = map(int, open(path + ".expect").read().split())
recs, problems, _ = dec.parse(path)
fails = 0
def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    fails += 0 if ok else 1

check(not problems, f"no decode problems {problems[:2]}")
data = [r for r in recs if r.chan < 0x80]
ctr = [struct.unpack_from("<I", r.payload, 1)[0] for r in data]
check(ctr == list(range(ctr[0], ctr[0] + len(ctr))), f"kept records are one contiguous run ({ctr[0]}..{ctr[-1]})")
check(ctr[-1] == total - 1, "newest record is the last one written")
ok = True
for r, c in zip(data, ctr):
    want = 5 + (c * 37) % 260
    if r.trunc != (want > 254) or len(r.payload) != min(want, 254) or r.src != 26 or (r.chan & 0x3F) != c % 6:
        ok = False; break
    if any(r.payload[i] != (c + i) & 0xFF for i in range(5, len(r.payload))):
        ok = False; break
check(ok, "payload length, truncation flag, channel, source and bytes all intact")
b3 = [(r.t, c) for r, c in zip(data, ctr) if r.boot == 3]
check(len(b3) == 40 and all(t == 250 + 50 * i for i, (t, _) in enumerate(b3)), "boot 3 timestamps exact (250 + 50 k ms)")
check(max(r.seq for r in recs) == newest, "newest sector seq matches")
seqs = sorted({r.seq for r in recs})
check(seqs == list(range(seqs[0], seqs[-1] + 1)) and len(seqs) <= 12, f"retained sectors are the newest {len(seqs)} (seq {seqs[0]}..{seqs[-1]})")
print("\nALL PASS" if not fails else f"\nFAILED ({fails})")
sys.exit(1 if fails else 0)
