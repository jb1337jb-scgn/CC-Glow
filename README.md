# ESP32-S3 Glow Challenge EVSE Simulator - OCPP Status LEDs

Diese Version nutzt die Ausgaenge/Status-LEDs als echte Ladestationsstatus.

## Ausgaenge

| GPIO | Status |
|---|---|
| GPIO8 | Available |
| GPIO9 | Preparing |
| GPIO10 | Charging |
| GPIO11 | Faulted |

## Eingaenge

| GPIO | Funktion |
|---|---|
| GPIO3 | Poti Ladeleistung 0-22 kW |
| GPIO4 | Fahrzeug verbunden / Plugged |
| GPIO5 | Start / Autorisierung |
| GPIO6 | Stop |
| GPIO7 | Fehler |

## Weitere Funktionen

- AP+STA Modus
- Geraeteabhaengige AP-SSID
- Webinterface im Pink/Blau-Design
- OCPP-Ablaufanzeige
- Session-Aufzeichnung
- Preis: 5,00 EUR/kWh


## NeoPixel

Bei aktivem Ladevorgang (`CHARGING`) blitzt die Onboard-NeoPixel-LED blau.

Standard-Pin: GPIO48. Falls dein Board die RGB-LED auf einem anderen Pin hat, `NEOPIXEL_PIN` in `src/main.cpp` anpassen.
