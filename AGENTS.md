# ESP32 Mesh Node — guia pra agentes de código

Este repo é um template: um clone dele = uma placa física na malha, com seus próprios
sensores. Todo repo derivado deve seguir esta mesma estrutura pra funcionar junto com os
outros nós.

## Arquitetura

- `src/mesh_node_core.h` / `mesh_node_core.cpp` — núcleo compartilhado. **Não edite.**
  Cuida de: formar a malha (painlessMesh), priorizar WiFi direto (cai pra retransmitir
  via vizinho sozinho se não achar o roteador), publicar telemetria própria, repassar
  telemetria de vizinho sem WiFi via Gateway API do ThingsBoard.
- `src/main.cpp` — **é aqui que você mexe.** Implementa 3 hooks:
  - `appSetup()` — inicializa os sensores/atuadores dessa placa.
  - `appLoop()` — leitura contínua (sem `delay()`, roda todo ciclo do loop principal).
  - `appCollectTelemetry(JsonObject &out)` — chamado só quando a placa tem WiFi direto
    e vai publicar; adiciona os campos dessa placa em `out`.

## Por que essa arquitetura (bug histórico)

O painlessMesh oficial (e forks) tem um bug conhecido sem correção: `stationManual()`
reaproveita o scan de descoberta de peer da malha, deixando o nó cego pros outros nós da
malha assim que tenta conectar num roteador externo (GitLab issues #380, #450, #279).

O `lib_deps` do `platformio.ini` aponta pra `github.com/jonatasperaza/painlessMesh-fixed`
— fork corrigido, funciona em qualquer máquina que clonar este template.

Devido a essa mesma limitação de rádio único (AP+STA compartilham canal e não dá pra
manter uma conexão externa persistente com a malha ativa ao mesmo tempo sem instabilidade),
a publicação de telemetria funciona em "rajadas" curtas (conecta, publica, desconecta) a
cada 30s, não uma conexão MQTT sempre aberta.

## Configuração (`.env`, não versionado)

```
WIFI_SSID=
WIFI_PASS=
TB_HOST=
TB_PORT=1883
FW_TITLE=
FW_VERSION=

NODE_<id>_TOKEN=
NODE_<id>_NAME=
```

`<id>` é o "Node ID" que aparece no boot via Serial (`[MESH] Node ID: ...`), derivado do
MAC da placa. Cria um device por placa no ThingsBoard, pega o token, preenche aqui.

**Importante:** liste TODOS os nós da malha no `.env`, não só o desse repo — qualquer
placa pode precisar repassar telemetria de qualquer vizinho sem WiFi, e pra isso precisa
saber o nome do device dele (não o token, só o nome). Mantenha o `.env` sincronizado
entre todos os repos da mesma malha.

Sem uma entrada na `NODE_TABLE` pro Node ID dessa placa, ela ainda participa da malha e
retransmite pros outros, mas não publica a própria telemetria (evita misturar dado de
placa não identificada num device errado).

## Canal WiFi

`ROUTER_CHANNEL` em `mesh_node_core.cpp` precisa bater com o canal do seu roteador (o
ESP32 só opera em 1 canal por vez pra AP+STA). Descubra o canal no painel do roteador ou
com um app de WiFi no celular.

## Ao criar um novo tipo de placa

1. Edita só `src/main.cpp` — preenche os 3 hooks com os sensores reais.
2. Adiciona os `lib_deps` que esses sensores precisarem no `platformio.ini` (o template
   não vem com nenhuma lib de sensor por padrão).
3. Não mexe em `mesh_node_core.h/.cpp` a menos que seja pra corrigir um bug do núcleo
   em si (nesse caso, propague a correção pros outros repos da malha também).

## OTA

O núcleo já suporta atualização remota via ThingsBoard: a cada rajada de publish, a
placa (se tiver WiFi direto) também pede os atributos compartilhados de firmware; se a
versão for diferente da instalada (`FW_VERSION`), baixa os chunks, confere o SHA-256 e
reinicia sozinha. Nada a fazer no `main.cpp` pra isso funcionar.

Publicar uma nova versão é automático: push pra `main` com um
[commit convencional](https://www.conventionalcommits.org/) (`fix:`, `feat:`, etc.) —
o workflow builda, cria um pacote OTA no ThingsBoard vinculado ao Device Profile
(todas as placas desse tipo recebem, não é por device) e faz a release no GitHub.

Secrets necessários no repo (Settings → Secrets and variables → Actions):

```
WIFI_SSID, WIFI_PASS       — usados só pra buildar (nao pro fluxo de OTA em si)
TB_HOST                    — host do ThingsBoard
TB_URL                     — URL completa (http/https) da API do ThingsBoard
TB_USERNAME, TB_PASSWORD   — login de tenant admin, pra criar o pacote OTA
TB_DEVICE_PROFILE_ID       — ID do Device Profile que essas placas usam
```
