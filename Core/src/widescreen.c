// Game-aware widescreen policy. SMW keeps Layer 1/2 tilemap columns loaded
// from 8 tiles left to 23 tiles right of the camera (CODE_05877E in bank 05),
// so up to ~112 px beyond each edge of the 256 px view is already correct in
// VRAM. This module decides, per frame and per scene (GameMode values from
// SMWDisX rammap.asm), which layers may use the margins:
//  * level engine scenes: foreground up to the level bounds, background beyond;
//  * overworld: the map frame's edge pattern continues to the screen sides;
//  * everything else: background colour/gradient only.
// The screen is always fully covered; the centre 256 columns are untouched.
#include "smw_symbols.h"
#include "snes.h"

enum {
    MODE_FADE_IN_TITLE = 5,
    MODE_PLAYER_SELECT = 10,
    MODE_LOAD_OVERWORLD = 12,
    MODE_OVERWORLD = 14,
    MODE_PREPARE_LEVEL = 18,
    MODE_LEVEL = 20,
    MARGIN_SPEED = 6,
    LAYER_BG1 = 1, LAYER_BG2 = 2, LAYER_BG3 = 4, LAYER_BG4 = 8, LAYER_OBJ = 16,
    SCREENMODE_L2_INTERACT = 0x80,
};

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline int wram16(const Snes *s, uint32_t addr) {
    return s->wram[addr & 0x1FFFF] | (s->wram[(addr + 1) & 0x1FFFF] << 8);
}

static void approach(int *value, int target) {
    // Open smoothly; close instantly so nothing invalid is ever revealed.
    *value = *value < target ? clampi(target, *value, *value + MARGIN_SPEED) : target;
}

void widescreen_frame(Snes *s) {
    Widescreen *w = &s->wide;
    uint8_t mode = s->wram[SYM_GameMode];
    int targetL = 0, targetR = 0;
    bool mode7 = (s->ppu.bgmode & 7) == 7;
    bool titleScenes = mode >= MODE_FADE_IN_TITLE && mode <= MODE_PLAYER_SELECT;
    bool levelScenes = mode >= MODE_PREPARE_LEVEL && mode <= MODE_LEVEL;
    w->edgeExtendMask = 0;

    if (titleScenes) {
        // Title / file select run on top of the attract-mode level.
        w->fgMask = LAYER_BG1 | LAYER_OBJ;
        w->bg3MarginStartLine = VISIBLE_LINES + 1;
    } else if (mode >= MODE_LOAD_OVERWORLD && mode <= MODE_OVERWORLD) {
        // The map lives inside a frame drawn on BG3: extend the frame's edge
        // pattern to the screen sides rather than revealing other map regions.
        w->fgMask = LAYER_BG1 | LAYER_BG2 | LAYER_OBJ;
        w->bg3MarginStartLine = 0;
        w->edgeExtendMask = LAYER_BG3;
    } else if (levelScenes) {
        w->fgMask = LAYER_OBJ | (mode7 ? 0 : LAYER_BG1);
        if (s->wram[SYM_ScreenMode] & SCREENMODE_L2_INTERACT) w->fgMask |= LAYER_BG2;
        w->bg3MarginStartLine = SMW_StatusBarHeight + 1;
    } else {
        w->fgMask = LAYER_BG1 | LAYER_BG2 | LAYER_BG3 | LAYER_BG4 | LAYER_OBJ;
        w->bg3MarginStartLine = VISIBLE_LINES + 1;
    }

    if ((titleScenes || levelScenes) && w->extra > 0) {
        // Camera X as the PPU will draw it: the 10-bit BG1 scroll register,
        // unwrapped around the game's own 16-bit camera variable.
        int ram = wram16(s, SYM_Layer1XPos);
        int diff = (s->ppu.hofs[0] - ram) & 0x3FF;
        if (diff >= 512) diff -= 1024;
        int camX = ram + diff;
        bool vertical = s->wram[SYM_ScreenMode] & 1;
        int levelWidth = vertical ? 512 : (s->wram[SYM_LastScreenHoriz] + 1) * 256;
        targetL = clampi(camX, 0, w->extra);
        targetR = clampi(levelWidth - (camX + 256), 0, w->extra);
    }

    approach(&w->fgLeft, targetL);
    approach(&w->fgRight, targetR);
}
