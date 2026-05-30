# ESP32-S3 Glow Challenge EVSE Simulator - Session Sync Fix

Diese Version koppelt die Session zentral an den Status CHARGING.

- Wenn Status CHARGING ist und keine Session aktiv ist, startet automatisch eine Session.
- Wenn Status nicht mehr CHARGING ist und eine Session aktiv ist, wird sie automatisch beendet.
- Dadurch bleibt OCPP Status Charging/Preparing synchron zur Session-Anzeige.
- Rechnungsdownload letzte 3 bleibt enthalten.
- Persistente, nummerierte Ladevorgaenge und Loeschen bleiben enthalten.
