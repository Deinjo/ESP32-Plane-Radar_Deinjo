#pragma once

namespace web::display {

constexpr char kPage[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Plane Radar Display</title><style>
:root{color-scheme:dark;--bg:#0d151e;--surface:#141f2a;--line:#2a3a49;--text:#d7e0e9;--muted:#9aaabd;--accent:#8eb5c5}
*{box-sizing:border-box}body{margin:0;padding:20px;background:var(--bg);color:var(--text);font:15px/1.45 Segoe UI,Arial,sans-serif}
main{max-width:1100px;margin:auto}header{display:flex;align-items:center;justify-content:space-between;gap:16px;flex-wrap:wrap;margin-bottom:20px}
h1{font-size:1.35rem;margin:0}h2{font-size:1.05rem;margin:0 0 8px}a{color:var(--accent)}
.home{padding:8px 13px;border:1px solid #5e8191;border-radius:6px;background:#38596b;color:#eef5f8;text-decoration:none}
section{padding:20px;background:var(--surface);border:1px solid var(--line);border-radius:12px;margin-bottom:20px}
.preview{text-align:center}.preview img{display:block;width:min(100%,480px);aspect-ratio:1;height:auto;margin:12px auto;background:var(--bg);border:1px solid #526577;border-radius:8px}
.muted{color:var(--muted);font-size:.9rem}p{margin:6px 0}.status-line{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin:10px 0}
#data-state{padding:3px 9px;border-radius:4px;background:#243a48;font-size:.85rem;font-weight:600}
#data-state[data-state=live]{background:#193b30;color:#b0dcc9}#data-state[data-state=stale],#data-state[data-state=error]{background:#493921;color:#f1d6a8}
.table-scroll{overflow-x:auto;max-width:100%;margin-top:12px}table{border-collapse:collapse;width:100%;white-space:nowrap;font-size:.9rem}
caption{text-align:left;color:var(--muted);padding-bottom:8px}th,td{text-align:left;padding:9px 10px;border-bottom:1px solid var(--line)}th{color:#b8c6d3;background:#1a2937;font-weight:600}
td.num,th.num{text-align:right;font-variant-numeric:tabular-nums}td.empty{text-align:center;color:var(--muted);padding:24px}tbody tr:hover{background:#1a2937}
@media(max-width:520px){body{padding:12px}section{padding:14px}th,td{padding:8px}}
</style><script defer src="/display.js?v=)HTML" __DATE__ "-" __TIME__ R"HTML("></script></head><body><main>
<header><h1>Plane Radar Display</h1><a class="home" href="/">Home</a></header>
<section class="preview" aria-labelledby="preview-title"><h2 id="preview-title">Live display</h2>
<img id="display" alt="Current radar display, refreshed approximately every five seconds">
<p id="image-state" class="muted" role="status">Waiting for display image...</p></section>
<section aria-labelledby="aircraft-title"><h2 id="aircraft-title">Current ADS-B aircraft</h2>
<div class="status-line"><span id="data-state" data-state="loading" role="status">Loading</span><span id="data-updated" class="muted">Waiting for aircraft data...</span></div>
<p class="muted">Received positions, not animation predictions. Includes retained traffic outside the outer ring. Missing values are shown as &mdash;.</p>
<div class="table-scroll" tabindex="0" role="region" aria-label="Current aircraft data, scroll horizontally for all columns">
<table id="aircraft-table"><caption id="aircraft-count">Aircraft in the current feed area</caption><thead><tr>
<th scope="col">Callsign</th><th scope="col">Route</th><th scope="col">Type</th><th scope="col">Hex ID</th>
<th scope="col" class="num">Altitude</th><th scope="col" class="num">Speed (kn)</th><th scope="col" class="num">Track (&deg;)</th><th scope="col" class="num" id="distance-heading">Distance (km)</th><th scope="col" class="num">Position age (s)</th>
</tr></thead><tbody id="aircraft-body"><tr><td colspan="9" class="empty">Waiting for aircraft data...</td></tr></tbody></table>
</div></section></main></body></html>)HTML";

constexpr char kScript[] PROGMEM = R"JS((function(){'use strict';
const image=document.getElementById('display'),imageState=document.getElementById('image-state');
if(window.__RADAR_MANUAL_PREVIEW__){image.src='/display.bmp';return;}
const badge=document.getElementById('data-state'),updated=document.getElementById('data-updated'),body=document.getElementById('aircraft-body');
let timer=0,busy=false,snapshot=null,readAt=0,imageUrl=null;
function state(kind,text){badge.dataset.state=kind;badge.textContent=text;}
function ageMs(){return snapshot&&snapshot.has_sample?snapshot.sample_age_ms+performance.now()-readAt:null;}
function status(){const age=ageMs();if(age===null){state('stale','Waiting for ADS-B');updated.textContent='No successful position sample yet';return;}
const stamp=new Date(Date.now()-age).toLocaleTimeString();updated.textContent='Last ADS-B update '+stamp+' · '+(age/1000).toFixed(1)+' s ago';
state(age>15000?'stale':'live',age>15000?'Data stale':'Live');}
function value(v,digits){return typeof v==='number'&&Number.isFinite(v)?v.toFixed(digits):'—';}
function render(data){const fragment=document.createDocumentFragment();
data.aircraft.forEach(function(a){const row=document.createElement('tr');
[a.callsign,a.route,a.type,a.hex,a.altitude,value(a.speed_knots,0),value(a.track_deg,0),value(a.distance,1),value(a.position_age_s,1)].forEach(function(v,i){const cell=document.createElement('td');cell.textContent=v||'—';if(i>=4)cell.className='num';row.appendChild(cell);});fragment.appendChild(row);});
if(!data.aircraft.length){const row=document.createElement('tr'),cell=document.createElement('td');cell.colSpan=9;cell.className='empty';cell.textContent=data.has_sample?'No aircraft in the current feed area':'Waiting for first ADS-B sample';row.appendChild(cell);fragment.appendChild(row);}
body.replaceChildren(fragment);document.getElementById('aircraft-count').textContent=data.aircraft.length+' aircraft in the current feed area';
document.getElementById('distance-heading').textContent='Distance ('+data.distance_unit+')';}
async function withTimeout(work){const controller=new AbortController(),timeout=setTimeout(function(){controller.abort();},10000);try{return await work(controller.signal);}finally{clearTimeout(timeout);}}
async function readData(){const data=await withTimeout(async function(signal){const r=await fetch('/aircraft.json',{cache:'no-store',signal:signal});if(!r.ok)throw Error('Aircraft request failed');return await r.json();});
if(!Array.isArray(data.aircraft)||typeof data.has_sample!=='boolean'||(data.has_sample&&!Number.isFinite(data.sample_age_ms)))throw Error('Invalid aircraft response');
snapshot=data;readAt=performance.now();render(data);status();}
async function readImage(){await withTimeout(async function(signal){const r=await fetch('/display.bmp?t='+Date.now(),{cache:'no-store',signal:signal});if(!r.ok)throw Error('Display request failed');const blob=await r.blob(),url=URL.createObjectURL(blob),probe=new Image();
try{probe.src=url;await probe.decode();image.src=url;if(imageUrl)URL.revokeObjectURL(imageUrl);imageUrl=url;}catch(error){URL.revokeObjectURL(url);throw error;}});imageState.textContent='Display updated '+new Date().toLocaleTimeString();}
function schedule(){clearTimeout(timer);if(!document.hidden)timer=setTimeout(refresh,5000);}
async function refresh(){if(busy||document.hidden)return;busy=true;
try{await readData();}catch(error){state('error','Connection failed · data stale');updated.textContent=snapshot?'Keeping the last received table':'Aircraft data unavailable';}
if(!document.hidden){try{await readImage();}catch(error){imageState.textContent='Display update failed · keeping the last image';}}
busy=false;if(document.hidden)state('paused','Paused in background');schedule();}
document.addEventListener('visibilitychange',function(){clearTimeout(timer);if(document.hidden){state('paused','Paused in background');}else if(!busy){refresh();}});
window.addEventListener('pagehide',function(){clearTimeout(timer);if(imageUrl)URL.revokeObjectURL(imageUrl);});
refresh();})();)JS";

}  // namespace web::display
