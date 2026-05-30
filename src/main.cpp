#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

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

// ESP32-S3 Ladestations-Simulation
// Verwendet GPIO3 bis GPIO11. GPIO1 und GPIO2 bleiben frei.

// Eingaenge
const int PIN_POWER_POTI        = 3;  // ADC: Poti fuer 0..22 kW
const int PIN_VEHICLE_CONNECTED = 4;  // Schalter nach GND
const int PIN_START_AUTH        = 5;  // Taster nach GND
const int PIN_STOP              = 6;  // Taster nach GND
const int PIN_ERROR             = 7;  // Schalter/Taster nach GND

// Ausgaenge
const int LED_IDLE      = 8;
const int LED_CONNECTED = 9;
const int LED_CHARGING  = 10;
const int LED_ERROR     = 11;

ChargerState state = IDLE;
unsigned long lastPowerPrint = 0;

bool readActiveLow(int pin) {
  return digitalRead(pin) == LOW;
}

void setAllLedsOff() {
  digitalWrite(LED_IDLE, LOW);
  digitalWrite(LED_CONNECTED, LOW);
  digitalWrite(LED_CHARGING, LOW);
  digitalWrite(LED_ERROR, LOW);
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
      digitalWrite(LED_IDLE, HIGH);
      break;
    case CONNECTED:
      digitalWrite(LED_CONNECTED, HIGH);
      break;
    case AUTHORIZED:
      digitalWrite(LED_CONNECTED, HIGH);
      digitalWrite(LED_IDLE, HIGH);
      break;
    case CHARGING:
      digitalWrite(LED_CHARGING, HIGH);
      break;
    case ERROR_STATE:
      digitalWrite(LED_ERROR, HIGH);
      break;
  }
}

String getDeviceId() {
  uint64_t chipId = ESP.getEfuseMac();
  char id[13];
  snprintf(id, sizeof(id), "%04X%08X", (uint16_t)(chipId >> 32), (uint32_t)chipId);
  return String(id);
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
  <title>EVSE Simulator</title>
  <style>
    body { font-family: Arial, sans-serif; background: #0f172a; color: #e5e7eb; margin: 0; padding: 20px; }
    .container { max-width: 980px; margin: auto; }
    h1 { color: #38bdf8; margin-bottom: 6px; }
    .subtitle { color: #94a3b8; margin-bottom: 20px; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(270px, 1fr)); gap: 16px; }
    .card { background: #1e293b; border-radius: 14px; padding: 16px; box-shadow: 0 10px 25px rgba(0,0,0,0.25); }
    .row { display: flex; justify-content: space-between; gap: 12px; border-bottom: 1px solid #334155; padding: 9px 0; align-items: center; }
    .row:last-child { border-bottom: none; }
    .badge { padding: 5px 10px; border-radius: 999px; font-weight: bold; font-size: 0.82rem; white-space: nowrap; }
    .on { background: #16a34a; color: white; }
    .off { background: #475569; color: white; }
    .error { background: #dc2626; color: white; }
    .warn { background: #ca8a04; color: white; }
    .value { font-weight: bold; color: #facc15; text-align: right; }
    .footer { margin-top: 20px; color: #94a3b8; font-size: 0.85rem; }
    .powerbar { width: 100%; height: 18px; background: #334155; border-radius: 999px; overflow: hidden; margin-top: 12px; }
    .powerfill { height: 100%; background: linear-gradient(90deg, #22c55e, #eab308, #ef4444); width: 0%; transition: width 0.2s ease; }
    code { color: #7dd3fc; }
  </style>
</head>
<body>
  <div class="container">
    <h1>EVSE Ladestations-Simulator</h1>
    <div class="subtitle">Live-Anzeige aller Eingaenge, Ausgaenge und Leistungswerte</div>

    <div class="grid">
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
        <div class="row"><span>Ladeleistung</span><span><span id="powerKw" class="value">0.0</span> kW</span></div>
        <div class="row"><span>Strom 3-phasig</span><span><span id="currentA" class="value">0.0</span> A</span></div>
        <div class="powerbar"><div id="powerFill" class="powerfill"></div></div>
      </div>

      <div class="card">
        <h2>Eingaenge</h2>
        <div class="row"><span>GPIO4 Fahrzeug verbunden</span><span id="inVehicle" class="badge off">-</span></div>
        <div class="row"><span>GPIO5 Start / Autorisierung</span><span id="inStart" class="badge off">-</span></div>
        <div class="row"><span>GPIO6 Stop</span><span id="inStop" class="badge off">-</span></div>
        <div class="row"><span>GPIO7 Fehler</span><span id="inError" class="badge off">-</span></div>
        <div class="row"><span>GPIO3 Poti ADC</span><span id="adcValue" class="value">-</span></div>
      </div>

      <div class="card">
        <h2>Ausgaenge</h2>
        <div class="row"><span>GPIO8 LED Bereit</span><span id="outIdle" class="badge off">-</span></div>
        <div class="row"><span>GPIO9 LED Verbunden</span><span id="outConnected" class="badge off">-</span></div>
        <div class="row"><span>GPIO10 LED Laedt</span><span id="outCharging" class="badge off">-</span></div>
        <div class="row"><span>GPIO11 LED Fehler</span><span id="outError" class="badge off">-</span></div>
      </div>
    </div>

    <div class="footer">
      Aktualisierung alle 500 ms. AP ist erreichbar unter <code>http://192.168.4.1</code>.
    </div>
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
async function updateStatus() {
  try {
    const res = await fetch("/api/status", { cache: "no-store" });
    const data = await res.json();

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

    document.getElementById("powerKw").textContent = Number(data.powerKw).toFixed(1);
    document.getElementById("currentA").textContent = Number(data.currentA).toFixed(1);
    document.getElementById("adcValue").textContent = data.adcValue;

    const percent = Math.max(0, Math.min(100, data.powerKw / 22.0 * 100.0));
    document.getElementById("powerFill").style.width = percent + "%";

    setBadge("inVehicle", data.inputs.vehicleConnected, "AKTIV", "AUS");
    setBadge("inStart", data.inputs.startPressed, "GEDRUECKT", "AUS");
    setBadge("inStop", data.inputs.stopPressed, "GEDRUECKT", "AUS");
    setErrorBadge("inError", data.inputs.errorActive, "AKTIV", "OK");

    setBadge("outIdle", data.outputs.ledIdle);
    setBadge("outConnected", data.outputs.ledConnected);
    setBadge("outCharging", data.outputs.ledCharging);
    setErrorBadge("outError", data.outputs.ledError, "AN", "AUS");
  } catch (e) {
    console.log("Update fehlgeschlagen", e);
  }
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
  json += "\"ledIdle\":" + String(digitalRead(LED_IDLE) ? "true" : "false") + ",";
  json += "\"ledConnected\":" + String(digitalRead(LED_CONNECTED) ? "true" : "false") + ",";
  json += "\"ledCharging\":" + String(digitalRead(LED_CHARGING) ? "true" : "false") + ",";
  json += "\"ledError\":" + String(digitalRead(LED_ERROR) ? "true" : "false");
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

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_VEHICLE_CONNECTED, INPUT_PULLUP);
  pinMode(PIN_START_AUTH, INPUT_PULLUP);
  pinMode(PIN_STOP, INPUT_PULLUP);
  pinMode(PIN_ERROR, INPUT_PULLUP);
  pinMode(PIN_POWER_POTI, INPUT);

  pinMode(LED_IDLE, OUTPUT);
  pinMode(LED_CONNECTED, OUTPUT);
  pinMode(LED_CHARGING, OUTPUT);
  pinMode(LED_ERROR, OUTPUT);

  analogReadResolution(12);
  updateLeds();

  setupWiFiApSta();
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
          Serial.println("Ladevorgang gestartet");
        }
        break;

      case AUTHORIZED:
        state = CHARGING;
        break;

      case CHARGING:
        if (!vehicleConnected) {
          state = IDLE;
          Serial.println("Fahrzeug getrennt");
        } else if (stopPressed) {
          state = CONNECTED;
          Serial.println("Ladevorgang gestoppt");
        }
        break;

      case ERROR_STATE:
        state = IDLE;
        break;
    }
  }

  updateLeds();

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
