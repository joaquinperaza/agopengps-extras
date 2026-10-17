#!/usr/bin/env python3
"""Probe 2: deadzone vs int-truncation, prevError handling, precise timeouts, switchByte while engaged.
Fake cabin on .27:3344; sends only PGN 254 unicast to the front node."""
import socket, threading, time, json, urllib.request, sys

NODE = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.26"
CABIN_IP = sys.argv[2] if len(sys.argv) > 2 else "192.168.1.27"
log, p253 = [], []
ack_on = threading.Event(); ack_on.set()
stop = threading.Event()
last_ack = [0.0]

def cabin():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((CABIN_IP, 3344)); s.settimeout(0.2)
    while not stop.is_set():
        try: b, addr = s.recvfrom(64)
        except socket.timeout: continue
        if len(b) < 8 or b[:4] != b"\x80\x81\x7a\xc0" or b[7] != (sum(b[2:7]) & 0xFF): continue
        pwm = int.from_bytes(b[5:7], "little", signed=True)
        log.append((time.time(), pwm))
        if ack_on.is_set():
            d = min(abs(pwm), 255); a = [0x80, 0x81, 0x7B, 0xC1, b[4], d, 1 if d else 0]
            a.append(sum(a[2:]) & 0xFF); s.sendto(bytes(a), addr); last_ack[0] = time.time()

def sniff():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", 9999)); s.settimeout(0.2)
    while not stop.is_set():
        try: d, a = s.recvfrom(256)
        except socket.timeout: continue
        if a[0] == NODE and d[:4] == b"\x80\x81\x7e\xfd": p253.append((time.time(), d[11], d[12]))

tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
last_254 = [0.0]
def send254(speed, status, sp):
    v = int(round(speed * 10)) & 0xFFFF; a = int(round(sp * 100)) & 0xFFFF
    b = [0x80, 0x81, 0x7F, 0xFE, 8, v & 0xFF, v >> 8, status, a & 0xFF, a >> 8, 255, 0, 0]
    b.append(sum(b[2:]) & 0xFF); tx.sendto(bytes(b), (NODE, 8888)); last_254[0] = time.time()

def run(label, sp, hold=1.5, speed=5, status=1):
    i0, j0 = len(log), len(p253); t0 = time.time()
    while time.time() - t0 < hold: send254(speed, status, sp); time.sleep(0.1)
    sw = sorted({hex(x[1]) for x in p253[j0:]}); pd = [x[2] for x in p253[j0:]][-4:]
    print(f"{label:<44} sp={sp:<7.2f} cmd={[p for _, p in log[i0:]]} pgn253 sw={sw} pwmDisp={pd}", flush=True)

threading.Thread(target=cabin, daemon=True).start(); threading.Thread(target=sniff, daemon=True).start()
W = None
try:
    time.sleep(1.2)
    # read current WAS from PGN253 sniff is |pwm| only; get angle via one /snap (may be off after 10 min)
    try: W = float(json.loads(urllib.request.urlopen(f"http://{NODE}/snap", timeout=2).read())["ang"])
    except Exception as e: print("snap unavailable:", e); W = -4.61
    print("WAS =", W)
    # E = actual - setpoint  => setpoint = W - E
    print("== A: int-truncation threshold (steady state, Kp=0.6 -> 1/0.6 = 1.667) ==")
    for E in [3, 1.9, 1.8, 1.7, 1.6, 1.5]:
        run(f"E={E}", W - E, hold=1.8)
    print("== B: deadzone early-return keeps prevError? ==")
    run("disengage (status 0)", W, hold=1.0, status=0)
    run("E=-5 hold", W + 5, hold=1.5)
    run("E=+0.5 hold (inside dz)", W - 0.5, hold=1.5)
    run("E=+1.2 (outside dz) first tick tells", W - 1.2, hold=1.2)
    run("E=+0.9 hold (inside dz=1.0?)", W - 0.9, hold=1.5)
    run("E=-5 hold", W + 5, hold=1.5)
    run("E=+1.05 just outside dz", W - 1.05, hold=1.2)
    print("== C: AOG-silence timeout x3 ==")
    for k in range(3):
        run("engage E=-5", W + 5, hold=1.2)
        t_last = last_254[0]; i0 = len(log); time.sleep(1.2)
        z = next((t for t, p in log[i0:] if p == 0), None)
        nz = [round(t - t_last, 3) for t, p in log[i0:] if p != 0]
        print(f"  silence#{k}: last nonzero CMD at +{nz[-1] if nz else None}s, first zero at +{round(z - t_last, 3) if z else None}s")
    print("== D: heartbeat-loss timeout x3 ==")
    for k in range(3):
        run("engage E=-5", W + 5, hold=1.2)
        ack_on.clear(); t_last = last_ack[0]; i0 = len(log); t0 = time.time()
        while time.time() - t0 < 1.2: send254(5, 1, W + 5); time.sleep(0.1)
        z = next((t for t, p in log[i0:] if p == 0), None)
        nz = [round(t - t_last, 3) for t, p in log[i0:] if p != 0]
        print(f"  noack#{k}: last nonzero CMD at +{nz[-1] if nz else None}s, first zero at +{round(z - t_last, 3) if z else None}s")
        ack_on.set()
        run("  resume", W + 5, hold=1.0)
finally:
    for _ in range(10): send254(0, 0, 0); time.sleep(0.1)
    time.sleep(0.4); print("final idle cmd:", [p for _, p in log[-4:]], " pgn253 sw:", [hex(x[1]) for x in p253[-3:]])
    stop.set()
