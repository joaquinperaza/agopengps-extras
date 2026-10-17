/*
  Steering logic: PGN 254 setpoint -> PID -> signed pwmCommand for the cabin node.

  Reconstructed from black-box probes of the live module (docs/live_capture/pid_probe_*.txt).
  Every steady-state and transient value in those runs is reproduced by this code
  (see test/steer_replay.cpp).

    err   = steerAngle - steerSetpoint            (AOG convention: + = wheels right of target)
    out   = Kp*err + Ki*integral + Kd*(err - prevError)/dt     dt fixed = DATA_PERIOD_MS
    pwm   = (int16_t)out, then +/- minPWM if non-zero (stiction kick)
    limit = lowPWM..highPWM ramp over LOW_HIGH_DEGREES of error

  Engaged only when: guidance on AND speed > 0 AND fresh PGN 254 AND cabin heartbeat alive.
  Inside the dead zone the PID returns early WITHOUT touching prevError (matches live).
  Disengage resets prevError/integral.

  Deliberate deviations from the live module (safer, invisible in normal use):
    - steering timeout keys on PGN 254 only (live also accepts AgIO hello, which keeps
      steering engaged forever if AgOpenGPS stops while AgIO runs)
    - setpoint is clamped to +/-maxSteerAngle (live does not clamp, despite the UI text)
*/

const float    PID_DT_S          = DATA_PERIOD_MS / 1000.0f;
const uint16_t STEER_TIMEOUT_MS  = 450;    // live: 450-500 ms after last AOG packet
const uint16_t CABIN_TIMEOUT_MS  = 300;    // live: 220-330 ms after last ACK
const float    LOW_HIGH_DEGREES  = 5.0f;   // UI: "Ramps from low to high over 5 degrees"

float prevError = 0.0f;
float integral  = 0.0f;

bool cabinAlive() {
  return lastHeartbeatMs != 0 && (millis() - lastHeartbeatMs) < CABIN_TIMEOUT_MS;
}

void updateSteering() {
  bool alive = cabinAlive();
  switchByte = alive ? 0x00 : 0x02;        // bit1 = steer switch (active low): OFF while motor node is down

  bool fresh = lastPgn254Ms != 0 && (millis() - lastPgn254Ms) < STEER_TIMEOUT_MS;
  steerEngaged = alive && fresh && guidanceStatus != 0 && aogSpeed > 0.0f;

  if (!steerEngaged) {
    pwmCommand = 0;
    prevError  = 0.0f;
    integral   = 0.0f;
    return;
  }

  float sp = steerSetpoint;
  float msa = (float)cfg.maxSteerAngle;
  if (sp >  msa) sp =  msa;
  if (sp < -msa) sp = -msa;

  float err = steerAngle - sp;
  if (fabsf(err) < cfg.deadZone) {         // hold still; prevError intentionally untouched
    pwmCommand = 0;
    return;
  }

  // Ki is 0 on the live module, so this term is untested against it. Anti-windup:
  // the integral term alone can never exceed highPWM.
  if (cfg.ki > 0.0f) {
    integral += err * PID_DT_S;
    float iMax = cfg.highPWM / cfg.ki;
    if (integral >  iMax) integral =  iMax;
    if (integral < -iMax) integral = -iMax;
  } else {
    integral = 0.0f;
  }

  float out = cfg.kp * err + cfg.ki * integral + cfg.kd * (err - prevError) / PID_DT_S;
  prevError = err;

  int16_t pwm = (int16_t)out;              // truncates toward zero, like the live module
  if (pwm > 0)      pwm += cfg.minPWM;
  else if (pwm < 0) pwm -= cfg.minPWM;

  float ae = fabsf(err);
  int16_t newMax = (ae < LOW_HIGH_DEGREES)
      ? (int16_t)(cfg.lowPWM + (cfg.highPWM - cfg.lowPWM) * ae / LOW_HIGH_DEGREES)
      : cfg.highPWM;
  if (pwm >  newMax) pwm =  newMax;
  if (pwm < -newMax) pwm = -newMax;

  if (cfg.motorDriveDir) pwm = -pwm;
  pwmCommand = pwm;
}
