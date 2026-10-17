#!/usr/bin/env python3
"""Passive burn-in logger: only RECEIVES PGN 253 broadcasts; never sends anything.
Per-minute summary per node: frames, max gap, gaps > 1 s, WAS / heading / roll ranges,
IMU-missing frames (9999/8888) and suspected reboots (gap > 4 s).
  usage: burnin.py HOURS NAME=IP [NAME=IP ...]"""
import socket, sys, time
hours = float(sys.argv[1]); nodes = dict(a.split("=")[::-1] for a in sys.argv[2:])   # ip -> name
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", 9999)); s.settimeout(0.5)

def fresh(): return dict(n=0, maxgap=0.0, gaps1=0, reboots=0, noimu=0, ang=[], hd=[], rl=[])
cur = {ip: fresh() for ip in nodes}; last = {ip: None for ip in nodes}; tot = {ip: fresh() for ip in nodes}
t_end = time.time() + hours * 3600; next_rep = time.time() + 60
print(f"burn-in start {time.strftime('%F %T')} for {hours} h: " + ", ".join(f"{v}={k}" for k, v in nodes.items()), flush=True)
while time.time() < t_end:
    try:
        d, a = s.recvfrom(256)
        ip = a[0]
        if ip in nodes and d[:4] == b"\x80\x81\x7e\xfd" and len(d) >= 14:
            t = time.time(); c = cur[ip]; c["n"] += 1
            if last[ip] is not None:
                g = t - last[ip]; c["maxgap"] = max(c["maxgap"], g)
                if g > 1.0: c["gaps1"] += 1
                if g > 4.0: c["reboots"] += 1
            last[ip] = t
            ang = int.from_bytes(d[5:7], "little", signed=True) / 100
            hd = int.from_bytes(d[7:9], "little", signed=True); rl = int.from_bytes(d[9:11], "little", signed=True)
            c["ang"].append(ang)
            if hd == 9999 or rl == 8888: c["noimu"] += 1
            else:
                c["hd"].append((hd - 3600 if hd > 1800 else hd) / 10); c["rl"].append(rl / 10)
    except socket.timeout:
        pass
    if time.time() >= next_rep:
        next_rep += 60
        for ip, name in nodes.items():
            c = cur[ip]; T = tot[ip]
            rng = lambda v: f"{min(v):.2f}..{max(v):.2f}" if v else "-"
            flag = "  <-- CHECK" if (c["n"] < 400 or c["gaps1"] or c["noimu"] or c["reboots"]) else ""
            print(f"{time.strftime('%H:%M')} {name:<6} frames {c['n']:4d} maxgap {c['maxgap']*1000:5.0f}ms gaps>1s {c['gaps1']} "
                  f"reboot? {c['reboots']} noIMU {c['noimu']} WAS {rng(c['ang'])} hdg {rng(c['hd'])} roll {rng(c['rl'])}{flag}", flush=True)
            for k in ("n", "gaps1", "reboots", "noimu"): T[k] += c[k]
            T["maxgap"] = max(T["maxgap"], c["maxgap"])
            cur[ip] = fresh()
print(f"burn-in end {time.strftime('%F %T')}", flush=True)
for ip, name in nodes.items():
    T, c = tot[ip], cur[ip]                  # include the last partial minute
    for k in ("n", "gaps1", "reboots", "noimu"): T[k] += c[k]
    T["maxgap"] = max(T["maxgap"], c["maxgap"])
    print(f"TOTAL {name}: frames {T['n']} maxgap {T['maxgap']*1000:.0f}ms gaps>1s {T['gaps1']} reboot? {T['reboots']} noIMU {T['noimu']}", flush=True)
