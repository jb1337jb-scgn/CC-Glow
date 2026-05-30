#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <Adafruit_NeoPixel.h>

// WLAN
const char* STA_SSID = "internet";
const char* STA_PASSWORD = "internet";
const char* AP_PASSWORD = "chargecloud";
String deviceId;
String apSsid;
WebServer server(80);
Preferences prefs;

// NeoPixel / Onboard RGB LED
#define NEOPIXEL_PIN 48
#define NEOPIXEL_COUNT 1
#define NEOPIXEL_ENABLED true
Adafruit_NeoPixel pixel(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// Pins
const int PIN_POWER_POTI        = 3;
const int PIN_VEHICLE_CONNECTED = 4;
const int PIN_START_AUTH        = 5;
const int PIN_STOP              = 6;
const int PIN_ERROR             = 7;
const int LED_AVAILABLE = 8;
const int LED_PREPARING = 9;
const int LED_CHARGING  = 10;
const int LED_FAULTED   = 11;

const float ENERGY_PRICE_EUR_PER_KWH = 5.00f;
const unsigned long METER_INTERVAL_MS = 10000;
const int MAX_SESSIONS = 20;
const unsigned long DEBOUNCE_MS = 50;

enum ChargerState { IDLE, CONNECTED, CHARGING, ERROR_STATE };
ChargerState state = IDLE;

struct DebouncedInput { int pin; bool stable; bool lastRaw; unsigned long changed; };
DebouncedInput inVehicle{PIN_VEHICLE_CONNECTED,false,false,0};
DebouncedInput inStart{PIN_START_AUTH,false,false,0};
DebouncedInput inStop{PIN_STOP,false,false,0};
DebouncedInput inError{PIN_ERROR,false,false,0};
bool lastStartStable=false, lastStopStable=false;

struct ChargeSession { bool valid; uint32_t id; String startTime; String endTime; float energyKwh; float costEur; };
ChargeSession sessions[MAX_SESSIONS];
int sessionCount=0;
uint32_t nextSessionId=1;
bool sessionActive=false;
String activeSessionStartTime="";
unsigned long sessionStartMillis=0;
unsigned long lastMeterMillis=0;
float activeEnergyKwh=0.0f;

String currentDateTimeString(){
  struct tm t;
  if(getLocalTime(&t,50)){ char b[24]; strftime(b,sizeof(b),"%Y-%m-%d %H:%M:%S",&t); return String(b); }
  unsigned long s=millis()/1000; char b[24]; snprintf(b,sizeof(b),"Uptime %02lu:%02lu:%02lu",s/3600,(s%3600)/60,s%60); return String(b);
}

bool updateDebounced(DebouncedInput &i){
  bool raw = digitalRead(i.pin)==LOW;
  if(raw!=i.lastRaw){ i.lastRaw=raw; i.changed=millis(); }
  if(millis()-i.changed>=DEBOUNCE_MS) i.stable=raw;
  return i.stable;
}

const char* stateName(ChargerState s){
  switch(s){ case IDLE:return "IDLE"; case CONNECTED:return "CONNECTED"; case CHARGING:return "CHARGING"; case ERROR_STATE:return "ERROR"; }
  return "UNKNOWN";
}

float adcToPowerKw(int adc){ return (adc/4095.0f)*22.0f; }
float powerKwToCurrentA(float kw){ return (kw*1000.0f)/(1.732f*400.0f); }

void saveSessions(){
  prefs.begin("sessions",false); prefs.putUInt("nextId",nextSessionId); prefs.putInt("count",sessionCount);
  for(int i=0;i<sessionCount;i++){ String p="s"+String(i)+"_"; prefs.putUInt((p+"id").c_str(),sessions[i].id); prefs.putString((p+"st").c_str(),sessions[i].startTime); prefs.putString((p+"et").c_str(),sessions[i].endTime); prefs.putFloat((p+"kwh").c_str(),sessions[i].energyKwh); prefs.putFloat((p+"eur").c_str(),sessions[i].costEur); }
  prefs.end();
}
void loadSessions(){
  prefs.begin("sessions",true); nextSessionId=prefs.getUInt("nextId",1); sessionCount=prefs.getInt("count",0); if(sessionCount<0)sessionCount=0; if(sessionCount>MAX_SESSIONS)sessionCount=MAX_SESSIONS;
  for(int i=0;i<sessionCount;i++){ String p="s"+String(i)+"_"; sessions[i].valid=true; sessions[i].id=prefs.getUInt((p+"id").c_str(),i+1); sessions[i].startTime=prefs.getString((p+"st").c_str(),"-"); sessions[i].endTime=prefs.getString((p+"et").c_str(),"-"); sessions[i].energyKwh=prefs.getFloat((p+"kwh").c_str(),0); sessions[i].costEur=prefs.getFloat((p+"eur").c_str(),0); }
  prefs.end();
}
void addSession(String st,String et,float kwh){
  ChargeSession cs{true,nextSessionId++,st,et,kwh,kwh*ENERGY_PRICE_EUR_PER_KWH};
  if(sessionCount<MAX_SESSIONS) sessions[sessionCount++]=cs; else { for(int i=1;i<MAX_SESSIONS;i++) sessions[i-1]=sessions[i]; sessions[MAX_SESSIONS-1]=cs; }
  saveSessions();
}
void deleteSession(uint32_t id){ for(int i=0;i<sessionCount;i++) if(sessions[i].id==id){ for(int j=i+1;j<sessionCount;j++) sessions[j-1]=sessions[j]; sessionCount--; saveSessions(); return; } }
void clearSessions(){ sessionCount=0; saveSessions(); }

void updateEnergy(float powerKw,bool force=false){
  if(!sessionActive){ lastMeterMillis=millis(); return; }
  unsigned long now=millis();
  if(force || now-lastMeterMillis>=METER_INTERVAL_MS){ activeEnergyKwh += powerKw*((now-lastMeterMillis)/3600000.0f); lastMeterMillis=now; }
}
void syncSession(float powerKw){
  if(state==CHARGING && !sessionActive){ sessionActive=true; activeSessionStartTime=currentDateTimeString(); sessionStartMillis=millis(); lastMeterMillis=millis(); activeEnergyKwh=0; }
  if(state!=CHARGING && sessionActive){ updateEnergy(powerKw,true); addSession(activeSessionStartTime,currentDateTimeString(),activeEnergyKwh); sessionActive=false; }
  if(state==CHARGING && sessionActive) updateEnergy(powerKw,false);
}

void updateNeoPixel(){
  static unsigned long lastBlink=0;
  static bool on=false;
  if(!NEOPIXEL_ENABLED) return;
  if(state==CHARGING){
    if(millis()-lastBlink>=250){
      lastBlink=millis(); on=!on;
      if(on) pixel.setPixelColor(0,pixel.Color(0,0,255)); else pixel.clear();
      pixel.show();
    }
  } else {
    on=false; pixel.clear(); pixel.show();
  }
}

void updateLeds(){
  digitalWrite(LED_AVAILABLE, state==IDLE);
  digitalWrite(LED_PREPARING, state==CONNECTED);
  digitalWrite(LED_CHARGING, state==CHARGING);
  digitalWrite(LED_FAULTED, state==ERROR_STATE);
}

String getDeviceId(){ uint64_t id=ESP.getEfuseMac(); char b[13]; snprintf(b,sizeof(b),"%04X%08X",(uint16_t)(id>>32),(uint32_t)id); return String(b); }
void setupWiFi(){
  deviceId=getDeviceId(); apSsid="EVSE-SIM-"+deviceId.substring(deviceId.length()-6);
  WiFi.persistent(false); WiFi.disconnect(true,true); WiFi.mode(WIFI_OFF); delay(300); WiFi.mode(WIFI_AP_STA); WiFi.setSleep(false);
  bool ok=WiFi.softAP(apSsid.c_str(),AP_PASSWORD,1,false,4);
  Serial.println(ok?"AP gestartet":"AP Fehler"); Serial.println(apSsid); Serial.println(WiFi.softAPIP());
  WiFi.begin(STA_SSID,STA_PASSWORD); configTime(3600,3600,"pool.ntp.org","time.nist.gov");
}

String htmlPage(){ return R"rawliteral(
<!doctype html><html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Glow Challenge</title><style>
:root{--bg:#08051f;--card:#151033;--line:#37305f;--text:#f7f2ff;--muted:#b8add6;--cyan:#00d9ff;--blue:#367cff;--pink:#ff3ecf;--green:#26f5a8;--red:#ff4d7d;--yellow:#ffd166}*{box-sizing:border-box}body{font-family:Arial,sans-serif;background:radial-gradient(circle at 20% 0%,rgba(255,62,207,.30),transparent 34%),radial-gradient(circle at 80% 10%,rgba(0,217,255,.28),transparent 36%),linear-gradient(160deg,#090420,#071b3d 55%,#19082d);color:var(--text);margin:0;padding:22px}.container{max-width:980px;margin:auto}.brand{text-align:center;margin-bottom:22px}.logo{font-size:2.1rem;font-weight:800}.logo span{color:var(--pink)}h1{text-shadow:0 0 18px rgba(255,62,207,.55),0 0 32px rgba(0,217,255,.35)}.stack{display:flex;flex-direction:column;gap:16px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:16px}.card{background:linear-gradient(160deg,rgba(21,16,51,.96),rgba(10,32,67,.92));border:1px solid rgba(255,62,207,.22);border-radius:16px;padding:16px}.row{display:flex;justify-content:space-between;gap:12px;border-bottom:1px solid var(--line);padding:10px 0}.row:last-child{border-bottom:0}.badge{padding:6px 11px;border-radius:999px;font-weight:700}.on{background:var(--green);color:#062014}.off{background:#506176}.error{background:var(--red)}.warn{background:var(--yellow);color:#271d00}.info{background:linear-gradient(90deg,var(--blue),var(--cyan));color:#001c25}.value{font-weight:700;color:var(--yellow)}button{border:0;border-radius:8px;padding:7px 10px;margin:4px;cursor:pointer}.btnPink{background:var(--pink);color:#fff}.btnBlue{background:var(--blue);color:#fff}.btnCyan{background:var(--cyan);color:#001c25}canvas{width:100%;height:260px;background:rgba(8,10,38,.72);border:1px solid rgba(0,217,255,.18);border-radius:14px}.step{display:flex;align-items:center;gap:10px;padding:10px;border-radius:12px;background:rgba(8,10,38,.72);border:1px solid rgba(0,217,255,.18);margin-top:8px}.dot{width:13px;height:13px;border-radius:50%;background:#506176}.active .dot{background:var(--green);box-shadow:0 0 12px var(--green)}.desc{margin-left:auto;text-align:right;color:var(--muted)}
</style></head><body><div class="container"><div class="brand"><div class="logo">charge<span>cloud</span></div><h1>Glow Challenge</h1><div>EVSE Simulator</div></div><div class="stack">
<div class="grid"><div class="card"><h2>System</h2><div class="row"><span>Geräte-ID</span><span id="deviceId" class="value">-</span></div><div class="row"><span>AP SSID</span><span id="apSsid" class="value">-</span></div><div class="row"><span>AP IP</span><span id="apIp" class="value">-</span></div><div class="row"><span>STA IP</span><span id="staIp" class="value">-</span></div></div><div class="card"><h2>Ladestation</h2><div class="row"><span>Status</span><span id="chargerState" class="badge off">-</span></div><div class="row"><span>OCPP Status</span><span id="ocppStatus" class="badge off">-</span></div><div class="row"><span>Ladeleistung</span><span><span id="powerKw" class="value">0.0</span> kW</span></div><div class="row"><span>Strom</span><span><span id="currentA" class="value">0.0</span> A</span></div></div></div>
<div class="card"><h2>Ladeleistung Verlauf</h2><canvas id="powerChart" width="900" height="260"></canvas></div>
<div class="card"><h2>Aktuelle Session</h2><div class="row"><span>Session Status</span><span id="activeState" class="badge off">INAKTIV</span></div><div class="row"><span>State ist CHARGING</span><span id="stateIsCharging" class="badge off">NEIN</span></div><div class="row"><span>Start</span><span id="activeStart" class="value">-</span></div><div class="row"><span>Dauer</span><span><span id="activeDuration" class="value">0</span> s</span></div><div class="row"><span>Verbrauch</span><span><span id="activeEnergy" class="value">0.000</span> kWh</span></div><div class="row"><span>Kosten</span><span><span id="activeCost" class="value">0.000</span> EUR</span></div><div class="row"><span>Tarif</span><span class="value">5,00 EUR/kWh</span></div></div>
<div class="grid"><div class="card"><h2>Eingänge</h2><div class="row"><span>GPIO3 Poti ADC</span><span id="adcValue" class="value">-</span></div><div class="row"><span>GPIO4 Fahrzeug verbunden</span><span id="inVehicle" class="badge off">-</span></div><div class="row"><span>GPIO5 Start/Auth</span><span id="inStart" class="badge off">-</span></div><div class="row"><span>GPIO6 Stop</span><span id="inStop" class="badge off">-</span></div><div class="row"><span>GPIO7 Fehler</span><span id="inError" class="badge off">-</span></div></div><div class="card"><h2>Ausgänge</h2><div class="row"><span>GPIO8 Available</span><span id="outAvailable" class="badge off">-</span></div><div class="row"><span>GPIO9 Preparing</span><span id="outPreparing" class="badge off">-</span></div><div class="row"><span>GPIO10 Charging</span><span id="outCharging" class="badge off">-</span></div><div class="row"><span>GPIO11 Faulted</span><span id="outFaulted" class="badge off">-</span></div></div></div>
<div class="card"><h2>Aufzeichnung Ladevorgänge</h2><button class="btnCyan" onclick="downloadInvoiceLast3()">Rechnung letzte 3 herunterladen</button><button class="btnBlue" onclick="clearSessions()">Alle löschen</button><div id="sessionsList"></div></div>
</div></div><script>
let latest=null,hist=[],lastCh=false,maxPts=120;function badge(id,on,t='AN',f='AUS',err=false){let e=document.getElementById(id);e.textContent=on?t:f;e.className='badge '+(on?(err?'error':'on'):'off')}function draw(){let c=document.getElementById('powerChart'),x=c.getContext('2d'),w=c.width,h=c.height;x.fillStyle='#080a26';x.fillRect(0,0,w,h);x.strokeStyle='rgba(255,255,255,.12)';for(let i=0;i<=4;i++){let y=(h-30)-i*((h-50)/4);x.beginPath();x.moveTo(45,y);x.lineTo(w-15,y);x.stroke();x.fillStyle='#b8add6';x.fillText((22*i/4).toFixed(0)+' kW',8,y+4)}if(hist.length<2)return;x.beginPath();hist.forEach((p,i)=>{let xx=45+i*((w-60)/(maxPts-1)),yy=(h-30)-(p/22)*(h-50);i?x.lineTo(xx,yy):x.moveTo(xx,yy)});x.strokeStyle='#ff3ecf';x.lineWidth=3;x.stroke()}function chart(d){if(d.state!=='CHARGING'){hist=[];draw();return}hist.push(Number(d.powerKw));if(hist.length>maxPts)hist.shift();draw()}async function upd(){let d=await (await fetch('/api/status',{cache:'no-store'})).json();latest=d;chart(d);document.getElementById('deviceId').textContent=d.deviceId;document.getElementById('apSsid').textContent=d.apSsid;document.getElementById('apIp').textContent=d.apIp;document.getElementById('staIp').textContent=d.staIp;let oc=d.state==='ERROR'?'Faulted':d.state==='CHARGING'?'Charging':d.state==='CONNECTED'?'Preparing':'Available';document.getElementById('chargerState').textContent=d.state;document.getElementById('ocppStatus').textContent=oc;document.getElementById('powerKw').textContent=Number(d.powerKw).toFixed(1);document.getElementById('currentA').textContent=Number(d.currentA).toFixed(1);document.getElementById('adcValue').textContent=d.adcValue;badge('inVehicle',d.inputs.vehicleConnected,'AKTIV','AUS');badge('inStart',d.inputs.startPressed,'GEDRÜCKT','AUS');badge('inStop',d.inputs.stopPressed,'GEDRÜCKT','AUS');badge('inError',d.inputs.errorActive,'AKTIV','OK',true);badge('outAvailable',d.outputs.available);badge('outPreparing',d.outputs.preparing);badge('outCharging',d.outputs.charging);badge('outFaulted',d.outputs.faulted,'AN','AUS',true);badge('activeState',d.session.active,'AKTIV','INAKTIV');badge('stateIsCharging',d.session.stateIsCharging,'JA','NEIN');document.getElementById('activeStart').textContent=d.session.active?d.session.startTime:'-';document.getElementById('activeDuration').textContent=d.session.activeDurationSeconds;document.getElementById('activeEnergy').textContent=Number(d.session.activeEnergyKwh).toFixed(3);document.getElementById('activeCost').textContent=Number(d.session.activeCostEur).toFixed(3);let list=document.getElementById('sessionsList');list.innerHTML='';d.session.history.slice().reverse().forEach(s=>{let div=document.createElement('div');div.className='step active';div.innerHTML=`<div class="dot"></div><div><b>Ladevorgang #${s.id}</b><br>${s.startTime} bis ${s.endTime}</div><div class="desc">${Number(s.energyKwh).toFixed(3)} kWh<br>${Number(s.costEur).toFixed(3)} EUR<br><button class="btnPink" onclick="delSession(${s.id})">Löschen</button></div>`;list.appendChild(div)})}
async function delSession(id){await fetch('/api/session/delete?id='+id);upd()}async function clearSessions(){await fetch('/api/session/clear');upd()}function downloadInvoiceLast3(){if(!latest||!latest.session.history.length){alert('Keine Ladevorgänge vorhanden');return}let arr=latest.session.history.slice(-3),totE=0,totC=0,rows=arr.map(s=>{totE+=Number(s.energyKwh);totC+=Number(s.costEur);return `<tr><td>#${s.id}</td><td>${s.startTime}</td><td>${s.endTime}</td><td>${Number(s.energyKwh).toFixed(3)} kWh</td><td>5.00 EUR/kWh</td><td>${Number(s.costEur).toFixed(2)} EUR</td></tr>`}).join('');let html=`<html><body><h1>chargecloud Glow Challenge Rechnung</h1><table border="1" cellspacing="0" cellpadding="6"><tr><th>ID</th><th>Start</th><th>Ende</th><th>Verbrauch</th><th>Preis</th><th>Betrag</th></tr>${rows}</table><h2>Gesamt: ${totE.toFixed(3)} kWh / ${totC.toFixed(2)} EUR</h2></body></html>`;let a=document.createElement('a');a.href=URL.createObjectURL(new Blob([html],{type:'text/html'}));a.download='glow-challenge-rechnung-letzte-3.html';a.click()}setInterval(upd,500);upd();
</script></body></html>
)rawliteral"; }

void handleStatus(){
  bool vehicle=inVehicle.stable, start=inStart.stable, stop=inStop.stable, err=inError.stable; int adc=analogRead(PIN_POWER_POTI); float kw=adcToPowerKw(adc);
  String j="{"; j += "\"deviceId\":\""+deviceId+"\",\"apSsid\":\""+apSsid+"\",\"apIp\":\""+WiFi.softAPIP().toString()+"\",\"staIp\":\""+(WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():String("0.0.0.0"))+"\",";
  j += "\"state\":\""+String(stateName(state))+"\",\"adcValue\":"+String(adc)+",\"powerKw\":"+String(kw,2)+",\"currentA\":"+String(powerKwToCurrentA(kw),2)+",";
  j += "\"inputs\":{\"vehicleConnected\":"+String(vehicle?"true":"false")+",\"startPressed\":"+String(start?"true":"false")+",\"stopPressed\":"+String(stop?"true":"false")+",\"errorActive\":"+String(err?"true":"false")+"},";
  j += "\"outputs\":{\"available\":"+String(digitalRead(LED_AVAILABLE)?"true":"false")+",\"preparing\":"+String(digitalRead(LED_PREPARING)?"true":"false")+",\"charging\":"+String(digitalRead(LED_CHARGING)?"true":"false")+",\"faulted\":"+String(digitalRead(LED_FAULTED)?"true":"false")+"},";
  unsigned long dur=sessionActive?((millis()-sessionStartMillis)/1000):0;
  float displayEnergy = activeEnergyKwh;
  if (sessionActive) {
    displayEnergy += kw * ((millis() - lastMeterMillis) / 3600000.0f);
  }
  j += "\"session\":{\"active\":"+String(sessionActive?"true":"false")+",\"stateIsCharging\":"+String(state==CHARGING?"true":"false")+",\"startTime\":\""+activeSessionStartTime+"\",\"activeDurationSeconds\":"+String(dur)+",\"activeEnergyKwh\":"+String(displayEnergy,5)+",\"activeCostEur\":"+String(displayEnergy*ENERGY_PRICE_EUR_PER_KWH,3)+",\"history\":[";
  for(int i=0;i<sessionCount;i++){ if(i)j+=","; j+="{\"id\":"+String(sessions[i].id)+",\"startTime\":\""+sessions[i].startTime+"\",\"endTime\":\""+sessions[i].endTime+"\",\"energyKwh\":"+String(sessions[i].energyKwh,5)+",\"costEur\":"+String(sessions[i].costEur,3)+"}"; }
  j += "]}}"; server.send(200,"application/json",j);
}

void setupWeb(){ server.on("/",[](){server.send(200,"text/html",htmlPage());}); server.on("/api/status",handleStatus); server.on("/api/session/delete",[](){ if(server.hasArg("id")) deleteSession(server.arg("id").toInt()); server.send(200,"application/json","{\"ok\":true}");}); server.on("/api/session/clear",[](){clearSessions(); server.send(200,"application/json","{\"ok\":true}");}); server.begin(); }

void setup(){ Serial.begin(115200); delay(300); if(NEOPIXEL_ENABLED){ pixel.begin(); pixel.clear(); pixel.show(); } pinMode(PIN_VEHICLE_CONNECTED,INPUT_PULLUP); pinMode(PIN_START_AUTH,INPUT_PULLUP); pinMode(PIN_STOP,INPUT_PULLUP); pinMode(PIN_ERROR,INPUT_PULLUP); pinMode(PIN_POWER_POTI,INPUT); pinMode(LED_AVAILABLE,OUTPUT); pinMode(LED_PREPARING,OUTPUT); pinMode(LED_CHARGING,OUTPUT); pinMode(LED_FAULTED,OUTPUT); analogReadResolution(12); loadSessions(); setupWiFi(); setupWeb(); updateLeds(); }

void loop(){
  server.handleClient();
  bool vehicle=updateDebounced(inVehicle), start=updateDebounced(inStart), stop=updateDebounced(inStop), err=updateDebounced(inError);
  bool startEvent=start && !lastStartStable; bool stopEvent=stop && !lastStopStable; lastStartStable=start; lastStopStable=stop;
  int adc=analogRead(PIN_POWER_POTI); float kw=adcToPowerKw(adc);
  if(err){ state=ERROR_STATE; }
  else {
    switch(state){
      case IDLE: if(vehicle) state=CONNECTED; break;
      case CONNECTED: if(!vehicle) state=IDLE; else if(startEvent) state=CHARGING; break;
      case CHARGING: if(!vehicle) state=IDLE; else if(stopEvent) state=CONNECTED; break;
      case ERROR_STATE: state=vehicle?CONNECTED:IDLE; break;
    }
  }
  syncSession(kw); updateLeds(); updateNeoPixel(); delay(20);
}
