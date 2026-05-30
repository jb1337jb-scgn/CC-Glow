#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <Adafruit_NeoPixel.h>

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

WebServer server(80);

#define NEOPIXEL_PIN 48
#define NEOPIXEL_COUNT 1
Adafruit_NeoPixel pixel(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

const float ENERGY_PRICE_EUR_PER_KWH = 5.00f;
const int MAX_SESSIONS = 10;

struct ChargeSession {
  bool valid;
  String startTime;
  String endTime;
  float energyKwh;
  float costEur;
};

ChargeSession sessions[MAX_SESSIONS];
int sessionCount = 0;
bool sessionActive = false;
String activeSessionStartTime = "";
unsigned long lastEnergyMillis = 0;
float activeEnergyKwh = 0.0f;
ChargerState previousState = IDLE;

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

void addCompletedSession(const String& startTime, const String& endTime, float energyKwh) {
  ChargeSession cs;
  cs.valid = true;
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
}

void startChargeSession() {
  if (sessionActive) return;
  sessionActive = true;
  activeSessionStartTime = currentDateTimeString();
  activeEnergyKwh = 0.0f;
  lastEnergyMillis = millis();
  Serial.println("Ladesession gestartet: " + activeSessionStartTime);
}

void stopChargeSession() {
  if (!sessionActive) return;
  String endTime = currentDateTimeString();
  addCompletedSession(activeSessionStartTime, endTime, activeEnergyKwh);
  sessionActive = false;
  Serial.print("Ladesession beendet: ");
  Serial.print(endTime);
  Serial.print(" | Energie: ");
  Serial.print(activeEnergyKwh, 3);
  Serial.print(" kWh | Kosten: ");
  Serial.print(activeEnergyKwh * ENERGY_PRICE_EUR_PER_KWH, 2);
  Serial.println(" EUR");
}

void updateEnergy(float powerKw) {
  unsigned long now = millis();
  if (!sessionActive) {
    lastEnergyMillis = now;
    return;
  }
  float hours = (now - lastEnergyMillis) / 3600000.0f;
  activeEnergyKwh += powerKw * hours;
  lastEnergyMillis = now;
}

void setupWiFiApSta() {
  deviceId = getDeviceId();
  String shortId = deviceId.substring(deviceId.length() - 6);
  apSsid = "EVSE-SIM-" + shortId;

  WiFi.mode(WIFI_AP_STA);

  Serial.println();
  Serial.println("Starte WiFi AP+STA Modus");

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

  bool apStarted = WiFi.softAP(apSsid.c_str(), AP_PASSWORD);
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
        <div class="row"><span>Aktive Session Start</span><span id="activeStart" class="value">-</span></div>
        <div class="row"><span>Aktiver Verbrauch</span><span><span id="activeEnergy" class="value">0.000</span> kWh</span></div>
        <div class="row"><span>Aktive Kosten</span><span><span id="activeCost" class="value">0.00</span> EUR</span></div>
        <div id="sessionsList" class="ocpp"></div>
      </div>
    <div class="footer">Aktualisierung alle 500 ms. AP: <code>http://192.168.4.1</code></div>
  </div>

<script>
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
        div.innerHTML = `<div class="dot"></div><div><div class="name">Session ${data.session.history.length - idx}</div><div>${s.startTime} bis ${s.endTime}</div></div><div class="desc">${Number(s.energyKwh).toFixed(3)} kWh<br>${Number(s.costEur).toFixed(3)} EUR</div>`;
        list.appendChild(div);
      });
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
setInterval(updateStatus, 500);
updateStatus();
</script>
</body>
</html>
)rawliteral";
  return html;
}

void handleStatusApi() {
  bool vehicleConnected = readActiveLow(PIN_VEHICLE_CONNECTED);
  bool startPressed     = readActiveLow(PIN_START_AUTH);
  bool stopPressed      = readActiveLow(PIN_STOP);
  bool errorActive      = readActiveLow(PIN_ERROR);

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
  json += "}";
  json += "}";

  server.send(200, "application/json", json);
}

void setupWebServer() {
  server.on("/", []() {
    server.send(200, "text/html", htmlPage());
  });
  server.on("/api/status", handleStatusApi);
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

  float powerKw = readChargingPowerKw();
  float currentA = powerKwToCurrentA(powerKw);

  if (errorActive) {
    state = ERROR_STATE;
  } else {
    switch (state) {
      case IDLE:
        if (vehicleConnected) {
          state = CONNECTED;
          Serial.println("Fahrzeug verbunden");
        }
        break;

      case CONNECTED:
        if (!vehicleConnected) {
          state = IDLE;
          Serial.println("Fahrzeug getrennt");
        } else if (startPressed) {
          state = AUTHORIZED;
          Serial.println("Autorisiert");
          delay(300);
          state = CHARGING;
          startChargeSession();
          previousState = CHARGING;
          Serial.println("Ladevorgang gestartet");
        }
        break;

      case AUTHORIZED:
        state = CHARGING;
        break;

      case CHARGING:
        if (!vehicleConnected) {
          updateEnergy(powerKw);
          stopChargeSession();
          state = IDLE;
          previousState = IDLE;
          Serial.println("Fahrzeug getrennt");
        } else if (stopPressed) {
          updateEnergy(powerKw);
          stopChargeSession();
          state = CONNECTED;
          previousState = CONNECTED;
          Serial.println("Ladevorgang gestoppt");
        }
        break;

      case ERROR_STATE:
        state = IDLE;
        break;
    }
  }

  if (state == CHARGING && previousState != CHARGING) {
    startChargeSession();
  }
  if (state != CHARGING && previousState == CHARGING) {
    stopChargeSession();
  }
  updateEnergy(powerKw);
  previousState = state;

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
