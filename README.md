# ESP32-S3 Glow Challenge EVSE Simulator - Combined

Enthaelt:

- AP startet robust vor STA, fester Kanal 1, WiFi Sleep aus
- AP+STA Modus, geraeteabhaengige AP-SSID
- Pink/Blau Webinterface
- OCPP Status-Ausgaenge: GPIO8 Available, GPIO9 Preparing, GPIO10 Charging, GPIO11 Faulted
- NeoPixel blinkt blau bei CHARGING
- Robuste Session-Aufzeichnung
- Meterintervall 10 Sekunden
- Finale Restberechnung bei Stop oder Fahrzeugtrennung
- Preis: 5,00 EUR/kWh
- Live-Grafik Ladeleistung ueber Zeit waehrend CHARGING
- Grafik-Reset sobald CHARGING inaktiv ist

## Bedienung

1. GPIO4 Fahrzeug verbunden aktivieren
2. GPIO5 Start/Auth druecken
3. Poti GPIO3 steuert Leistung 0-22 kW
4. GPIO6 Stop druecken oder GPIO4 Fahrzeug verbunden deaktivieren
5. Ladevorgang wird sauber beendet und aufgezeichnet
