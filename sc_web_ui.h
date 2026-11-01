// The single-page web UI served at "/".
#pragma once
#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="ar" dir="rtl">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmoothCam</title>
<style>
:root{--bg:#0f1214;--panel:#181c20;--line:#2a3036;--text:#e8ebee;--dim:#98a2ab;--accent:#3fb6a8;--rec:#e5484d;--warn:#e0a13a}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 system-ui,"Segoe UI",Tahoma,sans-serif}
main{max-width:980px;margin:0 auto;padding:12px 16px 40px;display:grid;gap:14px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px}
h1{font-size:18px;margin:0;font-weight:600}
h2{font-size:15px;margin:0 0 10px;font-weight:600}
.dim{color:var(--dim)}
.card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px}
.view{position:relative;background:#000;border-radius:10px;overflow:hidden;aspect-ratio:4/3;max-height:62vh;width:100%;display:flex;align-items:center;justify-content:center}
.view img{width:100%;height:100%;object-fit:contain;display:block}
.view .off{position:absolute;color:var(--dim)}
.badge{position:absolute;top:10px;right:10px;background:rgba(0,0,0,.65);padding:3px 10px;border-radius:6px;font-variant-numeric:tabular-nums;direction:ltr}
.badge.rec{background:var(--rec);color:#fff}
.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
button,a.btn,select,input[type=text],input[type=password]{font:inherit;color:var(--text);background:#21262b;border:1px solid var(--line);border-radius:8px;padding:8px 12px}
button,a.btn{cursor:pointer;text-decoration:none}
button:hover,a.btn:hover{border-color:var(--dim)}
button.primary{background:var(--rec);border-color:var(--rec);color:#fff;font-weight:600;min-width:130px}
button.primary.on{background:#21262b;border-color:var(--rec);color:var(--rec)}
.small{padding:4px 10px!important;font-size:13px!important}
button:disabled{opacity:.5;cursor:default}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:10px}
.stat b{display:block;font-size:18px;font-weight:600;font-variant-numeric:tabular-nums;direction:ltr;text-align:right}
.stat span{color:var(--dim);font-size:13px}
.form{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px 18px}
label{display:flex;flex-direction:column;gap:4px;color:var(--dim);font-size:13px}
label.check{flex-direction:row;align-items:center;gap:8px;color:var(--text);font-size:15px}
input[type=range]{width:100%;accent-color:var(--accent)}
select,input[type=text],input[type=password]{width:100%}
.files{display:grid;gap:6px}
.file{display:flex;flex-wrap:wrap;align-items:center;gap:6px 12px;padding:8px 0;border-top:1px solid var(--line)}
.file:first-child{border-top:0}
.file .name{direction:ltr;font-variant-numeric:tabular-nums}
.file .meta{color:var(--dim);font-size:13px;flex:1;min-width:140px}
.notice{border-radius:8px;padding:8px 12px;background:#33291a;color:var(--warn)}
[hidden]{display:none!important}
</style>
</head>
<body>
<main>
<header>
  <h1>SmoothCam</h1>
  <span class="dim" id="netInfo"></span>
</header>

<div class="notice" id="notice" hidden></div>

<div class="view">
  <img id="live" alt="" aria-label="البث المباشر">
  <span class="off" id="off" hidden>البث متوقف</span>
  <span class="badge" id="badge"></span>
</div>

<section class="card bar">
  <button class="primary" id="recBtn">تسجيل</button>
  <button id="streamBtn">إيقاف البث</button>
  <a class="btn" id="snap" target="_blank">لقطة</a>
  <label style="flex:1;min-width:170px">الفلاش
    <input type="range" id="flash" min="0" max="255" value="0">
  </label>
</section>

<section class="card">
  <div class="stats">
    <div class="stat"><b id="sFps">-</b><span>إطار/ثانية</span></div>
    <div class="stat"><b id="sSize">-</b><span>حجم الإطار</span></div>
    <div class="stat"><b id="sRssi">-</b><span>قوة الإشارة</span></div>
    <div class="stat"><b id="sSd">-</b><span>مساحة البطاقة الحرة</span></div>
    <div class="stat"><b id="sRec">-</b><span>حالة التسجيل</span></div>
    <div class="stat"><b id="sRing">-</b><span>امتلاء الذاكرة المؤقتة (أقصى)</span></div>
    <div class="stat"><b id="sDrop">-</b><span>إطارات مفقودة بالتسجيل</span></div>
    <div class="stat"><b id="sSpeed">-</b><span>سرعة الكتابة</span></div>
  </div>
</section>

<section class="card">
  <h2>الإعدادات</h2>
  <div class="form">
    <label>الدقة
      <select data-key="size">
        <option value="QVGA">320×240 (QVGA)</option>
        <option value="CIF">400×296 (CIF)</option>
        <option value="VGA">640×480 (VGA)</option>
        <option value="SVGA">800×600 (SVGA)</option>
        <option value="XGA">1024×768 (XGA)</option>
        <option value="HD">1280×720 (HD)</option>
      </select>
    </label>
    <label>حد الإطارات
      <select data-key="fpsCap">
        <option value="0">الأقصى</option>
        <option value="30">30</option>
        <option value="25">25</option>
        <option value="20">20</option>
        <option value="15">15</option>
        <option value="10">10</option>
      </select>
    </label>
    <label><span>ضغط JPEG: <span id="qVal"></span> (الأقل أوضح وأكبر حجماً)</span>
      <input type="range" data-key="quality" min="6" max="40">
    </label>
    <label>السطوع <input type="range" data-key="brightness" min="-2" max="2"></label>
    <label>التباين <input type="range" data-key="contrast" min="-2" max="2"></label>
    <label>التشبع <input type="range" data-key="saturation" min="-2" max="2"></label>
    <label>مدة ملف الفيديو
      <select data-key="segmentMin">
        <option value="1">دقيقة</option>
        <option value="2">دقيقتان</option>
        <option value="5">5 دقائق</option>
        <option value="10">10 دقائق</option>
      </select>
    </label>
    <label class="check"><input type="checkbox" data-key="vflip"> قلب الصورة عمودياً</label>
    <label class="check"><input type="checkbox" data-key="hmirror"> عكس الصورة أفقياً</label>
    <label class="check"><input type="checkbox" data-key="autoRec"> بدء التسجيل عند التشغيل</label>
    <label class="check"><input type="checkbox" data-key="loopRec"> حذف الأقدم عند امتلاء البطاقة</label>
  </div>
</section>

<section class="card">
  <div class="bar" style="justify-content:space-between;margin-bottom:6px">
    <h2 style="margin:0">التسجيلات</h2>
    <button class="small" id="refresh">تحديث</button>
  </div>
  <div class="files" id="files"></div>
</section>

<section class="card">
  <h2>شبكة الواي فاي</h2>
  <form class="form" id="wifi">
    <label>اسم الشبكة <input type="text" name="ssid" maxlength="32" autocomplete="off"></label>
    <label>كلمة المرور <input type="password" name="pass" maxlength="64" autocomplete="off"></label>
    <div class="bar"><button>حفظ وإعادة التشغيل</button></div>
  </form>
</section>
</main>

<script>
const $=id=>document.getElementById(id);
const media=`http://${location.hostname}:81`;
const post=(path,opts)=>fetch(path,{method:'POST',...opts});
const REC_LABEL=['لا توجد بطاقة','إصلاح آخر ملف','جاهز','يسجّل','يحفظ','خطأ'];
const REC_NO_SD=0,REC_RECORDING=3,REC_STOPPING=4,REC_ERROR=5;
let streaming=true,recording=false,filled=false,lastActive=false;

function startStream(){$('live').src=`${media}/stream?t=${Date.now()}`}
function setStream(on){
  streaming=on;
  $('streamBtn').textContent=on?'إيقاف البث':'تشغيل البث';
  $('off').hidden=on;
  $('live').hidden=!on;
  if(on)startStream();else $('live').removeAttribute('src');
}
$('live').onerror=()=>{if(streaming)setTimeout(startStream,1500)};
$('streamBtn').onclick=()=>setStream(!streaming);
$('snap').href=`${media}/snapshot`;
document.addEventListener('visibilitychange',()=>{
  if(!streaming)return;
  if(document.hidden)$('live').removeAttribute('src');else startStream();
});

$('recBtn').onclick=async()=>{
  $('recBtn').disabled=true;
  await post(`/api/record?on=${recording?0:1}`).catch(()=>{});
  setTimeout(()=>{$('recBtn').disabled=false;poll()},400);
};

$('flash').oninput=e=>post(`/api/set?flash=${e.target.value}`);

document.querySelectorAll('[data-key]').forEach(el=>{
  el.onchange=()=>{
    const value=el.type==='checkbox'?(el.checked?1:0):el.value;
    post(`/api/set?${el.dataset.key}=${encodeURIComponent(value)}`);
    if(el.dataset.key==='quality')$('qVal').textContent=value;
  };
});

function fillSettings(cfg){
  document.querySelectorAll('[data-key]').forEach(el=>{
    const value=cfg[el.dataset.key];
    if(el.type==='checkbox')el.checked=!!value;else el.value=value;
  });
  $('qVal').textContent=cfg.quality;
}

function mmss(total){
  const m=Math.floor(total/60),s=total%60;
  return `${String(m).padStart(2,'0')}:${String(s).padStart(2,'0')}`;
}

async function poll(){
  let s;
  try{s=await (await fetch('/api/status')).json()}catch(e){return}
  const r=s.rec;
  recording=r.state===REC_RECORDING;
  const active=recording||r.state===REC_STOPPING;
  if(lastActive&&!active)loadFiles();
  lastActive=active;

  $('recBtn').textContent=recording?'إيقاف التسجيل':'تسجيل';
  $('recBtn').classList.toggle('on',recording);
  $('recBtn').disabled=r.state===REC_NO_SD;
  $('badge').classList.toggle('rec',recording);
  $('badge').textContent=recording?`REC ${mmss(r.seconds)}`:`${s.fps} fps`;

  $('sFps').textContent=s.fps;
  $('sSize').textContent=`${s.frameKB} KB`;
  $('sRssi').textContent=s.net.ap?'-':`${s.net.rssi} dBm`;
  $('sSd').textContent=r.state===REC_NO_SD?'-':`${(s.sd.freeMB/1024).toFixed(1)} GB`;
  $('sRec').textContent=REC_LABEL[r.state];
  $('sRing').textContent=`${r.ring}% (${r.ringPeak}%)`;
  $('sDrop').textContent=r.dropped;
  $('sSpeed').textContent=active?`${r.kbps} KB/s`:'-';

  $('netInfo').textContent=s.net.ap?`نقطة وصول: ${s.net.ip}`:`${s.net.ssid} · ${s.net.ip}`;
  const notice=r.state===REC_ERROR?r.error:r.state===REC_NO_SD?'لم يتم العثور على بطاقة SD، التسجيل غير متاح.':'';
  $('notice').textContent=notice;
  $('notice').hidden=!notice;

  if(!filled){
    filled=true;
    fillSettings(s.cfg);
    $('flash').value=s.flash;
    document.querySelector('#wifi [name=ssid]').value=s.net.ssid;
  }
}

function fileRow(f){
  const row=document.createElement('div');
  row.className='file';
  const name=document.createElement('span');
  name.className='name';
  name.textContent=f.name;
  const meta=document.createElement('span');
  meta.className='meta';
  const when=f.time>1577836800?new Date(f.time*1000).toLocaleString('ar'):'';
  const size=document.createElement('bdi');
  size.textContent=`${(f.size/1048576).toFixed(1)} MB`;
  meta.append(size,` ${when}`);
  row.append(name,meta);
  if(f.active){
    const note=document.createElement('span');
    note.className='dim';
    note.textContent='قيد التسجيل';
    row.append(note);
    return row;
  }
  const dl=document.createElement('a');
  dl.className='btn small';
  dl.href=`${media}/file/${f.name}`;
  dl.textContent='تنزيل';
  const del=document.createElement('button');
  del.className='small';
  del.textContent='حذف';
  del.onclick=async()=>{
    if(!confirm(`حذف ${f.name}؟`))return;
    await post(`/api/delete?name=${f.name}`);
    loadFiles();
  };
  row.append(dl,del);
  return row;
}

async function loadFiles(){
  let list=[];
  try{list=await (await fetch('/api/files')).json()}catch(e){return}
  const box=$('files');
  box.replaceChildren(...list.map(fileRow));
  if(!list.length){
    const empty=document.createElement('span');
    empty.className='dim';
    empty.textContent='لا توجد تسجيلات بعد.';
    box.append(empty);
  }
}
$('refresh').onclick=loadFiles;

$('wifi').onsubmit=async e=>{
  e.preventDefault();
  await post('/api/wifi',{body:new URLSearchParams(new FormData(e.target))});
  alert('تم الحفظ. سيعاد تشغيل الكاميرا ثم تتصل بالشبكة.');
};

post(`/api/time?t=${Math.floor(Date.now()/1000)}`);
setStream(true);
poll();
loadFiles();
setInterval(poll,1500);
</script>
</body>
</html>
)HTML";
