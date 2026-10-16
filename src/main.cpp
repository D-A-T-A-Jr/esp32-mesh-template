#include "mesh_node_core.h"

// MICS-5524: Ao -> GPIO34 (ADC1, so entrada), En -> GPIO22 (liga o aquecedor).
// GPIO34 e ADC1, entao continua funcionando com WiFi ligado (ADC2 nao funcionaria).
static const uint8_t GAS_AO_PIN = 34;
static const uint8_t GAS_EN_PIN = 22;

static const uint32_t GAS_SAMPLE_INTERVAL_MS = 50;
static const uint32_t GAS_WARMUP_MS = 3UL * 60UL * 1000UL;

static uint32_t gasSampleSum = 0;
static uint16_t gasSampleCount = 0;
static uint32_t lastGasSampleMs = 0;
static uint32_t gasBootMs = 0;
static uint32_t lastWarmupLogMs = 0;
static const uint32_t GAS_WARMUP_LOG_MS = 30000;

void appSetup()
{
  pinMode(GAS_EN_PIN, OUTPUT);
  digitalWrite(GAS_EN_PIN, HIGH);
  pinMode(GAS_AO_PIN, INPUT);
  gasBootMs = millis();
}

void appLoop()
{
  uint32_t now = millis();

  uint32_t elapsed = now - gasBootMs;
  if (elapsed < GAS_WARMUP_MS && now - lastWarmupLogMs >= GAS_WARMUP_LOG_MS)
  {
    lastWarmupLogMs = now;
    Serial.printf("[GAS] Aquecendo: faltam %lu s para comecar a enviar dados\n",
                  (unsigned long)((GAS_WARMUP_MS - elapsed) / 1000));
  }

  if (now - lastGasSampleMs < GAS_SAMPLE_INTERVAL_MS)
    return;
  lastGasSampleMs = now;
  gasSampleSum += analogRead(GAS_AO_PIN);
  gasSampleCount++;
}

void appCollectTelemetry(JsonObject &out)
{
  if (millis() - gasBootMs < GAS_WARMUP_MS || gasSampleCount == 0)
    return;
  out["gas_raw"] = gasSampleSum / gasSampleCount;
  gasSampleSum = 0;
  gasSampleCount = 0;
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
