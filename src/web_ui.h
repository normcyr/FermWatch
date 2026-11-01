#pragma once

#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FermWatch</title>
<style>
:root{--bg:#14161a;--card:#1e2127;--line:#2e333b;--fg:#e8eaed;--mut:#9aa0a8;--acc:#ff9f1c}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px system-ui,sans-serif}
main{max-width:560px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:8px 0 0}
.sub{color:var(--mut);margin:2px 0 12px}
.badge{display:inline-block;padding:2px 10px;border-radius:12px;font-size:13px;background:#3a2a2a;color:#ff7b72}
.badge.ok{background:#1f3a2a;color:#56d364}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin:12px 0}
h2{font-size:15px;margin:0 0 10px}
label{display:block;margin:8px 0 3px;color:var(--mut);font-size:13px}
input[type=text],input[type=password],input[type=number],select{width:100%;padding:8px;border-radius:6px;border:1px solid var(--line);background:#111317;color:var(--fg)}
.row{display:flex;gap:8px;align-items:center}
.row>*{flex:1}
.chk{display:flex;align-items:center;gap:8px;margin:6px 0;color:var(--fg);font-size:15px}
input[type=range]{width:100%;accent-color:var(--acc)}
.ends{display:flex;justify-content:space-between;color:var(--mut);font-size:12px}
.val{color:var(--acc);font-weight:600}
.led{background:#000;border:2px solid #333;border-radius:6px;padding:14px 10px;color:#ff3b30;font:700 22px ui-monospace,Menlo,monospace;letter-spacing:2px;text-shadow:0 0 6px #f00;text-align:center;min-height:56px;overflow:hidden;white-space:nowrap}
.cap{color:var(--mut);font-size:12px;margin-top:6px}
button{width:100%;padding:11px;border:0;border-radius:8px;background:var(--acc);color:#111;font-weight:700;font-size:15px;cursor:pointer;margin-top:6px}
button.sec{background:#2e333b;color:var(--fg)}
details summary{cursor:pointer;color:var(--mut)}
#msg{text-align:center;color:var(--mut);min-height:20px}
</style></head><body><main>
<h1>FermWatch</h1>
<p class="sub">Display configuration - ESP8266 - <span id="conn" class="badge">Offline</span></p>

<section><h2>Sensors</h2>
<div id="sensors" class="cap">No sensor received yet.</div></section>

<section><h2>Information to display</h2>
<label class="chk"><input type="checkbox" id="showName"> Sensor name</label>
<label class="chk"><input type="checkbox" id="showSg"> Specific gravity (SG)</label>
<label class="chk"><input type="checkbox" id="showTemp"> Temperature (&deg;C)</label>
<label class="chk"><input type="checkbox" id="showTrend"> Gravity trend over 24 h</label></section>

<section><h2>Units</h2>
<label>Temperature</label><select id="tempUnit"><option value="C">&deg;C - Celsius</option><option value="F">&deg;F - Fahrenheit</option><option value="K">K - Kelvin</option><option value="R">&deg;R - Rankine</option></select>
<label>Gravity</label><select id="gravUnit"><option value="SG">SG - Specific gravity</option><option value="P">&deg;P - Plato</option></select>
<div class="cap">Incoming MQTT data is always in &deg;C and SG.</div></section>

<section><h2>Page duration: <span class="val" id="pageV"></span></h2>
<input type="range" id="pageSeconds" min="1" max="30"><div class="ends"><span>2 s</span><span>15 s</span><span>30 s</span></div>
<h2 style="margin-top:14px">Scroll speed: <span class="val" id="scrollV"></span></h2>
<input type="range" id="scrollMs" min="10" max="100"><div class="ends"><span>Fast</span><span>Slow</span></div>
<h2 style="margin-top:14px">Brightness: <span class="val" id="briV"></span></h2>
<input type="range" id="brightness" min="0" max="15"></section>

<section><h2>Display preview</h2>
<div class="led" id="led">--</div>
<div class="cap" id="ledCap"></div></section>

<section><details><summary>Advanced settings (topic, MQTT, Wi-Fi)</summary>
<label>Sensor topic base (subscribes to base/+)</label><input type="text" id="topicBase">
<label>Stale data timeout, shown with "!" (minutes)</label><input type="number" id="staleMinutes" min="1" max="1440">
<label>Remove silent sensors after (minutes)</label><input type="number" id="expireMinutes" min="1" max="10080">
<label>Broker MQTT</label><div class="row"><input type="text" id="mqttHost" placeholder="192.168.0.204"><input type="number" id="mqttPort" style="max-width:100px"></div>
<label>MQTT user</label><input type="text" id="mqttUser" autocomplete="off">
<label>MQTT password (empty = unchanged)</label><input type="password" id="mqttPass" autocomplete="new-password">
<label>Web password (8-64 characters; empty = unchanged)</label><input type="password" id="webPassword" minlength="8" maxlength="64" autocomplete="new-password">
<label>Wi-Fi network (SSID)</label><input type="text" id="ssid" autocomplete="off">
<label>Wi-Fi password (empty = unchanged)</label><input type="password" id="password" autocomplete="new-password">
<label>Device name</label><input type="text" id="deviceName">
</details></section>

<section><details><summary>Firmware update</summary>
<label>Firmware file (firmware.bin built for the ESP8266)</label>
<input type="file" id="fw" accept=".bin">
<button class="sec" id="upd">Upload firmware</button>
<div class="cap" id="updMsg">The device restarts after a successful update. Requires a Web password.</div>
</details></section>

<button id="save">Save</button>
<p id="msg"></p>
<p class="cap" id="foot" style="text-align:center"></p>
</main>
<script>
const $=id=>document.getElementById(id);
let live={},pageIdx=0,timer=null;
const F=['showName','showSg','showTemp','showTrend'];
const N=['pageSeconds','scrollMs','brightness','staleMinutes','expireMinutes','mqttPort'];
const T=['tempUnit','gravUnit','topicBase','mqttHost','mqttUser','ssid','deviceName'];
const SAMPLE=[{name:'Tilt Yellow',sg:1.012,temp:20.4,trend:-0.004}];

function plato(sg){return -616.868+1111.14*sg-630.272*sg*sg+135.997*sg*sg*sg}
function tempTxt(c){
  const u=$('tempUnit').value;
  if(u=='F')return fmt(c*9/5+32,1)+'\u00b0F';
  if(u=='K')return fmt(c+273.15,1)+'K';
  if(u=='R')return fmt((c+273.15)*9/5,1)+'\u00b0R';
  return fmt(c,1)+'\u00b0C';
}
function fmt(v,d,sign){return v==null?'--':(sign&&v>=0?'+':'')+v.toFixed(d)}
function pages(){
  const real=live.sensors&&live.sensors.length;
  const list=real?live.sensors:SAMPLE,out=[];
  list.forEach(s=>{
    const f=s.stale?'!':'';
    if($('showName').checked)out.push(s.name.toUpperCase());
    const pl=$('gravUnit').value=='P';
    if($('showSg').checked&&s.sg!=null)out.push(f+(pl?fmt(plato(s.sg),1)+'\u00b0P':'SG '+fmt(s.sg,3)));
    if($('showTemp').checked&&s.temp!=null)out.push(f+'T '+tempTxt(s.temp));
    if($('showTrend').checked&&s.trend!=null){
      if(!pl)out.push(f+fmt(s.trend,4,true)+'/d');
      else if(s.sg!=null)out.push(f+fmt(plato(s.sg)-plato(s.sg-s.trend),1,true)+'\u00b0P/d');
    }
  });
  return out.length?out:['--'];
}
function renderPreview(){
  const p=pages();pageIdx%=p.length;$('led').textContent=p[pageIdx];
  const n=live.sensors?live.sensors.length:0;
  $('ledCap').textContent=n?'LIVE DATA - '+n+' SENSOR'+(n>1?'S':''):'PREVIEW (sample values)';
  $('sensors').innerHTML=n?live.sensors.map(s=>'<div>'+s.name.replace(/[<&]/g,'')+' - SG '+fmt(s.sg,3)+' - '+fmt(s.temp,1)+'&deg;C - '+fmt(s.trend,4,true)+'/d - '+Math.round(s.age/60)+' min ago'+(s.stale?' (stale)':'')+'</div>').join(''):'No sensor received yet.';
}
function labels(){
  $('pageV').textContent=$('pageSeconds').value+' s';
  $('scrollV').textContent=$('scrollMs').value+' ms/pixel';
  $('briV').textContent=$('brightness').value+' / 15';
}
function cycle(){clearInterval(timer);timer=setInterval(()=>{pageIdx++;renderPreview()},$('pageSeconds').value*1000)}
function fill(c){
  F.forEach(k=>$(k).checked=c[k]);
  N.forEach(k=>$(k).value=c[k]);
  T.forEach(k=>$(k).value=c[k]||'');
  labels();renderPreview();cycle();
}
function collect(){
  const o={};
  F.forEach(k=>o[k]=$(k).checked);
  N.forEach(k=>o[k]=parseInt($(k).value)||0);
  T.forEach(k=>o[k]=$(k).value);
  ['mqttPass','password','webPassword'].forEach(k=>o[k]=$(k).value);
  return o;
}
async function poll(){
  try{
    live=await (await fetch('/api/state')).json();
    $('conn').textContent=live.mqtt?'Connected':'MQTT disconnected';
    $('conn').className='badge'+(live.mqtt?' ok':'');
    $('foot').innerHTML='v'+live.version+' - '+live.ip+' ('+live.host.replace(/[<&]/g,'')+'.local) - RSSI '+live.rssi+' dBm - <a href="https://github.com/normcyr/FermWatch" style="color:inherit">GitHub</a>';
  }catch(e){$('conn').textContent='Offline';$('conn').className='badge'}
  renderPreview();
}
async function load(){
  const r=await (await fetch('/api/config')).json();
  fill(r.config);
  await poll();setInterval(poll,5000);
}
document.addEventListener('input',e=>{
  labels();renderPreview();
  if(e.target.id=='pageSeconds')cycle();
});
$('save').onclick=async()=>{
  $('msg').textContent='Saving...';
  try{
    const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(collect())});
    const j=await r.json();
    $('msg').textContent=r.ok&&j.ok?(j.restart?'Saved. Restarting...':'Configuration saved.'):'Error: '+(j.error||r.status);
    $('mqttPass').value='';$('password').value='';$('webPassword').value='';
  }catch(e){$('msg').textContent='Communication error'}
};
$('upd').onclick=async()=>{
  const f=$('fw').files[0];
  if(!f){$('updMsg').textContent='Choose a .bin file first.';return}
  $('updMsg').textContent='Uploading...';
  try{
    const fd=new FormData();fd.append('firmware',f);
    const r=await fetch('/update',{method:'POST',body:fd});
    const j=await r.json();
    $('updMsg').textContent=r.ok&&j.ok?'Updated. Restarting...':'Error: '+(j.error||r.status);
  }catch(e){$('updMsg').textContent='Communication error'}
};
load();
</script></body></html>)HTML";
