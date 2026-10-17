#!/usr/bin/env python3
"""Replay run-2 setpoints around the 1.99 deg outlier, logging every CMD (seq, pwm, arrival) and PGN253 angle."""
import socket, threading, time, sys
NODE = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.26"
CABIN_IP = sys.argv[2] if len(sys.argv) > 2 else "192.168.1.27"
log = []; stop = threading.Event(); angs = []

def cabin():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((CABIN_IP, 3344)); s.settimeout(0.2)
    while not stop.is_set():
        try: b, addr = s.recvfrom(64)
        except socket.timeout: continue
        if len(b) < 8 or b[:4] != b"\x80\x81\x7a\xc0": continue
        pwm = int.from_bytes(b[5:7], "little", signed=True); log.append((time.time(), b[4], pwm))
        d = min(abs(pwm), 255); a = [0x80, 0x81, 0x7B, 0xC1, b[4], d, 1 if d else 0]; a.append(sum(a[2:]) & 0xFF)
        s.sendto(bytes(a), addr)

def sniff():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", 9999)); s.settimeout(0.2)
    while not stop.is_set():
        try: d, a = s.recvfrom(256)
        except socket.timeout: continue
        if a[0] == NODE and d[:4] == b"\x80\x81\x7e\xfd": angs.append(int.from_bytes(d[5:7], "little", signed=True) / 100)

tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
def send(status, sp):
    a = int(round(sp * 100)) & 0xFFFF
    b = [0x80, 0x81, 0x7F, 0xFE, 8, 50, 0, status, a & 0xFF, a >> 8, 255, 0, 0]; b.append(sum(b[2:]) & 0xFF)
    tx.sendto(bytes(b), (NODE, 8888))

def run(label, sp, hold=1.5, status=1):
    i0, j0 = len(log), len(angs); t0 = time.time()
    while time.time() - t0 < hold: send(status, sp); time.sleep(0.1)
    seg = log[i0:]; seqs = [s for _, s, _ in seg]
    gaps = [b - a for a, b in zip(seqs, seqs[1:]) if (b - a) % 256 != 1]
    a = angs[j0:]
    print(f"{label:<24} sp={sp:<6} cmd={[p for *_, p in seg]} seqgaps={gaps} WAS[{min(a) if a else None},{max(a) if a else None}]", flush=True)

threading.Thread(target=cabin, daemon=True).start(); threading.Thread(target=sniff, daemon=True).start()
try:
    time.sleep(1.0)
    for rep in range(2):
        print(f"== replay {rep} ==")
        run("reset", 0, status=0, hold=0.8)
        run("E~-30 (sp 25.4)", 25.4)
        run("E~+0.49 (sp -5.1)", -5.1)
        run("E~+1.49 (sp -6.1)", -6.1)
        run("E~+1.99 (sp -6.6)", -6.6)
        run("E~+2.99 (sp -7.6)", -7.6)
finally:
    for _ in range(10): send(0, 0); time.sleep(0.1)
    time.sleep(0.3); print("final idle:", [p for *_, p in log[-3:]]); stop.set()
