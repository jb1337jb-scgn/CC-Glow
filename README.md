# ESP32-S3 EVSE Simulator - repaired session handling

Diese Version macht die letzte Bild-Aenderung rueckgaengig und repariert/verbessert das Session Handling:

- Keine eingebettete grosse Grafik mehr im Webinterface
- NeoPixel bleibt enthalten
- Session startet automatisch bei CHARGING
- Session endet automatisch sobald CHARGING verlassen wird
- Aktive Session zeigt Verbrauch/Kosten live als Vorschau, auch innerhalb des 10s Meterintervalls
- Finale Abrechnung nutzt Restzeit bei Stop, Fehler oder Fahrzeugtrennung
- Persistente Ladevorgaenge, Loeschen und Rechnung letzte 3 bleiben enthalten
