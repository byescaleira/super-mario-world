// Motion interpolation for high refresh rate displays. The game logic stays
// at 60 Hz; in-between frames are re-rendered by the PPU from the recorded
// per-line state of the two most recent displayed frames, with every BG scroll
// value and every sprite placed part-way between them. Large jumps (scene
// cuts, teleports, sprite slot reuse) are not interpolated.
#include <stdlib.h>

#include "smw_core.h"
#include "snes.h"

enum { MAX_SCROLL_STEP = 64, MAX_SPRITE_STEP = 24, OAM_HIDDEN_Y = 0xF0 };

void interp_capture_line(Snes *s, int line) {
    InterpCapture *c = s->capture;
    if (line == 1) {
        c->cur ^= 1;
        c->complete[c->cur] = false;
        memcpy(c->oam[c->cur], s->ppu.oam, sizeof(s->ppu.oam));
        memcpy(c->vram[c->cur], s->ppu.vram, sizeof(s->ppu.vram));
    }
    memcpy(c->regs[c->cur][line], (const uint8_t *)&s->ppu + PPU_REG_OFFSET, PPU_REG_SIZE);
    memcpy(c->cgram[c->cur][line], s->ppu.cgram, sizeof(s->ppu.cgram));
    if (line == VISIBLE_LINES) c->complete[c->cur] = true;
}

static inline int lerp_wrapped(int a, int b, int mask, int maxStep, float t) {
    int range = mask + 1;
    int diff = ((b - a + range / 2) & mask) - range / 2;
    if (diff > maxStep || diff < -maxStep) return b;
    int v = a + (int)(diff * t + (diff >= 0 ? 0.5f : -0.5f));
    return v & mask;
}

static inline int lerp_signed16(int a, int b, int maxStep, float t) {
    int diff = b - a;
    if (diff > maxStep || diff < -maxStep) return b;
    return a + (int)(diff * t + (diff >= 0 ? 0.5f : -0.5f));
}

static void interpolate_oam(uint8_t *out, const uint8_t *a, const uint8_t *b, float t) {
    memcpy(out, b, 544);
    for (int i = 0; i < 128; i++) {
        const uint8_t *ea = a + i * 4, *eb = b + i * 4;
        int ha = a[0x200 + (i >> 2)] >> ((i & 3) * 2);
        int hb = b[0x200 + (i >> 2)] >> ((i & 3) * 2);
        if (ea[1] == OAM_HIDDEN_Y || eb[1] == OAM_HIDDEN_Y) continue;
        if ((ea[3] & 0x0E) != (eb[3] & 0x0E) || (ha & 2) != (hb & 2)) continue; // palette / size
        int xa = ea[0] | ((ha & 1) << 8), xb = eb[0] | ((hb & 1) << 8);
        int x = lerp_wrapped(xa, xb, 0x1FF, MAX_SPRITE_STEP, t);
        int y = lerp_wrapped(ea[1], eb[1], 0xFF, MAX_SPRITE_STEP, t);
        out[i * 4] = x & 0xFF;
        out[i * 4 + 1] = (uint8_t)y;
        uint8_t *hi = &out[0x200 + (i >> 2)];
        int shift = (i & 3) * 2;
        *hi = (uint8_t)((*hi & ~(1 << shift)) | (((x >> 8) & 1) << shift));
    }
}

void smw_set_motion_capture(SMWCore *core, bool enabled);
bool smw_render_interpolated(SMWCore *core, uint32_t *pixels, int pitch, float t);

// The public SMWCore begins with its Snes (see snes.c).
static inline Snes *snes_of(SMWCore *c) { return (Snes *)c; }

void smw_set_motion_capture(SMWCore *core, bool enabled) {
    Snes *s = snes_of(core);
    if (enabled && !s->capture) {
        s->capture = calloc(1, sizeof(InterpCapture));
    } else if (!enabled && s->capture) {
        free(s->capture);
        s->capture = NULL;
    }
}

bool smw_render_interpolated(SMWCore *core, uint32_t *pixels, int pitch, float t) {
    Snes *s = snes_of(core);
    InterpCapture *c = s->capture;
    if (!c) return false;
    int b = c->cur, a = b ^ 1;
    if (!c->complete[a] || !c->complete[b]) return false;

    static Ppu frame, prev;
    memcpy(frame.vram, c->vram[b], sizeof(frame.vram));
    interpolate_oam(frame.oam, c->oam[a], c->oam[b], t);
    uint8_t *frameRegs = (uint8_t *)&frame + PPU_REG_OFFSET;
    uint8_t *prevRegs = (uint8_t *)&prev + PPU_REG_OFFSET;

    for (int line = 1; line <= VISIBLE_LINES; line++) {
        memcpy(frameRegs, c->regs[b][line], PPU_REG_SIZE);
        memcpy(prevRegs, c->regs[a][line], PPU_REG_SIZE);
        memcpy(frame.cgram, c->cgram[b][line], sizeof(frame.cgram));
        for (int bg = 0; bg < 4; bg++) {
            frame.hofs[bg] = (uint16_t)lerp_wrapped(prev.hofs[bg], frame.hofs[bg], 0x3FF, MAX_SCROLL_STEP, t);
            frame.vofs[bg] = (uint16_t)lerp_wrapped(prev.vofs[bg], frame.vofs[bg], 0x3FF, MAX_SCROLL_STEP, t);
        }
        if ((frame.bgmode & 7) == 7) {
            frame.m7hofs = (int16_t)lerp_signed16(prev.m7hofs, frame.m7hofs, MAX_SCROLL_STEP, t);
            frame.m7vofs = (int16_t)lerp_signed16(prev.m7vofs, frame.m7vofs, MAX_SCROLL_STEP, t);
        }
        ppu_draw_line(s, &frame, line, pixels + (size_t)(line - 1) * pitch);
    }
    return true;
}
