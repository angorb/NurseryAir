#pragma once
#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Nursery Air</title>
<style>
:root{--bg:#f6f7f9;--card:#fff;--fg:#1c2330;--mut:#6b7686;--line:#e2e6ec;--acc:#2f6fed;--bad:#d64545;--ok:#2e9e6a}
@media(prefers-color-scheme:dark){:root{--bg:#10141b;--card:#181e28;--fg:#e6eaf0;--mut:#8e99a9;--line:#2a3342;--acc:#6c9bff;--bad:#ff6b6b;--ok:#4cc38a}}
*{box-sizing:border-box}
body{margin:0;font:15px system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:760px;margin:0 auto;padding:12px}
h1{font-size:18px;margin:8px 0}h2{font-size:15px;margin:0 0 8px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px;margin-bottom:12px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(105px,1fr));gap:8px}
.k{color:var(--mut);font-size:12px}.v{font-size:20px;font-weight:600}
#badge{display:inline-block;padding:4px 10px;border-radius:99px;font-weight:600;color:#fff;background:var(--mut)}
#badge.ok{background:var(--ok)}#badge.bad{background:var(--bad)}
canvas{width:100%;height:220px;display:block}
label{display:block;margin:8px 0 2px;font-size:13px}
.hint{color:var(--mut);font-size:12px}
input[type=number],input[type=text]{width:100%;padding:8px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit}
button{padding:9px 14px;border:0;border-radius:8px;background:var(--acc);color:#fff;font:inherit;font-weight:600;margin:6px 6px 0 0}
button.alt{background:var(--card);color:var(--fg);border:1px solid var(--line)}
ul{list-style:none;margin:0;padding:0}li{padding:5px 0;border-bottom:1px solid var(--line)}
#msg{margin-left:6px;color:var(--mut);font-size:13px}
</style></head><body><main>
<h1>Nursery Air <span id="badge">...</span></h1>

<div class="card"><div class="grid">
<div><div class="k">Temperature (tap)</div><div class="v" id="temp">-</div></div>
<div><div class="k">Humidity</div><div class="v" id="hum">-</div></div>
<div><div class="k">Gas vs baseline</div><div class="v" id="ratio">-</div></div>
<div><div class="k">Gas resistance</div><div class="v" id="gas">-</div></div>
</div><div class="hint" id="meta" style="margin-top:8px"></div></div>

<div class="card"><h2>Last 12 hours</h2>
<canvas id="chart"></canvas>
<div class="hint">Blue: gas resistance (kΩ). Grey: baseline. Dashed: alert threshold. Red lines: alerts. Green lines: diaper changes you marked.</div>
<button id="mark">Mark diaper change</button>
</div>

<div class="card"><h2>Recent events</h2><ul id="events"><li class="hint">None yet</li></ul></div>

<div class="card"><h2>Settings</h2>
<label>Alert threshold (fraction of baseline)</label>
<input type="number" id="trigger" step="0.01" min="0.2" max="0.95">
<div class="hint">Lower = less sensitive. Default 0.60.</div>
<label>Re-arm level</label>
<input type="number" id="clear" step="0.01" min="0.3" max="1">
<label>Samples below threshold before alert (10 s each)</label>
<input type="number" id="count" step="1" min="1" max="60">
<label>Cooldown between alerts (minutes)</label>
<input type="number" id="cooldown" step="1" min="1" max="1440">
<label>Warm-up after boot (minutes)</label>
<input type="number" id="warmup" step="1" min="5" max="1440">
<label><input type="checkbox" id="notify"> Send ntfy notifications</label>
<label>ntfy URL</label>
<input type="text" id="ntfy" maxlength="127">
<button id="save">Save</button>
<button class="alt" id="test">Send test alert</button>
<button class="alt" id="reset">Reset baseline</button>
<span id="msg"></span></div>
</main>
<script>
const $=id=>document.getElementById(id);
let H=null,S=null,F=localStorage.getItem('f')==='1';
async function api(u,o){const r=await fetch(u,o);let j={};try{j=await r.json()}catch(e){}
 if(!r.ok)throw new Error(j.error||r.status);return j}
function ago(s){return s<90?s+' s ago':s<5400?Math.round(s/60)+' min ago':(s/3600).toFixed(1)+' h ago'}
function say(t){$('msg').textContent=t;setTimeout(()=>$('msg').textContent='',4000)}

async function loadStatus(){
 try{S=await api('/api/status');
  $('temp').textContent=F?(S.temp*9/5+32).toFixed(1)+' °F':S.temp.toFixed(1)+' °C';
  $('hum').textContent=S.hum.toFixed(0)+' %';
  $('gas').textContent=(S.gas/1000).toFixed(0)+' kΩ';
  $('ratio').textContent=S.base>0?(S.ratio*100).toFixed(0)+' %':'-';
  const b=$('badge');
  if(S.warm>0){b.textContent='Learning ('+Math.ceil(S.warm/60)+' min)';b.className=''}
  else if(S.alert){b.textContent='Odor detected';b.className='bad'}
  else{b.textContent='Normal';b.className='ok'}
  $('meta').textContent='Up '+(S.up/3600).toFixed(1)+' h · WiFi '+S.rssi+' dBm · heap '+Math.round(S.heap/1024)+' kB';
 }catch(e){$('badge').textContent='Offline';$('badge').className='bad'}
}
async function loadHist(){try{H=await api('/api/history');draw();list()}catch(e){}}
function list(){
 const u=$('events');if(!H||!H.events.length){u.innerHTML='<li class="hint">None yet</li>';return}
 u.innerHTML=H.events.slice().reverse().map(e=>'<li>'+(e[1]?'🟢 Diaper change (marked)':'🔴 Odor alert')+' · '+ago(e[0])+'</li>').join('')
}
function draw(){
 const c=$('chart'),d=devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;
 c.width=w*d;c.height=h*d;const x=c.getContext('2d');x.scale(d,d);
 const cs=getComputedStyle(document.documentElement),mut=cs.getPropertyValue('--mut').trim(),
  acc=cs.getPropertyValue('--acc').trim(),bad=cs.getPropertyValue('--bad').trim(),
  ok=cs.getPropertyValue('--ok').trim(),ln=cs.getPropertyValue('--line').trim();
 x.font='11px system-ui';x.fillStyle=mut;
 if(!H||H.n<2){x.fillText('Collecting data...',10,20);return}
 const win=Math.max(H.n*H.period,3600),L=40,B=18,T=6,pw=w-L-6,ph=h-B-T;
 const all=H.gas.concat(H.base);let mn=Math.min(...all),mx=Math.max(...all);
 const pad=(mx-mn)*.1||1;mn-=pad;mx+=pad;
 const X=a=>L+pw-a/win*pw,Y=v=>T+ph-(v-mn)/(mx-mn)*ph;
 x.strokeStyle=ln;x.lineWidth=1;
 for(let i=0;i<=4;i++){const v=mn+(mx-mn)*i/4,y=Y(v);
  x.beginPath();x.moveTo(L,y);x.lineTo(L+pw,y);x.stroke();
  x.fillText((v/1000).toFixed(0),2,y+4)}
 x.fillText('now',L+pw-18,h-4);x.fillText('-'+(win/3600).toFixed(0)+'h',L,h-4);
 const age=i=>(H.n-1-i)*H.period;
 const line=(arr,col,wd)=>{x.beginPath();x.strokeStyle=col;x.lineWidth=wd;
  arr.forEach((v,i)=>{const px=X(age(i)),py=Y(v);i?x.lineTo(px,py):x.moveTo(px,py)});x.stroke()};
 line(H.base,mut,1.5);line(H.gas,acc,2);
 if(S){const t=H.base[H.n-1]*(C?C.trigger:0.6);x.setLineDash([5,4]);
  x.beginPath();x.strokeStyle=bad;x.lineWidth=1;x.moveTo(L,Y(t));x.lineTo(L+pw,Y(t));x.stroke();x.setLineDash([])}
 H.events.forEach(e=>{if(e[0]>win)return;const px=X(e[0]);
  x.strokeStyle=e[1]?ok:bad;x.lineWidth=2;x.beginPath();x.moveTo(px,T);x.lineTo(px,T+ph);x.stroke()})
}
let C=null;
async function loadCfg(){try{C=await api('/api/config');
 for(const k of['trigger','clear','count','cooldown','warmup','ntfy'])$(k).value=C[k];
 $('notify').checked=C.notify}catch(e){}}
$('save').onclick=async()=>{
 const p=new URLSearchParams();
 for(const k of['trigger','clear','count','cooldown','warmup','ntfy'])p.set(k,$(k).value);
 p.set('notify',$('notify').checked?'1':'0');
 try{await api('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});say('Saved');loadCfg()}
 catch(e){say('Error: '+e.message)}};
$('mark').onclick=async()=>{try{await api('/api/mark',{method:'POST'});say('Marked');loadHist()}catch(e){say('Error')}};
$('test').onclick=async()=>{say('Sending...');try{await api('/api/test',{method:'POST'});say('Sent')}catch(e){say('Failed: '+e.message)}};
$('reset').onclick=async()=>{if(!confirm('Reset baseline to current reading?'))return;
 try{await api('/api/reset',{method:'POST'});say('Baseline reset');loadStatus();loadHist()}catch(e){say('Error')}};
$('temp').parentNode.onclick=()=>{F=!F;localStorage.setItem('f',F?'1':'0');loadStatus()};
addEventListener('resize',draw);
(async()=>{await loadCfg();await loadStatus();await loadHist();
 setInterval(loadStatus,5000);setInterval(loadHist,30000)})();
</script></body></html>)rawliteral";
