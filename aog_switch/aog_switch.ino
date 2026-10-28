/*
  AOG Switch  (ESP32-C3 Super Mini)
  ---------------------------------
  Work switch for the planter: a contact between a GPIO and GND tells AgOpenGPS whether the
  machine is working. Closed (GPIO pulled to GND) = working -> AOG section control master goes to
  AUTO (paints / drives sections). Open = stopped -> master goes OFF (stops painting).

  How: PGN 234 "remote switches" (the AOG section-control switch box protocol), broadcast to
  AgIO on x.x.x.255:9999; AgIO forwards it to AgOpenGPS.
    80 81 7B EA 08 | main | autoGr0 autoGr1 numSections onGr0 offGr0 onGr1 offGr1 | crc
    main bit0 = ON, bit1 = OFF, bit2 = "switch hardware" (AOG keeps the master in that state)
    All section bytes are 0 = individual sections are left alone.
  AgOpenGPS only acts on it while a field (job) is open. It is resent every second, so the state
  is applied as soon as a field is opened. This never touches steering (unlike PGN 253).

  Mode "follow changes" clears bit2: AOG then only reacts when the switch changes, and the
  operator can override the master button in between.

  WiFi (same as the logger):
    - fresh flash: open hotspot "AOG-Switch" (no password, page at http://192.168.4.1) right away,
      while also trying AGOPENGPS / agopengps in the background
    - configured: keep trying the saved network; after 10 min without it, open the hotspot too,
      and keep retrying. The hotspot closes as soon as the network is joined.
  All settings are stored in flash (NVS) and survive reboots and power cuts.

  Wiring: switch between the chosen GPIO (0-4, default 3) and GND; the internal pull-up is used.
  For long cables on the planter add an external 4.7k pull-up to 3V3 and 100 nF to GND.
  GPIO2 is a boot strapping pin on the C3: if the board will not start while the switch is
  closed, use another pin.

  Board: ESP32-C3 Super Mini, FQBN esp32:esp32:esp32c3:CDCOnBoot=cdc
  Onboard LED (GPIO8, active LOW): on = machine working, slow blink = setup hotspot open.
*/

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

const char *FW = "aog_switch-1.0";
const uint8_t  LED_PIN = 8;                // active LOW
const uint16_t AGIO_PORT = 9999;           // modules -> AgIO
const uint8_t  SRC_BYTE = 0x7B;            // AOG source byte used by machine / switch boxes
const uint8_t  PGN_SWITCHES = 234;
const uint32_t RESEND_MS = 1000;           // keep AOG in sync (and apply once a field opens)
const uint32_t DEBOUNCE_MS = 50;

// ---------------- settings (NVS, survive reboots) ----------------
struct Settings {
  uint32_t magic;
  char     ssid[33];
  char     pass[64];
  uint8_t  ip[4];
  bool     dhcp;
  uint8_t  gpio;                           // 0..4
  bool     invert;                         // true: open contact = working
  bool     followChanges;                  // true: button mode (bit2 = 0)
  bool     configured;                     // WiFi saved by the user at least once
};
const uint32_t SET_MAGIC = 0xA05A0001UL;
Settings set;
Preferences prefs;

void setDefaults() {
  memset(&set, 0, sizeof(set));
  set.magic = SET_MAGIC;
  strlcpy(set.ssid, "AGOPENGPS", sizeof(set.ssid));
  strlcpy(set.pass, "agopengps", sizeof(set.pass));
  set.ip[0] = 192; set.ip[1] = 168; set.ip[2] = 1; set.ip[3] = 60;
  set.dhcp = false;
  set.gpio = 3;
  set.invert = false;
  set.followChanges = false;
  set.configured = false;
}

void loadSettings() {
  prefs.begin("aogswitch", false);
  if (prefs.getBytesLength("set") == sizeof(Settings)) {
    prefs.getBytes("set", &set, sizeof(set));
    if (set.magic != SET_MAGIC || set.gpio > 4) setDefaults();
  } else {
    setDefaults();
  }
}

void saveSettings() {
  set.magic = SET_MAGIC;
  prefs.putBytes("set", &set, sizeof(set));
}

// ---------------- switch input ----------------
bool rawLevel = true, stableLevel = true;  // HIGH = open (pull-up)
uint32_t rawChangedMs = 0;
bool working = false;                      // debounced machine state
uint32_t changes = 0, lastChangeMs = 0;
int8_t testOverride = -1;                  // -1 none, 0 off, 1 on (from the WebUI test buttons)
uint32_t testUntilMs = 0;

void setupInput() {
  for (uint8_t p = 0; p <= 4; p++) if (p != set.gpio) pinMode(p, INPUT);   // release unused pins
  pinMode(set.gpio, INPUT_PULLUP);
  delay(5);
  rawLevel = stableLevel = digitalRead(set.gpio);
  rawChangedMs = millis();
  working = set.invert ? stableLevel : !stableLevel;
}

void readInput() {
  bool lvl = digitalRead(set.gpio);
  uint32_t now = millis();
  if (lvl != rawLevel) { rawLevel = lvl; rawChangedMs = now; }
  if (lvl != stableLevel && now - rawChangedMs >= DEBOUNCE_MS) stableLevel = lvl;
  bool w = set.invert ? stableLevel : !stableLevel;          // closed (LOW) = working
  if (w != working) { working = w; changes++; lastChangeMs = now; }
}

bool machineOn() {
  if (testOverride >= 0 && (int32_t)(millis() - testUntilMs) < 0) return testOverride == 1;
  testOverride = -1;
  return working;
}

// ---------------- PGN 234 ----------------
WiFiUDP udp;
uint32_t sent = 0, lastSendMs = 0;
bool lastSentOn = false;
uint8_t burst = 0;                          // extra quick resends after a change

void sendSwitches(bool on) {
  if (WiFi.status() != WL_CONNECTED) return;
  uint8_t main = on ? 0x01 : 0x02;
  if (!set.followChanges) main |= 0x04;     // "switch hardware": AOG holds the master in this state
  uint8_t p[14] = { 0x80, 0x81, SRC_BYTE, PGN_SWITCHES, 8, main, 0, 0, 0, 0, 0, 0, 0, 0 };
  uint8_t crc = 0;
  for (uint8_t i = 2; i < 13; i++) crc += p[i];
  p[13] = crc;
  udp.beginPacket(WiFi.broadcastIP(), AGIO_PORT);
  udp.write(p, sizeof(p));
  udp.endPacket();
  sent++;
  lastSentOn = on;
}

void serviceSend() {
  uint32_t now = millis();
  bool on = machineOn();
  if (on != lastSentOn) burst = 3;          // a change: send now and twice more, 100 ms apart
  uint32_t every = burst ? 100 : RESEND_MS;
  if (now - lastSendMs >= every || on != lastSentOn) {
    lastSendMs = now;
    sendSwitches(on);
    if (burst) burst--;
  }
}

// ---------------- WiFi ----------------
const char    *AP_SSID       = "AOG-Switch";       // open hotspot (no password), on purpose
const uint32_t AP_AFTER_MS   = 10UL * 60 * 1000;   // saved network missing this long -> hotspot
const uint32_t RETRY_MS      = 30000;
const uint32_t RETRY_BUSY_MS = 120000;             // slower while a phone is on the hotspot
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
  WiFi.softAP(AP_SSID);
  dns.start(53, "*", WiFi.softAPIP());     // captive portal
  apOn = true;
  Serial.printf("hotspot %s open at http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void apStop() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  apOn = false;
  Serial.println("hotspot closed");
}

void startWifi() {
  WiFi.mode(WIFI_STA);
  configSta();
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  if (strlen(set.ssid)) WiFi.begin(set.ssid, set.pass);
  staDownSince = lastRetryMs = millis();
  if (!set.configured) apStart();          // fresh flash: hotspot right away
}

void serviceWifi() {
  uint32_t now = millis();
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wasUp) {
    if (up) { MDNS.begin("aogswitch"); Serial.printf("wifi up %s\n", WiFi.localIP().toString().c_str()); }
    else Serial.println("wifi down");
    wasUp = up;
  }
  if (up) { staDownSince = now; if (apOn) apStop(); return; }
  if (!apOn && (!set.configured || now - staDownSince >= AP_AFTER_MS)) apStart();
  uint32_t every = (apOn && WiFi.softAPgetStationNum() > 0) ? RETRY_BUSY_MS : RETRY_MS;
  if (strlen(set.ssid) && now - lastRetryMs >= every) {
    lastRetryMs = now;
    if (apOn) WiFi.begin(set.ssid, set.pass); else WiFi.reconnect();
  }
}

// ---------------- WebUI ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html><html lang=en><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>AOG Switch</title><style>
:root{--bg:#f4f6f8;--card:#fff;--ink:#1d2329;--mute:#6b7682;--line:#e2e6ea;--on:#1f9d6b;--off:#b5402f;--acc:#2f6fb5}
@media (prefers-color-scheme:dark){:root{--bg:#12161a;--card:#1b2127;--ink:#e6eaee;--mute:#93a0ac;--line:#2b333b}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.4 system-ui,sans-serif}
main{max-width:520px;margin:0 auto;padding:12px}.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:12px}
h1{font-size:18px;margin:4px 0 12px}h3{margin:0 0 8px;font-size:13px;color:var(--mute);text-transform:uppercase;letter-spacing:.04em}
#big{border-radius:14px;padding:22px;text-align:center;color:#fff;font-size:28px;font-weight:800;background:var(--mute)}
#big small{display:block;font-size:13px;font-weight:500;opacity:.9;margin-top:4px}
.on{background:var(--on)!important}.off{background:var(--off)!important}
.kv{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;font-size:13px}.kv b{color:var(--mute);font-weight:600}
label{display:block;font-size:12px;color:var(--mute);margin:10px 0 3px}input,select{width:100%;padding:8px;font-size:15px;border:1px solid var(--line);border-radius:7px;background:var(--card);color:var(--ink)}
input[type=checkbox]{width:auto;margin-right:6px}.chk{display:flex;align-items:center;color:var(--ink);font-size:14px}
button{border:0;border-radius:9px;padding:10px 14px;font-size:15px;color:#fff;background:var(--acc);cursor:pointer;margin:8px 6px 0 0}
button.g{background:var(--on)}button.r{background:var(--off)}button.n{background:none;color:var(--ink);border:1px solid var(--line)}
.desc{font-size:12px;color:var(--mute)}table{width:100%;border-collapse:collapse;font-size:13px}td{padding:5px;border-bottom:1px solid var(--line);cursor:pointer}
</style></head><body><main>
<h1>AOG Switch</h1>
<div class=card><div id=big>...<small></small></div>
 <div class=kv style=margin-top:12px><b>Contact</b><span id=contact>-</span><b>Sent to AOG</b><span id=sent>-</span><b>Changes</b><span id=chg>-</span><b>WiFi</b><span id=wifi>-</span></div>
 <p class=desc>AgOpenGPS only follows the switch while a field is open. Working = section control master AUTO; stopped = OFF.</p>
 <h3 style=margin-top:12px>Test (5 s, overrides the switch)</h3>
 <button class=g onclick="post('/api/test',{on:1})">Send WORKING</button><button class=r onclick="post('/api/test',{on:0})">Send STOPPED</button></div>
<div class=card><h3>Switch</h3>
 <label>Input GPIO (switch to GND)</label><select id=gpio><option value=0>GPIO 0</option><option value=1>GPIO 1</option><option value=2>GPIO 2 (boot strapping pin)</option><option value=3>GPIO 3 (default)</option><option value=4>GPIO 4</option></select>
 <label class=chk><input type=checkbox id=invert>Invert: contact OPEN = working</label>
 <label>How AgOpenGPS follows it</label><select id=mode><option value=0>Switch: AOG always matches the contact (recommended)</option><option value=1>Follow changes: operator can override between changes</option></select>
 <button onclick=saveSw()>Save</button><span class=desc id=swmsg></span></div>
<div class=card><h3>WiFi</h3><div class=desc id=wstat></div>
 <button class=n onclick=scan()>Scan networks</button><span class=desc id=scanst></span><table id=nets></table>
 <label>Network (SSID)</label><input id=ssid><label>Password</label><input id=pass type=password placeholder="(unchanged if blank)">
 <label class=chk><input type=checkbox id=open>Open network (no password)</label>
 <label class=chk><input type=checkbox id=dhcp>Use DHCP (then http://aogswitch.local)</label>
 <label>Static IP (when not DHCP)</label><input id=ip>
 <button onclick=saveWifi()>Save &amp; reboot</button>
 <p class=desc>If the saved network is missing for 10 minutes, the open hotspot <b>AOG-Switch</b> (http://192.168.4.1) opens and the network keeps being retried. On a fresh flash the hotspot opens immediately.</p></div>
<p class=desc id=fw></p></main><script>
const $=s=>document.querySelector(s);
async function post(u,d){const r=await fetch(u,{method:'POST',body:new URLSearchParams(d||{})});return r.text()}
async function st(){try{const s=await(await fetch('/api/status')).json();const b=$('#big');
 b.className=s.on?'on':'off';b.innerHTML=(s.on?'WORKING':'STOPPED')+'<small>'+(s.test?'TEST override':'from the switch on GPIO '+s.gpio)+'</small>';
 $('#contact').textContent=(s.closed?'closed':'open')+' (GPIO '+s.gpio+' '+(s.closed?'LOW':'HIGH')+')';
 $('#sent').textContent=s.wifi.up?(s.sentOn?'working':'stopped')+' · '+s.sent+' packets':'not sent (no WiFi)';
 $('#chg').textContent=s.changes+(s.changes?' · last '+Math.round(s.lastChange)+' s ago':'');
 $('#wifi').textContent=s.wifi.up?s.wifi.ssid+' · '+s.wifi.ip+' · '+s.wifi.rssi+' dBm':(s.wifi.ap?'hotspot AOG-Switch open, retrying '+s.wifi.ssid:'connecting to '+s.wifi.ssid);
 $('#wstat').textContent=$('#wifi').textContent;$('#fw').textContent=s.fw+' · up '+Math.floor(s.up/60)+' min'}catch(e){$('#big').className='';$('#big').innerHTML='connection lost<small></small>'}}
async function cfg(){const c=await(await fetch('/api/cfg')).json();$('#gpio').value=c.gpio;$('#invert').checked=!!c.invert;$('#mode').value=c.follow;$('#ssid').value=c.ssid;$('#ip').value=c.ip;$('#dhcp').checked=!!c.dhcp}
async function saveSw(){await post('/api/savecfg',{gpio:$('#gpio').value,invert:$('#invert').checked?1:0,follow:$('#mode').value});$('#swmsg').textContent=' saved';setTimeout(()=>$('#swmsg').textContent='',2000);st()}
async function saveWifi(){const ssid=$('#ssid').value.trim();if(!ssid)return alert('Enter a network name');if(!confirm('Save WiFi and reboot?'))return;
 await post('/api/savecfg',{ssid,pass:$('#pass').value,open:$('#open').checked?1:0,dhcp:$('#dhcp').checked?1:0,ip:$('#ip').value,reboot:1});alert('Saved. Rebooting and joining "'+ssid+'".')}
async function scan(){$('#scanst').textContent=' scanning...';for(let i=0;i<20;i++){const d=await(await fetch('/api/scan')).json();if(d.nets){$('#scanst').textContent=' tap a network';
 $('#nets').innerHTML=d.nets.sort((a,b)=>b.rssi-a.rssi).map(n=>`<tr data-s="${n.ssid.replace(/"/g,'&quot;')}"><td>${n.ssid.replace(/</g,'&lt;')||'(hidden)'}</td><td>${n.rssi} dBm</td><td>${n.open?'open':'🔒'}</td></tr>`).join('');
 document.querySelectorAll('#nets tr').forEach(t=>t.onclick=()=>{$('#ssid').value=t.dataset.s;$('#pass').focus()});return}await new Promise(r=>setTimeout(r,700))}$('#scanst').textContent=' scan timed out'}
cfg();st();setInterval(st,1000);
</script></body></html>)rawliteral";

void jsonEsc(String &j, const char *s) {
  j += '"';
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') { j += '\\'; j += *s; }
    else if ((uint8_t)*s < 0x20) j += ' ';
    else j += *s;
  }
  j += '"';
}

void sendJson(const String &s) { server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", s); }

void handleStatus() {
  uint32_t now = millis();
  bool up = WiFi.status() == WL_CONNECTED;
  String j = "{\"fw\":\"" + String(FW) + "\",\"up\":" + String(now / 1000) + ",\"on\":" + String(machineOn() ? 1 : 0) +
             ",\"test\":" + String(testOverride >= 0 ? 1 : 0) + ",\"closed\":" + String(stableLevel ? 0 : 1) +
             ",\"gpio\":" + String(set.gpio) + ",\"sent\":" + String(sent) + ",\"sentOn\":" + String(lastSentOn ? 1 : 0) +
             ",\"changes\":" + String(changes) + ",\"lastChange\":" + String(changes ? (now - lastChangeMs) / 1000.0f : 0, 1) +
             ",\"wifi\":{\"up\":" + String(up ? 1 : 0) + ",\"ssid\":";
  jsonEsc(j, set.ssid);
  j += ",\"ip\":\"" + (up ? WiFi.localIP().toString() : String("0.0.0.0")) + "\",\"rssi\":" + String(up ? WiFi.RSSI() : 0) +
       ",\"ap\":" + String(apOn ? 1 : 0) + "}}";
  sendJson(j);
}

void handleCfg() {
  String j = "{\"ssid\":";
  jsonEsc(j, set.ssid);
  char b[120];
  snprintf(b, sizeof(b), ",\"ip\":\"%u.%u.%u.%u\",\"dhcp\":%d,\"gpio\":%u,\"invert\":%d,\"follow\":%d}",
           set.ip[0], set.ip[1], set.ip[2], set.ip[3], set.dhcp ? 1 : 0, set.gpio, set.invert ? 1 : 0, set.followChanges ? 1 : 0);
  j += b;
  sendJson(j);
}

// POST form: any of gpio, invert, follow, ssid, pass (blank = unchanged), open=1, dhcp, ip, reboot=1
void handleSaveCfg() {
  bool pinChanged = false;
  if (server.hasArg("gpio")) { long v = server.arg("gpio").toInt(); if (v >= 0 && v <= 4 && v != set.gpio) { set.gpio = v; pinChanged = true; } }
  if (server.hasArg("invert")) set.invert = server.arg("invert") == "1";
  if (server.hasArg("follow")) set.followChanges = server.arg("follow") == "1";
  if (server.hasArg("ssid") && server.arg("ssid").length()) { strlcpy(set.ssid, server.arg("ssid").c_str(), sizeof(set.ssid)); set.configured = true; }
  if (server.arg("open") == "1") set.pass[0] = 0;
  else if (server.hasArg("pass") && server.arg("pass").length()) strlcpy(set.pass, server.arg("pass").c_str(), sizeof(set.pass));
  if (server.hasArg("dhcp")) set.dhcp = server.arg("dhcp") == "1";
  if (server.hasArg("ip")) { IPAddress a; if (a.fromString(server.arg("ip"))) for (uint8_t i = 0; i < 4; i++) set.ip[i] = a[i]; }
  saveSettings();
  if (pinChanged || server.hasArg("invert")) setupInput();   // switch settings apply immediately
  server.send(200, "text/plain", "OK");
  if (server.arg("reboot") == "1") reqReboot = true;
}

void handleTest() {
  testOverride = server.arg("on") == "1" ? 1 : 0;
  testUntilMs = millis() + 5000;
  server.send(200, "text/plain", "OK");
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
  WiFi.scanDelete();
  sendJson(j);
}

void captiveRedirect() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
  server.send(302, "text/plain", "");
}

void handleNotFound() {
  String host = server.hostHeader();
  if (apOn && host != WiFi.softAPIP().toString() && host != WiFi.localIP().toString() && host != "aogswitch.local") {
    captiveRedirect();
    return;
  }
  server.send(404, "text/plain", "Not found");
}

void setupWeb() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/cfg", HTTP_GET, handleCfg);
  server.on("/api/savecfg", HTTP_POST, handleSaveCfg);
  server.on("/api/test", HTTP_POST, handleTest);
  server.on("/api/scan", HTTP_GET, handleScan);
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
  setupInput();
  startWifi();
  udp.begin(0);                            // send only (ephemeral local port)
  setupWeb();
  lastSentOn = !machineOn();               // forces an immediate first send once WiFi is up
}

void loop() {
  uint32_t now = millis();
  readInput();
  serviceSend();
  server.handleClient();
  if (apOn) dns.processNextRequest();

  static uint32_t lastWifi = 0, lastBlink = 0;
  if (now - lastWifi >= 500) { lastWifi = now; serviceWifi(); }
  if (apOn) {
    if (now - lastBlink >= 500) { lastBlink = now; digitalWrite(LED_PIN, !digitalRead(LED_PIN)); }
  } else {
    digitalWrite(LED_PIN, machineOn() ? LOW : HIGH);
  }
  if (reqReboot) { delay(500); ESP.restart(); }
  delay(2);
}
