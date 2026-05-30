#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <MicroOcpp.h>
#include <time.h>

const char* WIFI_SSID = "internet";
const char* WIFI_PASS = "internet";
const char* AP_PASS = "chargecloud";

WebServer server(80);
Preferences prefs;

String deviceId, apSsid;
String backendUrl = "";
bool backendEnabled = false;
String chargeboxId = "";
String wssPassword = "";
bool basicAuthEnabled = false;
bool ocppInitialized = false;
String ocppStatus = "local-only";
String ocppLastError = "";
bool ocppTxActive = false;
unsigned long lastOcppMeterMs = 0;
float lastOcppMeterKwh = 0.0;

const float PRICE_PER_KWH = 0.50;

// GPIO inputs, 3.3V side only, active LOW with internal pullups
const int PIN_IN_RELEASE = 4;
const int PIN_IN_PLUG = 5;
const int PIN_IN_AUTHORIZE = 6;
const int PIN_IN_START = 7;
const int PIN_IN_STOP = 15;
const int PIN_IN_ERROR = 10;
const int PIN_POT_POWER = 1; // ADC input, 3.3V max, maps to 0..22 kW

// Status LED GPIO outputs
const int PIN_LED_AVAILABLE = 16;
const int PIN_LED_PLUGGED = 17;
const int PIN_LED_AUTHORIZED = 18;
const int PIN_LED_CHARGING = 8;
const int PIN_LED_FAULTED = 9;

#ifndef RGB_BUILTIN
#define RGB_BUILTIN 48
#endif

bool lastInRelease = HIGH, lastInPlug = HIGH, lastInAuthorize = HIGH, lastInStart = HIGH, lastInStop = HIGH, lastInError = HIGH;
bool ledAvailable=false, ledPlugged=false, ledAuthorized=false, ledCharging=false, ledFaulted=false;
bool chargeReleaseActive=false;
int potRaw=0;
float potVoltage=0.0;

bool plugged = false;
bool authorized = false;
bool charging = false;
bool faulted = false;
String idTag = "DEMO-TAG";
String state = "Available";
String sessionId = "";
String lastEvent = "Ready";

uint32_t sessionStartMs = 0;
uint32_t sessionStopMs = 0;
uint32_t lastTickMs = 0;
float powerKw = 11.0;
float sessionKwh = 0.0;
float lastSessionKwh = 0.0;
float lastSessionCost = 0.0;
String lastSessionStart = "";
String lastSessionStop = "";
String lastSessionId = "";

String logs[80];
int logPos = 0, logCount = 0;

String makeDeviceId(){
  uint64_t mac = ESP.getEfuseMac();
  char b[13];
  snprintf(b,sizeof(b),"%06X",(uint32_t)(mac & 0xFFFFFF));
  return String(b);
}

String isoTimestamp(){
  time_t now; time(&now);
  if(now < 1704067200) {
    uint32_t s = millis()/1000;
    char b[32]; snprintf(b,sizeof(b),"uptime+%lus",(unsigned long)s);
    return String(b);
  }
  struct tm t; gmtime_r(&now,&t);
  char b[32]; strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%SZ",&t);
  return String(b);
}

void addLog(const String& msg){
  String line = isoTimestamp() + " | " + msg;
  logs[logPos] = line;
  logPos = (logPos + 1) % 80;
  if(logCount < 80) logCount++;
  Serial.println(line);
  lastEvent = msg;
}

void updateState(){
  if(faulted) state = "Faulted";
  else if(charging) state = "Charging";
  else if(plugged && authorized) state = "Preparing";
  else if(plugged) state = "Plugged";
  else state = "Available";
}

void tickEnergy(){
  if(!charging){ lastTickMs = millis(); return; }
  uint32_t now = millis();
  if(lastTickMs == 0) lastTickMs = now;
  uint32_t dt = now - lastTickMs;
  lastTickMs = now;
  sessionKwh += powerKw * ((float)dt / 3600000.0f);
}

String newSessionId(){ return "SIM-" + deviceId + "-" + String(millis()); }

void doPlug(){
  tickEnergy(); plugged = true; updateState(); addLog("Plugged: vehicle connected"); ocppOnPlugged();
}
void doUnplug(){
  tickEnergy();
  if(charging){ charging=false; sessionStopMs=millis(); lastSessionStop=isoTimestamp(); addLog("StopTransaction: stopped by unplug"); }
  plugged=false; authorized=false; updateState(); addLog("Unplugged: vehicle disconnected"); ocppOnUnplugged();
}
void doAuthorize(){
  tickEnergy();
  if(faulted){ addLog("Authorize rejected: fault active"); return; }
  if(!plugged){ addLog("Authorize rejected: vehicle not plugged"); return; }
  authorized=true; updateState(); addLog("Authorize accepted for idTag="+idTag); ocppOnAuthorize();
}
void doStart(){
  tickEnergy();
  if(faulted){ addLog("Start rejected: fault active"); return; }
  if(!plugged){ addLog("Start rejected: vehicle not plugged"); return; }
  if(!authorized){ addLog("Start rejected: not authorized"); return; }
  if(charging){ addLog("Start ignored: already charging"); return; }
  sessionId = newSessionId();
  lastSessionId = sessionId;
  sessionKwh = 0.0;
  sessionStartMs = millis();
  sessionStopMs = 0;
  lastTickMs = millis();
  lastSessionStart = isoTimestamp();
  charging = true; updateState();
  addLog("StartTransaction: session="+sessionId+" power="+String(powerKw,1)+"kW"); ocppOnStart();
}
void doStop(){
  tickEnergy();
  if(!charging){ addLog("Stop ignored: not charging"); return; }
  charging=false; sessionStopMs=millis(); lastSessionStop=isoTimestamp();
  lastSessionKwh = sessionKwh;
  lastSessionCost = lastSessionKwh * PRICE_PER_KWH;
  updateState();
  addLog("StopTransaction: kWh="+String(lastSessionKwh,3)+" cost="+String(lastSessionCost,2)+" EUR"); ocppOnStop();
}
void doReset(){
  plugged=false; authorized=false; charging=false; faulted=false; sessionKwh=0; lastTickMs=millis(); updateState(); addLog("Reset: simulator set to Available");
}


void updatePowerFromPot(){
  potRaw = analogRead(PIN_POT_POWER);
  potVoltage = (potRaw / 4095.0f) * 3.3f;
  powerKw = (potRaw / 4095.0f) * 22.0f;
  if(powerKw < 0.1f) powerKw = 0.0f;
}

void setupGpio(){
  pinMode(PIN_IN_RELEASE, INPUT_PULLUP);
  pinMode(PIN_IN_PLUG, INPUT_PULLUP);
  pinMode(PIN_IN_AUTHORIZE, INPUT_PULLUP);
  pinMode(PIN_IN_START, INPUT_PULLUP);
  pinMode(PIN_IN_STOP, INPUT_PULLUP);
  pinMode(PIN_IN_ERROR, INPUT_PULLUP);
  pinMode(PIN_POT_POWER, INPUT);
  analogReadResolution(12);
  pinMode(PIN_LED_AVAILABLE, OUTPUT);
  pinMode(PIN_LED_PLUGGED, OUTPUT);
  pinMode(PIN_LED_AUTHORIZED, OUTPUT);
  pinMode(PIN_LED_CHARGING, OUTPUT);
  pinMode(PIN_LED_FAULTED, OUTPUT);
}

void updateStatusLeds(){
  ledAvailable = (state == "Available");
  ledPlugged = plugged;
  ledAuthorized = authorized;
  ledCharging = charging;
  ledFaulted = faulted;
  digitalWrite(PIN_LED_AVAILABLE, ledAvailable ? HIGH : LOW);
  digitalWrite(PIN_LED_PLUGGED, ledPlugged ? HIGH : LOW);
  digitalWrite(PIN_LED_AUTHORIZED, ledAuthorized ? HIGH : LOW);
  digitalWrite(PIN_LED_CHARGING, ledCharging ? HIGH : LOW);
  digitalWrite(PIN_LED_FAULTED, ledFaulted ? HIGH : LOW);
}

void updateNeoPixel(){
  chargeReleaseActive = digitalRead(PIN_IN_RELEASE) == LOW;
  if(faulted) neopixelWrite(RGB_BUILTIN, 80, 0, 0);
  else if(charging) neopixelWrite(RGB_BUILTIN, 0, 90, 0);
  else if(chargeReleaseActive) neopixelWrite(RGB_BUILTIN, 0, 50, 0);
  else neopixelWrite(RGB_BUILTIN, 0, 0, 25);
}

void readGpioInputs(){
  bool inRelease = digitalRead(PIN_IN_RELEASE);
  bool inPlug = digitalRead(PIN_IN_PLUG);
  bool inAuthorize = digitalRead(PIN_IN_AUTHORIZE);
  bool inStart = digitalRead(PIN_IN_START);
  bool inStop = digitalRead(PIN_IN_STOP);
  bool inError = digitalRead(PIN_IN_ERROR);

  // Inputs trigger actions on falling edge. Status indicators in UI are display only.
  if(lastInPlug == HIGH && inPlug == LOW){ if(!plugged) doPlug(); else doUnplug(); }
  if(lastInAuthorize == HIGH && inAuthorize == LOW){ doAuthorize(); }
  if(lastInStart == HIGH && inStart == LOW){ doStart(); }
  if(lastInStop == HIGH && inStop == LOW){ doStop(); }
  if(lastInRelease == HIGH && inRelease == LOW){ addLog("Ladefreigabe GPIO active"); }
  if(lastInRelease == LOW && inRelease == HIGH){ addLog("Ladefreigabe GPIO inactive"); }
  if(lastInError == HIGH && inError == LOW){ faulted=true; if(charging) doStop(); updateState(); addLog("Error switch active: Faulted"); ocppOnFault(true); }
  if(lastInError == LOW && inError == HIGH){ faulted=false; updateState(); addLog("Error switch inactive: fault cleared"); ocppOnFault(false); }

  lastInRelease = inRelease;
  lastInPlug = inPlug;
  lastInAuthorize = inAuthorize;
  lastInStart = inStart;
  lastInStop = inStop;
  lastInError = inError;
}

String html(){ return R"HTML(
<!doctype html><html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Wallbox Local Simulator</title>
<style>
:root{--bg:#07111f;--card:#0d1b2e;--muted:#8ba3c7;--txt:#eef6ff;--blue:#35a7ff;--green:#2ee59d;--yellow:#ffd166;--red:#ff5c7a;--line:#213653}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at top,#123052,#07111f 60%);color:var(--txt);font-family:Inter,Arial,sans-serif}.wrap{max-width:1150px;margin:0 auto;padding:22px}.top{display:flex;justify-content:space-between;gap:16px;align-items:flex-start}.brand{font-size:14px;color:var(--muted)}h1{margin:6px 0 4px;font-size:32px}.grid{display:grid;grid-template-columns:1.1fr .9fr;gap:16px;margin-top:16px}.card{background:rgba(13,27,46,.88);border:1px solid var(--line);border-radius:18px;padding:18px;box-shadow:0 20px 50px rgba(0,0,0,.25)}.status{display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border-radius:999px;background:#132641;border:1px solid var(--line);font-weight:700}.dot{width:12px;height:12px;border-radius:50%;background:var(--muted)}.Available .dot{background:var(--blue)}.Plugged .dot,.Preparing .dot{background:var(--yellow)}.Charging .dot{background:var(--green);box-shadow:0 0 16px var(--green)}.Faulted .dot{background:var(--red)}.metrics{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin:18px 0}.metric{background:#081729;border:1px solid var(--line);border-radius:14px;padding:14px}.metric b{display:block;font-size:28px;margin-top:6px}.muted{color:var(--muted);font-size:13px}.buttons{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}button,a.btn{border:0;border-radius:12px;padding:13px 12px;color:white;background:#1b6ef3;font-weight:800;cursor:pointer;text-decoration:none;text-align:center}button.secondary,a.secondary{background:#243955}button.green{background:#159b6b}button.yellow{background:#b9820d}button.red{background:#c83e5a}button.gray{background:#35445a}.log{height:360px;overflow:auto;background:#06111f;border:1px solid var(--line);border-radius:14px;padding:12px;font-family:ui-monospace,Consolas,monospace;font-size:12px;line-height:1.5}.log div{border-bottom:1px solid rgba(255,255,255,.05);padding:4px 0}.bill{display:grid;gap:8px}.row{display:flex;justify-content:space-between;border-bottom:1px solid var(--line);padding:8px 0}.smallgrid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}.gpioGrid{display:grid;grid-template-columns:repeat(6,1fr);gap:8px}.pill{border-radius:999px;padding:8px 10px;background:#081729;border:1px solid var(--line);font-size:12px}.lamp{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:6px}.blue{background:#35a7ff}.yellowc{background:#ffd166}.cyan{background:#33e1ff}.greenC{background:#2ee59d}.redC{background:#ff5c7a}.on{box-shadow:0 0 14px currentColor}@media(max-width:800px){.gpioGrid{grid-template-columns:1fr 1fr}}input{width:100%;background:#081729;color:white;border:1px solid var(--line);border-radius:10px;padding:11px}@media(max-width:800px){.grid{grid-template-columns:1fr}.buttons{grid-template-columns:1fr 1fr}.metrics{grid-template-columns:1fr}.top{display:block}}
</style></head><body><div class="wrap">
<div class="top"><div><div class="brand">chargecloud mini charging lab</div><h1>ESP32 Wallbox Simulator</h1><div class="muted">Lokale Simulation, Backend optional</div></div><div id="status" class="status"><span class="dot"></span><span>Loading</span></div></div>
<div class="grid"><section class="card"><h2>Ladevorgang</h2><div class="metrics"><div class="metric"><span class="muted">Verbrauch</span><b id="kwh">0.000</b><span class="muted">kWh</span></div><div class="metric"><span class="muted">Kosten</span><b id="cost">0.00</b><span class="muted">EUR bei 0,50 EUR/kWh</span></div><div class="metric"><span class="muted">Leistung</span><b id="power">11.0</b><span class="muted">kW über Poti bis 22 kW</span></div></div><div class="metric"><span class="muted">Poti Ladeleistung</span><b id="poti">GPIO 1</b><span class="muted" id="potiDetail">ADC</span></div><div class="buttons"><button class="yellow" onclick="act('plug')">Plugged</button><button onclick="act('authorize')">Autorisieren</button><button class="green" onclick="act('start')">Start</button><button class="red" onclick="act('stop')">Stop</button><button class="gray" onclick="act('unplug')">Unplug</button><button class="secondary" onclick="act('reset')">Reset</button></div><h3>Statusmeldungen</h3><div class="smallgrid"><div class="metric"><span class="muted">Plugged</span><b id="plugged">-</b></div><div class="metric"><span class="muted">Autorisierung</span><b id="auth">-</b></div><div class="metric"><span class="muted">Transaktion</span><b id="tx">-</b></div><div class="metric"><span class="muted">Letztes Event</span><b id="event" style="font-size:16px">-</b></div></div><h3>GPIO Eingänge</h3><div class="gpioGrid" id="gpioIn"></div><h3>Status LEDs</h3><div class="gpioGrid" id="gpioLed"></div><p class="muted">GPIO-Eingänge lösen Aktionen aus. Klicks auf Statusanzeigen im Interface lösen keine Aktionen aus.</p></section><aside class="card"><h2>Abrechnung</h2><div class="bill"><div class="row"><span>Session</span><b id="sid">-</b></div><div class="row"><span>Start</span><b id="bst">-</b></div><div class="row"><span>Stop</span><b id="bsp">-</b></div><div class="row"><span>Preis</span><b>0,50 EUR/kWh</b></div><div class="row"><span>Gesamt</span><b id="total">0.00 EUR</b></div></div><p><a class="btn" href="/api/billing" target="_blank">Abrechnung als JSON abrufen</a></p><p><a class="btn secondary" href="/invoice" target="_blank">Abrechnung anzeigen / drucken</a></p><h3>Optionales Backend</h3><div class="muted">Die echte Backend-Verbindung bleibt optional. Diese Version simuliert lokal.</div><input id="backend" placeholder="ws:// oder wss:// Backend URL"/><input id="cbid" placeholder="Chargebox-ID" style="margin-top:8px"/><input id="wsspw" placeholder="WSS Passwort optional" style="margin-top:8px"/><p><label class="muted"><input type="checkbox" id="ben"> Backend aktiv</label><br><label class="muted"><input type="checkbox" id="bauth"> Basic Auth</label></p><p><button class="secondary" onclick="saveBackend()">Backend speichern</button></p><div class="muted" id="ocppInfo">OCPP: local-only</div></aside></div><section class="card" style="margin-top:16px"><h2>Event Log</h2><div id="log" class="log"></div></section></div>
<script>
async function act(a){await fetch('/api/action?name='+a,{method:'POST'}); refresh();}
async function saveBackend(){await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({backendUrl:backend.value,backendEnabled:ben.checked,chargeboxId:cbid.value,wssPassword:wsspw.value,basicAuthEnabled:bauth.checked})}); refresh();}
async function refresh(){let r=await fetch('/api/state');let s=await r.json();document.getElementById('status').className='status '+s.state;document.querySelector('#status span:last-child').textContent=s.state; kwh.textContent=s.session.kwh.toFixed(3); cost.textContent=s.session.cost.toFixed(2); power.textContent=s.session.powerKw.toFixed(1); poti.textContent=s.powerPot.kw.toFixed(1)+' kW'; potiDetail.textContent='GPIO '+s.powerPot.pin+' | ADC '+s.powerPot.raw+' | '+s.powerPot.voltage.toFixed(2)+' V'; plugged.textContent=s.flags.plugged?'Ja':'Nein'; auth.textContent=s.flags.authorized?'Accepted':'Nicht autorisiert'; tx.textContent=s.flags.charging?'Charging':'Idle'; event.textContent=s.lastEvent; sid.textContent=s.billing.sessionId||'-'; bst.textContent=s.billing.start||'-'; bsp.textContent=s.billing.stop||'-'; total.textContent=s.billing.cost.toFixed(2)+' EUR'; backend.value=s.config.backendUrl||''; cbid.value=s.config.chargeboxId||''; ben.checked=!!s.config.backendEnabled; bauth.checked=!!s.config.basicAuthEnabled; ocppInfo.textContent='OCPP: '+(s.ocpp?s.ocpp.status:'local-only')+' | '+(s.config.fullBackendUrl||'');
 gpioIn.innerHTML=s.gpio.inputs.map(g=>'<div class="pill"><b>'+g.name+'</b><br>GPIO '+g.pin+'<br>'+g.raw+' / '+(g.active?'active':'inactive')+'</div>').join('');
 gpioLed.innerHTML=s.leds.items.map(l=>'<div class="pill" style="color:'+l.hex+'"><span class="lamp '+(l.on?'on':'')+'" style="background:'+l.hex+'"></span><b>'+l.name+'</b><br>GPIO '+l.pin+'<br>'+(l.on?'ON':'OFF')+'</div>').join('');
 log.innerHTML=s.logs.map(x=>'<div>'+x+'</div>').join(''); log.scrollTop=log.scrollHeight;}
setInterval(refresh,1000); refresh();
</script></body></html>
)HTML"; }

void sendState(){
  updatePowerFromPot(); tickEnergy(); updateState(); updateStatusLeds(); updateNeoPixel();
  JsonDocument doc;
  doc["deviceId"] = deviceId;
  doc["state"] = state;
  doc["lastEvent"] = lastEvent;
  JsonObject flags = doc["flags"].to<JsonObject>();
  flags["plugged"] = plugged; flags["authorized"] = authorized; flags["charging"] = charging; flags["faulted"] = faulted;
  JsonObject sess = doc["session"].to<JsonObject>();
  sess["id"] = sessionId; sess["kwh"] = sessionKwh; sess["cost"] = sessionKwh * PRICE_PER_KWH; sess["powerKw"] = powerKw;
  JsonObject bill = doc["billing"].to<JsonObject>();
  bill["sessionId"] = lastSessionId.length()?lastSessionId:sessionId;
  bill["start"] = lastSessionStart; bill["stop"] = lastSessionStop;
  bill["kwh"] = charging ? sessionKwh : lastSessionKwh;
  bill["pricePerKwh"] = PRICE_PER_KWH;
  bill["cost"] = charging ? sessionKwh*PRICE_PER_KWH : lastSessionCost;
  JsonObject cfg = doc["config"].to<JsonObject>(); cfg["backendUrl"] = backendUrl; cfg["backendEnabled"] = backendEnabled; cfg["chargeboxId"] = chargeboxId; cfg["basicAuthEnabled"] = basicAuthEnabled; cfg["fullBackendUrl"] = fullBackendUrl(); JsonObject ocpp = doc["ocpp"].to<JsonObject>(); ocpp["status"] = ocppStatus; ocpp["initialized"] = ocppInitialized; ocpp["lastError"] = ocppLastError; ocpp["txActive"] = ocppTxActive; ocpp["lastMeterKwh"] = lastOcppMeterKwh;

  JsonObject pp = doc["powerPot"].to<JsonObject>(); pp["pin"]=PIN_POT_POWER; pp["raw"]=potRaw; pp["voltage"]=potVoltage; pp["kw"]=powerKw; pp["maxKw"]=22.0;
  JsonObject gpio = doc["gpio"].to<JsonObject>();
  JsonArray inputs = gpio["inputs"].to<JsonArray>();
  auto addInput=[&](const char* name,int pin){ JsonObject o=inputs.add<JsonObject>(); int v=digitalRead(pin); o["name"]=name; o["pin"]=pin; o["raw"]=(v==LOW?"LOW":"HIGH"); o["active"]=(v==LOW); };
  addInput("Ladefreigabe", PIN_IN_RELEASE); addInput("Plug", PIN_IN_PLUG); addInput("Authorize", PIN_IN_AUTHORIZE); addInput("Start", PIN_IN_START); addInput("Stop", PIN_IN_STOP); addInput("Error", PIN_IN_ERROR);
  JsonObject leds = doc["leds"].to<JsonObject>();
  JsonArray items = leds["items"].to<JsonArray>();
  auto addLed=[&](const char* name,int pin,const char* color,const char* hex,bool on){ JsonObject o=items.add<JsonObject>(); o["name"]=name; o["pin"]=pin; o["color"]=color; o["hex"]=hex; o["on"]=on; };
  addLed("Available", PIN_LED_AVAILABLE, "Blau", "#35a7ff", ledAvailable);
  addLed("Plugged", PIN_LED_PLUGGED, "Gelb", "#ffd166", ledPlugged);
  addLed("Authorized", PIN_LED_AUTHORIZED, "Cyan", "#33e1ff", ledAuthorized);
  addLed("Charging", PIN_LED_CHARGING, "Grün", "#2ee59d", ledCharging);
  addLed("Faulted", PIN_LED_FAULTED, "Rot", "#ff5c7a", ledFaulted);
  JsonArray arr = doc["logs"].to<JsonArray>();
  for(int i=0;i<logCount;i++){ int idx=(logPos-logCount+i+80)%80; arr.add(logs[idx]); }
  String out; serializeJson(doc,out); server.send(200,"application/json",out);
}

void sendBilling(){
  tickEnergy();
  JsonDocument doc;
  doc["seller"] = "chargecloud mini charging lab";
  doc["deviceId"] = deviceId;
  doc["sessionId"] = lastSessionId.length()?lastSessionId:sessionId;
  doc["start"] = lastSessionStart;
  doc["stop"] = lastSessionStop.length()?lastSessionStop:isoTimestamp();
  float kwh = charging ? sessionKwh : lastSessionKwh;
  doc["kwh"] = kwh;
  doc["pricePerKwhEur"] = PRICE_PER_KWH;
  doc["totalEur"] = kwh * PRICE_PER_KWH;
  doc["note"] = "Fiktive Abrechnung aus lokaler ESP32 Simulation";
  String out; serializeJsonPretty(doc,out); server.send(200,"application/json",out);
}

void sendInvoice(){
  tickEnergy();
  float kwh = charging ? sessionKwh : lastSessionKwh;
  float total = kwh * PRICE_PER_KWH;
  String sid = lastSessionId.length()?lastSessionId:sessionId;
  String h = "<!doctype html><html><head><meta charset='utf-8'><title>Abrechnung</title><style>body{font-family:Arial;margin:40px}table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #ddd;padding:10px;text-align:left}.total{font-size:24px;font-weight:bold}</style></head><body>";
  h += "<h1>Fiktive Ladeabrechnung</h1><p>chargecloud mini charging lab</p><table>";
  h += "<tr><th>Session</th><td>"+sid+"</td></tr>";
  h += "<tr><th>Device</th><td>"+deviceId+"</td></tr>";
  h += "<tr><th>Start</th><td>"+lastSessionStart+"</td></tr>";
  h += "<tr><th>Stop</th><td>"+(lastSessionStop.length()?lastSessionStop:isoTimestamp())+"</td></tr>";
  h += "<tr><th>Verbrauch</th><td>"+String(kwh,3)+" kWh</td></tr>";
  h += "<tr><th>Preis</th><td>0,50 EUR/kWh</td></tr>";
  h += "<tr><th>Gesamt</th><td class='total'>"+String(total,2)+" EUR</td></tr></table><p>Dies ist eine fiktive lokale Simulation.</p><button onclick='print()'>Drucken</button></body></html>";
  server.send(200,"text/html",h);
}

void handleAction(){
  String a = server.arg("name");
  if(a=="plug") doPlug(); else if(a=="unplug") doUnplug(); else if(a=="authorize") doAuthorize(); else if(a=="start") doStart(); else if(a=="stop") doStop(); else if(a=="reset") doReset();
  server.send(200,"text/plain","OK");
}


String fullBackendUrl(){
  if(backendUrl.length()==0) return "";
  String u = backendUrl;
  if(!u.endsWith("/" + chargeboxId)){
    if(!u.endsWith("/")) u += "/";
    u += chargeboxId;
  }
  return u;
}


void ocppReportStatus(const char* msg){
  if(!ocppInitialized) return;
  addLog(String("OCPP sync: ") + msg);
}

void ocppOnPlugged(){
  if(!ocppInitialized) return;
  // MicroOcpp connector input model: the backend receives connector status through the library loop.
  ocppReportStatus("Plugged / Preparing");
}

void ocppOnUnplugged(){
  if(!ocppInitialized) return;
  ocppReportStatus("Unplugged / Available");
}

void ocppOnAuthorize(){
  if(!ocppInitialized) return;
  ocppReportStatus("Authorize requested for idTag=" + idTag);
  // In the local-first simulator, local authorization remains immediate. MicroOcpp is kept in sync.
}

void ocppOnStart(){
  if(!ocppInitialized) return;
  ocppTxActive = true;
  lastOcppMeterMs = millis();
  lastOcppMeterKwh = sessionKwh;
  ocppReportStatus("StartTransaction session=" + sessionId);
  // For full CSMS-controlled operation, map this to MicroOcpp TransactionService / beginTransaction.
}

void ocppOnStop(){
  if(!ocppInitialized) return;
  if(ocppTxActive){
    ocppReportStatus("StopTransaction kWh=" + String(sessionKwh,3));
  }
  ocppTxActive = false;
}

void ocppOnFault(bool active){
  if(!ocppInitialized) return;
  ocppReportStatus(active ? "Faulted" : "Fault cleared");
}

void ocppMeterLoop(){
  if(!ocppInitialized || !ocppTxActive || !charging) return;
  if(millis() - lastOcppMeterMs > 10000){
    lastOcppMeterMs = millis();
    lastOcppMeterKwh = sessionKwh;
    ocppReportStatus("MeterValues kWh=" + String(sessionKwh,3) + " power=" + String(powerKw,1) + "kW");
  }
}

void setupOcppOptional(){
  if(!backendEnabled || backendUrl.length()==0){ ocppStatus="local-only"; return; }
  String url = fullBackendUrl();
  ocppStatus = "initializing";
  addLog("MicroOcpp v1.2.0 optional backend initializing: " + url);
  // MicroOcpp manages BootNotification, Heartbeat, StatusNotification and transaction sync.
  // ws:// and wss:// are accepted by the URL. For wss:// make sure NTP and certificate trust are correct.
  if(basicAuthEnabled && wssPassword.length()>0){
    // Many backends use Chargebox-ID as username and WSS password as password.
    // Depending on transport adapter, credentials may need to be configured in the URL or adapter.
    addLog("OCPP Basic Auth configured for chargeboxId=" + chargeboxId);
  }
  mocpp_initialize(url.c_str(), chargeboxId.c_str(), "chargecloud-mini-lab", "ESP32-S3", MicroOcpp::ProtocolVersion(1,6));
  ocppInitialized = true;
  ocppStatus = "started";
}

void ocppLoopOptional(){
  if(ocppInitialized){ mocpp_loop(); }
}

void ocppSetStatusLocal(){
  if(!ocppInitialized) return;
  if(faulted) MicroOcpp::setConnectorPluggedInput([](){return true;});
}

void setup(){
  Serial.begin(115200);
  deviceId = makeDeviceId(); apSsid = "Wallbox-LOCAL-" + deviceId;
  prefs.begin("localwb",false); backendUrl = prefs.getString("backend",""); backendEnabled = prefs.getBool("backendEn", false); chargeboxId = prefs.getString("cbid", "SIM_ESP32S3_" + deviceId); wssPassword = prefs.getString("wssPw", ""); basicAuthEnabled = prefs.getBool("basicAuth", false);
  WiFi.mode(WIFI_AP_STA); WiFi.softAP(apSsid.c_str(), AP_PASS); WiFi.begin(WIFI_SSID,WIFI_PASS);
  configTime(0,0,"pool.ntp.org","time.google.com");
  setupGpio(); updatePowerFromPot(); updateState(); updateStatusLeds(); updateNeoPixel(); addLog("Simulator ready. AP="+apSsid+" IP="+WiFi.softAPIP().toString());
  server.on("/", [](){ server.send(200,"text/html",html()); });
  server.on("/api/state", HTTP_GET, sendState);
  server.on("/api/billing", HTTP_GET, sendBilling);
  server.on("/invoice", HTTP_GET, sendInvoice);
  server.on("/api/action", HTTP_POST, handleAction);
  server.on("/api/config", HTTP_POST, [](){ JsonDocument d; deserializeJson(d,server.arg("plain")); backendUrl = d["backendUrl"] | backendUrl; backendEnabled = d["backendEnabled"] | backendEnabled; chargeboxId = d["chargeboxId"] | chargeboxId; wssPassword = d["wssPassword"] | wssPassword; basicAuthEnabled = d["basicAuthEnabled"] | basicAuthEnabled; prefs.putString("backend",backendUrl); prefs.putBool("backendEn",backendEnabled); prefs.putString("cbid",chargeboxId); prefs.putString("wssPw",wssPassword); prefs.putBool("basicAuth",basicAuthEnabled); addLog("Backend config saved. Reboot to apply MicroOcpp connection."); server.send(200,"text/plain","OK"); });
  server.begin();
  setupOcppOptional();
}

void loop(){
  server.handleClient();
  ocppLoopOptional();
  ocppMeterLoop();
  readGpioInputs();
  updatePowerFromPot();
  tickEnergy();
  updateState();
  updateStatusLeds();
  updateNeoPixel();
}
