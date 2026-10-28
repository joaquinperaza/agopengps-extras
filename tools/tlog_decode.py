#!/usr/bin/env python3
"""Decode an aog_logger log (log.bin downloaded from the logger WebUI) into CSVs + a tuning summary.

  python3 -I tools/tlog_decode.py log.bin [-o outdir]

Format: see aog_logger/tlog.h. Outputs (in outdir, default <log>_decoded/):
  pgn254.csv   AgIO -> modules steer data: speed, status, setpoint, cross-track error, sections
  pgn253.csv   steer module -> AgIO: actual angle, heading, roll, switches, |pwm|
  machine.csv  AgIO -> machine module: PGN 239 (u-turn, hyd lift, tram, relays), 238 config, 229 sections
  nmea.csv     GPS sentences that were kept
  packets.csv  every kept packet (hex), for anything not decoded above
  events.csv   logger boots and user markers
  stats.csv    once-a-minute logger stats (WiFi RSSI, per-port packet counts, RTCM bytes)
  summary.txt  the same summary that is printed
"""
import argparse, csv, os, statistics as st, struct, sys

SECTOR, HDR, MAGIC = 4096, 16, 0x31474C54
CH_TRUNC, CH_BOOT, CH_MARK, CH_STATS, CH_TIME = 0x40, 0xF0, 0xF1, 0xF2, 0xF3
DEFAULT_PORTS = [8888, 9999, 2233, 4242, 3344, 4433]
AOG_SRC = {0x7F: "AgIO", 0x7E: "steer", 0x7B: "machine", 0x7C: "GPS", 0x79: "IMU", 0x7A: "front->cabin"}


class Rec:
    __slots__ = ("seq", "boot", "t", "chan", "src", "payload", "trunc", "port")

    def __init__(self, seq, boot, t, chan, src, payload):
        self.seq, self.boot, self.t, self.src, self.payload = seq, boot, t, src, payload
        self.chan = chan
        self.trunc = chan < 0x80 and bool(chan & CH_TRUNC)
        self.port = None


def read_sectors(data):
    """Valid sectors, newest copy per seq, in seq order."""
    by_seq = {}
    for off in range(0, len(data) - SECTOR + 1, SECTOR):
        magic, seq, boot, t0 = struct.unpack_from("<IIII", data, off)
        if magic == MAGIC and seq != 0xFFFFFFFF:
            by_seq[seq] = (boot, t0, data[off:off + SECTOR])
    return [(seq,) + by_seq[seq] for seq in sorted(by_seq)]


def parse(path):
    """Return (records, problems). Records are in write order; t is logger millis since its boot."""
    data = open(path, "rb").read()
    recs, problems = [], []
    for seq, boot, t0, sec in read_sectors(data):
        pos, t = HDR, t0
        while pos + 5 <= SECTOR:
            ln = sec[pos]
            if ln == 0xFF:
                break
            chan, src, dt = sec[pos + 1], sec[pos + 2], struct.unpack_from("<H", sec, pos + 3)[0]
            if pos + 5 + ln > SECTOR:
                problems.append(f"seq {seq}: record overruns sector at {pos}")
                break
            payload = sec[pos + 5:pos + 5 + ln]
            t += dt
            if chan == CH_TIME and ln == 4:
                t = struct.unpack("<I", payload)[0]
            else:
                recs.append(Rec(seq, boot, t, chan, src, payload))
            pos += 5 + ln
    # ports and subnet per boot from the boot records
    ports, prefix = {}, {}
    for r in recs:
        if r.chan == CH_BOOT:
            kv = dict(x.split("=", 1) for x in r.payload.decode(errors="replace").split() if "=" in x)
            if "ports" in kv:
                ports[r.boot] = [int(p) for p in kv["ports"].split(",") if p]
            if "ip" in kv:
                prefix[r.boot] = kv["ip"].rsplit(".", 1)[0]
    for r in recs:
        if r.chan < 0x80:
            pl = ports.get(r.boot, DEFAULT_PORTS)
            i = r.chan & 0x3F
            r.port = pl[i] if i < len(pl) else None
    return recs, problems, prefix


def kind(r):
    if r.chan >= 0xF0:
        return {CH_BOOT: "boot", CH_MARK: "marker", CH_STATS: "stats"}.get(r.chan, "logger")
    p = r.payload
    if len(p) >= 5 and p[0] == 0x80 and p[1] == 0x81:
        return "pgn"
    if p[:1] == b"$":
        return "nmea"
    if r.port == 4242 and len(p) >= 4 and p[0] == 0x3A and p[1] == 0x3A:
        return "hydraulic"
    return "raw"


def s16(p, i):
    return struct.unpack_from("<h", p, i)[0]


def utc_offsets(recs):
    """Per boot: seconds to add to logger time to get UTC seconds of day (from the first GGA/RMC)."""
    off = {}
    for r in recs:
        if r.boot in off or kind(r) != "nmea":
            continue
        f = r.payload.decode(errors="replace").split(",")
        if len(f) > 1 and f[0][3:6] in ("GGA", "RMC") and len(f[1]) >= 6:
            try:
                sod = int(f[1][0:2]) * 3600 + int(f[1][2:4]) * 60 + float(f[1][4:])
                off[r.boot] = sod - r.t / 1000.0
            except ValueError:
                pass
    return off


def fmt_utc(sod):
    sod %= 86400
    return f"{int(sod // 3600):02d}:{int(sod % 3600 // 60):02d}:{sod % 60:05.2f}"


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))] if v else float("nan")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("-o", "--outdir")
    a = ap.parse_args()
    out = a.outdir or os.path.splitext(a.log)[0] + "_decoded"
    os.makedirs(out, exist_ok=True)
    recs, problems, prefix = parse(a.log)
    utc = utc_offsets(recs)

    def ip(r):
        return f"{prefix.get(r.boot, 'x.x.x')}.{r.src}"

    def ts(r):
        return f"{r.t / 1000:.3f}"

    def utcs(r):
        return fmt_utc(r.t / 1000 + utc[r.boot]) if r.boot in utc else ""

    w = {}
    def writer(name, header):
        f = open(os.path.join(out, name), "w", newline="")
        cw = csv.writer(f); cw.writerow(header); w[name] = (f, cw); return cw

    c254 = writer("pgn254.csv", ["boot", "t_s", "utc", "src", "speed_kmh", "status", "setpoint_deg", "xte_cm", "sc1to8", "sc9to16"])
    c253 = writer("pgn253.csv", ["boot", "t_s", "utc", "src", "actual_deg", "heading_deg", "roll_deg", "work_sw", "steer_sw_off", "pwm_abs"])
    cmc = writer("machine.csv", ["boot", "t_s", "utc", "src", "pgn", "uturn", "speed_kmh", "hyd_lift", "tram",
                                  "relay_lo", "relay_hi", "sections_1to64_hex", "raw_hex"])
    cnm = writer("nmea.csv", ["boot", "t_s", "utc", "src", "port", "sentence"])
    cpk = writer("packets.csv", ["boot", "t_s", "src", "port", "kind", "aog_src", "pgn", "len", "truncated", "hex"])
    cev = writer("events.csv", ["boot", "t_s", "utc", "type", "text"])
    cst = writer("stats.csv", ["boot", "t_s", "rssi_dbm", "heap_kb", "uptime_s", "flash_errors", "port", "seen", "kept", "bytes"])

    counts, boots = {}, {}
    steer, xte = [], []          # engaged samples
    last254 = {}                  # boot -> (t, setpoint, status, speed)
    t253 = {}                     # (boot, src) -> list of t
    for r in recs:
        k = kind(r)
        b = boots.setdefault(r.boot, [r.t, r.t]); b[0] = min(b[0], r.t); b[1] = max(b[1], r.t)
        p = r.payload
        if k in ("boot", "marker"):
            cev.writerow([r.boot, ts(r), utcs(r), k, p.decode(errors="replace")])
            continue
        if k == "stats":
            if len(p) >= 9 and p[0] == 1:
                rssi, heap, up, n = struct.unpack_from("<bHIB", p, 1)
                o = 9
                seen = struct.unpack_from(f"<{n}H", p, o); o += 2 * n
                kept = struct.unpack_from(f"<{n}H", p, o); o += 2 * n
                byts = struct.unpack_from(f"<{n}I", p, o); o += 4 * n
                ferr = struct.unpack_from("<H", p, o)[0] if o + 2 <= len(p) else ""
                for i in range(n):
                    port = DEFAULT_PORTS[i] if i < len(DEFAULT_PORTS) else i
                    cst.writerow([r.boot, ts(r), rssi, heap, up, ferr, port, seen[i], kept[i], byts[i]])
            continue
        key = (k, r.port, p[2] if k == "pgn" else None, p[3] if k == "pgn" else None)
        counts[key] = counts.get(key, 0) + 1
        cpk.writerow([r.boot, ts(r), ip(r), r.port, k, f"0x{p[2]:02X}" if k == "pgn" else "",
                      p[3] if k == "pgn" else "", len(p), int(r.trunc), p.hex()])
        if k == "nmea":
            cnm.writerow([r.boot, ts(r), utcs(r), ip(r), r.port, p.decode(errors="replace").strip()])
        elif k == "pgn" and p[3] == 0xFE and p[2] == 0x7F and len(p) >= 13:
            speed = struct.unpack_from("<H", p, 5)[0] / 10
            status, sp = p[7], s16(p, 8) / 100
            xr = p[10]
            x = "" if xr == 255 else (xr - 127) * 2
            c254.writerow([r.boot, ts(r), utcs(r), ip(r), speed, status, sp, x, p[11], p[12]])
            last254[r.boot] = (r.t, sp, status, speed)
            if status == 1 and speed > 0 and x != "":
                xte.append(x)
        elif k == "pgn" and p[2] == 0x7F and p[3] in (239, 238, 229) and len(p) >= 13:
            if p[3] == 239:
                cmc.writerow([r.boot, ts(r), utcs(r), ip(r), 239, p[5], p[6] / 10, p[7], p[8], p[11], p[12], "", p.hex()])
            elif p[3] == 229:
                cmc.writerow([r.boot, ts(r), utcs(r), ip(r), 229, "", "", "", "", "", "", p[5:13].hex(), p.hex()])
            else:
                cmc.writerow([r.boot, ts(r), utcs(r), ip(r), 238] + [""] * 7 + [p.hex()])
        elif k == "pgn" and p[3] == 0xFD and len(p) >= 13:
            ang, hd, rl = s16(p, 5) / 100, s16(p, 7), s16(p, 9)
            c253.writerow([r.boot, ts(r), utcs(r), ip(r), ang, "" if hd == 9999 else hd / 10,
                           "" if rl == 8888 else rl / 10, p[11] & 1, (p[11] >> 1) & 1, p[12]])
            t253.setdefault((r.boot, r.src), []).append(r.t)
            l = last254.get(r.boot)
            if l and r.t - l[0] <= 300 and l[2] == 1 and l[3] > 0:
                steer.append((ang - l[1], p[12], l[3]))
    for f, _ in w.values():
        f.close()

    # ---------------- summary ----------------
    L = []
    L.append(f"log: {a.log}   records: {len(recs)}   problems: {len(problems)}")
    for pr in problems[:5]:
        L.append(f"  ! {pr}")
    for bt, (t0, t1) in sorted(boots.items()):
        u = f"  UTC {fmt_utc(t0 / 1000 + utc[bt])}..{fmt_utc(t1 / 1000 + utc[bt])}" if bt in utc else ""
        L.append(f"boot {bt}: logger t {t0 / 1000:.1f}..{t1 / 1000:.1f} s ({(t1 - t0) / 60000:.1f} min){u}")
    L.append("packets kept (kind, port, AOG source, PGN):")
    for (k, port, s, pg), n in sorted(counts.items(), key=lambda kv: -kv[1]):
        who = f" {AOG_SRC.get(s, hex(s))} PGN {pg}" if k == "pgn" else ""
        L.append(f"  {n:7d}  {k:<9} port {port}{who}")
    for (bt, src), tl in sorted(t253.items()):
        d = [(b - a_) for a_, b in zip(tl, tl[1:])]
        if d:
            L.append(f"PGN 253 from .{src} (boot {bt}): interval mean {st.mean(d):.0f} ms, p95 {pct(d, 95):.0f} ms, "
                     f"gaps>300ms {sum(1 for x in d if x > 300)}  (broadcast: some loss at the logger is normal)")
    if steer:
        e = [s[0] for s in steer]
        L.append(f"steering while engaged: {len(e)} samples (~{len(e) * 0.11 / 60:.1f} min)")
        L.append(f"  angle error (actual - setpoint): mean {st.mean(e):+.2f}  RMS {(sum(x * x for x in e) / len(e)) ** 0.5:.2f}  "
                 f"p95|e| {pct([abs(x) for x in e], 95):.2f}  max|e| {max(abs(x) for x in e):.2f} deg")
        pw = [s[1] for s in steer]
        L.append(f"  |pwm|: mean {st.mean(pw):.0f}  p95 {pct(pw, 95)}  at 0: {100 * sum(1 for x in pw if x == 0) / len(pw):.0f}%")
        for lo, hi in ((0, 4), (4, 8), (8, 12), (12, 99)):
            band = [s[0] for s in steer if lo <= s[2] < hi]
            if band:
                L.append(f"  speed {lo:>2}-{hi:<2} km/h: n {len(band):6d}  RMS error {(sum(x * x for x in band) / len(band)) ** 0.5:.2f} deg")
    else:
        L.append("steering while engaged: no samples (needs PGN 254 with status 1 and speed > 0)")
    if xte:
        L.append(f"cross-track error while engaged: n {len(xte)}  mean {st.mean(xte):+.1f}  RMS {(sum(x * x for x in xte) / len(xte)) ** 0.5:.1f}  "
                 f"p95|xte| {pct([abs(x) for x in xte], 95)} cm  (AOG sends it in 2 cm steps, +-254 cm)")
    text = "\n".join(L)
    print(text)
    open(os.path.join(out, "summary.txt"), "w").write(text + "\n")
    print(f"\nCSV files in {out}/")


if __name__ == "__main__":
    main()
