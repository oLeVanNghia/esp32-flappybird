#include "catch.h"
#include "config.h"
#include <Arduino.h>
#include <Preferences.h>

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr int BASKET_W = 68;
static constexpr int BASKET_H = 14;
static constexpr int BASKET_Y = GROUND_Y - BASKET_H - 2;
static constexpr int OBJ_R    = 12;
static constexpr int MAX_OBJ  = 5;

// ── Object types ──────────────────────────────────────────────────────────────

enum ObjType : uint8_t { OBJ_NONE, OBJ_APPLE, OBJ_COIN, OBJ_BOMB };

struct FallingObj {
    float   x, y, speed;
    ObjType type;
    bool    active;

    void spawn(int sc) {
        x      = (float)random(OBJ_R + 4, SCREEN_W - OBJ_R - 4);
        y      = -(float)(OBJ_R + 2);
        float spd = 1.8f + sc * 0.055f;
        if (spd > 6.5f) spd = 6.5f;
        speed  = spd + random(0, 70) / 100.0f;
        int r  = random(100);
        type   = (r < 55) ? OBJ_APPLE : (r < 82) ? OBJ_COIN : OBJ_BOMB;
        active = true;
    }
};

// ── Game state ────────────────────────────────────────────────────────────────

static FallingObj g_obj[MAX_OBJ];
static float      g_bx;          // basket left-edge X
static int        g_score;
static int        g_hiScore;
static int        g_lives;
static uint32_t   g_lastSpawn;

// ── Helpers ───────────────────────────────────────────────────────────────────

static void load_hi() {
    Preferences p; p.begin("catch_hi", true);
    g_hiScore = p.getInt("hi", 0);
    p.end();
}
static void save_hi() {
    Preferences p; p.begin("catch_hi", false);
    p.putInt("hi", g_hiScore);
    p.end();
}

static int active_count() {
    int n = 0;
    for (const auto &o : g_obj) if (o.active) n++;
    return n;
}

// Centre-aligned text helper (local to catch.cpp)
static void cv_mc(GFXcanvas16 *cv, const char *s, int cx, int cy) {
    int16_t x1, y1; uint16_t tw, th;
    cv->getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
    cv->setCursor(cx - (int)(tw / 2) - x1, cy - (int)(th / 2) - y1);
    cv->print(s);
}
static void cv_mc(GFXcanvas16 *cv, int n, int cx, int cy) {
    char buf[12]; snprintf(buf, sizeof(buf), "%d", n);
    cv_mc(cv, buf, cx, cy);
}

// ── Public API ────────────────────────────────────────────────────────────────

int catch_score()   { return g_score; }
int catch_hiscore() { return g_hiScore; }

void catch_reset() {
    for (auto &o : g_obj) o.active = false;
    g_bx        = (SCREEN_W - BASKET_W) / 2.0f;
    g_score     = 0;
    g_lives     = 3;
    g_lastSpawn = 0;
    load_hi();
}

void catch_set_basket(int tx) {
    g_bx = (float)(tx - BASKET_W / 2);
    if (g_bx < 0)                   g_bx = 0;
    if (g_bx > SCREEN_W - BASKET_W) g_bx = (float)(SCREEN_W - BASKET_W);
}

bool catch_update() {
    uint32_t now = millis();

    // How many objects on screen at once (scales with score)
    int maxActive = 1 + g_score / 7;
    if (maxActive > 4) maxActive = 4;

    // Spawn
    uint32_t interval = (uint32_t)(1600.0f - g_score * 16.0f);
    if (interval < 350) interval = 350;
    if (active_count() < maxActive && now - g_lastSpawn > interval) {
        for (auto &o : g_obj) {
            if (!o.active) { o.spawn(g_score); break; }
        }
        g_lastSpawn = now;
    }

    // Physics + collision
    for (auto &o : g_obj) {
        if (!o.active) continue;
        o.y += o.speed;

        // Basket collision (AABB vs circle)
        bool inX = (o.x + OBJ_R > g_bx) && (o.x - OBJ_R < g_bx + BASKET_W);
        bool inY = (o.y + OBJ_R >= BASKET_Y) && (o.y - OBJ_R <= BASKET_Y + BASKET_H);

        if (inX && inY) {
            o.active = false;
            if      (o.type == OBJ_APPLE) { g_score += 1; }
            else if (o.type == OBJ_COIN)  { g_score += 3; }
            else if (o.type == OBJ_BOMB)  {
                g_lives--;
                if (g_lives <= 0) {
                    if (g_score > g_hiScore) { g_hiScore = g_score; save_hi(); }
                    return true;
                }
            }
            continue;
        }

        // Fell below ground
        if (o.y - OBJ_R > GROUND_Y) {
            o.active = false;
            if (o.type == OBJ_APPLE) {      // missing an apple costs a life
                g_lives--;
                if (g_lives <= 0) {
                    if (g_score > g_hiScore) { g_hiScore = g_score; save_hi(); }
                    return true;
                }
            }
            // Missed COIN or BOMB → no penalty
        }
    }
    return false;
}

// ── Rendering ─────────────────────────────────────────────────────────────────

static void draw_heart(GFXcanvas16 *cv, int cx, int cy, uint16_t col) {
    cv->fillCircle(cx - 4, cy - 2, 5, col);
    cv->fillCircle(cx + 4, cy - 2, 5, col);
    cv->fillTriangle(cx - 9, cy + 2, cx + 9, cy + 2, cx, cy + 11, col);
}

void catch_render(GFXcanvas16 *canvas) {
    // Background — dark night sky to distinguish from Flabby Bird
    for (int y = 0; y < GROUND_Y; y++) {
        uint16_t c = (y < GROUND_Y / 2)
            ? C565(10, 10, 40)
            : C565(25 + y / 8, 20, 60 - y / 5);
        canvas->drawFastHLine(0, y, SCREEN_W, c);
    }
    // Stars (static, based on position)
    for (int i = 0; i < 30; i++) {
        int sx = (i * 67 + 13) % SCREEN_W;
        int sy = (i * 43 + 7)  % (GROUND_Y - 20);
        canvas->drawPixel(sx, sy, TFT_WHITE);
    }
    // Ground
    canvas->fillRect(0, GROUND_Y,      SCREEN_W, 6,               CLR_GND_TOP);
    canvas->fillRect(0, GROUND_Y + 6,  SCREEN_W, SCREEN_H - GROUND_Y - 6,  CLR_GROUND);
    canvas->fillRect(0, GROUND_Y + 14, SCREEN_W, SCREEN_H - GROUND_Y - 14, CLR_DIRT);

    // Falling objects
    for (const auto &o : g_obj) {
        if (!o.active) continue;
        int ox = (int)o.x, oy = (int)o.y;
        switch (o.type) {
        case OBJ_APPLE:
            canvas->fillCircle(ox, oy, OBJ_R,     C565(200, 40, 40));
            canvas->fillCircle(ox, oy, OBJ_R - 3, C565(240, 80, 80));
            canvas->drawFastVLine(ox + 2, oy - OBJ_R - 3, 4, C565(60, 120, 20));
            canvas->drawLine(ox + 2, oy - OBJ_R - 1, ox + 7, oy - OBJ_R - 4,
                             C565(60, 120, 20));
            break;
        case OBJ_COIN:
            canvas->fillCircle(ox, oy, OBJ_R,     C565(220, 180, 0));
            canvas->fillCircle(ox, oy, OBJ_R - 3, C565(255, 220, 60));
            canvas->setFont(nullptr); canvas->setTextSize(1);
            canvas->setTextColor(C565(160, 120, 0));
            canvas->setCursor(ox - 3, oy - 4);
            canvas->print("$");
            break;
        case OBJ_BOMB:
            canvas->fillCircle(ox, oy, OBJ_R,     C565(30, 30, 30));
            canvas->fillCircle(ox, oy, OBJ_R - 2, C565(60, 60, 60));
            canvas->drawLine(ox - 5, oy - 5, ox + 5, oy + 5, TFT_RED);
            canvas->drawLine(ox + 5, oy - 5, ox - 5, oy + 5, TFT_RED);
            canvas->fillCircle(ox + OBJ_R - 2, oy - OBJ_R + 2, 3, C565(255, 140, 0));
            break;
        default: break;
        }
    }

    // Basket
    int bx = (int)g_bx;
    canvas->fillRoundRect(bx, BASKET_Y, BASKET_W, BASKET_H, 5,
                          C565(160, 100, 40));
    canvas->drawRoundRect(bx, BASKET_Y, BASKET_W, BASKET_H, 5,
                          C565(210, 160, 80));
    canvas->drawFastHLine(bx + 3, BASKET_Y + 2, BASKET_W - 6, C565(220, 175, 100));

    // Lives (hearts top-left)
    for (int i = 0; i < 3; i++) {
        uint16_t col = (i < g_lives) ? TFT_RED : C565(60, 30, 30);
        draw_heart(canvas, 12 + i * 22, 14, col);
    }

    // Score (top-right)
    canvas->setFont(nullptr);
    canvas->setTextSize(2);
    canvas->setTextColor(C565(40, 40, 40));
    cv_mc(canvas, g_score, SCREEN_W - 26 + 1, 12 + 1);
    canvas->setTextColor(TFT_WHITE);
    cv_mc(canvas, g_score, SCREEN_W - 26, 12);
}

void catch_draw_gameover(GFXcanvas16 *canvas, bool flashOn) {
    catch_render(canvas);   // frozen game scene

    // Popup
    const int px = SCREEN_W / 2 - 95, pw = 190, ph = 138;
    canvas->fillRoundRect(px, 56, pw, ph, 12, C565(10, 10, 20));
    canvas->drawRoundRect(px, 56, pw, ph, 12, TFT_WHITE);

    canvas->setFont(nullptr);
    canvas->setTextSize(2);
    canvas->setTextColor(TFT_WHITE);
    cv_mc(canvas, "GAME OVER", SCREEN_W / 2, 74);

    canvas->setTextSize(1);
    canvas->setTextColor(C565(200, 200, 200));
    cv_mc(canvas, "SCORE",   SCREEN_W / 2 - 42, 100);
    cv_mc(canvas, "BEST",    SCREEN_W / 2 + 42, 100);

    canvas->setTextSize(3);
    canvas->setTextColor(TFT_WHITE);
    cv_mc(canvas, g_score,   SCREEN_W / 2 - 42, 118);
    cv_mc(canvas, g_hiScore, SCREEN_W / 2 + 42, 118);

    // Buttons (same layout as Flabby Bird game over)
    const int by = 152, bh = 30;
    const int lx = px + 6,         lw = pw / 2 - 10;
    const int rx = SCREEN_W / 2 + 4, rw = pw / 2 - 10;

    canvas->fillRoundRect(lx, by, lw, bh, 6, C565(40, 40, 80));
    canvas->drawRoundRect(lx, by, lw, bh, 6, C565(140, 140, 220));
    canvas->setTextSize(1);
    canvas->setTextColor(C565(180, 180, 255));
    cv_mc(canvas, "HOLD MENU", lx + lw / 2, by + bh / 2);

    if (flashOn) {
        canvas->fillRoundRect(rx, by, rw, bh, 6, C565(40, 80, 40));
        canvas->drawRoundRect(rx, by, rw, bh, 6, C565(140, 220, 140));
        canvas->setTextColor(C565(180, 255, 180));
    } else {
        canvas->fillRoundRect(rx, by, rw, bh, 6, C565(25, 50, 25));
        canvas->drawRoundRect(rx, by, rw, bh, 6, C565(80, 140, 80));
        canvas->setTextColor(C565(120, 180, 120));
    }
    cv_mc(canvas, "TAP RETRY", rx + rw / 2, by + bh / 2);
}
