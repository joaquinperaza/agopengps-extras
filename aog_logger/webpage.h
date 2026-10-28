// AOG Logger WebUI: one self-contained page (no internet needed). The browser downloads the raw
// log (/log.bin), decodes it (format: tlog.h) and draws charts/metrics; the C3 only serves bytes.
#pragma once
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html><html lang=en><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>AOG Logger</title><style>
:root{--bg:#f4f6f8;--card:#fff;--ink:#1d2329;--mute:#6b7682;--line:#e2e6ea;--acc:#1f7a4d;--acc2:#2f6fb5;--warn:#b5402f;--c1:#2f6fb5;--c2:#e07a1f;--c3:#1f9d6b;--c4:#9b4dca;--c5:#c9a227;--shade:rgba(31,157,107,.10)}
@media (prefers-color-scheme:dark){:root{--bg:#12161a;--card:#1b2127;--ink:#e6eaee;--mute:#93a0ac;--line:#2b333b;--shade:rgba(31,157,107,.18)}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{position:sticky;top:0;z-index:5;background:var(--card);border-bottom:1px solid var(--line);padding:8px 12px;display:flex;flex-wrap:wrap;gap:8px;align-items:center}
header b{font-size:16px}#hstat{color:var(--mute);font-size:12px;flex:1}nav{display:flex;gap:4px;flex-wrap:wrap}
nav button{background:none;border:1px solid var(--line);color:var(--ink);border-radius:999px;padding:5px 12px;cursor:pointer}nav button.on{background:var(--acc);border-color:var(--acc);color:#fff}
main{padding:12px;max-width:1200px;margin:0 auto}section{display:none}section.on{display:block}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(170px,1fr))}.g2{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(320px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px;margin-bottom:12px;min-width:0}
.card h3{margin:0 0 8px;font-size:13px;color:var(--mute);font-weight:600;text-transform:uppercase;letter-spacing:.04em}
.kpi .v{font-size:22px;font-weight:700}.kpi .s{color:var(--mute);font-size:12px}
.ok{color:var(--acc)}.bad{color:var(--warn)}
canvas{width:100%;height:170px;display:block;touch-action:pan-y}canvas.tall{height:220px}
table{width:100%;border-collapse:collapse;font-size:12px}th,td{text-align:left;padding:4px 6px;border-bottom:1px solid var(--line);vertical-align:top}th{color:var(--mute);font-weight:600}
td.n,th.n{text-align:right;font-variant-numeric:tabular-nums}.mono{font-family:ui-monospace,Menlo,Consolas,monospace}
.bar{height:10px;background:var(--acc2);border-radius:5px;min-width:2px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:6px 0}
button.b,a.b{background:var(--acc);color:#fff;border:0;border-radius:8px;padding:8px 14px;font-size:14px;cursor:pointer;text-decoration:none;display:inline-block}
.b.alt{background:var(--acc2)}.b.warn{background:var(--warn)}.b.ghost{background:none;color:var(--ink);border:1px solid var(--line)}.b.ghost.on{background:var(--acc2);color:#fff;border-color:var(--acc2)}
input,select{background:var(--card);color:var(--ink);border:1px solid var(--line);border-radius:6px;padding:7px;font-size:14px}input[type=text],input[type=password]{width:100%}
label{display:block;font-size:12px;color:var(--mute);margin:8px 0 3px}.desc{font-size:12px;color:var(--mute)}
.pk tr.d td{background:var(--bg)}.pk tbody tr{cursor:pointer}.legend{display:flex;gap:10px;flex-wrap:wrap;font-size:12px;color:var(--mute);margin-top:4px}.legend i{display:inline-block;width:10px;height:3px;margin-right:4px;vertical-align:middle}
.net{cursor:pointer}.net:hover{background:var(--bg)}.pill{font-size:11px;border:1px solid var(--line);border-radius:999px;padding:1px 7px;color:var(--mute)}
</style></head><body>
<header><b>AOG Logger</b><span id=hstat>connecting...</span>
<nav id=nav><button data-t=dash class=on>Dashboard</button><button data-t=steer>Steering</button><button data-t=pk>Packets</button><button data-t=gps>GPS</button><button data-t=set>Settings</button></nav></header>
<main>
<div class=row id=rangebar><span class=desc>Window:</span>
 <button class="b ghost" data-r=120>2 min</button><button class="b ghost on" data-r=300>5 min</button><button class="b ghost" data-r=900>15 min</button><button class="b ghost" data-r=3600>60 min</button>
 <button class="b ghost" id=livebtn>Live</button><span class=desc id=zoominfo>drag on a chart to zoom, double-click to reset</span></div>

<section id=t-dash class=on>
 <div class=grid>
  <div class="card kpi"><h3>WiFi</h3><div class=v id=k-wifi>-</div><div class=s id=k-wifi2></div></div>
  <div class="card kpi"><h3>Log</h3><div class=v id=k-log>-</div><div class=s id=k-log2></div></div>
  <div class="card kpi"><h3>Traffic</h3><div class=v id=k-rate>-</div><div class=s id=k-rate2></div></div>
  <div class="card kpi"><h3>Autosteer</h3><div class=v id=k-eng>-</div><div class=s id=k-eng2></div></div>
 </div>
 <div class=g2>
  <div class=card><h3>Packets per second by source</h3><canvas id=c-rates></canvas><div class=legend id=l-rates></div></div>
  <div class=card><h3>WiFi signal (logger)</h3><canvas id=c-rssi></canvas></div>
 </div>
 <div class=g2>
  <div class=card><h3>PGN count by type (since logger boot)</h3><table id=pgnt></table></div>
  <div class=card><h3>Who is talking</h3><table id=srct></table></div>
 </div>
</section>

<section id=t-steer>
 <div class=grid id=steerk></div>
 <div class=card><h3>Steer angle: setpoint (AOG) vs actual (WAS)</h3><canvas id=c-ang class=tall></canvas><div class=legend><span><i style=background:var(--c2)></i>setpoint</span><span><i style=background:var(--c1)></i>actual</span><span><i style=background:var(--shade);height:10px></i>autosteer engaged</span></div></div>
 <div class=g2>
  <div class=card><h3>Error (actual - setpoint), deg</h3><canvas id=c-err></canvas></div>
  <div class=card><h3>Motor |PWM|</h3><canvas id=c-pwm></canvas></div>
  <div class=card><h3>Cross-track error, cm</h3><canvas id=c-xte></canvas></div>
  <div class=card><h3>Speed, km/h</h3><canvas id=c-spd></canvas></div>
  <div class=card><h3>Heading / roll (IMU)</h3><canvas id=c-imu></canvas><div class=legend><span><i style=background:var(--c4)></i>heading</span><span><i style=background:var(--c3)></i>roll x10</span></div></div>
  <div class=card><h3>Error by speed band</h3><table id=bandt></table></div>
 </div>
 <div class=card><h3>Latest settings / machine values seen</h3><table id=latest></table></div>
</section>

<section id=t-pk>
 <div class=card><div class=row>
  <select id=f-kind><option value="">all kinds</option><option>pgn</option><option>nmea</option><option>hydraulic</option><option>raw</option><option>logger</option></select>
  <select id=f-src><option value="">all sources</option></select>
  <select id=f-pgn><option value="">all PGNs</option></select>
  <input id=f-txt placeholder="search hex / text" style="width:auto;flex:1">
  <span class=desc id=pkn></span></div>
  <div style=overflow-x:auto><table class=pk><thead><tr><th>time</th><th>from</th><th>port</th><th>what</th><th>decoded</th></tr></thead><tbody id=pkb></tbody></table></div>
 </div>
</section>

<section id=t-gps>
 <div class=grid id=gpsk></div>
 <div class=g2>
  <div class=card><h3>Fix quality (4 = RTK fix, 5 = float, 1 = GPS)</h3><canvas id=c-fix></canvas></div>
  <div class=card><h3>Satellites / HDOP x10</h3><canvas id=c-sat></canvas><div class=legend><span><i style=background:var(--c1)></i>satellites</span><span><i style=background:var(--c2)></i>HDOP x10</span></div></div>
  <div class=card><h3>RTCM corrections, bytes/s (AgIO to GPS)</h3><canvas id=c-rtcm></canvas></div>
  <div class=card><h3>Correction age, s</h3><canvas id=c-age></canvas></div>
 </div>
 <div class=card><h3>Latest GPS sentences</h3><table id=nmeat></table></div>
</section>

<section id=t-set>
 <div class=g2>
 <div class=card><h3>WiFi</h3><div id=wstat class=desc></div>
  <div class=row><button class="b alt" onclick=scan()>Scan networks</button><span class=desc id=scanst></span></div><table id=nets></table>
  <label>Network name (SSID)</label><input type=text id=s-ssid>
  <label>Password</label><input type=password id=s-pass placeholder="(unchanged if blank)">
  <label><input type=checkbox id=s-open style=width:auto> Open network (no password)</label>
  <label><input type=checkbox id=s-dhcp style=width:auto> Use DHCP (then open http://aoglogger.local)</label>
  <label>Static IP (when not DHCP)</label><input type=text id=s-ip>
  <div class=row><button class=b onclick=saveWifi()>Save &amp; reboot</button></div>
  <p class=desc>If the saved network cannot be joined for 10 minutes, the logger opens the open hotspot <b>AOG-Logger-Setup</b> (address 192.168.4.1) and keeps retrying the network. On a fresh flash the hotspot is open immediately.</p></div>
 <div class=card><h3>Logging</h3>
  <label>GPS NMEA sentences</label><select id=s-nmea><option value=0>Off (longest log)</option><option value=1>Key sentences, 1 per second</option><option value=2>All (log window much shorter)</option></select>
  <div class=row><button class=b onclick=saveLog()>Save</button></div>
  <label>Marker (what did you change?)</label><input type=text id=mk placeholder="e.g. Kp 0.60 -> 0.70, field 3">
  <div class=row><button class="b alt" onclick=mark()>Add marker</button></div>
  <label>Download raw log (decode with tools/tlog_decode.py, or give it to Claude)</label>
  <div class=row><a class="b alt" href="/log.bin?minutes=15">Last 15 min</a><a class="b alt" href="/log.bin?minutes=60">Last 60 min</a><a class="b alt" href="/log.bin">Everything</a></div>
  <div class=row><button class="b warn" onclick=clr()>Clear log</button><button class="b ghost" onclick="post('/api/reboot')">Reboot</button></div></div>
 </div>
</section>
</main>
<script>
"use strict";
const $=s=>document.querySelector(s),$$=s=>[...document.querySelectorAll(s)];
const SRCN={127:'AgIO',126:'steer',123:'machine',124:'GPS',121:'IMU',122:'front→cabin'};
const PGNN={254:'Steer data',253:'Steer status',252:'Steer settings',251:'Steer config',250:'Sensor',239:'Machine data',238:'Machine config',237:'Machine pins',229:'Sections 64',234:'Remote switches',211:'IMU',212:'IMU',200:'Hello',201:'Subnet set',202:'Scan',203:'Scan reply',126:'Hello reply',123:'Hello reply',121:'Hello reply',124:'Hello reply'};
const HYD=['Lift L','Lift R','Open L','All off','Backup'];
let defSub='x.x.x',ST=null,SEC=new Map(),REC=[],maxSeq=0,range=300,zoom=null,live=true,hoverT=null,tab='dash',utcOff=null,curBoot=0;
const css=v=>getComputedStyle(document.documentElement).getPropertyValue(v).trim();

// ---------- log decoding (format: tlog.h) ----------
function parseLog(buf){const dv=new DataView(buf),u=new Uint8Array(buf);
 for(let o=0;o+4096<=u.length;o+=4096){if(dv.getUint32(o,true)!==0x31474C54)continue;
  const seq=dv.getUint32(o+4,true),boot=dv.getUint32(o+8,true);let t=dv.getUint32(o+12,true),p=o+16;const recs=[];
  while(p+5<=o+4096){const len=u[p];if(len===255)break;const ch=u[p+1],src=u[p+2],dt=dv.getUint16(p+3,true);
   if(p+5+len>o+4096)break;const pl=u.slice(p+5,p+5+len);t+=dt;
   if(ch===0xF3&&len===4)t=new DataView(pl.buffer).getUint32(0,true);else recs.push({t,boot,ch,src,p:pl});p+=5+len}
  SEC.set(seq,{seq,boot,recs});if(seq>maxSeq)maxSeq=seq}}
let ports={},subnet={};
function rebuild(){const seqs=[...SEC.keys()].sort((a,b)=>a-b);REC=[];
 for(const s of seqs){const S=SEC.get(s);if(S.boot!==curBoot)continue;for(const r of S.recs){if(r.k===undefined)classify(r);REC.push(r)}}
 utcOff=null;for(const r of REC){if(r.k==='nmea'&&r.f&&r.f.sod!==undefined){utcOff=r.f.sod*1000-r.t;break}}}
function classify(r){const p=r.p;
 if(r.ch>=0xF0){r.k='logger';r.what=['boot/info','marker','stats'][r.ch-0xF0]||'logger';
  if(r.ch===0xF0||r.ch===0xF1){r.txt=new TextDecoder().decode(p);if(r.ch===0xF0){const m=/ports=([\d,]+)/.exec(r.txt);if(m)ports[r.boot]=m[1].split(',').map(Number);const i=/\bip=(\d+\.\d+\.\d+)\.\d+/.exec(r.txt);if(i)subnet[r.boot]=i[1]}}
  if(r.ch===0xF2&&p.length>=9&&p[0]===1){const d=new DataView(p.buffer);const n=p[8];r.f={rssi:d.getInt8(1),heap:d.getUint16(2,true),up:d.getUint32(4,true),seen:[],kept:[],bytes:[]};
   let o=9;for(let i=0;i<n;i++){r.f.seen.push(d.getUint16(o,true));o+=2}for(let i=0;i<n;i++){r.f.kept.push(d.getUint16(o,true));o+=2}for(let i=0;i<n;i++){r.f.bytes.push(d.getUint32(o,true));o+=4}}
  return}
 r.port=(ports[r.boot]||[8888,9999,2233,4242,3344,4433])[r.ch&63];r.trunc=!!(r.ch&64);r.ip=(subnet[r.boot]||defSub)+'.'+r.src;
 if(p.length>=5&&p[0]===0x80&&p[1]===0x81){r.k='pgn';r.as=p[2];r.pgn=p[3];let c=0;for(let i=2;i<p.length-1;i++)c+=p[i];r.crc=((c&255)===p[p.length-1]);
  const d=new DataView(p.buffer),s16=i=>d.getInt16(i,true),u16=i=>d.getUint16(i,true);
  if(p.length>=13){if(r.pgn===254&&r.as===127){const x=p[10];r.f={spd:u16(5)/10,st:p[7],sp:s16(8)/100,xte:x===255?null:(x-127)*2,sc:p[11]|p[12]<<8}}
   else if(r.pgn===253){const h=s16(7),ro=s16(9);r.f={ang:s16(5)/100,hd:h===9999?null:h/10,roll:ro===8888?null:ro/10,sw:p[11],pwm:p[12]}}
   else if(r.pgn===252)r.f={kp:p[5],hi:p[6],lo:p[7],min:p[8],cnt:p[9],off:s16(10),ack:p[12]};
   else if(r.pgn===251)r.f={b0:p[5],pulse:p[6],b1:p[8]};
   else if(r.pgn===239)r.f={ut:p[5],spd:p[6]/10,hyd:p[7],tram:p[8],rl:p[11],rh:p[12]};
   else if(r.pgn===229)r.f={sec:[...p.slice(5,13)]}}
  if(r.pgn===126&&p.length>=10)r.f={ang:s16(5)/100,raw:u16(7),sw:p[9]};
  return}
 if(p[0]===36){r.k='nmea';r.txt=new TextDecoder().decode(p).trim();const f=r.txt.split(',');const ty=f[0].slice(3,6);r.ty=f[0].slice(1);
  const sod=s=>s&&s.length>=6?(+s.slice(0,2))*3600+(+s.slice(2,4))*60+parseFloat(s.slice(4)):undefined;
  if(ty==='GGA')r.f={sod:sod(f[1]),fix:+f[6],sats:+f[7],hdop:parseFloat(f[8]),age:f[13]===''?null:parseFloat(f[13])};
  else if(ty==='NDA'||ty==='OGI')r.f={sod:sod(f[1]),fix:+f[6],sats:+f[7],hdop:parseFloat(f[8]),age:parseFloat(f[10])};
  else if(ty==='RMC')r.f={sod:sod(f[1])};return}
 if(r.port===4242&&p.length>=3&&p[0]===58&&p[1]===58){r.k='hydraulic';r.f={cmd:p[2]};return}
 r.k='raw'}
function label(r){if(r.k==='pgn')return 'PGN '+r.pgn+' '+(PGNN[r.pgn]||'')+' ('+(SRCN[r.as]||'0x'+r.as.toString(16))+')';
 if(r.k==='nmea')return 'NMEA '+r.ty;if(r.k==='hydraulic')return 'Hydraulic';if(r.k==='logger')return 'Logger '+r.what;return 'raw '+r.p.length+' B'}
const f1=(v,d=1)=>v===null||v===undefined||isNaN(v)?'-':(+v).toFixed(d);
function summary(r){const f=r.f||{};
 if(r.k==='pgn'){if(!r.crc)return '<span class=bad>bad CRC</span>';
  if(r.pgn===254&&r.f)return f1(f.spd)+' km/h, '+(f.st?'<b class=ok>ON</b>':'off')+', setpoint '+f1(f.sp,2)+'°, xte '+(f.xte===null?'-':f.xte+' cm');
  if(r.pgn===253)return 'actual '+f1(f.ang,2)+'°, hdg '+f1(f.hd)+', roll '+f1(f.roll)+', |pwm| '+f.pwm+', steer sw '+((f.sw&2)?'off':'<b class=ok>on</b>');
  if(r.pgn===252)return 'Kp '+f.kp+', high '+f.hi+', low '+f.lo+', min '+f.min+', counts '+f.cnt+', offset '+f.off+', Ackerman '+f.ack+'%';
  if(r.pgn===251)return 'bits 0x'+f.b0.toString(16)+' / 0x'+f.b1.toString(16)+', pulses '+f.pulse;
  if(r.pgn===239)return 'uturn '+f.ut+', hyd '+f.hyd+', tram '+f.tram+', relays 0x'+(f.rh<<8|f.rl).toString(16);
  if(r.pgn===229)return 'sections '+f.sec.map(b=>b.toString(2).padStart(8,'0')).join(' ');
  if(r.pgn===126&&r.f)return 'angle '+f1(f.ang,2)+'°, raw WAS '+f.raw;return ''}
 if(r.k==='nmea')return '<span class=mono>'+esc(r.txt)+'</span>';if(r.k==='hydraulic')return 'cmd '+f.cmd+' ('+(HYD[f.cmd]||'?')+')';
 if(r.k==='logger')return esc(r.txt||(r.f?'RSSI '+r.f.rssi+' dBm, heap '+r.f.heap+' KB':''));return ''}
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const hex=p=>[...p].map(b=>b.toString(16).padStart(2,'0')).join(' ');

// ---------- time window ----------
function win(){const now=ST?ST.now:0;if(zoom)return zoom;return [now-range*1000,now]}
function inWin(){const [a,b]=win();let lo=0,hi=REC.length;while(lo<hi){const m=(lo+hi)>>1;if(REC[m].t<a)lo=m+1;else hi=m}const out=[];for(let i=lo;i<REC.length&&REC[i].t<=b;i++)out.push(REC[i]);return out}
function tfmt(t,sec){if(utcOff!==null){const s=((t+utcOff)/1000)%86400;const h=Math.floor(s/3600),m=Math.floor(s%3600/60),x=s%60;return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+(sec!==false?':'+String(Math.floor(x)).padStart(2,'0'):'')}
 const d=(t-(ST?ST.now:t))/1000;return (d<0?'-':'')+Math.floor(Math.abs(d)/60)+':'+String(Math.floor(Math.abs(d)%60)).padStart(2,'0')}

// ---------- charts ----------
const charts={};
function chart(id,series,o={}){charts[id]={series,o};draw(id)}
function draw(id){const C=charts[id];if(!C)return;const cv=document.getElementById(id);if(!cv||!cv.offsetParent)return;
 const dpr=window.devicePixelRatio||1,W=cv.clientWidth,H=cv.clientHeight;cv.width=W*dpr;cv.height=H*dpr;const g=cv.getContext('2d');g.scale(dpr,dpr);
 const [t0,t1]=win(),L=44,R=8,T=6,B=18,pw=W-L-R,ph=H-T-B;g.font='11px system-ui';
 let lo=Infinity,hi=-Infinity;for(const s of C.series)for(let i=0;i<s.t.length;i++){if(s.t[i]<t0||s.t[i]>t1)continue;const v=s.v[i];if(v===null||isNaN(v))continue;if(v<lo)lo=v;if(v>hi)hi=v}
 if(C.o.min!==undefined)lo=Math.min(lo,C.o.min);if(C.o.max!==undefined)hi=Math.max(hi,C.o.max);if(!isFinite(lo)){lo=0;hi=1}if(hi-lo<1e-6){hi+=1;lo-=1}const pad=(hi-lo)*.08;lo=C.o.min!==undefined&&lo>=C.o.min?Math.max(lo-pad,C.o.min):lo-pad;hi=C.o.max!==undefined&&hi<=C.o.max?Math.min(hi+pad,C.o.max):hi+pad;
 const X=t=>L+(t-t0)/(t1-t0)*pw,Y=v=>T+(1-(v-lo)/(hi-lo))*ph;C.X=X;C.t0=t0;C.t1=t1;C.L=L;C.pw=pw;
 g.strokeStyle=css('--line');g.fillStyle=css('--mute');g.lineWidth=1;
 for(let i=0;i<=4;i++){const v=lo+(hi-lo)*i/4,y=Y(v);g.beginPath();g.moveTo(L,y);g.lineTo(W-R,y);g.stroke();g.fillText(Math.abs(hi-lo)<10?v.toFixed(1):v.toFixed(0),2,y+4)}
 const nx=Math.max(2,Math.floor(pw/90));for(let i=0;i<=nx;i++){const t=t0+(t1-t0)*i/nx;g.fillText(tfmt(t),Math.max(L-14,Math.min(X(t)-22,W-50)),H-4)}
 if(C.o.shade){g.fillStyle=css('--shade');for(const [a,b] of C.o.shade){if(b<t0||a>t1)continue;g.fillRect(X(Math.max(a,t0)),T,X(Math.min(b,t1))-X(Math.max(a,t0)),ph)}}
 if(C.o.zero&&lo<0&&hi>0){g.strokeStyle=css('--mute');g.setLineDash([3,3]);g.beginPath();g.moveTo(L,Y(0));g.lineTo(W-R,Y(0));g.stroke();g.setLineDash([])}
 for(const s of C.series){g.strokeStyle=css(s.c);g.lineWidth=1.5;g.beginPath();let pen=false,lastPx=-1,mn=0,mx=0,lastY=0,lastT=-1e15;const gap=s.gap||2500;
  for(let i=0;i<s.t.length;i++){const t=s.t[i];if(t<t0)continue;if(t>t1)break;const v=s.v[i];if(v===null||isNaN(v)){pen=false;continue}
   const px=Math.round(X(t)),y=Y(v);if(t-lastT>gap){pen=false;lastPx=-1}lastT=t;
   if(px===lastPx){if(y<mn)mn=y;if(y>mx)mx=y;lastY=y;continue}
   if(lastPx>=0&&mn!==mx){g.moveTo(lastPx,mn);g.lineTo(lastPx,mx);g.moveTo(lastPx,lastY)}
   if(!pen){g.moveTo(px,y);pen=true}else{if(s.step)g.lineTo(px,lastY);g.lineTo(px,y)}lastPx=px;mn=mx=lastY=y}
  if(lastPx>=0&&mn!==mx){g.moveTo(lastPx,mn);g.lineTo(lastPx,mx)}g.stroke()}
 for(const m of marks){if(m.t<t0||m.t>t1)continue;const x=X(m.t);g.strokeStyle=css('--warn');g.beginPath();g.moveTo(x,T);g.lineTo(x,T+ph);g.stroke();g.fillStyle=css('--warn');g.fillText(m.txt.slice(0,28),Math.min(x+3,W-150),T+11)}
 if(hoverT!==null&&hoverT>=t0&&hoverT<=t1){const x=X(hoverT);g.strokeStyle=css('--ink');g.globalAlpha=.4;g.beginPath();g.moveTo(x,T);g.lineTo(x,T+ph);g.stroke();g.globalAlpha=1;
  const txt=[tfmt(hoverT)];for(const s of C.series){const i=nearest(s.t,hoverT);if(i>=0&&Math.abs(s.t[i]-hoverT)<(t1-t0)/50)txt.push((s.n||'')+' '+f1(s.v[i],s.d===undefined?2:s.d))}
  const tw=Math.max(...txt.map(z=>g.measureText(z).width))+10,bx=x+8+tw>W?x-tw-8:x+8;g.fillStyle=css('--card');g.strokeStyle=css('--line');g.fillRect(bx,T+2,tw,txt.length*14+6);g.strokeRect(bx,T+2,tw,txt.length*14+6);
  g.fillStyle=css('--ink');txt.forEach((z,i)=>g.fillText(z,bx+5,T+16+i*14))}
 if(drag&&drag.id===id){g.fillStyle='rgba(47,111,181,.15)';g.fillRect(Math.min(drag.x0,drag.x1),T,Math.abs(drag.x1-drag.x0),ph)}}
function nearest(a,t){let lo=0,hi=a.length-1;if(hi<0)return -1;while(lo<hi){const m=(lo+hi)>>1;if(a[m]<t)lo=m+1;else hi=m}if(lo>0&&Math.abs(a[lo-1]-t)<Math.abs(a[lo]-t))lo--;return lo}
let drag=null;
function hookCharts(){$$('canvas').forEach(cv=>{const id=cv.id,tOf=x=>{const C=charts[id];return C?C.t0+(x-C.L)/C.pw*(C.t1-C.t0):null};
 cv.addEventListener('mousemove',e=>{const x=e.offsetX;hoverT=tOf(x);if(drag)drag.x1=x;redraw()});
 cv.addEventListener('mouseleave',()=>{hoverT=null;drag=null;redraw()});
 cv.addEventListener('mousedown',e=>{drag={id,x0:e.offsetX,x1:e.offsetX}});
 cv.addEventListener('mouseup',e=>{if(drag&&Math.abs(drag.x1-drag.x0)>8){const a=tOf(Math.min(drag.x0,drag.x1)),b=tOf(Math.max(drag.x0,drag.x1));zoom=[a,b];live=false;setLive()}drag=null;render()});
 cv.addEventListener('dblclick',()=>{zoom=null;render()});
 cv.addEventListener('touchstart',e=>{const r=cv.getBoundingClientRect();hoverT=tOf(e.touches[0].clientX-r.left);redraw()},{passive:true})})}
function redraw(){for(const id in charts)draw(id)}
let marks=[];

// ---------- views ----------
function series(recs,pred,fn){const t=[],v=[];for(const r of recs)if(pred(r)){t.push(r.t);v.push(fn(r))}return {t,v}}
function engagedSpans(recs){const out=[];let a=null,last=null;for(const r of recs){if(r.k==='pgn'&&r.pgn===254&&r.f){const on=r.f.st===1&&r.f.spd>0;if(on&&a===null)a=r.t;if(!on&&a!==null){out.push([a,r.t]);a=null}last=r.t}}if(a!==null)out.push([a,last]);return out}
function pct(a,p){if(!a.length)return NaN;const s=[...a].sort((x,y)=>x-y);return s[Math.min(s.length-1,Math.round(p/100*(s.length-1)))]}
const rms=a=>a.length?Math.sqrt(a.reduce((s,x)=>s+x*x,0)/a.length):NaN;
function kpi(el,items){$(el).innerHTML=items.map(([h,v,s,c])=>`<div class="card kpi"><h3>${h}</h3><div class="v ${c||''}">${v}</div><div class=s>${s||''}</div></div>`).join('')}

function renderDash(recs){
 const [a,b]=win(),secs=(b-a)/1000;const bySrc={};for(const r of recs){if(r.k==='logger')continue;const key=r.ip+' '+label(r).replace(/ \(.*/,'');(bySrc[key]=bySrc[key]||[]).push(r.t)}
 const top=Object.entries(bySrc).sort((x,y)=>y[1].length-x[1].length).slice(0,5),cols=['--c1','--c2','--c3','--c4','--c5'];
 const bucket=Math.max(1000,(b-a)/120);const ser=top.map(([k,ts],i)=>{const t=[],v=[];let j=0;for(let x=a;x<b;x+=bucket){let n=0;while(j<ts.length&&ts[j]<x+bucket){if(ts[j]>=x)n++;j++}t.push(x+bucket/2);v.push(n/(bucket/1000))}return {t,v,c:cols[i],n:k.split(' ').slice(1).join(' '),d:1}});
 chart('c-rates',ser,{min:0});$('#l-rates').innerHTML=top.map(([k],i)=>`<span><i style="background:var(${cols[i]})"></i>${esc(k)}</span>`).join('');
 const rs=series(recs,r=>r.k==='logger'&&r.f&&r.f.rssi!==undefined&&r.f.rssi!==0,r=>r.f.rssi);if(ST&&ST.rssi){rs.t.push(ST.now);rs.v.push(ST.rssi)}chart('c-rssi',[Object.assign(rs,{c:'--c1',n:'dBm',d:0,gap:2e5})],{});
 const sp=engagedSpans(recs);const eng=sp.reduce((s,[x,y])=>s+(y-x),0)/1000;$('#k-eng').textContent=eng>0?(eng/60).toFixed(1)+' min':'off';$('#k-eng2').textContent='engaged in this '+Math.round(secs/60)+' min window';
 const n=recs.filter(r=>r.k!=='logger').length;$('#k-rate').textContent=(n/Math.max(secs,1)).toFixed(1)+' pkt/s';$('#k-rate2').textContent=n+' packets in window';}

function renderStatus(){if(!ST)return;const w=ST.wifi;
 $('#hstat').textContent=(w.up?'● '+w.ssid+' '+w.ip+' '+w.rssi+' dBm':'○ not connected')+(w.ap?'  | hotspot ON':'')+'  | boot '+ST.boot+', up '+Math.floor(ST.up/3600)+'h'+String(Math.floor(ST.up%3600/60)).padStart(2,'0');
 $('#k-wifi').innerHTML=w.up?`<span class=ok>${w.rssi} dBm</span>`:'<span class=bad>down</span>';$('#k-wifi2').textContent=w.up?w.ssid+' · '+w.ip:(w.ap?'hotspot AOG-Logger-Setup open':'retrying '+w.ssid);
 const L=ST.log;$('#k-log').textContent=L.total?Math.round(100*L.used/L.total)+'%':'n/a';$('#k-log2').textContent=L.total?`${L.rate} B/s · window ≈ ${L.cov>0?Math.round(L.cov)+' min':'?'}${L.ferr?' · FLASH ERRORS '+L.ferr:''}`:'no log partition';
 const P=[...ST.pgns].sort((x,y)=>y.n-x.n),mx=Math.max(1,...P.map(p=>p.n));
 $('#pgnt').innerHTML='<tr><th>PGN</th><th>from</th><th class=n>count</th><th class=n>/s</th><th style=width:35%></th></tr>'+P.map(p=>`<tr><td>${p.pgn} <span class=desc>${PGNN[p.pgn]||''}</span></td><td>${SRCN[p.src]||'0x'+p.src.toString(16)}</td><td class=n>${p.n}</td><td class=n>${(p.r/10).toFixed(1)}</td><td><div class=bar style="width:${100*p.n/mx}%"></div></td></tr>`).join('');
 $('#srct').innerHTML='<tr><th>from</th><th>port</th><th>last packet</th><th class=n>/s</th><th class=n>total</th><th class=n>ago</th></tr>'+ST.src.map(s=>`<tr><td>${s.ip}</td><td>${s.port}</td><td>${esc(s.what)}</td><td class=n>${(s.r/10).toFixed(1)}</td><td class=n>${s.n}</td><td class="n ${s.age>3?'bad':''}">${s.age<60?s.age.toFixed(1)+'s':(s.age/60).toFixed(0)+'m'}</td></tr>`).join('');
 $('#wstat').innerHTML=`Status: <b>${w.up?'connected to '+esc(w.ssid)+' ('+w.ip+', '+w.rssi+' dBm)':'not connected'}</b>${w.ap?' · hotspot <b>AOG-Logger-Setup</b> open ('+w.apClients+' client)':''}${!w.up&&w.downFor?' · trying for '+Math.round(w.downFor/60)+' min':''}`}

function renderSteer(recs){
 const sp=engagedSpans(recs);const s254=recs.filter(r=>r.k==='pgn'&&r.pgn===254&&r.f),s253=recs.filter(r=>r.k==='pgn'&&r.pgn===253&&r.f);
 const S=(arr,fn)=>({t:arr.map(r=>r.t),v:arr.map(fn)});
 chart('c-ang',[Object.assign(S(s254,r=>r.f.sp),{c:'--c2',n:'setpoint'}),Object.assign(S(s253,r=>r.f.ang),{c:'--c1',n:'actual'})],{shade:sp});
 const err=[],errT=[],pairs=[];let j=0,last=null;for(const r of s253){while(j<s254.length&&s254[j].t<=r.t){last=s254[j];j++}if(last&&r.t-last.t<=300){const e=r.f.ang-last.f.sp;errT.push(r.t);err.push(e);if(last.f.st===1&&last.f.spd>0)pairs.push({e,pwm:r.f.pwm,spd:last.f.spd,t:r.t})}}
 chart('c-err',[{t:errT,v:err,c:'--c4',n:'error'}],{shade:sp,zero:true});chart('c-pwm',[Object.assign(S(s253,r=>r.f.pwm),{c:'--c2',n:'|pwm|',d:0})],{shade:sp,min:0});
 chart('c-xte',[Object.assign(S(s254.filter(r=>r.f.xte!==null),r=>r.f.xte),{c:'--c3',n:'xte',d:0})],{shade:sp,zero:true});chart('c-spd',[Object.assign(S(s254,r=>r.f.spd),{c:'--c1',n:'km/h',d:1})],{shade:sp,min:0});
 chart('c-imu',[Object.assign(S(s253.filter(r=>r.f.hd!==null),r=>r.f.hd),{c:'--c4',n:'hdg',d:1}),Object.assign(S(s253.filter(r=>r.f.roll!==null),r=>r.f.roll*10),{c:'--c3',n:'roll x10',d:1})],{});
 const e=pairs.map(p=>p.e),ae=e.map(Math.abs),x=s254.filter(r=>r.f.st===1&&r.f.spd>0&&r.f.xte!==null).map(r=>r.f.xte);let zc=0;for(let i=1;i<e.length;i++)if((e[i]>0)!==(e[i-1]>0)&&pairs[i].t-pairs[i-1].t<500)zc++;
 const engMin=sp.reduce((s,[a,b])=>s+(b-a),0)/60000,g253=[];for(let i=1;i<s253.length;i++)g253.push(s253[i].t-s253[i-1].t);
 kpi('#steerk',[['Engaged',engMin.toFixed(1)+' min',pairs.length+' samples'],['Angle error RMS',f1(rms(e),2)+'°','p95 '+f1(pct(ae,95),2)+'° · max '+f1(ae.length?Math.max(...ae):NaN,2)+'°',rms(e)>1?'bad':'ok'],['Error mean',f1(e.length?e.reduce((s,v)=>s+v,0)/e.length:NaN,2)+'°','+ = wheels right of target'],
  ['Cross-track RMS',f1(rms(x),1)+' cm','p95 '+f1(pct(x.map(Math.abs),95),0)+' cm',rms(x)>5?'bad':'ok'],['Hunting',engMin>0?(zc/engMin).toFixed(0)+' /min':'-','error sign changes per minute'],['|PWM| at 0',pairs.length?Math.round(100*pairs.filter(p=>p.pwm===0).length/pairs.length)+'%':'-','motor idle while engaged'],
  ['PGN 253 gaps',g253.filter(g=>g>300).length,'> 300 ms (broadcast loss is normal)']]);
 const bands=[[0,4],[4,8],[8,12],[12,99]];$('#bandt').innerHTML='<tr><th>speed</th><th class=n>samples</th><th class=n>RMS error</th><th class=n>p95 |err|</th><th class=n>mean |pwm|</th></tr>'+bands.map(([a,b])=>{const B=pairs.filter(p=>p.spd>=a&&p.spd<b);return `<tr><td>${a}-${b<99?b:'+'} km/h</td><td class=n>${B.length}</td><td class=n>${f1(rms(B.map(p=>p.e)),2)}°</td><td class=n>${f1(pct(B.map(p=>Math.abs(p.e)),95),2)}°</td><td class=n>${B.length?Math.round(B.reduce((s,p)=>s+p.pwm,0)/B.length):'-'}</td></tr>`}).join('');
 const lastOf=pg=>{for(let i=REC.length-1;i>=0;i--){const r=REC[i];if(r.k==='pgn'&&r.pgn===pg&&r.f)return r}return null};
 $('#latest').innerHTML=[252,251,239,229].map(pg=>{const r=lastOf(pg);return `<tr><td>PGN ${pg} ${PGNN[pg]}</td><td>${r?summary(r):'<span class=desc>not seen</span>'}</td><td class=desc>${r?tfmt(r.t):''}</td></tr>`}).join('')}

function renderGps(recs){const g=recs.filter(r=>r.k==='nmea'&&r.f&&r.f.fix!==undefined);
 chart('c-fix',[Object.assign(series(g,()=>1,r=>r.f.fix),{c:'--c3',n:'fix',d:0,step:1})],{min:0,max:5});
 chart('c-sat',[Object.assign(series(g,()=>1,r=>r.f.sats),{c:'--c1',n:'sats',d:0}),Object.assign(series(g,()=>1,r=>r.f.hdop*10),{c:'--c2',n:'hdop x10',d:0})],{min:0});
 chart('c-age',[Object.assign(series(g,r=>r.f.age!==null&&!isNaN(r.f.age),r=>r.f.age),{c:'--c4',n:'age',d:1})],{min:0});
 const st=series(recs,r=>r.k==='logger'&&r.f&&r.f.bytes,r=>r.f.bytes[2]/60);chart('c-rtcm',[Object.assign(st,{c:'--c5',n:'B/s',d:0,gap:2e5})],{min:0});
 const l=g[g.length-1],fixN={0:'none',1:'GPS',2:'DGPS',4:'RTK fix',5:'RTK float'};
 kpi('#gpsk',l?[['Fix',fixN[l.f.fix]||l.f.fix,l.ty,l.f.fix===4?'ok':'bad'],['Satellites',l.f.sats,''],['HDOP',f1(l.f.hdop,2),''],['Correction age',f1(l.f.age,1)+' s','']]:[['GPS','no NMEA','enable NMEA in Settings or check the GPS']]);
 const n=recs.filter(r=>r.k==='nmea').slice(-12).reverse();$('#nmeat').innerHTML=n.map(r=>`<tr><td class=desc>${tfmt(r.t)}</td><td>${r.ip}</td><td class=mono>${esc(r.txt)}</td></tr>`).join('')||'<tr><td class=desc>no NMEA in window</td></tr>'}

let openRow=null;
function renderPk(recs){const fk=$('#f-kind').value,fs=$('#f-src').value,fp=$('#f-pgn').value,ft=$('#f-txt').value.toLowerCase();
 const srcs=new Set(),pgns=new Set();for(const r of recs){if(r.ip)srcs.add(r.ip+':'+r.port);if(r.k==='pgn')pgns.add(r.pgn)}
 const fill=(sel,vals,cur,fmt)=>{const h='<option value="">'+sel.options[0].text+'</option>'+[...vals].sort((a,b)=>a>b?1:-1).map(v=>`<option value="${v}" ${String(v)===cur?'selected':''}>${fmt(v)}</option>`).join('');if(sel.innerHTML!==h)sel.innerHTML=h};
 fill($('#f-src'),srcs,fs,v=>v);fill($('#f-pgn'),pgns,fp,v=>v+' '+(PGNN[v]||''));
 const sel=recs.filter(r=>(!fk||r.k===fk)&&(!fs||(r.ip+':'+r.port)===fs)&&(!fp||String(r.pgn)===fp)&&(!ft||hex(r.p).includes(ft)||(r.txt||'').toLowerCase().includes(ft)));
 const rows=sel.slice(-300).reverse();$('#pkn').textContent=sel.length+' match, showing newest '+rows.length;
 $('#pkb').innerHTML=rows.map((r,i)=>{const id=r.t+'_'+r.src+'_'+i;const d=openRow===r.t+'_'+r.src?`<tr class=d><td colspan=5 class=mono>${bytes(r)}</td></tr>`:'';
  return `<tr data-k="${r.t}_${r.src}"><td class=desc>${tfmt(r.t)}</td><td>${r.ip||'logger'}</td><td>${r.port||''}</td><td>${esc(label(r))}${r.trunc?' <span class=pill>truncated</span>':''}</td><td>${summary(r)}</td></tr>`+d}).join('')}
function bytes(r){const names=r.k==='pgn'?{0:'hdr',1:'hdr',2:'src',3:'pgn',4:'len'}:{};return [...r.p].map((b,i)=>`<span title="byte ${i}${names[i]?' '+names[i]:''}">${b.toString(16).padStart(2,'0')}</span>`).join(' ')+(r.k==='pgn'?`<br><span class=desc>CRC ${r.crc?'ok':'BAD'} · ${r.p.length} bytes</span>`:'')}
$('#pkb').addEventListener('click',e=>{const tr=e.target.closest('tr[data-k]');if(!tr)return;openRow=openRow===tr.dataset.k?null:tr.dataset.k;render()});
['#f-kind','#f-src','#f-pgn'].forEach(s=>$(s).addEventListener('change',render));$('#f-txt').addEventListener('input',render);

function render(){renderStatus();const recs=inWin();marks=recs.filter(r=>r.ch===0xF1).map(r=>({t:r.t,txt:r.txt}));
 $('#zoominfo').textContent=zoom?'zoomed '+tfmt(zoom[0])+' – '+tfmt(zoom[1])+' (double-click a chart to reset)':'drag on a chart to zoom, double-click to reset';
 if(tab==='dash')renderDash(recs);if(tab==='steer')renderSteer(recs);if(tab==='gps')renderGps(recs);if(tab==='pk')renderPk(recs);redraw()}

// ---------- data loading ----------
async function getJSON(u){const r=await fetch(u);if(!r.ok)throw new Error(u+' '+r.status);return r.json()}
async function loadLog(q){const r=await fetch('/log.bin?'+q);if(!r.ok)return;parseLog(await r.arrayBuffer())}
let loadedMin=0;
async function tick(){try{ST=await getJSON('/api/status');if(ST.wifi.ip&&ST.wifi.ip!=='0.0.0.0')defSub=ST.wifi.ip.replace(/\.\d+$/,'');if(ST.boot!==curBoot){curBoot=ST.boot;SEC.clear();maxSeq=0;loadedMin=0}
  const need=Math.ceil(range/60)+1;if(loadedMin<need){await loadLog('minutes='+need);loadedMin=need}else if(live)await loadLog('fromseq='+maxSeq);
  const cut=maxSeq-700;for(const k of SEC.keys())if(k<cut)SEC.delete(k);rebuild();render()}catch(e){$('#hstat').textContent='connection lost — retrying'}}
function setLive(){$('#livebtn').classList.toggle('on',live);$('#livebtn').textContent=live?'● Live':'Paused'}
$('#livebtn').onclick=()=>{live=!live;if(live)zoom=null;setLive();render()};
$$('#rangebar [data-r]').forEach(b=>b.onclick=()=>{range=+b.dataset.r;zoom=null;$$('#rangebar [data-r]').forEach(x=>x.classList.toggle('on',x===b));tick()});
$$('#nav button').forEach(b=>b.onclick=()=>{tab=b.dataset.t;$$('#nav button').forEach(x=>x.classList.toggle('on',x===b));$$('section').forEach(s=>s.classList.toggle('on',s.id==='t-'+tab));$('#rangebar').style.display=tab==='set'?'none':'flex';if(tab==='set')loadCfg();render()});
window.addEventListener('resize',redraw);

// ---------- settings ----------
async function post(u,data){const b=new URLSearchParams(data||{});const r=await fetch(u,{method:'POST',body:b});return r.text()}
async function loadCfg(){try{const c=await getJSON('/api/cfg');$('#s-ssid').value=c.ssid;$('#s-ip').value=c.ip;$('#s-dhcp').checked=!!c.dhcp;$('#s-nmea').value=c.nmea;$('#s-open').checked=false}catch(e){}}
async function scan(){$('#scanst').textContent='scanning...';for(let i=0;i<20;i++){const d=await getJSON('/api/scan');if(d.nets){$('#scanst').textContent=d.nets.length+' networks (tap one)';
  $('#nets').innerHTML=d.nets.sort((a,b)=>b.rssi-a.rssi).map(n=>`<tr class=net data-s="${esc(n.ssid)}"><td>${esc(n.ssid||'(hidden)')}</td><td class=n>${n.rssi} dBm</td><td>${n.open?'open':'🔒'}</td></tr>`).join('');
  $$('#nets tr').forEach(tr=>tr.onclick=()=>{$('#s-ssid').value=tr.dataset.s;$('#s-pass').focus()});return}await new Promise(r=>setTimeout(r,700))}$('#scanst').textContent='scan timed out'}
async function saveWifi(){const ssid=$('#s-ssid').value.trim();if(!ssid){alert('Enter a network name');return}
 if(!confirm('Save WiFi settings and reboot the logger?'))return;
 await post('/api/savecfg',{ssid,pass:$('#s-pass').value,open:$('#s-open').checked?1:0,dhcp:$('#s-dhcp').checked?1:0,ip:$('#s-ip').value,reboot:1});
 alert('Saved. The logger reboots and joins "'+ssid+'". Reconnect your device to that network.')}
async function saveLog(){await post('/api/savecfg',{nmea:$('#s-nmea').value});alert('Saved')}
async function mark(){const t=$('#mk').value.trim();if(!t)return;await post('/api/mark',{text:t});$('#mk').value='';tick()}
async function clr(){if(confirm('Clear the whole log? This cannot be undone.')){await post('/api/clear',{confirm:'yes'});SEC.clear();maxSeq=0;loadedMin=0;tick()}}

hookCharts();setLive();tick();setInterval(()=>{if(live||!ST)tick();else{getJSON('/api/status').then(s=>{ST=s;renderStatus()}).catch(()=>{})}},2000);
</script></body></html>)rawliteral";
