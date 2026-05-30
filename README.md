# ESP32-S3 Ladestations-Simulation

Firmware-Projekt fuer eine einfache Ladestations-Simulation mit ESP32-S3-N16R8.

## Funktionen

- Fahrzeug verbunden per Schalter
- Start/Autorisierung per Taster
- Stop per Taster
- Fehler per Schalter/Taster
- Ladeleistung per Poti von 0 bis 22 kW
- Statusanzeige ueber LEDs
- Serielle Ausgabe mit Status, kW und berechnetem 3-phasigem Strom

## Pinbelegung

| Funktion | GPIO |
|---|---:|
| Poti Ladeleistung 0-22 kW | GPIO3 |
| Fahrzeug verbunden | GPIO4 |
| Start / Autorisierung | GPIO5 |
| Stop | GPIO6 |
| Fehler | GPIO7 |
| LED Bereit | GPIO8 |
| LED Verbunden | GPIO9 |
| LED Laedt | GPIO10 |
| LED Fehler | GPIO11 |

GPIO1 und GPIO2 werden nicht verwendet.

## Verdrahtung

### Poti

10 kOhm empfohlen.

```text
3V3   ---- Poti aussen
GPIO3 ---- Poti Mitte / Schleifer
GND   ---- Poti aussen
```

### Schalter/Taster

Die Firmware nutzt interne Pullups. Daher jeweils nach GND schalten:

```text
GPIO4 ---- Schalter ---- GND
GPIO5 ---- Taster   ---- GND
GPIO6 ---- Taster   ---- GND
GPIO7 ---- Schalter ---- GND
```

### LEDs

```text
GPIO8  ---- 330 Ohm ---- LED ---- GND
GPIO9  ---- 330 Ohm ---- LED ---- GND
GPIO10 ---- 330 Ohm ---- LED ---- GND
GPIO11 ---- 330 Ohm ---- LED ---- GND
```

## Build mit PlatformIO

1. ZIP entpacken
2. Ordner in VS Code mit PlatformIO oeffnen
3. Build ausfuehren
4. Upload auf ESP32-S3
5. Seriellen Monitor mit 115200 Baud starten

Alternativ per CLI:

```bash
pio run
pio run --target upload
pio device monitor -b 115200
```

## Hinweis

Wenn dein konkretes ESP32-S3-Board nicht `esp32-s3-devkitc-1` ist, in `platformio.ini` ggf. den `board`-Eintrag anpassen.
