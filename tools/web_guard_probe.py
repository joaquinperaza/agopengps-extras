#!/usr/bin/env python3
"""Bench check: /ping, /scan, /bnorst must be refused (409) while steering and work when idle.
Fake cabin on CABIN_IP:3344; sends only PGN 254 to NODE. Never point this at the tractor module."""
import socket, threading, time, sys, urllib.request, urllib.error
NODE = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.36"
CABIN_IP = sys.argv[2] if len(sys.argv) > 2 else "192.168.1.113"
stop = threading.Event(); engage = threading.Event(); log = []

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

def aog():
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    while not stop.is_set():
        st, sp = (1, 0.4) if engage.is_set() else (0, 0.0)
        a = int(round(sp * 100)) & 0xFFFF
        b = [0x80, 0x81, 0x7F, 0xFE, 8, 50, 0, st, a & 0xFF, a >> 8, 255, 0, 0]; b.append(sum(b[2:]) & 0xFF)
        tx.sendto(bytes(b), (NODE, 8888)); time.sleep(0.1)

def get(path):
    try:
        r = urllib.request.urlopen(f"http://{NODE}{path}", timeout=8); return r.status, r.read().decode()
    except urllib.error.HTTPError as e: return e.code, e.read().decode()

threading.Thread(target=cabin, daemon=True).start(); threading.Thread(target=aog, daemon=True).start()
try:
    time.sleep(1.0)
    engage.set(); time.sleep(1.0)
    print("engaged, last pwm:", log[-3:], " /snap:", get("/snap")[1])
    for p in ["/ping", "/scan", "/bnorst"]:
        code, body = get(p); print(f"  while steering {p:<8} -> {code} {body}")
    print("pwm still steering after refusals:", log[-3:])
    engage.clear(); time.sleep(1.0)
    print("idle, last pwm:", log[-3:])
    for p in ["/ping", "/scan"]:
        code, body = get(p); print(f"  idle {p:<8} -> {code} {body}")
finally:
    engage.clear(); time.sleep(0.5); stop.set()
