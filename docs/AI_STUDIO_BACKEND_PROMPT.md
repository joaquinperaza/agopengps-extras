# Prompt for Google AI Studio: AOG logger backend + packet inspector

Copy everything below the line into Google AI Studio. Optional: paste the contents of
`tools/tlog_decode.py` (the reference decoder) at the end, where it says so.

---

You are building a small Google Cloud backend and web app for an agricultural project. Build it
complete, deployable, and as cheap as possible (it should run almost entirely in the free tier).

## Context

A tractor runs AgOpenGPS (AOG), autosteer software. Its modules talk over WiFi via UDP. A passive
logger (an ESP32-C3 microcontroller) records that UDP traffic into a rolling log on its flash.
When it has internet, it uploads the log in ~1 MB chunks to your backend. The owner wants to:
1. store every uploaded chunk durably,
2. decode it into packets and fields,
3. inspect it in a web UI, to tune the autosteer parameters (PID gains, PWM limits, etc.).

## Part 1: Upload API (the ESP32 firmware will be written against this exact contract; do not change it)

The device is a microcontroller: keep the API trivial. No multipart, no JSON body, no redirects,
no cookies.

### `GET /v1/ping`
- Header `Authorization: Bearer <device_key>`
- `200` with body `ok`, or `401`. The device uses it to check connectivity before uploading.

### `POST /v1/upload`
Headers:
- `Authorization: Bearer <device_key>`
- `X-Device-Id`: short ASCII id, e.g. `tractor1-logger`
- `X-First-Seq`, `X-Last-Seq`: decimal, the lowest and highest sector `seq` in the body
- `X-Content-SHA256`: lowercase hex SHA-256 of the body
- `Content-Type: application/octet-stream`
- `Content-Length`: a multiple of 4096, max 2 MiB

Body: raw concatenated 4096-byte log sectors (format below), in any order.

Responses (small JSON, the device only checks the status code):
- `200 {"stored":"<object path>","sectors":N,"new":K,"duplicates":D}`
- `400`: bad length, bad SHA-256, or no valid sector
- `401`: unknown or revoked device key
- `413`: body over 2 MiB
- `429`: rate limit (max 60 uploads/hour/device)

Uploads **must be idempotent**. The device retries when it misses a response, so the same sectors
can arrive more than once. A sector's identity is `(device_id, boot, seq)`. Store the raw body
anyway, but never create duplicate decoded records.

Also return `200` on a duplicate upload. The device treats `200` as "these sectors are safe" and
moves on.

## Part 2: Log format (binary, little-endian)

The log is a sequence of 4096-byte **sectors**. Erased flash is `0xFF`.

Sector header (16 bytes):
| off | type | field |
|---|---|---|
| 0 | u32 | magic = `0x31474C54` (ASCII "TLG1") |
| 4 | u32 | seq: increases by 1 for every sector ever written by that device (higher = newer) |
| 8 | u32 | boot: the logger's boot counter when the sector was opened |
| 12 | u32 | t0: logger uptime in ms (millis) when the sector was opened |

Then **records** back to back from offset 16, never crossing the sector end:
| off | type | field |
|---|---|---|
| 0 | u8 | len: payload length 0..254. **0xFF = end of records in this sector** |
| 1 | u8 | chan (below) |
| 2 | u8 | src: last octet of the sender's IPv4 address (0 for logger records) |
| 3 | u16 | dt: ms since the previous record in this sector (first record: since t0) |
| 5 | u8[len] | payload |

The record time is `t0 + sum(dt)`, in logger ms since that boot.

`chan`:
- `0x00..0x3F`: UDP port index; `+0x40` if the packet was truncated to 254 bytes. The index maps
  through the `ports=` list in that boot's boot record (default `8888,9999,2233,4242,3344,4433`).
- `0xF0` **boot/info**: ASCII `key=value` pairs separated by spaces, e.g.
  `fw=aog_logger-1.0 boot=12 ip=192.168.1.50 dhcp=0 ssid=AGOPENGPS nmea=1 reset=1 ports=8888,9999,2233,4242,3344,4433`.
  Also `wifi=up ip=... rssi=-55`, `wifi=down`, `log cleared`. The subnet comes from `ip=`, so a
  full sender IP is `<first 3 octets of ip>.<src>`.
- `0xF1` **marker**: ASCII text the user typed, e.g. `Kp 0.60 -> 0.70`. Show these prominently:
  they mark parameter changes.
- `0xF2` **stats**, once a minute, packed binary: `u8 ver(=1), i8 rssi_dbm, u16 free_heap_kb,
  u32 uptime_s, u8 n, u16 seen[n], u16 kept[n], u32 bytes[n], u16 flash_errors`. Per port index:
  packets seen, packets stored, bytes seen, during the last minute.
- `0xF3` **time sync**: payload is a u32 absolute ms. Set the running time to it (used when dt
  would overflow).
- Ignore other `0xF0+` values (forward compatibility).

**Wall-clock time**: the logger has no clock. Per boot, derive UTC from the first NMEA GGA/RMC
sentence in the log: `hhmmss.ss` in field 1, and the date from RMC field 9 `ddmmyy`. Then
`utc = logger_ms + offset`. Without NMEA, show relative time (boot N + seconds).

## Part 3: Payload decoding (AgOpenGPS UDP protocol)

Classify each packet payload:
- **AOG binary PGN**: `80 81 <src> <pgn> <len> <data...> <crc>`. crc = low byte of the sum of
  bytes 2..(n-2). Mark packets with a bad CRC, don't drop them.
  Sources: `0x7F` AgIO, `0x7E` steer module, `0x7B` machine, `0x7C` GPS, `0x79` IMU.
- **NMEA**: ASCII starting with `$` (GGA, RMC, VTG, HDT, PANDA, PAOGI, ...).
- **Hydraulic command** (port 4242): `3A 3A <cmd 0..4> <crc>`.
- **Raw**: anything else.

PGNs to decode into named fields (byte offsets from the start of the payload):
- **254 (0xFE) AgIO → steer**: speed = u16@5 / 10 km/h; status = u8@7 (1 = autosteer on);
  setpoint = i16@8 / 100 degrees; lineDistance = u8@10 (255 = no line; else cross-track error =
  (value − 127) × 2 cm); sections 1–8 = u8@11, 9–16 = u8@12.
- **253 (0xFD) steer module → AgIO**: actual steer angle = i16@5 / 100 degrees;
  heading = i16@7 / 10 deg (9999 = no IMU); roll = i16@9 / 10 deg (8888 = no IMU);
  switch byte u8@11 (bit 0 work switch, bit 1 steer switch, **1 = off**); |pwm| = u8@12 (0–255).
- **252 (0xFC) steer settings**: Kp u8@5, highPWM u8@6, lowPWM u8@7, minPWM u8@8, counts u8@9,
  WAS offset i16@10, Ackerman u8@12 (%).
- **251 (0xFB) steer config**: bits u8@5 (0 invertWAS, 1 relayActiveHigh, 2 motorDir,
  3 singleInputWAS, 4 Cytron, 5 steerSwitch, 6 steerButton, 7 shaftEncoder); pulse count max u8@6;
  bits u8@8 (0 Danfoss, 1 pressure sensor, 2 current sensor, 3 use Y axis).
- **239 (0xEF) machine data**: uTurn u8@5, speed u8@6 / 10, hydLift u8@7, tram u8@8,
  relays lo u8@11 / hi u8@12.
- **238 (0xEE) machine config**: raiseTime u8@5, lowerTime u8@6, enableToolLift u8@7, bits u8@8,
  user1–4 u8@9..12.
- **229 (0xE5)**: 64 section bits in u8@5..12.
- **200** hello from AgIO; **201** subnet change (`len=5, 201, 201, ip1, ip2, ip3`);
  **202** scan request; **203** scan reply (module IP + subnet).
- The steer module's hello reply is `80 81 7E 7E 05 <angle i16> <raw WAS u16> <switch> <x>`.
- Keep every other PGN as "PGN n from <src>" with hex, so new ones are still visible.

The steer module computes its motor command (PWM) about every **110 ms**. AgIO sends PGN 254 at
the GPS rate (typically 10 Hz).

## Part 4: Google Cloud architecture

- **Cloud Run** service (Python FastAPI preferred, or Node). Scale to zero, max 2 instances.
- **Cloud Storage** bucket for the raw bodies:
  `raw/<device_id>/<yyyy-mm-dd>/<first_seq>-<last_seq>-<sha8>.bin`. Use a lifecycle rule to move
  objects to Nearline after 30 days.
- **BigQuery** for decoded data, tables partitioned by date and clustered by device and boot:
  - `sectors` (device_id, boot, seq, t0_ms, uploaded_at, raw_object): the dedup key
  - `packets` (device_id, boot, seq, t_ms, utc_ts, src_ip, port, kind, aog_src, pgn, len,
    truncated, crc_ok, hex)
  - `steer` (device_id, boot, t_ms, utc_ts, pgn, speed_kmh, status, setpoint_deg, xte_cm,
    actual_deg, heading_deg, roll_deg, work_sw, steer_sw_off, pwm_abs)
  - `machine` (decoded 239/238/229)
  - `nmea` (device_id, boot, t_ms, utc_ts, src_ip, sentence, type, fix_quality, sats, hdop,
    lat, lon, speed_kmh)
  - `events` (boot records, markers, wifi up/down)
  - `stats` (per minute and per port: rssi, heap, seen, kept, bytes, flash_errors)
  Decode synchronously in the upload request if it takes under ~10 s. Otherwise store the raw
  object, return 200, and decode asynchronously (Pub/Sub or Cloud Tasks).
- **Device keys**: random 32-byte keys. Store only SHA-256 hashes (Firestore or Secret Manager).
  Provide a small CLI/script to create, list and revoke keys. Never log keys.
- **Optional Google Drive mirror** (setting, default off): copy each raw chunk, and a daily
  `summary.txt` plus decoded CSVs, into a Drive folder shared with the service account. This lets
  an AI assistant with Drive access read the logs.
- Budget alert at a small amount (e.g. 5 USD/month). Document the expected monthly cost.

## Part 5: Web frontend ("packet inspector")

Single-page app served by the same Cloud Run service (or Firebase Hosting). Access is restricted
to the owner's Google account: use IAP or Firebase Auth with an email allowlist that is configured
at deploy time. Only the device API is public, protected by device keys.

Pages:
1. **Sessions**: one row per (device, boot): UTC start/end, duration, packet counts by kind,
   minutes with autosteer engaged, markers count, worst WiFi RSSI, number of gaps.
2. **Timeline** (the main tool) for a chosen session or time range, with synchronized,
   zoomable/pannable charts sharing one time axis:
   - steer setpoint (PGN 254) vs actual angle (PGN 253), plus the error (actual − setpoint)
   - |pwm|
   - speed and autosteer status (shade the engaged periods)
   - cross-track error (cm)
   - heading and roll
   - WiFi RSSI and packet rates per source (from stats)
   - markers as labelled vertical lines across all charts; clicking one jumps there
   - gaps: highlight intervals where PGN 253 or PGN 254 is missing for more than 300 ms
3. **Packets**: virtualised table (must handle 500k+ rows), with filters by time range, source
   IP, port, kind, AOG source, PGN, CRC failures and free-text hex search. A detail pane shows a
   byte-by-byte breakdown with field names (Part 3). "Export CSV" exports the current filter.
4. **Tuning report** for a selected range, or **A/B between two ranges** (e.g. before/after a
   marker), side by side:
   - engaged time
   - steering error mean / RMS / p95 / max overall and by speed band (0–4, 4–8, 8–12, 12+ km/h)
   - cross-track error mean / RMS / p95
   - |pwm| distribution and % of time at 0
   - oscillation indicator (dominant frequency of the error signal, e.g. via FFT)
   - the most recent PGN 252/251 values seen
5. **Sources**: who talks on which port, rates, last seen, total bytes (including RTCM on 2233).

Use a simple, fast, readable design that works on a tablet in the tractor cab. Charts must stay
smooth with a few hundred thousand points (downsample per zoom level, e.g. min/max buckets).

## Part 6: Deliverables

1. Complete source code: backend, decoder module, frontend, infra.
2. Deployment from a fresh Google Cloud project, as copy-paste `gcloud` commands or Terraform:
   enabling APIs, bucket, BigQuery dataset, service account with least privilege, Cloud Run,
   IAP/Auth.
3. A script to create a device key that prints the key once.
4. A `curl` example uploading a local `log.bin` (the logger's WebUI also lets the user download
   a file in the same format), so the pipeline can be tested before the device firmware exists.
5. Decoder unit tests covering:
   - sectors in random order
   - duplicate sectors
   - a sector with no records
   - a truncated record (`chan & 0x40`)
   - a time-sync record (`0xF3`)
   - a `0xFF` end marker mid-sector
   - a bad CRC PGN
6. A README covering cost, limits, and how to revoke a lost device.

Ask me before deviating from Part 1 or Part 2: they are fixed by existing firmware.

(Optional: the reference Python decoder that already exists for this format follows. Match its
behaviour.)
