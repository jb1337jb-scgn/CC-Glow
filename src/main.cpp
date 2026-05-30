#include <Arduino.h>

enum ChargerState {
  IDLE,
  CONNECTED,
  AUTHORIZED,
  CHARGING,
  ERROR_STATE
};

// ESP32-S3 Ladestations-Simulation
// Verwendet GPIO3 bis GPIO11. GPIO1 und GPIO2 bleiben frei.

// Eingaenge
const int PIN_POWER_POTI        = 3;  // ADC: Poti fuer 0..22 kW
const int PIN_VEHICLE_CONNECTED = 4;  // Schalter nach GND
const int PIN_START_AUTH        = 5;  // Taster nach GND
const int PIN_STOP              = 6;  // Taster nach GND
const int PIN_ERROR             = 7;  // Schalter/Taster nach GND

// Ausgaenge
const int LED_IDLE      = 8;
const int LED_CONNECTED = 9;
const int LED_CHARGING  = 10;
const int LED_ERROR     = 11;

ChargerState state = IDLE;
unsigned long lastPowerPrint = 0;

bool readActiveLow(int pin) {
  return digitalRead(pin) == LOW;
}

void setAllLedsOff() {
  digitalWrite(LED_IDLE, LOW);
  digitalWrite(LED_CONNECTED, LOW);
  digitalWrite(LED_CHARGING, LOW);
  digitalWrite(LED_ERROR, LOW);
}

float readChargingPowerKw() {
  int adcValue = analogRead(PIN_POWER_POTI);
  return (adcValue / 4095.0f) * 22.0f;
}

float powerKwToCurrentA(float powerKw) {
  // 3-phasig, 400 V, cos phi ca. 1
  float powerW = powerKw * 1000.0f;
  return powerW / (1.732f * 400.0f);
}

const char* stateName(ChargerState s) {
  switch (s) {
    case IDLE: return "IDLE";
    case CONNECTED: return "CONNECTED";
    case AUTHORIZED: return "AUTHORIZED";
    case CHARGING: return "CHARGING";
    case ERROR_STATE: return "ERROR";
    default: return "UNKNOWN";
  }
}

void updateLeds() {
  setAllLedsOff();

  switch (state) {
    case IDLE:
      digitalWrite(LED_IDLE, HIGH);
      break;
    case CONNECTED:
      digitalWrite(LED_CONNECTED, HIGH);
      break;
    case AUTHORIZED:
      digitalWrite(LED_CONNECTED, HIGH);
      digitalWrite(LED_IDLE, HIGH);
      break;
    case CHARGING:
      digitalWrite(LED_CHARGING, HIGH);
      break;
    case ERROR_STATE:
      digitalWrite(LED_ERROR, HIGH);
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_VEHICLE_CONNECTED, INPUT_PULLUP);
  pinMode(PIN_START_AUTH, INPUT_PULLUP);
  pinMode(PIN_STOP, INPUT_PULLUP);
  pinMode(PIN_ERROR, INPUT_PULLUP);
  pinMode(PIN_POWER_POTI, INPUT);

  pinMode(LED_IDLE, OUTPUT);
  pinMode(LED_CONNECTED, OUTPUT);
  pinMode(LED_CHARGING, OUTPUT);
  pinMode(LED_ERROR, OUTPUT);

  analogReadResolution(12);

  updateLeds();

  Serial.println();
  Serial.println("Ladestations-Simulation gestartet");
  Serial.println("Board: ESP32-S3");
  Serial.println("Pins: GPIO3 bis GPIO11");
  Serial.println("Poti: GPIO3 = 0..22 kW");
}

void loop() {
  bool vehicleConnected = readActiveLow(PIN_VEHICLE_CONNECTED);
  bool startPressed     = readActiveLow(PIN_START_AUTH);
  bool stopPressed      = readActiveLow(PIN_STOP);
  bool errorActive      = readActiveLow(PIN_ERROR);

  float powerKw = readChargingPowerKw();
  float currentA = powerKwToCurrentA(powerKw);

  if (errorActive) {
    state = ERROR_STATE;
  } else {
    switch (state) {
      case IDLE:
        if (vehicleConnected) {
          state = CONNECTED;
          Serial.println("Fahrzeug verbunden");
        }
        break;

      case CONNECTED:
        if (!vehicleConnected) {
          state = IDLE;
          Serial.println("Fahrzeug getrennt");
        } else if (startPressed) {
          state = AUTHORIZED;
          Serial.println("Autorisiert");
          delay(300);
          state = CHARGING;
          Serial.println("Ladevorgang gestartet");
        }
        break;

      case AUTHORIZED:
        state = CHARGING;
        break;

      case CHARGING:
        if (!vehicleConnected) {
          state = IDLE;
          Serial.println("Fahrzeug getrennt");
        } else if (stopPressed) {
          state = CONNECTED;
          Serial.println("Ladevorgang gestoppt");
        }
        break;

      case ERROR_STATE:
        state = IDLE;
        break;
    }
  }

  updateLeds();

  if (millis() - lastPowerPrint >= 1000) {
    lastPowerPrint = millis();

    Serial.print("Status: ");
    Serial.print(stateName(state));
    Serial.print(" | Ladeleistung: ");
    Serial.print(powerKw, 1);
    Serial.print(" kW | Strom 3-phasig: ");
    Serial.print(currentA, 1);
    Serial.println(" A");
  }

  delay(50);
}
