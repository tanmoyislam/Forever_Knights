// Default web pages. At start-up the firmware copies these into LittleFS
// (/setup.html and /index.html) and the web server serves them FROM LittleFS.
// Edit here, re-flash, and the files in flash are refreshed automatically.
#pragma once
#include <Arduino.h>

static const char SETUP_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmartClass setup</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#0f172a;color:#e2e8f0}
main{max-width:460px;margin:0 auto;padding:18px}
h1{font-size:20px;margin:6px 0 14px}
label{display:block;font-size:13px;margin:12px 0 4px;color:#94a3b8}
input{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #334155;background:#1e293b;color:#e2e8f0;font-size:15px}
button{padding:10px 14px;border:0;border-radius:8px;background:#2563eb;color:#fff;font-size:15px}
button.sec{background:#334155}
#nets div{display:flex;justify-content:space-between;padding:9px 10px;border-bottom:1px solid #1e293b;cursor:pointer}
#nets div:hover{background:#1e293b}
#nets{border:1px solid #334155;border-radius:8px;max-height:210px;overflow:auto}
.msg{padding:10px;border-radius:8px;margin:12px 0;display:none}
#err{background:#7f1d1d;color:#fecaca}
#info{background:#1e3a8a;color:#bfdbfe}
#ok{background:#14532d;color:#bbf7d0}
small{color:#64748b}
</style></head><body><main>
<h1>SmartClass device setup</h1>
<div id="err" class="msg"></div><div id="info" class="msg"></div><div id="ok" class="msg"></div>
<div style="display:flex;justify-content:space-between;align-items:center">
<label style="margin:0">Nearby networks</label><button class="sec" type="button" onclick="scan(1)">Rescan</button></div>
<div id="nets" style="margin-top:6px"><div><span>Scanning...</span></div></div>
<form id="f">
<label>Network name (SSID)</label><input name="ssid" id="ssid" maxlength="32" required>
<label>Wi-Fi password</label><input name="pass" type="password" maxlength="64">
<label>Room ID</label><input name="room" id="room" maxlength="32" required>
<label>Device token</label><input name="token" id="token" type="password" maxlength="128">
<small id="tokhint"></small>
<label>Server URL (https)</label><input name="server" id="server" maxlength="120" required>
<p><button type="submit" id="go">Save and connect</button></p>
</form>
</main><script>
var filled=false,E=function(i){return document.getElementById(i)};
function show(id,t){['err','info','ok'].forEach(function(k){E(k).style.display=(k==id&&t)?'block':'none'});if(t)E(id).textContent=t}
function scan(force){fetch('/api/scan'+(force?'?force=1':'')).then(function(r){return r.json()}).then(function(j){
 var n=E('nets');n.innerHTML='';
 if(!j.nets.length){n.innerHTML='<div><span>'+(j.scanning?'Scanning...':'No networks found')+'</span></div>'}
 j.nets.forEach(function(w){var d=document.createElement('div'),a=document.createElement('span'),b=document.createElement('span');
  a.textContent=w.ssid+(w.lock?' 🔒':'');b.textContent=w.rssi+' dBm';d.appendChild(a);d.appendChild(b);
  d.onclick=function(){E('ssid').value=w.ssid};n.appendChild(d)});
 if(j.scanning)setTimeout(scan,1500)}).catch(function(){setTimeout(scan,3000)})}
function poll(){fetch('/api/setup').then(function(r){return r.json()}).then(function(j){
 if(!filled){filled=true;E('room').value=j.room;E('server').value=j.server;
  E('tokhint').textContent=j.hasToken?'A token is saved. Leave blank to keep it.':''}
 E('go').disabled=(j.state!='idle');
 if(j.state=='connecting')show('info','Connecting to '+j.ssid+' ... (up to 12 s)');
 else if(j.state=='connected')show('ok','Connected. IP '+j.ip+' - open http://'+j.mdns+'.local or http://'+j.ip+' on the same network. This access point closes in a few seconds.');
 else if(j.error)show('err','Setup error: '+j.error);
 }).catch(function(){})}
E('f').onsubmit=function(e){e.preventDefault();show('info','Saving...');
 fetch('/save',{method:'POST',body:new URLSearchParams(new FormData(E('f')))}).then(function(r){return r.json()})
 .then(function(j){if(!j.ok)show('err',j.error)}).catch(function(){})};
scan();poll();setInterval(poll,1000);
</script></body></html>
)HTML";

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmartClass status</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#0f172a;color:#e2e8f0}
main{max-width:720px;margin:0 auto;padding:18px}
h1{font-size:20px;margin:6px 0 4px}
#sub{color:#94a3b8;font-size:13px;margin-bottom:14px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}
.c{background:#1e293b;border-radius:10px;padding:12px}
.k{font-size:12px;color:#94a3b8}
.v{font-size:20px;margin-top:4px;word-break:break-all}
.pill{display:inline-block;padding:3px 10px;border-radius:99px;font-size:13px;background:#334155}
.LIVE{background:#166534}.OFFLINE,.AUTH{background:#991b1b}.WIFI{background:#92400e}
#stale{color:#fca5a5;font-size:13px;display:none}
</style></head><body><main>
<h1>SmartClass - Room <span id="room">-</span></h1>
<div id="sub"><span id="conn" class="pill">-</span> &nbsp; <span id="time">-</span> (Asia/Dhaka) <span id="stale">- no reply from device</span></div>
<div class="grid">
<div class="c"><div class="k">Temperature</div><div class="v" id="temp">-</div></div>
<div class="c"><div class="k">Humidity</div><div class="v" id="rh">-</div></div>
<div class="c"><div class="k">Status</div><div class="v" id="status">-</div></div>
<div class="c"><div class="k">Flame</div><div class="v" id="flame">-</div></div>
<div class="c"><div class="k">Door</div><div class="v" id="door">-</div></div>
<div class="c"><div class="k">Last card UID</div><div class="v" id="uid">-</div></div>
<div class="c"><div class="k">Wi-Fi signal</div><div class="v" id="rssi">-</div></div>
<div class="c"><div class="k">Chip temperature</div><div class="v" id="chip">-</div></div>
<div class="c"><div class="k">Firmware</div><div class="v" id="fw">-</div></div>
<div class="c"><div class="k">Uptime</div><div class="v" id="up">-</div></div>
</div></main><script>
var last=0,E=function(i){return document.getElementById(i)};
function tick(){fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){
 last=Date.now();
 E('room').textContent=j.room;E('time').textContent=j.time;
 E('temp').textContent=j.temp===null?'-':j.temp.toFixed(1)+' °C';
 E('rh').textContent=j.rh===null?'-':j.rh+' %';
 E('status').textContent=j.status;E('flame').textContent=j.flame?'FLAME':'No flame';
 E('door').textContent=j.door;E('uid').textContent=j.uid||'-';
 E('rssi').textContent=j.rssi===null?'-':j.rssi+' dBm';
 E('chip').textContent=j.chip.toFixed(1)+' °C';E('fw').textContent=j.fw;E('up').textContent=j.up+' s';
 var c=E('conn');c.textContent=j.conn;c.className='pill '+j.conn.split(' ')[0];
 }).catch(function(){})}
setInterval(function(){E('stale').style.display=(Date.now()-last>4000)?'inline':'none'},1000);
tick();setInterval(tick,1000);
</script></body></html>
)HTML";
