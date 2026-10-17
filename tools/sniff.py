#!/usr/bin/env python3
"""Passive UDP sniffer: binds broadcast ports and only RECEIVES. Never sends."""
import socket, select, sys, time

ports = [int(p) for p in sys.argv[1].split(",")] if len(sys.argv) > 1 else [9999, 8888]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 15
socks = []
for p in ports:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(("", p))
    socks.append(s)

t0 = time.time()
while time.time() - t0 < secs:
    r, _, _ = select.select(socks, [], [], 0.5)
    for s in r:
        data, addr = s.recvfrom(2048)
        port = s.getsockname()[1]
        line = f"{time.time()-t0:7.3f} :{port} {addr[0]}:{addr[1]} len={len(data)} " + data.hex(" ")
        if len(data) >= 13 and data[:4] == b"\x80\x81\x7e\xfd":
            ang = int.from_bytes(data[5:7], "little", signed=True) / 100
            hd = int.from_bytes(data[7:9], "little", signed=True)
            rl = int.from_bytes(data[9:11], "little", signed=True)
            crc_ok = (sum(data[2:13]) & 0xFF) == data[13] if len(data) > 13 else None
            line += f"  | PGN253 ang={ang:.2f} head10={hd} roll10={rl} sw=0x{data[11]:02x} pwm={data[12]} crc_ok={crc_ok}"
        print(line, flush=True)
