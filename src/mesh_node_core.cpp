#include "mesh_node_core.h"

#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#if defined(ESP32)
#include <Update.h>
#include "mbedtls/md.h"
#elif defined(ESP8266)
#include <Updater.h>
#include <bearssl/bearssl_hash.h>
#endif

// 0 = placa standalone: WiFiManager + MQTT direto + OTA, sem malha nenhuma.
#define MESH_ENABLED 1

#if MESH_ENABLED
#include <painlessMesh.h>
#endif

#include "generated_secrets.h"

static const int LED_PIN = 2;

#if defined(ESP8266)
static const bool LED_ACTIVE_LOW = true;
#else
static const bool LED_ACTIVE_LOW = false;
#endif

static inline void setLed(bool on)
{
  bool physicalHigh = LED_ACTIVE_LOW ? !on : on;
  digitalWrite(LED_PIN, physicalHigh ? HIGH : LOW);
}

#if MESH_ENABLED
static const char *MESH_PREFIX = "EstufaMesh";
static const char *MESH_PASSWORD = "estufa12345";
static const uint16_t MESH_PORT = 5555;

// Canal WiFi de TODA a malha (AP + STA). Fixo e igual em todo repo da mesma
// malha -- ver AGENTS.md, secao "Canal WiFi".
static const int32_t ROUTER_CHANNEL = 11;

static const unsigned long NODE_BROADCAST_INTERVAL_MS = 5000;
static const unsigned long BURST_CONNECT_TIMEOUT_MS = 10000;
static const int MAX_PENDING = 10;
#endif

// Valor inicial e limites do intervalo entre rajadas de publish. O valor em uso
// (publishCycleMs) pode ser trocado em runtime via atributo compartilhado
// "publish_cycle_ms" no ThingsBoard -- ver AGENTS.md.
static const unsigned long PUBLISH_CYCLE_DEFAULT_MS = 30000;
static const unsigned long PUBLISH_CYCLE_MIN_MS = 5000;
static const unsigned long PUBLISH_CYCLE_MAX_MS = 3600000;
unsigned long publishCycleMs = PUBLISH_CYCLE_DEFAULT_MS;

static const unsigned long ATTR_WAIT_MS = 4000;
static const unsigned long OTA_CHUNK_TIMEOUT_MS = 30000;
static const size_t OTA_CHUNK_SIZE = 4096;

static const char *TOPIC_TELEMETRY = "v1/devices/me/telemetry";
static const char *TOPIC_ATTRIBUTES = "v1/devices/me/attributes";
static const char *TOPIC_ATTR_RESPONSE = "v1/devices/me/attributes/response/+";
static const char *TOPIC_FW_RESPONSE = "v2/fw/response/+/chunk/+";
static const char *TOPIC_RPC_REQUEST = "v1/devices/me/rpc/request/+";

WiFiManager wm;
String provisionedSSID;
String provisionedPass;

#if MESH_ENABLED
Scheduler userScheduler;
painlessMesh mesh;
#endif

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

uint32_t meshMyNodeId = 0;
unsigned long lastPublishCycleAt = 0;
bool ledState = false;
unsigned long lastBlink = 0;

int attrRequestId = 1;
int fwRequestId = 1;
bool otaInProgress = false;
bool sha256Active = false;
String otaVersion;
String otaChecksum;
String otaChecksumAlgorithm;
size_t otaSize = 0;
size_t otaWritten = 0;
int otaChunkIndex = 0;
unsigned long lastChunkAt = 0;

#if defined(ESP32)
mbedtls_md_context_t sha256Ctx;
#elif defined(ESP8266)
br_sha256_context sha256Ctx;
#endif

bool meshHasDirectWifi()
{
  return WiFi.status() == WL_CONNECTED && provisionedSSID.length() > 0 &&
         WiFi.SSID() == provisionedSSID;
}

void updateStatusLed()
{
  if (meshHasDirectWifi())
  {
    setLed(true);
    return;
  }

  if (millis() - lastBlink > 500)
  {
    lastBlink = millis();
    ledState = !ledState;
    setLed(ledState);
  }
}

void publishTelemetry(const String &json)
{
  mqtt.publish(TOPIC_TELEMETRY, json.c_str());
}

void publishOTAState(const String &state, const String &error = "")
{
  JsonDocument doc;
  doc["fw_state"] = state;

  if (state == "UPDATED")
  {
    doc["current_fw_title"] = FW_TITLE;
    doc["current_fw_version"] = FW_VERSION;
  }

  if (otaSize > 0)
  {
    doc["fw_size"] = otaSize;
    doc["fw_written"] = otaWritten;
    doc["fw_progress"] = (int)((otaWritten * 100) / otaSize);
  }

  if (error.length())
    doc["fw_error"] = error;

  String json;
  serializeJson(doc, json);
  publishTelemetry(json);
}

bool sha256Start()
{
#if defined(ESP32)
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info)
    return false;

  mbedtls_md_init(&sha256Ctx);
  if (mbedtls_md_setup(&sha256Ctx, info, 0) != 0)
    return false;
  if (mbedtls_md_starts(&sha256Ctx) != 0)
    return false;
#elif defined(ESP8266)
  br_sha256_init(&sha256Ctx);
#endif

  sha256Active = true;
  return true;
}

void sha256Update(const uint8_t *data, size_t len)
{
  if (!sha256Active)
    return;

#if defined(ESP32)
  mbedtls_md_update(&sha256Ctx, data, len);
#elif defined(ESP8266)
  br_sha256_update(&sha256Ctx, data, len);
#endif
}

bool sha256FinishAndVerify()
{
  if (!sha256Active)
    return true;

  unsigned char hash[32];

#if defined(ESP32)
  if (mbedtls_md_finish(&sha256Ctx, hash) != 0)
  {
    mbedtls_md_free(&sha256Ctx);
    sha256Active = false;
    return false;
  }
  mbedtls_md_free(&sha256Ctx);
#elif defined(ESP8266)
  br_sha256_out(&sha256Ctx, hash);
#endif

  sha256Active = false;

  String actual;
  for (int i = 0; i < 32; i++)
  {
    if (hash[i] < 16)
      actual += "0";
    actual += String(hash[i], HEX);
  }
  actual.toLowerCase();

  String expected = otaChecksum;
  expected.toLowerCase();
  expected.replace(" ", "");

  return actual == expected;
}

void sha256Abort()
{
  if (sha256Active)
  {
#if defined(ESP32)
    mbedtls_md_free(&sha256Ctx);
#endif
    sha256Active = false;
  }
}

void failOTA(const String &reason)
{
#if defined(ESP32)
  Update.abort();
#endif
  // ESP8266 Updater nao tem abort(): o proximo Update.begin() ja reseta o
  // estado interno sozinho quando uma OTA anterior ficou pela metade.
  sha256Abort();
  publishOTAState("FAILED", reason);
  otaInProgress = false;
  otaWritten = 0;
  otaSize = 0;
  otaChunkIndex = 0;
}

void requestNextChunk()
{
  if (!otaInProgress || !mqtt.connected())
    return;

  String topic = "v2/fw/request/" + String(fwRequestId) + "/chunk/" + String(otaChunkIndex);
  mqtt.publish(topic.c_str(), String(OTA_CHUNK_SIZE).c_str());
  lastChunkAt = millis();
}

void finishOTA()
{
  if (otaSize > 0 && otaWritten != otaSize)
  {
    failOTA("Tamanho recebido diferente do esperado");
    return;
  }

  publishOTAState("DOWNLOADED");

  String alg = otaChecksumAlgorithm;
  alg.toUpperCase();

  if ((alg == "SHA256" || alg == "SHA-256") && otaChecksum.length())
  {
    if (!sha256FinishAndVerify())
    {
      failOTA("Checksum SHA-256 invalido");
      return;
    }
  }

  publishOTAState("VERIFIED");

  if (!Update.end(true) || !Update.isFinished())
  {
    failOTA("Update.end falhou");
    return;
  }

  publishOTAState("UPDATING");
  delay(1000);
  ESP.restart();
}

void handleFirmwareChunk(const uint8_t *payload, unsigned int length)
{
  if (!otaInProgress)
    return;

  lastChunkAt = millis();

  if (length == 0)
  {
    finishOTA();
    return;
  }

  size_t written = Update.write((uint8_t *)payload, length);
  if (written != length)
  {
    failOTA("Update.write incompleto");
    return;
  }

  sha256Update(payload, length);
  otaWritten += written;

  if (otaChunkIndex % 5 == 0)
    publishOTAState("DOWNLOADING");

  if (otaSize > 0 && otaWritten >= otaSize)
  {
    finishOTA();
    return;
  }

  otaChunkIndex++;
  requestNextChunk();
}

void startOTA()
{
  if (otaInProgress || otaSize == 0)
    return;

  if (otaSize > ESP.getFreeSketchSpace())
  {
    failOTA("Firmware maior que o espaco OTA livre");
    return;
  }

  String alg = otaChecksumAlgorithm;
  alg.toUpperCase();

  if (alg == "SHA256" || alg == "SHA-256")
  {
    if (!sha256Start())
    {
      failOTA("Nao conseguiu iniciar SHA-256");
      return;
    }
  }

  if (!Update.begin(otaSize))
  {
    failOTA("Update.begin falhou");
    return;
  }

  otaInProgress = true;
  otaWritten = 0;
  otaChunkIndex = 0;
  fwRequestId++;

  publishOTAState("DOWNLOADING");
  requestNextChunk();
}

void parseOTAAttributes(JsonObject obj)
{
  String newTitle = obj["fw_title"] | "";
  String newVersion = obj["fw_version"] | "";

  if (!newTitle.length() || !newVersion.length() || newTitle != FW_TITLE || newVersion == FW_VERSION)
    return;

  otaVersion = newVersion;
  otaSize = obj["fw_size"] | 0;
  otaChecksum = String((const char *)(obj["fw_checksum"] | ""));
  otaChecksumAlgorithm = String((const char *)(obj["fw_checksum_algorithm"] | ""));

  startOTA();
}

void parsePublishCycleAttribute(JsonObject obj)
{
  if (obj["publish_cycle_ms"].isNull())
    return;

  unsigned long requested = obj["publish_cycle_ms"] | publishCycleMs;
  if (requested >= PUBLISH_CYCLE_MIN_MS && requested <= PUBLISH_CYCLE_MAX_MS)
    publishCycleMs = requested;
}

void parseSharedAttributes(JsonObject obj)
{
  parseOTAAttributes(obj);
  parsePublishCycleAttribute(obj);
}

void requestSharedAttributes()
{
  if (!mqtt.connected())
    return;

  String topic = "v1/devices/me/attributes/request/" + String(attrRequestId++);
  mqtt.publish(topic.c_str(), "{\"sharedKeys\":\"fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm,publish_cycle_ms\"}");
}

void handleRpcRequest(const String &topicStr, uint8_t *payload, unsigned int length)
{
  String requestId = topicStr.substring(topicStr.lastIndexOf('/') + 1);

  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++)
    msg += (char)payload[i];

  JsonDocument doc;
  if (deserializeJson(doc, msg))
    return;

  String method = doc["method"] | "";
  if (!method.length())
    return;

  JsonDocument response;
  appHandleRpc(method, doc["params"], response);

  if (!response.isNull())
  {
    String responseJson;
    serializeJson(response, responseJson);
    String responseTopic = "v1/devices/me/rpc/response/" + requestId;
    mqtt.publish(responseTopic.c_str(), responseJson.c_str());
  }
}

void mqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
  String topicStr = String(topic);

  if (topicStr.startsWith("v2/fw/response/"))
  {
    handleFirmwareChunk(payload, length);
    return;
  }

  if (topicStr.startsWith("v1/devices/me/rpc/request/"))
  {
    handleRpcRequest(topicStr, payload, length);
    return;
  }

  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++)
    msg += (char)payload[i];

  JsonDocument doc;
  if (deserializeJson(doc, msg))
    return;

  if (doc["shared"].is<JsonObject>())
    parseSharedAttributes(doc["shared"].as<JsonObject>());
  else if (doc.is<JsonObject>())
    parseSharedAttributes(doc.as<JsonObject>());
}

#if MESH_ENABLED

const char *myToken = nullptr;
const char *myDeviceName = nullptr;
unsigned long lastHelloAt = 0;

struct PendingMeshMsg
{
  uint32_t fromNodeId;
  unsigned long uptimeMs;
};

PendingMeshMsg pendingMsgs[MAX_PENDING];
int pendingCount = 0;

const char *findDeviceName(uint32_t nodeId)
{
  for (int i = 0; i < NODE_TABLE_COUNT; i++)
  {
    if (NODE_TABLE[i].nodeId == nodeId)
      return NODE_TABLE[i].name;
  }
  return nullptr;
}

void queuePendingMsg(uint32_t fromNodeId, unsigned long uptimeMs)
{
  if (pendingCount >= MAX_PENDING)
  {
    memmove(pendingMsgs, pendingMsgs + 1, (MAX_PENDING - 1) * sizeof(PendingMeshMsg));
    pendingCount--;
  }

  pendingMsgs[pendingCount].fromNodeId = fromNodeId;
  pendingMsgs[pendingCount].uptimeMs = uptimeMs;
  pendingCount++;
}

void forwardToThingsBoard(uint32_t fromNodeId, unsigned long nodeUptimeMs)
{
  const char *deviceName = findDeviceName(fromNodeId);
  if (deviceName == nullptr)
    return;

  JsonDocument connectDoc;
  connectDoc["device"] = deviceName;
  String connectJson;
  serializeJson(connectDoc, connectJson);
  mqtt.publish("v1/gateway/connect", connectJson.c_str());

  JsonDocument doc;
  JsonArray arr = doc[deviceName].to<JsonArray>();
  JsonObject entry = arr.add<JsonObject>();
  JsonObject values = entry["values"].to<JsonObject>();
  values["node_uptime_ms"] = nodeUptimeMs;
  values["via_relay_node_id"] = meshMyNodeId;

  String json;
  serializeJson(doc, json);
  mqtt.publish("v1/gateway/telemetry", json.c_str());
}

void runPublishBurst()
{
  if (myToken == nullptr)
    return;

  WiFi.mode(WIFI_STA);

  unsigned long deadline = millis() + BURST_CONNECT_TIMEOUT_MS;
  bool connected = false;

  while (millis() < deadline)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      String clientId = "esp32-mesh-" + String(meshMyNodeId, HEX);
      connected = mqtt.connect(clientId.c_str(), myToken, "");
      break;
    }
    delay(200);
  }

#ifdef MESH_DEBUG
  Serial.print("[DEBUG] burst wifi_connected=");
  Serial.print(WiFi.status() == WL_CONNECTED);
  Serial.print(" mqtt_connected=");
  Serial.println(connected);
#endif

  if (connected)
  {
    mqtt.subscribe(TOPIC_ATTRIBUTES);
    mqtt.subscribe(TOPIC_ATTR_RESPONSE);
    mqtt.subscribe(TOPIC_FW_RESPONSE);
    mqtt.subscribe(TOPIC_RPC_REQUEST);

    for (int i = 0; i < pendingCount; i++)
      forwardToThingsBoard(pendingMsgs[i].fromNodeId, pendingMsgs[i].uptimeMs);
    pendingCount = 0;

    JsonDocument doc;
    doc["node_id"] = meshMyNodeId;
    doc["uptime_ms"] = millis();
    doc["has_wifi"] = true;
    doc["fw_title"] = FW_TITLE;
    doc["fw_version"] = FW_VERSION;

    JsonObject obj = doc.as<JsonObject>();
    appCollectTelemetry(obj);

    String json;
    serializeJson(doc, json);
    publishTelemetry(json);

    requestSharedAttributes();

    unsigned long attrDeadline = millis() + ATTR_WAIT_MS;
    while (millis() < attrDeadline && !otaInProgress)
    {
      mqtt.loop();
      delay(50);
    }

    if (otaInProgress)
    {
      while (otaInProgress && millis() - lastChunkAt < OTA_CHUNK_TIMEOUT_MS)
      {
        mqtt.loop();
      }

      if (otaInProgress)
        failOTA("Timeout esperando chunk OTA");
    }

    mqtt.disconnect();
  }

  WiFi.mode(WIFI_AP_STA);
}

void sendHello()
{
  JsonDocument doc;
  doc["type"] = "hello";
  doc["node_id"] = meshMyNodeId;
  doc["uptime_ms"] = millis();
  doc["has_wifi"] = meshHasDirectWifi();

  String json;
  serializeJson(doc, json);
  mesh.sendBroadcast(json);
}

void onMeshReceive(uint32_t from, String &msg)
{
  JsonDocument doc;
  if (deserializeJson(doc, msg))
    return;

  const char *type = doc["type"] | "";

  if (strcmp(type, "hello") == 0)
  {
    bool senderHasWifi = doc["has_wifi"] | false;
    if (!senderHasWifi && meshHasDirectWifi())
      queuePendingMsg(from, doc["uptime_ms"] | 0);
  }
}

void onNewMeshConnection(uint32_t nodeId)
{
  Serial.print("[MESH] Nova conexao: ");
  Serial.println(nodeId);
}

void onMeshTopologyChanged()
{
  Serial.print("[MESH] Nos conectados: ");
  Serial.println(mesh.getNodeList().size());
}

#ifdef MESH_DEBUG
unsigned long lastDebugPrintAt = 0;
static const unsigned long DEBUG_PRINT_INTERVAL_MS = 5000;

void printDebugStatus()
{
  if (millis() - lastDebugPrintAt < DEBUG_PRINT_INTERVAL_MS)
    return;
  lastDebugPrintAt = millis();

  Serial.print("[DEBUG] wifi_status=");
  Serial.print(WiFi.status());
  Serial.print(" has_direct_wifi=");
  Serial.print(meshHasDirectWifi());
  Serial.print(" ssid=");
  Serial.print(WiFi.SSID());
  Serial.print(" channel=");
  Serial.print(WiFi.channel());
  Serial.print(" rssi=");
  Serial.print(WiFi.RSSI());
  Serial.print(" ip=");
  Serial.print(WiFi.localIP());
  Serial.print(" mesh_nodes=");
  Serial.println(mesh.getNodeList().size());
}
#endif

void warnIfRouterChannelMismatch()
{
  // So avisa -- nao usa o resultado pra decidir o canal da malha. O canal
  // tem que ser IGUAL em todo repo da mesma malha (ver AGENTS.md).
  if (provisionedSSID.length() == 0)
    return;

  WiFi.mode(WIFI_STA);
  int found = WiFi.scanNetworks();

  for (int i = 0; i < found; i++)
  {
    if (WiFi.SSID(i) == provisionedSSID)
    {
      int32_t realChannel = WiFi.channel(i);
      if (realChannel != ROUTER_CHANNEL)
      {
        Serial.print("[SYS] Aviso: roteador esta no canal ");
        Serial.print(realChannel);
        Serial.print(", mas ROUTER_CHANNEL da malha esta em ");
        Serial.print(ROUTER_CHANNEL);
        Serial.println(" -- WiFi direto nunca vai conectar assim.");
      }
      WiFi.scanDelete();
      return;
    }
  }

  Serial.println("[SYS] Roteador nao visivel no scan de boot.");
  WiFi.scanDelete();
}

#endif // MESH_ENABLED

void ensureMqttConfigured()
{
  mqtt.setServer(TB_HOST, TB_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(5);
  mqtt.setBufferSize(4096);
}

#if !MESH_ENABLED
unsigned long lastStandaloneReconnectAt = 0;

void standaloneEnsureMqttConnected()
{
  if (mqtt.connected() || millis() - lastStandaloneReconnectAt < 3000)
    return;
  lastStandaloneReconnectAt = millis();

  String clientId = String(FW_TITLE) + "-" + WiFi.macAddress();
  if (mqtt.connect(clientId.c_str(), TB_TOKEN, ""))
  {
    mqtt.subscribe(TOPIC_ATTRIBUTES);
    mqtt.subscribe(TOPIC_ATTR_RESPONSE);
    mqtt.subscribe(TOPIC_FW_RESPONSE);
    mqtt.subscribe(TOPIC_RPC_REQUEST);
    requestSharedAttributes();
  }
}

void standaloneLoop()
{
  standaloneEnsureMqttConnected();
  mqtt.loop();
  updateStatusLed();
  appLoop();

  if (millis() - lastPublishCycleAt > publishCycleMs)
  {
    lastPublishCycleAt = millis();

    JsonDocument doc;
    doc["fw_title"] = FW_TITLE;
    doc["fw_version"] = FW_VERSION;

    JsonObject obj = doc.as<JsonObject>();
    appCollectTelemetry(obj);

    String json;
    serializeJson(doc, json);
    publishTelemetry(json);
  }
}
#endif

void meshNodeSetup()
{
  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  Serial.begin(115200);
  delay(500);

  String apName = "ESP-" + WiFi.macAddress();
  apName.replace(":", "");

#if MESH_ENABLED
  // So sobe portal se nao conectar com credencial ja salva -- o board sem
  // WiFi ainda participa da malha normalmente (relay), nao fica bloqueado
  // esperando alguem configurar.
  wm.setConfigPortalTimeout(180);
  wm.autoConnect(apName.c_str());
#else
  // Sem malha nao ha fallback: fica tentando ate alguem configurar.
  wm.autoConnect(apName.c_str());
#endif

  provisionedSSID = WiFi.SSID();
  provisionedPass = WiFi.psk();

  ensureMqttConfigured();

#if MESH_ENABLED
  warnIfRouterChannelMismatch();

#ifdef MESH_DEBUG
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
#else
  mesh.setDebugMsgTypes(ERROR | STARTUP);
#endif
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT, WIFI_AP_STA, ROUTER_CHANNEL);

  mesh.onReceive(&onMeshReceive);
  mesh.onNewConnection(&onNewMeshConnection);
  mesh.onChangedConnections(&onMeshTopologyChanged);

  if (provisionedSSID.length() > 0)
    mesh.stationManual(provisionedSSID.c_str(), provisionedPass.c_str());

  meshMyNodeId = mesh.getNodeId();
  Serial.print("[MESH] Node ID: ");
  Serial.println(meshMyNodeId);

  for (int i = 0; i < NODE_TABLE_COUNT; i++)
  {
    if (NODE_TABLE[i].nodeId == meshMyNodeId)
    {
      myToken = NODE_TABLE[i].token;
      myDeviceName = NODE_TABLE[i].name;
      break;
    }
  }

  if (myToken == nullptr)
    Serial.println("[SYS] Node ID nao esta na NODE_TABLE (.env) — relay funciona, publish proprio nao.");
#endif

  appSetup();
}

void meshNodeLoop()
{
#if MESH_ENABLED
  mesh.update();
  updateStatusLed();
  appLoop();

#ifdef MESH_DEBUG
  printDebugStatus();
#endif

  if (millis() - lastHelloAt > NODE_BROADCAST_INTERVAL_MS)
  {
    lastHelloAt = millis();
    sendHello();
  }

  if (meshHasDirectWifi() && millis() - lastPublishCycleAt > publishCycleMs)
  {
    lastPublishCycleAt = millis();
    runPublishBurst();
  }
#else
  standaloneLoop();
#endif
}
