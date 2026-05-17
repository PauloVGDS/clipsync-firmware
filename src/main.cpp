/**
 * ClipSync - ESP32 + ILI9341 + XPT2046
 *
 * Relay BLE entre PC e celular para sincronizar clipboard sob demanda.
 *
 * Arquitetura:
 *   - ESP32 atua como BLE GATT server.
 *   - PC (Python/bleak) e Android (Tasker ou app proprio) conectam como
 *     centrals BLE simultaneamente.
 *   - Touch UI: dois "checkboxes" (radio na pratica) + botao ENVIAR.
 *
 * Limitacoes conhecidas:
 *   - MTU negociado em 517 -> payload util ~ 500 bytes. Clipboards maiores
 *     SAO TRUNCADOS. Para resolver, implemente chunking (header com seq+total).
 *   - Sem reconexao automatica do lado dos clientes: eles precisam tratar
 *     desconexao e reconectar.
 *   - Sem criptografia da camada de aplicacao. BLE bonding ajuda contra
 *     eavesdropping casual, mas para producao adicione AES no payload.
 */

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <NimBLEDevice.h>

// =============================================================================
// PINAGEM DO TOUCH (ILI9341 sai pela config do TFT_eSPI em User_Setup.h)
// =============================================================================
// Ajuste conforme sua placa. O XPT2046 compartilha o barramento SPI com o TFT,
// so muda o CS. IRQ e opcional mas recomendado (libera o loop quando nao ha
// toque ao inves de ficar pollando SPI).
#define TOUCH_CS_PIN   33
// IRQ em GPIO bidirecional para podermos habilitar INPUT_PULLUP por software.
// GPIO 34-39 sao input-only e nao tem pull-up interno -> exigiriam resistor
// externo. GPIO 27 (ou 25, 26, 32) resolve sem hardware adicional.
#define TOUCH_IRQ_PIN  27

// =============================================================================
// CALIBRACAO DO TOUCH (transformacao AFIM)
// =============================================================================
// O painel touch desta bancada tem ~12 graus de rotacao fisica em relacao
// ao display - mapeamento linear por eixo (min/max) nao corrige isso, faz
// o trace virar losango. Por isso usamos afim:
//
//   screen_x = AX*raw_x + BX*raw_y + CX
//   screen_y = AY*raw_x + BY*raw_y + CY
//
// Os 6 coeficientes vem do display_test Stage 4 (minimos quadrados sobre
// 4 alvos). Reverse-check < 4 px em todos os cantos.
//
// Para recalibrar: `pio run -e display_test -t upload`, anote os defines
// impressos no serial e cole aqui.
#define TOUCH_CAL_AX   0.112955f
#define TOUCH_CAL_BX   0.023929f
#define TOUCH_CAL_CX  -108.791977f
#define TOUCH_CAL_AY   0.020522f
#define TOUCH_CAL_BY   0.088083f
#define TOUCH_CAL_CY  -101.225655f

// Pressao minima para considerar toque real. Filtra leituras espurias do
// XPT2046 (~z=300) causadas pela SPI compartilhada com o TFT.
#define TOUCH_Z_THRESHOLD  600

// =============================================================================
// UUIDs BLE (gere os seus se for distribuir; estes sao validos v4)
// =============================================================================
#define SERVICE_UUID            "b1c2d3e4-f5a6-4b7c-8d9e-0f1a2b3c4d5e"
#define CHAR_CMD_UUID           "b1c2d3e4-f5a6-4b7c-8d9e-0f1a2b3c4d5f"
#define CHAR_FROM_PC_UUID       "b1c2d3e4-f5a6-4b7c-8d9e-0f1a2b3c4d60"
#define CHAR_FROM_MOBILE_UUID   "b1c2d3e4-f5a6-4b7c-8d9e-0f1a2b3c4d61"
#define CHAR_TO_PC_UUID         "b1c2d3e4-f5a6-4b7c-8d9e-0f1a2b3c4d62"
#define CHAR_TO_MOBILE_UUID     "b1c2d3e4-f5a6-4b7c-8d9e-0f1a2b3c4d63"

// Comandos enviados via `cmd` (NOTIFY do ESP32 para os clientes)
static const uint8_t CMD_REQ_PC     = 0x01;  // "PC, me mande sua clipboard"
static const uint8_t CMD_REQ_MOBILE = 0x02;  // "Celular, me mande sua clipboard"

// =============================================================================
// UI - tela 320x240 (rotation 1)
// =============================================================================
struct Rect { int16_t x, y, w, h; };
static const Rect RECT_PC     = {  30,  70, 110, 90 };
static const Rect RECT_MOBILE = { 180,  70, 110, 90 };
// Botao ENVIAR aumentado (era 90,180,140,45) - dedo no painel resistivo
// tende a cair em y=235+ quando o usuario mira no botao do fundo da tela.
static const Rect RECT_SEND   = {  70, 175, 180,  60 };

enum Source : uint8_t { SRC_NONE, SRC_PC, SRC_MOBILE };
static Source g_selected = SRC_NONE;
static String g_status = "Aguardando...";

// Flag setado por callbacks BLE (task do host NimBLE) e consumido pelo
// loop() na task do Arduino. NUNCA desenhe TFT dentro de callback BLE -
// SPI compartilhado + stack apertado da task NimBLE 2.x causam crash.
static volatile bool g_uiDirty = false;

// =============================================================================
// Globais de hardware
// =============================================================================
static TFT_eSPI tft = TFT_eSPI();
static XPT2046_Touchscreen ts(TOUCH_CS_PIN, TOUCH_IRQ_PIN);

static NimBLECharacteristic *charCmd        = nullptr;
static NimBLECharacteristic *charFromPc     = nullptr;
static NimBLECharacteristic *charFromMobile = nullptr;
static NimBLECharacteristic *charToPc       = nullptr;
static NimBLECharacteristic *charToMobile   = nullptr;

static uint8_t g_connCount = 0;

// =============================================================================
// Prototipos
// =============================================================================
void drawUI();
void drawCheckbox(const Rect& r, const char* label, bool checked);
void drawButton(const Rect& r, const char* label, bool enabled);
void handleTouch();
bool inRect(int16_t x, int16_t y, const Rect& r);
void triggerSend();
void flashStatus(const char* msg, uint16_t color, uint16_t durationMs);

// =============================================================================
// Callbacks BLE
// =============================================================================
class ServerCb : public NimBLEServerCallbacks {
    // NimBLE 2.x: assinatura recebe NimBLEConnInfo em vez de ble_gap_conn_desc*.
    void onConnect(NimBLEServer* s, NimBLEConnInfo& /*connInfo*/) override {
        g_connCount = s->getConnectedCount();
        Serial.printf("[BLE] Connected, total=%u\n", g_connCount);
        // Re-anuncia pra aceitar o outro cliente
        NimBLEDevice::startAdvertising();
        g_uiDirty = true;
    }
    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& /*connInfo*/, int reason) override {
        g_connCount = s->getConnectedCount();
        Serial.printf("[BLE] Disconnected (reason=%d), total=%u\n", reason, g_connCount);
        NimBLEDevice::startAdvertising();
        g_uiDirty = true;
    }
};

class WriteCb : public NimBLECharacteristicCallbacks {
    // NimBLE 2.x: onWrite tambem recebe NimBLEConnInfo.
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*connInfo*/) override {
        const std::string uuid = c->getUUID().toString();
        const std::string value = c->getValue();
        Serial.printf("[BLE] Write em %s, %u bytes\n", uuid.c_str(),
                      (unsigned)value.size());

        if (value.empty()) return;

        if (uuid == CHAR_FROM_PC_UUID) {
            // Veio do PC -> encaminha pro celular
            charToMobile->setValue((uint8_t*)value.data(), value.size());
            charToMobile->notify();
            g_status = "PC -> Celular OK";
        } else if (uuid == CHAR_FROM_MOBILE_UUID) {
            // Veio do celular -> encaminha pro PC
            charToPc->setValue((uint8_t*)value.data(), value.size());
            charToPc->notify();
            g_status = "Celular -> PC OK";
        }
        g_uiDirty = true;
    }
};

// =============================================================================
// Setup
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[ClipSync] boot");

    // --- CS dos dois chips em HIGH ANTES do tft.init():
    // sem isso, o CS do touch pode estar em LOW quando o ILI9341 esta
    // recebendo o init-sequence, e o XPT2046 "rouba" os bytes do SPI.
    // GPIO 15 (TFT_CS) tambem e strapping pin - forcar HIGH cedo ajuda
    // o boot.
    pinMode(15, OUTPUT); digitalWrite(15, HIGH);          // TFT_CS
    pinMode(TOUCH_CS_PIN, OUTPUT); digitalWrite(TOUCH_CS_PIN, HIGH);  // T_CS

    // --- TFT
    tft.init();
    tft.setRotation(1);  // 320x240 landscape
    tft.fillScreen(TFT_BLACK);

    // --- Touch (compartilha SPI com o TFT; TFT_eSPI ja inicializou o bus)
    ts.begin();
    ts.setRotation(1);
    // ts.begin() faz pinMode(IRQ, INPUT) e zera pull-up. Reabilitar aqui.
    pinMode(TOUCH_IRQ_PIN, INPUT_PULLUP);

    // --- BLE
    NimBLEDevice::init("ClipSync");
    NimBLEDevice::setMTU(517);
    NimBLEDevice::setPower(ESP_PWR_LVL_P7);  // potencia maxima

    NimBLEServer* server = NimBLEDevice::createServer();
    static ServerCb scb;
    server->setCallbacks(&scb);

    NimBLEService* svc = server->createService(SERVICE_UUID);

    charCmd = svc->createCharacteristic(
        CHAR_CMD_UUID, NIMBLE_PROPERTY::NOTIFY);
    charFromPc = svc->createCharacteristic(
        CHAR_FROM_PC_UUID, NIMBLE_PROPERTY::WRITE);
    charFromMobile = svc->createCharacteristic(
        CHAR_FROM_MOBILE_UUID, NIMBLE_PROPERTY::WRITE);
    charToPc = svc->createCharacteristic(
        CHAR_TO_PC_UUID, NIMBLE_PROPERTY::NOTIFY);
    charToMobile = svc->createCharacteristic(
        CHAR_TO_MOBILE_UUID, NIMBLE_PROPERTY::NOTIFY);

    static WriteCb wcb;
    charFromPc->setCallbacks(&wcb);
    charFromMobile->setCallbacks(&wcb);

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    // NimBLE 2.x: scan response e gerenciado automaticamente via
    // setScanResponseData(...) se houver payload extra. Sem isso, segue
    // com advertising padrao.
    adv->start();
    Serial.println("[BLE] advertising started");

    drawUI();
}

// =============================================================================
// Loop
// =============================================================================
void loop() {
    handleTouch();
    if (g_uiDirty) {
        g_uiDirty = false;
        drawUI();
    }
    delay(15);
}

// =============================================================================
// Touch handling
// =============================================================================
bool inRect(int16_t x, int16_t y, const Rect& r) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

void handleTouch() {
    static unsigned long lastEvent = 0;
    if (!ts.tirqTouched() || !ts.touched()) return;
    if (millis() - lastEvent < 250) return;  // debounce

    TS_Point p = ts.getPoint();
    if (p.z < TOUCH_Z_THRESHOLD) return;  // rejeita leitura espuria

    // Afim: combina rx e ry para corrigir rotacao fisica do painel.
    float fsx = TOUCH_CAL_AX * p.x + TOUCH_CAL_BX * p.y + TOUCH_CAL_CX;
    float fsy = TOUCH_CAL_AY * p.x + TOUCH_CAL_BY * p.y + TOUCH_CAL_CY;
    int16_t x = constrain((int16_t)lroundf(fsx), 0, 319);
    int16_t y = constrain((int16_t)lroundf(fsy), 0, 239);

    Serial.printf("[Touch] raw=(%d,%d) z=%d screen=(%d,%d)\n", p.x, p.y, p.z, x, y);

    if (inRect(x, y, RECT_PC)) {
        g_selected = (g_selected == SRC_PC) ? SRC_NONE : SRC_PC;
        drawUI();
    } else if (inRect(x, y, RECT_MOBILE)) {
        g_selected = (g_selected == SRC_MOBILE) ? SRC_NONE : SRC_MOBILE;
        drawUI();
    } else if (inRect(x, y, RECT_SEND) && g_selected != SRC_NONE) {
        triggerSend();
    }
    lastEvent = millis();
}

void triggerSend() {
    if (g_connCount == 0) {
        flashStatus("Sem clientes BLE!", TFT_RED, 1200);
        drawUI();
        return;
    }
    uint8_t cmd = (g_selected == SRC_PC) ? CMD_REQ_PC : CMD_REQ_MOBILE;
    charCmd->setValue(&cmd, 1);
    charCmd->notify();

    flashStatus("Enviando...", TFT_GREEN, 600);
    g_selected = SRC_NONE;
    drawUI();
}

// =============================================================================
// UI rendering
// =============================================================================
void drawUI() {
    tft.fillScreen(TFT_BLACK);

    // Title bar
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextSize(2);
    tft.drawString("ClipSync", 160, 5);

    // Status line
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    char buf[40];
    snprintf(buf, sizeof(buf), "Clientes BLE: %u   %s",
             g_connCount, g_status.c_str());
    tft.drawString(buf, 10, 35);

    drawCheckbox(RECT_PC,     "PC",      g_selected == SRC_PC);
    drawCheckbox(RECT_MOBILE, "Celular", g_selected == SRC_MOBILE);
    drawButton  (RECT_SEND,   "ENVIAR",  g_selected != SRC_NONE);
}

void drawCheckbox(const Rect& r, const char* label, bool checked) {
    uint16_t bg = checked ? TFT_DARKGREEN : tft.color565(40, 40, 50);
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 10, bg);
    tft.drawRoundRect(r.x, r.y, r.w, r.h, 10, TFT_WHITE);

    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.drawString(label, r.x + r.w / 2, r.y + r.h / 2);

    if (checked) {
        tft.fillCircle(r.x + r.w - 14, r.y + 14, 7, TFT_WHITE);
        tft.fillCircle(r.x + r.w - 14, r.y + 14, 4, TFT_DARKGREEN);
    }
}

void drawButton(const Rect& r, const char* label, bool enabled) {
    uint16_t bg = enabled ? TFT_BLUE : tft.color565(50, 50, 50);
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 10, bg);
    tft.setTextColor(enabled ? TFT_WHITE : TFT_DARKGREY);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.drawString(label, r.x + r.w / 2, r.y + r.h / 2);
}

void flashStatus(const char* msg, uint16_t color, uint16_t durationMs) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(color);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(3);
    tft.drawString(msg, 160, 120);
    delay(durationMs);
}
