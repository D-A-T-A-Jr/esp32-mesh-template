# ESP32 Mesh Node

Template pra um nó da rede mesh de sensores (ESP32/ESP8266 + painlessMesh +
ThingsBoard). Cada placa física clona este repo e customiza `src/main.cpp` com seus
próprios sensores; a malha (rede, WiFi, MQTT) já vem pronta e não precisa mexer.

## Uso rápido

```
cp .env.example .env
# preenche TB_HOST no .env (WiFi nao vai aqui, ver abaixo)
pio run -t upload
pio device monitor
```

Placa ESP8266 (ex. LOLIN/Wemos D1 mini): `pio run -e esp8266 -t upload` (o env padrão
é `esp32`, ver AGENTS.md).

No primeiro boot (ou sempre que não achar WiFi salvo), a placa sobe um AP próprio
(`ESP-<MAC>`) — conecta nele pelo celular e configura a rede pela interface web que
abre sozinha. Não precisa editar `.env` nem recompilar pra trocar de rede.

Anota o "Node ID" que aparece no boot, cria um device no ThingsBoard, e preenche
`NODE_<id>_TOKEN` / `NODE_<id>_NAME` no `.env` (ver AGENTS.md pros detalhes).

## Customizando os sensores

Edita `src/main.cpp` — três funções pra implementar. Não mexe em
`src/mesh_node_core.*`.

## OTA / release

Push pra `main` (commit convencional) publica firmware novo automaticamente via
ThingsBoard — ver `AGENTS.md` pros secrets necessários no GitHub.

Ver `AGENTS.md` pra arquitetura completa e o histórico do bug do painlessMesh que essa
lib corrige.
