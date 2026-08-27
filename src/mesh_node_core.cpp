#include "mesh_node_core.h"

#include <painlessMesh.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "generated_secrets.h"

static const int LED_PIN = 2;

static const char *MESH_PREFIX = "EstufaMesh";
static const char *MESH_PASSWORD = "estufa12345";
static const uint16_t MESH_PORT = 5555;

// Canal do roteador WiFi. AP+STA do ESP32 compartilham o mesmo canal — ajusta se o
// roteador mudar (ver AGENTS.md pra como descobrir o canal).
static const int32_t ROUTER_CHANNEL = 11;

static const unsigned long NODE_BROADCAST_INTERVAL_MS = 5000;
static const unsigned long PUBLISH_CYCLE_MS = 30000;
static const unsigned long BURST_CONNECT_TIMEOUT_MS = 10000;

Scheduler userScheduler;
painlessMesh mesh;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

uint32_t meshMyNodeId = 0;
const char *myToken = nullptr;
const char *myDeviceName = nullptr;

unsigned long lastHelloAt = 0;
unsigned long lastPublishCycleAt = 0;
bool ledState = false;
unsigned long lastBlink = 0;

const char *findDeviceName(uint32_t nodeId)
{
  for (int i = 0; i < NODE_TABLE_COUNT; i++)
  {
    if (NODE_TABLE[i].nodeId == nodeId)
      return NODE_TABLE[i].name;
  }
  return nullptr;
}

bool meshHasDirectWifi()
{
  return WiFi.status() == WL_CONNECTED && WiFi.SSID() == String(WIFI_SSID);
}

struct PendingMeshMsg
{
  uint32_t fromNodeId;
  unsigned long uptimeMs;
};

static const int MAX_PENDING = 10;
PendingMeshMsg pendingMsgs[MAX_PENDING];
int pendingCount = 0;

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

void updateStatusLed()
{
  if (meshHasDirectWifi())
  {
    digitalWrite(LED_PIN, HIGH);
    return;
  }

  if (millis() - lastBlink > 500)
  {
    lastBlink = millis();
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  }
}

void forwardToThingsBoard(uint32_t fromNodeId, unsigned long nodeUptimeMs)
{
  const char *deviceName = findDeviceName(fromNodeId);

  if (deviceName == nullptr)
  {
    Serial.print("[TB] No ");
    Serial.print(fromNodeId);
    Serial.println(" nao esta na NODE_TABLE, ignorado");
    return;
  }

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

  Serial.print("[TB] Encaminhado (gateway) do no ");
  Serial.print(fromNodeId);
  Serial.print(" (");
  Serial.print(deviceName);
  Serial.print("): ");
  Serial.println(json);
}

void runPublishBurst()
{
  if (myToken == nullptr)
  {
    Serial.println("[BURST] Sem token proprio (NODE_TABLE), nao publica.");
    return;
  }

  unsigned long burstStart = millis();

  WiFi.mode(WIFI_STA);

  unsigned long deadline = millis() + BURST_CONNECT_TIMEOUT_MS;
  bool connected = false;

  while (millis() < deadline)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      String clientId = "esp32-mesh-" + String(meshMyNodeId, HEX);
      connected = mqtt.connect(clientId.c_str(), myToken, "");

      if (!connected)
      {
        Serial.print("[BURST] MQTT falhou. State: ");
        Serial.println(mqtt.state());
      }

      break;
    }

    delay(200);
  }

  if (connected)
  {
    for (int i = 0; i < pendingCount; i++)
    {
      forwardToThingsBoard(pendingMsgs[i].fromNodeId, pendingMsgs[i].uptimeMs);
    }
    pendingCount = 0;

    JsonDocument doc;
    doc["node_id"] = meshMyNodeId;
    doc["uptime_ms"] = millis();
    doc["has_wifi"] = true;

    JsonObject obj = doc.as<JsonObject>();
    appCollectTelemetry(obj);

    String json;
    serializeJson(doc, json);
    mqtt.publish("v1/devices/me/telemetry", json.c_str());

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
  DeserializationError err = deserializeJson(doc, msg);
  if (err)
    return;

  const char *type = doc["type"] | "";

  if (strcmp(type, "hello") == 0)
  {
    bool senderHasWifi = doc["has_wifi"] | false;

    if (!senderHasWifi && meshHasDirectWifi())
    {
      queuePendingMsg(from, doc["uptime_ms"] | 0);
    }
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

void meshNodeSetup()
{
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(115200);
  delay(500);

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT, WIFI_AP_STA, ROUTER_CHANNEL);

  mesh.onReceive(&onMeshReceive);
  mesh.onNewConnection(&onNewMeshConnection);
  mesh.onChangedConnections(&onMeshTopologyChanged);

  mesh.stationManual(WIFI_SSID, WIFI_PASS);

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
  {
    Serial.println("[SYS] Node ID nao esta na NODE_TABLE (.env) — relay funciona, publish proprio nao.");
  }

  mqtt.setServer(TB_HOST, TB_PORT);
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(5);

  appSetup();
}

void meshNodeLoop()
{
  mesh.update();
  updateStatusLed();
  appLoop();

  if (millis() - lastHelloAt > NODE_BROADCAST_INTERVAL_MS)
  {
    lastHelloAt = millis();
    sendHello();
  }

  if (meshHasDirectWifi() && millis() - lastPublishCycleAt > PUBLISH_CYCLE_MS)
  {
    lastPublishCycleAt = millis();
    runPublishBurst();
  }
}
