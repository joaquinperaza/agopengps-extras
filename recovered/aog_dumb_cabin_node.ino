/*
  AOG Dumb Cabin Motor Node  (ESP32-C3 Super Mini)
  ------------------------------------------------
  Pure actuator. Talks ONLY to the front-axle node over UDP (private ports,
  off the AgIO network). Receives a signed PWM command, drives a motor driver
  (PWM magnitude + DIR sign), and echoes an ACK that doubles as the heartbeat.

  Link frames (AOG-style, 8 bytes):
    CMD  front -> cabin :3344   {80 81 7A C0 seq pwmLo pwmHi crc}   (pwm = int16, signed)
    ACK  cabin -> front :4433   {80 81 7B C1 seqEcho pwmApplied status crc}

  Direction: sign of pwm -> DIR pin. Polarity is arbitrary - if the wheels turn
  the wrong way, flip "Invert Motor" in the AOG UI (handled on the front node).

  Safety: motor off at boot and whenever no valid CMD arrives for WATCHDOG_MS.
*/

#include <WiFi.h>
#include <WiFiUdp.h>

// ---------------- EDIT ME ----------------
#define WIFI_SSID  "AGOPENGPS"
#define WIFI_PASS  "agopengps"
IPAddress myIp(192, 168, 0, 27);          // must match front node's "cabin IP"
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);

// ---------------- pins (ESP32-C3 Super Mini) ----------------
const uint8_t PWM_PIN = 4;                // LEDC PWM out -> driver speed input
const uint8_t DIR_PIN = 5;                // direction (HIGH/LOW)
const uint8_t LED_PIN = 8;                // onboard LED (active LOW)

// ---------------- link ----------------
const uint16_t LISTEN_PORT  = 3344;       // CMD in from front
const uint8_t  SRC_CABIN    = 0x7B;
const uint8_t  PGN_ACK      = 0xC1;
const uint8_t  SRC_FRONT    = 0x7A;
const uint8_t  PGN_CMD      = 0xC0;

const uint16_t PWM_FREQ     = 5000;       // Hz
const uint8_t  PWM_RES      = 8;          // bits -> duty 0..255
const uint16_t WATCHDOG_MS  = 500;        // no CMD for this long -> motor off (CMD rate ~5 Hz)

WiFiUDP  udp;
uint32_t lastCmdMs = 0;
uint8_t  curDuty   = 0;                    // current motor magnitude (for LED debug)

static uint8_t crc8(const uint8_t *p) {   // sum of bytes [2..6]
  uint8_t c = 0;
  for (uint8_t i = 2; i <= 6; i++) c += p[i];
  return c;
}

void motorStop() {
  ledcWrite(PWM_PIN, 0);
  curDuty = 0;
}

void setup() {
  pinMode(DIR_PIN, OUTPUT);
  ledcAttach(PWM_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(LED_PIN, 1000, 8);           // LED via PWM = |pwm| brightness (debug)
  motorStop();                            // boot failsafe: motor off

  WiFi.mode(WIFI_STA);
  WiFi.config(myIp, gateway, subnet);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);                   // low-latency UDP
  while (WiFi.status() != WL_CONNECTED) delay(200);

  udp.begin(LISTEN_PORT);
}

void loop() {
  // --- receive CMD ---
  if (udp.parsePacket() >= 8) {
    uint8_t b[16];
    int n = udp.read(b, sizeof(b));
    if (n >= 8 && b[0] == 0x80 && b[1] == 0x81 &&
        b[2] == SRC_FRONT && b[3] == PGN_CMD && b[7] == crc8(b)) {

      int16_t pwm  = (int16_t)(b[5] | (b[6] << 8));   // signed
      uint8_t duty = abs(pwm) > 255 ? 255 : abs(pwm); // magnitude 0..255

      digitalWrite(DIR_PIN, pwm < 0 ? LOW : HIGH);    // sign -> direction
      ledcWrite(PWM_PIN, duty);                       // magnitude -> PWM
      curDuty = duty;
      lastCmdMs = millis();

      // --- ACK / heartbeat back to whoever sent the CMD ---
      uint8_t a[8] = { 0x80, 0x81, SRC_CABIN, PGN_ACK,
                       b[4], duty, (uint8_t)(duty ? 1 : 0), 0 };
      a[7] = crc8(a);
      udp.beginPacket(udp.remoteIP(), udp.remotePort());
      udp.write(a, sizeof(a));
      udp.endPacket();
    }
  }

  // --- watchdog: link lost -> stop the motor ---
  if ((millis() - lastCmdMs) >= WATCHDOG_MS) motorStop();

  // LED brightness = |pwm| (active-low, so invert). Dark = stopped/no link.
  ledcWrite(LED_PIN, 255 - curDuty);
}
