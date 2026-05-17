/**
 * Stage 3 - TOUCH + feedback no TFT (sem BLE)
 *
 * Objetivo: validar que o XPT2046 esta respondendo, capturar coordenadas
 * brutas e ver que cada toque aparece como um ponto na tela. O mapeamento
 * usa valores genericos - vai estar desalinhado proposital. A calibracao
 * fina vem no Stage 4 (display_test).
 *
 * Como rodar:
 *   pio run -e test_03_touch -t upload
 *   pio device monitor -e test_03_touch
 *
 * O que esperar:
 *   - Tela preta com instrucao "Toque na tela" e um contador de toques.
 *   - Cada toque: ponto ciano onde o dedo esta + linha no serial:
 *       [touch] raw=(1234,2300) z=420 screen=(150,120) total=3
 *   - O ponto ciano provavelmente NAO vai coincidir com o dedo (calibracao
 *     pendente). O que importa por enquanto e:
 *       a) o touch responder
 *       b) coordenadas brutas mudarem com a posicao do dedo
 *
 * Diagnostico por sintoma:
 *   - Nenhum toque detectado, nem no serial:
 *       T_CS, T_CLK, T_DIN, T_DO ou T_IRQ no pino errado.
 *   - Touch disparando sozinho (chuva de pontos sem encostar):
 *       T_IRQ flutuando. Reforce com pull-up externo 10k entre IRQ e 3V3.
 *   - Coordenadas iguais sempre (~0 ou ~4095) independente da posicao:
 *       SPI nao retorna - confira MISO (T_DO -> GPIO 19).
 *   - Z (pressao) sempre = 0:
 *       Toque muito leve, ou xy do touch nao chega - mesma checagem
 *       acima.
 */

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#define TFT_CS_GPIO     15
#define TOUCH_CS_PIN    33
#define TOUCH_IRQ_PIN   27

static TFT_eSPI tft = TFT_eSPI();
static XPT2046_Touchscreen ts(TOUCH_CS_PIN, TOUCH_IRQ_PIN);

// Mapeamento ainda nao calibrado - valores genericos. Stage 4 corrige.
static const int16_t RAW_X_MIN = 300;
static const int16_t RAW_X_MAX = 3800;
static const int16_t RAW_Y_MIN = 300;
static const int16_t RAW_Y_MAX = 3800;

static uint32_t g_touchCount = 0;
static uint32_t g_lastTouchMs = 0;

static void drawHeader() {
    tft.fillRect(0, 0, tft.width(), 36, TFT_NAVY);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setTextSize(2);
    tft.drawString("Stage 3 - Touch", tft.width() / 2, 4);
    tft.setTextSize(1);
    tft.drawString("Toque na tela - pontos ciano = ok", tft.width() / 2, 22);
}

static void drawCounter() {
    tft.fillRect(0, tft.height() - 16, tft.width(), 16, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    char buf[40];
    snprintf(buf, sizeof(buf), "Toques: %lu", (unsigned long)g_touchCount);
    tft.drawString(buf, 4, tft.height() - 12);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[test_03_touch] boot");

    // CSes em HIGH antes do tft.init() (ver CLAUDE.md).
    pinMode(TFT_CS_GPIO,  OUTPUT); digitalWrite(TFT_CS_GPIO,  HIGH);
    pinMode(TOUCH_CS_PIN, OUTPUT); digitalWrite(TOUCH_CS_PIN, HIGH);

    tft.init();
    tft.setRotation(1);  // 320x240 landscape
    tft.fillScreen(TFT_BLACK);

    ts.begin();
    ts.setRotation(1);
    pinMode(TOUCH_IRQ_PIN, INPUT_PULLUP);  // ts.begin() zera PUR - reabilitar

    drawHeader();
    drawCounter();
    Serial.println("[test_03_touch] pronto, toque na tela");
}

void loop() {
    // Polling simples: tirqTouched libera o SPI quando nao ha toque,
    // touched() confirma com leitura completa.
    if (!ts.tirqTouched() || !ts.touched()) {
        delay(10);
        return;
    }

    // Debounce simples para nao spammar o serial.
    uint32_t now = millis();
    if (now - g_lastTouchMs < 80) {
        delay(5);
        return;
    }
    g_lastTouchMs = now;

    TS_Point p = ts.getPoint();
    int16_t sx = map(p.x, RAW_X_MIN, RAW_X_MAX, 0, tft.width());
    int16_t sy = map(p.y, RAW_Y_MIN, RAW_Y_MAX, 0, tft.height());
    sx = constrain(sx, 0, tft.width()  - 1);
    sy = constrain(sy, 0, tft.height() - 1);

    g_touchCount++;
    Serial.printf("[touch] raw=(%d,%d) z=%d screen=(%d,%d) total=%lu\n",
                  p.x, p.y, p.z, sx, sy, (unsigned long)g_touchCount);

    tft.fillCircle(sx, sy, 3, TFT_CYAN);
    drawCounter();
}
