# Reverse-engineering notes — recovered AgOpenGPS extras

Reconstructed from the 5 `.ino` files recovered after the HDD loss (kept unchanged in
`recovered/`). The front node's missing tabs have been rebuilt; see
[FRONT_NODE_RECONSTRUCTION.md](FRONT_NODE_RECONSTRUCTION.md).

## 1. System overview

Two independent subsystems share the tractor WiFi `AGOPENGPS` / `agopengps`, subnet `192.168.0.x`.

```
                      WiFi "AGOPENGPS"  192.168.0.0/24
 ┌──────────────┐  UDP 8888 <- PGN 254/252/251/200/201/202
 │ AgOpenGPS +  │ ───────────────────────────────┐
 │ AgIO (tablet)│ <─────────────────────────────┐ │
 └──────────────┘  UDP 9999 <- PGN 253, hello,  │ │
                   scan reply (broadcast .255)  │ │
                                                │ v
                                   ┌──────────────────────────┐
                                   │ FRONT-AXLE NODE  .26     │  QT Py ESP32-S3
                                   │ WAS (ADS1115) + IMU      │  aog_front_node.ino
                                   │ (BNO085), PID, WebUI     │  + link.ino (+ lost tabs)
                                   └──────────────────────────┘
                                        │ CMD :3344   ^ ACK :4433
                                        v (unicast)   │
                                   ┌──────────────────────────┐
                                   │ CABIN MOTOR NODE  .27    │  ESP32-C3 Super Mini
                                   │ PWM + DIR -> motor driver│  aog_dumb_cabin_node.ino
                                   └──────────────────────────┘

 ┌──────────────────────┐  UDP broadcast :4242  ┌──────────────────────────┐
 │ HYDRAULIC REMOTE     │ ────────────────────> │ HYDRAULIC RELAY BOX .42  │
 │ ("llave") 5 buttons  │   {3A 3A cmd crc} x2  │ LILYGO T-Relay, 4 relays │
 │ ESP32-C3, DHCP       │                       │ + WebSocket web UI :80   │
 └──────────────────────┘                       └──────────────────────────┘
   llave_hidraulico.ino                           rele_hidraulico.ino
```

If the `AGOPENGPS` network is down, the relay box starts its own open AP,
`HIDRAULICO_SINSENAL` ("hydraulic, no signal"), on 192.168.0.42. The remote falls back to that
SSID, so the hydraulics keep working without the tractor router.

## 2. Autosteer: front-axle node (`aog_front_node.ino` + `link.ino`)

**Hardware:** Adafruit QT Py ESP32-S3, 4 MB flash / 2 MB PSRAM (the recovered comment says "ESP32 Pico (#5426)", but the USB ID and the live MAC say S3). I2C on `Wire1` (STEMMA QT), 100 kHz:
- ADS1115 @0x48: WAS (wheel angle sensor), differential A0−A1 (wiper→A0, sensor ground→A1), gain 1, 128 SPS
- BNO085 @0x4A (or 0x4B): game rotation vector at 50 Hz, giving heading and roll

**Loop timing**
| Task | Period |
|---|---|
| WAS sample, non-blocking ADS trigger/read, 6-sample median | 20 ms (50 Hz) |
| IMU read → `updateSteering()` (PID) → `sendCmd()` to cabin → `sendPgn253()` to AgIO | 110 ms (~9 Hz) |
| Status LED | 200 ms |
| Web server | separate FreeRTOS task; turns itself off 10 min after boot (except in AP mode) |

**WAS math:** `angle = (median(raw) - wasZero) / countsPerDegree`, negated if `invertWas`, and
multiplied by `ackermanFix/100` when positive.

**AgOpenGPS protocol (v5/v6 compatible)**, listening on :8888 and sending to `x.x.x.255:9999`:
| PGN in | Action |
|---|---|
| 254 (0xFE) | speed (×0.1), guidanceStatus, steer setpoint (int16 ×0.01°) |
| 200 (0xC8) hello | reply `80 81 7E 7E 5 angle(2) rawWAS(2) switchByte 71` |
| 202 (0xCA) scan | reply PGN 203 with own IP and subnet, sent to 255.255.255.255 |
| 201 (0xC9) subnet | change the first 3 IP octets (own and cabin IP), save, reboot |
| 252 / 251 | **received but ignored**: all steer tuning is done locally in the WebUI `/setup` page |

PGN 253 out: `80 81 7E FD 08 | angle×100 (i16) | heading×10 (i16) | roll×10 (i16) | switchByte | |pwm| | crc`.
Without an IMU it sends heading 9999 and roll 8888, which are AOG's "no IMU" values.

**Config** (NVS namespace `aogfront`, key `cfg`, magic `0xA6F70007`). Defaults:
SSID AGOPENGPS/agopengps, IP .26 static, cabin .27, countsPerDegree 120, ackerman 100,
maxSteer 40°, differential WAS on, IMU on, Kp 0.2, Ki 0, Kd 0.1, highPWM 20, lowPWM 1, minPWM 1,
deadZone 1.0°, motorDriveDir 0.
Fallback AP: `AOG-FrontNode` / `12345678`.

## 3. Private front↔cabin link (`link.ino`, `aog_dumb_cabin_node.ino`)

8-byte AOG-style frames. The checksum (crc) is the sum of bytes 2..6.
```
CMD  front -> cabin:3344   80 81 7A C0 seq pwmLo pwmHi crc     pwm = int16, signed
ACK  cabin -> front:4433   80 81 7B C1 seq duty  status crc    duty = |pwm| 0..255, status = duty?1:0
```
- The front sends a CMD on every PID tick, even with pwm=0, so the ACK also works as a heartbeat.
- The front accepts an ACK only if it echoes a seq sent less than 500 ms ago. It also measures RTT.
- **Cabin:** PWM on GPIO4 (5 kHz, 8 bit), DIR on GPIO5 (HIGH = positive pwm), onboard LED GPIO8
  (active-low) dims with |pwm|. The motor stops if no valid CMD arrives for 500 ms.
- The WebUI ping test uses the same CMD with pwm=0, five times, with a 200 ms timeout each.

## 4. Hydraulic remote + relay box

**Remote (`llave_hidraulico.ino`):** ESP32-C3 with 5 buttons on GPIO {6,3,2,5,4} (pull-ups,
active LOW, 50 ms debounce). Button *i* broadcasts `3A 3A i crc` to 192.168.0.255:4242
**twice**, 100 ms apart. It tries AGOPENGPS for about 24 s, then waits on the open
`HIDRAULICO_SINSENAL` network indefinitely.

**Relay box (`rele_hidraulico.ino`):** LILYGO T-Relay, static IP .42. Relays on GPIO 21/19/18/5.
| cmd | Label (UI) | Relays ON |
|---|---|---|
| 0 | Levante I (lift left) | R1 |
| 1 | Levante D (lift right) | R2 |
| 2 | Apertura I (open left) | R3 + R4 |
| 3 | "Apertura D", actually **ALL OFF / stop** | none |
| 4 | Backup | R4 |

Only one state is active at a time. Any state other than 3 times out to OFF after **120 s** of
inactivity. The web UI at `http://192.168.0.42/` uses a WebSocket on `/ws` and syncs the active
button across all connected clients.

## 5. What is missing (lost with the HDD)

The front node calls four functions that are not defined anywhere, and several variables are
written but never read. Together they show which sketch tabs were lost:

| Lost tab (likely name) | Must define | Evidence of what it did |
|---|---|---|
| `steer.ino` / `pid.ino` | `updateSteering()` | Uses `steerSetpoint`, `steerAngle`, `guidanceStatus`, `aogSpeed`, `cfg.kp/ki/kd`, `highPWM/lowPWM/minPWM`, `deadZone`, `maxSteerAngle`, `motorDriveDir`; sets `pwmCommand` (signed) and `steerEngaged`, and probably `switchByte` |
| `web.ino` | `setupWeb()` | `/setup` page for all `Config` fields; I2C scan (`reqScan`/`scanResult`), BNO reset (`reqBnoReset`), cabin ping (`reqPing`/`pingResults`); shows `linkRttMs`, `pwmApplied`, `cabinStatus`, `bnoRetries`, live WAS/IMU; probably a "set WAS zero" button |
| `led.ino` | `initStatusLed()`, `updateStatusLed()` | Uses `lastAogMs` (AgIO alive) and `lastHeartbeatMs` (cabin alive). The QT Py ESP32-S3 has an onboard NeoPixel (`PIN_NEOPIXEL` + `NEOPIXEL_POWER`) |

Also: Arduino needs each sketch in a folder with the same name. The front node is
`aog_front_node/aog_front_node.ino` + `link.ino` + the lost tabs. The other three are separate
sketches, so this flat folder will not compile as-is.

## 6. Bugs and risks found while reading

**Front node**
1. **Subnet change from AgIO is never saved.** `handleSubnet()` calls `saveConfig()`, which
   only sets a flag, then reboots 100 ms later. The real NVS write happens in `loop()`, which
   never runs again. Fix: call `doSaveConfig()` directly there. The cabin node's IP is also
   hard-coded, so after a subnet change it would not match anyway.
2. `pwmApplied` is `int8_t`, but the cabin sends 0..255, so any duty above 127 shows as negative.
   Use `uint8_t`.
3. The cabin ping test blocks `loop()` for up to ~1.25 s. During that time no CMD frames are
   sent, so the cabin watchdog (500 ms) cuts the motor. This is fine as long as you never run
   it while steering.
4. The IMU reports at 50 Hz but is read only once per 110 ms tick. Check that the heading isn't
   lagging; reading all pending events, or setting the report interval to ~100 ms, avoids it.
5. The hello reply ends with a fixed `71` instead of a computed CRC. AgIO tolerates this.
6. Ackerman correction applies to positive angles. Official firmware applies it to negative
   angles. That may be intentional because of `invertWas`.
7. Comments disagree with the code: "8 Hz" vs 110 ms, and the cabin comment says "~5 Hz".

**Relay box** (this one is safety-relevant, because the relays drive hydraulics)
1. **CRC is computed but never checked.** Any UDP packet on :4242 starting `3A 3A` with byte 2
   in 0..4 moves the relays.
2. **WebSocket input uses `atoi()`.** Any non-numeric text gives 0, which turns on
   *Levante I* (R1).
3. `data[len] = 0` writes one byte past the WebSocket buffer, a known issue in the
   ESPAsyncWebServer example code.
4. `cmd >= 0` is always true for `uint8_t`. This is harmless.
5. The timeout comment says 60 s, but the code uses 120 s.
6. The UI labels button 3 "Apertura D", but it is the stop/all-off state.

**Remote**
1. The comment says 10 s, but the code waits 30 × 800 ms ≈ 24 s.
2. The remote can deadlock. If it misses AGOPENGPS at boot (for example, the router boots
   slowly) while the relay box *does* join AGOPENGPS, the remote waits forever for
   `HIDRAULICO_SINSENAL`, which never appears. It needs a power-cycle.
