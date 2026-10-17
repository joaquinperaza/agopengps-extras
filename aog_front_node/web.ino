/*
  WebUI: one page + JSON/text endpoints. Runs in webTask (aog_front_node.ino), which stops
  serving WEB_TIMEOUT_MS after boot unless in AP mode.

  INDEX_HTML is byte-identical to the live module's page (docs/live_capture/index.html).
  /cfg and /snap reproduce the live JSON field order and number formats.

  The web task never touches Wire1 or the link socket: scan / BNO reset / ping are requested
  via flags and executed by loop(). Those three stall the control loop for up to ~1.3 s,
  so they are refused while steering is engaged (the live module does not guard this).
*/

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>AOG Front Node</title><style>
 body{font-family:system-ui,sans-serif;max-width:520px;margin:12px auto;padding:0 12px}
 h1{font-size:1.2rem} fieldset{border:1px solid #ccc;border-radius:8px;margin:10px 0;padding:10px}
 legend{font-size:.9rem;color:#333;font-weight:bold}
 label{display:block;margin:8px 0 2px;font-size:.85rem;color:#444}
 .desc{font-size:.75rem;color:#888;margin:0 0 4px}
 input[type=number],input[type=text],input[type=password]{width:100%;padding:6px;box-sizing:border-box;border:1px solid #ccc;border-radius:4px}
 input[type=number]{width:120px}
 input[type=checkbox]{margin-right:6px}
 button{padding:9px 14px;border:0;border-radius:8px;background:#2b6;color:#fff;font-size:1rem;cursor:pointer;margin:3px 2px}
 button.alt{background:#37c} button.warn{background:#a44}
 .snap{background:#f4f4f7;border-radius:8px;padding:10px;margin:8px 0;font-family:monospace;font-size:.85rem}
 #msg{margin:8px 0;padding:8px;border-radius:6px;display:none;font-size:.9rem}
</style></head><body>
<h1>AOG Front-Axle Node</h1>
<div id=msg></div>

<fieldset><legend>Quick actions</legend>
 <button onclick="doZero()">Zero WAS now (center wheels first)</button>
 <button class=alt onclick="doSnap()">Snapshot (read sensors once)</button>
 <button class=alt onclick="doPing()">Ping cabin motor (5x, pwm=0)</button>
 <div id=snap class=snap style="display:none"></div>
</fieldset>

<form onsubmit="return doSave(event)">

<fieldset><legend>WAS (Wheel Angle Sensor)</legend>
 <label>Counts per degree</label>
 <p class=desc>ADC counts per 1 degree of wheel turn. Measure by turning wheels a known angle and dividing raw count change by degrees.</p>
 <input type=number id=f_cpd step=1 min=1 max=1000>

 <label>WAS zero offset (raw ADC)</label>
 <p class=desc>Raw ADC reading at straight-ahead. Use the "Zero WAS now" button above instead of typing manually.</p>
 <input type=number id=f_wz step=1>

 <label>Ackerman fix (%)</label>
 <p class=desc>Corrects for Ackerman steering geometry. 100 = no correction. Adjust if the tractor drifts in curves.</p>
 <input type=number id=f_ack step=1 min=1 max=200>

 <label>Max steer angle (degrees)</label>
 <p class=desc>Safety clamp. PID will never command beyond this. Set to your wheel-lock angle.</p>
 <input type=number id=f_msa step=1 min=5 max=80>

 <label><input type=checkbox id=f_diff> Differential WAS (A0-A1)</label>
 <p class=desc>ADS1115 differential mode. Rejects common-mode noise. Uncheck for single-ended A0.</p>

 <label><input type=checkbox id=f_inv> Invert WAS</label>
 <p class=desc>Flip angle sign. Check if tractor steers the wrong way.</p>
</fieldset>

<fieldset><legend>PID / Motor drive</legend>
 <label>Kp (proportional gain)</label>
 <p class=desc>Corrects based on current error. Start low (0.3-1.0), increase until responsive.</p>
 <input type=number id=f_kp step=0.01 min=0 max=255>

 <label>Ki (integral gain)</label>
 <p class=desc>Corrects accumulated error over time. Fixes steady-state offset. Start at 0, increase slowly (0.01-0.1). Too high causes slow oscillation. Leave 0 if not needed.</p>
 <input type=number id=f_ki step=0.01 min=0 max=10>

 <label>Kd (derivative gain)</label>
 <p class=desc>Opposes rapid error change — brakes the motor before it reaches the target. Prevents overshoot with fast/strong motors. Start 0, increase (0.5-5.0) until overshoot stops.</p>
 <input type=number id=f_kd step=0.01 min=0 max=100>

 <label>High PWM (max motor speed)</label>
 <p class=desc>Maximum PWM duty (0-255) when steering error is large.</p>
 <input type=number id=f_hpwm step=1 min=0 max=255>

 <label>Low PWM (near-target speed)</label>
 <p class=desc>PWM duty when error is small. Ramps from low to high over 5 degrees.</p>
 <input type=number id=f_lpwm step=1 min=0 max=255>

 <label>Min PWM (stiction kick)</label>
 <p class=desc>Minimum PWM to overcome motor stiction. Set to lowest duty that makes your motor move.</p>
 <input type=number id=f_mpwm step=1 min=0 max=255>

 <label>Dead zone (degrees)</label>
 <p class=desc>Error below this = motor stays still. Prevents hunting. Start 1.0, raise if motor chatters.</p>
 <input type=number id=f_dz step=0.1 min=0 max=10>

 <label><input type=checkbox id=f_mdir> Invert motor direction</label>
 <p class=desc>Flip PWM sign to cabin node. Check if motor turns the wrong way.</p>
</fieldset>

<fieldset><legend>IMU</legend>
 <label><input type=checkbox id=f_imu> IMU enabled (BNO085)</label>
 <p class=desc>Enable BNO085 for heading and roll (PGN 253). Disable if no BNO connected.</p>
 <button type=button class=warn onclick="doBnoReset()">Hard reset BNO (tears down I2C bus)</button>
 <p class=desc>Tears down Wire1 and does full bus recovery + BNO re-init. WAS will glitch for ~1 second. Only use if BNO is stuck and soft retries aren't working.</p>
 <button type=button class=alt onclick="doScan()">I2C scan (Wire1)</button>
 <p class=desc>Probes all addresses on the STEMMA QT bus. Expected: 0x48 (ADS1115), 0x4A or 0x4B (BNO085).</p>
 <div id=scanres style="font-family:monospace;font-size:.85rem;margin:4px 0"></div>
</fieldset>

<fieldset><legend>Network</legend>
 <label>WiFi SSID</label>
 <input type=text id=f_ssid>
 <label>WiFi password</label>
 <input type=password id=f_pass placeholder="(unchanged if blank)">
 <label>Static IP</label>
 <input type=text id=f_ip>
 <label><input type=checkbox id=f_dhcp> Use DHCP</label>
 <label>Cabin motor node IP</label>
 <input type=text id=f_cip>
</fieldset>

<button type=submit>Save</button>
<button type=button class=warn onclick="doReboot()">Save &amp; Reboot</button>
</form>

<script>
var E=function(id){return document.getElementById(id)};
function msg(t,ok){var m=E('msg');m.textContent=t;m.style.background=ok?'#d4edda':'#f8d7da';m.style.display='block';setTimeout(function(){m.style.display='none'},3000);}
async function loadCfg(){
 try{let c=await(await fetch('/cfg')).json();
 E('f_cpd').value=c.cpd;E('f_wz').value=c.wz;E('f_ack').value=c.ack;
 E('f_msa').value=c.msa;E('f_diff').checked=!!c.diff;E('f_inv').checked=!!c.inv;
 E('f_kp').value=c.kp;E('f_ki').value=c.ki;E('f_kd').value=c.kd;E('f_hpwm').value=c.hpwm;E('f_lpwm').value=c.lpwm;
 E('f_mpwm').value=c.mpwm;E('f_mdir').checked=!!c.mdir;E('f_dz').value=c.dz;
 E('f_imu').checked=!!c.imu;E('f_ssid').value=c.ssid;
 E('f_ip').value=c.ip;E('f_cip').value=c.cip;E('f_dhcp').checked=!!c.dhcp;
 }catch(e){msg('Failed to load config',false);}
}
loadCfg();
function qs(){
 return 'cpd='+E('f_cpd').value+'&wz='+E('f_wz').value+'&ack='+E('f_ack').value
 +'&msa='+E('f_msa').value+'&diff='+(E('f_diff').checked?1:0)
 +'&inv='+(E('f_inv').checked?1:0)+'&kp='+E('f_kp').value+'&ki='+E('f_ki').value+'&kd='+E('f_kd').value
 +'&hpwm='+E('f_hpwm').value+'&lpwm='+E('f_lpwm').value+'&mpwm='+E('f_mpwm').value
 +'&mdir='+(E('f_mdir').checked?1:0)+'&dz='+E('f_dz').value+'&imu='+(E('f_imu').checked?1:0)
 +'&ssid='+encodeURIComponent(E('f_ssid').value)
 +'&pass='+encodeURIComponent(E('f_pass').value)
 +'&ip='+encodeURIComponent(E('f_ip').value)
 +'&cip='+encodeURIComponent(E('f_cip').value)
 +'&dhcp='+(E('f_dhcp').checked?1:0);
}
async function doSave(e){e.preventDefault();await fetch('/savecfg?'+qs());msg('Saved!',true);await loadCfg();return false;}
async function doZero(){await fetch('/zero');await loadCfg();msg('WAS zeroed to current position',true);}
async function doSnap(){
 E('snap').style.display='block';E('snap').textContent='reading...';
 try{let d=await(await fetch('/snap')).json();
 E('snap').textContent='WAS raw='+d.raw+' angle='+d.ang+' hdg='+d.head+' roll='+d.roll
  +' BNO='+(d.bno?'ok':'FAIL')+' retries='+d.bnor
  +' cabin='+(d.cabin?'ok ('+d.rtt+'ms)':'--')+' engaged='+(d.eng?'YES':'no')+' pwm='+d.pwm;
 }catch(e){E('snap').textContent='read failed';}
}
async function doPing(){E('snap').style.display='block';E('snap').textContent='pinging cabin (5x pwm=0)...';
 try{let d=await(await fetch('/ping')).json();
 let r=d.rtt.map(function(v){return v<0?'TIMEOUT':v+'ms'});
 E('snap').textContent='Cabin RTT: '+r.join(', ');}catch(e){E('snap').textContent='ping failed';}}
async function doBnoReset(){msg('Resetting BNO...',true);let r=await(await fetch('/bnorst')).text();msg(r,r.startsWith('BNO ok'));}
async function doScan(){E('scanres').textContent='scanning...';let d=await(await fetch('/scan')).json();E('scanres').textContent='Wire1: '+(d.found.length?d.found.join('  '):'(nothing found)')+' [ADS=0x48 BNO=0x4A/0x4B]';}
async function doReboot(){await fetch('/savecfg?'+qs());await fetch('/reboot');msg('Rebooting...',true);}
</script></body></html>
)rawliteral";

static bool waitFor(volatile bool &flag, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (!flag && millis() - t0 < timeoutMs) vTaskDelay(5);
  return flag;
}

static bool refuseWhileSteering() {
  if (!steerEngaged) return false;
  server.send(409, "text/plain", "Refused: steering engaged");
  return true;
}

static bool parseIp(const String &s, uint8_t *out) {
  IPAddress a;
  if (!a.fromString(s)) return false;
  for (uint8_t i = 0; i < 4; i++) out[i] = a[i];
  return true;
}

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleCfg() {
  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"cpd\":%.0f,\"wz\":%d,\"ack\":%.0f,\"msa\":%u,\"diff\":%d,\"inv\":%d,"
    "\"kp\":%.2f,\"ki\":%.2f,\"kd\":%.2f,\"hpwm\":%u,\"lpwm\":%u,\"mpwm\":%u,\"mdir\":%u,\"dz\":%.1f,"
    "\"ssid\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"cip\":\"%u.%u.%u.%u\",\"imu\":%d,\"dhcp\":%d}",
    cfg.countsPerDegree, cfg.wasZero, cfg.ackermanFix, cfg.maxSteerAngle,
    cfg.wasDifferential ? 1 : 0, cfg.invertWas ? 1 : 0,
    cfg.kp, cfg.ki, cfg.kd, cfg.highPWM, cfg.lowPWM, cfg.minPWM, cfg.motorDriveDir, cfg.deadZone,
    cfg.ssid, cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3],
    cfg.cabinIp[0], cfg.cabinIp[1], cfg.cabinIp[2], cfg.cabinIp[3],
    cfg.imuEnable ? 1 : 0, cfg.useDhcp ? 1 : 0);
  server.send(200, "application/json", buf);
}

void handleSaveCfg() {
  // Only fields present (and valid) are changed. Blank password = unchanged.
  if (server.hasArg("cpd"))  { float v = server.arg("cpd").toFloat(); if (v >= 1 && v <= 1000) cfg.countsPerDegree = v; }
  if (server.hasArg("wz"))   cfg.wasZero = (int16_t)server.arg("wz").toInt();
  if (server.hasArg("ack"))  { float v = server.arg("ack").toFloat(); if (v >= 1 && v <= 200) cfg.ackermanFix = v; }
  if (server.hasArg("msa"))  { long v = server.arg("msa").toInt(); if (v >= 5 && v <= 80) cfg.maxSteerAngle = v; }
  if (server.hasArg("diff")) cfg.wasDifferential = server.arg("diff") == "1";
  if (server.hasArg("inv"))  cfg.invertWas = server.arg("inv") == "1";
  if (server.hasArg("kp"))   { float v = server.arg("kp").toFloat(); if (v >= 0 && v <= 255) cfg.kp = v; }
  if (server.hasArg("ki"))   { float v = server.arg("ki").toFloat(); if (v >= 0 && v <= 10) cfg.ki = v; }
  if (server.hasArg("kd"))   { float v = server.arg("kd").toFloat(); if (v >= 0 && v <= 100) cfg.kd = v; }
  if (server.hasArg("hpwm")) { long v = server.arg("hpwm").toInt(); if (v >= 0 && v <= 255) cfg.highPWM = v; }
  if (server.hasArg("lpwm")) { long v = server.arg("lpwm").toInt(); if (v >= 0 && v <= 255) cfg.lowPWM = v; }
  if (server.hasArg("mpwm")) { long v = server.arg("mpwm").toInt(); if (v >= 0 && v <= 255) cfg.minPWM = v; }
  if (server.hasArg("mdir")) cfg.motorDriveDir = server.arg("mdir") == "1" ? 1 : 0;
  if (server.hasArg("dz"))   { float v = server.arg("dz").toFloat(); if (v >= 0 && v <= 10) cfg.deadZone = v; }
  if (server.hasArg("imu"))  cfg.imuEnable = server.arg("imu") == "1";
  if (server.hasArg("ssid") && server.arg("ssid").length() > 0)
    strlcpy(cfg.ssid, server.arg("ssid").c_str(), sizeof(cfg.ssid));
  if (server.hasArg("pass") && server.arg("pass").length() > 0)
    strlcpy(cfg.pass, server.arg("pass").c_str(), sizeof(cfg.pass));
  if (server.hasArg("ip"))   parseIp(server.arg("ip"), cfg.ip);
  if (server.hasArg("cip"))  parseIp(server.arg("cip"), cfg.cabinIp);
  if (server.hasArg("dhcp")) cfg.useDhcp = server.arg("dhcp") == "1";
  saveConfig();
  server.send(200, "text/plain", "OK");
}

void handleZero() {
  cfg.wasZero = wasRaw;
  saveConfig();
  server.send(200, "text/plain", "OK");
}

void handleSnap() {
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"raw\":%d,\"ang\":\"%.1f\",\"head\":\"%.1f\",\"roll\":\"%.1f\",\"cabin\":%d,\"rtt\":%d,"
    "\"eng\":%d,\"pwm\":%d,\"bno\":%d,\"bnor\":%lu}",
    wasRaw, steerAngle, imuHeading, imuRoll, cabinAlive() ? 1 : 0, linkRttMs,
    steerEngaged ? 1 : 0, pwmCommand, bnoOk ? 1 : 0, (unsigned long)bnoRetries);
  server.send(200, "application/json", buf);
}

void handlePing() {
  if (refuseWhileSteering()) return;
  pingDone = false;
  reqPing = true;
  waitFor(pingDone, 3000);
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"rtt\":[%d,%d,%d,%d,%d]}",
           pingResults[0], pingResults[1], pingResults[2], pingResults[3], pingResults[4]);
  server.send(200, "application/json", buf);
}

void handleScan() {
  if (refuseWhileSteering()) return;
  scanDone = false;
  reqScan = true;
  waitFor(scanDone, 3000);
  String s = "{\"found\":[";
  for (uint8_t i = 0; i < scanCount; i++) {
    char a[12];
    snprintf(a, sizeof(a), "%s\"0x%02X\"", i ? "," : "", scanResult[i]);
    s += a;
  }
  s += "]}";
  server.send(200, "application/json", s);
}

void handleBnoReset() {
  if (refuseWhileSteering()) return;
  bnoResetDone = false;
  reqBnoReset = true;
  if (!waitFor(bnoResetDone, 5000)) { server.send(200, "text/plain", "BNO reset timed out"); return; }
  server.send(200, "text/plain", bnoResetOk ? "BNO ok (re-initialised)" : "BNO FAIL (not found after bus reset)");
}

void handleReboot() {
  uint32_t t0 = millis();
  while (reqSave && millis() - t0 < 2000) vTaskDelay(5);   // let loop() finish the NVS write
  server.send(200, "text/plain", "Rebooting");
  vTaskDelay(300);
  ESP.restart();
}

void handleNotFound() {
  server.send(404, "text/html", "Not found: " + server.uri());
}

void setupWeb() {
  server.on("/",        HTTP_GET, handleRoot);
  server.on("/cfg",     HTTP_GET, handleCfg);
  server.on("/savecfg", HTTP_GET, handleSaveCfg);
  server.on("/zero",    HTTP_GET, handleZero);
  server.on("/snap",    HTTP_GET, handleSnap);
  server.on("/ping",    HTTP_GET, handlePing);
  server.on("/scan",    HTTP_GET, handleScan);
  server.on("/bnorst",  HTTP_GET, handleBnoReset);
  server.on("/reboot",  HTTP_GET, handleReboot);
  server.onNotFound(handleNotFound);
  server.begin();
}
