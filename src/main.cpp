#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <base64.h>
#include <time.h>
#include <Adafruit_NeoPixel.h>

const char* WIFI_SSID = "internet";
const char* WIFI_PASS = "internet";

WebServer server(80);
WebSocketsClient ws;
Preferences prefs;

String chargeboxId = "SIM_ESP32S3_001";
String backendUrl = "wss://demo.ocpp.cc/83d29edd7b79881259e1759ed19ea569";
String backendPassword = "";
bool useWss = true;
String idTag = "CAFFEE";

bool wsConnected = false;
bool ocppAccepted = false;
unsigned long heartbeatIntervalSec = 60;
unsigned long lastHeartbeatMs = 0;
unsigned long lastMeterMs = 0;
unsigned long lastWsReconnectMs = 0;

int msgCounter = 1;
int transactionId = -1;
float powerKw = 11.0;
float meterWh = 0.0;
float sessionWh = 0.0;
bool plugged = false;
bool authorized = false;
bool faulted = false;
String state = "Available";
String lastEvent = "Boot";
String faultReason = "";

const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 0;
const int DAYLIGHT_OFFSET_SEC = 0;

String isoTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) {
    return "1970-01-01T00:00:00Z";
  }
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

bool ntpSynced() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) return false;
  return timeinfo.tm_year >= 120;
}

const int LED_AVAILABLE = 16;
const int LED_PREPARING = 17;
const int LED_CHARGING  = 25;
const int LED_FAULTED   = 33;

// =============================
// Hardware-Eingaenge active LOW
// Schalter/Taster jeweils zwischen GPIO und GND
// =============================
const int PIN_IN_PLUG    = 4;   // rastender Schalter empfohlen
const int PIN_IN_AUTH    = 5;   // Taster
const int PIN_IN_START   = 6;   // Taster
const int PIN_IN_STOP    = 7;   // Taster
const int PIN_IN_FAULT   = 15;  // Schalter oder Taster
const int PIN_IN_RESET   = 18;  // Taster
const int PIN_IN_CONNECT = 8;   // Taster
const int PIN_POWER_POT  = 1;   // optional ADC: Poti 3V3 - GPIO - GND

// Ladefreigabe-Ausgang: HIGH nur bei Status Charging
const int PIN_OUT_CHARGE_ENABLE = 21;

// Eingebaute RGB-LED vieler ESP32-S3-WROOM-1 Devboards, haeufig WS2812 auf GPIO 48
const int PIN_RGB_LED = 48;
const int RGB_LED_COUNT = 1;
Adafruit_NeoPixel rgbLed(RGB_LED_COUNT, PIN_RGB_LED, NEO_GRB + NEO_KHZ800);

void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  rgbLed.setPixelColor(0, rgbLed.Color(r, g, b));
  rgbLed.show();
}

bool lastAuth = HIGH;
bool lastStart = HIGH;
bool lastStop = HIGH;
bool lastFault = HIGH;
bool lastReset = HIGH;
bool lastConnect = HIGH;
bool lastPlugPhysical = HIGH;
unsigned long lastInputMs = 0;
unsigned long lastPowerPotMs = 0;

String uid() { return String(msgCounter++); }

void setState(String s);
void sendBootNotification();
void sendHeartbeat();
void sendStatusNotification(const String &status, const String &errorCode = "NoError");
void sendAuthorize();
void sendStartTransaction();
void sendStopTransaction(const String &reason = "Local");
void sendMeterValues();
void connectOcpp();

void loadConfig() {
  prefs.begin("backend", true);
  chargeboxId = prefs.getString("chargeboxId", "SIM_ESP32S3_001");
  backendUrl = prefs.getString("backendUrl", "wss://demo.ocpp.cc/83d29edd7b79881259e1759ed19ea569");
  backendPassword = prefs.getString("password", "");
  useWss = prefs.getBool("useWss", true);
  idTag = prefs.getString("idTag", "CAFFEE");
  prefs.end();
}

void saveConfig(String cbid, String url, String pass, bool wss, String tag) {
  prefs.begin("backend", false);
  prefs.putString("chargeboxId", cbid);
  prefs.putString("backendUrl", url);
  prefs.putString("password", pass);
  prefs.putBool("useWss", wss);
  prefs.putString("idTag", tag);
  prefs.end();
  chargeboxId = cbid; backendUrl = url; backendPassword = pass; useWss = wss; idTag = tag;
}

String fullOcppUrl() {
  String base = backendUrl;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  return base + "/" + chargeboxId;
}

void parseUrl(const String &url, String &host, uint16_t &port, String &path, bool &secure) {
  String u = url;
  secure = u.startsWith("wss://");
  u.replace("wss://", "");
  u.replace("ws://", "");
  int slash = u.indexOf('/');
  String hostport = slash >= 0 ? u.substring(0, slash) : u;
  path = slash >= 0 ? u.substring(slash) : "/";
  int colon = hostport.indexOf(':');
  if (colon >= 0) {
    host = hostport.substring(0, colon);
    port = hostport.substring(colon + 1).toInt();
  } else {
    host = hostport;
    port = secure ? 443 : 80;
  }
}

void wsSendCall(const String &action, JsonDocument &payload) {
  String id = uid();
  String p; serializeJson(payload, p);
  String msg = "[2,\"" + id + "\",\"" + action + "\"," + p + "]";
  Serial.println("OCPP TX: " + msg);
  ws.sendTXT(msg);
}

void wsSendCallResult(const String &id, JsonDocument &payload) {
  String p; serializeJson(payload, p);
  String msg = "[3,\"" + id + "\"," + p + "]";
  Serial.println("OCPP TX: " + msg);
  ws.sendTXT(msg);
}

void sendBootNotification() {
  StaticJsonDocument<256> doc;
  doc["chargePointVendor"] = "chargecloud-sim";
  doc["chargePointModel"] = "ESP32-S3-WebSim";
  doc["chargePointSerialNumber"] = chargeboxId;
  doc["firmwareVersion"] = "0.1.0";
  wsSendCall("BootNotification", doc);
  lastEvent = "BootNotification sent";
}

void sendHeartbeat() {
  StaticJsonDocument<8> doc;
  wsSendCall("Heartbeat", doc);
  lastHeartbeatMs = millis();
  lastEvent = "Heartbeat sent";
}

void sendStatusNotification(const String &status, const String &errorCode) {
  StaticJsonDocument<256> doc;
  doc["connectorId"] = 1;
  doc["errorCode"] = errorCode;
  doc["status"] = status;
  wsSendCall("StatusNotification", doc);
  lastEvent = "Status " + status;
}

void sendAuthorize() {
  StaticJsonDocument<128> doc;
  doc["idTag"] = idTag;
  wsSendCall("Authorize", doc);
  lastEvent = "Authorize sent";
}

void sendStartTransaction() {
  StaticJsonDocument<256> doc;
  doc["connectorId"] = 1;
  doc["idTag"] = idTag;
  doc["meterStart"] = (int)meterWh;
  doc["timestamp"] = isoTimestamp();
  wsSendCall("StartTransaction", doc);
  lastEvent = "StartTransaction sent";
}

void sendStopTransaction(const String &reason) {
  if (transactionId < 0) return;
  StaticJsonDocument<256> doc;
  doc["transactionId"] = transactionId;
  doc["idTag"] = idTag;
  doc["meterStop"] = (int)meterWh;
  doc["timestamp"] = isoTimestamp();
  doc["reason"] = reason;
  wsSendCall("StopTransaction", doc);
  lastEvent = "StopTransaction sent";
}

void sendMeterValues() {
  if (transactionId < 0 || state != "Charging") return;
  StaticJsonDocument<512> doc;
  doc["connectorId"] = 1;
  doc["transactionId"] = transactionId;
  JsonArray mv = doc.createNestedArray("meterValue");
  JsonObject v = mv.createNestedObject();
  v["timestamp"] = isoTimestamp();
  JsonArray sampled = v.createNestedArray("sampledValue");
  JsonObject e = sampled.createNestedObject();
  e["value"] = String((int)meterWh);
  e["context"] = "Sample.Periodic";
  e["measurand"] = "Energy.Active.Import.Register";
  e["unit"] = "Wh";
  JsonObject p = sampled.createNestedObject();
  p["value"] = String(powerKw, 1);
  p["context"] = "Sample.Periodic";
  p["measurand"] = "Power.Active.Import";
  p["unit"] = "kW";
  wsSendCall("MeterValues", doc);
  lastMeterMs = millis();
}

void setState(String s) {
  state = s;
  digitalWrite(LED_AVAILABLE, state == "Available");
  digitalWrite(LED_PREPARING, state == "Preparing" || state == "SuspendedEVSE");
  digitalWrite(LED_CHARGING, state == "Charging");
  digitalWrite(LED_FAULTED, state == "Faulted");
  digitalWrite(PIN_OUT_CHARGE_ENABLE, state == "Charging" ? HIGH : LOW);

  if (!wsConnected) {
    setRgb(80, 0, 80);       // violett: OCPP nicht verbunden
  } else if (state == "Available") {
    setRgb(0, 80, 0);        // gruen
  } else if (state == "Preparing") {
    setRgb(100, 70, 0);      // gelb
  } else if (state == "Charging") {
    setRgb(0, 0, 120);       // blau
  } else if (state == "SuspendedEVSE") {
    setRgb(120, 50, 0);      // orange
  } else if (state == "Faulted") {
    setRgb(120, 0, 0);       // rot
  } else {
    setRgb(30, 30, 30);      // weiss/grau
  }
}

void updateMeter() {
  static unsigned long last = millis();
  unsigned long now = millis();
  float h = (now - last) / 3600000.0;
  last = now;
  if (state == "Charging") {
    float delta = powerKw * 1000.0 * h;
    meterWh += delta;
    sessionWh += delta;
  }
}

void handleOcppMessage(const String &msg) {
  Serial.println("OCPP RX: " + msg);
  StaticJsonDocument<2048> doc;
  auto err = deserializeJson(doc, msg);
  if (err || !doc.is<JsonArray>()) return;
  int type = doc[0];
  String id = doc[1] | "";
  if (type == 3) {
    JsonObject payload = doc[2].as<JsonObject>();
    if (payload.containsKey("status") && String(payload["status"] | "") == "Accepted") {
      ocppAccepted = true;
      lastEvent = "Accepted";
      sendStatusNotification(state);
    }
    if (payload.containsKey("interval")) heartbeatIntervalSec = payload["interval"];
    if (payload.containsKey("transactionId")) {
      transactionId = payload["transactionId"];
      setState("Charging");
      sendStatusNotification("Charging");
      lastEvent = "Transaction started";
    }
  } else if (type == 2) {
    String action = doc[2] | "";
    StaticJsonDocument<256> res;
    if (action == "RemoteStartTransaction") {
      JsonObject payload = doc[3].as<JsonObject>();
      if (payload.containsKey("idTag")) idTag = String((const char*)payload["idTag"]);
      res["status"] = "Accepted";
      wsSendCallResult(id, res);
      plugged = true; authorized = true; setState("Preparing"); sendStartTransaction();
    } else if (action == "RemoteStopTransaction") {
      res["status"] = "Accepted";
      wsSendCallResult(id, res);
      sendStopTransaction("Remote");
      transactionId = -1; authorized = false; setState(plugged ? "Preparing" : "Available");
      sendStatusNotification(state);
    } else if (action == "Reset") {
      res["status"] = "Accepted";
      wsSendCallResult(id, res);
    } else if (action == "UnlockConnector") {
      res["status"] = "Unlocked";
      wsSendCallResult(id, res);
    } else if (action == "ChangeAvailability") {
      res["status"] = "Accepted";
      wsSendCallResult(id, res);
    } else {
      wsSendCallResult(id, res);
    }
  }
}

void wsEvent(WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_CONNECTED) {
    wsConnected = true; ocppAccepted = false; lastEvent = "WebSocket connected"; sendBootNotification();
  } else if (type == WStype_DISCONNECTED) {
    wsConnected = false; ocppAccepted = false; lastEvent = "WebSocket disconnected";
  } else if (type == WStype_TEXT) {
    handleOcppMessage(String((char*)payload));
  }
}

void connectOcpp() {
  String url = fullOcppUrl();
  String host, path; uint16_t port; bool secure;
  parseUrl(url, host, port, path, secure);
  ws.disconnect();
  ws.setReconnectInterval(5000);
  ws.onEvent(wsEvent);
  if (backendPassword.length() > 0) {
    String auth = base64::encode(chargeboxId + ":" + backendPassword);
    //ws.setAuthorization("Basic " + auth);
    String authHeader = "Basic " + auth;
ws.setAuthorization(authHeader.c_str());

  }
  ws.setExtraHeaders("Sec-WebSocket-Protocol: ocpp1.6\r\n");
  if (secure) {
    ws.beginSSL(host.c_str(), port, path.c_str(), "");
  } else {
    ws.begin(host.c_str(), port, path.c_str());
  }
  lastEvent = "Connecting OCPP " + url;
}

String htmlPage() {
  return R"HTML(<!doctype html><html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32-S3 OCPP Wallbox Simulator</title><style>body{font-family:Arial,sans-serif;background:#eef3f7;margin:0;padding:24px;color:#102033}.wrap{max-width:980px;margin:auto}.card{background:#fff;border-radius:18px;padding:20px;margin-bottom:16px;box-shadow:0 10px 30px #0001}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px}.metric{background:#f6f9fc;border-radius:12px;padding:12px}.label{font-size:12px;color:#607084}.value{font-size:22px;font-weight:700}button{border:0;border-radius:12px;padding:12px 14px;font-weight:700;background:#102033;color:white;cursor:pointer}.secondary{background:#dce6ee;color:#102033}.warn{background:#c62828}.good{background:#137cbd}.buttons{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}input{box-sizing:border-box;width:100%;padding:10px;border-radius:10px;border:1px solid #ccd6e0}pre{background:#0b1622;color:#d8eaff;border-radius:14px;padding:14px;overflow:auto}.pill{display:inline-block;padding:8px 12px;border-radius:999px;background:#e8eef5;font-weight:700}</style></head><body><div class="wrap"><div class="card"><h1>ESP32-S3 OCPP Wallbox Simulator</h1><div class="pill" id="state">...</div></div><div class="card"><div class="grid"><div class="metric"><div class="label">OCPP WS</div><div class="value" id="ws">...</div></div><div class="metric"><div class="label">Boot accepted</div><div class="value" id="accepted">...</div></div><div class="metric"><div class="label">Plugged</div><div class="value" id="plugged">...</div></div><div class="metric"><div class="label">Transaction</div><div class="value" id="tx">...</div></div><div class="metric"><div class="label">Power</div><div class="value"><span id="power">...</span> kW</div></div><div class="metric"><div class="label">Session</div><div class="value"><span id="session">...</span> kWh</div></div></div></div><div class="card"><h2>Bedienung</h2><div class="buttons"><button onclick="action('connect')">OCPP verbinden</button><button onclick="action('plug')">Auto anstecken</button><button class="secondary" onclick="action('unplug')">Auto abziehen</button><button onclick="action('authorize')">Authorize</button><button class="good" onclick="action('start')">StartTransaction</button><button class="secondary" onclick="action('stop')">StopTransaction</button><button class="warn" onclick="action('fault')">Faulted</button><button class="secondary" onclick="action('reset')">Reset</button></div></div><div class="card"><h2>Ladeleistung</h2><input id="powerSlider" type="range" min="0" max="50" step="1" oninput="setPower(this.value)"></div><div class="card"><h2>Backend</h2><div class="grid"><div><div class="label">ChargeboxID</div><input id="chargeboxId"></div><div><div class="label">BackendURL ohne CBID</div><input id="backendUrl"></div><div><div class="label">Basic-Auth Passwort</div><input id="backendPassword" type="password"></div><div><div class="label">idTag</div><input id="idTag"></div><div><label><input id="useWss" type="checkbox" style="width:auto"> WSS</label></div></div><br><button onclick="saveBackendConfig()">Speichern</button></div><div class="card"><h2>Status JSON</h2><pre id="json">...</pre></div></div><script>async function refresh(){let s=await(await fetch('/api/state')).json();state.textContent=s.state;ws.textContent=s.ocpp.wsConnected?'Ja':'Nein';accepted.textContent=s.ocpp.accepted?'Ja':'Nein';plugged.textContent=s.plugged?'Ja':'Nein';tx.textContent=s.transactionId;power.textContent=s.powerKw.toFixed(1);session.textContent=(s.sessionWh/1000).toFixed(3);powerSlider.value=s.powerKw;chargeboxId.value=s.backend.chargeboxId;backendUrl.value=s.backend.backendUrl;backendPassword.value=s.backend.passwordSet?'********':'';idTag.value=s.backend.idTag;useWss.checked=s.backend.useWss;json.textContent=JSON.stringify(s,null,2)}async function action(c){await fetch('/api/action?cmd='+encodeURIComponent(c));refresh()}async function setPower(v){await fetch('/api/power?value='+encodeURIComponent(v));refresh()}async function saveBackendConfig(){let p={chargeboxId:chargeboxId.value,backendUrl:backendUrl.value,backendPassword:backendPassword.value==='********'?'':backendPassword.value,useWss:useWss.checked,idTag:idTag.value};await fetch('/api/backend',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});refresh()}setInterval(refresh,1000);refresh();</script></body></html>)HTML";
}

void sendState() {
  StaticJsonDocument<1024> doc;
  doc["state"] = state; doc["plugged"] = plugged; doc["authorized"] = authorized; doc["faulted"] = faulted;
  doc["powerKw"] = powerKw; doc["meterWh"] = meterWh; doc["sessionWh"] = sessionWh; doc["transactionId"] = transactionId; doc["lastEvent"] = lastEvent; doc["faultReason"] = faultReason; doc["ntpSynced"] = ntpSynced(); doc["time"] = isoTimestamp();
  JsonObject gpio = doc.createNestedObject("gpio"); gpio["plug"] = digitalRead(PIN_IN_PLUG)==LOW; gpio["auth"] = digitalRead(PIN_IN_AUTH)==LOW; gpio["start"] = digitalRead(PIN_IN_START)==LOW; gpio["stop"] = digitalRead(PIN_IN_STOP)==LOW; gpio["fault"] = digitalRead(PIN_IN_FAULT)==LOW; gpio["reset"] = digitalRead(PIN_IN_RESET)==LOW; gpio["connect"] = digitalRead(PIN_IN_CONNECT)==LOW; gpio["powerPotRaw"] = analogRead(PIN_POWER_POT); gpio["chargeEnableOut"] = digitalRead(PIN_OUT_CHARGE_ENABLE)==HIGH; gpio["rgbLedPin"] = PIN_RGB_LED;
  JsonObject b = doc.createNestedObject("backend"); b["chargeboxId"] = chargeboxId; b["backendUrl"] = backendUrl; b["useWss"] = useWss; b["idTag"] = idTag; b["fullUrl"] = fullOcppUrl(); b["passwordSet"] = backendPassword.length() > 0;
  JsonObject o = doc.createNestedObject("ocpp"); o["wsConnected"] = wsConnected; o["accepted"] = ocppAccepted; o["heartbeatIntervalSec"] = heartbeatIntervalSec;
  String out; serializeJson(doc, out); server.send(200, "application/json", out);
}

void handleAction() {
  String cmd = server.arg("cmd");
  if (cmd == "connect") connectOcpp();
  else if (cmd == "plug") { plugged = true; setState("Preparing"); sendStatusNotification("Preparing"); }
  else if (cmd == "unplug") { if (transactionId >= 0) sendStopTransaction("EVDisconnected"); transactionId = -1; plugged = false; authorized = false; setState("Available"); sendStatusNotification("Available"); }
  else if (cmd == "authorize") { authorized = true; sendAuthorize(); }
  else if (cmd == "start") { if (plugged) sendStartTransaction(); }
  else if (cmd == "stop") { sendStopTransaction("Local"); transactionId = -1; authorized = false; setState(plugged ? "Preparing" : "Available"); sendStatusNotification(state); }
  else if (cmd == "fault") { faulted = true; faultReason = "Manual fault"; setState("Faulted"); sendStatusNotification("Faulted", "OtherError"); }
  else if (cmd == "reset") { faulted = false; faultReason = ""; transactionId = -1; authorized = false; sessionWh = 0; setState(plugged ? "Preparing" : "Available"); sendStatusNotification(state); }
  server.send(200, "text/plain", "OK");
}

void handlePower(){ if(server.hasArg("value")){ powerKw=server.arg("value").toFloat(); if(powerKw<0)powerKw=0; if(powerKw>350)powerKw=350; if(state=="Charging" && powerKw<=0.1){setState("SuspendedEVSE"); sendStatusNotification("SuspendedEVSE");} else if(state=="SuspendedEVSE" && powerKw>0.1){setState("Charging"); sendStatusNotification("Charging");}} server.send(200,"text/plain","OK"); }

void handleBackend(){
  StaticJsonDocument<512> doc; if(deserializeJson(doc, server.arg("plain"))){server.send(400,"text/plain","Invalid JSON");return;}
  String pass = doc["backendPassword"] | ""; if(pass.length()==0) pass=backendPassword;
  saveConfig(doc["chargeboxId"] | chargeboxId, doc["backendUrl"] | backendUrl, pass, doc["useWss"] | true, doc["idTag"] | idTag);
  lastEvent="Backend config saved"; server.send(200,"text/plain","OK");
}

bool fallingEdgeDebounced(int pin, bool &lastState) {
  bool current = digitalRead(pin);
  bool edge = (lastState == HIGH && current == LOW);
  lastState = current;
  return edge;
}

void setupHardwareInputs() {
  pinMode(PIN_IN_PLUG, INPUT_PULLUP);
  pinMode(PIN_IN_AUTH, INPUT_PULLUP);
  pinMode(PIN_IN_START, INPUT_PULLUP);
  pinMode(PIN_IN_STOP, INPUT_PULLUP);
  pinMode(PIN_IN_FAULT, INPUT_PULLUP);
  pinMode(PIN_IN_RESET, INPUT_PULLUP);
  pinMode(PIN_IN_CONNECT, INPUT_PULLUP);
  analogReadResolution(12);
  lastPlugPhysical = digitalRead(PIN_IN_PLUG);
}

void handlePlugSwitch() {
  bool current = digitalRead(PIN_IN_PLUG);
  bool currentPlugged = current == LOW;
  if (current != lastPlugPhysical) {
    lastPlugPhysical = current;
    plugged = currentPlugged;
    if (plugged) {
      setState("Preparing");
      sendStatusNotification("Preparing");
      lastEvent = "Hardware plug detected";
    } else {
      if (transactionId >= 0) sendStopTransaction("EVDisconnected");
      transactionId = -1;
      authorized = false;
      setState("Available");
      sendStatusNotification("Available");
      lastEvent = "Hardware unplug detected";
    }
  }
}

void handlePowerPot() {
  if (millis() - lastPowerPotMs < 500) return;
  lastPowerPotMs = millis();
  int raw = analogRead(PIN_POWER_POT);
  float newPower = round((raw / 4095.0) * 500.0) / 10.0; // 0.0 - 50.0 kW
  if (fabs(newPower - powerKw) >= 0.5) {
    powerKw = newPower;
    if(state=="Charging" && powerKw<=0.1){setState("SuspendedEVSE"); sendStatusNotification("SuspendedEVSE");}
    else if(state=="SuspendedEVSE" && powerKw>0.1){setState("Charging"); sendStatusNotification("Charging");}
  }
}

void handleHardwareInputs() {
  if (millis() - lastInputMs < 40) return;
  lastInputMs = millis();

  handlePlugSwitch();

  if (fallingEdgeDebounced(PIN_IN_CONNECT, lastConnect)) {
    connectOcpp();
    lastEvent = "Hardware OCPP connect";
  }
  if (fallingEdgeDebounced(PIN_IN_AUTH, lastAuth)) {
    authorized = true;
    sendAuthorize();
    lastEvent = "Hardware authorize";
  }
  if (fallingEdgeDebounced(PIN_IN_START, lastStart)) {
    if (plugged) {
      sendStartTransaction();
      lastEvent = "Hardware start";
    }
  }
  if (fallingEdgeDebounced(PIN_IN_STOP, lastStop)) {
    sendStopTransaction("Local");
    transactionId = -1;
    authorized = false;
    setState(plugged ? "Preparing" : "Available");
    sendStatusNotification(state);
    lastEvent = "Hardware stop";
  }
  if (fallingEdgeDebounced(PIN_IN_FAULT, lastFault)) {
    faulted = true;
    faultReason = "Hardware fault input";
    setState("Faulted");
    sendStatusNotification("Faulted", "OtherError");
    lastEvent = "Hardware fault";
  }
  if (fallingEdgeDebounced(PIN_IN_RESET, lastReset)) {
    faulted = false;
    faultReason = "";
    transactionId = -1;
    authorized = false;
    sessionWh = 0;
    setState(plugged ? "Preparing" : "Available");
    sendStatusNotification(state);
    lastEvent = "Hardware reset";
  }

  handlePowerPot();
}

void setup(){
  Serial.begin(115200); loadConfig();
  pinMode(LED_AVAILABLE,OUTPUT); pinMode(LED_PREPARING,OUTPUT); pinMode(LED_CHARGING,OUTPUT); pinMode(LED_FAULTED,OUTPUT); pinMode(PIN_OUT_CHARGE_ENABLE, OUTPUT); digitalWrite(PIN_OUT_CHARGE_ENABLE, LOW); rgbLed.begin(); rgbLed.clear(); rgbLed.show(); setupHardwareInputs(); setState("Available");
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASS); Serial.print("WiFi"); while(WiFi.status()!=WL_CONNECTED){delay(500);Serial.print(".");} Serial.println(); Serial.println(WiFi.localIP());
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  Serial.print("NTP sync");
  for (int i = 0; i < 20 && !ntpSynced(); i++) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.println("Time: " + isoTimestamp());
  server.on("/", [](){server.send(200,"text/html",htmlPage());}); server.on("/api/state", sendState); server.on("/api/action", handleAction); server.on("/api/power", handlePower); server.on("/api/backend", HTTP_POST, handleBackend); server.begin();
  connectOcpp();
}

void loop(){
  server.handleClient(); ws.loop(); handleHardwareInputs(); updateMeter();
  if(wsConnected && ocppAccepted && millis()-lastHeartbeatMs > heartbeatIntervalSec*1000UL) sendHeartbeat();
  if(wsConnected && state=="Charging" && millis()-lastMeterMs > 10000) sendMeterValues();
}
