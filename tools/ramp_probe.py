#!/usr/bin/env python3
"""Does the module apply the lowPWM->highPWM ramp / highPWM cap? Fake cabin; sends only PGN 254.
Each kick step is chosen so 'no ramp', '3 deg ramp' (official AOG) and '5 deg ramp' (UI text) give
different first-tick values. Expected values assume Kp .6 Kd .1 min 40 low 50 high 140 dz 1.0, WAS -4.61."""
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

def run(label, sp, hold=1.3, status=1, expect=""):
    i0, j0 = len(log), len(angs); t0 = time.time()
    while time.time() - t0 < hold: send(status, sp); time.sleep(0.1)
    a = angs[j0:]
    print(f"{label:<34} sp={sp:<7.2f} cmd={log[i0:]}  {expect}  WAS[{min(a) if a else None},{max(a) if a else None}]", flush=True)

threading.Thread(target=cabin, daemon=True).start(); threading.Thread(target=sniff, daemon=True).start()
try:
    time.sleep(1.2)
    W = round(sum(angs[-5:]) / len(angs[-5:]), 2) if angs else -4.61
    print("WAS =", W, flush=True)
    run("reset", 0, status=0, hold=0.8)
    print("-- kick at E=+1.2 after prev E=-74.6 --")
    run("engage sp=70 (E=-74.6)", 70, expect="steady -84 (gold, no clamp)")
    run("dead zone E=+0.5", W - 0.5, expect="0")
    run("E=+1.2 KICK", W - 1.2, expect="first tick: none=109  3deg=86  5deg=71")
    print("-- kick at E=+4.0 after prev E=-74.6 --")
    run("engage sp=70", 70)
    run("dead zone E=+0.5", W - 0.5)
    run("E=+4.0 KICK", W - 4.0, expect="first tick: none=113  3deg=113  5deg=122->113?")
    print("-- negative mirror: kick at E=-1.2 after prev E=+65.4 --")
    run("engage sp=-70 (E=+65.4)", -70, expect="steady 79")
    run("dead zone E=-0.5", W + 0.5)
    run("E=-1.2 KICK", W + 1.2, expect="first tick: none=-101  3deg=-86  5deg=-71")
    print("-- highPWM cap: jump E=+65.4 -> E=-74.6 --")
    run("engage sp=-70", -70)
    run("sp=70 JUMP", 70, expect="first tick: no cap=-212  cap=-140")
finally:
    for _ in range(10): send(0, 0); time.sleep(0.1)
    time.sleep(0.3); print("final idle:", log[-3:], flush=True); stop.set()
