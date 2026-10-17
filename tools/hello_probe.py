#!/usr/bin/env python3
"""Does PGN 200 (AgIO hello) keep steering engaged after PGN 254 stops? Fake cabin on .27."""
import socket, threading, time, sys
NODE = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.26"
CABIN_IP = sys.argv[2] if len(sys.argv) > 2 else "192.168.1.27"
log = []; stop = threading.Event(); hello_replies = []

def cabin():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((CABIN_IP, 3344)); s.settimeout(0.2)
    while not stop.is_set():
        try: b, addr = s.recvfrom(64)
        except socket.timeout: continue
        if len(b) < 8 or b[:4] != b"\x80\x81\x7a\xc0": continue
        pwm = int.from_bytes(b[5:7], "little", signed=True); log.append((time.time(), pwm))
        d = min(abs(pwm), 255); a = [0x80, 0x81, 0x7B, 0xC1, b[4], d, 1 if d else 0]; a.append(sum(a[2:]) & 0xFF)
        s.sendto(bytes(a), addr)

def sniff():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", 9999)); s.settimeout(0.2)
    while not stop.is_set():
        try: d, a = s.recvfrom(256)
        except socket.timeout: continue
        if a[0] == NODE and d[:4] == b"\x80\x81\x7e\x7e": hello_replies.append(d.hex(" "))

tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
def pgn254(sp):
    a = int(round(sp * 100)) & 0xFFFF
    b = [0x80, 0x81, 0x7F, 0xFE, 8, 50, 0, 1, a & 0xFF, a >> 8, 255, 0, 0]; b.append(sum(b[2:]) & 0xFF); return bytes(b)
HELLO = bytes([0x80, 0x81, 0x7F, 0xC8, 3, 56, 0, 0, 0x47])  # AgIO hello (PGN 200)
idle = bytes([0x80, 0x81, 0x7F, 0xFE, 8, 0, 0, 0, 0, 0, 255, 0, 0, (0x7F + 0xFE + 8 + 255) & 0xFF])

threading.Thread(target=cabin, daemon=True).start(); threading.Thread(target=sniff, daemon=True).start()
try:
    time.sleep(1.0)
    t0 = time.time()
    while time.time() - t0 < 1.5: tx.sendto(pgn254(0.4), (NODE, 8888)); time.sleep(0.1)
    print("engaged pwm:", [p for _, p in log[-5:]])
    t_last = time.time(); i0 = len(log)
    while time.time() - t_last < 2.0: tx.sendto(HELLO, (NODE, 8888)); time.sleep(0.1)
    print("hello-only, pwm over time:", [(round(t - t_last, 2), p) for t, p in log[i0:]])
    print("hello replies seen:", len(hello_replies), hello_replies[:1])
finally:
    for _ in range(10): tx.sendto(idle, (NODE, 8888)); time.sleep(0.1)
    time.sleep(0.3); print("final idle:", [p for _, p in log[-3:]]); stop.set()
