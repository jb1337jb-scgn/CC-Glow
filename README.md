# ESP32-S3 OCPP Wallbox Simulator mit RGB-LED

Diese Version enthaelt:
- OCPP 1.6J ueber WSS
- NTP-Zeit
- GPIO-Eingaenge fuer physisches Bedienpanel
- Ladefreigabe-Ausgang auf GPIO 21
- eingebaute RGB-LED / NeoPixel auf GPIO 48

## RGB-LED Statusfarben

| Status | Farbe |
|---|---|
| OCPP nicht verbunden | Violett |
| Available | Gruen |
| Preparing | Gelb |
| Charging | Blau |
| SuspendedEVSE | Orange |
| Faulted | Rot |

Hinweis: Viele ESP32-S3-WROOM-1 Devboards nutzen GPIO 48 fuer die eingebaute WS2812 RGB-LED. Falls dein Board einen anderen Pin nutzt, `PIN_RGB_LED` im Code anpassen.

## Ladefreigabe-Ausgang

| Funktion | GPIO | Logik |
|---|---:|---|
| Charge Enable | 21 | HIGH nur bei Status Charging |

## GPIO-Eingaenge active LOW

| Funktion | GPIO |
|---|---:|
| Plug | 4 |
| Auth | 5 |
| Start | 6 |
| Stop | 7 |
| Fault | 15 |
| Reset | 18 |
| OCPP verbinden | 8 |
| Poti Ladeleistung | 1 |

## Flashen
```bash
pio run -t upload
pio device monitor
```
