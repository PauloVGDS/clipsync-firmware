# ClipSync - ESP32 BLE Clipboard Relay

Sincroniza clipboard entre PC e celular sob demanda, usando ESP32 com display
touch como gatilho fisico.

## Estrutura

Este repo cobre **somente o firmware** do ESP32. Os clientes ficam em
projetos separados:

    BLE Task Board/                          <- ESTE repo (firmware)
      src/
        main.cpp            <- firmware ClipSync (TFT + touch + relay BLE)
        display_test.cpp    <- bancada Stage 4: smoke + calibracao
        test_02_tft.cpp     <- bancada Stage 2: so TFT (sem touch)
        test_03_touch.cpp   <- bancada Stage 3: touch com feedback no TFT
      include/, lib/, test/ <- (vazios) padrao PlatformIO
      platformio.ini        <- envs esp32dev + bancadas Stage 2/3/4
      CLAUDE.md             <- notas tecnicas, pontos de atencao, TODOs
      README.md             <- este arquivo

    C:\Users\lordk\ClipSync\                 <- workspace dos clientes
      pc_client/
        clipsync_pc.py      <- cliente Python (bleak + pyperclip)
        requirements.txt
      android/              <- (futuro) app Android nativo

> Cliente do celular: enquanto o app Android nao existe, da pra usar
> **Tasker** (instrucoes mais abaixo).

## Setup do firmware (ESP32)

1. Abra a pasta do projeto no VSCode com PlatformIO.
2. **Ajuste os pinos do TFT no `platformio.ini`** (`TFT_MOSI`, `TFT_MISO`,
   `TFT_SCLK`, `TFT_CS`, `TFT_DC`, `TFT_RST`, `TFT_BL`) conforme seu cabeamento.
3. **Ajuste os pinos do touch no `src/main.cpp`** (`TOUCH_CS_PIN`,
   `TOUCH_IRQ_PIN`). O XPT2046 compartilha MOSI/MISO/SCK com o TFT.
4. Faca upload do env principal:

        pio run -e esp32dev -t upload
        pio device monitor -e esp32dev

   No monitor serial voce deve ver `[BLE] advertising started`.

### Bancada: smoke test do display e calibracao do touch

Antes de subir o firmware completo, rode o env `display_test` para validar
hardware e capturar os limites brutos do XPT2046:

    pio run -e display_test -t upload
    pio device monitor -e display_test

O sketch:

1. Roda um smoke test do TFT (cores, primitivas, fontes).
2. Pede que voce toque em 4 alvos (cantos da tela).
3. Imprime no serial e na tela os 4 valores prontos:

        TOUCH_RAW_X_MIN = ...
        TOUCH_RAW_X_MAX = ...
        TOUCH_RAW_Y_MIN = ...
        TOUCH_RAW_Y_MAX = ...

4. Entra em modo verificacao: cada toque desenha um ponto ciano - confirme
   que o ponto cai onde o dedo esta. Se nao bater, pode ser necessario
   trocar X por Y ou inverter um eixo (orientacao do conector do touch).

Cole os quatro valores em `src/main.cpp` e refaca o upload do env `esp32dev`.

## Setup do cliente PC

O cliente vive em **outro repo/folder**: `C:\Users\lordk\ClipSync\pc_client\`.
Setup (Windows):

    cd C:\Users\lordk\ClipSync\pc_client
    python -m venv .venv
    .venv\Scripts\activate
    pip install -r requirements.txt
    python clipsync_pc.py

**Linux:** instale `xclip` (X11) ou `wl-clipboard` (Wayland) para o pyperclip
funcionar:

    sudo apt install xclip          # X11
    sudo apt install wl-clipboard   # Wayland

**macOS:** o cliente Python funciona, mas o backend BLE da Apple negocia MTU
em ~185 bytes, o que aperta bastante o payload util (~180 bytes). Sem chunking
no firmware, textos maiores que isso serao truncados.

## Setup do celular (Android via Tasker)

Tasker tem actions de Bluetooth LE desde a versao 6.x. Voce vai precisar de:

1. **Tarefa: Conectar e subscribe ao cmd**
   - Action: Net > Bluetooth LE > Connect
     - Device name: `ClipSync`
   - Action: Net > Bluetooth LE > Subscribe to characteristic
     - Service UUID: `b1c2d3e4-f5a6-4b7c-8d9e-0f1a2b3c4d5e`
     - Characteristic UUID: `b1c2d3e4-f5a6-4b7c-8d9e-0f1a2b3c4d5f`
     - Variable: `%cmd_value`
   - Action: Subscribe novamente para `to_mobile`
     - Characteristic UUID: `b1c2d3e4-f5a6-4b7c-8d9e-0f1a2b3c4d63`
     - Variable: `%to_mobile_value`

2. **Profile: BLE notification em cmd**
   - Quando `%cmd_value` mudar e for `0x02` (REQ_MOBILE):
     - Action: Variable > Variable Set > `%clip` = `%CLIP` (clipboard do Android)
     - Action: Net > Bluetooth LE > Write characteristic
       - Service UUID: o mesmo
       - Characteristic UUID: `b1c2d3e4-f5a6-4b7c-8d9e-0f1a2b3c4d61` (from_mobile)
       - Value: `%clip` em UTF-8

3. **Profile: BLE notification em to_mobile**
   - Quando `%to_mobile_value` mudar:
     - Action: System > Set Clipboard > Text: `%to_mobile_value`

**Atencao:** a action de BLE no Tasker e instavel em algumas ROMs. Se sofrer
muito, considere escrever um app Android nativo - ~150 linhas de Kotlin com
`BluetoothLeScanner` + `BluetoothGatt` e bem suficiente.

## Comandos do protocolo

| Codigo | Char    | Significado                          |
|--------|---------|--------------------------------------|
| 0x01   | cmd     | "PC, envie sua clipboard"            |
| 0x02   | cmd     | "Celular, envie sua clipboard"       |

Fluxo PC -> Celular:
1. Toque "PC" + "ENVIAR" no display
2. ESP32 NOTIFY cmd=0x01
3. Python le clipboard, WRITE em from_pc
4. ESP32 callback NOTIFY to_mobile com o mesmo payload
5. Tasker recebe, escreve em `%CLIP`

## Limitacoes a corrigir antes de chamar de "pronto"

- **Sem chunking:** payloads > ~500 bytes (ou ~180 no macOS) sao truncados
  silenciosamente. Para v2, adicione um header binario com `seq, total, len`
  e reassemble no ESP32.
- **Sem autenticacao:** qualquer dispositivo BLE pode conectar e ler. Para
  uso real, ative bonding com PIN no NimBLE e/ou criptografe o payload com
  AES-128 e uma chave compartilhada.
- **Sem feedback de erro pro usuario:** se o cliente nao responder ao cmd,
  o ESP32 fica esperando indefinidamente. Adicione timeout e mostre falha
  na UI.
- **Sem persistencia da clipboard:** o ESP32 nao armazena, so encaminha. Se
  o destinatario estiver offline no momento do ENVIAR, perde-se o evento.
