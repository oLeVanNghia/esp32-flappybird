#pragma once
#include <Adafruit_GFX.h>

// ── Catch! — falling-objects game ─────────────────────────────────────────────

void catch_reset();

// Move the basket to the tapped display X.  Call when a touch is detected.
void catch_set_basket(int tx);

// Advance physics one frame.  Returns true when all lives are lost.
bool catch_update();

// Draw the gameplay scene onto canvas (caller calls pushFrame).
void catch_render(GFXcanvas16 *canvas);

// Draw the game-over overlay on top of the frozen scene.
void catch_draw_gameover(GFXcanvas16 *canvas, bool flashOn);

int catch_score();
int catch_hiscore();
