#ifndef WEB_DASHBOARD_H
#define WEB_DASHBOARD_H

/* ============================================================================
 * SereniSuit — Wi-Fi dashboard, embedded in flash and served by the ESP32-S3.
 *
 * ONE page, two roles:
 *   - on the laptop it is the "website"
 *   - on the phone, "Add to Home Screen" turns it into the "mobile app"
 * Both can be open at the same time. Each one says hello over the WebSocket
 * (and every 5 s after that), so the suit knows who is paired and every
 * screen shows the same "Paired" list, the same BPM and the same status.
 *
 * Alerts after a BPM check: banner + sound + vibration work on plain http
 * while the page/app is open. System notifications need a secure (https)
 * page, so they are used only where the browser allows them.
 * ==========================================================================*/

const char DASHBOARD_HTML[] PROGMEM = R"HTMLPAGE1(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-title" content="SereniSuit">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="theme-color" content="#0f172a">
<title>SereniSuit</title>
<link rel="manifest" href="/manifest.json">
<link rel="icon" href="/icon.svg" type="image/svg+xml">
<style>
  :root{
    --bg:#0f172a; --card:#182338; --line:#2a3752;
    --text:#e7ecf5; --muted:#93a1bd;
    --ok:#37c975; --warn:#f5b942; --danger:#ff5470; --off:#5b6b8c; --accent:#4fa8ff;
  }
  *{box-sizing:border-box;}
  html,body{margin:0;padding:0;background:var(--bg);color:var(--text);
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;}
  body{padding:16px;padding-top:max(16px,env(safe-area-inset-top));max-width:560px;margin:0 auto;padding-bottom:56px;}
  h1{font-size:20px;margin:6px 0 2px;}
  .sub{color:var(--muted);font-size:13px;margin-bottom:16px;}
  .card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px 18px;margin-bottom:12px;}
  .label{font-size:14px;font-weight:600;}
  .hint{color:var(--muted);font-size:12px;margin-top:4px;line-height:1.4;}
  .row{display:flex;align-items:center;justify-content:space-between;gap:12px;}
  .row + .row{margin-top:14px;}

  /* pairing */
  .pair-head{display:flex;align-items:center;gap:10px;font-weight:600;font-size:15px;}
  .pair-dot{width:10px;height:10px;border-radius:50%;background:var(--warn);flex:none;}
  .pair.ok .pair-dot{background:var(--ok);box-shadow:0 0 0 4px rgba(55,201,117,.18);}
  .pair.bad .pair-dot{background:var(--danger);}
  .pair.ok{border-color:rgba(55,201,117,.45);}
  .devices{list-style:none;margin:10px 0 0;padding:0;}
  .devices li{display:flex;align-items:center;gap:8px;font-size:13px;padding:6px 0;border-top:1px solid var(--line);}
  .devices li:first-child{border-top:none;}
  .me{color:var(--accent);font-size:11px;margin-left:auto;}

  /* reading */
  .top{display:flex;justify-content:space-between;align-items:center;min-height:20px;}
  .bpm-row{display:flex;align-items:baseline;gap:10px;justify-content:center;margin-top:6px;}
  .bpm{font-size:68px;font-weight:700;line-height:1;font-variant-numeric:tabular-nums;}
  .bpm-unit{color:var(--muted);font-size:16px;}
  .status-wrap{text-align:center;margin-top:10px;}
  .status{font-weight:700;letter-spacing:.05em;padding:8px 16px;border-radius:999px;display:inline-block;font-size:13px;}
  .status.normal{background:rgba(55,201,117,.15);color:var(--ok);}
  .status.danger{background:rgba(255,84,112,.18);color:var(--danger);animation:pulse 1s infinite;}
  .status.off{background:rgba(91,107,140,.2);color:var(--muted);}
  .status.wait{background:rgba(79,168,255,.15);color:var(--accent);}
  @keyframes pulse{0%,100%{opacity:1;}50%{opacity:.5;}}
  .pill{font-size:11px;padding:3px 9px;border-radius:999px;background:rgba(245,185,66,.15);color:var(--warn);font-weight:600;}

  /* controls */
  .switch{position:relative;width:50px;height:28px;flex:none;}
  .switch input{opacity:0;width:0;height:0;}
  .slider{position:absolute;inset:0;background:var(--off);border-radius:999px;cursor:pointer;transition:.15s;}
  .slider:before{content:"";position:absolute;width:22px;height:22px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.15s;}
  .switch input:checked + .slider{background:var(--ok);}
  .switch input:checked + .slider:before{transform:translateX(22px);}
  input[type=number]{width:100%;padding:10px 12px;border-radius:10px;border:1px solid var(--line);
    background:#0d1626;color:var(--text);font-size:15px;}
  button{border:none;border-radius:10px;padding:10px 14px;font-size:14px;font-weight:600;cursor:pointer;
    font-family:inherit;}
  button:active{transform:scale(.98);}
  .btn-primary{background:var(--accent);color:#06111f;}
  .btn-ghost{background:transparent;color:var(--text);border:1px solid var(--line);}
  .btn-on{background:rgba(55,201,117,.15);color:var(--ok);border:1px solid rgba(55,201,117,.45);}
  .quick{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:12px;}
  .quick button{padding:10px 6px;line-height:1.25;}
  .quick small{display:block;font-weight:500;font-size:11px;opacity:.8;}
  .q-ok{background:rgba(55,201,117,.14);color:var(--ok);}
  .q-bad{background:rgba(255,84,112,.14);color:var(--danger);}
  .custom{display:flex;gap:8px;margin-top:10px;}
  .custom button{flex:none;}
  .full{width:100%;margin-top:10px;}

  /* info */
  .info dl{display:grid;grid-template-columns:auto 1fr;gap:6px 12px;margin:8px 0 0;font-size:13px;}
  .info dt{color:var(--muted);}
  .info dd{margin:0;word-break:break-word;}
  details{margin-top:10px;font-size:13px;color:var(--muted);}
  summary{cursor:pointer;color:var(--accent);}
  details ol{padding-left:18px;margin:8px 0 0;line-height:1.6;}

  /* alerts */
  .banner{position:fixed;left:0;right:0;top:0;z-index:20;background:var(--danger);color:#fff;
    text-align:center;font-weight:700;padding:12px 16px;padding-top:max(12px,env(safe-area-inset-top));
    transform:translateY(-110%);transition:transform .2s;}
  .banner.show{transform:none;}
  body.banner-on{padding-top:calc(64px + env(safe-area-inset-top));}
  .toasts{position:fixed;left:0;right:0;bottom:16px;z-index:30;display:flex;flex-direction:column;
    align-items:center;gap:8px;pointer-events:none;padding:0 16px;}
  .toast{background:#e7ecf5;color:#0f172a;border-radius:12px;padding:10px 14px;font-size:14px;font-weight:600;
    box-shadow:0 6px 24px rgba(0,0,0,.35);max-width:520px;width:100%;opacity:0;transform:translateY(8px);
    transition:opacity .2s,transform .2s;}
  .toast.show{opacity:1;transform:none;}
  .toast.danger{background:var(--danger);color:#fff;}
  .toast.ok{background:var(--ok);color:#06111f;}
</style>
</head>
<body>
  <div class="banner" id="banner"></div>

  <h1>SereniSuit</h1>
  <div class="sub">Live heart rate from the suit's ECG (AD8232 chest electrodes)</div>

  <div class="card pair" id="pairCard">
    <div class="pair-head"><span class="pair-dot"></span><span id="pairTitle">Pairing with SereniSuit…</span></div>
    <div class="hint" id="pairSub">Connecting over the suit's Wi-Fi</div>
    <ul class="devices" id="deviceList"></ul>
  </div>

  <div class="card">
    <div class="top">
      <span class="pill" id="demoPill" style="display:none;">DEMO INPUT</span>
      <span></span>
    </div>
    <div class="bpm-row">
      <span class="bpm" id="bpmValue">--</span>
      <span class="bpm-unit">BPM</span>
    </div>
    <div class="status-wrap"><span class="status wait" id="statusBadge">CONNECTING</span></div>
    <div class="hint" id="shapeLine" style="display:none;text-align:center;margin-top:8px;"></div>
  </div>

  <div class="card">
    <div class="row">
      <div>
        <div class="label">Sensors</div>
        <div class="hint">Switch the suit's ECG sensor (AD8232) on or off.</div>
      </div>
      <label class="switch">
        <input type="checkbox" id="sensorsToggle" checked>
        <span class="slider"></span>
      </label>
    </div>
    <div class="row">
      <div>
        <div class="label">Alerts on this device</div>
        <div class="hint" id="alertHint">Tap to allow sound and vibration after every BPM check.</div>
      </div>
      <button class="btn-ghost" id="alertBtn">Enable</button>
    </div>
  </div>

  <div class="card">
    <div class="label">Demo — fake BPM input</div>
    <div class="hint">Drives the suit exactly like a real reading. 60–100 is normal and is just shown.
      Outside that range the suit shows DANGER and vibrates.</div>
    <div class="quick">
      <button class="q-ok"  data-bpm="72">72<small>normal</small></button>
      <button class="q-bad" data-bpm="45">45<small>too low</small></button>
      <button class="q-bad" data-bpm="140">140<small>too high</small></button>
    </div>
    <div class="custom">
      <input type="number" id="demoBpmInput" placeholder="Any BPM, e.g. 110" inputmode="numeric" min="1" max="300">
      <button class="btn-primary" id="sendDemoBtn">Send</button>
    </div>
    <button class="btn-ghost full" id="clearDemoBtn">Back to live sensor</button>
  </div>

  <div class="card info">
    <div class="label">Suit</div>
    <dl>
      <dt>Wi-Fi</dt><dd id="iNet">—</dd>
      <dt>Address</dt><dd id="iAddr">—</dd>
      <dt>ECG sensor</dt><dd id="iSensor">—</dd>
      <dt>Screen</dt><dd id="iOled">—</dd>
      <dt>On-chip ML</dt><dd id="iMl">—</dd>
    </dl>
    <details>
      <summary>Install as an app on your phone</summary>
      <ol>
        <li>Open this page on the phone (joined to the suit's Wi-Fi).</li>
        <li>Android Chrome: menu ⋮ → <b>Add to Home screen</b>.</li>
        <li>iPhone Safari: Share → <b>Add to Home Screen</b>.</li>
        <li>Open SereniSuit from the new icon — it pairs on launch.</li>
      </ol>
    </details>
  </div>

  <div class="toasts" id="toasts"></div>

)HTMLPAGE1"
R"HTMLPAGE2(<script>
(function(){
  var $ = function(id){ return document.getElementById(id); };
  var bpmEl = $('bpmValue'), statusEl = $('statusBadge'), demoPill = $('demoPill');
  var pairCard = $('pairCard'), pairTitle = $('pairTitle'), pairSub = $('pairSub'), deviceList = $('deviceList');
  var sensorsToggle = $('sensorsToggle'), alertBtn = $('alertBtn'), alertHint = $('alertHint');
  var demoInput = $('demoBpmInput'), banner = $('banner');

  var ws = null, wsOpen = false, reconnectTimer = null;
  var myId = null;                 // this device's id on the suit
  var known = null;                // id -> label of paired devices (for "X paired" toasts)
  var lastCheckSeq = null;
  var lastState = null;
  var alertsOn = false, audioCtx = null;
  var toggleBusyUntil = 0;         // ignore echoes right after this device flips the switch
  var myLabel = deviceLabel();

  /* ---------- who am I ---------- */
  function deviceLabel(){
    var ua = navigator.userAgent || '';
    var mobile = /Android|iPhone|iPad|iPod|Mobile/i.test(ua);
    var standalone = (window.matchMedia && window.matchMedia('(display-mode: standalone)').matches) ||
                     window.navigator.standalone === true;
    var os = /Android/i.test(ua) ? 'Android' : /iPhone|iPad|iPod/i.test(ua) ? 'iPhone' :
             /Mac/i.test(ua) ? 'Mac' : /Windows/i.test(ua) ? 'Windows' : /Linux/i.test(ua) ? 'Linux' : '';
    var kind = mobile ? (standalone ? 'Mobile app' : 'Mobile browser') : 'Website';
    return os ? kind + ' (' + os + ')' : kind;
  }
  function isMobileLabel(l){ return /^Mobile/.test(l); }

  /* ---------- small UI helpers ---------- */
  function toast(text, kind){
    var t = document.createElement('div');
    t.className = 'toast' + (kind ? ' ' + kind : '');
    t.textContent = text;
    $('toasts').appendChild(t);
    requestAnimationFrame(function(){ t.classList.add('show'); });
    setTimeout(function(){ t.classList.remove('show'); setTimeout(function(){ t.remove(); }, 250); }, 3500);
  }

  var STATUS = {
    normal:       {text:'NORMAL',         cls:'normal'},
    danger:       {text:'DANGER',         cls:'danger'},
    sensors_off:  {text:'SENSORS OFF',    cls:'off'},
    sensor_error: {text:'NO ECG SIGNAL',     cls:'off'},
    leads_off:    {text:'ATTACH ELECTRODES', cls:'wait'},
    reading:      {text:'READING…',       cls:'wait'},
    booting:      {text:'STARTING…',      cls:'wait'}
  };

  /* ---------- pairing ---------- */
  function renderPairing(s){
    var devices = (s && s.devices) || [];
    var mine = myId !== null && devices.some(function(d){ return d.id === myId; });

    pairCard.classList.remove('ok', 'bad');
    if (!wsOpen) {
      pairCard.classList.add('bad');
      pairTitle.textContent = 'Not connected to SereniSuit';
      pairSub.textContent = 'Reconnecting… check this device is on the SereniSuit-Demo Wi-Fi.';
    } else if (mine) {
      pairCard.classList.add('ok');
      pairTitle.textContent = 'Paired with SereniSuit ✓';
      pairSub.textContent = devices.length + (devices.length === 1 ? ' device' : ' devices') + ' connected right now';
    } else {
      pairTitle.textContent = 'Pairing with SereniSuit…';
      pairSub.textContent = 'Connected — finishing pairing';
    }

    deviceList.innerHTML = '';
    devices.forEach(function(d){
      var li = document.createElement('li');
      var icon = document.createElement('span');
      icon.textContent = isMobileLabel(d.label) ? '📱' : '💻';
      var name = document.createElement('span');
      name.textContent = d.label;
      li.appendChild(icon); li.appendChild(name);
      if (d.id === myId) {
        var me = document.createElement('span');
        me.className = 'me'; me.textContent = 'this device';
        li.appendChild(me);
      }
      deviceList.appendChild(li);
    });

    /* toasts when another device pairs or leaves */
    var now = {};
    devices.forEach(function(d){ now[d.id] = d.label; });
    if (known !== null && mine) {
      Object.keys(now).forEach(function(id){
        if (!(id in known) && +id !== myId) toast((isMobileLabel(now[id]) ? '📱 ' : '💻 ') + now[id] + ' paired', 'ok');
      });
      Object.keys(known).forEach(function(id){
        if (!(id in now) && +id !== myId) toast(known[id] + ' disconnected');
      });
    }
    if (mine) known = now;
  }

  /* ---------- alerts after a BPM check ---------- */
  function beep(freq, ms, when){
    if (!audioCtx) return;
    var o = audioCtx.createOscillator(), g = audioCtx.createGain();
    o.frequency.value = freq; o.type = 'sine';
    g.gain.setValueAtTime(0.0001, audioCtx.currentTime + when);
    g.gain.exponentialRampToValueAtTime(0.3, audioCtx.currentTime + when + 0.02);
    g.gain.exponentialRampToValueAtTime(0.0001, audioCtx.currentTime + when + ms / 1000);
    o.connect(g); g.connect(audioCtx.destination);
    o.start(audioCtx.currentTime + when); o.stop(audioCtx.currentTime + when + ms / 1000 + 0.05);
  }

  function systemNotify(title, body){
    if (!window.isSecureContext || !('Notification' in window) || Notification.permission !== 'granted') return;
    var opts = {body: body, tag: 'serenisuit'};
    var plain = function(){ try { new Notification(title, opts); } catch (e) {} };
    if (navigator.serviceWorker && navigator.serviceWorker.getRegistration) {
      navigator.serviceWorker.getRegistration().then(function(reg){
        if (reg) reg.showNotification(title, opts); else plain();
      }).catch(plain);
    } else {
      plain();
    }
  }

  function alertCheck(s){
    var danger = s.status === 'danger';
    var title = danger ? '⚠ DANGER — ' + s.bpm + ' BPM' : 'BPM check: ' + s.bpm + ' BPM — normal';
    var body  = danger ? 'Outside 60–100 BPM. Vibration alert is running on the suit.' : 'Within the 60–100 BPM range.';
    toast(title, danger ? 'danger' : 'ok');
    if (!alertsOn) return;
    if (navigator.vibrate) navigator.vibrate(danger ? [250, 120, 250, 120, 250] : [90]);
    if (danger) { beep(880, 180, 0); beep(880, 180, 0.3); beep(880, 180, 0.6); }
    else        { beep(660, 120, 0); }
    systemNotify(danger ? 'SereniSuit — DANGER' : 'SereniSuit — BPM check', title + '. ' + body);
  }

  alertBtn.addEventListener('click', function(){
    try {
      var AC = window.AudioContext || window.webkitAudioContext;
      if (AC && !audioCtx) audioCtx = new AC();
      if (audioCtx && audioCtx.state === 'suspended') audioCtx.resume();
    } catch (e) {}
    alertsOn = true;
    alertBtn.textContent = 'On';
    alertBtn.className = 'btn-on';
    beep(660, 100, 0);
    if (navigator.vibrate) navigator.vibrate(60);
    if (window.isSecureContext && 'Notification' in window && Notification.permission === 'default') {
      Notification.requestPermission();
    }
    alertHint.textContent = window.isSecureContext
      ? 'Banner, sound, vibration and system notifications after every BPM check.'
      : 'Banner, sound and vibration after every BPM check while this page/app is open.';
  });

  /* ---------- state from the suit ---------- */
  function applyState(s){
    lastState = s;
    bpmEl.textContent = (s.bpm > 0 && (s.status === 'normal' || s.status === 'danger')) ? s.bpm : '--';
    var m = STATUS[s.status] || {text: String(s.status || '').toUpperCase(), cls: 'wait'};
    statusEl.textContent = m.text;
    statusEl.className = 'status ' + m.cls;
    demoPill.style.display = s.demoActive ? 'inline-block' : 'none';
    var shapeLine = $('shapeLine');
    if (s.shape >= 0 && s.shapeN > 0) {
      shapeLine.textContent = 'Beat shape (research preview): ' + s.shape + '% typical of ' + s.shapeN + ' beats';
      shapeLine.style.display = 'block';
    } else {
      shapeLine.style.display = 'none';
    }
    if (Date.now() > toggleBusyUntil) sensorsToggle.checked = !!s.sensorsOn;

    if (s.status === 'danger') {
      banner.textContent = '⚠ DANGER — ' + s.bpm + ' BPM · vibration alert active';
      banner.classList.add('show');
      document.body.classList.add('banner-on');
    } else {
      banner.classList.remove('show');
      document.body.classList.remove('banner-on');
    }

    renderPairing(s);

    if (typeof s.checkSeq === 'number') {
      if (lastCheckSeq === null || s.checkSeq < lastCheckSeq) {
        lastCheckSeq = s.checkSeq;                      // first message / suit rebooted: no alert
      } else if (s.checkSeq > lastCheckSeq) {
        lastCheckSeq = s.checkSeq;
        if (s.status === 'normal' || s.status === 'danger') alertCheck(s);
      }
    }
  }

  /* ---------- talking to the suit ---------- */
  function postJSON(url, body){
    return fetch(url, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)})
      .then(function(r){ if (!r.ok) throw new Error(r.status); return r; })
      .catch(function(){ toast("Couldn't reach the suit — is this device on its Wi-Fi?", 'danger'); });
  }

  sensorsToggle.addEventListener('change', function(){
    toggleBusyUntil = Date.now() + 1500;
    postJSON('/api/sensors', {on: sensorsToggle.checked});
  });

  function sendDemo(v){
    if (isNaN(v) || v < 1 || v > 300) { toast('Enter a BPM between 1 and 300'); return; }
    postJSON('/api/demo', {bpm: v});
  }
  Array.prototype.forEach.call(document.querySelectorAll('.quick button'), function(b){
    b.addEventListener('click', function(){ sendDemo(parseInt(b.getAttribute('data-bpm'), 10)); });
  });
  $('sendDemoBtn').addEventListener('click', function(){ sendDemo(parseInt(demoInput.value, 10)); });
  demoInput.addEventListener('keydown', function(e){ if (e.key === 'Enter') sendDemo(parseInt(demoInput.value, 10)); });
  $('clearDemoBtn').addEventListener('click', function(){ postJSON('/api/demo', {clear: true}); });

  function sendHello(){
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({type: 'hello', device: myLabel}));
  }

  function loadInfo(){
    fetch('/api/info').then(function(r){ return r.json(); }).then(function(i){
      $('iNet').textContent = i.network + (i.ownNet ? ' (the suit\'s own network)' : '');
      $('iAddr').textContent = 'http://' + i.ip + '  ·  http://' + i.mdns;
      $('iSensor').textContent = (i.sensor || 'AD8232 ECG') + ' · OUTPUT→5, LO+→6, LO−→7';
      $('iOled').textContent = i.oled || '—';
      $('iMl').textContent = i.mlCorrect + '/' + i.mlTotal + ' test beats correct · ' + i.mlUs + ' µs each · ' + i.mlKb + ' KB model (PTB ECG)';
    }).catch(function(){});
  }

  function connect(){
    clearTimeout(reconnectTimer);
    var proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
    ws = new WebSocket(proto + location.host + '/ws');
    ws.onopen = function(){
      wsOpen = true;
      myId = null;
      sendHello();
      loadInfo();
      renderPairing(lastState);
    };
    ws.onclose = function(){
      var was = wsOpen;
      wsOpen = false; myId = null; known = null;
      renderPairing(lastState);
      if (was) toast('Connection to the suit lost — reconnecting…', 'danger');
      reconnectTimer = setTimeout(connect, 1500);
    };
    ws.onerror = function(){ try { ws.close(); } catch (e) {} };
    ws.onmessage = function(evt){
      var msg;
      try { msg = JSON.parse(evt.data); } catch (e) { return; }
      if (msg.type === 'paired') {
        myId = msg.id;
        toast('Paired with SereniSuit ✓', 'ok');
        if (lastState) renderPairing(lastState);
        return;
      }
      applyState(msg);
    };
  }

  setInterval(sendHello, 5000);                         // heartbeat keeps this device listed
  document.addEventListener('visibilitychange', function(){
    if (document.visibilityState !== 'visible') return;
    if (ws && ws.readyState === WebSocket.OPEN) sendHello(); else connect();
  });

  /* fallback: keep the reading fresh even if the socket can't open */
  setInterval(function(){
    if (wsOpen) return;
    fetch('/api/state').then(function(r){ return r.json(); }).then(applyState).catch(function(){});
  }, 4000);

  if (window.isSecureContext && 'serviceWorker' in navigator) {
    navigator.serviceWorker.register('/sw.js').catch(function(){});
  }

  connect();
})();
</script>
</body>
</html>
)HTMLPAGE2";

const char DASHBOARD_MANIFEST[] PROGMEM = R"JSONDOC({
  "name": "SereniSuit",
  "short_name": "SereniSuit",
  "start_url": "/",
  "scope": "/",
  "display": "standalone",
  "background_color": "#0f172a",
  "theme_color": "#0f172a",
  "icons": [
    { "src": "/icon.svg", "sizes": "any", "type": "image/svg+xml", "purpose": "any" }
  ]
})JSONDOC";

const char DASHBOARD_ICON_SVG[] PROGMEM = R"SVGDOC(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<rect width="100" height="100" rx="20" fill="#0f172a"/>
<path d="M10 52 H32 L40 30 L52 72 L62 45 L70 52 H90" fill="none" stroke="#4fa8ff" stroke-width="7" stroke-linecap="round" stroke-linejoin="round"/>
</svg>
)SVGDOC";

/* Service worker: only registered on secure (https) pages, where it enables
 * system notifications. Network-first - live data is never cached. */
const char DASHBOARD_SW[] PROGMEM = R"SWDOC(
self.addEventListener('install', function(e){ self.skipWaiting(); });
self.addEventListener('activate', function(e){ e.waitUntil(self.clients.claim()); });
self.addEventListener('fetch', function(e){
  e.respondWith(fetch(e.request).catch(function(){ return new Response('offline', {status: 503}); }));
});
self.addEventListener('notificationclick', function(e){
  e.notification.close();
  e.waitUntil(self.clients.matchAll({type: 'window'}).then(function(list){
    if (list.length) return list[0].focus();
    return self.clients.openWindow('/');
  }));
});
)SWDOC";

#endif
