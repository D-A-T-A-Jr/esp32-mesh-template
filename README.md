# ESP32 Mesh Node

Template pra um nó da rede mesh de sensores (ESP32 + painlessMesh + ThingsBoard). Cada
placa física clona este repo e customiza `src/main.cpp` com seus próprios sensores; a
malha (rede, WiFi, MQTT) já vem pronta e não precisa mexer.

## Uso rápido

```
cp .env.example .env
# preenche WIFI_SSID, WIFI_PASS, TB_HOST no .env
pio run -t upload
pio device monitor
```

Anota o "Node ID" que aparece no boot, cria um device no ThingsBoard, e preenche
`NODE_<id>_TOKEN` / `NODE_<id>_NAME` no `.env` (ver AGENTS.md pros detalhes).

## Customizando os sensores

Edita `src/main.cpp` — três funções pra implementar. Não mexe em
`src/mesh_node_core.*`.

Ver `AGENTS.md` pra arquitetura completa e o histórico do bug do painlessMesh que essa
lib corrige.
