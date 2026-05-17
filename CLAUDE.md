# CLAUDE.md - ClipSync (BLE Task Board)

Notas para futuras sessões. O README cobre o "como usar". Aqui ficam **pontos
de atenção do código** que ainda não foram corrigidos.

## Layout do repo

- `src/main.cpp` - firmware ClipSync (TFT + touch + relay BLE)
- `src/display_test.cpp` - bancada para validar TFT/touch e calibrar
- `platformio.ini` - dois envs:
  - `esp32dev` - firmware completo (build_src_filter exclui display_test.cpp)
  - `display_test` - só TFT + touch, sem NimBLE (build_src_filter exclui main.cpp)

Para trocar o que vai pra placa, use `pio run -e <env> -t upload`.

## Pinagem (consolidada)

Display ILI9341 (`platformio.ini`, `[common].tft_flags`).
**Alimentacao 5V:** o modulo tem regulador 3V3 onboard. VCC e LED vao em
**VIN/5V do ESP32**, nao em 3V3. Logica SPI continua 3V3 (level shifter
onboard no modulo).

| Sinal     | GPIO ESP32  | Notas                          |
|-----------|-------------|--------------------------------|
| TFT VCC   | VIN (5V)    | regulador onboard cuida do 3V3 |
| TFT GND   | GND         |                                |
| TFT_MOSI  | 23          | VSPI MOSI default              |
| TFT_MISO  | 19          | VSPI MISO default              |
| TFT_SCLK  | 18          | VSPI SCK default               |
| TFT_CS    | 15          | strapping pin - ok no boot     |
| TFT_DC    | 2           | strapping pin - ok no boot     |
| TFT_RST   | 4           |                                |
| TFT LED   | VIN (5V)    | always-on, sem controle por GPIO|

Touch XPT2046 (`src/main.cpp:33-36`, `src/display_test.cpp:23-25`):

| Sinal           | GPIO | Notas                                                  |
|-----------------|------|--------------------------------------------------------|
| TOUCH_CS_PIN    | 33   | CS dedicado                                            |
| TOUCH_IRQ_PIN   | 27   | bidirecional, INPUT_PULLUP habilitado apos `ts.begin()`|

MOSI/MISO/SCK compartilhados entre TFT e touch. Frequencias: TFT 40 MHz
write / 20 MHz read; touch 2.5 MHz.

### Pinagem fisica: DC e RST invertidos no conector
O modulo desta bancada tem o **DC e o RST trocados** em relacao ao
silkscreen / ordem "esperada". Pinagem real verificada:
- DC do display -> GPIO 2 do ESP32
- RST do display -> GPIO 4 do ESP32

Sintoma quando trocado: tela completamente branca, sem reagir, mesmo com
SPI 100% correto. Detectado no Stage 2 do bring-up.

### Sequencia obrigatoria de boot (TFT + touch no mesmo SPI)
**Antes** de chamar `tft.init()`, e necessario forcar `TFT_CS` e `TOUCH_CS`
em HIGH manualmente:

    pinMode(15, OUTPUT); digitalWrite(15, HIGH);          // TFT_CS
    pinMode(33, OUTPUT); digitalWrite(33, HIGH);          // TOUCH_CS

Se nao fizer isso, o XPT2046 pode estar com CS flutuando em LOW no boot
e intercepta os bytes do init-sequence do ILI9341 -> tela fica branca
permanentemente. Sintoma: backlight aceso, sem imagem, sem mensagens de
erro. Foi exatamente o que travou o bring-up.

Bonus: GPIO 15 e strapping pin - forcar HIGH cedo tambem evita problemas
de boot quando o display esta plugado.

Aplicado em `src/display_test.cpp` e `src/main.cpp`.

### Riscos de pinagem
- IRQ originalmente era GPIO 36 (input-only, sem pull-up interno). Movido
  para GPIO 27 porque (a) a placa do usuario nao expoe GPIOs > 35 e (b)
  PUR por software evita resistor externo. `XPT2046_Touchscreen::begin()`
  faz `pinMode(IRQ, INPUT)` e zera o pull-up - **sempre** chamar
  `pinMode(TOUCH_IRQ_PIN, INPUT_PULLUP)` *depois* de `ts.begin()`.
- Backlight sem controle por software (LED ligado direto em 5V). Para
  dimming/PWM, intercalar um MOSFET-N entre LED- e GND e chavear pelo
  GPIO 21 com LEDC. Reativar `-DTFT_BL=21 -DTFT_BACKLIGHT_ON=HIGH` em
  `platformio.ini`.

## Pontos de atenção pendentes em `src/main.cpp`

### 1. Calibração de touch [OK - 2026-05-17] — transformação AFIM
**Mapeamento linear por eixo (min/max) NAO funciona** neste painel: ele
tem ~12 graus de rotacao fisica em relacao ao display, e o resultado fica
em forma de losango ao tracar o bezel. Foi necessario afim:

    screen_x = AX*raw_x + BX*raw_y + CX
    screen_y = AY*raw_x + BY*raw_y + CY

Coeficientes aplicados em `src/main.cpp` (linhas ~46-51):

    AX =   0.112955    BX = 0.023929    CX = -108.792
    AY =   0.020522    BY = 0.088083    CY = -101.226

Calculados por minimos quadrados sobre 4 alvos no `display_test.cpp`.
Reverse-check em pixels: erro de 1.1 a 3.6 px em todos os 4 cantos
(quase pixel-perfect). Os termos cruzados (`BX` e `AY`) sao o que
absorve a rotacao - num painel sem rotacao seriam ~0.

**Para recalibrar:** `pio run -e display_test -t upload`, anote os 6
defines impressos no serial e cole em `main.cpp`. Use alvos posicionados
em 40px de inset do bezel para evitar o usuario pressionar fora do
crosshair (alvos em cantos absolutos sao dificeis de mirar).

**Cuidados tecnicos do XPT2046 sob SPI compartilhado que motivaram este
nivel de filtragem:**

1. Leituras espurias quando o chip "ouve" trafego SPI do TFT - assinatura
   X 13-50, Y 3400-3900, **z 300-330**. Reais tem z >= 700.
2. Warm-up do dedo: ao aterrissar, as primeiras 1-3 leituras estao em
   uma posicao transitoria diferente da posicao estavel.

Mitigacoes em `display_test.cpp` que devem ser replicadas em qualquer
outro consumo do XPT2046:
- Gate `tirqTouched() + touched() + z >= 600` (helper `sampleTouch()`)
- Coletar amostras com warm-up (descarte das 3 primeiras) + media de 5
- Rejeitar captura se spread max-min for > 150 (dedo escorregou)

Em `main.cpp/handleTouch()` aplicamos so o gate de z (TOUCH_Z_THRESHOLD)
porque la nao precisamos de precisao de calibracao, so de filtrar ruido.

### 2. `flashStatus()` usa `delay()` bloqueante (linha 308)
O `delay(durationMs)` no meio do callback de touch trava o `loop()` por até
1,2 s. Eventos BLE (`onWrite`) ainda rodam porque NimBLE é em task própria,
mas o touch fica surdo. Solução: estado de "flash até `millis() > expiraEm`"
desenhado no próximo frame.

### 3. `drawUI()` em callback BLE [OK - 2026-05-17]
**Resolvido.** Padrao `g_uiDirty` (volatile bool) implementado: callbacks
`ServerCb::onConnect/onDisconnect` e `WriteCb::onWrite` apenas setam a
flag, e o `loop()` consome ela chamando `drawUI()` na task do Arduino.

Foi mais critico do que parecia: NimBLE 2.x tem stack apertado na task
do host, e desenhar TFT (SPI heavy) lah dentro causava **crash com
backtrace** sempre que um WRITE de from_pc/from_mobile chegava. So
parou de crashar com a flag em uso.

Lesson: NUNCA fazer SPI/I2C/IO pesado dentro de callbacks NimBLE - so
mexer em estado (atomico ou volatile) e drenar no loop principal.

### 4. Sem chunking de payload BLE
MTU 517 → ~500 bytes úteis. macOS negocia ~185. Clipboards maiores são
**silenciosamente truncados** no `setValue`/`notify`. Para v2: header binário
`{seq, total, len}` + reassembly no ESP32 + ACK por chunk.

### 5. Sem autenticação/bonding
Qualquer dispositivo BLE pode conectar, ler e escrever. NimBLE expõe
`setSecurityAuth(bond, mitm, sc)` e `setSecurityPasskey()`. Mínimo viável:
PIN estático + bonding. Ideal: AES-128 no payload com chave compartilhada
fora do canal BLE.

### 6. Sem timeout de comando
Se o cliente não responder a `CMD_REQ_PC`/`CMD_REQ_MOBILE`, o status fica
em "Enviando..." pra sempre e a UI não dá feedback. Adicionar timer de
~3 s e mostrar "Sem resposta" na status line.

### 7. Sem persistência de eventos
Se o destinatário estiver desconectado no momento do ENVIAR, o `notify` é
descartado pelo stack. Para fluxos assíncronos, guardar o último payload
em RAM e re-enviar no próximo `onConnect`.

### 8. UI usa coordenadas absolutas mas `tft.width()/height()` não é checado
Os `Rect` (linhas 66-68) assumem 320x240. Se alguém trocar a rotação ou o
display, os retângulos saem da tela sem erro. Não é urgente, mas vale
parametrizar quando mexer no layout.

## Backlog / TODOs de produto

### Historico de clipboard com selecao (futuro)
**Pedido pelo usuario em 2026-05-17.** Comportamento atual: ao tocar
ENVIAR, o cliente (PC ou celular) le APENAS o item atual da area de
transferencia e envia. Comportamento desejado depois que tudo basico
estiver funcionando: cliente mantem uma lista historica dos ultimos N
itens da clipboard (clipboard manager) e a UI do ClipSync no display
permite escolher qual item enviar antes de confirmar.

Esboco de implementacao:
- Cliente (PC/celular) hookeia eventos de mudanca de clipboard e mantem
  lista circular dos ultimos N (ex.: 10) itens com timestamp.
- Novo comando BLE no protocolo: `CMD_REQ_PC_LIST` / `CMD_REQ_MOBILE_LIST`.
- Cliente responde com payload contendo a lista (precisa de chunking -
  ver item 4 acima).
- Display mostra lista scrollavel, usuario toca o item desejado.
- Display envia comando `CMD_PICK_INDEX` com o indice escolhido.
- Cliente envia o item correspondente como hoje (via from_pc/from_mobile).

Pre-requisito: chunking (item 4) precisa estar pronto, porque lista de 10
itens de ate 500 bytes cada nao cabe num write/notify unico.

## NimBLE-Arduino: usar 2.x (NAO 1.4.x)

`platformio.ini` pinou `h2zero/NimBLE-Arduino@^2.0.0`. **Nao downgrade pra
1.4.x.**

Por que: o platform-espressif32 atual (55.x) traz arduino-esp32 3.x baseado
em ESP-IDF v5.x. NimBLE-Arduino 1.4.x foi escrito para arduino-esp32 2.x /
ESP-IDF v4.x e a API do controlador BT divergiu. Sintoma classico:

    [BLE] BT controller status pre-init: 0      <- IDLE ja!
    ESP_ERROR_CHECK failed: esp_err_t 0x103 (ESP_ERR_INVALID_STATE)
    at NimBLEDevice.cpp line 880 (esp_bt_controller_init)
    abort() ... Rebooting...  (crash loop infinito)

E nao adianta cleanup manual de estado - o status do controller ja esta em
IDLE quando a falha ocorre. So upgrade da lib resolve.

### Diferencas de API 1.4 -> 2.x aplicadas neste projeto
- `NimBLEServerCallbacks::onConnect(server, ble_gap_conn_desc*)`
  -> `onConnect(server, NimBLEConnInfo&)`
- `NimBLEServerCallbacks::onDisconnect(server, ble_gap_conn_desc*)`
  -> `onDisconnect(server, NimBLEConnInfo&, int reason)`
- `NimBLECharacteristicCallbacks::onWrite(char)`
  -> `onWrite(char, NimBLEConnInfo&)`
- `NimBLEAdvertising::setScanResponse(bool)` removido (scan response
  agora gerenciado automatico via setScanResponseData).

## Convenções

- Português nos comentários e nas strings da UI (o usuário é PT-BR).
- Sem emoji no código nem em commits, salvo pedido explícito.
- `delay()` bloqueante é aceitável **só** em bring-up (display_test); no
  firmware de produção, evite.
