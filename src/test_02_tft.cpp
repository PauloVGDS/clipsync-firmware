/**
 * Stage 2 - TFT ONLY (sem touch, sem BLE)
 *
 * Objetivo: validar que o ILI9341 esta recebendo SPI e respondendo.
 * Sem a lib XPT2046_Touchscreen no projeto - se ainda assim a tela
 * ficar branca, e 100% pinagem ou hardware.
 *
 * Como rodar:
 *   pio run -e test_02_tft -t upload
 *   pio device monitor -e test_02_tft
 *
 * O que esperar:
 *   - Backlight aceso (vem do Stage 1).
 *   - Tela cicla cores: VERMELHO -> VERDE -> AZUL -> BRANCO -> PRETO
 *   - Texto grande com o nome da cor no centro.
 *   - Cruz X cyan/magenta + circulo amarelo + retangulo verde.
 *   - Repete tudo em loop ate voce desligar.
 *
 * Diagnostico por sintoma:
 *   - Tela continua BRANCA: pinagem (DC, CS, RST, MOSI, SCK).
 *     Faca o teste manual: encoste RST em GND, deve piscar.
 *   - Cores TROCADAS (vermelho aparece azul): trocar
 *     -DTFT_RGB_ORDER=TFT_BGR para TFT_RGB no platformio.ini.
 *   - Cores INVERTIDAS (branco vira preto): negativo. Acrescentar
 *     -DTFT_INVERSION_ON ou -DTFT_INVERSION_OFF.
 *   - Imagem ESPELHADA / rodada: ajustar tft.setRotation(0..3).
 *   - LIXO / linhas aleatorias: integridade SPI. Manter SPI_FREQUENCY
 *     baixa (ja em 10 MHz) e encurtar jumpers.
 */

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

// CSes para forcar HIGH antes do init (ver CLAUDE.md, "Sequencia obrigatoria
// de boot"). Mesmo sem o XPT2046 fisicamente conectado, deixar o T_CS em
// HIGH e seguro - so vai impedir leituras parasitas do touch caso o pino
// 33 esteja flutuando.
#define TFT_CS_GPIO     15
#define TOUCH_CS_GPIO   33

static TFT_eSPI tft = TFT_eSPI();

static void colorStep(uint16_t bg, uint16_t fg, const char* name) {
    tft.fillScreen(bg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(3);
    tft.setTextColor(fg);
    tft.drawString(name, tft.width() / 2, tft.height() / 2);
    Serial.printf("[tft] color %s\n", name);
    delay(800);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[test_02_tft] boot");

    // CS em HIGH antes do init (idle SPI).
    pinMode(TFT_CS_GPIO,   OUTPUT); digitalWrite(TFT_CS_GPIO,   HIGH);
    pinMode(TOUCH_CS_GPIO, OUTPUT); digitalWrite(TOUCH_CS_GPIO, HIGH);

    tft.init();
    tft.setRotation(1);  // 320x240 landscape
    Serial.println("[tft] init ok");
}

void loop() {
    colorStep(TFT_RED,   TFT_WHITE, "VERMELHO");
    colorStep(TFT_GREEN, TFT_BLACK, "VERDE");
    colorStep(TFT_BLUE,  TFT_WHITE, "AZUL");
    colorStep(TFT_WHITE, TFT_BLACK, "BRANCO");
    colorStep(TFT_BLACK, TFT_WHITE, "PRETO");

    Serial.println("[tft] primitives");
    tft.fillScreen(TFT_BLACK);
    tft.drawRect(0, 0, tft.width(), tft.height(), TFT_WHITE);
    tft.drawLine(0, 0, tft.width() - 1, tft.height() - 1, TFT_CYAN);
    tft.drawLine(tft.width() - 1, 0, 0, tft.height() - 1, TFT_MAGENTA);
    tft.fillCircle(tft.width() / 2, tft.height() / 2, 35, TFT_YELLOW);
    tft.drawRoundRect(20, 20, 80, 50, 8, TFT_GREEN);
    delay(1500);
}
