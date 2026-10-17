/*
  AOG Front-Axle Node  (sparse-node architecture, Option B)
  ---------------------------------------------------------
  Reads WAS (ADS1115) + IMU (BNO085), sends PGN 253 (0xFD) to AgIO over UDP.
  100% AOG-compatible (v5/v6): answers Hello/scan, receives PGN 252/251 (acked
  but ignored — all steer tuning is owned locally via the /setup WebUI page).

  Architecture modeled on the official AOG ESP32 firmware (MTZ v44):
    - Web server runs in a separate FreeRTOS task (never blocks control loop)
    - ADS1115 uses non-blocking trigger/read pattern (never waits for conversion)
    - vTaskDelay() yields to WiFi stack between cycles

  Board : Adafruit QT Py ESP32-S3, 4MB Flash / 2MB PSRAM
          (FQBN esp32:esp32:adafruit_qtpy_esp32s3_n4r2; the recovered comment said
          "QT Py ESP32 Pico (#5426)", but USB ID 239a:8143 and the live module's MAC say S3)
  I2C chain (STEMMA QT, solderless):  QT Py -> BNO085 -> ADS1115 (end)
      ADS1115  addr 0x48   WAS  (differential A0-A1: wiper->A0, gnd-sense->A1)
      BNO085   addr 0x4A   IMU  (heading + roll)

  Libraries (Library Manager):
      "Adafruit ADS1X15"
      "Adafruit BNO08x"
*/

#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_BNO08x.h>

// ---------------- Config (persisted in NVS) ----------------
struct Config {
  uint32_t magic;
  char     ssid[33];
  char     pass[64];
  uint8_t  ip[4];
  bool     useDhcp;
  int16_t  wasZero;
  float    countsPerDegree;
  float    ackermanFix;
  uint8_t  maxSteerAngle;
  bool     wasDifferential;
  bool     invertWas;
  bool     imuEnable;
  uint8_t  cabinIp[4];
  float    kp;
  uint8_t  highPWM;
  uint8_t  lowPWM;
  uint8_t  minPWM;
  uint8_t  motorDriveDir;
  float    deadZone;
  float    ki;
  float    kd;
};

#define CFG_MAGIC 0xA6F70007UL
Config cfg;
Preferences prefs;

// First-boot addresses (only used when NVS has no config). Overridable at build time so a
// bench board never comes up on the tractor module's addresses, e.g.
//   -DDEFAULT_IP=192,168,1,36 -DDEFAULT_CABIN_IP=192,168,1,113
#ifndef DEFAULT_IP
#define DEFAULT_IP 192, 168, 0, 26
#endif
#ifndef DEFAULT_CABIN_IP
#define DEFAULT_CABIN_IP 192, 168, 0, 27
#endif

void setDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = CFG_MAGIC;
  strlcpy(cfg.ssid, "AGOPENGPS", sizeof(cfg.ssid));
  strlcpy(cfg.pass, "agopengps", sizeof(cfg.pass));
  const uint8_t defIp[4] = { DEFAULT_IP }, defCabin[4] = { DEFAULT_CABIN_IP };
  memcpy(cfg.ip, defIp, 4);
  cfg.useDhcp = false;
  cfg.wasZero = 0;
  cfg.countsPerDegree = 120.0f;
  cfg.ackermanFix = 100.0f;
  cfg.maxSteerAngle = 40;
  cfg.wasDifferential = true;
  cfg.invertWas = false;
  cfg.imuEnable = true;
  memcpy(cfg.cabinIp, defCabin, 4);
  cfg.kp = 0.2f; cfg.highPWM = 20; cfg.lowPWM = 1; cfg.minPWM = 1;
  cfg.motorDriveDir = 0;
  cfg.deadZone = 1.0f;
  cfg.ki = 0.0f;
  cfg.kd = 0.1f;
}

void loadConfig() {
  prefs.begin("aogfront", false);
  size_t n = prefs.getBytesLength("cfg");
  if (n == sizeof(Config)) {
    prefs.getBytes("cfg", &cfg, sizeof(Config));
    if (cfg.magic != CFG_MAGIC) setDefaults();
  } else {
    setDefaults();
  }
}

volatile bool reqSave = false;

void saveConfig() {
  reqSave = true;   // actual NVS write happens in main loop (thread-safe)
}

void doSaveConfig() {
  cfg.magic = CFG_MAGIC;
  prefs.putBytes("cfg", &cfg, sizeof(Config));
  reqSave = false;
}

// ---------------- AOG networking ----------------
const uint16_t AOG_PORT_LISTEN = 8888;
const uint16_t AOG_PORT_SEND   = 9999;
WiFiUDP   udp;
IPAddress aogBroadcast(192, 168, 0, 255);
bool      apMode = false;


// ---------------- Devices ----------------
Adafruit_ADS1115 ads;
Adafruit_BNO08x  bno(-1);
sh2_SensorValue_t bnoVal;

// I2C bus speed for the STEMMA QT chain. The BNO08x violates I2C setup-timing when
// it stretches the clock, and the ESP32's I2C peripheral is sensitive to it (see
// Adafruit "troublesome chips"). At 400kHz the BNO drops off the bus on ESP32 —
// 100kHz is the reliable speed here. (Official AOG runs 400kHz, but that firmware
// targets Teensy, which doesn't have this clock-stretching bug.) The ADS1115 is
// happy at 100kHz too. Drop to 50000 if it's still marginal.
static const uint32_t BNO_I2C_HZ = 100000;

bool adsOk = false, bnoOk = false;
uint32_t lastImuMs = 0, lastBnoTryMs = 0;
uint32_t bnoRetries = 0;
bool wasConvReady = false;                 // non-blocking ADS: conversion triggered?

WebServer server(80);
bool webActive = true;
const uint32_t WEB_TIMEOUT_MS = 600000UL;

// ---------------- Status LED ----------------
uint32_t lastAogMs       = 0;
uint32_t lastPgn254Ms    = 0;              // steering timeout keys on this, not on hello/scan
uint32_t lastHeartbeatMs = 0;

// ---------------- Live state ----------------
volatile int16_t wasRaw = 0;
float steerAngle  = 0.0f;
float imuHeading  = 0.0f;
float imuRoll     = 0.0f;

float   steerSetpoint  = 0.0f;
uint8_t guidanceStatus = 0;
float   aogSpeed       = 0.0f;
uint8_t switchByte     = 0;

int16_t pwmCommand  = 0;
bool    steerEngaged = false;
uint8_t pwmApplied   = 0;              // cabin duty 0..255 (unsigned)
uint8_t cabinStatus  = 0;

uint8_t pgn253[14];

// ---------------- Timing ----------------
uint32_t lastWasMs = 0, lastPgnMs = 0, lastImuServiceMs = 0;
const uint16_t WAS_PERIOD_MS     = 20;     // 50 Hz WAS sampling (feeds median filter)
const uint16_t DATA_PERIOD_MS    = 110;    // ~9 Hz PID + CMD + PGN (matches official AOG)

// I2C request flags — web task sets these, main loop executes them.
// Prevents two threads from hitting Wire1 simultaneously.
volatile bool    reqScan     = false;
volatile bool    reqBnoReset = false;
volatile bool    reqPing     = false;
volatile bool    pingDone    = false;
volatile int16_t pingResults[5];           // RTT in ms for each ping (-1 = timeout)

// Forward declarations for link.ino variables (compiled after main .ino)
extern WiFiUDP  linkUdp;
extern uint8_t  linkSeq;
volatile uint8_t scanResult[128];
volatile uint8_t scanCount   = 0;
volatile bool    scanDone    = false;
volatile bool    bnoResetDone = false;
volatile bool    bnoResetOk   = false;

// ============================================================
// FreeRTOS tasks — decoupled from control loop

// Web server task
void webTask(void *param) {
  for (;;) {
    if (webActive) {
      server.handleClient();
      if (!apMode && millis() > WEB_TIMEOUT_MS) {
        server.stop();
        webActive = false;
        Serial.println("WebUI stopped (config window elapsed)");
      }
    }
    vTaskDelay(5);
  }
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nAOG Front-Axle Node");

  loadConfig();
  initStatusLed();

  i2cRecover(SDA1, SCL1);
  Wire1.begin();
  Wire1.setClock(BNO_I2C_HZ);          // 100 kHz — BNO08x is unreliable faster on ESP32
  Wire1.setTimeOut(50);                // 50ms I2C timeout — hung sensor can't kill the loop
  initBno();
  Wire1.setClock(BNO_I2C_HZ);          // restore after BNO init may change it
  initAds();

  startWifi();
  udp.begin(AOG_PORT_LISTEN);
  initLink();
  setupWeb();

  // Web server in its own FreeRTOS task (doesn't block control loop)
  xTaskCreate(webTask, "web", 5000, NULL, 1, NULL);

  Serial.printf("ADS=%s  BNO=%s\n", adsOk ? "ok" : "FAIL", bnoOk ? "ok" : "FAIL");

  vTaskDelay(500);                     // let everything settle
}

void loop() {
  uint32_t now = millis();

  handleAogUdp();
  handleLink();

  // ── WAS read: 50 Hz, non-blocking (fast sensor, clean angle) ──
  if (now - lastWasMs >= WAS_PERIOD_MS) {
    lastWasMs = now;
    readWas();
  }

  // ── PID + CMD + PGN: 8 Hz (sensor → PID → actuator → report, all in one tick) ──
  if (now - lastPgnMs >= DATA_PERIOD_MS) {
    lastPgnMs = now;
    serviceImu();
    updateSteering();                    // PID computes pwmCommand
    sendCmd();                           // send to cabin IMMEDIATELY after PID
    sendPgn253();                        // report to AOG

  }

  static uint32_t lastLedMs = 0;
  if (now - lastLedMs >= 200) { lastLedMs = now; updateStatusLed(); }

  // Handle I2C requests from web task (thread-safe: only main loop touches Wire1)
  if (reqScan) {
    reqScan = false;
    scanCount = 0;
    for (uint8_t a = 1; a < 127; a++) {
      Wire1.beginTransmission(a);
      if (Wire1.endTransmission() == 0 && scanCount < sizeof(scanResult))
        scanResult[scanCount++] = a;
    }
    scanDone = true;
  }
  if (reqBnoReset) {
    reqBnoReset = false;
    Wire1.end();
    i2cRecover(SDA1, SCL1);
    Wire1.begin();
    Wire1.setClock(BNO_I2C_HZ);
    delay(500);
    bnoResetOk = bnoBegin();
    adsOk = ads.begin(0x48, &Wire1);
    ads.setGain(GAIN_ONE);
    ads.setDataRate(RATE_ADS1115_128SPS);
    wasConvReady = false;
    bnoResetDone = true;
  }
  if (reqPing) {
    reqPing = false;
    IPAddress cabin(cfg.cabinIp[0], cfg.cabinIp[1], cfg.cabinIp[2], cfg.cabinIp[3]);
    for (uint8_t i = 0; i < 5; i++) {
      uint8_t seq = ++linkSeq;
      uint8_t f[8] = { 0x80, 0x81, 0x7A, 0xC0, seq, 0, 0, 0 };
      f[7] = f[2] + f[3] + f[4] + f[5] + f[6];
      linkUdp.beginPacket(cabin, 3344);
      linkUdp.write(f, 8);
      linkUdp.endPacket();
      uint32_t t0 = millis();
      pingResults[i] = -1;
      while (millis() - t0 < 200) {
        if (linkUdp.parsePacket() >= 8) {
          uint8_t r[16];
          linkUdp.read(r, sizeof(r));
          if (r[0]==0x80 && r[1]==0x81 && r[2]==0x7B && r[3]==0xC1 && r[4]==seq) {
            pingResults[i] = (int16_t)(millis() - t0);
            break;
          }
        }
        vTaskDelay(1);
      }
      if (i < 4) vTaskDelay(50);
    }
    pingDone = true;
  }

  if (reqSave) doSaveConfig();

  vTaskDelay(1);                       // yield to WiFi stack + other FreeRTOS tasks
}

// ---------------- Devices init ----------------
void initAds() {
  adsOk = ads.begin(0x48, &Wire1);
  if (adsOk) {
    ads.setGain(GAIN_ONE);
    ads.setDataRate(RATE_ADS1115_128SPS);
  }
}

void i2cRecover(uint8_t sda, uint8_t scl) {
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, OUTPUT);
  for (uint8_t i = 0; i < 9 && digitalRead(sda) == LOW; i++) {
    digitalWrite(scl, HIGH); delayMicroseconds(5);
    digitalWrite(scl, LOW);  delayMicroseconds(5);
  }
  pinMode(sda, OUTPUT);
  digitalWrite(sda, LOW);  delayMicroseconds(5);
  digitalWrite(scl, HIGH); delayMicroseconds(5);
  digitalWrite(sda, HIGH); delayMicroseconds(5);
}

bool bnoBegin() {
  if (bno.begin_I2C(0x4A, &Wire1) || bno.begin_I2C(0x4B, &Wire1)) {
    bno.enableReport(SH2_GAME_ROTATION_VECTOR, 20000);
    bnoOk = true;
    lastImuMs = millis();
    return true;
  }
  bnoOk = false;
  return false;
}

void initBno() {
  delay(800);
  const int MAX_TRIES = 10;
  for (int attempt = 0; attempt < MAX_TRIES; attempt++) {
    if (bnoBegin()) break;
    Serial.printf("BNO init retry %d/%d...\n", attempt + 1, MAX_TRIES);
    // Every 3rd miss, clock out a possibly-stuck device and re-init the bus.
    // NOTE: if the BNO only shows on an I2C scan a few % of the time, the bus is
    // electrically marginal — fix BNO_I2C_HZ / pullups, don't just retry harder.
    if (attempt % 3 == 2) {
      Wire1.end();
      i2cRecover(SDA1, SCL1);
      Wire1.begin();
      Wire1.setClock(BNO_I2C_HZ);
      Wire1.setTimeOut(50);
    }
    delay(300);
  }
  if (!bnoOk) {
    cfg.imuEnable = false;                 // disable IMU if it won't init (like official)
    Serial.println("BNO not found — IMU disabled");
  }
}

// Soft BNO retry — no bus teardown, ADS stays safe
void serviceImu() {
  if (!cfg.imuEnable) return;
  if (!bnoOk) {
    if (millis() - lastBnoTryMs > 5000) {
      lastBnoTryMs = millis();
      bnoRetries++;
      bnoBegin();
    }
    return;
  }
  readImu();
  if (millis() - lastImuMs > 3000) bnoOk = false;
}

// ---------------- WAS (non-blocking ADS + median filter) ----------------
#define WAS_BUF_SIZE 6                     // median window: ~6 samples per 8 Hz PID tick
int16_t wasBuf[WAS_BUF_SIZE];
uint8_t wasBufIdx = 0;
bool    wasBufFull = false;

static int cmpInt16(const void *a, const void *b) {
  return (int)(*(const int16_t *)a) - (int)(*(const int16_t *)b);
}

int16_t wasMedian() {
  uint8_t n = wasBufFull ? WAS_BUF_SIZE : wasBufIdx;
  if (n == 0) return 0;
  int16_t tmp[WAS_BUF_SIZE];
  memcpy(tmp, wasBuf, n * sizeof(int16_t));
  qsort(tmp, n, sizeof(int16_t), cmpInt16);
  return tmp[n / 2];
}

void readWas() {
  if (!adsOk) return;

  if (wasConvReady) {
    int16_t raw = ads.getLastConversionResults();

    // Store in circular buffer for median
    wasBuf[wasBufIdx] = raw;
    wasBufIdx++;
    if (wasBufIdx >= WAS_BUF_SIZE) { wasBufIdx = 0; wasBufFull = true; }

    wasRaw = wasMedian();

    float counts = (float)(wasRaw - cfg.wasZero);
    if (cfg.invertWas) counts = -counts;
    float ang = counts / (cfg.countsPerDegree > 0.01f ? cfg.countsPerDegree : 1.0f);
    if (ang > 0) ang *= (cfg.ackermanFix / 100.0f);
    steerAngle = ang;
  }

  if (cfg.wasDifferential)
    ads.startADCReading(ADS1X15_REG_CONFIG_MUX_DIFF_0_1, false);
  else
    ads.startADCReading(ADS1X15_REG_CONFIG_MUX_SINGLE_0, false);
  wasConvReady = true;
}

// ---------------- IMU ----------------
void readImu() {
  if (!bnoOk) return;
  if (bno.wasReset()) bno.enableReport(SH2_GAME_ROTATION_VECTOR, 20000);
  if (!bno.getSensorEvent(&bnoVal)) return;
  if (bnoVal.sensorId != SH2_GAME_ROTATION_VECTOR) return;
  lastImuMs = millis();

  float qr = bnoVal.un.gameRotationVector.real;
  float qi = bnoVal.un.gameRotationVector.i;
  float qj = bnoVal.un.gameRotationVector.j;
  float qk = bnoVal.un.gameRotationVector.k;

  float yaw  = atan2f(2.0f * (qr * qk + qi * qj), 1.0f - 2.0f * (qj * qj + qk * qk));
  float roll = atan2f(2.0f * (qr * qi + qj * qk), 1.0f - 2.0f * (qi * qi + qj * qj));

  float h = -(yaw * 57.29578f);
  if (h < 0 && h >= -180) h += 360.0f;
  imuHeading = h;
  imuRoll    = roll * 57.29578f;
}

// ---------------- PGN 253 -> AgIO ----------------
void sendPgn253() {
  pgn253[0] = 0x80;
  pgn253[1] = 0x81;
  pgn253[2] = 0x7E;
  pgn253[3] = 0xFD;
  pgn253[4] = 8;

  int16_t was100 = (int16_t)(steerAngle * 100.0f);
  pgn253[5] = (uint8_t)(was100 & 0xFF);
  pgn253[6] = (uint8_t)(was100 >> 8);

  uint16_t head10, roll10;
  if (cfg.imuEnable && bnoOk) {
    head10 = (uint16_t)(int16_t)(imuHeading * 10.0f);
    roll10 = (uint16_t)(int16_t)(imuRoll * 10.0f);
  } else {
    head10 = 9999;
    roll10 = 8888;
  }
  pgn253[7]  = (uint8_t)(head10 & 0xFF);
  pgn253[8]  = (uint8_t)(head10 >> 8);
  pgn253[9]  = (uint8_t)(roll10 & 0xFF);
  pgn253[10] = (uint8_t)(roll10 >> 8);

  pgn253[11] = switchByte;
  int16_t pd = pwmCommand < 0 ? -pwmCommand : pwmCommand;
  pgn253[12] = (uint8_t)(pd > 255 ? 255 : pd);

  uint8_t crc = 0;
  for (uint8_t i = 2; i <= 12; i++) crc += pgn253[i];
  pgn253[13] = crc;

  if (apMode || WiFi.status() != WL_CONNECTED) return;
  udp.beginPacket(aogBroadcast, AOG_PORT_SEND);
  udp.write(pgn253, sizeof(pgn253));
  udp.endPacket();
}

// ---------------- AOG -> module ----------------
void handleAogUdp() {
  int sz = udp.parsePacket();
  if (sz < 5) return;
  uint8_t d[64];
  int n = udp.read(d, sizeof(d));
  if (n < 5 || d[0] != 0x80 || d[1] != 0x81 || d[2] != 0x7F) return;
  lastAogMs = millis();

  switch (d[3]) {
    case 0xFE:
      lastPgn254Ms   = millis();
      aogSpeed       = ((d[6] << 8) | d[5]) * 0.1f;
      guidanceStatus = d[7];
      steerSetpoint  = (int16_t)((d[9] << 8) | d[8]) * 0.01f;
      break;
    case 0xC8:  sendHello();        break;
    case 0xCA:  handleScanReq(d);   break;
    case 0xC9:  handleSubnet(d);    break;
    case 0xFC:  applySteerSettings(d); break;
    case 0xFB:  applySteerConfig(d);   break;
  }
}

void sendHello() {
  if (apMode || WiFi.status() != WL_CONNECTED) return;
  int16_t sa  = (int16_t)(steerAngle * 100.0f);
  int16_t pos = wasRaw;
  uint8_t h[] = { 0x80, 0x81, 0x7E, 0x7E, 5,
                  (uint8_t)sa, (uint8_t)(sa >> 8),
                  (uint8_t)pos, (uint8_t)(pos >> 8),
                  switchByte, 71 };
  udp.beginPacket(aogBroadcast, AOG_PORT_SEND);
  udp.write(h, sizeof(h));
  udp.endPacket();
}

void handleScanReq(const uint8_t *d) {
  if (!(d[4] == 3 && d[5] == 202 && d[6] == 202)) return;
  uint8_t r[] = { 0x80, 0x81, 0x7E, 0xCB, 7,
                  cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3],
                  cfg.ip[0], cfg.ip[1], cfg.ip[2], 0 };
  uint8_t crc = 0;
  for (uint8_t i = 2; i < sizeof(r) - 1; i++) crc += r[i];
  r[sizeof(r) - 1] = crc;
  IPAddress bcast(255, 255, 255, 255);
  udp.beginPacket(bcast, AOG_PORT_SEND);
  udp.write(r, sizeof(r));
  udp.endPacket();
}

void handleSubnet(const uint8_t *d) {
  if (!(d[4] == 5 && d[5] == 201 && d[6] == 201)) return;
  cfg.ip[0] = d[7]; cfg.ip[1] = d[8]; cfg.ip[2] = d[9];
  cfg.cabinIp[0] = d[7]; cfg.cabinIp[1] = d[8]; cfg.cabinIp[2] = d[9];
  doSaveConfig();                      // write now: loop() never runs again before the restart
  delay(100);
  ESP.restart();
}

void applySteerSettings(const uint8_t *d) {
  Serial.printf("AOG: PGN252 received (ignored — steer settings owned locally via /setup)\n");
}

void applySteerConfig(const uint8_t *d) {
  Serial.printf("AOG: PGN251 received (ignored — steer config owned locally via /setup)\n");
}

// ---------------- WiFi ----------------
void startWifi() {
  aogBroadcast = IPAddress(cfg.ip[0], cfg.ip[1], cfg.ip[2], 255);

  if (strlen(cfg.ssid) == 0) { startConfigAp(); return; }

  WiFi.mode(WIFI_STA);
  if (!cfg.useDhcp) {
    IPAddress local(cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3]);
    IPAddress gw(cfg.ip[0], cfg.ip[1], cfg.ip[2], 1);
    IPAddress sn(255, 255, 255, 0);
    WiFi.config(local, gw, sn);
  }
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid, cfg.pass);
  Serial.printf("WiFi -> %s", cfg.ssid);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    delay(300); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    Serial.printf("\nconnected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nSTA failed -> config AP");
    startConfigAp();
  }
}

void startConfigAp() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("AOG-FrontNode", "12345678");
  Serial.printf("Config AP: AOG-FrontNode / 12345678  http://%s\n",
                WiFi.softAPIP().toString().c_str());
}
