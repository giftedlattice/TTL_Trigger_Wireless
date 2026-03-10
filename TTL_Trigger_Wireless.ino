/*
  ESP32 TTL Box - Browser Controlled Trial Logic
  ------------------------------------------------
  Browser UI over SoftAP:
    - Start Session
    - Arm / Disarm
    - Start Trial
    - End Trial
    - Abort
    - Download CSV log

  Trial logic:
    START TRIAL:
      - records trialStartTime
      - LED ON immediately
      - mousePin HIGH briefly, then auto-off

    END TRIAL:
      - computes reaction time
      - LED OFF
      - mousePin LOW
      - pulses fSyncPin + ttlPin4 + ttlPin6 HIGH together for 20 ms

  Notes:
    - Non-blocking LED auto-off
    - Non-blocking mouse TTL auto-off
    - No physical button required
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

// ---------------- Pin map ----------------
// CHANGE THESE FOR YOUR BOARD
//
// IMPORTANT:
// On many ESP32 boards, GPIO 6-11 are reserved for flash and should not be used.
// Your original Arduino sketch used 11, 7, 6. Those are usually NOT safe on ESP32.
//
// Suggested safe example mapping:
const int ledPin   = 25;   // LED output
const int mousePin = 26;   // mouse TTL output
const int fSyncPin = 27;   // camera sync output
const int ttlPin4  = 32;   // TTL output
const int ttlPin6  = 33;   // TTL output

// ---------------- Timing ----------------
const unsigned long ledOnDuration   = 6000; // ms
const unsigned long mouseOnDuration = 20;   // ms
const unsigned long recordPulseMs   = 20;   // ms

// ---------------- State ----------------
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

// ---------------- Trial runtime ----------------
static bool trialInProgress = false;
static unsigned long trialStartTime = 0;
static unsigned long lastReactionTime = 0;

// LED auto-off
static bool ledAutoOffArmed = false;
static unsigned long ledOffAt = 0;

// Mouse auto-off
static bool mouseAutoOffArmed = false;
static unsigned long mouseOffAt = 0;

// ---------------- In-RAM log ----------------
struct LogEvent {
  uint32_t ms;
  String type;
  String detail;
  uint32_t trial;
};

static const int LOG_CAP = 300;
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

// ---------------- Core trial functions ----------------
static void mouseClick() {
  digitalWrite(mousePin, LOW);
  delay(30);

  digitalWrite(mousePin, HIGH);
  delay(10);
  digitalWrite(mousePin, LOW);
  delay(10);
}

static void triggerRecording() {
  // Set low first
  digitalWrite(fSyncPin, LOW);
  digitalWrite(ttlPin4, LOW);
  digitalWrite(ttlPin6, LOW);

  // Pulse all high together
  digitalWrite(fSyncPin, HIGH);
  digitalWrite(ttlPin4, HIGH);
  digitalWrite(ttlPin6, HIGH);

  addLog("RECORD_PULSE_HIGH", "fSync+ttl4+ttl6");
  Serial.println("F-Sync, TTL4, TTL6 HIGH");

  delay(recordPulseMs);

  // Return low
  digitalWrite(fSyncPin, LOW);
  digitalWrite(ttlPin4, LOW);
  digitalWrite(ttlPin6, LOW);

  addLog("RECORD_PULSE_LOW", "fSync+ttl4+ttl6");
  Serial.println("F-Sync, TTL4, TTL6 LOW");
}

static void startTrialLogic() {
  Serial.println("Starting Trial");

  trialStartTime = millis();
  trialInProgress = true;

  // LED ON immediately
  digitalWrite(ledPin, HIGH);
  ledAutoOffArmed = true;
  ledOffAt = trialStartTime + ledOnDuration;

  // Mouse TTL HIGH briefly, then auto off
  digitalWrite(mousePin, HIGH);
  mouseAutoOffArmed = true;
  mouseOffAt = trialStartTime + mouseOnDuration;

  addLog("TRIAL_START", "LED_ON mousePin_HIGH");
  addLog("LED_ON", "auto_off_armed");
  addLog("MOUSE_HIGH", "trial_start_ttl");

  Serial.print("Trial Start Time: ");
  Serial.println(trialStartTime);
}

static void endTrialLogic() {
  Serial.println("Ending Trial");

  unsigned long trialEndTime = millis();
  lastReactionTime = trialEndTime - trialStartTime;

  // Turn LED off
  digitalWrite(ledPin, LOW);
  ledAutoOffArmed = false;

  // Force mouse LOW
  digitalWrite(mousePin, LOW);
  mouseAutoOffArmed = false;

  addLog("TRIAL_END", "reaction_ms=" + String(lastReactionTime));
  addLog("LED_OFF", "trial_end");
  addLog("MOUSE_LOW", "trial_end");

  Serial.print("Reaction Time (ms): ");
  Serial.println(lastReactionTime);

  // Trigger recording pulse
  triggerRecording();

  trialInProgress = false;
  trial++;
}

// ---------------- UI ----------------
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
    main{padding:18px;max-width:860px;margin:0 auto}
    .card{background:#fff;border:1px solid #e7e7e7;border-radius:18px;padding:16px;margin:12px 0;box-shadow:0 1px 2px rgba(0,0,0,.04)}
    label{display:block;font-size:13px;color:#444;margin:10px 0 6px}
    input,button{width:100%;box-sizing:border-box;font-size:16px;padding:12px;border-radius:14px;border:1px solid #ddd;background:#fff}
    input:disabled{background:#f4f4f4;color:#666}
    button{border:0;background:#111;color:#fff;cursor:pointer}
    button.secondary{background:#eaeaea;color:#111}
    button.danger{background:#b00020}
    button.good{background:#0f8a3b}
    button.warn{background:#b36b00}
    .grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
    .grid3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}
    .pill{display:inline-block;padding:6px 10px;border-radius:999px;font-size:13px}
    .pIdle{background:#fff2cc}
    .pReady{background:#d9edf7}
    .pArmed{background:#d4edda}
    .pRun{background:#ffe0e0}
    .muted{color:#666;font-size:13px}
    .big{font-size:18px;padding:16px;border-radius:18px}
    .log{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;white-space:pre-wrap;background:#0b0b0b;color:#d7ffd7;border-radius:14px;padding:12px;max-height:240px;overflow:auto}
    .row{display:flex;gap:18px;flex-wrap:wrap}
    .stat{min-width:140px}
    .stat .k{font-size:12px;color:#666}
    .stat .v{font-size:20px;font-weight:700}
  </style>
</head>
<body>
<header><h1>TTL Box (Local Browser Control)</h1></header>
<main>

  <div class="card">
    <span id="pill" class="pill pIdle">IDLE</span>
    <div class="muted" id="subline">Connecting…</div>
    <div class="row" style="margin-top:12px;">
      <div class="stat"><div class="k">Trial</div><div class="v" id="trialNum">0</div></div>
      <div class="stat"><div class="k">Trial Active</div><div class="v" id="trialActive">NO</div></div>
      <div class="stat"><div class="k">Last Reaction (ms)</div><div class="v" id="rtVal">0</div></div>
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
  </div>

  <div class="card" id="runtimeCard" style="display:none;">
    <h3 style="margin:0 0 10px 0;font-size:16px;">Session Controls</h3>

    <div class="grid">
      <button class="big" onclick="arm()">ARM</button>
      <button class="big secondary" onclick="disarm()">DISARM</button>
    </div>

    <div id="armedControls" style="display:none;margin-top:12px;">
      <div class="grid">
        <button class="big good" onclick="startTrial()">START TRIAL</button>
        <button class="big warn" onclick="endTrial()">END TRIAL</button>
      </div>

      <div class="grid3" style="margin-top:10px;">
        <button class="secondary" onclick="manualRecordPulse()">Manual Record Pulse</button>
        <button class="secondary" onclick="manualMouseClick()">Mouse Click</button>
        <button class="danger" onclick="abortRun()">ABORT</button>
      </div>

      <div class="muted" style="margin-top:10px;">
        Start Trial = LED on + mouse TTL.<br>
        End Trial = compute reaction time + pulse F-Sync / TTL4 / TTL6.
      </div>
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

  document.getElementById('trialNum').textContent = status.trial;
  document.getElementById('trialActive').textContent = status.trialInProgress ? "YES" : "NO";
  document.getElementById('rtVal').textContent = status.lastReactionTime;

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
  const res = await fetch(path, {
    method:"POST",
    headers:{"Content-Type":"application/json"},
    body:JSON.stringify(data||{})
  });
  if(!res.ok){
    const txt = await res.text();
    console.log("ERR", txt);
    alert(txt);
  }
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
async function startTrial(){ await post("/api/trial/start",{}); await refreshStatus(); await refreshLogs(); }
async function endTrial(){ await post("/api/trial/end",{}); await refreshStatus(); await refreshLogs(); }
async function abortRun(){ await post("/api/abort",{}); await refreshStatus(); await refreshLogs(); }
async function manualRecordPulse(){ await post("/api/manual/recordpulse",{}); await refreshStatus(); await refreshLogs(); }
async function manualMouseClick(){ await post("/api/manual/mouseclick",{}); await refreshStatus(); await refreshLogs(); }

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
    if (quoted) {
      if (c == '"') break;
    } else {
      if (c == ',' || c == '}' || c == ' ') break;
    }
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
  j += "\"dateStr\":\"" + esc(meta.dateStr) + "\",";
  j += "\"trialInProgress\":" + String(trialInProgress ? "true" : "false") + ",";
  j += "\"lastReactionTime\":" + String(lastReactionTime);
  j += "}";
  server.send(200, "application/json", j);
}

static void apiSessionStart() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing body");
    return;
  }
  if (state != ST_IDLE) {
    server.send(409, "text/plain", "Session already active");
    return;
  }

  String body = server.arg("plain");
  meta.subject = getJsonVal(body, "subject");
  meta.project = getJsonVal(body, "project");
  meta.dateStr = getJsonVal(body, "dateStr");
  meta.sessionId = makeSessionId();

  trial = 0;
  lastReactionTime = 0;
  trialInProgress = false;
  state = ST_SESSION_READY;

  addLog("SESSION_START", meta.sessionId);
  server.send(200, "text/plain", "OK");
}

static void apiReset() {
  // Force outputs safe
  digitalWrite(ledPin, LOW);
  digitalWrite(mousePin, LOW);
  digitalWrite(fSyncPin, LOW);
  digitalWrite(ttlPin4, LOW);
  digitalWrite(ttlPin6, LOW);

  meta = SessionMeta{};
  trial = 0;
  lastReactionTime = 0;
  trialInProgress = false;
  ledAutoOffArmed = false;
  mouseAutoOffArmed = false;
  state = ST_IDLE;

  logHead = 0;
  logCount = 0;
  addLog("RESET", "");

  server.send(200, "text/plain", "OK");
}

static void apiArm() {
  if (state == ST_IDLE) {
    server.send(400, "text/plain", "Start session first");
    return;
  }
  if (state != ST_SESSION_READY) {
    server.send(409, "text/plain", "Invalid state");
    return;
  }
  state = ST_ARMED;
  addLog("ARM", "");
  server.send(200, "text/plain", "OK");
}

static void apiDisarm() {
  if (state != ST_ARMED && state != ST_RUNNING) {
    server.send(409, "text/plain", "Invalid state");
    return;
  }
  if (trialInProgress) {
    server.send(409, "text/plain", "Cannot disarm while trial is active");
    return;
  }
  state = ST_SESSION_READY;
  addLog("DISARM", "");
  server.send(200, "text/plain", "OK");
}

static void apiTrialStart() {
  if (state != ST_ARMED) {
    server.send(409, "text/plain", "System not armed");
    return;
  }
  if (trialInProgress) {
    server.send(409, "text/plain", "Trial already in progress");
    return;
  }

  state = ST_RUNNING;
  startTrialLogic();
  server.send(200, "text/plain", "OK");
}

static void apiTrialEnd() {
  if (state != ST_RUNNING) {
    server.send(409, "text/plain", "No running trial");
    return;
  }
  if (!trialInProgress) {
    server.send(409, "text/plain", "Trial not in progress");
    return;
  }

  endTrialLogic();
  state = ST_ARMED;  // ready for next browser-triggered trial
  server.send(200, "text/plain", "OK");
}

static void apiAbort() {
  if (state == ST_IDLE) {
    server.send(409, "text/plain", "No session");
    return;
  }

  digitalWrite(ledPin, LOW);
  digitalWrite(mousePin, LOW);
  digitalWrite(fSyncPin, LOW);
  digitalWrite(ttlPin4, LOW);
  digitalWrite(ttlPin6, LOW);

  ledAutoOffArmed = false;
  mouseAutoOffArmed = false;
  trialInProgress = false;
  state = ST_SESSION_READY;

  addLog("ABORT", "all_outputs_low");
  server.send(200, "text/plain", "OK");
}

static void apiManualRecordPulse() {
  if (state == ST_IDLE) {
    server.send(409, "text/plain", "No session");
    return;
  }
  triggerRecording();
  server.send(200, "text/plain", "OK");
}

static void apiManualMouseClick() {
  if (state == ST_IDLE) {
    server.send(409, "text/plain", "No session");
    return;
  }
  addLog("MOUSE_CLICK", "manual");
  mouseClick();
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
  int show = min(40, logCount);
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

static void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

static void handleRedirectAny() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
  server.send(302, "text/plain", "Redirecting");
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(150);

  // Pin configuration
  pinMode(ledPin, OUTPUT);
  pinMode(mousePin, OUTPUT);
  pinMode(fSyncPin, OUTPUT);
  pinMode(ttlPin4, OUTPUT);
  pinMode(ttlPin6, OUTPUT);

  digitalWrite(ledPin, LOW);
  digitalWrite(mousePin, LOW);
  digitalWrite(fSyncPin, LOW);
  digitalWrite(ttlPin4, LOW);
  digitalWrite(ttlPin6, LOW);

  // Wi-Fi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 8);  // allow 8 clients

  IPAddress ip = WiFi.softAPIP();

  Serial.print("AP SSID: ");
  Serial.println(AP_SSID);

  Serial.print("AP IP: ");
  Serial.println(ip);

  Serial.print("Max clients: ");
  Serial.println(WiFi.softAPgetStationNum());

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, sendStatus);

  server.on("/api/session/start", HTTP_POST, apiSessionStart);
  server.on("/api/reset", HTTP_POST, apiReset);
  server.on("/api/arm", HTTP_POST, apiArm);
  server.on("/api/disarm", HTTP_POST, apiDisarm);

  server.on("/api/trial/start", HTTP_POST, apiTrialStart);
  server.on("/api/trial/end", HTTP_POST, apiTrialEnd);
  server.on("/api/abort", HTTP_POST, apiAbort);

  server.on("/api/manual/recordpulse", HTTP_POST, apiManualRecordPulse);
  server.on("/api/manual/mouseclick", HTTP_POST, apiManualMouseClick);

  server.on("/api/log.csv", HTTP_GET, handleLogCsv);
  server.on("/api/log.tail", HTTP_GET, handleLogTail);

  server.onNotFound(handleRedirectAny);

  server.begin();
  addLog("BOOT", ip.toString());
}

void loop() {
  server.handleClient();

  unsigned long now = millis();

  // Non-blocking LED auto-off
  if (ledAutoOffArmed && now >= ledOffAt) {
    digitalWrite(ledPin, LOW);
    ledAutoOffArmed = false;
    addLog("LED_AUTO_OFF", "timeout");
    Serial.println("LED Auto-Off elapsed");
  }

  // Non-blocking mouse auto-off
  if (mouseAutoOffArmed && now >= mouseOffAt) {
    digitalWrite(mousePin, LOW);
    mouseAutoOffArmed = false;
    addLog("MOUSE_AUTO_OFF", "timeout");
    Serial.println("MousePin Auto-Off elapsed");
  }
}