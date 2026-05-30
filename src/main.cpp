#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>

enum ChargerState {
  IDLE,
  CONNECTED,
  AUTHORIZED,
  CHARGING,
  ERROR_STATE
};

// WLAN STA Zugangsdaten
const char* STA_SSID = "internet";
const char* STA_PASSWORD = "internet";

// Lokaler AP
const char* AP_PASSWORD = "chargecloud"; // mindestens 8 Zeichen
String deviceId;
String apSsid;
String currentDateTimeString();

WebServer server(80);
Preferences prefs;

#define NEOPIXEL_PIN 48
#define NEOPIXEL_COUNT 1
Adafruit_NeoPixel pixel(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

const float ENERGY_PRICE_EUR_PER_KWH = 5.00f;
const unsigned long METER_INTERVAL_MS = 10000;
const int MAX_SESSIONS = 10;

struct ChargeSession {
  bool valid;
  uint32_t id;
  String startTime;
  String endTime;
  float energyKwh;
  float costEur;
};

ChargeSession sessions[MAX_SESSIONS];
int sessionCount = 0;
uint32_t nextSessionId = 1;
bool sessionActive = false;
String activeSessionStartTime = "";
unsigned long lastEnergyMillis = 0;
float activeEnergyKwh = 0.0f;
ChargerState previousState = IDLE;

const int MAX_DEBUG_LOGS = 30;
String debugLogs[MAX_DEBUG_LOGS];
int debugLogCount = 0;

bool lastVehicleConnected = false;
bool lastStartPressed = false;
bool lastStopPressed = false;
bool lastErrorActive = false;
ChargerState lastLoggedState = IDLE;

void addDebugLog(const String& msg) {
  String line = currentDateTimeString() + " | " + msg;
  Serial.println(line);
  if (debugLogCount < MAX_DEBUG_LOGS) {
    debugLogs[debugLogCount++] = line;
  } else {
    for (int i = 1; i < MAX_DEBUG_LOGS; i++) debugLogs[i - 1] = debugLogs[i];
    debugLogs[MAX_DEBUG_LOGS - 1] = line;
  }
}

// ESP32-S3 Ladestations-Simulation
// Verwendet GPIO3 bis GPIO11. GPIO1 und GPIO2 bleiben frei.

// Eingaenge
const int PIN_POWER_POTI        = 3;  // ADC: Poti fuer 0..22 kW
const int PIN_VEHICLE_CONNECTED = 4;  // Schalter nach GND
const int PIN_START_AUTH        = 5;  // Taster nach GND
const int PIN_STOP              = 6;  // Taster nach GND
const int PIN_ERROR             = 7;  // Schalter/Taster nach GND

// Ausgaenge
const int LED_AVAILABLE = 8;
const int LED_PREPARING = 9;
const int LED_CHARGING  = 10;
const int LED_FAULTED   = 11;

ChargerState state = IDLE;
unsigned long lastPowerPrint = 0;

bool readActiveLow(int pin) {
  return digitalRead(pin) == LOW;
}

const unsigned long DEBOUNCE_MS = 50;
struct DebouncedInput {
  int pin;
  bool stableState;
  bool lastRawState;
  unsigned long lastChange;
};
DebouncedInput inputVehicle = {PIN_VEHICLE_CONNECTED, false, false, 0};
DebouncedInput inputStart   = {PIN_START_AUTH, false, false, 0};
DebouncedInput inputStop    = {PIN_STOP, false, false, 0};
DebouncedInput inputError   = {PIN_ERROR, false, false, 0};

bool updateDebouncedInput(DebouncedInput &input) {
  bool rawState = digitalRead(input.pin) == LOW;
  if (rawState != input.lastRawState) {
    input.lastChange = millis();
    input.lastRawState = rawState;
  }
  if ((millis() - input.lastChange) >= DEBOUNCE_MS) {
    input.stableState = rawState;
  }
  return input.stableState;
}

void setAllLedsOff() {
  digitalWrite(LED_AVAILABLE, LOW);
  digitalWrite(LED_PREPARING, LOW);
  digitalWrite(LED_CHARGING, LOW);
  digitalWrite(LED_FAULTED, LOW);
}

int readPowerAdc() {
  return analogRead(PIN_POWER_POTI);
}

float adcToPowerKw(int adcValue) {
  return (adcValue / 4095.0f) * 22.0f;
}

float readChargingPowerKw() {
  return adcToPowerKw(readPowerAdc());
}

float powerKwToCurrentA(float powerKw) {
  // 3-phasig, 400 V, cos phi ca. 1
  float powerW = powerKw * 1000.0f;
  return powerW / (1.732f * 400.0f);
}

const char* stateName(ChargerState s) {
  switch (s) {
    case IDLE: return "IDLE";
    case CONNECTED: return "CONNECTED";
    case AUTHORIZED: return "AUTHORIZED";
    case CHARGING: return "CHARGING";
    case ERROR_STATE: return "ERROR";
    default: return "UNKNOWN";
  }
}

void updateLeds() {
  setAllLedsOff();

  switch (state) {
    case IDLE:
      digitalWrite(LED_AVAILABLE, HIGH);
      break;
    case CONNECTED:
      digitalWrite(LED_PREPARING, HIGH);
      break;
    case AUTHORIZED:
      digitalWrite(LED_PREPARING, HIGH);
      break;
    case CHARGING:
      digitalWrite(LED_CHARGING, HIGH);
      break;
    case ERROR_STATE:
      digitalWrite(LED_FAULTED, HIGH);
      break;
  }
}

String getDeviceId() {
  uint64_t chipId = ESP.getEfuseMac();
  char id[13];
  snprintf(id, sizeof(id), "%04X%08X", (uint16_t)(chipId >> 32), (uint32_t)chipId);
  return String(id);
}

String currentDateTimeString() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 50)) {
    char buffer[24];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buffer);
  }

  unsigned long seconds = millis() / 1000;
  unsigned long h = seconds / 3600;
  unsigned long m = (seconds % 3600) / 60;
  unsigned long sec = seconds % 60;
  char fallback[24];
  snprintf(fallback, sizeof(fallback), "Uptime %02lu:%02lu:%02lu", h, m, sec);
  return String(fallback);
}

void setupTimeSync() {
  configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");
  Serial.println("Zeitsynchronisation gestartet");
}

void saveSessions() {
  prefs.begin("sessions", false);
  prefs.putUInt("nextId", nextSessionId);
  prefs.putInt("count", sessionCount);
  for (int i = 0; i < sessionCount; i++) {
    String prefix = "s" + String(i) + "_";
    prefs.putUInt((prefix + "id").c_str(), sessions[i].id);
    prefs.putString((prefix + "st").c_str(), sessions[i].startTime);
    prefs.putString((prefix + "et").c_str(), sessions[i].endTime);
    prefs.putFloat((prefix + "kwh").c_str(), sessions[i].energyKwh);
    prefs.putFloat((prefix + "eur").c_str(), sessions[i].costEur);
  }
  prefs.end();
}

void loadSessions() {
  prefs.begin("sessions", true);
  nextSessionId = prefs.getUInt("nextId", 1);
  sessionCount = prefs.getInt("count", 0);
  if (sessionCount < 0) sessionCount = 0;
  if (sessionCount > MAX_SESSIONS) sessionCount = MAX_SESSIONS;
  for (int i = 0; i < sessionCount; i++) {
    String prefix = "s" + String(i) + "_";
    sessions[i].valid = true;
    sessions[i].id = prefs.getUInt((prefix + "id").c_str(), i + 1);
    sessions[i].startTime = prefs.getString((prefix + "st").c_str(), "-");
    sessions[i].endTime = prefs.getString((prefix + "et").c_str(), "-");
    sessions[i].energyKwh = prefs.getFloat((prefix + "kwh").c_str(), 0.0f);
    sessions[i].costEur = prefs.getFloat((prefix + "eur").c_str(), 0.0f);
  }
  prefs.end();
}

void deleteSessionById(uint32_t id) {
  int idx = -1;
  for (int i = 0; i < sessionCount; i++) {
    if (sessions[i].id == id) { idx = i; break; }
  }
  if (idx < 0) return;
  for (int i = idx + 1; i < sessionCount; i++) sessions[i - 1] = sessions[i];
  sessionCount--;
  saveSessions();
  addDebugLog("SESSION DELETE | id=" + String(id));
}

void clearSessions() {
  sessionCount = 0;
  saveSessions();
  addDebugLog("SESSION CLEAR | all completed sessions deleted");
}

void addCompletedSession(const String& startTime, const String& endTime, float energyKwh) {
  ChargeSession cs;
  cs.valid = true;
  cs.id = nextSessionId++;
  cs.startTime = startTime;
  cs.endTime = endTime;
  cs.energyKwh = energyKwh;
  cs.costEur = energyKwh * ENERGY_PRICE_EUR_PER_KWH;

  if (sessionCount < MAX_SESSIONS) {
    sessions[sessionCount++] = cs;
  } else {
    for (int i = 1; i < MAX_SESSIONS; i++) sessions[i - 1] = sessions[i];
    sessions[MAX_SESSIONS - 1] = cs;
  }
  saveSessions();
}

void startChargeSession() {
  if (sessionActive) return;
  sessionActive = true;
  activeSessionStartTime = currentDateTimeString();
  activeEnergyKwh = 0.0f;
  lastEnergyMillis = millis();
  addDebugLog("SESSION START | start=" + activeSessionStartTime);
}

void stopChargeSession() {
  if (!sessionActive) return;
  String endTime = currentDateTimeString();
  addCompletedSession(activeSessionStartTime, endTime, activeEnergyKwh);
  sessionActive = false;
  addDebugLog("SESSION STOP | end=" + endTime + " | energy=" + String(activeEnergyKwh, 4) + " kWh | cost=" + String(activeEnergyKwh * ENERGY_PRICE_EUR_PER_KWH, 3) + " EUR");
}

void updateEnergy(float powerKw, bool force = false) {
  unsigned long now = millis();
  if (!sessionActive) {
    lastEnergyMillis = now;
    return;
  }
  if (force || now - lastEnergyMillis >= METER_INTERVAL_MS) {
    float hours = (now - lastEnergyMillis) / 3600000.0f;
    activeEnergyKwh += powerKw * hours;
    lastEnergyMillis = now;
  }
}

void setupWiFiApSta() {
  deviceId = getDeviceId();
  String shortId = deviceId.substring(deviceId.length() - 6);
  apSsid = "EVSE-SIM-" + shortId;

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  Serial.println();
  Serial.println("Starte WiFi AP+STA Modus");

  bool apStarted = WiFi.softAP(apSsid.c_str(), AP_PASSWORD, 1, false, 4);
  if (apStarted) {
    Serial.print("Lokaler AP gestartet: ");
    Serial.println(apSsid);
    Serial.print("AP Passwort: ");
    Serial.println(AP_PASSWORD);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Fehler beim Starten des lokalen AP");
  }

  WiFi.begin(STA_SSID, STA_PASSWORD);
  Serial.print("Verbinde mit Router SSID: ");
  Serial.println(STA_SSID);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("STA verbunden");
    Serial.print("STA IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("STA nicht verbunden - AP bleibt trotzdem aktiv");
  }

  Serial.print("Geraete-ID: ");
  Serial.println(deviceId);
}

String htmlPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Glow Challenge</title>
  <style>
    :root { --bg:#08051f; --card:#151033; --line:#37305f; --text:#f7f2ff; --muted:#b8add6; --cyan:#00d9ff; --blue:#367cff; --pink:#ff3ecf; --green:#26f5a8; --yellow:#ffd166; --red:#ff4d7d; }
    * { box-sizing: border-box; }
    body { font-family: Arial, sans-serif; background: radial-gradient(circle at 20% 0%, rgba(255,62,207,.30) 0, transparent 34%), radial-gradient(circle at 80% 10%, rgba(0,217,255,.28) 0, transparent 36%), linear-gradient(160deg, #090420 0%, #071b3d 55%, #19082d 100%); color: var(--text); margin: 0; padding: 22px; }
    .container { max-width: 980px; margin: auto; }
    .brand { display:flex; flex-direction:column; align-items:center; gap:10px; margin-bottom:22px; text-align:center; }
    .logo { font-size: 2.1rem; font-weight: 800; letter-spacing: -1px; color:#fff; }
    .logo span { color: var(--pink); }
    h1 { margin: 0; font-size: 2.2rem; color: #fff; text-shadow: 0 0 18px rgba(255,62,207,.55), 0 0 32px rgba(0,217,255,.35); }
    .subtitle { color: var(--muted); }
    .stack { display:flex; flex-direction:column; gap:16px; }
    .card { background: linear-gradient(160deg, rgba(21,16,51,.96), rgba(10,32,67,.92)); border:1px solid rgba(255,62,207,.22); border-radius: 16px; padding: 16px; box-shadow: 0 14px 34px rgba(0,0,0,.35), 0 0 28px rgba(0,217,255,.08); }
    .row { display:flex; justify-content:space-between; gap:12px; border-bottom:1px solid var(--line); padding:10px 0; align-items:center; }
    .row:last-child { border-bottom:none; }
    .grid2 { display:grid; grid-template-columns: repeat(auto-fit, minmax(260px,1fr)); gap:16px; }
    .badge { padding:6px 11px; border-radius:999px; font-weight:700; font-size:.82rem; white-space:nowrap; }
    .on { background: var(--green); color:#062014; }
    .off { background:#506176; color:#fff; }
    .error { background: var(--red); color:#fff; }
    .warn { background: var(--yellow); color:#271d00; }
    .info { background: linear-gradient(90deg, var(--blue), var(--cyan)); color:#001c25; }
    .value { font-weight:700; color: var(--yellow); text-align:right; }
    .powerbar { width:100%; height:18px; background:#26384f; border-radius:999px; overflow:hidden; margin-top:12px; }
    .powerfill { height:100%; background:linear-gradient(90deg,var(--blue),var(--cyan),var(--pink)); width:0%; transition:width .2s ease; }
    .ocpp { display:flex; flex-direction:column; gap:10px; margin-top:10px; }
    .step { display:flex; align-items:center; gap:10px; padding:10px; border-radius:12px; background:rgba(8,10,38,.72); border:1px solid rgba(0,217,255,.18); }
    .dot { width:13px; height:13px; border-radius:50%; background:#506176; box-shadow:0 0 0 transparent; }
    .step.active .dot { background:var(--green); box-shadow:0 0 12px var(--green); }
    .step.errorStep .dot { background:var(--red); box-shadow:0 0 12px var(--red); }
    .step .name { font-weight:700; }
    .step .desc { color:var(--muted); font-size:.9rem; margin-left:auto; text-align:right; }
    .footer { margin-top:18px; color:var(--muted); font-size:.85rem; }
    canvas { width:100%; height:260px; background:rgba(8,10,38,.72); border:1px solid rgba(0,217,255,.18); border-radius:14px; }
    code { color: var(--cyan); }
  </style>
</head>
<body>
  <div class="container">
    <div class="brand">
      <div class="logo">charge<span>cloud</span></div>
      <h1>Glow Challenge</h1>
      <div class="subtitle">EVSE Simulator mit OCPP-Ladevorgang aus GPIO-Eingaengen</div>
    </div>

    <div class="stack">
      <div class="grid2">
        <div class="card">
          <h2>System</h2>
          <div class="row"><span>Geraete-ID</span><span id="deviceId" class="value">-</span></div>
          <div class="row"><span>AP SSID</span><span id="apSsid" class="value">-</span></div>
          <div class="row"><span>AP IP</span><span id="apIp" class="value">-</span></div>
          <div class="row"><span>STA IP</span><span id="staIp" class="value">-</span></div>
          <div class="row"><span>STA Status</span><span id="staStatus" class="badge off">-</span></div>
        </div>

        <div class="card">
          <h2>Ladestation</h2>
          <div class="row"><span>Status</span><span id="chargerState" class="badge off">-</span></div>
          <div class="row"><span>OCPP Status</span><span id="ocppStatus" class="badge off">-</span></div>
          <div class="row"><span>Ladeleistung</span><span><span id="powerKw" class="value">0.0</span> kW</span></div>
          <div class="row"><span>Strom 3-phasig</span><span><span id="currentA" class="value">0.0</span> A</span></div>
          <div class="powerbar"><div id="powerFill" class="powerfill"></div></div>
        </div>
      </div>

      <div class="card">
        <h2>Ladeleistung Verlauf</h2>
        <canvas id="powerChart" width="900" height="260"></canvas>
        <div class="subtitle">Live-Verlauf waehrend CHARGING. Reset bei inaktivem Ladevorgang.</div>
      </div>

      <div class="card">
        <h2>OCPP Ablauf</h2>
        <div class="ocpp">
          <div id="stepBoot" class="step"><div class="dot"></div><div><div class="name">BootNotification</div><div>Charge Point meldet sich am Backend</div></div><div class="desc">nach Start</div></div>
          <div id="stepAvailable" class="step"><div class="dot"></div><div><div class="name">StatusNotification: Available</div><div>Ladepunkt ist bereit</div></div><div class="desc">IDLE</div></div>
          <div id="stepPreparing" class="step"><div class="dot"></div><div><div class="name">StatusNotification: Preparing</div><div>Fahrzeug verbunden</div></div><div class="desc">GPIO4</div></div>
          <div id="stepAuthorize" class="step"><div class="dot"></div><div><div class="name">Authorize</div><div>Start/Autorisierung angefordert</div></div><div class="desc">GPIO5</div></div>
          <div id="stepStart" class="step"><div class="dot"></div><div><div class="name">StartTransaction</div><div>Ladevorgang aktiv</div></div><div class="desc">CHARGING</div></div>
          <div id="stepMeter" class="step"><div class="dot"></div><div><div class="name">MeterValues</div><div>Leistung und Strom werden gesendet</div></div><div class="desc">Poti GPIO3</div></div>
          <div id="stepStop" class="step"><div class="dot"></div><div><div class="name">StopTransaction</div><div>Ladevorgang beendet</div></div><div class="desc">GPIO6</div></div>
          <div id="stepFault" class="step"><div class="dot"></div><div><div class="name">StatusNotification: Faulted</div><div>Fehlerzustand aktiv</div></div><div class="desc">GPIO7</div></div>
        </div>
      </div>

      <div class="card">
        <h2>Eingaenge</h2>
        <div class="row"><span>GPIO3 Poti ADC</span><span id="adcValue" class="value">-</span></div>
        <div class="row"><span>GPIO4 Fahrzeug verbunden</span><span id="inVehicle" class="badge off">-</span></div>
        <div class="row"><span>GPIO5 Start / Autorisierung</span><span id="inStart" class="badge off">-</span></div>
        <div class="row"><span>GPIO6 Stop</span><span id="inStop" class="badge off">-</span></div>
        <div class="row"><span>GPIO7 Fehler</span><span id="inError" class="badge off">-</span></div>
      </div>

      <div class="card">
        <h2>Ausgaenge</h2>
        <div class="row"><span>GPIO8 Available</span><span id="outAvailable" class="badge off">-</span></div>
        <div class="row"><span>GPIO9 Preparing</span><span id="outPreparing" class="badge off">-</span></div>
        <div class="row"><span>GPIO10 Charging</span><span id="outCharging" class="badge off">-</span></div>
        <div class="row"><span>GPIO11 Faulted</span><span id="outFaulted" class="badge off">-</span></div>
      </div>
    </div>



      <div class="card">
        <h2>Aufzeichnung Ladevorgaenge</h2>
        <button onclick="clearSessions()" style="background:#367cff;color:white;border:0;border-radius:8px;padding:7px 10px;margin-bottom:10px;">Alle abgeschlossenen Ladevorgaenge loeschen</button>
        <div class="row"><span>Aktive Session Start</span><span id="activeStart" class="value">-</span></div>
        <div class="row"><span>Aktiver Verbrauch</span><span><span id="activeEnergy" class="value">0.000</span> kWh</span></div>
        <div class="row"><span>Aktive Kosten</span><span><span id="activeCost" class="value">0.00</span> EUR</span></div>
        <div id="sessionsList" class="ocpp"></div>
      </div>
      <div class="card">
        <h2>Terminal / Debug</h2>
        <pre id="debugTerminal" style="min-height:220px; max-height:320px; overflow:auto; background:#050718; color:#7CFFCB; padding:14px; border-radius:12px; border:1px solid rgba(0,217,255,.25); font-family:monospace; font-size:12px; white-space:pre-wrap;">Warte auf Debug-Daten...</pre>
      </div>

    <div class="footer">Aktualisierung alle 500 ms. AP: <code>http://192.168.4.1</code></div>
  </div>

<script>
let powerHistory = [];
let lastCharging = false;
const maxPoints = 120;

function drawPowerChart() {
  const canvas = document.getElementById("powerChart");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0,0,w,h);
  ctx.fillStyle = "#080a26";
  ctx.fillRect(0,0,w,h);
  ctx.strokeStyle = "rgba(255,255,255,0.12)";
  ctx.lineWidth = 1;
  for (let i=0;i<=4;i++) {
    const y = (h-30) - i*((h-50)/4);
    ctx.beginPath(); ctx.moveTo(45,y); ctx.lineTo(w-15,y); ctx.stroke();
    ctx.fillStyle = "#b8add6"; ctx.font = "12px Arial";
    ctx.fillText((22*i/4).toFixed(0)+" kW", 8, y+4);
  }
  ctx.strokeStyle = "rgba(0,217,255,0.8)";
  ctx.beginPath(); ctx.moveTo(45,15); ctx.lineTo(45,h-30); ctx.lineTo(w-15,h-30); ctx.stroke();
  if (powerHistory.length < 2) {
    ctx.fillStyle = "#b8add6"; ctx.font = "14px Arial";
    ctx.fillText("Warte auf CHARGING...", 55, 35);
    return;
  }
  ctx.beginPath();
  powerHistory.forEach((p, index) => {
    const x = 45 + index*((w-60)/(maxPoints-1));
    const y = (h-30) - (p.powerKw/22.0)*(h-50);
    if (index === 0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
  });
  ctx.strokeStyle = "rgba(0,217,255,0.45)"; ctx.lineWidth = 8; ctx.stroke();
  ctx.strokeStyle = "#ff3ecf"; ctx.lineWidth = 3; ctx.stroke();
  const last = powerHistory[powerHistory.length-1];
  ctx.fillStyle = "#f7f2ff"; ctx.font = "14px Arial";
  ctx.fillText("Aktuell: " + last.powerKw.toFixed(1) + " kW", 55, h-8);
}

function updatePowerChart(data) {
  const isCharging = data.state === "CHARGING";
  if (!isCharging) {
    if (lastCharging || powerHistory.length) { powerHistory = []; drawPowerChart(); }
    lastCharging = false;
    return;
  }
  lastCharging = true;
  powerHistory.push({ t: Date.now(), powerKw: Number(data.powerKw) });
  if (powerHistory.length > maxPoints) powerHistory.shift();
  drawPowerChart();
}

function setBadge(id, active, trueText = "AN", falseText = "AUS") {
  const el = document.getElementById(id);
  el.textContent = active ? trueText : falseText;
  el.className = "badge " + (active ? "on" : "off");
}
function setErrorBadge(id, active, trueText = "FEHLER", falseText = "OK") {
  const el = document.getElementById(id);
  el.textContent = active ? trueText : falseText;
  el.className = "badge " + (active ? "error" : "on");
}
function setStep(id, active, error=false) {
  const el = document.getElementById(id);
  el.className = "step " + (active ? (error ? "errorStep" : "active") : "");
}
function deriveOcpp(data) {
  if (data.inputs.errorActive || data.state === "ERROR") return "Faulted";
  if (data.state === "CHARGING") return "Charging";
  if (data.state === "CONNECTED" || data.inputs.vehicleConnected) return "Preparing";
  return "Available";
}
async function updateStatus() {
  try {
    const res = await fetch("/api/status", { cache: "no-store" });
    const data = await res.json();
    const ocpp = deriveOcpp(data);
    updatePowerChart(data);

    document.getElementById("deviceId").textContent = data.deviceId;
    document.getElementById("apSsid").textContent = data.apSsid;
    document.getElementById("apIp").textContent = data.apIp;
    document.getElementById("staIp").textContent = data.staIp;
    const sta = document.getElementById("staStatus");
    sta.textContent = data.staConnected ? "VERBUNDEN" : "GETRENNT";
    sta.className = "badge " + (data.staConnected ? "on" : "off");

    const state = document.getElementById("chargerState");
    state.textContent = data.state;
    state.className = "badge " + (data.state === "ERROR" ? "error" : (data.state === "CHARGING" ? "on" : "warn"));
    const ocppStatus = document.getElementById("ocppStatus");
    ocppStatus.textContent = ocpp;
    ocppStatus.className = "badge " + (ocpp === "Faulted" ? "error" : (ocpp === "Charging" ? "on" : "info"));

    document.getElementById("powerKw").textContent = Number(data.powerKw).toFixed(1);
    document.getElementById("currentA").textContent = Number(data.currentA).toFixed(1);
    document.getElementById("adcValue").textContent = data.adcValue;
    document.getElementById("powerFill").style.width = Math.max(0, Math.min(100, data.powerKw / 22.0 * 100.0)) + "%";

    setBadge("inVehicle", data.inputs.vehicleConnected, "AKTIV", "AUS");
    setBadge("inStart", data.inputs.startPressed, "GEDRUECKT", "AUS");
    setBadge("inStop", data.inputs.stopPressed, "GEDRUECKT", "AUS");
    setErrorBadge("inError", data.inputs.errorActive, "AKTIV", "OK");
    setBadge("outAvailable", data.outputs.available);
    setBadge("outPreparing", data.outputs.preparing);
    setBadge("outCharging", data.outputs.charging);
    setErrorBadge("outFaulted", data.outputs.faulted, "AN", "AUS");

    document.getElementById("activeStart").textContent = data.session.active ? data.session.startTime : "-";
    document.getElementById("activeEnergy").textContent = Number(data.session.activeEnergyKwh).toFixed(3);
    document.getElementById("activeCost").textContent = Number(data.session.activeCostEur).toFixed(3);
    const list = document.getElementById("sessionsList");
    list.innerHTML = "";
    if (!data.session.history.length) {
      list.innerHTML = '<div class="step"><div class="dot"></div><div><div class="name">Noch keine abgeschlossenen Ladevorgaenge</div><div>StartTransaction und StopTransaction erzeugen Eintraege</div></div></div>';
    } else {
      data.session.history.slice().reverse().forEach((s, idx) => {
        const div = document.createElement("div");
        div.className = "step active";
        div.innerHTML = `<div class="dot"></div><div><div class="name">Ladevorgang #${s.id}</div><div>${s.startTime} bis ${s.endTime}</div></div><div class="desc">${Number(s.energyKwh).toFixed(3)} kWh<br>${Number(s.costEur).toFixed(3)} EUR<br><button onclick="deleteSession(${s.id})" style="margin-top:6px;background:#ff3ecf;color:white;border:0;border-radius:8px;padding:5px 8px;">Loeschen</button></div>`;
        list.appendChild(div);
      });
    }

    const term = document.getElementById("debugTerminal");
    if (term && data.debugLog) {
      term.textContent = data.debugLog.join("\n");
      term.scrollTop = term.scrollHeight;
    }

    setStep("stepBoot", true);
    setStep("stepAvailable", ocpp === "Available");
    setStep("stepPreparing", ocpp === "Preparing" || ocpp === "Charging");
    setStep("stepAuthorize", data.inputs.startPressed || ocpp === "Charging");
    setStep("stepStart", ocpp === "Charging");
    setStep("stepMeter", ocpp === "Charging");
    setStep("stepStop", data.inputs.stopPressed);
    setStep("stepFault", ocpp === "Faulted", true);
  } catch (e) { console.log("Update fehlgeschlagen", e); }
}
async function deleteSession(id) {
  await fetch("/api/session/delete?id=" + encodeURIComponent(id), { cache: "no-store" });
  updateStatus();
}
async function clearSessions() {
  await fetch("/api/session/clear", { cache: "no-store" });
  updateStatus();
}

setInterval(updateStatus, 500);
updateStatus();
</script>
</body>
</html>
)rawliteral";
  return html;
}

void handleStatusApi() {
  bool vehicleConnected = updateDebouncedInput(inputVehicle);
  bool startPressed     = updateDebouncedInput(inputStart);
  bool stopPressed      = updateDebouncedInput(inputStop);
  bool errorActive      = updateDebouncedInput(inputError);

  int adcValue = readPowerAdc();
  float powerKw = adcToPowerKw(adcValue);
  float currentA = powerKwToCurrentA(powerKw);

  String staIp = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("0.0.0.0");

  String json = "{";
  json += "\"deviceId\":\"" + deviceId + "\",";
  json += "\"apSsid\":\"" + apSsid + "\",";
  json += "\"apIp\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"staIp\":\"" + staIp + "\",";
  json += "\"staConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"state\":\"" + String(stateName(state)) + "\",";
  json += "\"adcValue\":" + String(adcValue) + ",";
  json += "\"powerKw\":" + String(powerKw, 2) + ",";
  json += "\"currentA\":" + String(currentA, 2) + ",";
  json += "\"inputs\":{";
  json += "\"vehicleConnected\":" + String(vehicleConnected ? "true" : "false") + ",";
  json += "\"startPressed\":" + String(startPressed ? "true" : "false") + ",";
  json += "\"stopPressed\":" + String(stopPressed ? "true" : "false") + ",";
  json += "\"errorActive\":" + String(errorActive ? "true" : "false");
  json += "},";
  json += "\"outputs\":{";
  json += "\"available\":" + String(digitalRead(LED_AVAILABLE) ? "true" : "false") + ",";
  json += "\"preparing\":" + String(digitalRead(LED_PREPARING) ? "true" : "false") + ",";
  json += "\"charging\":" + String(digitalRead(LED_CHARGING) ? "true" : "false") + ",";
  json += "\"faulted\":" + String(digitalRead(LED_FAULTED) ? "true" : "false");
  json += "},";
  json += "\"debugLog\":[";
  for (int i = 0; i < debugLogCount; i++) {
    if (i > 0) json += ",";
    String line = debugLogs[i];
    line.replace("\\", "\\\\");
    line.replace("\"", "\\\"");
    json += "\"" + line + "\"";
  }
  json += "]";
  json += "}";

  server.send(200, "application/json", json);
}

void setupWebServer() {
  server.on("/", []() {
    server.send(200, "text/html", htmlPage());
  });
  server.on("/api/status", handleStatusApi);
  server.on("/api/session/delete", []() {
    if (server.hasArg("id")) deleteSessionById((uint32_t) server.arg("id").toInt());
    server.send(200, "application/json", "{"ok":true}");
  });
  server.on("/api/session/clear", []() {
    clearSessions();
    server.send(200, "application/json", "{"ok":true}");
  });
  server.begin();
  Serial.println("Webserver gestartet");
}

void updateNeoPixel() {
  static unsigned long lastBlink = 0;
  static bool pixelOn = false;

  if (state == CHARGING) {
    if (millis() - lastBlink >= 250) {
      lastBlink = millis();
      pixelOn = !pixelOn;
      if (pixelOn) {
        pixel.setPixelColor(0, pixel.Color(0, 0, 255));
      } else {
        pixel.clear();
      }
      pixel.show();
    }
  } else {
    if (pixelOn) {
      pixelOn = false;
    }
    pixel.clear();
    pixel.show();
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pixel.begin();
  pixel.clear();
  pixel.show();

  pinMode(PIN_VEHICLE_CONNECTED, INPUT_PULLUP);
  pinMode(PIN_START_AUTH, INPUT_PULLUP);
  pinMode(PIN_STOP, INPUT_PULLUP);
  pinMode(PIN_ERROR, INPUT_PULLUP);
  pinMode(PIN_POWER_POTI, INPUT);

  pinMode(LED_AVAILABLE, OUTPUT);
  pinMode(LED_PREPARING, OUTPUT);
  pinMode(LED_CHARGING, OUTPUT);
  pinMode(LED_FAULTED, OUTPUT);

  analogReadResolution(12);
  updateLeds();
  loadSessions();
  addDebugLog("BOOT | sessions loaded=" + String(sessionCount) + " nextId=" + String(nextSessionId));

  setupWiFiApSta();
  setupTimeSync();
  setupWebServer();

  Serial.println();
  Serial.println("Ladestations-Simulation gestartet");
  Serial.println("Board: ESP32-S3");
  Serial.println("Pins: GPIO3 bis GPIO11");
  Serial.println("Poti: GPIO3 = 0..22 kW");
}

void loop() {
  server.handleClient();

  bool vehicleConnected = readActiveLow(PIN_VEHICLE_CONNECTED);
  bool startPressed     = readActiveLow(PIN_START_AUTH);
  bool stopPressed      = readActiveLow(PIN_STOP);
  bool errorActive      = readActiveLow(PIN_ERROR);

  if (vehicleConnected != lastVehicleConnected) {
    addDebugLog(String("GPIO4 Fahrzeug verbunden -> ") + (vehicleConnected ? "AKTIV" : "AUS"));
    lastVehicleConnected = vehicleConnected;
  }
  if (startPressed != lastStartPressed) {
    addDebugLog(String("GPIO5 Start/Auth -> ") + (startPressed ? "GEDRUECKT" : "AUS"));
    lastStartPressed = startPressed;
  }
  if (stopPressed != lastStopPressed) {
    addDebugLog(String("GPIO6 Stop -> ") + (stopPressed ? "GEDRUECKT" : "AUS"));
    lastStopPressed = stopPressed;
  }
  if (errorActive != lastErrorActive) {
    addDebugLog(String("GPIO7 Fehler -> ") + (errorActive ? "AKTIV" : "OK"));
    lastErrorActive = errorActive;
  }

  float powerKw = readChargingPowerKw();
  float currentA = powerKwToCurrentA(powerKw);

  if (errorActive) {
    state = ERROR_STATE;
  } else {
    switch (state) {
      case IDLE:
        if (vehicleConnected) {
          state = CONNECTED;
          addDebugLog("STATE | Fahrzeug verbunden -> CONNECTED/PREPARING");
        }
        break;

      case CONNECTED:
        if (!vehicleConnected) {
          state = IDLE;
          addDebugLog("STATE | Fahrzeug getrennt -> IDLE/AVAILABLE");
        } else if (startPressed) {
          state = AUTHORIZED;
          addDebugLog("OCPP | Authorize");
          delay(300);
          state = CHARGING;
          addDebugLog("OCPP | StartTransaction -> CHARGING");
        }
        break;

      case AUTHORIZED:
        state = CHARGING;
        break;

      case CHARGING:
        if (!vehicleConnected) {
          state = IDLE;
          addDebugLog("STATE | Fahrzeug getrennt -> IDLE/AVAILABLE");
        } else if (stopPressed) {
          state = CONNECTED;
          addDebugLog("OCPP | StopTransaction -> CONNECTED/PREPARING");
        }
        break;

      case ERROR_STATE:
        state = IDLE;
        break;
    }
  }

  if (state == CHARGING) {
    if (!sessionActive) {
      startChargeSession();
    }
    updateEnergy(powerKw);
  } else {
    if (sessionActive) {
      updateEnergy(powerKw, true);
      stopChargeSession();
    } else {
      lastEnergyMillis = millis();
    }
  }

  if (state != lastLoggedState) {
    addDebugLog(String("STATUS | ") + stateName(lastLoggedState) + " -> " + stateName(state));
    lastLoggedState = state;
  }

  updateLeds();
  updateNeoPixel();

  if (millis() - lastPowerPrint >= 1000) {
    lastPowerPrint = millis();
    Serial.print("Status: ");
    Serial.print(stateName(state));
    Serial.print(" | Ladeleistung: ");
    Serial.print(powerKw, 1);
    Serial.print(" kW | Strom 3-phasig: ");
    Serial.print(currentA, 1);
    Serial.print(" A | AP: ");
    Serial.print(apSsid);
    Serial.print(" | AP IP: ");
    Serial.print(WiFi.softAPIP());
    Serial.print(" | STA IP: ");
    Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("0.0.0.0"));
  }

  delay(20);
}
