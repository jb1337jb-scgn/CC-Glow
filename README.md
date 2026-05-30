# ESP32-S3 Glow Challenge EVSE Simulator - Persistente Ladevorgaenge

Neu:

- Taster/Eingaenge entprellt mit 50 ms
- Abgeschlossene Ladevorgaenge bleiben nach Reboot erhalten
- Ladevorgaenge werden hochlaufend nummeriert
- Einzelne Ladevorgaenge koennen im Webinterface geloescht werden
- Alle abgeschlossenen Ladevorgaenge koennen geloescht werden
- Debug-Terminal bleibt enthalten

Speicherung erfolgt im ESP32 NVS via Preferences.
