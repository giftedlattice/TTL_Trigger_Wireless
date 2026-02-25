/*
  ESP32 MIN132 v1 - GUI/UX V0 (NO WebSockets, NO SD)
  - SoftAP + single-page web UI
  - HTTP endpoints for actions
  - Polling for status updates
  - In-RAM event log + CSV download
  - LED PWM ramp on TRIGGER (GPIO25)
*/

#include <WiFi.h>
#include <WebServer.h>
#include "esp_timer.h"

// ---------------- Wi-Fi AP ----------------
static const char* AP_SSID = "TTL_Box_01";
static const char* AP_PASS = "trigger123";
WebServer server(80);

// ---------------- Time ----------------
static int64_t nowUs() { return esp_timer_get_time(); }
static uint32_t nowMs() { return (uint32_t)(nowUs() / 1000); }

// ---------------- State (use int to avoid Arduino enum/prototype weirdness) ----------------
static const int ST_IDLE          = 0;
static const int ST_SESSION_READY = 1;
static const int ST_ARMED         = 2;
static const int ST_RUNNING       = 3;
static int state = ST_IDLE;

static String stateNameInt(int s) {
  switch (s) {
    case ST_IDLE: return "IDLE";
    case ST_SESSION_READY: return "SESSION_READY";
    case ST_ARMED: return "ARMED";
    case ST_RUNNING: return "RUNNING";
    default: return "UNKNOWN";
  }
}

// ---------------- Session metadata ----------------
struct SessionMeta {
  String subject, project, dateStr, sessionId;
};
static SessionMeta meta;
static uint32_t trial = 0;
static int64_t lastEventUs = 0;

// ---------------- In-RAM log ----------------
struct LogEvent {
  uint32_t ms;
  String type;
  String detail;
  uint32_t trial;
};
static const int LOG_CAP = 200;
static LogEvent logBuf[LOG_CAP];
static int logHead = 0;
static int logCount = 0;

static void addLog(const String& type, const String& detail = "") {
  LogEvent &e = logBuf[logHead];
  e.ms = nowMs();
  e.type = type;
  e.detail = detail;
  e.trial = trial;
  logHead = (logHead + 1) % LOG_CAP;
  if (logCount < LOG_CAP) logCount++;
  lastEventUs = nowUs();
}

static String esc(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  s.replace("\r", "\\r");
  return s;
}

static String makeSessionId() {
  String sid = meta.subject.length() ? meta.subject : "Subject";
  sid += "_";
  sid += meta.project.length() ? meta.project : "Project";
  sid += "_";
  sid += String((uint32_t)millis());
  sid.replace(" ", "_");
  sid.replace("/", "_");
  sid.replace("\\", "_");
  return sid;
}

// ---------------- LED PWM (compat across ESP32 core 2.x & 3.x) ----------------
static const int LED_PWM_PIN = 25;

// PWM parameters
static const int PWM_FREQ_HZ = 10000;
static const int PWM_BITS    = 12;
static const int PWM_MAX     = (1 << PWM_BITS) - 1;

// Ramp parameters
static const uint32_t RAMP_UP_MS   = 300;
static const uint32_t RAMP_DOWN_MS = 300;
static const uint32_t STEP_MS      = 5;

// We will try new API first (core 3.x), else fallback to old
static bool pwmInited = false;

// Forward declare wrappers
static void pwmInit();
static void pwmWriteDuty(uint16_t duty);

static void pwmInit() {
  if (pwmInited) return;

  // --- ESP32 Arduino core 3.x style ---
  // ledcAttach(pin, freq, resolution_bits)
  // If your core doesn't have this symbol, compilation will fail,
  // so we use a preprocessor trick: check if LEDC is available via ESP_ARDUINO_VERSION.
  // If that macro isn't present, we still fall back to old API below.

#if defined(ESP_ARDUINO_VERSION) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  // New API
  ledcAttach(LED_PWM_PIN, PWM_FREQ_HZ, PWM_BITS);
  pwmInited = true;
#else
  // Old API (core 2.x): ledcSetup + ledcAttachPin + ledcWrite(channel, duty)
  const int ch = 0;
  ledcSetup(ch, PWM_FREQ_HZ, PWM_BITS);
  ledcAttachPin(LED_PWM_PIN, ch);
  pwmInited = true;
#endif

  pwmWriteDuty(0);
}

static void pwmWriteDuty(uint16_t duty) {
  if (!pwmInited) pwmInit();
  if (duty > PWM_MAX) duty = PWM_MAX;

#if defined(ESP_ARDUINO_VERSION) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  // New API: ledcWrite(pin, duty)
  ledcWrite(LED_PWM_PIN, duty);
#else
  // Old API: ledcWrite(channel, duty)
  const int ch = 0;
  ledcWrite(ch, duty);
#endif
}

static void ledRampOnce() {
  const uint32_t stepsUp = max<uint32_t>(1, RAMP_UP_MS / STEP_MS);
  for (uint32_t i = 0; i <= stepsUp; i++) {
    uint16_t duty = (uint16_t)((uint32_t)PWM_MAX * i / stepsUp);
    pwmWriteDuty(duty);
    delay(STEP_MS);
  }

  const uint32_t stepsDown = max<uint32_t>(1, RAMP_DOWN_MS / STEP_MS);
  for (uint32_t i = 0; i <= stepsDown; i++) {
    uint16_t duty = (uint16_t)((uint32_t)PWM_MAX * (stepsDown - i) / stepsDown);
    pwmWriteDuty(duty);
    delay(STEP_MS);
  }
  pwmWriteDuty(0);
}

// ---------------- UI (single page) ----------------
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>TTL Box</title>
  <style>
    body{font-family:system-ui;margin:0;background:#fafafa;color:#111}
    header{padding:16px 18px;background:#111;color:#fff}
    header h1{font-size:18px;margin:0}
    main{padding:18px;max-width:760px;margin:0 auto}
    .card{background:#fff;border:1px solid #e7e7e7;border-radius:18px;padding:16px;margin:12px 0;box-shadow:0 1px 2px rgba(0,0,0,.04)}
    label{display:block;font-size:13px;color:#444;margin:10px 0 6px}
    input,button{width:100%;box-sizing:border-box;font-size:16px;padding:12px;border-radius:14px;border:1px solid #ddd;background:#fff}
    input:disabled{background:#f4f4f4;color:#666}
    button{border:0;background:#111;color:#fff;cursor:pointer}
    button.secondary{background:#eaeaea;color:#111}
    button.danger{background:#b00020}
    .grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
    .grid3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}
    .pill{display:inline-block;padding:6px 10px;border-radius:999px;font-size:13px}
    .pIdle{background:#fff2cc}
    .pReady{background:#d9edf7}
    .pArmed{background:#d4edda}
    .pRun{background:#ffe0e0}
    .muted{color:#666;font-size:13px}
    .big{font-size:18px;padding:16px;border-radius:18px}
    .log{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;white-space:pre-wrap;background:#0b0b0b;color:#d7ffd7;border-radius:14px;padding:12px;max-height:180px;overflow:auto}
  </style>
</head>
<body>
<header><h1>TTL Box (Local)</h1></header>
<main>

  <div class="card">
    <div>
      <span id="pill" class="pill pIdle">IDLE</span>
      <div class="muted" id="subline">Connecting…</div>
    </div>
  </div>

  <div class="card" id="setupCard">
    <h3 style="margin:0 0 10px 0;font-size:16px;">Session Setup</h3>
    <label>Subject</label>
    <input id="subject" placeholder="e.g., Bat12">
    <label>Project</label>
    <input id="project" placeholder="e.g., MazePilot">
    <label>Date</label>
    <input id="dateStr" placeholder="YYYY-MM-DD">

    <div class="grid" style="margin-top:12px;">
      <button class="big" onclick="startSession()">Start Session</button>
      <button class="big secondary" onclick="resetAll()">Reset</button>
    </div>
    <div class="muted" style="margin-top:10px;">Starting a session locks metadata so trials stay consistent.</div>
  </div>

  <div class="card" id="runtimeCard" style="display:none;">
    <h3 style="margin:0 0 10px 0;font-size:16px;">Run Controls</h3>

    <div class="grid">
      <button class="big" onclick="arm()">ARM</button>
      <button class="big secondary" onclick="disarm()">DISARM</button>
    </div>

    <div id="armedControls" style="display:none;margin-top:12px;">
      <div class="grid3">
        <button class="big" onclick="trigger()">TRIGGER</button>
        <button class="big secondary" onclick="nextTrial()">NEXT TRIAL</button>
        <button class="big danger" onclick="abortRun()">ABORT</button>
      </div>
      <div class="muted" style="margin-top:10px;">TRIGGER runs LED ramp on GPIO25.</div>
    </div>

    <div class="grid" style="margin-top:12px;">
      <button class="secondary" onclick="downloadCsv()">Download CSV</button>
      <button class="secondary" onclick="refreshLogs()">Refresh Logs</button>
    </div>

    <div style="margin-top:12px;">
      <div class="muted">Recent events</div>
      <div class="log" id="logBox">(none)</div>
    </div>
  </div>

<script>
let status=null;

function setPill(st){
  const pill=document.getElementById('pill');
  pill.textContent=st;
  pill.className="pill " + (st==="IDLE"?"pIdle":st==="SESSION_READY"?"pReady":st==="ARMED"?"pArmed":"pRun");
}

function setUI(){
  if(!status) return;
  setPill(status.state);
  const sub=[];
  if(status.sessionId) sub.push("Session: "+status.sessionId);
  sub.push("Trial: "+status.trial);
  document.getElementById('subline').textContent=sub.join(" • ");

  const locked=(status.state!=="IDLE");
  document.getElementById('subject').disabled=locked;
  document.getElementById('project').disabled=locked;
  document.getElementById('dateStr').disabled=locked;

  document.getElementById('setupCard').style.display = (status.state==="IDLE") ? "" : "none";
  document.getElementById('runtimeCard').style.display = (status.state==="IDLE") ? "none" : "";
  document.getElementById('armedControls').style.display =
    (status.state==="ARMED" || status.state==="RUNNING") ? "" : "none";
}

async function post(path, data){
  const res=await fetch(path,{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(data||{})});
  if(!res.ok){ console.log("ERR", await res.text()); }
  return res;
}

async function startSession(){
  const subject=document.getElementById('subject').value||"Subject";
  const project=document.getElementById('project').value||"Project";
  let dateStr=document.getElementById('dateStr').value;
  if(!dateStr){
    dateStr=new Date().toISOString().slice(0,10);
    document.getElementById('dateStr').value=dateStr;
  }
  await post("/api/session/start",{subject,project,dateStr});
  await refreshStatus();
  await refreshLogs();
}
async function resetAll(){ await post("/api/reset",{}); await refreshStatus(); await refreshLogs(); }
async function arm(){ await post("/api/arm",{}); await refreshStatus(); await refreshLogs(); }
async function disarm(){ await post("/api/disarm",{}); await refreshStatus(); await refreshLogs(); }
async function trigger(){ await post("/api/trigger",{}); await refreshStatus(); await refreshLogs(); }
async function nextTrial(){ await post("/api/next",{}); await refreshStatus(); await refreshLogs(); }
async function abortRun(){ await post("/api/abort",{}); await refreshStatus(); await refreshLogs(); }

function downloadCsv(){ window.location="/api/log.csv"; }

async function refreshLogs(){
  const res=await fetch("/api/log.tail");
  document.getElementById('logBox').textContent=await res.text();
}
async function refreshStatus(){
  const res=await fetch("/api/status");
  status=await res.json();
  setUI();
}
setInterval(refreshStatus, 500);
refreshStatus();
refreshLogs();
</script>
</main>
</body>
</html>
)HTML";

// ---------------- API helpers ----------------
static String getJsonVal(const String& body, const char* key) {
  String k = String("\"") + key + "\":";
  int i = body.indexOf(k);
  if (i < 0) return "";
  i += k.length();
  while (i < (int)body.length() && body[i] == ' ') i++;
  bool quoted = (i < (int)body.length() && body[i] == '"');
  if (quoted) i++;
  int j = i;
  while (j < (int)body.length()) {
    char c = body[j];
    if (quoted) { if (c == '"') break; }
    else { if (c == ',' || c == '}' || c == ' ') break; }
    j++;
  }
  return body.substring(i, j);
}

static void sendStatus() {
  String j = "{";
  j += "\"state\":\"" + stateNameInt(state) + "\",";
  j += "\"trial\":" + String(trial) + ",";
  j += "\"sessionId\":\"" + esc(meta.sessionId) + "\",";
  j += "\"subject\":\"" + esc(meta.subject) + "\",";
  j += "\"project\":\"" + esc(meta.project) + "\",";
  j += "\"dateStr\":\"" + esc(meta.dateStr) + "\"";
  j += "}";
  server.send(200, "application/json", j);
}

static void apiSessionStart() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "Missing body"); return; }
  if (state != ST_IDLE) { server.send(409, "text/plain", "Session already active"); return; }

  String body = server.arg("plain");
  meta.subject = getJsonVal(body, "subject");
  meta.project = getJsonVal(body, "project");
  meta.dateStr = getJsonVal(body, "dateStr");
  meta.sessionId = makeSessionId();

  trial = 0;
  state = ST_SESSION_READY;
  addLog("SESSION_START", meta.sessionId);
  server.send(200, "text/plain", "OK");
}

static void apiReset() {
  meta = SessionMeta{};
  trial = 0;
  logHead = 0; logCount = 0;
  addLog("RESET", "");
  state = ST_IDLE;
  pwmWriteDuty(0);
  server.send(200, "text/plain", "OK");
}

static void apiArm() {
  if (state == ST_IDLE) { server.send(400, "text/plain", "Start session first"); return; }
  if (state != ST_SESSION_READY) { server.send(409, "text/plain", "Invalid state"); return; }
  state = ST_ARMED;
  addLog("ARM", "");
  server.send(200, "text/plain", "OK");
}

static void apiDisarm() {
  if (state != ST_ARMED && state != ST_RUNNING) { server.send(409, "text/plain", "Invalid state"); return; }
  state = ST_SESSION_READY;
  addLog("DISARM", "");
  pwmWriteDuty(0);
  server.send(200, "text/plain", "OK");
}

static void apiTrigger() {
  if (state != ST_ARMED) { server.send(409, "text/plain", "Not armed"); return; }

  state = ST_RUNNING;
  addLog("TRIGGER", "manual_gui");
  addLog("RAMP_START", "GPIO25 PWM");

  ledRampOnce();

  addLog("RAMP_END", "");
  addLog("RUN_COMPLETE", "");
  trial++;
  state = ST_ARMED;

  server.send(200, "text/plain", "OK");
}

static void apiNext() {
  if (state == ST_IDLE) { server.send(409, "text/plain", "No session"); return; }
  trial++;
  addLog("NEXT_TRIAL", "");
  server.send(200, "text/plain", "OK");
}

static void apiAbort() {
  if (state == ST_IDLE) { server.send(409, "text/plain", "No session"); return; }
  state = ST_SESSION_READY;
  addLog("ABORT", "");
  pwmWriteDuty(0);
  server.send(200, "text/plain", "OK");
}

static void handleLogCsv() {
  String out = "ms,type,detail,trial\n";
  int start = (logHead - logCount + LOG_CAP) % LOG_CAP;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % LOG_CAP;
    out += String(logBuf[idx].ms) + ",";
    out += "\"" + esc(logBuf[idx].type) + "\",";
    out += "\"" + esc(logBuf[idx].detail) + "\",";
    out += String(logBuf[idx].trial) + "\n";
  }
  server.send(200, "text/csv", out);
}

static void handleLogTail() {
  String out;
  int show = min(30, logCount);
  int start = (logHead - show + LOG_CAP) % LOG_CAP;
  for (int i = 0; i < show; i++) {
    int idx = (start + i) % LOG_CAP;
    out += String(logBuf[idx].ms) + "  ";
    out += logBuf[idx].type;
    if (logBuf[idx].detail.length()) out += " (" + logBuf[idx].detail + ")";
    out += "  trial=" + String(logBuf[idx].trial) + "\n";
  }
  server.send(200, "text/plain", out);
}

static void handleRoot() { server.send(200, "text/html", INDEX_HTML); }
static void handleRedirectAny() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
  server.send(302, "text/plain", "Redirecting");
}

// ---------------- Setup/Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(150);

  pwmInit(); // setup PWM on GPIO25

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("AP SSID: "); Serial.println(AP_SSID);
  Serial.print("AP IP:   "); Serial.println(ip);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, sendStatus);

  server.on("/api/session/start", HTTP_POST, apiSessionStart);
  server.on("/api/reset", HTTP_POST, apiReset);
  server.on("/api/arm", HTTP_POST, apiArm);
  server.on("/api/disarm", HTTP_POST, apiDisarm);
  server.on("/api/trigger", HTTP_POST, apiTrigger);
  server.on("/api/next", HTTP_POST, apiNext);
  server.on("/api/abort", HTTP_POST, apiAbort);

  server.on("/api/log.csv", HTTP_GET, handleLogCsv);
  server.on("/api/log.tail", HTTP_GET, handleLogTail);

  server.onNotFound(handleRedirectAny);

  server.begin();
  addLog("BOOT", ip.toString());
}

void loop() {
  server.handleClient();
}