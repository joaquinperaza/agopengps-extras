/*
  AOG Traffic Logger  (ESP32-C3 Super Mini)
  -----------------------------------------
  Passive recorder of the tractor's AgOpenGPS UDP traffic into a rolling log on the C3's flash,
  with a WebUI (webpage.h) that charts it live: the browser downloads the raw log and decodes it,
  so the C3 only serves bytes. Downloads decode offline with tools/tlog_decode.py.

  It NEVER transmits on the AOG ports: it does not answer AgIO hellos or scans, so AgIO does not
  see it as a module. It only listens.

  What a WiFi station can hear is broadcast traffic plus traffic addressed to itself:
    8888  AgIO -> modules   PGN 254 steer data, 252/251, 239/238/229 machine + sections, hello,
                            NMEA forwarded by AgIO                       (broadcast x.x.x.255)
    9999  modules -> AgIO   PGN 253 from the steer module, GPS NMEA, machine/IMU replies
    2233  AgIO -> GPS       RTCM corrections: counted only (large, not useful for tuning)
    4242  hydraulic remote -> relay box
    3344/4433 front node <-> cabin: normally unicast and WiFi-encrypted, so NOT visible
              (only recorded if they are ever sent as broadcast)
  Kept: every AOG binary packet (80 81 ...), every non-NMEA packet, and NMEA per the setting
  (default: GGA/RMC/VTG/HDT/PANDA/PAOGI at 1 per second each).

  WiFi:
    - fresh flash (never configured): open hotspot "AOG-Logger-Setup" (no password, captive
      portal at 192.168.4.1) right away, while also trying the default network in the background
    - configured: keep trying the saved network; only after 10 min without it, open the hotspot,
      and keep retrying the network. The hotspot closes as soon as the network is joined.

  Board: ESP32-C3 Super Mini (4 MB flash). Build with the sketch's partitions.csv, USB CDC on:
    FQBN esp32:esp32:esp32c3:CDCOnBoot=cdc
  Default address 192.168.1.50 (WebUI http://192.168.1.50/). Log format: tlog.h.
*/

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <vector>
#include "tlog.h"
#include "webpage.h"

const char *FW = "aog_logger-1.1";

// ---------------- ports ----------------
const uint8_t  NPORTS = 6;
const uint16_t PORTS[NPORTS]     = { 8888, 9999, 2233, 4242, 3344, 4433 };   // index = log channel
const bool     KEEP_PORT[NPORTS] = { true, true, false, true, true, true };  // 2233 RTCM: count only
const char    *PORT_LIST = "8888,9999,2233,4242,3344,4433";

const uint8_t LED_PIN = 8;                 // Super Mini onboard LED, active LOW

// ---------------- settings (NVS) ----------------
struct Settings {
  uint32_t magic;
  char     ssid[33];
  char     pass[64];
  uint8_t  ip[4];
  bool     dhcp;
  uint8_t  nmea;                           // 0 off, 1 key sentences at 1 Hz, 2 all
  bool     configured;                     // WiFi saved by the user at least once
};
const uint32_t SET_MAGIC = 0xA0610002UL;
Settings set;
Preferences prefs;
uint32_t bootCount = 0, clearFloor = 0;    // sectors with seq < clearFloor are hidden ("cleared")

void setDefaults() {
  memset(&set, 0, sizeof(set));
  set.magic = SET_MAGIC;
  strlcpy(set.ssid, "AGOPENGPS", sizeof(set.ssid));
  strlcpy(set.pass, "agopengps", sizeof(set.pass));
  set.ip[0] = 192; set.ip[1] = 168; set.ip[2] = 1; set.ip[3] = 50;
  set.dhcp = false;
  set.nmea = 1;
  set.configured = false;
}

void loadSettings() {
  prefs.begin("aoglog", false);
  if (prefs.getBytesLength("set") == sizeof(Settings)) {
    prefs.getBytes("set", &set, sizeof(set));
    if (set.magic != SET_MAGIC) setDefaults();
  } else {
    setDefaults();
  }
  bootCount  = prefs.getUInt("boot", 0) + 1;
  prefs.putUInt("boot", bootCount);
  clearFloor = prefs.getUInt("floor", 0);
}

void saveSettings() {
  set.magic = SET_MAGIC;
  prefs.putBytes("set", &set, sizeof(set));
}

// ---------------- log on flash ----------------
const esp_partition_t *logPart = nullptr;
tlog::Ring ring;
bool logOk = false;
uint32_t flashErrors = 0;

bool pRead(uint32_t o, void *d, uint32_t n)        { return esp_partition_read(logPart, o, d, n) == ESP_OK; }
bool pWrite(uint32_t o, const void *s, uint32_t n) { return esp_partition_write(logPart, o, s, n) == ESP_OK; }
bool pErase(uint32_t o)                            { return esp_partition_erase_range(logPart, o, tlog::SECTOR) == ESP_OK; }

void logText(uint8_t chan, const char *txt) {
  if (!logOk) return;
  if (!ring.append(millis(), chan, 0, (const uint8_t *)txt, strlen(txt))) flashErrors++;
}

// ---------------- live counters (shown in the WebUI) ----------------
WiFiUDP udp[NPORTS];

struct Source {                            // one sender (ip, port)
  uint8_t  ip[4];
  uint16_t port;
  uint32_t pkts, kept, lastMs, mark10s;
  uint16_t rate10s;                        // packets in the last 10 s
  char     what[28];
};
const uint8_t MAX_SRC = 24;
Source srcs[MAX_SRC];
uint8_t nSrc = 0;

struct PgnCount {                          // AOG binary packets per (source byte, PGN) since boot
  uint8_t  src, pgn;
  uint32_t n, mark10s;
  uint16_t rate10s;
};
const uint8_t MAX_PGN = 40;
PgnCount pgns[MAX_PGN];
uint8_t nPgn = 0;

uint16_t seenMin[NPORTS], keptMin[NPORTS];
uint32_t bytesMin[NPORTS];
uint32_t keptBytesMin = 0, keptBytesPrevMin = 0;

#pragma pack(push, 1)
struct StatsV1 {                           // CH_STATS payload, decoded by tools/tlog_decode.py
  uint8_t  ver;                            // 1
  int8_t   rssi;
  uint16_t heapKB;
  uint32_t uptimeS;
  uint8_t  nports;
  uint16_t seen[NPORTS];
  uint16_t kept[NPORTS];
  uint32_t bytes[NPORTS];
  uint16_t flashErrors;
};
#pragma pack(pop)

const char *aogSrcName(uint8_t s) {
  switch (s) {
    case 0x7F: return "AgIO";
    case 0x7E: return "steer";
    case 0x7B: return "machine";
    case 0x7C: return "GPS";
    case 0x79: return "IMU";
    case 0x7A: return "front->cabin";
    default:   return "?";
  }
}

void describe(char *out, size_t n, uint8_t portIdx, const uint8_t *p, int len) {
  if (len >= 5 && p[0] == 0x80 && p[1] == 0x81) snprintf(out, n, "PGN %u (%s)", p[3], aogSrcName(p[2]));
  else if (len >= 6 && p[0] == '$')             snprintf(out, n, "NMEA %.5s", (const char *)p + 1);
  else if (PORTS[portIdx] == 2233)              snprintf(out, n, "RTCM");
  else if (PORTS[portIdx] == 4242 && len >= 3 && p[0] == 0x3A && p[1] == 0x3A) snprintf(out, n, "hydraulic cmd %u", p[2]);
  else                                          snprintf(out, n, "raw %d B", len);
}

// Index into srcs[] for (ip, port), adding it if new; -1 when the table is full.
// (Returns an index, not Source*: Arduino's generated prototypes would precede struct Source.)
int sourceIndex(IPAddress ip, uint16_t port) {
  for (uint8_t i = 0; i < nSrc; i++)
    if (srcs[i].port == port && srcs[i].ip[0] == ip[0] && srcs[i].ip[1] == ip[1] &&
        srcs[i].ip[2] == ip[2] && srcs[i].ip[3] == ip[3]) return i;
  if (nSrc >= MAX_SRC) return -1;
  Source &s = srcs[nSrc];
  memset(&s, 0, sizeof(s));
  for (uint8_t k = 0; k < 4; k++) s.ip[k] = ip[k];
  s.port = port;
  return nSrc++;
}

void countPgn(uint8_t src, uint8_t pgn) {
  for (uint8_t i = 0; i < nPgn; i++)
    if (pgns[i].src == src && pgns[i].pgn == pgn) { pgns[i].n++; return; }
  if (nPgn >= MAX_PGN) return;
  pgns[nPgn] = { src, pgn, 1, 0, 0 };
  nPgn++;
}

// NMEA mode 1: keep these sentence types, at most once per second each.
bool keepNmea(const uint8_t *p, int len, uint32_t now) {
  if (set.nmea == 0) return false;
  if (set.nmea == 2) return true;
  if (len < 6) return false;
  static const char *KEYS[] = { "GGA", "RMC", "VTG", "HDT", "NDA", "OGI" };   // ..NDA = PANDA, ..OGI = PAOGI
  static uint32_t last[6] = { 0 };
  for (uint8_t k = 0; k < 6; k++) {
    if (memcmp(p + 3, KEYS[k], 3) == 0) {
      if (last[k] != 0 && now - last[k] < 1000) return false;
      last[k] = now;
      return true;
    }
  }
  return false;
}

uint32_t ledOffAt = 0;

void handlePacket(uint8_t i, IPAddress from, const uint8_t *p, int len) {
  uint32_t now = millis();
  seenMin[i]++; bytesMin[i] += len;
  int si = sourceIndex(from, PORTS[i]);
  if (si >= 0) { srcs[si].pkts++; srcs[si].lastMs = now; describe(srcs[si].what, sizeof(srcs[si].what), i, p, len); }
  if (len >= 5 && p[0] == 0x80 && p[1] == 0x81) countPgn(p[2], p[3]);

  bool keep = KEEP_PORT[i];
  if (keep && len > 0 && p[0] == '$') keep = keepNmea(p, len, now);
  if (!keep || !logOk) return;
  if (!ring.append(now, i, from[3], p, len > 0xFFFF ? 0xFFFF : len)) flashErrors++;
  keptMin[i]++;
  keptBytesMin += tlog::REC_HDR + (len > tlog::MAX_PAYLOAD ? tlog::MAX_PAYLOAD : len);
  if (si >= 0) srcs[si].kept++;
  digitalWrite(LED_PIN, LOW);              // blink per kept packet
  ledOffAt = now + 15;
}

void capture() {
  static uint8_t buf[1600];
  for (uint8_t i = 0; i < NPORTS; i++) {
    for (uint8_t k = 0; k < 8; k++) {      // round-robin: at most 8 packets per port per pass
      int n = udp[i].parsePacket();
      if (n <= 0) break;
      int r = udp[i].read(buf, sizeof(buf));
      if (r < 0) continue;
      handlePacket(i, udp[i].remoteIP(), buf, r);
    }
  }
}

void writeStats() {
  StatsV1 s;
  memset(&s, 0, sizeof(s));
  s.ver = 1;
  s.rssi = WiFi.status() == WL_CONNECTED ? (int8_t)WiFi.RSSI() : 0;
  s.heapKB = ESP.getFreeHeap() / 1024;
  s.uptimeS = millis() / 1000;
  s.nports = NPORTS;
  for (uint8_t i = 0; i < NPORTS; i++) { s.seen[i] = seenMin[i]; s.kept[i] = keptMin[i]; s.bytes[i] = bytesMin[i]; }
  s.flashErrors = flashErrors > 0xFFFF ? 0xFFFF : flashErrors;
  if (logOk && !ring.append(millis(), tlog::CH_STATS, 0, (const uint8_t *)&s, sizeof(s))) flashErrors++;
  memset(seenMin, 0, sizeof(seenMin)); memset(keptMin, 0, sizeof(keptMin)); memset(bytesMin, 0, sizeof(bytesMin));
  keptBytesPrevMin = keptBytesMin; keptBytesMin = 0;
}

// ---------------- WiFi ----------------
const char    *AP_SSID       = "AOG-Logger-Setup";   // open hotspot (no password), on purpose
const uint32_t AP_AFTER_MS   = 10UL * 60 * 1000;     // saved network missing this long -> hotspot
const uint32_t RETRY_MS      = 30000;                // retry the saved network
const uint32_t RETRY_BUSY_MS = 120000;               // ...slower while a phone uses the hotspot
                                                     // (each retry scans channels: hotspot blips)
WebServer server(80);
DNSServer dns;
bool apOn = false, wasUp = false;
uint32_t staDownSince = 0, lastRetryMs = 0;
volatile bool reqReboot = false;

void configSta() {
  if (set.dhcp) return;
  WiFi.config(IPAddress(set.ip[0], set.ip[1], set.ip[2], set.ip[3]),
              IPAddress(set.ip[0], set.ip[1], set.ip[2], 1), IPAddress(255, 255, 255, 0),
              IPAddress(set.ip[0], set.ip[1], set.ip[2], 1));
}

void apStart() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);                    // no password
  dns.start(53, "*", WiFi.softAPIP());     // captive portal: every name -> the logger
  apOn = true;
  logText(tlog::CH_BOOT, "hotspot=on");
  Serial.printf("hotspot %s open at http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void apStop() {
  dns.stop();
  WiFi.softAPdisconnect(true);             // back to plain station mode
  apOn = false;
  logText(tlog::CH_BOOT, "hotspot=off");
  Serial.println("hotspot closed");
}

void startWifi() {
  WiFi.mode(WIFI_STA);
  configSta();
  WiFi.setSleep(false);                    // no power save: receive broadcasts promptly
  WiFi.setAutoReconnect(true);
  if (strlen(set.ssid)) WiFi.begin(set.ssid, set.pass);
  staDownSince = lastRetryMs = millis();
  if (!set.configured) apStart();          // fresh flash: hotspot right away for setup
}

void serviceWifi() {
  uint32_t now = millis();
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wasUp) {                       // record WiFi up/down in the log
    char t[96];
    if (up) snprintf(t, sizeof(t), "wifi=up ip=%s rssi=%d", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    else    snprintf(t, sizeof(t), "wifi=down");
    logText(tlog::CH_BOOT, t);
    if (up) MDNS.begin("aoglogger");
    Serial.println(t);
    wasUp = up;
  }
  if (up) {
    staDownSince = now;
    if (apOn) apStop();
    return;
  }
  if (!apOn && (!set.configured || now - staDownSince >= AP_AFTER_MS)) apStart();
  uint32_t every = (apOn && WiFi.softAPgetStationNum() > 0) ? RETRY_BUSY_MS : RETRY_MS;
  if (strlen(set.ssid) && now - lastRetryMs >= every) {
    lastRetryMs = now;
    if (apOn) WiFi.begin(set.ssid, set.pass); else WiFi.reconnect();
  }
}

// ---------------- requests from the web task (executed by loop) ----------------
volatile bool reqFlush = false, reqClear = false, reqMark = false;
char markText[200];

static bool waitDone(volatile bool &flag, uint32_t ms) {
  uint32_t t0 = millis();
  while (flag && millis() - t0 < ms) vTaskDelay(5);
  return !flag;
}

// ---------------- WebUI handlers ----------------
void sendJson(const String &s) { server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", s); }

void jsonEsc(String &j, const char *s) {   // append a JSON string literal
  j += '"';
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') { j += '\\'; j += *s; }
    else if ((uint8_t)*s < 0x20) j += ' ';
    else j += *s;
  }
  j += '"';
}

void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }

void handleStatus() {
  uint32_t now = millis();
  uint32_t total = logOk ? ring.sectors() : 0, used = 0;
  if (logOk) {
    uint32_t sinceClear = ring.newestSeq() >= clearFloor ? ring.newestSeq() - clearFloor + 1 : 0;
    used = min(ring.usedSectors(), sinceClear);
  }
  float rate = (keptBytesPrevMin ? keptBytesPrevMin : keptBytesMin) / 60.0f;
  float cov = rate > 1 ? total * (float)tlog::SECTOR / rate / 60.0f : 0;
  bool up = WiFi.status() == WL_CONNECTED;
  String j;
  j.reserve(3072);
  char b[220];
  snprintf(b, sizeof(b), "{\"fw\":\"%s\",\"boot\":%lu,\"now\":%lu,\"up\":%lu,\"rssi\":%d,\"wifi\":{\"up\":%d,\"ssid\":",
           FW, (unsigned long)bootCount, (unsigned long)now, (unsigned long)(now / 1000), up ? (int)WiFi.RSSI() : 0, up ? 1 : 0);
  j += b; jsonEsc(j, set.ssid);
  snprintf(b, sizeof(b), ",\"ip\":\"%s\",\"rssi\":%d,\"ap\":%d,\"apClients\":%d,\"downFor\":%lu,\"configured\":%d},",
           up ? WiFi.localIP().toString().c_str() : "0.0.0.0", up ? (int)WiFi.RSSI() : 0, apOn ? 1 : 0,
           apOn ? WiFi.softAPgetStationNum() : 0, up ? 0UL : (unsigned long)((now - staDownSince) / 1000), set.configured ? 1 : 0);
  j += b;
  snprintf(b, sizeof(b), "\"log\":{\"used\":%lu,\"total\":%lu,\"rate\":%.0f,\"cov\":%.1f,\"ferr\":%lu,\"seq\":%lu},\"src\":[",
           (unsigned long)used, (unsigned long)total, rate, cov, (unsigned long)flashErrors, logOk ? (unsigned long)ring.newestSeq() : 0UL);
  j += b;
  for (uint8_t i = 0; i < nSrc; i++) {
    Source &s = srcs[i];
    snprintf(b, sizeof(b), "%s{\"ip\":\"%u.%u.%u.%u\",\"port\":%u,\"what\":", i ? "," : "", s.ip[0], s.ip[1], s.ip[2], s.ip[3], s.port);
    j += b; jsonEsc(j, s.what);
    snprintf(b, sizeof(b), ",\"r\":%u,\"n\":%lu,\"kept\":%lu,\"age\":%.1f}", s.rate10s, (unsigned long)s.pkts,
             (unsigned long)s.kept, (now - s.lastMs) / 1000.0f);
    j += b;
  }
  j += "],\"pgns\":[";
  for (uint8_t i = 0; i < nPgn; i++) {
    snprintf(b, sizeof(b), "%s{\"src\":%u,\"pgn\":%u,\"n\":%lu,\"r\":%u}", i ? "," : "", pgns[i].src, pgns[i].pgn,
             (unsigned long)pgns[i].n, pgns[i].rate10s);
    j += b;
  }
  j += "]}";
  sendJson(j);
}

void handleCfg() {
  String j = "{\"ssid\":";
  jsonEsc(j, set.ssid);
  char b[120];
  snprintf(b, sizeof(b), ",\"ip\":\"%u.%u.%u.%u\",\"dhcp\":%d,\"nmea\":%u,\"configured\":%d}",
           set.ip[0], set.ip[1], set.ip[2], set.ip[3], set.dhcp ? 1 : 0, set.nmea, set.configured ? 1 : 0);
  j += b;
  sendJson(j);
}

// POST form: any of ssid, pass (blank = unchanged), open=1 (no password), dhcp, ip, nmea, reboot=1
void handleSaveCfg() {
  if (server.hasArg("nmea")) { long v = server.arg("nmea").toInt(); if (v >= 0 && v <= 2) set.nmea = v; }
  if (server.hasArg("ssid") && server.arg("ssid").length()) {
    strlcpy(set.ssid, server.arg("ssid").c_str(), sizeof(set.ssid));
    set.configured = true;
  }
  if (server.arg("open") == "1") set.pass[0] = 0;
  else if (server.hasArg("pass") && server.arg("pass").length()) strlcpy(set.pass, server.arg("pass").c_str(), sizeof(set.pass));
  if (server.hasArg("dhcp")) set.dhcp = server.arg("dhcp") == "1";
  if (server.hasArg("ip")) { IPAddress a; if (a.fromString(server.arg("ip"))) for (uint8_t i = 0; i < 4; i++) set.ip[i] = a[i]; }
  saveSettings();
  server.send(200, "text/plain", "OK");
  if (server.arg("reboot") == "1") reqReboot = true;
}

void handleScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) { WiFi.scanNetworks(true); sendJson("{\"scanning\":1}"); return; }
  if (n == WIFI_SCAN_RUNNING) { sendJson("{\"scanning\":1}"); return; }
  String j = "{\"nets\":[";
  for (int i = 0; i < n; i++) {
    if (i) j += ',';
    j += "{\"ssid\":"; jsonEsc(j, WiFi.SSID(i).c_str());
    j += ",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 1 : 0) + "}";
  }
  j += "]}";
  WiFi.scanDelete();                       // next call starts a fresh scan
  sendJson(j);
}

void handleMark() {
  if (!server.hasArg("text") || reqMark) { server.send(400, "text/plain", "no text or busy"); return; }
  strlcpy(markText, server.arg("text").c_str(), sizeof(markText));
  reqMark = true;
  waitDone(reqMark, 1000);
  server.send(200, "text/plain", "OK");
}

void handleClear() {
  if (server.arg("confirm") != "yes") { server.send(400, "text/plain", "confirm=yes required"); return; }
  reqClear = true;
  waitDone(reqClear, 3000);
  server.send(200, "text/plain", "cleared");
}

// /log.bin                 everything since the last clear
// /log.bin?minutes=N       current boot, the last N minutes
// /log.bin?fromseq=S       sectors with seq >= S (the WebUI's live refresh; includes the open sector)
void handleLog() {
  if (!logOk) { server.send(500, "text/plain", "no log partition"); return; }
  reqFlush = true;
  waitDone(reqFlush, 1000);                // current sector on flash before reading it
  uint32_t nsec = ring.sectors(), startSeq = clearFloor;
  uint32_t minutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : 0;
  if (server.hasArg("fromseq")) { uint32_t s = server.arg("fromseq").toInt(); if (s > startSeq) startSeq = s; }
  if (minutes) {                           // from the sector that covers the cutoff
    uint32_t now = millis(), cutoff = now > minutes * 60000UL ? now - minutes * 60000UL : 0;
    uint32_t firstAfter = UINT32_MAX;
    for (uint32_t i = 0; i < nsec; i++) {
      tlog::SectorHdr h;
      if (pRead(i * tlog::SECTOR, &h, sizeof(h)) && h.magic == tlog::MAGIC && h.boot == bootCount &&
          h.t0 >= cutoff && h.seq < firstAfter) firstAfter = h.seq;
    }
    uint32_t s = firstAfter == UINT32_MAX ? ring.newestSeq() : (firstAfter > 0 ? firstAfter - 1 : 0);
    if (s > startSeq) startSeq = s;
  }
  std::vector<uint16_t> list;
  for (uint32_t i = 0; i < nsec; i++) {
    tlog::SectorHdr h;
    if (pRead(i * tlog::SECTOR, &h, sizeof(h)) && h.magic == tlog::MAGIC && h.seq != 0xFFFFFFFFu &&
        h.seq >= startSeq && (!minutes || h.boot == bootCount)) list.push_back(i);
  }
  char fn[96];
  snprintf(fn, sizeof(fn), "attachment; filename=\"aog_log_b%lu_%lus%s.bin\"", (unsigned long)bootCount,
           (unsigned long)(millis() / 1000), minutes ? "_recent" : "");
  server.sendHeader("Content-Disposition", fn);
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(list.size() * tlog::SECTOR);
  server.send(200, "application/octet-stream", "");
  uint8_t *b = (uint8_t *)malloc(tlog::SECTOR);
  if (!b) return;
  WiFiClient c = server.client();
  for (uint16_t idx : list) {              // physical order; decoders sort by seq
    if (!c.connected()) break;
    if (!pRead(idx * tlog::SECTOR, b, tlog::SECTOR)) memset(b, 0xFF, tlog::SECTOR);
    c.write(b, tlog::SECTOR);
  }
  free(b);
}

void handleReboot() { server.send(200, "text/plain", "Rebooting"); reqReboot = true; }

// Captive portal: phones probe these URLs when joining the hotspot; send them to the WebUI.
void captiveRedirect() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
  server.send(302, "text/plain", "");
}

void handleNotFound() {
  String host = server.hostHeader();
  if (apOn && host != WiFi.softAPIP().toString() && host != WiFi.localIP().toString() && host != "aoglogger.local") {
    captiveRedirect();
    return;
  }
  server.send(404, "text/plain", "Not found");
}

void webTask(void *) {
  for (;;) { server.handleClient(); vTaskDelay(2); }
}

void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/cfg", HTTP_GET, handleCfg);
  server.on("/api/savecfg", HTTP_POST, handleSaveCfg);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/mark", HTTP_POST, handleMark);
  server.on("/api/clear", HTTP_POST, handleClear);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/log.bin", HTTP_GET, handleLog);
  for (const char *u : { "/generate_204", "/gen_204", "/hotspot-detect.html", "/connecttest.txt", "/ncsi.txt", "/fwlink" })
    server.on(u, HTTP_GET, captiveRedirect);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ============================================================
void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n%s\n", FW);
  loadSettings();

  logPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "tlog");
  if (logPart) {
    tlog::Flash f = { pRead, pWrite, pErase, logPart->size };
    logOk = ring.begin(f, bootCount, millis());
    Serial.printf("log: %lu sectors, newest seq %lu, %s\n", (unsigned long)ring.sectors(),
                  (unsigned long)ring.newestSeq(), logOk ? "ok" : "FLASH ERROR");
  } else {
    Serial.println("log: NO 'tlog' PARTITION (build with partitions.csv)");
  }

  char t[220];
  snprintf(t, sizeof(t), "fw=%s boot=%lu ip=%u.%u.%u.%u dhcp=%d ssid=%s configured=%d nmea=%u reset=%d ports=%s",
           FW, (unsigned long)bootCount, set.ip[0], set.ip[1], set.ip[2], set.ip[3], set.dhcp ? 1 : 0,
           set.ssid, set.configured ? 1 : 0, set.nmea, (int)esp_reset_reason(), PORT_LIST);
  logText(tlog::CH_BOOT, t);

  startWifi();
  for (uint8_t i = 0; i < NPORTS; i++) udp[i].begin(PORTS[i]);   // receive only, never sent on
  setupWeb();
  xTaskCreate(webTask, "web", 6144, NULL, 1, NULL);
}

void loop() {
  uint32_t now = millis();
  capture();
  if (apOn) dns.processNextRequest();

  if (reqMark)  { logText(tlog::CH_MARK, markText); reqMark = false; }
  if (reqFlush) { if (logOk && !ring.flush()) flashErrors++; reqFlush = false; }
  if (reqClear) {
    if (logOk) {
      clearFloor = ring.newestSeq() + 1;
      prefs.putUInt("floor", clearFloor);
      if (!ring.openSector(now)) flashErrors++;
      logText(tlog::CH_BOOT, "log cleared");
    }
    reqClear = false;
  }
  if (reqReboot) { if (logOk) ring.flush(); delay(500); ESP.restart(); }

  static uint32_t lastFlush = 0, lastStats = 0, last10s = 0, lastWifi = 0, lastBlink = 0;
  if (now - lastFlush >= 1000) { lastFlush = now; if (logOk && !ring.flush()) flashErrors++; }   // <= 1 s lost on power cut
  if (now - lastStats >= 60000) { lastStats = now; writeStats(); }
  if (now - last10s >= 10000) {
    last10s = now;
    for (uint8_t i = 0; i < nSrc; i++) { srcs[i].rate10s = srcs[i].pkts - srcs[i].mark10s; srcs[i].mark10s = srcs[i].pkts; }
    for (uint8_t i = 0; i < nPgn; i++) { pgns[i].rate10s = pgns[i].n - pgns[i].mark10s; pgns[i].mark10s = pgns[i].n; }
  }
  if (now - lastWifi >= 500) { lastWifi = now; serviceWifi(); }

  if (ledOffAt && (int32_t)(now - ledOffAt) >= 0) { digitalWrite(LED_PIN, HIGH); ledOffAt = 0; }
  if (apOn && now - lastBlink >= 500) { lastBlink = now; digitalWrite(LED_PIN, !digitalRead(LED_PIN)); }
  vTaskDelay(1);
}
