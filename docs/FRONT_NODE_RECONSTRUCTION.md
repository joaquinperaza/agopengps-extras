# Front node reconstruction (2026-10-16)

The lost tabs of `aog_front_node` were rebuilt by black-box testing the live module
(192.168.1.26) **without changing its settings**:
- the WebUI was read with GET `/`, `/cfg` and `/snap` only
- the cabin node was powered off, and this PC took its IP (.27) as a fake cabin
- only PGN 254 (plus one harmless hello test) was sent to the module

All raw captures are in [`live_capture/`](live_capture/).

## Sketch layout

```
aog_front_node/
  aog_front_node.ino   recovered (2 small fixes, see below)
  link.ino             recovered (pwmApplied type fix)
  led.ino              NEW: status NeoPixel (colour scheme guessed, see file header)
  steer.ino            NEW: engage logic + PID, fitted to live measurements
  web.ino              NEW: WebUI, page byte-identical to the live one
recovered/             untouched copies of everything recovered from the HDD
test/steer_replay.cpp  host test: steer.ino vs recorded live values
tools/                 sniffers, fake cabin + PGN 254 probes used for this work
```

Run the steering test:
```
g++ -std=c++17 -Wall -o /tmp/steer_replay test/steer_replay.cpp && /tmp/steer_replay
```

## Steering behaviour measured on the live module

Live config (`/cfg`): Kp 0.60, Ki 0, Kd 0.10, highPWM 140, lowPWM 50, minPWM 40, dead zone 1.0°,
maxSteer 50°, counts/deg 210, WAS zero 12848, Ackerman 90 %, motor dir normal.

| Item | Finding |
|---|---|
| Error | `err = steerAngle − setpoint` (AOG convention). The sign of pwm follows err |
| Output | `pwm = (int16)(Kp·err + Ki·∫ + Kd·(err − prevErr)/0.110)`, then ±minPWM if non-zero |
| Dead zone | `|err| < deadZone` → pwm 0 **and an early return; prevErr is not updated**. This causes the large first-tick kicks (+46, +69) when leaving the dead zone |
| Truncation | because of the int cast, pwm stays 0 until `Kp·|err| ≥ 1` (1.67° with Kp 0.6). Measured boundary: 1.6° → 0, 1.7° → 41 |
| dt | fixed 0.110 s (= DATA_PERIOD_MS), not the measured time |
| Engage | guidance status ≠ 0 **and** speed > 0 (0.1 km/h engages) **and** fresh AOG packet **and** cabin heartbeat alive |
| Disengage | pwm 0, and prevErr resets to 0 |
| AOG timeout | pwm → 0 about 450–500 ms after the last AOG packet |
| Cabin timeout | pwm → 0 between 220 and 330 ms after the last ACK (3/3 runs) |
| switchByte | `0x00` while the cabin heartbeat is alive, `0x02` (steer switch "off") while it is not |
| Low/high PWM ramp | never reached in the probes, so it is implemented as described in the UI text ("ramps from low to high over 5 degrees") |
| Ki | 0 on the live module, so the integral term is untested (anti-windup added) |

A hysteresis hypothesis (based on one odd sample in run 2) was tested in run 4 and **falsified**.
The sample did not reproduce in run 5, so it was most likely WiFi packet loss.

## Deliberate deviations from the live module

1. **Steering timeout uses PGN 254 only.** On the live module, AgIO hello packets (PGN 200)
   also keep steering engaged: in `hello_probe.txt` pwm stayed at −43 for 2 s with no PGN 254.
   If AgOpenGPS freezes or closes while AgIO keeps running, the live module keeps holding the
   last commanded angle indefinitely. **Worth fixing on the gold module too.**
2. **Setpoint is clamped to ±maxSteerAngle.** The live module does not clamp: setpoint 70° with
   maxSteer 50 gave pwm −84, which fits error 74.6°. The UI text promises "PID will never command
   beyond this".
3. **Ping, I2C scan and BNO reset are refused while steering.** They block `loop()` for up to
   1.3 s, and the cabin watchdog would cut the motor.
4. A subnet change from AgIO (PGN 201) is now actually saved. The recovered code rebooted
   before the NVS write ran.
5. `pwmApplied` is unsigned, so duty values above 127 no longer show as negative.

## Verified identical
- The WebUI page (8626 bytes) is identical to the live page.
- `/cfg` and `/snap` produce exactly the live JSON for the live values (same printf formats).
- Hello reply `80 81 7e 7e 05 | angle | rawWAS | switchByte | 71` matches the live capture.
- PGN 253 rate ≈ 112 ms, sent from source port 8888 with a valid checksum.

## Bench validation on real hardware (2026-10-17)

Board: QT Py ESP32-S3 (same model as the gold module) with BNO085 + ADS1115, A0–A1 jumpered.
It ran at .36 with cabin IP = this PC (.113). WAS zero was set to 969, so the fixed WAS reads the
gold's −4.614°. Build target: `esp32:esp32:adafruit_qtpy_esp32s3_n4r2`, esp32 core 3.3.12.
The first flash erased the whole chip.

| Check | Result |
|---|---|
| Compile | no errors (1.03 MB, 35 % of flash) |
| Boot | ADS1115 0x48 + BNO085 0x4A found, PGN 253 every ~110 ms, valid checksum |
| WebUI page / headers / 404 | byte-identical to gold |
| `/savecfg` → `/reboot` → `/cfg` | settings and WiFi password persist |
| Steering probe (`bench_probe_run2.txt` vs gold `pid_probe_run2_fake_cabin.txt`) | identical, including the Kd transients (−58, −49, +42) and re-engage −47. The only differences are the gold's ±1 flicker from real WAS jitter, and the deliberate setpoint clamp (−72/67 vs −84/79) |
| AOG-silence / cabin-ACK timeouts | 0.39 s / 0.29 s (gold 0.38 s / 0.28 s) |
| Hello-only (`bench_hello_probe.txt`) | pwm → 0 at 0.46 s (gold kept steering) ✓ |
| `/ping` `/scan` `/bnorst` while steering (`bench_web_guard.txt`) | 409 refused, steering uninterrupted. Idle: ping 2–3 ms, scan OK |
| "Zero WAS now" (`/zero`) | wz 969 → current raw (0), angle → 0.0° in `/snap` and PGN 253, survives reboot. Only tested with raw = 0 (jumper); a pot would test a non-zero raw |

Flashing note (S3 native USB): after an upload the board can stay in the ROM bootloader
(USB ID `303a:1001`). `esptool --before no-reset --after watchdog-reset read-mac` starts the app.

## Not yet verified
- The LED colours are guessed.
- The PGN 201 subnet save was not exercised on the bench.
- Real WAS / motor on the tractor.

## Testing the backup board safely
- Never run the backup and the gold module with the same cabin IP while the real cabin is on:
  two front nodes would command one motor.
- Bench-test the backup with its own IP (e.g. .36) and its cabin IP pointing at this PC.
  Then run `tools/cabin_probe.py <backup-ip> <pc-ip>` and compare with
  `live_capture/pid_probe_run2_fake_cabin.txt`.
