// Sub-pixel parallax scroll. Each frame, background layers that don't move in
// lockstep with BG1 (level layer 1) get a smoothed position: predicted from
// their average speed and kept inside the game's own pixel, so the picture
// never leaves what the game shows by a whole pixel. BG1, sprites and the HUD
// are never touched.
#include <stdlib.h>
#include <string.h>

#include "snes.h"

enum { MAX_STEP = 16 };
static const float kVelocityRate = 0.25f;

static inline int wrapped_step(int a, int b) { return ((b - a + 512) & 0x3FF) - 512; }

static void reset_layer(SubpixelScroll *u, int bg) {
    u->offset[bg] = 0;
    u->velocity[bg] = 0;
    u->locked[bg] = true;
    memset(u->diff[bg], 0, SUBPIXEL_HISTORY);
}

void subpixel_frame(Snes *s) {
    SubpixelScroll *u = snes_subpixel(s);
    const Ppu *p = &s->ppu;
    if ((p->bgmode & 7) == 7) {
        u->valid = false;
        return;
    }
    if (!u->valid) {
        for (int bg = 0; bg < 4; bg++) {
            u->prevHofs[bg] = p->hofs[bg];
            reset_layer(u, bg);
        }
        memset(u->move, 0, sizeof(u->move));
        u->valid = true;
        return;
    }
    int step1 = wrapped_step(u->prevHofs[0], p->hofs[0]);
    u->prevHofs[0] = p->hofs[0];
    int slot = u->histPos = (u->histPos + 1) % SUBPIXEL_HISTORY;
    u->move[slot] = (uint8_t)(abs(step1) > 255 ? 255 : abs(step1));
    int moved = 0;
    for (int k = 0; k < SUBPIXEL_HISTORY; k++) moved += u->move[k];

    for (int bg = 1; bg < 4; bg++) {
        int step = wrapped_step(u->prevHofs[bg], p->hofs[bg]);
        u->prevHofs[bg] = p->hofs[bg];
        // Scene cuts and teleports: start over from the game's position.
        if (abs(step) > MAX_STEP || abs(step1) > MAX_STEP) {
            reset_layer(u, bg);
            continue;
        }
        int d = abs(step - step1);
        u->diff[bg][slot] = (uint8_t)(d > 255 ? 255 : d);
        int differs = 0;
        for (int k = 0; k < SUBPIXEL_HISTORY; k++) differs += u->diff[bg][k];
        // Layers scrolling with BG1 (layer-2 levels, the overworld) must stay
        // pixel-aligned with the sprites standing on them.
        if (differs >= 2) {
            if (u->locked[bg]) {
                u->locked[bg] = false;
                u->offset[bg] = 0;
            }
        } else if (differs == 0 && moved > 4) {
            u->locked[bg] = true;
        }
        u->velocity[bg] += (step - u->velocity[bg]) * kVelocityRate;
        if (u->locked[bg]) {
            u->offset[bg] = 0;
            continue;
        }
        float o = u->offset[bg] + u->velocity[bg] - (float)step;
        u->offset[bg] = o < 0 ? 0 : (o > 0.999f ? 0.999f : o);
    }
}
