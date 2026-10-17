#!/usr/bin/env python3
"""Side-by-side latency/stability comparison of two front nodes on the same WiFi, same minutes.
Each node gets its own fake cabin (its configured cabin IP must be on this PC). Sends only PGN 200
(hello) and PGN 254 (unicast). REAL CABIN MUST BE OFF.
  usage: compare_nodes.py NAME=NODE_IP:CABIN_IP [NAME=NODE_IP:CABIN_IP ...] [--steer-secs N]"""
import socket, threading, time, sys, subprocess, statistics as st, re

args = [a for i, a in enumerate(sys.argv[1:], 1) if "=" in a and not a.startswith("--")]
STEER_SECS = int(sys.argv[sys.argv.index("--steer-secs") + 1]) if "--steer-secs" in sys.argv else 180
IDLE_SECS = int(sys.argv[sys.argv.index("--idle-secs") + 1]) if "--idle-secs" in sys.argv else 60
nodes = {}
for a in args:
    name, rest = a.split("="); ip, cab = rest.split(":")
    nodes[name] = dict(ip=ip, cabin=cab, cmd=[], p253=[], hello=[], imu=[])
by_ip = {n["ip"]: k for k, n in nodes.items()}
stop = threading.Event(); phase = ["idle"]

def cabin(name):
    n = nodes[name]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((n["cabin"], 3344)); s.settimeout(0.2)
    while not stop.is_set():
        try: b, addr = s.recvfrom(64)
        except socket.timeout: continue
        t = time.time()
        if len(b) < 8 or b[:4] != b"\x80\x81\x7a\xc0" or addr[0] != n["ip"]: continue
        pwm = int.from_bytes(b[5:7], "little", signed=True)
        d = min(abs(pwm), 255); a = [0x80, 0x81, 0x7B, 0xC1, b[4], d, 1 if d else 0]; a.append(sum(a[2:]) & 0xFF)
        s.sendto(bytes(a), addr)
        n["cmd"].append((t, b[4], pwm, phase[0]))

def sniff():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", 9999)); s.settimeout(0.2)
    while not stop.is_set():
        try: d, a = s.recvfrom(256)
        except socket.timeout: continue
        t = time.time(); k = by_ip.get(a[0])
        if not k: continue
        if d[:4] == b"\x80\x81\x7e\xfd" and len(d) >= 14:
            nodes[k]["p253"].append((t, phase[0]))
            if phase[0] == "idle":
                nodes[k]["imu"].append((int.from_bytes(d[7:9], "little", signed=True), int.from_bytes(d[9:11], "little", signed=True)))
        elif d[:4] == b"\x80\x81\x7e\x7e":
            nodes[k]["hello"].append(t)

tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
def pgn254(status, sp):
    a = int(round(sp * 100)) & 0xFFFF
    b = [0x80, 0x81, 0x7F, 0xFE, 8, 50, 0, status, a & 0xFF, a >> 8, 255, 0, 0]; b.append(sum(b[2:]) & 0xFF); return bytes(b)
HELLO = bytes([0x80, 0x81, 0x7F, 0xC8, 3, 56, 0, 0, 0x47])

def pct(v, p): v = sorted(v); return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))] if v else float("nan")
def stats(v, unit="ms"):
    if not v: return "n/a"
    return f"mean {st.mean(v):6.1f}  p50 {pct(v,50):6.1f}  p95 {pct(v,95):6.1f}  max {max(v):6.1f} {unit}  (n={len(v)})"

for k in nodes: threading.Thread(target=cabin, args=(k,), daemon=True).start()
threading.Thread(target=sniff, daemon=True).start()
results = {k: {} for k in nodes}
try:
    # ---- phase 1: idle broadcast stability + ICMP ping (60 s) ----
    print(f"phase 1: idle {IDLE_SECS} s (PGN 253 timing, IMU noise, ICMP ping)", flush=True)
    pings = {k: subprocess.Popen(["ping", "-c", str(IDLE_SECS * 5 - 5), "-i", "0.2", "-W", "1", n["ip"]], stdout=subprocess.PIPE, text=True)
             for k, n in nodes.items()}
    t_end = time.time() + IDLE_SECS
    while time.time() < t_end:
        for n in nodes.values(): tx.sendto(pgn254(0, 0), (n["ip"], 8888))
        time.sleep(0.1)
    for k, p in pings.items():
        out = p.communicate()[0]
        rtts = [float(x) for x in re.findall(r"time=([\d.]+)", out)]
        loss = re.search(r"([\d.]+)% packet loss", out)
        results[k]["icmp"] = (rtts, loss.group(1) if loss else "?")

    # ---- phase 2: hello -> reply latency (firmware reaction, no tick quantisation) ----
    print("phase 2: 100 hellos per node", flush=True)
    phase[0] = "hello"
    for k, n in nodes.items():
        lat, lost = [], 0
        for i in range(100):
            i0 = len(n["hello"]); t0 = time.time(); tx.sendto(HELLO, (n["ip"], 8888))
            while time.time() - t0 < 0.5 and len(n["hello"]) == i0: time.sleep(0.0005)
            if len(n["hello"]) > i0: lat.append((n["hello"][i0] - t0) * 1000)
            else: lost += 1
            time.sleep(0.05)
        results[k]["hello"] = (lat, lost)

    # ---- phase 3: steering, setpoint flips every 1 s, both nodes simultaneously ----
    print(f"phase 3: steering {STEER_SECS} s, setpoint flips E=+5/-5 every 1 s", flush=True)
    phase[0] = "steer"
    W = -4.61
    steps = {k: [] for k in nodes}       # (t_first_send, expected_sign)
    t_start = time.time(); sign = 1; next_flip = t_start
    while time.time() - t_start < STEER_SECS:
        now = time.time()
        if now >= next_flip:
            sign = -sign; next_flip = now + 1.0
            for k in nodes: steps[k].append((now, sign))    # sp = W - 5*sign -> E = +5*sign -> pwm sign = sign
        sp = W - 5 * sign
        for n in nodes.values(): tx.sendto(pgn254(1, sp), (n["ip"], 8888))
        time.sleep(0.1)
    phase[0] = "end"
    for k, n in nodes.items():
        cmds = [c for c in n["cmd"] if c[3] == "steer"]
        lat, dropouts, settled = [], 0, 0
        for i, (t0, want) in enumerate(steps[k]):
            t1 = steps[k][i + 1][0] if i + 1 < len(steps[k]) else t0 + 1.0
            seg = [c for c in cmds if t0 <= c[0] < t1]
            hit = next((c for c in seg if c[2] != 0 and (c[2] > 0) == (want > 0)), None)
            if hit:
                lat.append((hit[0] - t0) * 1000)
                after = [c for c in seg if c[0] > hit[0]]
                dropouts += sum(1 for c in after if c[2] == 0); settled += len(after)
        seqs = [c[1] for c in cmds]
        lost = sum(((b - a) % 256) - 1 for a, b in zip(seqs, seqs[1:]) if (b - a) % 256 != 1)
        dts = [(b[0] - a[0]) * 1000 for a, b in zip(cmds, cmds[1:])]
        results[k]["steer"] = (lat, dropouts, settled, lost, len(cmds), dts)
finally:
    for _ in range(10):
        for n in nodes.values(): tx.sendto(pgn254(0, 0), (n["ip"], 8888))
        time.sleep(0.1)
    time.sleep(0.3); stop.set()

print()
for k, n in nodes.items():
    r = results[k]; print(f"===== {k} ({n['ip']}) =====")
    if "icmp" in r: print(f"  ICMP ping RTT            {stats(r['icmp'][0])}  loss {r['icmp'][1]}%")
    if "hello" in r: print(f"  hello->reply latency     {stats(r['hello'][0])}  lost {r['hello'][1]}")
    for ph in ("idle", "steer"):
        ts = [t for t, p in n["p253"] if p == ph]
        dts = [(b - a) * 1000 for a, b in zip(ts, ts[1:])]
        gaps = sum(1 for d in dts if d > 300)
        print(f"  PGN253 interval ({ph:<5})   {stats(dts)}  gaps>300ms {gaps}")
    if n["imu"]:
        hd = [h for h, _ in n["imu"]]; rl = [r_ for _, r_ in n["imu"]]
        hd = [h - 3600 if h > 1800 else h for h in hd]
        print(f"  IMU idle (x0.1 deg)      heading range {min(hd)}..{max(hd)}  roll range {min(rl)}..{max(rl)}  roll std {st.pstdev(rl):.2f}")
    if "steer" in r:
        lat, drop, settled, lost, ncmd, dts = r["steer"]
        print(f"  setpoint->motor latency  {stats(lat)}")
        print(f"  CMD interval (steering)  {stats(dts)}")
        print(f"  CMD frames {ncmd}, lost {lost}, unexpected pwm=0 while steering {drop}/{settled}")
