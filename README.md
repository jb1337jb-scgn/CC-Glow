# ESP32-S3 Local Wallbox Simulator v8

This version runs a complete local charging session simulation in the ESP32 web interface. A real OCPP backend connection is optional.

Features:
- Local web UI for Plugged, Authorisation, Start, Charging, Stop
- Consumption in kWh
- Cost calculation at 0.50 EUR/kWh
- Status/event log similar to backend messages
- Downloadable billing JSON and printable invoice view
- Optional backend URL field kept for later OCPP use

Default WiFi station credentials in `src/main.cpp`:
- SSID: `internet`
- Password: `internet`

The ESP also starts an access point:
- SSID: `Wallbox-LOCAL-XXXXXX`
- Password: `chargecloud`


## v9 GPIO + NeoPixel + Status LEDs

GPIO inputs are active LOW with `INPUT_PULLUP` and are intended for the 3.3V side only. Do not connect 5V to ESP32 GPIOs.

Inputs trigger actions:
- GPIO 4: Ladefreigabe indicator for built-in NeoPixel, no start by itself
- GPIO 5: Plug / Unplug toggle
- GPIO 6: Authorize
- GPIO 7: Start
- GPIO 15: Stop

Status LED outputs:
- GPIO 16: Available, blue
- GPIO 17: Plugged, yellow
- GPIO 18: Authorized, cyan
- GPIO 8: Charging, green
- GPIO 9: Faulted, red

The web interface shows GPIO input states and LED states. Clicking status indicators in the interface does not trigger actions; only the existing control buttons and physical GPIO inputs do.


## v10 Potentiometer for charging power

A potentiometer on GPIO 1 controls simulated charging power from 0 to 22 kW.

Wiring on the ESP32 3.3V side only:
- Potentiometer end 1: 3.3V
- Potentiometer end 2: GND
- Potentiometer wiper: GPIO 1 / ADC

Never connect 5V to the ADC/GPIO pin. The current power is shown in the web interface and used for the kWh calculation while charging.


## v11 Error switch

Adds an active-LOW error switch on GPIO 10 using `INPUT_PULLUP`.

- GPIO 10 LOW: Faulted active, charging is stopped, red status LED on
- GPIO 10 HIGH: fault cleared

Use only 3.3V-side wiring: GPIO 10 to switch to GND. Do not connect 5V.


## v12 MicroOcpp v1.2.0 optional backend

This version adds `matth-x/MicroOcpp@1.2.0` to PlatformIO and keeps the local-first simulator.

Backend URL supports both:
- `ws://...` for lab/VPN/internal tests
- `wss://...` for TLS-secured backends

For `wss://`, make sure time/NTP and certificate trust are correct. Depending on the transport adapter and backend, Basic Auth may need additional adapter-specific configuration.

The local UI, GPIO inputs, status LEDs, NeoPixel, potentiometer up to 22 kW, error switch, and local billing remain available without a backend.


## v13 real MicroOcpp communication mapping

This version maps local simulator events to the MicroOcpp communication layer and logs the synchronization steps:

- Plugged -> OCPP sync Preparing
- Authorize -> OCPP sync Authorize request
- Start -> OCPP sync StartTransaction
- Charging -> OCPP sync MeterValues every 10 seconds
- Stop -> OCPP sync StopTransaction
- Error switch -> OCPP sync Faulted / fault cleared
- Unplug -> OCPP sync Available

The local simulator remains local-first. MicroOcpp v1.2.0 is initialized when backend mode is enabled and `mocpp_loop()` runs continuously. Depending on the exact CSMS and transport adapter, additional adapter-specific transaction API calls may be required for strict backend-controlled operation.


## v13 fixed compile patch

Patched for MicroOcpp v1.2.0 API:
- `mocpp_initialize(...)` now uses the 4-argument URL overload; OCPP 1.6 is the default.
- Removed the invalid `MicroOcpp::setConnectorPluggedInput` namespace usage. In v1.2.0 this helper is exposed as a global API function.
