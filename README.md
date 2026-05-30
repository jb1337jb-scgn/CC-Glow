# ESP32-S3 EVSE Simulator - AP only

Diese Version startet nur den lokalen Access Point, keine STA/Router-Verbindung.

- AP SSID: EVSE-SIM-xxxxxx, geraeteabhaengig
- AP Passwort: chargecloud
- Webinterface: http://192.168.4.1
- Keine Router-Verbindung, keine NTP-Zeit; Start/Endzeit nutzt Uptime-Fallback
- Session Handling bleibt repariert
- NeoPixel bleibt enthalten
