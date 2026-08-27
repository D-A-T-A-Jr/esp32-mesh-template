#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

extern uint32_t meshMyNodeId;

bool meshHasDirectWifi();

// Implementados no seu main.cpp
void appSetup();
void appLoop();
void appCollectTelemetry(JsonObject &out);

// Chame no setup()/loop() do seu main.cpp
void meshNodeSetup();
void meshNodeLoop();
