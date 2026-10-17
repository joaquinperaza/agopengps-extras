// Host-side replay of steer.ino against values recorded from the live front node
// (docs/live_capture/pid_probe_run*.txt).  Build & run:
//   g++ -std=c++17 -Wall -o /tmp/steer_replay test/steer_replay.cpp && /tmp/steer_replay
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <vector>

// ---- minimal stand-ins for what steer.ino uses from the main sketch ----
static uint32_t fakeMillis = 1000;
uint32_t millis() { return fakeMillis; }

struct Config {
  float countsPerDegree, ackermanFix, kp, ki, kd, deadZone;
  uint8_t maxSteerAngle, highPWM, lowPWM, minPWM, motorDriveDir;
} cfg;

const uint16_t DATA_PERIOD_MS = 110;
float    steerAngle = 0, steerSetpoint = 0, aogSpeed = 0;
uint8_t  guidanceStatus = 0, switchByte = 0;
int16_t  pwmCommand = 0;
bool     steerEngaged = false;
uint32_t lastPgn254Ms = 0, lastHeartbeatMs = 0;

#include "../aog_front_node/steer.ino"

// ---- harness ----
static int failures = 0;

// Live config read from GET /cfg (docs/live_capture/cfg.json)
static void liveConfig() {
  cfg.kp = 0.60f; cfg.ki = 0.0f; cfg.kd = 0.10f; cfg.deadZone = 1.0f;
  cfg.maxSteerAngle = 50; cfg.highPWM = 140; cfg.lowPWM = 50; cfg.minPWM = 40; cfg.motorDriveDir = 0;
}

// One PID tick with AOG + cabin both fresh (like the probes, which sent PGN 254 every 100 ms).
static int16_t tick(float sp, uint8_t status = 1, float speed = 5.0f) {
  fakeMillis += DATA_PERIOD_MS;
  steerSetpoint = sp; guidanceStatus = status; aogSpeed = speed;
  lastPgn254Ms = fakeMillis - 20;
  lastHeartbeatMs = fakeMillis - 50;
  updateSteering();
  return pwmCommand;
}

// Hold a setpoint for n ticks and compare to the expected pwm sequence (shorter expected = rest repeats last).
static void expectSeq(const char *label, float sp, std::vector<int> expected, int n = 12, uint8_t status = 1, float speed = 5.0f) {
  std::vector<int> got;
  for (int i = 0; i < n; i++) got.push_back(tick(sp, status, speed));
  bool ok = true;
  for (int i = 0; i < n; i++) {
    int e = expected[i < (int)expected.size() ? i : expected.size() - 1];
    if (got[i] != e) ok = false;
  }
  printf("%s %-46s got [", ok ? "PASS" : "FAIL", label);
  for (int i = 0; i < n && i < 6; i++) printf("%d%s", got[i], i < 5 && i < n - 1 ? "," : "");
  printf(n > 6 ? ",...]\n" : "]\n");
  if (!ok) failures++;
}

int main() {
  liveConfig();
  steerAngle = -4.61f;                      // WAS reading during all probes

  puts("-- run 5 replay: reset, -30, dz, kick, 1.99, 2.99 --");
  expectSeq("disengaged", 0, {0}, 4, 0);
  expectSeq("E=-30.01 first tick -85, steady -58", 25.4f, {-85, -58});
  expectSeq("E=+0.49 dead zone", -5.1f, {0});
  expectSeq("E=+1.49 kick +69 (prevError kept), then 0", -6.1f, {69, 0});
  expectSeq("E=+1.99 -> 41", -6.6f, {41});
  expectSeq("E=+2.99 first 42, steady 41", -7.6f, {42, 41});

  puts("-- run 3 B: dead zone keeps prevError --");
  expectSeq("disengaged", 0, {0}, 4, 0);
  expectSeq("E=-5.01 first -47, steady -43", 0.4f, {-47, -43});
  expectSeq("E=+0.49 dead zone", -5.1f, {0});
  expectSeq("E=+1.19 kick +46 then 0", -5.8f, {46, 0});
  expectSeq("E=+0.89 dead zone", -5.5f, {0});
  expectSeq("E=-5.01 first -48 (prev=1.19), steady -43", 0.4f, {-48, -43});
  expectSeq("E=+1.04 kick +46 then 0", -5.65f, {46, 0});

  puts("-- run 4 mirror --");
  expectSeq("E=+1.9 -> 41", -6.51f, {41}, 12);
  expectSeq("E=-1.5 first -43, then 0", -3.11f, {-43, 0});
  expectSeq("E=-1.9 first -41, steady -41", -2.71f, {-41});

  puts("-- run 2 steady-state table --");
  struct { float sp; int pwm; } table[] = {
    {-2.6f, -41}, {-1.6f, -41}, {-0.6f, -42}, {0.4f, -43}, {1.4f, -43}, {3.4f, -44},
    {10.4f, -49}, {25.4f, -58}, {-7.6f, 41}, {-9.6f, 42}, {-19.6f, 48},
  };
  for (auto &t : table) {
    for (int i = 0; i < 6; i++) tick(t.sp);
    char label[64]; snprintf(label, sizeof label, "steady sp=%.1f -> %d", t.sp, t.pwm);
    expectSeq(label, t.sp, {t.pwm}, 4);
  }

  puts("-- gating (live: speed 0 / status 0 / no ACK / no PGN254 -> 0) --");
  expectSeq("speed 0", 0.4f, {0}, 4, 1, 0.0f);
  expectSeq("speed 0.1 engages", 0.4f, {-47, -43}, 4, 1, 0.1f);
  expectSeq("status 0", 0.4f, {0}, 4, 0);
  {
    tick(0.4f); tick(0.4f);
    fakeMillis += DATA_PERIOD_MS; lastPgn254Ms = fakeMillis - 20; lastHeartbeatMs = fakeMillis - 310;
    updateSteering();
    bool ok = pwmCommand == 0 && switchByte == 0x02;
    printf("%s %-46s pwm=%d sw=0x%02x\n", ok ? "PASS" : "FAIL", "cabin heartbeat 310 ms old -> 0, sw=0x02", pwmCommand, switchByte);
    if (!ok) failures++;
    fakeMillis += DATA_PERIOD_MS; lastPgn254Ms = fakeMillis - 460; lastHeartbeatMs = fakeMillis - 50;
    updateSteering();
    ok = pwmCommand == 0 && switchByte == 0x00;
    printf("%s %-46s pwm=%d sw=0x%02x\n", ok ? "PASS" : "FAIL", "PGN254 460 ms old -> 0, sw=0x00", pwmCommand, switchByte);
    if (!ok) failures++;
  }

  puts("-- deliberate deviation: setpoint clamped to maxSteerAngle (live gave -84 / 79) --");
  expectSeq("disengaged", 0, {0}, 2, 0);
  expectSeq("sp=70 clamped to 50 -> kick -122, steady -72", 70.0f, {-122, -72}, 4);   // E=-54.61
  for (int i = 0; i < 4; i++) tick(-70.0f);
  expectSeq("sp=-70 clamped to -50 -> steady 67", -70.0f, {67}, 4);

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
