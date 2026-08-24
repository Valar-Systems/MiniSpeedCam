/**
 * config_portal.h - Self-hosted web configuration portal (replaces ESPUI).
 *
 * Serves the device's configuration page from the IDF-native esp_http_server
 * on port 80 -- the same stack the aiming-stream module already uses
 * (camera_stream.cpp on port 81). Dropping ESPUI lets us drop the whole
 * ESPAsyncWebServer / AsyncTCP stack, whose cross-core WebSocket broadcasts
 * were the source of the rev-1.1 freeze under WiFi load; here the browser
 * *pulls* a JSON snapshot instead of the firmware pushing from Core 1.
 *
 * Because ESPUI/ESPAsyncWebServer are gone, esp_http_server.h no longer clashes
 * with anything in the main translation unit (the old HTTP_* enum collision was
 * async-vs-IDF), so this can be a plain header compiled into main.cpp with
 * direct access to the globals / preferences / diagnostics helpers -- exactly
 * as espui_settings.h was.
 *
 * Endpoints (all on port 80):
 *   GET  /              -> the single-page config UI (CONFIG_HTML, from flash)
 *   GET  /api/state     -> JSON snapshot of live values + current settings
 *   GET  /api/events    -> recent-detection ring (speed + age) for LAN companions
 *   POST /api/settings  -> device settings (units, speeds, signals); applied live
 *   POST /api/wifi      -> SSID / password / API base URL; persisted then reboot
 *   POST /api/clear     -> wipe WiFi credentials then reboot (into the setup AP)
 *   POST /api/stream    -> start/stop the aiming MJPEG stream
 *
 * The page polls /api/state ~1 Hz to refresh the live readouts (speed, signal,
 * heap, uptime, ...), so there is no server-side push and no per-control state.
 */
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_random.h>   // esp_random(): mints the per-boot portal CSRF token
#include <string.h>       // strchr/strlen for the request-auth checks
#include "esp_http_server.h"

// Compose the installed-firmware version string (ESP is compile-time; STM is
// read over UART at boot and can change after an STM auto-flash). Reported in
// /api/state and shown on the Status section.
static String firmwareVersionText() {
  return String("ESP ") + FW_VERSION + "  /  STM " + stm_fw_version;
}

// ---------------------------------------------------------------------------
// The config page. One self-contained document (inline CSS/JS, no CDN) so it
// renders fully in AP mode during first-time setup, when there is no internet.
// Live values are read-only spans refreshed by polling /api/state; the form
// inputs are populated once on first load so a refresh never clobbers typing.
// esp_http_server does no template substitution, so literal '%' (e.g. CSS
// width:100%) needs no escaping.
// ---------------------------------------------------------------------------
static const char CONFIG_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang=en>
<head>
<meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<meta name=csrf content="__CSRF__">
<title>MiniSpeedCam</title>
<style>
:root{--bg:#0f1115;--card:#1a1d24;--fg:#e6e8eb;--mut:#9aa0a6;--acc:#3b82f6;--ok:#22c55e;--bd:#2a2e37}
*{box-sizing:border-box}
body{margin:0 auto;max-width:560px;padding:16px;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
h1{font-size:20px;margin:8px 0 2px}
.sub{color:var(--mut);font-size:13px;margin-bottom:16px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:16px;margin-bottom:16px}
.card h2{font-size:13px;margin:0 0 12px;text-transform:uppercase;letter-spacing:.05em;color:var(--mut)}
label{display:block;margin:10px 0}
label>span{display:block;font-size:13px;color:var(--mut);margin-bottom:4px}
input[type=text],input[type=password],input[type=number]{width:100%;padding:9px 10px;background:#0c0e12;border:1px solid var(--bd);border-radius:8px;color:var(--fg);font-size:15px}
.switch{display:flex;align-items:center;justify-content:space-between;gap:12px}
.switch>span{margin:0}
.row{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid var(--bd)}
.row:last-child{border-bottom:0}
.row .k{color:var(--mut);font-size:13px}
.row .v{font-variant-numeric:tabular-nums;font-weight:600}
button{width:100%;padding:11px;border:0;border-radius:8px;background:var(--acc);color:#fff;font-size:15px;font-weight:600;cursor:pointer;margin-top:12px}
button.warn{background:#3a2326;color:#f87171;border:1px solid #5b2a2e}
.big{font-size:34px;font-weight:700;text-align:center;margin:2px 0}
.claim{text-align:center;color:var(--ok);font-weight:600;min-height:1.2em}
.muted{color:var(--mut);font-size:12px;margin-top:6px;word-break:break-all}
.stream{display:none;width:100%;max-width:320px;margin-top:10px;border-radius:8px;background:#000;aspect-ratio:4/3}
.big .u{font-size:15px;font-weight:500;color:var(--mut);margin-left:4px}
.cap{display:none;width:100%;border-radius:8px;margin-top:12px;background:#000;aspect-ratio:4/3;object-fit:contain}
.noimg{display:none;width:100%;aspect-ratio:4/3;margin-top:12px;border-radius:8px;background:#0c0e12;border:1px dashed var(--bd);color:var(--mut);font-size:14px;align-items:center;justify-content:center;text-align:center}
.toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:var(--ok);color:#06210f;padding:10px 16px;border-radius:8px;font-weight:600;opacity:0;transition:opacity .2s;pointer-events:none}
.toast.show{opacity:1}
</style>
</head>
<body>
<h1>MiniSpeedCam</h1>
<div class=sub>Device configuration</div>

<div class=card>
<div class=claim id=claim>&mdash;</div>
</div>

<div class=card id=lastCard style=display:none>
<h2>Last capture</h2>
<div class=big><span id=lastSpeed>&mdash;</span><span class=u id=lastUnit></span></div>
<div class=muted id=lastMeta style=text-align:center>&mdash;</div>
<img id=lastImg class=cap alt="last capture photo">
<div id=lastNoImg class=noimg>No photo available</div>
</div>

<div class=card>
<h2>Device</h2>
<label class=switch><span>Units (KPH)</span><input type=checkbox id=isKph></label>
<label class=switch><span>Power-Saver (drop WiFi when idle)</span><input type=checkbox id=powerSaver></label>
<label><span>Minimum speed</span><input type=number id=minSpeed></label>
<label><span>Photo speed</span><input type=number id=photoSpeed></label>
<label><span>Speed correction (cosine; 1.000 = off)</span><input type=number id=speedCorr step=0.001 min=1 max=1.3></label>
<label><span>Min radar signal (proximity, 0=off)</span><input type=number id=minSignal></label>
<label><span>Photo radar signal (shared/default, 0=off)</span><input type=number id=photoSignal></label>
<label><span>Photo radar signal FRONT (oncoming; raise=closer, 0=shared)</span><input type=number id=psFront></label>
<label><span>Photo radar signal REAR (receding; lower=farther, 0=shared)</span><input type=number id=psRear></label>
<button onclick=saveSettings()>Save device settings</button>
</div>

<div class=card>
<h2>Aiming stream</h2>
<label class=switch><span>Live video (5 min, for mounting)</span><input type=checkbox id=streamToggle onchange=toggleStream()></label>
<img id=streamImg class=stream alt="aiming preview">
</div>

<div class=card>
<h2>WiFi &amp; data server</h2>
<label><span>Network (SSID)</span><input type=text id=ssid autocomplete=off></label>
<label><span>Password</span><input type=password id=password placeholder="(unchanged)" autocomplete=off></label>
<label><span>Data Server API base URL (blank = default)</span><input type=text id=apiBase autocomplete=off></label>
<button onclick=saveWifi()>Save WiFi &amp; reboot</button>
<button class=warn onclick=clearWifi()>Clear settings &amp; reboot</button>
</div>

<div class=card>
<h2>Status</h2>
<div class=row><span class=k>Radar signal (proximity)</span><span class=v id=signal>&mdash;</span></div>
<div class=row><span class=k>Free heap (B)</span><span class=v id=heap>&mdash;</span></div>
<div class=row><span class=k>Free PSRAM (B)</span><span class=v id=psram>&mdash;</span></div>
<div class=row><span class=k>Uptime</span><span class=v id=uptime>&mdash;</span></div>
<div class=row><span class=k>WiFi RSSI (dBm)</span><span class=v id=rssi>&mdash;</span></div>
<div class=row><span class=k>IP</span><span class=v id=ip>&mdash;</span></div>
<div class=row><span class=k>Last upload HTTP</span><span class=v id=lastUpload>&mdash;</span></div>
<div class=row><span class=k>Last reset reason</span><span class=v id=resetReason>&mdash;</span></div>
<div class=row><span class=k>Boot count</span><span class=v id=bootCount>&mdash;</span></div>
<div class=row><span class=k>Firmware</span><span class=v id=fwVersion>&mdash;</span></div>
<div class=row><span class=k>Update status</span><span class=v id=otaStatus>&mdash;</span></div>
<button onclick=checkUpdate()>Check for updates now</button>
</div>

<div class=toast id=toast></div>

<script>
let filled=false,streamShown=false;
const $=id=>document.getElementById(id);
const txt=(id,v)=>{const e=$(id);if(e)e.textContent=v};
function toast(m){const t=$('toast');t.textContent=m;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),1800)}
async function refresh(){
  let s;
  try{s=await(await fetch('/api/state')).json()}catch(e){return}
  txt('claim',s.claim);
  txt('signal',s.signal);
  txt('heap',s.heap);
  txt('psram',s.psram);
  txt('uptime',s.uptime);
  txt('rssi',s.rssi);
  txt('ip',s.ip);
  txt('lastUpload',s.lastUpload);
  txt('resetReason',s.resetReason);
  txt('bootCount',s.bootCount);
  txt('fwVersion',s.fwVersion);
  txt('otaStatus',s.otaStatus);
  updateLastCapture(s);
  syncStream(s.streamActive);
  if(!filled){
    $('isKph').checked=s.isKph;
    $('powerSaver').checked=s.powerSaver;
    $('minSpeed').value=s.minSpeed;
    $('photoSpeed').value=s.photoSpeed;
    $('speedCorr').value=s.speedCorr;
    $('minSignal').value=s.minSignal;
    $('photoSignal').value=s.photoSignal;
    $('psFront').value=s.psFront;
    $('psRear').value=s.psRear;
    $('ssid').value=s.ssid;
    $('apiBase').value=s.apiBase;
    filled=true;
  }
}
// "Last capture" card. Text (speed/direction/age) comes from the newest /api/events
// pass on every poll. The image always corresponds to THAT pass: shown only when
// the newest pass has its own photo (photoSeq === lastSeq), otherwise a same-size
// "no photo" placeholder -- never a stale image from an earlier pass. The JPEG is
// fetched only when it changes, so it isn't re-pulled on every 4s /api/state poll.
let shownPhotoSeq=-1;
const ago=n=>n<5?'just now':n<60?n+'s ago':n<3600?Math.floor(n/60)+'m ago':Math.floor(n/3600)+'h ago';
const dirTxt=d=>d==1?' • approaching':d==2?' • receding':'';
function updateLastCapture(s){
  if(!s.lastSeq){$('lastCard').style.display='none';return}
  $('lastCard').style.display='';
  txt('lastSpeed',s.lastSpeed);
  txt('lastUnit',s.lastKph?'KPH':'MPH');
  txt('lastMeta',ago(s.lastAgeSec)+dirTxt(s.lastDir));
  const img=$('lastImg'),noimg=$('lastNoImg');
  if(s.photoSeq && s.photoSeq===s.lastSeq){
    if(s.photoSeq!==shownPhotoSeq){img.src='/api/last.jpg?seq='+s.photoSeq;shownPhotoSeq=s.photoSeq}
    img.style.display='block';noimg.style.display='none';
  }else{
    img.style.display='none';img.removeAttribute('src');shownPhotoSeq=-1;
    noimg.style.display='flex';
  }
}
const csrf=(document.querySelector('meta[name=csrf]')||{}).content||'';
const post=(p,b)=>fetch(p,{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},body:JSON.stringify(b)});
async function saveSettings(){
  await post('/api/settings',{
    isKph:$('isKph').checked,powerSaver:$('powerSaver').checked,
    minSpeed:+$('minSpeed').value,photoSpeed:+$('photoSpeed').value,
    speedCorr:+$('speedCorr').value,
    minSignal:+$('minSignal').value,photoSignal:+$('photoSignal').value,
    psFront:+$('psFront').value,psRear:+$('psRear').value});
  toast('Saved');
}
async function saveWifi(){
  await post('/api/wifi',{ssid:$('ssid').value,password:$('password').value,apiBase:$('apiBase').value});
  document.body.innerHTML='<h1>Rebooting&hellip;</h1><p class=sub>Reconnect to your network, then reopen this page.</p>';
}
async function clearWifi(){
  if(!confirm('Erase WiFi credentials and reboot into the setup AP?'))return;
  await post('/api/clear',{});
  document.body.innerHTML='<h1>Cleared. Rebooting&hellip;</h1><p class=sub>Join the MiniSpeedCam WiFi to reconfigure.</p>';
}
async function checkUpdate(){
  await post('/api/ota/check',{});
  toast('Checking GitHub for updates...');
}
function syncStream(on){
  if(on===streamShown)return;
  const img=$('streamImg');
  if(on){img.src='/stream?t='+Date.now();img.style.display='block'}
  else{img.style.display='none';img.removeAttribute('src')}
  $('streamToggle').checked=on;
  streamShown=on;
}
const toggleStream=async()=>{await post('/api/stream',{on:$('streamToggle').checked});syncStream($('streamToggle').checked)};
// Poll the live Status values every 4 s, and pause entirely while the tab is
// hidden -- each poll is a WiFi TX burst that disturbs the shared-rail radar, so
// a backgrounded page goes quiet instead of nagging the radio once a second.
let pollTimer=null;
function startPolling(){if(pollTimer)return;refresh();pollTimer=setInterval(refresh,4000)}
function stopPolling(){if(pollTimer){clearInterval(pollTimer);pollTimer=null}}
document.addEventListener('visibilitychange',()=>document.hidden?stopPolling():startPolling());
startPolling();
</script>
</body>
</html>)HTML";

// ---------------------------------------------------------------------------
// Handlers. Run in the esp_http_server task. They read/write the same globals
// and the same `preferences` (NVS) instance the ESPUI callbacks used, so the
// cross-task story is unchanged. Settings are applied live; WiFi/Clear reboot.
// ---------------------------------------------------------------------------

// Serialize the full device state (live values + current settings) as JSON.
static void portalBuildState(String& out) {
  char uptime_buf[32];
  diagnosticsFormatUptime(uptime_buf, sizeof(uptime_buf));
  bool connected = (WiFi.status() == WL_CONNECTED);

  JsonDocument doc;
  // --- live values (refreshed by the page poll) ---
  // Speed readout removed from the page: showing live speed while aiming is
  // misleading (the radar is meant for passing traffic, not bench checks) and
  // the per-second poll's WiFi TX disturbs the shared-rail radar anyway.
  doc["signal"]      = g_last_peak_mag;
  doc["heap"]        = ESP.getFreeHeap();
  doc["psram"]       = ESP.getFreePsram();
  doc["uptime"]      = uptime_buf;
  doc["rssi"]        = connected ? String(WiFi.RSSI()) : String("n/a");
  doc["ip"]          = connected ? WiFi.localIP().toString() : String("n/a");
  doc["lastUpload"]  = diagnosticsLastUploadCode();
  doc["resetReason"] = diagnosticsResetReason();
  doc["bootCount"]   = diagnosticsBootCount();
  doc["fwVersion"]   = firmwareVersionText();
  doc["otaStatus"]   = ota_status;
  doc["claim"]       = device_claimed ? String("Linked to your account") : claim_code;

  doc["streamActive"] = (bool)stream_active;  // page embeds /stream inline on this same server

  // --- last capture (top-of-page "Last capture" card) ---
  // lastSeq/Speed/Dir/AgeSec describe the newest pass from the /api/events ring
  // (every pass, photo or not). photoSeq is the seq of the pass whose JPEG we
  // still hold in PSRAM (0 = none): the page shows the image when a photo exists
  // and notes when photoSeq < lastSeq (newest pass was speed-only). Keeping the
  // image on a separate seq lets the page refetch the ~hundreds-of-KB JPEG only
  // when it actually changes, not on every 4 s /api/state poll.
  {
    int lastSpeed = 0; uint8_t lastDir = 0; bool lastKph = is_kph;
    uint32_t lastSeq = 0, lastAge = 0;
    if (eventsLatest(&lastSpeed, &lastDir, &lastKph, &lastSeq, &lastAge)) {
      doc["lastSeq"]    = lastSeq;
      doc["lastSpeed"]  = lastSpeed;
      doc["lastKph"]    = lastKph;
      doc["lastDir"]    = lastDir;      // 0 unknown, 1 approaching, 2 receding
      doc["lastAgeSec"] = lastAge;
    } else {
      doc["lastSeq"] = 0;
    }
    uint32_t photoSeq = 0;
    lastPhotoMeta(&photoSeq, nullptr, nullptr, nullptr, nullptr);  // leaves photoSeq 0 when none held
    doc["photoSeq"] = photoSeq;
  }

  // --- current settings (populate the form once, on first load) ---
  doc["isKph"]       = is_kph;
  doc["powerSaver"]  = (bool)power_saver;
  doc["minSpeed"]    = min_speed;
  doc["photoSpeed"]  = photo_speed;
  doc["speedCorr"]   = speed_correction;
  doc["minSignal"]   = min_signal;
  doc["photoSignal"] = photo_signal;
  doc["psFront"]     = photo_signal_front;
  doc["psRear"]      = photo_signal_rear;
  doc["ssid"]        = ssid;
  doc["apiBase"]     = api_base_url;

  serializeJson(doc, out);
}

// Wall-clock cap on assembling one request body. Our bodies are tiny JSON that
// arrive in a few milliseconds, so this only ever trips on a stalled/hostile
// client (see the slow-loris note below).
#ifndef PORTAL_BODY_TIMEOUT_MS
#define PORTAL_BODY_TIMEOUT_MS (3000UL)
#endif

// Read a small request body into buf (NUL-terminated). Returns false on a recv
// error, if the body is larger than the buffer (our bodies are tiny JSON), or
// if it doesn't fully arrive within PORTAL_BODY_TIMEOUT_MS.
static bool portalReadBody(httpd_req_t* req, char* buf, size_t buf_size) {
  if (req->content_len >= buf_size) return false;
  size_t total = 0;
  const unsigned long start = millis();
  while (total < req->content_len) {
    // Bound total assembly time. Without this a slow-loris client -- one that
    // opens the socket and then dribbles or stalls the body -- pins an httpd
    // worker indefinitely on repeated recv timeouts (the `continue` below),
    // and since the portal server runs a single worker that wedges the whole
    // config UI. A legitimate body completes far inside this window.
    if ((unsigned long)(millis() - start) > PORTAL_BODY_TIMEOUT_MS) return false;
    int r = httpd_req_recv(req, buf + total, req->content_len - total);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
      return false;
    }
    total += r;
  }
  buf[total] = '\0';
  return true;
}

// --- Portal request authentication ------------------------------------------
// The config portal has no user login, so its state-changing endpoints were
// reachable by any LAN client and (worse) forgeable from any web page the owner
// merely visited, via DNS-rebinding CSRF -- which could silently repoint the
// upload endpoint and exfiltrate the Bearer token, device_token, and photos.
// Two independent defenses close the browser-driven paths:
//   (1) Host allowlist: the portal is only ever reached as an IPv4 literal
//       (AP 192.168.4.1 or the STA IP) or an <hostname>.local mDNS name. A
//       DNS-rebinding page sends its own domain in the Host header, so it is
//       rejected. Legitimate access (always IP or .local) is unaffected.
//   (2) CSRF token: a per-boot secret embedded in the served page and required
//       in an X-CSRF-Token header on every mutating POST. It lives only in the
//       same-origin page body (an attacker can't read it cross-origin), and
//       being a custom header it forces a CORS preflight we never grant, so a
//       cross-site POST can't even be sent; a blind forged POST lacks the token.
// The physical WiFi-reset button (wifiResetButton) remains an out-of-band
// recovery path, so a bad token can never lock the owner out of the device.
//
// Residual (documented, not closed here): a malicious client already on the
// device's LAN can fetch the page over plain HTTP, read the token, and drive the
// portal. Closing that needs device-side TLS or a provisioned admin password
// (a hardware/label decision), tracked separately.
static char portal_csrf[17] = {0};   // 16 hex chars + NUL; minted in load_config_portal()

// Constant-time compare so a wrong token can't be recovered byte-by-byte via
// response timing. Compares exactly n bytes.
static bool portalTokenEquals(const char* a, const char* b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
  return diff == 0;
}

// Host allowlist -- the primary DNS-rebinding defense. Only meaningful when a
// Host header is present (browsers always send one; the rebinding attack depends
// on it). Accepts an IPv4 literal (optionally :port) or a case-insensitive
// *.local name; a rebinding page sends its own domain and is rejected. Returns
// true when allowed (including when no Host header is present -- non-browser
// clients, which rebinding cannot leverage). Shared by the mutating-POST gate
// below and by the read-only GET /api/state, which discloses claim_code/SSID.
// NOTE: deliberately NOT applied to GET / (captive-portal detection probes arrive
// with foreign Hosts and must still get the page) or to GET /api/events (a public
// LAN companion feed that carries no secrets).
static bool portalHostAllowed(httpd_req_t* req) {
  char host[64] = {0};
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return true;
  char* colon = strchr(host, ':');
  if (colon) *colon = '\0';                        // drop :port
  for (char* p = host; *p; p++) {                  // lowercase in place
    if (*p >= 'A' && *p <= 'Z') *p += 32;
  }
  size_t len = strlen(host);
  bool is_local = (len >= 6) && (strcmp(host + len - 6, ".local") == 0);
  bool is_ipv4 = (len > 0);
  for (size_t i = 0; i < len; i++) {
    if (!((host[i] >= '0' && host[i] <= '9') || host[i] == '.')) { is_ipv4 = false; break; }
  }
  return is_local || is_ipv4;
}

// Gate for state-changing handlers: Host allowlist + CSRF token. On failure it
// sends 403 and returns false (the caller returns ESP_FAIL).
static bool portalRequireAuth(httpd_req_t* req) {
  // (1) Host allowlist (DNS-rebinding defense).
  if (!portalHostAllowed(req)) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "bad host");
    return false;
  }

  // (2) CSRF token: must equal the per-boot token embedded in the page.
  char tok[24] = {0};
  size_t tlen = strlen(portal_csrf);
  if (httpd_req_get_hdr_value_str(req, "X-CSRF-Token", tok, sizeof(tok)) != ESP_OK ||
      strlen(tok) != tlen || !portalTokenEquals(tok, portal_csrf, tlen)) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "auth");
    return false;
  }
  return true;
}

// GET / -> the config page (served from flash with the per-boot CSRF token
// substituted in). The page load is unauthenticated on purpose -- it is how the
// legitimate browser obtains the token it then presents on every mutating POST.
static esp_err_t portalRootGet(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  String page = CONFIG_HTML;              // PROGMEM -> heap (~5.6 KB, transient)
  page.replace("__CSRF__", portal_csrf);  // single placeholder in the <head>
  return httpd_resp_send(req, page.c_str(), page.length());
}

// GET /api/state -> JSON snapshot polled by the page.
static esp_err_t portalStateGet(httpd_req_t* req) {
  // /api/state discloses claim_code (while unclaimed) and the configured SSID, so
  // apply the same Host allowlist the mutating handlers use -- otherwise a
  // DNS-rebinding page the owner merely visits could read them cross-origin. The
  // legitimate page poll is same-origin (Host = the device IP or .local), so it
  // passes; a non-browser client (no Host header) passes too.
  if (!portalHostAllowed(req)) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "bad host");
    return ESP_FAIL;
  }
  String out;
  portalBuildState(out);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, out.c_str());
}

// GET /api/events -> recent-detection ring (events.h). Not used by the config page; it's a
// read-only feed for LAN companion displays (e.g. Blipscope "Speedscope"). Allow cross-origin
// so a browser-based companion can read it too.
static esp_err_t portalEventsGet(httpd_req_t* req) {
  String out;
  eventsBuildJson(out);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, out.c_str());
}

// GET /api/last.jpg -> the most recent capture's JPEG, held in PSRAM by
// last_photo.h. The page shows it in the "Last capture" card. Like /api/state
// this can disclose road/vehicle imagery, so it gets the same Host allowlist
// (DNS-rebinding defense) but no CSRF -- it's a read, and the page must be able
// to load it in an <img> (which can't set custom headers). Sent chunked so the
// hundreds-of-KB frame never needs a contiguous send buffer, and via the
// borrow/release protocol so a fresh capture mid-send can't free it underfoot.
static esp_err_t portalLastPhotoGet(httpd_req_t* req) {
  if (!portalHostAllowed(req)) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "bad host");
    return ESP_FAIL;
  }
  size_t len = 0;
  const uint8_t* buf = lastPhotoBorrow(&len);
  if (buf == nullptr) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no capture yet");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  // Immutable per seq: the page appends ?seq=N and only refetches when N changes,
  // so let the browser cache a given frame hard and skip re-downloading it.
  httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=31536000, immutable");
  // Chunk the JPEG so we never allocate a second full-frame contiguous buffer.
  esp_err_t rc = ESP_OK;
  const size_t CHUNK = 8192;
  for (size_t off = 0; off < len; off += CHUNK) {
    const size_t n = (len - off < CHUNK) ? (len - off) : CHUNK;
    if (httpd_resp_send_chunk(req, (const char*)buf + off, n) != ESP_OK) { rc = ESP_FAIL; break; }
  }
  lastPhotoRelease();                       // release BEFORE the terminating chunk; the buffer is no longer read
  if (rc == ESP_OK) httpd_resp_send_chunk(req, nullptr, 0);   // end of chunked response
  // On failure we skip the terminator and return ESP_FAIL, so httpd closes the socket.
  return rc;
}

// POST /api/settings -> device settings; written to globals + NVS, applied live
// (no reboot). Each field falls back to its current value if absent/malformed.
static esp_err_t portalSettingsPost(httpd_req_t* req) {
  if (!portalRequireAuth(req)) return ESP_FAIL;
  char body[512];
  if (!portalReadBody(req, body, sizeof(body))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
    return ESP_FAIL;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    return ESP_FAIL;
  }

  is_kph = doc["isKph"] | is_kph;
  // Turning Power-Saver OFF asks Core 0 to bring WiFi back immediately (matches
  // the old powerSaverCall); no-op if WiFi is already up.
  bool ps = doc["powerSaver"] | (bool)power_saver;
  if (!ps && power_saver) connect_wifi = true;
  power_saver = ps;
  min_speed          = doc["minSpeed"]    | min_speed;
  if (min_speed < 1) min_speed = 1;  // never 0: taskCore1's `while (speed >= min_speed)` would run forever (blanking the field posts 0)
  photo_speed        = doc["photoSpeed"]  | photo_speed;
  // Installation cosine correction: accept only the physically honest range
  // (see variables.h); anything outside it keeps the current value. NaN is
  // rejected too, since NaN comparisons are false.
  {
    float sc = doc["speedCorr"] | speed_correction;
    if (sc >= 1.0f && sc <= 1.3f) speed_correction = sc;
  }
  min_signal         = doc["minSignal"]   | min_signal;
  photo_signal       = doc["photoSignal"] | photo_signal;
  photo_signal_front = doc["psFront"]     | photo_signal_front;
  photo_signal_rear  = doc["psRear"]      | photo_signal_rear;

  preferences.putBool("is_kph", is_kph);
  preferences.putBool("power_saver", power_saver);
  preferences.putInt("min_speed", min_speed);
  preferences.putInt("photo_speed", photo_speed);
  preferences.putFloat("speed_corr", speed_correction);
  preferences.putInt("min_signal", min_signal);
  preferences.putInt("photo_signal", photo_signal);
  preferences.putInt("ps_front", photo_signal_front);
  preferences.putInt("ps_rear", photo_signal_rear);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// POST /api/wifi -> SSID / password / API base URL. Persisted to NVS, then the
// device reboots so connectWifiAP() + rebuildServerEndpoints() pick them up at
// startup (Core 0 never reads an endpoint String mid-edit). A blank password
// keeps the stored one, so tweaking the SSID/server needn't re-enter it (the
// one trade-off: you can't switch a previously-secured network to an open one
// from here -- use Clear Settings for that). A blank API base restores the
// compile-time default via rebuildServerEndpoints().
static esp_err_t portalWifiPost(httpd_req_t* req) {
  if (!portalRequireAuth(req)) return ESP_FAIL;
  char body[512];
  if (!portalReadBody(req, body, sizeof(body))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
    return ESP_FAIL;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    return ESP_FAIL;
  }
  String newSsid = doc["ssid"]     | "";
  String newPass = doc["password"] | "";
  String newBase = doc["apiBase"]  | "";
  newBase.trim();
  // Reject a malformed data-server base early. The auth gate above already
  // blocks a remote attacker from repointing this, but this keeps a fat-fingered
  // value (or a non-http scheme) from silently disabling uploads after reboot.
  if (newBase.length() > 0 && !newBase.startsWith("http://") && !newBase.startsWith("https://")) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "apiBase scheme");
    return ESP_FAIL;
  }
  preferences.putString("ssid", newSsid);
  if (newPass.length() > 0) preferences.putString("pass", newPass);
  preferences.putString("api_base", newBase);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  delay(200);     // let the response flush before the reboot drops the socket
  ESP.restart();
  return ESP_OK;  // not reached
}

// POST /api/clear -> wipe stored WiFi credentials and reboot. After reboot
// connectWifiAP() fails to associate and brings up the captive AP. Mirrors the
// old buttonClearNetworkCall (including preferences.end() before the restart).
static esp_err_t portalClearPost(httpd_req_t* req) {
  if (!portalRequireAuth(req)) return ESP_FAIL;
  preferences.putInt("wifi_set", 0);
  preferences.putString("ssid", "NOT_SET");
  preferences.putString("pass", "NOT_SET");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  delay(200);
  preferences.end();
  ESP.restart();
  return ESP_OK;  // not reached
}

// POST /api/stream -> {on:bool}. Only sets the desired-state flags; taskCore1's
// streamService() does the camera/server work and the auto-off timeout, so all
// of that stays on one task (matches the old aimingStreamCall).
static esp_err_t portalStreamPost(httpd_req_t* req) {
  if (!portalRequireAuth(req)) return ESP_FAIL;
  char body[128];
  if (!portalReadBody(req, body, sizeof(body))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
    return ESP_FAIL;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    return ESP_FAIL;
  }
  bool on = doc["on"] | false;
  if (on) {
    stream_deadline = millis() + STREAM_TIMEOUT_MS;
    stream_active = true;
  } else {
    stream_active = false;
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// POST /api/ota/check -> request an immediate GitHub update check. Only sets the
// flag; taskCore0's otaAutoService() runs the check (and applies any newer image)
// the next time it's idle, bypassing the periodic timer. The result appears in
// the "Update status" line the page already polls.
static esp_err_t portalOtaCheckPost(httpd_req_t* req) {
  if (!portalRequireAuth(req)) return ESP_FAIL;
  ota_check_now = true;
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static httpd_handle_t config_httpd = NULL;

/**
 * Start the config portal: launch the port-80 esp_http_server and register the
 * routes. Replaces load_espui(). Call once in setup() after WiFi is up (STA or
 * AP) so the logged URL reflects the right interface; esp_http_server binds all
 * netifs, so the page is reachable in both modes (192.168.4.1 in AP mode).
 */
void load_config_portal(void) {
  // Mint the per-boot CSRF token (hardware RNG) before serving any page. RAM
  // only -- not persisted and NOT the cloud device_token, so embedding it in the
  // page is harmless. A reboot simply invalidates open pages (they refresh).
  for (int i = 0; i < 16; i += 8) snprintf(portal_csrf + i, 9, "%08x", (unsigned)esp_random());
  portal_csrf[16] = '\0';

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port      = 80;
  config.ctrl_port        = 32768;  // default
  // Room for the long-lived async /stream socket alongside the page's ~1 Hz
  // /api/state polls; the old separate :81 stream server is gone (its sockets
  // freed), so this single server can afford more open sockets.
  config.max_open_sockets = 7;
  config.max_uri_handlers = 12;   // 10 registered (/, /api/state, /api/events, /api/last.jpg,
                                  // /api/settings, /api/wifi, /api/clear, /api/stream,
                                  // /api/ota/check, GET /stream) plus a little headroom
  config.lru_purge_enable = true;
  config.stack_size       = 8192;   // headroom for ArduinoJson + String building
  // Cap a single blocking recv so a stalled client can't pin the worker for the
  // 5s default. Matched to portalReadBody's wall-clock body cap (seconds), so
  // that cap is actually effective rather than being floored by this timeout.
  config.recv_wait_timeout = PORTAL_BODY_TIMEOUT_MS / 1000;

  if (httpd_start(&config_httpd, &config) != ESP_OK) {
    Serial.println("[PORTAL] httpd_start FAILED");
    return;
  }

  httpd_uri_t u_root   = { .uri = "/",             .method = HTTP_GET,  .handler = portalRootGet,      .user_ctx = NULL };
  httpd_uri_t u_state  = { .uri = "/api/state",    .method = HTTP_GET,  .handler = portalStateGet,     .user_ctx = NULL };
  httpd_uri_t u_events = { .uri = "/api/events",   .method = HTTP_GET,  .handler = portalEventsGet,    .user_ctx = NULL };
  httpd_uri_t u_photo  = { .uri = "/api/last.jpg", .method = HTTP_GET,  .handler = portalLastPhotoGet, .user_ctx = NULL };
  httpd_uri_t u_set    = { .uri = "/api/settings", .method = HTTP_POST, .handler = portalSettingsPost, .user_ctx = NULL };
  httpd_uri_t u_wifi   = { .uri = "/api/wifi",     .method = HTTP_POST, .handler = portalWifiPost,     .user_ctx = NULL };
  httpd_uri_t u_clear  = { .uri = "/api/clear",    .method = HTTP_POST, .handler = portalClearPost,    .user_ctx = NULL };
  httpd_uri_t u_stream = { .uri = "/api/stream",   .method = HTTP_POST, .handler = portalStreamPost,   .user_ctx = NULL };
  httpd_uri_t u_otachk = { .uri = "/api/ota/check",.method = HTTP_POST, .handler = portalOtaCheckPost, .user_ctx = NULL };
  httpd_register_uri_handler(config_httpd, &u_root);
  httpd_register_uri_handler(config_httpd, &u_state);
  httpd_register_uri_handler(config_httpd, &u_events);
  httpd_register_uri_handler(config_httpd, &u_photo);
  httpd_register_uri_handler(config_httpd, &u_set);
  httpd_register_uri_handler(config_httpd, &u_wifi);
  httpd_register_uri_handler(config_httpd, &u_clear);
  httpd_register_uri_handler(config_httpd, &u_stream);
  httpd_register_uri_handler(config_httpd, &u_otachk);

  // The aiming MJPEG stream shares this same server (GET /stream), shown inline
  // in a small window on the config page. See camera_stream.cpp.
  streamRegisterHandler(config_httpd);

  String ip = (WiFi.getMode() == WIFI_STA) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("[PORTAL] config page at http://%s/\n", ip.c_str());
}
