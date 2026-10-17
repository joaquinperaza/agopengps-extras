#!/usr/bin/env python3
"""Fake cabin node (listens :3344, ACKs like aog_dumb_cabin_node) + PGN 254 probe.
Requires this PC to own 192.168.1.27 and the REAL cabin node to be OFF.
Sends only PGN 254 (unicast) to the front node; logs every CMD frame (signed pwm)."""
import socket, threading, time, sys

NODE = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.26"
CABIN_IP = sys.argv[2] if len(sys.argv) > 2 else "192.168.1.27"
log = []                      # (t, seq, pwm)
ack_enabled = threading.Event(); ack_enabled.set()
stop = threading.Event()

def cabin():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((CABIN_IP, 3344))
    s.settimeout(0.2)
    while not stop.is_set():
        try:
            b, addr = s.recvfrom(64)
        except socket.timeout:
            continue
        if len(b) < 8 or b[0] != 0x80 or b[1] != 0x81 or b[2] != 0x7A or b[3] != 0xC0:
            continue
        if b[7] != (sum(b[2:7]) & 0xFF):
            print("  !! bad CMD crc", b.hex(" "), flush=True); continue
        pwm = int.from_bytes(b[5:7], "little", signed=True)
        log.append((time.time(), b[4], pwm))
        if ack_enabled.is_set():
            duty = min(abs(pwm), 255)
            a = [0x80, 0x81, 0x7B, 0xC1, b[4], duty, 1 if duty else 0]
            a.append(sum(a[2:]) & 0xFF)
            s.sendto(bytes(a), addr)

tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
def pgn254(speed, status, sp):
    v = int(round(speed * 10)) & 0xFFFF; a = int(round(sp * 100)) & 0xFFFF
    b = [0x80, 0x81, 0x7F, 0xFE, 8, v & 0xFF, v >> 8, status, a & 0xFF, a >> 8, 255, 0, 0]
    b.append(sum(b[2:]) & 0xFF); return bytes(b)

def run(label, speed, status, sp, hold=1.5, show_all=False):
    i0 = len(log); t0 = time.time()
    while time.time() - t0 < hold:
        tx.sendto(pgn254(speed, status, sp), (NODE, 8888)); time.sleep(0.1)
    seg = log[i0:]
    pw = [p for _, _, p in seg]
    print(f"{label:<36} spd={speed:<4} st={status} sp={sp:<7.2f} cmd_pwm={pw if show_all else pw[-6:]}", flush=True)

threading.Thread(target=cabin, daemon=True).start()
try:
    time.sleep(1.5)
    if not log: print("NO CMD frames received on .27:3344 — is the IP set?"); sys.exit(1)
    dts = [b[0] - a[0] for a, b in zip(log, log[1:])]
    print(f"CMD rate: {len(log)} frames, avg dt {1000*sum(dts)/max(len(dts),1):.1f} ms, idle pwm={[p for *_, p in log]}", flush=True)
    W = -4.6   # current WAS angle; error = setpoint - WAS (sign as AOG sees it)
    run("idle status 0", 5, 0, W + 5)
    for e in [0, 0.5, 0.9, 1.1, 1.5, 2, 3, 4, 5, 6, 8, 15, 30]:
        run(f"engaged err={e:+}", 5, 1, W + e)
    for e in [-0.5, -1.5, -3, -5, -15]:
        run(f"engaged err={e:+}", 5, 1, W + e)
    print("-- step response (Kd transient), every CMD shown --", flush=True)
    run("hold err 0", 5, 1, W, hold=1.5)
    run("step to err +3", 5, 1, W + 3, hold=1.5, show_all=True)
    run("step to err +8", 5, 1, W + 8, hold=1.5, show_all=True)
    run("step back to err +3", 5, 1, W + 3, hold=1.5, show_all=True)
    print("-- gating --", flush=True)
    for spd in [0, 0.1, 0.5, 1.0, 2.0]:
        run(f"speed {spd} err +5", spd, 1, W + 5)
    run("status 0 err +5", 5, 0, W + 5)
    run("setpoint 70 (> maxSteer 50)", 5, 1, 70)
    run("setpoint -70", 5, 1, -70)
    print("-- AOG goes silent while engaged (err +5) --", flush=True)
    run("engage err +5", 5, 1, W + 5)
    i0 = len(log); t0 = time.time(); time.sleep(3)
    for t, _, p in log[i0:]: print(f"  no PGN254 t+{t-t0:5.2f}s pwm={p}", flush=True)
    print("-- cabin stops ACKing while engaged (err +5) --", flush=True)
    run("engage err +5", 5, 1, W + 5)
    ack_enabled.clear(); i0 = len(log); t0 = time.time()
    while time.time() - t0 < 2.5:
        tx.sendto(pgn254(5, 1, W + 5), (NODE, 8888)); time.sleep(0.1)
    for t, _, p in log[i0:]: print(f"  no ACK t+{t-t0:5.2f}s pwm={p}", flush=True)
    ack_enabled.set()
    run("ACKs resume, err +5", 5, 1, W + 5, hold=2.0, show_all=True)
finally:
    for _ in range(10):
        tx.sendto(pgn254(0, 0, 0), (NODE, 8888)); time.sleep(0.1)
    time.sleep(0.5)
    print("final idle pwm:", [p for *_, p in log[-4:]], flush=True)
    stop.set()
