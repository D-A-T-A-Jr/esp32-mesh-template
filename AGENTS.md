# ESP32 Mesh Node — guia pra agentes de código

Este repo é um template: um clone dele = uma placa física na malha, com seus próprios
sensores. Todo repo derivado compartilha a mesma malha com N outros repos clonados do
mesmo template — decisões aqui afetam placas que este agente pode nem estar vendo.

## Antes de mexer em qualquer coisa

**Pode editar livremente:** `src/main.cpp` inteiro (é o ponto de customização).

**Pode editar com cuidado, avisando o motivo no commit:** as constantes configuráveis
listadas abaixo, em `mesh_node_core.cpp`.

**Não edite `mesh_node_core.h`/`mesh_node_core.cpp` fora dessas constantes** sem um
motivo concreto (bug real no núcleo). Se editar mesmo assim, a mudança precisa ser
propagada pros outros repos da mesma malha — um núcleo divergente entre placas quebra a
malha de formas difíceis de depurar (protocolo `hello`/mensagens incompatíveis, canal
WiFi diferente, etc). Se não tiver certeza se algo é "customização" ou "núcleo", trate
como núcleo e não mexa.

## O que É configurável (topo de `mesh_node_core.cpp`)

| Constante | Efeito | Cuidado ao mudar |
|---|---|---|
| `ROUTER_CHANNEL` | Canal WiFi de toda a malha (AP + STA) | **Precisa ser idêntico em TODOS os repos da mesma malha**, igual `MESH_PREFIX`. Ver seção "Canal WiFi" — não é auto-detectado por nó, isso quebraria a malha entre placas com roteadores diferentes. |
| `PUBLISH_CYCLE_DEFAULT_MS` / `_MIN_MS` / `_MAX_MS` | Valor inicial e limites do intervalo entre rajadas de publish | O valor em uso (`publishCycleMs`) pode ser trocado em runtime via atributo compartilhado `publish_cycle_ms` no ThingsBoard — ver seção "Configurar intervalo pelo ThingsBoard". Mudar o default/limites aqui só afeta o que roda até alguém setar o atributo. |
| `NODE_BROADCAST_INTERVAL_MS` | Intervalo do "hello" que cada nó manda pra malha | Mais baixo = malha percebe nó sem WiFi mais rápido, mais tráfego. |
| `BURST_CONNECT_TIMEOUT_MS` | Quanto tempo espera WiFi/MQTT conectar antes de desistir da rajada | Rede mais lenta pode precisar de mais tempo. |
| `ATTR_WAIT_MS` | Quanto tempo espera resposta do ThingsBoard sobre firmware novo | Rede mais lenta pode precisar de mais tempo. |
| `OTA_CHUNK_TIMEOUT_MS` / `OTA_CHUNK_SIZE` | Timeout e tamanho de chunk do download de OTA | Chunk maior = menos round-trips, mais RAM usada por vez. |
| `MAX_PENDING` | Quantas mensagens de vizinho sem WiFi ficam em fila esperando repasse | Vizinho muito falante ou rajada rara pode precisar de mais. |
| `MESH_PREFIX` / `MESH_PASSWORD` | Nome/senha da malha | **Precisa ser idêntico em TODOS os repos da mesma malha.** Mudar num repo só isola ele dos outros. |
| `MESH_ENABLED` (topo do arquivo, `#define`) | `1` = participa da malha (padrão). `0` = placa standalone, sem malha nenhuma | Ver seção "Modo standalone". Decisão por repo, não precisa ser igual entre placas. |

## O que NÃO fazer (motivo: já quebrou antes, ver histórico abaixo)

- **Não** chamar `mesh.stationManual()` condicionalmente (só quando "eleito" gateway,
  ou só às vezes). Todo nó chama sempre, incondicional, no `meshNodeSetup()`. Uma
  eleição de gateway já foi tentada aqui e causava instabilidade de reconexão; o design
  atual (todo nó tenta WiFi sempre, a lib cai pra malha sozinha se não achar o
  roteador) é mais simples e mais estável.
- **Não** usar `mesh.initAsBridge()` / `mesh.initAsSharedGateway()` (recursos do fork
  Alteriom do painlessMesh). Foram testados neste projeto e se mostraram menos
  confiáveis que a combinação atual (`mesh.init()` + `stationManual()` na lib
  corrigida). Ver `PAINLESSMESH_FORK_TASK.md` no repo original se precisar do histórico
  completo de testes.
- **Não** manter a conexão MQTT aberta fora da janela da rajada (`runPublishBurst`),
  nem remover o `WiFi.mode(WIFI_STA)` / `WiFi.mode(WIFI_AP_STA)` dentro dela. AP da
  malha ativo + conexão externa persistente é instável no rádio único do ESP32 —
  testado e confirmado, não é suposição.
- **Não** renomear os campos do JSON usado no `hello` (`type`, `node_id`, `uptime_ms`,
  `has_wifi`) nem no relay (`v1/gateway/telemetry`) sem atualizar TODOS os repos da
  malha ao mesmo tempo — são o "protocolo" entre nós.
- **Não** mudar o formato `NODE_<id>_TOKEN` / `NODE_<id>_NAME` do `.env` nem o nome
  dos campos gerados em `generated_secrets.h` (`NodeCredential`, `NODE_TABLE`,
  `NODE_TABLE_COUNT`) sem atualizar `scripts/load_env.py` em todos os repos junto.
- **Não** colocar token, senha de WiFi ou qualquer segredo direto no código. Sempre via
  `.env` → `generated_secrets.h` (gerado, gitignored).
- **Não** usar `delay()` dentro de `appLoop()` — trava a malha inteira enquanto durar.

## Arquitetura

- `src/mesh_node_core.h` / `mesh_node_core.cpp` — núcleo compartilhado. Cuida de:
  formar a malha (painlessMesh), priorizar WiFi direto (cai pra retransmitir via
  vizinho sozinho se não achar o roteador), publicar telemetria própria, repassar
  telemetria de vizinho sem WiFi via Gateway API do ThingsBoard, e OTA.
- `src/main.cpp` — implementa 4 hooks:
  - `appSetup()` — inicializa os sensores/atuadores dessa placa.
  - `appLoop()` — leitura contínua (sem `delay()`, roda todo ciclo do loop principal).
  - `appCollectTelemetry(JsonObject &out)` — chamado só quando a placa tem WiFi direto
    e vai publicar; adiciona os campos dessa placa em `out`.
  - `appHandleRpc(const String &method, JsonVariantConst params, JsonDocument &response)`
    — chamado quando chega um RPC do ThingsBoard (`v1/devices/me/rpc/request/+`).
    Preenche `response` só se quiser responder (vazio = não publica nada). No modo
    malha só chega se a placa estiver conectada bem naquele instante (rajada de
    publish) — RPC não fica em fila, comando fora da janela de conexão se perde sem
    erro nem aviso. No modo standalone (`MESH_ENABLED=0`) a conexão é persistente,
    então chega sempre.

## Por que essa arquitetura (bug histórico)

O painlessMesh oficial (e forks) tem um bug conhecido sem correção: `stationManual()`
reaproveita o scan de descoberta de peer da malha, deixando o nó cego pros outros nós da
malha assim que tenta conectar num roteador externo (GitLab issues #380, #450, #279).

O `lib_deps` do `platformio.ini` aponta pra `github.com/jonatasperaza/painlessMesh-fixed`
— fork corrigido, funciona em qualquer máquina que clonar este template.

## Configuração (`.env`, não versionado)

```
TB_HOST=
TB_PORT=1883
FW_TITLE=
FW_VERSION=

# so pra placa standalone (MESH_ENABLED=0)
TB_TOKEN=

# so pra placa de malha (MESH_ENABLED=1)
NODE_<id>_TOKEN=
NODE_<id>_NAME=
```

WiFi **não** vem do `.env` — ver seção "Provisionamento WiFi" abaixo.

`<id>` é o "Node ID" que aparece no boot via Serial (`[MESH] Node ID: ...`), derivado do
MAC da placa. Cria um device por placa no ThingsBoard, pega o token, preenche aqui.

**Importante:** liste TODOS os nós da malha no `.env`, não só o desse repo — qualquer
placa pode precisar repassar telemetria de qualquer vizinho sem WiFi, e pra isso precisa
saber o nome do device dele (não o token, só o nome). Mantenha o `.env` sincronizado
entre todos os repos da mesma malha.

Sem uma entrada na `NODE_TABLE` pro Node ID dessa placa, ela ainda participa da malha e
retransmite pros outros, mas não publica a própria telemetria (evita misturar dado de
placa não identificada num device errado).

## Provisionamento WiFi (WiFiManager)

A rede WiFi não é fixa no código nem no `.env`: a placa usa `WiFiManager` pra conectar
com a credencial já salva na flash e, se não conseguir, sobe um AP de configuração
próprio (`ESP-<MAC>`) — conecta nele pelo celular/notebook e escolhe a rede pela
interface web do WiFiManager. A credencial fica salva na flash da placa (SDK), não
precisa reconfigurar a cada boot nem a cada novo firmware.

Placa de malha (`MESH_ENABLED=1`): o portal tem timeout (180s) — se ninguém configurar,
a placa segue o boot mesmo sem WiFi e participa da malha normalmente como relay (esse é
o comportamento certo: falta de configuração não pode travar a malha).

Placa standalone (`MESH_ENABLED=0`): sem esse fallback, então o portal fica esperando
indefinidamente até alguém configurar — não tem pra onde mais essa placa ir sem WiFi.

Depois de conectado, o core lê `WiFi.SSID()`/`WiFi.psk()` (o que o WiFiManager acabou de
guardar) pra usar como alvo do `stationManual()` — não precisa (e não deve) duplicar
essas credenciais em nenhuma constante do código.

## Canal WiFi

`ROUTER_CHANNEL` é o canal de rádio de **toda a malha** (AP da malha + STA de cada nó)
— fixo no código, igual em todo repo, exatamente como `MESH_PREFIX`/`MESH_PASSWORD`.
Não é auto-detectado por nó: o ESP32/ESP8266 só opera 1 canal por vez pra AP+STA, e
dois nós só formam malha entre si se estiverem no mesmo canal. Se cada placa
escolhesse o canal do próprio roteador, placas com roteadores diferentes nunca se
enxergariam — e a malha (o "se um cair, o outro segura") é o requisito mais importante
do projeto, prioridade acima de qualquer placa individual conseguir WiFi direto.

O boot faz um scan e **avisa** (não decide sozinho) se o seu `WIFI_SSID` está num canal
diferente do `ROUTER_CHANNEL` configurado (`warnIfRouterChannelMismatch()`) — nesse
caso, aquela placa especificamente nunca vai conseguir WiFi direto, só vai
relay/receber relay via malha, o que ainda é o comportamento correto do sistema.

Pra descobrir o canal certo pra configurar: no painel do roteador, ou um app de WiFi no
celular. Numa implantação normal (todas as placas na mesma estufa, mesmo roteador),
configure `ROUTER_CHANNEL` pro canal desse roteador em todos os repos e pronto.

## Modo standalone (`MESH_ENABLED=0`)

Pra uma placa que não precisa (ou não pode, por posição física) participar da malha:
muda `#define MESH_ENABLED` pra `0` no topo de `mesh_node_core.cpp`. Isso troca todo o
comportamento de malha por um fluxo bem mais simples, igual o firmware original antes
da malha existir:

- Sem `painlessMesh`, sem AP próprio — só `WiFiManager` (ver seção acima) + conexão
  WiFi direta e persistente (não tem o problema de canal AP+STA compartilhado que
  obriga a malha a fazer rajadas, então fica sempre conectado ao MQTT).
- Usa `TB_TOKEN` do `.env` direto (device único no ThingsBoard, sem `NODE_TABLE`).
- OTA continua funcionando igual (mesmo código, não depende de malha).
- Os 4 hooks do `main.cpp` continuam os mesmos — não muda entre os dois modos.

Não tem fallback nesse modo: se a placa perder WiFi, fica tentando reconectar sozinha
(sem vizinho pra repassar telemetria por ela). É a troca consciente ao desligar a
malha.

## Ao criar um novo tipo de placa

1. Edita só `src/main.cpp` — preenche os 4 hooks com os sensores reais.
2. Adiciona os `lib_deps` que esses sensores precisarem no `platformio.ini` (o template
   não vem com nenhuma lib de sensor por padrão).

## ESP8266

`platformio.ini` tem `[env:esp32]` (default) e `[env:esp8266]` (board `d1_mini`), os
dois compilando o mesmo `mesh_node_core.cpp`/`main.cpp`. Pra buildar/gravar no ESP8266:
`pio run -e esp8266` / `pio run -e esp8266 -t upload`.

`mesh_node_core.cpp` já isola as duas diferenças reais de plataforma via
`#if defined(ESP32) / #elif defined(ESP8266)` — não precisa mexer nisso:

- **SHA-256 do OTA**: mbedtls no ESP32, BearSSL (`bearssl/bearssl_hash.h`) no ESP8266
  (o core do ESP8266 não embute mbedtls).
- **Header de OTA**: `Update.h` no ESP32, `Updater.h` no ESP8266 — e nessa a classe não
  tem `.abort()` (o próximo `Update.begin()` já limpa o estado sozinho).
- **LED onboard**: `LED_ACTIVE_LOW` inverte a polaridade (D1 mini liga com `LOW`). Use
  sempre `setLed(bool)` no núcleo, nunca `digitalWrite(LED_PIN, ...)` direto.

`scripts/build_and_upload_ota.py` só builda `ENV_NAME="esp32"` — se for distribuir OTA
pra placas ESP8266 também, duplique o script/job apontando pro outro Device Profile e
env, não misture os dois binários no mesmo pacote OTA.

## Debug

`[env:esp32_debug]` / `[env:esp8266_debug]` compilam o mesmo firmware do env base
correspondente, só com `-D MESH_DEBUG` a mais (`extends` no `platformio.ini`, sem
duplicar `lib_deps`). Com isso ligado, `mesh_node_core.cpp` imprime no Serial:

- a cada 5s: status do WiFi direto (`WiFi.status()`, `meshHasDirectWifi()`, SSID,
  canal, RSSI, IP), e quantos nós a malha enxerga.
- no fim de cada rajada de publish (`runPublishBurst`): se o WiFi conectou e se o MQTT
  conectou.
- log interno `CONNECTION` da própria painlessMesh (`mesh.setDebugMsgTypes`) — mostra
  o ciclo de scan/conexão do `stationManual()` linha a linha (`stationScan()`,
  `scanComplete()`, APs achados, `connectToAP()`). Útil quando o `wifi_status=` fica
  parado em `0` (idle) e não muda nunca: se esse log também não aparecer, o problema é
  a tarefa de scan não estar rodando, não a rede em si.

Se o WiFi ficar preso em `wifi_status=0` por bastante tempo (minutos) sem nenhum log
`CONNECTION` aparecer, mesmo com a rede alvo visível: já aconteceu de um ESP8266 real
"empacar" o rádio depois de muitos reflashes/resets via software seguidos (reset por
RTS/software não reinicializa a calibração de RF). Um power-cycle de verdade
(desconectar e reconectar o USB) resolveu — vale tentar antes de desconfiar de bug de
código.

É só diagnóstico, não muda nenhum comportamento do núcleo; pode ligar/desligar à
vontade sem afetar os envs de produção.

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
TB_HOST                    — host do ThingsBoard
TB_URL                     — URL completa (http/https) da API do ThingsBoard
TB_USERNAME, TB_PASSWORD   — login de tenant admin, pra criar o pacote OTA
TB_DEVICE_PROFILE_ID       — ID do Device Profile que essas placas usam
```

## Configurar intervalo pelo ThingsBoard

`publish_cycle_ms` é um atributo compartilhado (shared attribute) — igual
`fw_title`/`fw_version` pro OTA, mesmo mecanismo. Seta no device (ou no Device Profile,
pra afetar todas as placas desse tipo de uma vez) no ThingsBoard, e a placa aplica na
próxima rajada — não precisa reflashar nem reiniciar. Limites: `PUBLISH_CYCLE_MIN_MS`
(5s) a `PUBLISH_CYCLE_MAX_MS` (1h) no `mesh_node_core.cpp`; valor fora disso é
ignorado, mantém o que já estava.

Esse é o mesmo mecanismo (atributo compartilhado, pedido em toda rajada) usado pro OTA
— ao contrário de RPC, nunca perde o comando por estar desconectado no instante exato,
só atrasa até a próxima rajada.
