#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

extern uint32_t meshMyNodeId;

bool meshHasDirectWifi();

// Implementados no seu main.cpp
void appSetup();
void appLoop();
void appCollectTelemetry(JsonObject &out);

// Chamado quando chega um RPC do ThingsBoard (v1/devices/me/rpc/request/+).
// Preenche `response` se quiser responder (fica vazio = nao publica resposta).
void appHandleRpc(const String &method, JsonVariantConst params, JsonDocument &response);

// Chame no setup()/loop() do seu main.cpp
void meshNodeSetup();
void meshNodeLoop();
