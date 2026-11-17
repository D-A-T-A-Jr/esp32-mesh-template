#include "mesh_node_core.h"

// Exemplo padrao: pisca um LED externo, sem delay() (delay trava a malha).
// Troque pelo codigo dos sensores dessa placa.
//
// Nao use aqui o LED onboard: ele e do nucleo (aceso fixo = WiFi direto,
// piscando = sem WiFi) e seria sobrescrito a cada ciclo do loop.
// GPIO4 existe e e livre no ESP32, ESP32-S3, ESP32-C3 e ESP8266 (D2 no D1 mini).
static const int BLINK_PIN = 4;
static const unsigned long BLINK_INTERVAL_MS = 1000;

static bool blinkState = false;
static unsigned long lastBlinkAt = 0;

void appSetup()
{
  pinMode(BLINK_PIN, OUTPUT);
  digitalWrite(BLINK_PIN, LOW);
}

void appLoop()
{
  if (millis() - lastBlinkAt < BLINK_INTERVAL_MS)
    return;

  lastBlinkAt = millis();
  blinkState = !blinkState;
  digitalWrite(BLINK_PIN, blinkState ? HIGH : LOW);
}

void appCollectTelemetry(JsonObject &out)
{
  out["blink_state"] = blinkState;
}

void appHandleRpc(const String &method, JsonVariantConst params, JsonDocument &response)
{
}

void setup()
{
  meshNodeSetup();
}

void loop()
{
  meshNodeLoop();
}
