/**
 * display_test - smoke test do ILI9341 + XPT2046 e calibracao do touch.
 *
 * Como rodar:
 *   pio run -e display_test -t upload && pio device monitor -e display_test
 *
 * Fluxo:
 *   1) Smoke test do TFT: preenche tela com cores RGBW, desenha primitivas
 *      e fontes. Se algo aqui aparecer torto/com cor errada, e pinagem ou
 *      driver do platformio.ini.
 *   2) Calibracao do touch: desenha um crosshair em cada um dos 4 cantos,
 *      um de cada vez. Toque firme no centro de cada crosshair. O firmware
 *      registra min/max em x e y brutos e ao final imprime no serial e
 *      mostra na tela os defines prontos para colar no main.cpp.
 *
 * Sem dependencia de BLE -> compila e roda mesmo sem o NimBLE inicializado.
 */

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#define TOUCH_CS_PIN   33
// Ver nota em main.cpp: GPIO bidirecional para podermos usar INPUT_PULLUP.
#define TOUCH_IRQ_PIN  27

static TFT_eSPI tft = TFT_eSPI();
static XPT2046_Touchscreen ts(TOUCH_CS_PIN, TOUCH_IRQ_PIN);

// Forward decl - usado por waitTouchAndCapture antes de sua definicao.
static void drawCrosshair(int16_t x, int16_t y, uint16_t color);

// ---- helpers ----------------------------------------------------------------
// O XPT2046 retorna leituras espurias quando a SPI do TFT chaveia o bus
// compartilhado - assinatura observada: X presa em 13-50, Y em 3400-3900,
// **z entre 300 e 330**. Toques reais dao z >= 700. Threshold de 600
// separa com folga.
static const int16_t Z_THRESHOLD = 600;

static bool sampleTouch(int16_t& rx, int16_t& ry, int16_t& rz) {
    if (!ts.tirqTouched()) return false;
    if (!ts.touched())     return false;
    TS_Point p = ts.getPoint();
    if (p.z < Z_THRESHOLD) return false;
    rx = p.x; ry = p.y; rz = p.z;
    return true;
}

static void waitNoTouch() {
    // Sai apos 200 ms consecutivos sem leitura valida.
    uint32_t lastValid = millis();
    while (millis() - lastValid < 200) {
        int16_t rx, ry, rz;
        if (sampleTouch(rx, ry, rz)) lastValid = millis();
        delay(10);
    }
    delay(100);  // colchao adicional
}

// Coleta N amostras consecutivas validas, calcula media, e REJEITA se o
// spread (max-min) for grande em qualquer eixo (dedo escorregou). Quando
// rejeita, espera o usuario soltar o dedo e refaz o alvo do zero.
// As 3 primeiras amostras sao descartadas (warm-up do dedo aterrissando -
// pressao crescente leva a coordenadas transitorias). As 5 seguintes sao
// usadas para spread/media.
static const int     WARMUP_SAMPLES   = 3;
static const int     SAMPLES_PER_ALVO = 5;
static const int     TOTAL_SAMPLES    = WARMUP_SAMPLES + SAMPLES_PER_ALVO;
static const int16_t MAX_SPREAD       = 150;

static bool tryCaptureAlvo(int16_t& rawX, int16_t& rawY) {
    int16_t xs[SAMPLES_PER_ALVO], ys[SAMPLES_PER_ALVO];
    int idx = 0;
    while (idx < TOTAL_SAMPLES) {
        int16_t rx, ry, rz;
        if (sampleTouch(rx, ry, rz)) {
            if (idx < WARMUP_SAMPLES) {
                Serial.printf("[Touch] warmup %d/%d raw=(%d,%d) z=%d\n",
                              idx + 1, WARMUP_SAMPLES, rx, ry, rz);
            } else {
                int useIdx = idx - WARMUP_SAMPLES;
                xs[useIdx] = rx; ys[useIdx] = ry;
                Serial.printf("[Touch] amostra %d/%d raw=(%d,%d) z=%d\n",
                              useIdx + 1, SAMPLES_PER_ALVO, rx, ry, rz);
            }
            idx++;
            delay(25);
        } else {
            delay(5);
        }
    }
    int16_t xMin = xs[0], xMax = xs[0], yMin = ys[0], yMax = ys[0];
    long sx = 0, sy = 0;
    for (int i = 0; i < SAMPLES_PER_ALVO; ++i) {
        if (xs[i] < xMin) xMin = xs[i];
        if (xs[i] > xMax) xMax = xs[i];
        if (ys[i] < yMin) yMin = ys[i];
        if (ys[i] > yMax) yMax = ys[i];
        sx += xs[i]; sy += ys[i];
    }
    int16_t spreadX = xMax - xMin;
    int16_t spreadY = yMax - yMin;
    if (spreadX > MAX_SPREAD || spreadY > MAX_SPREAD) {
        Serial.printf("[Touch] REJEITADO spread X=%d Y=%d (>%d). Refazendo...\n",
                      spreadX, spreadY, MAX_SPREAD);
        return false;
    }
    rawX = sx / SAMPLES_PER_ALVO;
    rawY = sy / SAMPLES_PER_ALVO;
    Serial.printf("[Touch] MEDIA raw=(%d,%d) spread=(%d,%d)\n",
                  rawX, rawY, spreadX, spreadY);
    return true;
}

// Captura uma vez; se a captura for instavel, redesenha o alvo (em
// amarelo, sinalizando "refaz") e tenta de novo. Bloqueia ate conseguir
// uma captura estavel.
static void waitTouchAndCapture(int16_t cx, int16_t cy, int16_t& rawX, int16_t& rawY) {
    while (true) {
        if (tryCaptureAlvo(rawX, rawY)) return;
        // Sinaliza rejeicao na tela e espera dedo sair.
        drawCrosshair(cx, cy, TFT_ORANGE);
        waitNoTouch();
        drawCrosshair(cx, cy, TFT_GREEN);
    }
}

static void drawCrosshair(int16_t x, int16_t y, uint16_t color) {
    // Alvo: braco da cruz + anel + DOT central grande (radius 5) para
    // remover qualquer ambiguidade sobre onde pressionar.
    tft.drawLine(x - 25, y, x - 8, y, color);
    tft.drawLine(x + 8,  y, x + 25, y, color);
    tft.drawLine(x, y - 25, x, y - 8, color);
    tft.drawLine(x, y + 8,  x, y + 25, color);
    tft.drawCircle(x, y, 15, color);
    tft.fillCircle(x, y, 5,  color);   // ponto central grande
}

static void banner(const char* msg, uint16_t color) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(color, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.drawString(msg, tft.width() / 2, tft.height() / 2);
}

// ---- smoke test do display --------------------------------------------------
static void smokeTest() {
    Serial.println("[smoke] fill colors");
    const uint16_t colors[] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE, TFT_BLACK };
    const char*    names[]  = { "RED", "GREEN", "BLUE", "WHITE", "BLACK" };
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        tft.fillScreen(colors[i]);
        tft.setTextDatum(MC_DATUM);
        tft.setTextSize(3);
        tft.setTextColor(colors[i] == TFT_WHITE ? TFT_BLACK : TFT_WHITE);
        tft.drawString(names[i], tft.width() / 2, tft.height() / 2);
        delay(600);
    }

    Serial.println("[smoke] primitives");
    tft.fillScreen(TFT_BLACK);
    tft.drawRect(0, 0, tft.width(), tft.height(), TFT_WHITE);
    tft.drawLine(0, 0, tft.width() - 1, tft.height() - 1, TFT_CYAN);
    tft.drawLine(tft.width() - 1, 0, 0, tft.height() - 1, TFT_MAGENTA);
    tft.fillCircle(tft.width() / 2, tft.height() / 2, 30, TFT_YELLOW);
    tft.drawRoundRect(20, 20, 60, 40, 8, TFT_GREEN);
    delay(1500);

    Serial.println("[smoke] fonts");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(1); tft.drawString("size=1 abc 123", 10,  10);
    tft.setTextSize(2); tft.drawString("size=2 abc 123", 10,  30);
    tft.setTextSize(3); tft.drawString("size=3 abc",     10,  60);
    tft.setTextSize(4); tft.drawString("size=4",         10, 100);
    delay(1800);
}

// ---- calibracao -------------------------------------------------------------
struct CalPoint { int16_t screenX, screenY; const char* label; };

// Alvos com inset de 40px (em vez de 20px) - mais faceis de pressionar
// com precisao, longe do bezel do display. A extrapolacao linear cuida
// do mapeamento ate os cantos absolutos.
static const CalPoint CAL_POINTS[] = {
    {  40,  40, "1/4 sup-esq" },
    { 280,  40, "2/4 sup-dir" },
    {  40, 200, "3/4 inf-esq" },
    { 280, 200, "4/4 inf-dir" },
};

// Resultado da calibracao - transformacao afim sobre o raw do XPT2046.
// Mapeamento linear por eixo (g_xMin/Max apenas) NAO consegue corrigir
// rotacao/skew fisicos do painel touch em relacao ao display. Por isso
// usamos afim: screen = a*rx + b*ry + c, com 6 coeficientes calculados
// por minimos quadrados sobre os 4 alvos.
struct AffineCoef { float a, b, c; };
static AffineCoef g_calX = { 0.f, 0.f, 0.f };
static AffineCoef g_calY = { 0.f, 0.f, 0.f };

// Resolve [a,b,c] tal que minimiza sum((s_i - (a*rx_i + b*ry_i + c))^2)
// para n pontos. Equacoes normais 3x3 resolvidas por Cramer.
static bool solveAffine(int n, const int16_t* rx, const int16_t* ry,
                        const int16_t* s, AffineCoef& out) {
    double Srx = 0, Sry = 0, Ss = 0;
    double Srx2 = 0, Sry2 = 0, Srxry = 0;
    double Srxs = 0, Srys = 0;
    for (int i = 0; i < n; ++i) {
        Srx   += rx[i];
        Sry   += ry[i];
        Ss    += s[i];
        Srx2  += (double)rx[i] * rx[i];
        Sry2  += (double)ry[i] * ry[i];
        Srxry += (double)rx[i] * ry[i];
        Srxs  += (double)rx[i] * s[i];
        Srys  += (double)ry[i] * s[i];
    }
    // M (simetrica):     RHS:
    // [Srx2  Srxry  Srx] [Srxs]
    // [Srxry Sry2   Sry] [Srys]
    // [Srx   Sry    n  ] [Ss  ]
    double det =
        Srx2 *(Sry2*n  - Sry*Sry)
      - Srxry*(Srxry*n - Sry*Srx)
      + Srx  *(Srxry*Sry - Sry2*Srx);
    if (det == 0.0) return false;

    double dA =
        Srxs*(Sry2*n  - Sry*Sry)
      - Srxry*(Srys*n - Sry*Ss)
      + Srx *(Srys*Sry - Sry2*Ss);
    double dB =
        Srx2*(Srys*n  - Sry*Ss)
      - Srxs*(Srxry*n - Sry*Srx)
      + Srx *(Srxry*Ss - Srys*Srx);
    double dC =
        Srx2 *(Sry2*Ss - Srys*Sry)
      - Srxry*(Srxry*Ss - Srys*Srx)
      + Srxs *(Srxry*Sry - Sry2*Srx);
    out.a = (float)(dA / det);
    out.b = (float)(dB / det);
    out.c = (float)(dC / det);
    return true;
}

static void calibrate() {
    Serial.println("[cal] iniciando calibracao");

    // Captura raw de cada um dos 4 alvos (com rejeicao automatica de
    // amostras instaveis em tryCaptureAlvo).
    int16_t rxs[4], rys[4];

    for (size_t i = 0; i < 4; ++i) {
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextDatum(TC_DATUM);
        tft.setTextSize(2);
        tft.drawString("Toque no alvo", tft.width() / 2, 90);
        tft.setTextSize(1);
        tft.drawString(CAL_POINTS[i].label, tft.width() / 2, 120);

        drawCrosshair(CAL_POINTS[i].screenX, CAL_POINTS[i].screenY, TFT_GREEN);

        waitNoTouch();
        waitTouchAndCapture(CAL_POINTS[i].screenX, CAL_POINTS[i].screenY,
                            rxs[i], rys[i]);
        Serial.printf("[cal] alvo %u (%d,%d) -> raw (%d,%d)\n",
                      (unsigned)i, CAL_POINTS[i].screenX, CAL_POINTS[i].screenY,
                      rxs[i], rys[i]);

        drawCrosshair(CAL_POINTS[i].screenX, CAL_POINTS[i].screenY, TFT_RED);
        delay(300);
    }

    // ---- Transformacao AFIM via minimos quadrados sobre os 4 alvos --------
    // Linear-por-eixo nao corrige rotacao/skew do painel. Afim sim:
    //   screen_x = aX*raw_x + bX*raw_y + cX
    //   screen_y = aY*raw_x + bY*raw_y + cY
    int16_t sxs[4] = {
        (int16_t)CAL_POINTS[0].screenX, (int16_t)CAL_POINTS[1].screenX,
        (int16_t)CAL_POINTS[2].screenX, (int16_t)CAL_POINTS[3].screenX
    };
    int16_t sys[4] = {
        (int16_t)CAL_POINTS[0].screenY, (int16_t)CAL_POINTS[1].screenY,
        (int16_t)CAL_POINTS[2].screenY, (int16_t)CAL_POINTS[3].screenY
    };

    if (!solveAffine(4, rxs, rys, sxs, g_calX) ||
        !solveAffine(4, rxs, rys, sys, g_calY)) {
        Serial.println("[cal] ERRO: nao foi possivel resolver afim (det=0)");
    }

    Serial.println("================ CALIBRACAO AFIM ================");
    Serial.printf("screen_x = %.6f*rx + %.6f*ry + %.3f\n",
                  g_calX.a, g_calX.b, g_calX.c);
    Serial.printf("screen_y = %.6f*rx + %.6f*ry + %.3f\n",
                  g_calY.a, g_calY.b, g_calY.c);
    Serial.println("");
    Serial.println("Para main.cpp:");
    Serial.printf("#define TOUCH_CAL_AX  %.6ff\n", g_calX.a);
    Serial.printf("#define TOUCH_CAL_BX  %.6ff\n", g_calX.b);
    Serial.printf("#define TOUCH_CAL_CX  %.6ff\n", g_calX.c);
    Serial.printf("#define TOUCH_CAL_AY  %.6ff\n", g_calY.a);
    Serial.printf("#define TOUCH_CAL_BY  %.6ff\n", g_calY.b);
    Serial.printf("#define TOUCH_CAL_CY  %.6ff\n", g_calY.c);
    Serial.println("");

    // Reverse-check: aplicar o afim sobre cada raw capturado, conferir erro
    // em pixels vs o alvo esperado.
    Serial.println("Erro afim em cada alvo (px):");
    for (int i = 0; i < 4; ++i) {
        float esx = g_calX.a * rxs[i] + g_calX.b * rys[i] + g_calX.c;
        float esy = g_calY.a * rxs[i] + g_calY.b * rys[i] + g_calY.c;
        Serial.printf("  alvo %d: esperado (%d,%d) -> afim (%.1f,%.1f)  erro=(%.1f,%.1f)\n",
                      i, sxs[i], sys[i], esx, esy, esx - sxs[i], esy - sys[i]);
    }
    Serial.println("==================================================");

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(1);
    int16_t y = 10;
    tft.drawString("Cal AFIM concluida.", 10, y); y += 14;
    tft.drawString("Veja serial para coeficientes.", 10, y); y += 18;
    char buf[64];
    snprintf(buf, sizeof(buf), "AX=%.4f BX=%.4f", g_calX.a, g_calX.b);
    tft.drawString(buf, 10, y); y += 12;
    snprintf(buf, sizeof(buf), "CX=%.2f", g_calX.c);
    tft.drawString(buf, 10, y); y += 14;
    snprintf(buf, sizeof(buf), "AY=%.4f BY=%.4f", g_calY.a, g_calY.b);
    tft.drawString(buf, 10, y); y += 12;
    snprintf(buf, sizeof(buf), "CY=%.2f", g_calY.c);
    tft.drawString(buf, 10, y); y += 18;
    tft.setTextColor(TFT_WHITE);
    tft.drawString("Toque para testar mapeamento.", 10, y);
    delay(500);
    waitNoTouch();
}

// ---- modo de verificacao apos calibrar --------------------------------------
// Le o touch e desenha um ponto onde o usuario tocou, usando o mapeamento
// derivado dos extremos coletados. Util para confirmar que os min/max estao
// fechando os 4 cantos certinhos. Se o ponto desenhado nao corresponde ao
// dedo, pode ser necessario trocar X<->Y ou inverter um eixo.
static void verifyLoop() {
    int16_t rx, ry, rz;
    if (!sampleTouch(rx, ry, rz)) return;

    float fsx = g_calX.a * rx + g_calX.b * ry + g_calX.c;
    float fsy = g_calY.a * rx + g_calY.b * ry + g_calY.c;
    int16_t sx = constrain((int16_t)lroundf(fsx), 0, tft.width()  - 1);
    int16_t sy = constrain((int16_t)lroundf(fsy), 0, tft.height() - 1);

    // Marcador BEM visivel: circulo amarelo + cruz vermelha por cima.
    tft.fillCircle(sx, sy, 6, TFT_YELLOW);
    tft.drawLine(sx - 8, sy, sx + 8, sy, TFT_RED);
    tft.drawLine(sx, sy - 8, sx, sy + 8, TFT_RED);

    Serial.printf("[verify] raw=(%d,%d) z=%d screen=(%d,%d)\n",
                  rx, ry, rz, sx, sy);
    delay(30);
}

// ---- entrypoints ------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[display_test] boot");

    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);
    pinMode(33, OUTPUT);
    digitalWrite(33, HIGH);

    tft.init();
    tft.setRotation(1);  // landscape 320x240, igual ao main.cpp
    tft.fillScreen(TFT_BLACK);

    // TFT_eSPI ja inicializa o SPI internamente; chamar SPI.begin() de novo
    // gera "[E] addApbChangeCallback(): duplicate". XPT2046_Touchscreen.begin()
    // tambem reaproveita o barramento existente.
    ts.begin();
    ts.setRotation(1);
    pinMode(TOUCH_IRQ_PIN, INPUT_PULLUP);  // re-habilita PUR apos ts.begin()

    banner("Smoke test...", TFT_WHITE);
    delay(600);
    smokeTest();

    banner("Calibracao do touch", TFT_YELLOW);
    delay(900);
    calibrate();

    banner("Verifique: toque na tela", TFT_GREEN);
    delay(800);
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.drawString("Verificacao: pontos onde voce tocar.", 10, 10);
    tft.drawString("Reset para repetir.", 10, 24);
}

void loop() {
    verifyLoop();
    delay(10);
}
