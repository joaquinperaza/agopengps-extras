#!/usr/bin/env python3
"""Mock AOG logger for developing the WebUI without hardware.

Serves aog_logger/webpage.h and the logger API on http://127.0.0.1:8765 with simulated tractor
traffic encoded in the real log format (aog_logger/tlog.h). It starts with HISTORY_MIN minutes
of history and keeps generating traffic in real time, so the page's live mode works.
  python3 -I tools/logger_mock.py [port]
Also writes the simulated log: python3 -I tools/logger_mock.py --dump out.bin
"""
import json, math, os, random, re, struct, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
SECTOR, NSEC, MAGIC, HISTORY_MIN = 4096, 624, 0x31474C54, 20
PORTS = [8888, 9999, 2233, 4242, 3344, 4433]
SUBNET, BOOT = "192.168.1", 7


class Ring:
    """Python twin of tlog::Ring (writes the same bytes)."""
    def __init__(self):
        self.flash = [None] * NSEC
        self.seq, self.cur = 0, NSEC - 1
        self.lock = threading.Lock()

    def open(self, t):
        self.cur = (self.cur + 1) % NSEC
        self.seq += 1
        self.buf = bytearray(b"\xff" * SECTOR)
        self.buf[0:16] = struct.pack("<IIII", MAGIC, self.seq, BOOT, t)
        self.wpos, self.last = 16, t
        self.flash[self.cur] = self.buf

    def append(self, t, chan, src, payload):
        with self.lock:
            payload = bytes(payload[:254])
            if self.seq == 0 or self.wpos + 5 + len(payload) > SECTOR:
                self.open(t)
            dt = t - self.last
            assert dt <= 0xFFFF
            self.buf[self.wpos:self.wpos + 5] = struct.pack("<BBBH", len(payload), chan, src, dt)
            self.buf[self.wpos + 5:self.wpos + 5 + len(payload)] = payload
            self.wpos += 5 + len(payload)
            self.last = t

    def sectors(self):
        with self.lock:
            return [(struct.unpack_from("<I", b, 4)[0], bytes(b)) for b in self.flash if b is not None]


def pgn(src, num, data):
    b = bytearray([0x80, 0x81, src, num, len(data)]) + bytearray(data)
    b.append(sum(b[2:]) & 0xFF)
    return bytes(b)


def nmea(body):
    cs = 0
    for c in body:
        cs ^= ord(c)
    return f"${body}*{cs:02X}\r\n".encode()


class Sim:
    """Simple tractor: AgIO setpoints, a steering axle following them with lag, GPS, machine."""
    def __init__(self, ring):
        self.r, self.t = ring, 1000
        self.angle, self.sp, self.speed, self.prev_err, self.xte = -1.0, 0.0, 0.0, 0.0, 30.0
        self.head, self.kp = 90.0, 0.6
        self.counts, self.pgns, self.src = {}, {}, {}
        self.minute = {"seen": [0] * 6, "kept": [0] * 6, "bytes": [0] * 6}
        self.next = {"254": 0, "253": 0, "nmea": 0, "239": 0, "hello": 0, "stats": 60000, "rtcm": 0, "hyd": 400000}
        boot = f"fw=aog_logger-1.1 boot={BOOT} ip={SUBNET}.50 dhcp=0 ssid=AGOPENGPS configured=1 nmea=1 reset=1 ports=8888,9999,2233,4242,3344,4433"
        self.r.append(self.t, 0xF0, 0, boot.encode())
        self.r.append(self.t, 0xF0, 0, f"wifi=up ip={SUBNET}.50 rssi=-58".encode())
        self.marked = False

    def rec(self, port_i, ip_last, payload, keep=True, what=""):
        self.minute["seen"][port_i] += 1
        self.minute["bytes"][port_i] += len(payload)
        key = (ip_last, PORTS[port_i])
        s = self.src.setdefault(key, {"n": 0, "kept": 0, "last": 0, "what": what, "hist": []})
        s["n"] += 1; s["last"] = self.t; s["what"] = what; s["hist"].append(self.t)
        if payload[:2] == b"\x80\x81":
            k = (payload[2], payload[3]); p = self.pgns.setdefault(k, {"n": 0, "hist": []}); p["n"] += 1; p["hist"].append(self.t)
        if keep:
            self.minute["kept"][port_i] += 1; s["kept"] += 1
            self.r.append(self.t, port_i, ip_last, payload)

    def step_to(self, t_end):
        while self.t < t_end:
            self.t += 10
            t = self.t
            mins = t / 60000
            engaged = 1.5 < mins % 15 < 13.5
            target_speed = 8.0 if engaged else (2.0 if mins % 15 < 14.5 else 0.0)
            self.speed += (target_speed - self.speed) * 0.002
            if not self.marked and mins > 11:
                self.kp = 0.7
                self.r.append(t, 0xF1, 0, b"Kp 0.60 -> 0.70")
                self.marked = True
            if t >= self.next["254"]:
                self.next["254"] = t + 100
                self.xte += (-0.04 * self.xte + random.gauss(0, 0.4)) if engaged else random.gauss(0, 1)
                self.sp = max(-30, min(30, -0.08 * self.xte + 3 * math.sin(t / 23000) + (8 if (t // 45000) % 7 == 3 else 0)))
                x = 255 if not engaged else max(0, min(254, int(round(self.xte / 2)) + 127))
                d = struct.pack("<HBhB", int(self.speed * 10), 1 if engaged else 0, int(self.sp * 100), x) + bytes([0b00001111, 0])
                self.rec(0, 100, pgn(0x7F, 254, d), what="PGN 254 (AgIO)")
            if t >= self.next["253"]:
                self.next["253"] = t + 110
                err = self.angle - self.sp
                out = 0
                if engaged and self.speed > 0 and abs(err) >= 1.0:
                    out = int(self.kp * err + 0.1 * (err - self.prev_err) / 0.11)
                    out = (out + 40 if out > 0 else out - 40) if out else 0
                    out = max(-140, min(140, out))
                self.prev_err = err
                self.angle += -out * 0.004 + random.gauss(0, 0.02)
                self.head = (self.head + self.speed * 0.0005 * self.angle) % 360
                d = struct.pack("<hhhBB", int(self.angle * 100), int(self.head * 10), int(random.gauss(-9, 2)), 0, min(255, abs(out)))
                self.rec(1, 26, pgn(0x7E, 253, d), what="PGN 253 (steer)")
            if t >= self.next["239"]:
                self.next["239"] = t + 200
                self.rec(0, 100, pgn(0x7F, 239, bytes([0, int(self.speed * 10), 0, 0, 0, 0, 0b111, 0])), what="PGN 239 (AgIO)")
            if t >= self.next["hello"]:
                self.next["hello"] = t + 2000
                self.rec(0, 100, pgn(0x7F, 200, bytes([56, 0, 0])), what="PGN 200 (AgIO)")
                self.rec(1, 26, bytes([0x80, 0x81, 0x7E, 0x7E, 5]) + struct.pack("<hH", int(self.angle * 100), 11879) + bytes([0, 71]), what="PGN 126 (steer)")
                if (t // 2000) % 15 == 0:
                    self.rec(0, 100, pgn(0x7F, 252, bytes([60, 140, 50, 40, 210]) + struct.pack("<h", 12848) + bytes([90])), what="PGN 252 (AgIO)")
            if t >= self.next["nmea"]:
                self.next["nmea"] = t + 100
                sod = (8 * 3600 + 15 * 60 + t / 1000) % 86400
                hh = f"{int(sod // 3600):02d}{int(sod % 3600 // 60):02d}{sod % 60:05.2f}"
                fix = 5 if 6.0 < mins % 15 < 6.5 else 4
                for typ, body in (("GGA", f"GPGGA,{hh},3230.4912,S,05736.4056,W,{fix},{random.randint(16, 19)},0.{random.randint(7, 9)},81.9,M,17.0,M,1.{random.randint(0, 9)},0000"),
                                  ("VTG", f"GPVTG,{self.head:.1f},T,,M,{self.speed / 1.852:.2f},N,{self.speed:.2f},K,A"),
                                  ("RMC", f"GPRMC,{hh},A,3230.4912,S,05736.4056,W,{self.speed / 1.852:.2f},{self.head:.1f},171026,,,D")):
                    keep = int(t / 100) % 10 == 0
                    self.rec(1, 95, nmea(body), keep=keep, what=f"NMEA GP{typ}")
            if t >= self.next["rtcm"]:
                self.next["rtcm"] = t + 1000
                self.rec(2, 100, bytes(random.getrandbits(8) for _ in range(620)), keep=False, what="RTCM")
            if t >= self.next["hyd"]:
                self.next["hyd"] = t + 300000
                for _ in range(2):
                    self.rec(3, 61, bytes([0x3A, 0x3A, 0, 0x74]), what="hydraulic cmd 0")
            if t >= self.next["stats"]:
                self.next["stats"] = t + 60000
                m = self.minute
                st = struct.pack("<BbHIB", 1, random.randint(-64, -52), 210, t // 1000, 6) + struct.pack("<6H", *m["seen"]) + \
                     struct.pack("<6H", *m["kept"]) + struct.pack("<6I", *m["bytes"]) + struct.pack("<H", 0)
                self.r.append(t, 0xF2, 0, st)
                self.minute = {"seen": [0] * 6, "kept": [0] * 6, "bytes": [0] * 6}


ring = Ring()
sim = Sim(ring)
sim.step_to(HISTORY_MIN * 60000)
t_real0, t_sim0 = time.time(), sim.t
lock = threading.Lock()


def now_ms():
    return t_sim0 + int((time.time() - t_real0) * 1000)


def advance():
    with lock:
        sim.step_to(now_ms())


def page():
    src = open(os.path.join(HERE, "..", "aog_logger", "webpage.h")).read()
    return src.split('R"rawliteral(', 1)[1].rsplit(')rawliteral"', 1)[0].encode()


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json"):
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.send_header("Cache-Control", "no-store")
        self.end_headers(); self.wfile.write(body)

    def do_GET(self):
        advance()
        u = urlparse(self.path); q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/":
            return self.send(200, page(), "text/html")
        if u.path == "/api/status":
            n = now_ms()
            src = [{"ip": f"{SUBNET}.{ip}", "port": port, "what": s["what"], "r": sum(1 for x in s["hist"][-500:] if x > n - 10000),
                    "n": s["n"], "kept": s["kept"], "age": (n - s["last"]) / 1000} for (ip, port), s in sim.src.items()]
            pg = [{"src": a, "pgn": b, "n": p["n"], "r": sum(1 for x in p["hist"][-500:] if x > n - 10000)} for (a, b), p in sim.pgns.items()]
            used = min(NSEC, ring.seq)
            st = {"fw": "aog_logger-1.1 (mock)", "boot": BOOT, "now": n, "up": n // 1000, "rssi": -58,
                  "wifi": {"up": 1, "ssid": "AGOPENGPS", "ip": f"{SUBNET}.50", "rssi": -58, "ap": 0, "apClients": 0, "downFor": 0, "configured": 1},
                  "log": {"used": used, "total": NSEC, "rate": 690, "cov": NSEC * 4096 / 690 / 60, "ferr": 0, "seq": ring.seq},
                  "src": src, "pgns": pg}
            return self.send(200, json.dumps(st).encode())
        if u.path == "/api/cfg":
            return self.send(200, json.dumps({"ssid": "AGOPENGPS", "ip": f"{SUBNET}.50", "dhcp": 0, "nmea": 1, "configured": 1}).encode())
        if u.path == "/api/scan":
            return self.send(200, json.dumps({"nets": [{"ssid": "AGOPENGPS", "rssi": -52, "open": 0}, {"ssid": "Farm-WiFi", "rssi": -71, "open": 0},
                                                        {"ssid": "HIDRAULICO_SINSENAL", "rssi": -80, "open": 1}]}).encode())
        if u.path == "/log.bin":
            secs = ring.sectors()
            start = 0
            if "fromseq" in q:
                start = int(q["fromseq"])
            if "minutes" in q:
                cut = now_ms() - int(q["minutes"]) * 60000
                after = [s for s, b in secs if struct.unpack_from("<I", b, 12)[0] >= cut]
                start = max(start, (min(after) - 1) if after else ring.seq)
            body = b"".join(b for s, b in sorted(secs) if s >= start)
            return self.send(200, body, "application/octet-stream")
        self.send(404, b"Not found", "text/plain")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = parse_qs(self.rfile.read(n).decode())
        if self.path == "/api/mark" and "text" in body:
            with lock:
                ring.append(now_ms(), 0xF1, 0, body["text"][0].encode())
        self.send(200, b"OK", "text/plain")


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--dump":
        img = bytearray(b"\xff" * SECTOR * NSEC)
        for i, b in enumerate(ring.flash):
            if b is not None:
                img[i * SECTOR:(i + 1) * SECTOR] = b
        open(sys.argv[2], "wb").write(img)
        print(f"wrote {sys.argv[2]} ({ring.seq} sectors used)")
        sys.exit(0)
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    print(f"mock logger on http://127.0.0.1:{port}/  ({ring.seq} sectors of history)", flush=True)
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
