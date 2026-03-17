// ============================================================
//  Flabby Bird — Flappy Bird clone for ESP32-S3
//  Display : 2.8" ILI9341 320×240  — Adafruit_ILI9341
//  Touch   : FT6336G (I2C) — tap anywhere to flap
//  Mic     : onboard MEMS  — clap/blow to flap
// ============================================================
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Preferences.h>
#include <esp_idf_version.h>
#include "config.h"
#include "touch.h"
#include "mic.h"

// ── Types ─────────────────────────────────────────────────────────────────────

enum GameState : uint8_t { STATE_SPLASH, STATE_PLAYING, STATE_DEAD };

struct Bird {
    float y   = SCREEN_H / 2.0f;
    float vy  = 0.0f;
    int   frame = 0;

    void reset() { y = SCREEN_H / 2.0f; vy = 0.0f; frame = 0; }
    void update() {
        vy += GRAVITY;
        if (vy > MAX_FALL_VEL) vy = MAX_FALL_VEL;
        y += vy;
        frame++;
    }
    void flap() { vy = FLAP_VEL; }
    bool outOfBounds() const {
        return y - BIRD_R < CEILING_Y || y + BIRD_R >= (float)GROUND_Y;
    }
};

struct Pipe {
    float x      = SCREEN_W + 100;
    int   gapY   = 80;
    bool  passed = false;

    void spawn(float startX) {
        x      = startX;
        gapY   = random(40, GROUND_Y - PIPE_GAP - 40);
        passed = false;
    }
    bool hitsBird(float by) const {
        if (BIRD_X + BIRD_HIT_R < (int)x || BIRD_X - BIRD_HIT_R > (int)x + PIPE_W)
            return false;
        return (by - BIRD_HIT_R < gapY) || (by + BIRD_HIT_R > gapY + PIPE_GAP);
    }
};

// ── Globals ───────────────────────────────────────────────────────────────────

// Hardware SPI2 (FSPI on ESP32-S3) — MOSI=11, SCLK=12, MISO=13 are native pins
#ifndef FSPI
#  define FSPI VSPI   // fallback for older ESP32 Arduino cores
#endif
static SPIClass      g_spi(FSPI);
Adafruit_ILI9341     tft(&g_spi, PIN_TFT_DC, PIN_TFT_CS);

// Full-screen 16-bit canvas in PSRAM — double-buffer to avoid flicker
GFXcanvas16*         canvas = nullptr;

Preferences          prefs;
GameState            state     = STATE_SPLASH;
uint32_t             deadSince = 0;
Bird                 bird;
Pipe                 pipes[PIPE_COUNT];
int                  score     = 0;
int                  hiScore   = 0;
float                pipeSpeed = INITIAL_SPEED;

uint16_t             skyGrad[GROUND_Y];   // precomputed gradient
uint32_t             flashTimer = 0;
bool                 flashOn    = true;

struct Cloud { float x; int y, w; };
static Cloud clouds[4] = {
    {  20, 28, 80 }, { 135, 18, 92 }, { 240, 48, 68 }, { 310, 30, 74 },
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint16_t lerpColor(uint16_t c1, uint16_t c2, int t, int tmax) {
    float f = (float)t / tmax;
    int r1=(c1>>11)&0x1F, g1=(c1>>5)&0x3F, b1=c1&0x1F;
    int r2=(c2>>11)&0x1F, g2=(c2>>5)&0x3F, b2=c2&0x1F;
    return (uint16_t)(((int)(r1+(r2-r1)*f)<<11)|((int)(g1+(g2-g1)*f)<<5)|(int)(b1+(b2-b1)*f));
}

// Push the canvas framebuffer to the display in one bulk SPI transfer
static void pushFrame() {
    tft.startWrite();
    tft.setAddrWindow(0, 0, SCREEN_W, SCREEN_H);  // w, h — NOT x2, y2
    tft.writePixels(canvas->getBuffer(), (uint32_t)SCREEN_W * SCREEN_H);
    tft.endWrite();
}

// ── Text helpers (Adafruit_GFX has no setTextDatum / drawString) ──────────────

// Print string with its centre at (cx, cy)
static void printMC(const char* s, int cx, int cy) {
    int16_t x1, y1; uint16_t tw, th;
    canvas->getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
    canvas->setCursor(cx - (int)(tw / 2) - x1, cy - (int)(th / 2) - y1);
    canvas->print(s);
}
// Print integer centred
static void printMC(int n, int cx, int cy) {
    char buf[12]; snprintf(buf, sizeof(buf), "%d", n);
    printMC(buf, cx, cy);
}
// Print string with top-centre anchor at (cx, ty)
static void printTC(const char* s, int cx, int ty) {
    int16_t x1, y1; uint16_t tw, th;
    canvas->getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
    canvas->setCursor(cx - (int)(tw / 2) - x1, ty - y1);
    canvas->print(s);
}

// ── Drawing ───────────────────────────────────────────────────────────────────

static void buildSkyGradient() {
    for (int y = 0; y < GROUND_Y; y++)
        skyGrad[y] = lerpColor(CLR_SKY_TOP, CLR_SKY_BTM, y, GROUND_Y);
}

static void drawBackground() {
    for (int y = 0; y < GROUND_Y; y++)
        canvas->drawFastHLine(0, y, SCREEN_W, skyGrad[y]);

    for (const auto& c : clouds) {
        int cx = (int)c.x;
        canvas->fillCircle(cx + c.w/4,      c.y+14, c.w/4+5, CLR_CLOUD);
        canvas->fillCircle(cx + c.w/2,      c.y+ 8, c.w/3+2, CLR_CLOUD);
        canvas->fillCircle(cx + 3*c.w/4,   c.y+14, c.w/4+4, CLR_CLOUD);
        canvas->fillRect  (cx + c.w/4-5,   c.y+14, c.w/2+10, 12, CLR_CLOUD);
        int cx2 = cx - SCREEN_W;
        if (cx2 + c.w > 0) {
            canvas->fillCircle(cx2 + c.w/4,     c.y+14, c.w/4+5, CLR_CLOUD);
            canvas->fillCircle(cx2 + c.w/2,     c.y+ 8, c.w/3+2, CLR_CLOUD);
            canvas->fillCircle(cx2 + 3*c.w/4,  c.y+14, c.w/4+4, CLR_CLOUD);
            canvas->fillRect  (cx2 + c.w/4-5,  c.y+14, c.w/2+10, 12, CLR_CLOUD);
        }
    }

    canvas->fillRect(0, GROUND_Y,     SCREEN_W, 6,                       CLR_GND_TOP);
    canvas->fillRect(0, GROUND_Y+6,   SCREEN_W, SCREEN_H-GROUND_Y-6,     CLR_GROUND);
    canvas->fillRect(0, GROUND_Y+14,  SCREEN_W, SCREEN_H-GROUND_Y-14,    CLR_DIRT);
}

static void drawPipe(int x, int gapY) {
    const int capH=10, capExt=4, liteW=8, darkW=6;

    if (gapY > 0) {
        int bodyH = gapY - capH;
        if (bodyH > 0) {
            canvas->fillRect(x,                0, PIPE_W,  bodyH, CLR_PIPE);
            canvas->fillRect(x+4,              0, liteW,   bodyH, CLR_PIPE_LIGHT);
            canvas->fillRect(x+PIPE_W-darkW,   0, darkW,   bodyH, CLR_PIPE_DARK);
        }
        canvas->fillRect(x-capExt,              gapY-capH, PIPE_W+capExt*2, capH, CLR_PIPE_CAP);
        canvas->fillRect(x-capExt+4,            gapY-capH, liteW,           capH, CLR_PIPE_LIGHT);
        canvas->fillRect(x-capExt+PIPE_W+capExt-darkW, gapY-capH, darkW,    capH, CLR_PIPE_DARK);
    }

    int botY = gapY + PIPE_GAP;
    if (botY < GROUND_Y) {
        canvas->fillRect(x-capExt,              botY, PIPE_W+capExt*2, capH, CLR_PIPE_CAP);
        canvas->fillRect(x-capExt+4,            botY, liteW,           capH, CLR_PIPE_LIGHT);
        canvas->fillRect(x-capExt+PIPE_W+capExt-darkW, botY, darkW,    capH, CLR_PIPE_DARK);
        int bodyH = GROUND_Y - botY - capH;
        if (bodyH > 0) {
            canvas->fillRect(x,               botY+capH, PIPE_W, bodyH, CLR_PIPE);
            canvas->fillRect(x+4,             botY+capH, liteW,  bodyH, CLR_PIPE_LIGHT);
            canvas->fillRect(x+PIPE_W-darkW,  botY+capH, darkW,  bodyH, CLR_PIPE_DARK);
        }
    }
}

static void drawBird(float by, float vy, int frame) {
    const int bx  = BIRD_X;
    const int iby = (int)by;
    int  tilt   = constrain((int)(vy * 1.1f), -7, 9);
    bool wingUp = (frame / 6) & 1;

    // Shadow
    canvas->fillCircle(bx+3, iby+4, BIRD_R-1, C565(30,50,30));
    // Body
    canvas->fillCircle(bx, iby, BIRD_R, TFT_YELLOW);
    // Wing — fillEllipse not in Adafruit_GFX; use a circle
    canvas->fillCircle(bx-3, wingUp ? iby-5 : iby+6, 5, C565(240,200,60));
    // Eye
    canvas->fillCircle(bx+5, iby-3+tilt/2, 4, TFT_WHITE);
    canvas->fillCircle(bx+6, iby-3+tilt/2, 2, TFT_BLACK);
    canvas->drawPixel (bx+7, iby-4+tilt/2,    TFT_WHITE);
    // Beak
    int bkY = iby + tilt;
    canvas->fillTriangle(bx+9, bkY-1,  bx+17, bkY+1,  bx+9, bkY+4, C565(240,120,20));
    canvas->drawLine    (bx+9, bkY+1,  bx+17, bkY+1,               C565(180, 80,10));
    // Outline
    canvas->drawCircle(bx, iby, BIRD_R, C565(190,140,20));
}

static void drawScore(int s, int cx, int cy) {
    char buf[8]; snprintf(buf, sizeof(buf), "%d", s);
    canvas->setFont(nullptr);
    canvas->setTextSize(3);
    canvas->setTextColor(CLR_SCORE_SHAD);
    printTC(buf, cx+2, cy+2);
    canvas->setTextColor(TFT_WHITE);
    printTC(buf, cx, cy);
}

// ── Screens ───────────────────────────────────────────────────────────────────

static void drawSplash() {
    drawBackground();

    // Title (drop-shadow then gold)
    canvas->setFont(&FreeSansBold18pt7b);
    canvas->setTextSize(1);
    canvas->setTextColor(C565(60,60,60));
    printMC("FLABBY BIRD", SCREEN_W/2+2, 72);
    canvas->setTextColor(C565(250,220,0));
    printMC("FLABBY BIRD", SCREEN_W/2,   70);

    drawBird(SCREEN_H/2.0f - 10, -2.0f, 0);

    if (hiScore > 0) {
        canvas->setFont(nullptr);
        canvas->setTextSize(2);
        canvas->setTextColor(TFT_WHITE);
        printMC("BEST", SCREEN_W/2, 125);
        canvas->setTextSize(3);
        printMC(hiScore, SCREEN_W/2, 148);
    }

    if (flashOn) {
        canvas->setFont(nullptr);
        canvas->setTextSize(1);
        canvas->setTextColor(TFT_WHITE);
        printMC("TAP OR CLAP TO PLAY", SCREEN_W/2, 205);
    }

    pushFrame();
}

static void drawDead() {
    drawBackground();
    for (int i = 0; i < PIPE_COUNT; i++) drawPipe((int)pipes[i].x, pipes[i].gapY);
    drawBird(bird.y, bird.vy, bird.frame);

    canvas->fillRoundRect(SCREEN_W/2-90, 70, 180, 110, 12, C565(0,0,0));
    canvas->drawRoundRect(SCREEN_W/2-90, 70, 180, 110, 12, TFT_WHITE);

    canvas->setFont(nullptr);
    canvas->setTextSize(2);
    canvas->setTextColor(TFT_WHITE);
    printMC("GAME OVER", SCREEN_W/2, 92);

    canvas->setTextSize(1);
    canvas->setTextColor(C565(200,200,200));
    printMC("SCORE", SCREEN_W/2-40, 118);
    printMC("BEST",  SCREEN_W/2+40, 118);

    canvas->setTextSize(3);
    canvas->setTextColor(TFT_WHITE);
    printMC(score,   SCREEN_W/2-40, 140);
    printMC(hiScore, SCREEN_W/2+40, 140);

    if (flashOn) {
        canvas->setTextSize(1);
        canvas->setTextColor(TFT_YELLOW);
        printMC("TAP OR CLAP TO RETRY", SCREEN_W/2, 175);
    }

    pushFrame();
}

// ── Game logic ────────────────────────────────────────────────────────────────

static void saveHiScore() {
    prefs.begin("flappy", false);
    prefs.putInt("hi", hiScore);
    prefs.end();
}

static void resetGame() {
    bird.reset();
    pipeSpeed = INITIAL_SPEED;
    score     = 0;
    for (int i = 0; i < PIPE_COUNT; i++)
        pipes[i].spawn(SCREEN_W + 60 + i * PIPE_SPACING);
}

static bool checkFlap() {
    return touch_was_pressed() || mic_clap_ready();
}

static void updateGame() {
    bird.update();

    for (auto& c : clouds) {
        c.x -= pipeSpeed * 0.4f;
        if (c.x + c.w < 0) c.x += SCREEN_W + c.w;
    }

    for (int i = 0; i < PIPE_COUNT; i++) {
        pipes[i].x -= pipeSpeed;
        if (pipes[i].x + PIPE_W < 0) {
            float maxX = 0;
            for (int j = 0; j < PIPE_COUNT; j++) maxX = max(maxX, pipes[j].x);
            pipes[i].spawn(maxX + PIPE_SPACING);
        }
        if (!pipes[i].passed && pipes[i].x + PIPE_W < BIRD_X) {
            pipes[i].passed = true;
            score++;
            if (score % SPEED_STEP == 0) pipeSpeed += SPEED_INC;
        }
    }

    for (int i = 0; i < PIPE_COUNT; i++) {
        if (pipes[i].hitsBird(bird.y)) {
            if (score > hiScore) { hiScore = score; saveHiScore(); }
            deadSince = millis(); state = STATE_DEAD; return;
        }
    }
    if (bird.outOfBounds()) {
        if (score > hiScore) { hiScore = score; saveHiScore(); }
        deadSince = millis(); state = STATE_DEAD;
    }
}

static void renderGame() {
    drawBackground();
    for (int i = 0; i < PIPE_COUNT; i++) drawPipe((int)pipes[i].x, pipes[i].gapY);
    drawBird(bird.y, bird.vy, bird.frame);
    drawScore(score, SCREEN_W/2, 10);
    pushFrame();
}

// ── Arduino entry points ──────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.printf("[boot] IDF %d.%d.%d  heap %lu B  PSRAM %lu B\n",
        ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH,
        (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());

    // Backlight
    Serial.println("[1] backlight");
    pinMode(PIN_TFT_BL, OUTPUT);
    analogWrite(PIN_TFT_BL, 220);

    // Display — initialise SPI bus with explicit pins, then bring up ILI9341
    Serial.println("[2] tft.begin");
    g_spi.begin(PIN_TFT_SCLK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);
    tft.begin(ILI9341_SPI_FREQ);
    tft.setRotation(1);   // landscape, USB to the right
    tft.fillScreen(TFT_BLACK);

    // Full-screen canvas in PSRAM
    Serial.println("[3] canvas alloc");
    canvas = new GFXcanvas16(SCREEN_W, SCREEN_H);
    if (!canvas || !canvas->getBuffer()) {
        tft.fillScreen(TFT_RED);
        tft.setTextColor(TFT_WHITE);
        tft.setTextSize(2);
        tft.setCursor(10, 80);
        tft.println("PSRAM ALLOC FAILED");
        tft.setCursor(10, 110);
        tft.setTextSize(1);
        tft.println("platformio.ini needs:");
        tft.println("board_build.arduino");
        tft.println(".memory_type = qio_opi");
        Serial.println("[FATAL] canvas alloc failed");
        for (;;) delay(1000);
    }
    Serial.printf("[3] canvas OK  heap %lu B\n", (unsigned long)ESP.getFreeHeap());

    Serial.println("[4] sky gradient");
    buildSkyGradient();

    Serial.println("[5] touch_init");
    touch_init();

    Serial.println("[6] mic_init");
    mic_init();

    Serial.println("[7] preferences");
    prefs.begin("flappy", true);
    hiScore = prefs.getInt("hi", 0);
    prefs.end();

    Serial.println("[8] resetGame");
    resetGame();
    flashTimer = millis();
    Serial.println("[OK] setup complete");
}

void loop() {
    static uint32_t lastMs = 0;
    const  uint32_t FRAME_MS = 33;   // ~30 FPS

    uint32_t now = millis();
    if (now - lastMs < FRAME_MS) return;
    lastMs = now;

    if (now - flashTimer >= 550) { flashOn = !flashOn; flashTimer = now; }

    switch (state) {
    case STATE_SPLASH:
        drawSplash();
        if (checkFlap()) { resetGame(); state = STATE_PLAYING; }
        break;

    case STATE_PLAYING:
        if (checkFlap()) bird.flap();
        updateGame();
        renderGame();
        break;

    case STATE_DEAD:
        drawDead();
        if (now - deadSince > 800 && checkFlap()) { resetGame(); state = STATE_PLAYING; }
        break;
    }
}
