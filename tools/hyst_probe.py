#!/usr/bin/env python3
"""Probe 4: test the restart-hysteresis model. Fake cabin on .27; only PGN 254 sent to the node.
Model: 'resting' latches when |Kp*E| < 1 (P-term truncates to 0), clears only when |E| > 2*dz.
Dead-zone early return (|E| < dz) does not latch. Disengage clears it."""
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
        pwm = int.from_bytes(b[5:7], "little", signed=True); log.append(pwm)
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

def run(label, E, hold=1.3, status=1, expect=""):
    i0, j0 = len(log), len(angs); t0 = time.time()
    while time.time() - t0 < hold: send(status, W - E); time.sleep(0.1)
    a = angs[j0:]; amin, amax = (min(a), max(a)) if a else (None, None)
    print(f"{label:<40} E={E:<6} cmd={log[i0:]}   expect {expect}   WAS[{amin},{amax}]", flush=True)

threading.Thread(target=cabin, daemon=True).start(); threading.Thread(target=sniff, daemon=True).start()
try:
    time.sleep(1.2); W = round(sum(angs[-5:]) / len(angs[-5:]), 2); print("WAS =", W)
    run("reset", 0, status=0, hold=0.8)
    print("-- P1/P6: latch at 1.5 then climb --")
    run("E=-5", -5, expect="active ~-43")
    run("E=+1.5 (kick then latch)", 1.5, expect="[~46, 0...]")
    run("E=+1.9", 1.9, expect="ALL 0 (latched; no-latch model says 41)")
    run("E=+1.95", 1.95, expect="all 0")
    run("E=+2.1", 2.1, expect="exit -> 41")
    run("E=+1.9 (coming down)", 1.9, expect="41 (not latched)")
    print("-- P3: dz then 1.9 (dz must NOT latch) --")
    run("E=-5", -5, expect="~-43")
    run("E=+0.5 dz", 0.5, expect="0")
    run("E=+1.9", 1.9, expect="[~47, 41...]")
    print("-- P5: does disengage clear the latch? --")
    run("E=+1.5 latch", 1.5, expect="0")
    run("status 0", 1.5, status=0, hold=0.8, expect="0")
    run("re-engage E=+1.9", 1.9, expect="[~42, 41...] if cleared, 0 if not")
    print("-- negative side mirror --")
    run("E=-1.5 (kick then latch)", -1.5, expect="[~-6x, 0..]")
    run("E=-1.9", -1.9, expect="all 0")
    run("E=-2.1", -2.1, expect="-41")
finally:
    for _ in range(10): send(0, 0); time.sleep(0.1)
    time.sleep(0.3); print("final idle:", log[-3:]); stop.set()
