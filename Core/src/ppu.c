// Scanline renderer for the S-PPU: BG modes 0-4 and 7, sprites with range/time
// limits, windows, color math, mosaic and master brightness.
#include "snes.h"

enum { L_BG1, L_BG2, L_BG3, L_BG4, L_OBJ, L_BACK };

// Lines are rendered `extra` pixels wider on each side for widescreen. Arrays
// are indexed by i = x + extra, where x is the classic 0..255 screen column.
#define MAXW (256 + 2 * PPU_MAX_EXTRA)
// Composed lines hold `factor` samples per pixel (sub-pixel parallax scroll).
#define MAXS (MAXW * PPU_MAX_SUBPIXEL)

typedef struct {
    uint16_t color[MAXS];
    uint8_t z[MAXS];
    uint8_t layer[MAXS];
    uint8_t objMath[MAXS]; // sprite palettes 0-3 never take part in color math
} Screen;

// BG lines are rendered one pixel wider on each side (index 0 = x0 - 1) so a
// sub-pixel shifted layer always has its neighbouring pixel.
typedef struct {
    uint8_t pix[MAXW + 2];  // CGRAM index, 0 = transparent
    uint8_t pri[MAXW + 2];
} LayerLine;

void ppu_reset(Ppu *p) {
    memset(p, 0, sizeof(*p));
    p->inidisp = 0x80;
    p->mosaicStartLine = 1;
}

// ------------------------------------------------------------------- VRAM
static uint16_t vram_remap(Ppu *p) {
    uint16_t a = p->vramAddr;
    switch ((p->vmain >> 2) & 3) {
    case 1: return (a & 0xFF00) | ((a & 0x001F) << 3) | ((a >> 5) & 7);
    case 2: return (a & 0xFE00) | ((a & 0x003F) << 3) | ((a >> 6) & 7);
    case 3: return (a & 0xFC00) | ((a & 0x007F) << 3) | ((a >> 7) & 7);
    default: return a;
    }
}

static int vram_increment(Ppu *p) {
    static const int inc[4] = {1, 32, 128, 128};
    return inc[p->vmain & 3];
}

void ppu_latch_counters(Snes *s) {
    Ppu *p = &s->ppu;
    p->hLatch = s->hpos >> 2;
    p->vLatch = s->vpos;
    p->hvLatched = true;
}

uint8_t ppu_read(Snes *s, uint8_t reg) {
    Ppu *p = &s->ppu;
    switch (reg) {
    case 0x34: return p->mpyResult[0];
    case 0x35: return p->mpyResult[1];
    case 0x36: return p->mpyResult[2];
    case 0x37:
        ppu_latch_counters(s);
        return s->openBus;
    case 0x38: {
        uint8_t v;
        if (p->oamAddr < 0x200) v = p->oam[p->oamAddr];
        else v = p->oam[0x200 | (p->oamAddr & 0x1F)];
        p->oamAddr = (p->oamAddr + 1) & 0x3FF;
        return v;
    }
    case 0x39: {
        uint8_t v = p->vramPrefetch & 0xFF;
        if (!(p->vmain & 0x80)) {
            p->vramPrefetch = p->vram[vram_remap(p) & 0x7FFF];
            p->vramAddr += vram_increment(p);
        }
        return v;
    }
    case 0x3A: {
        uint8_t v = p->vramPrefetch >> 8;
        if (p->vmain & 0x80) {
            p->vramPrefetch = p->vram[vram_remap(p) & 0x7FFF];
            p->vramAddr += vram_increment(p);
        }
        return v;
    }
    case 0x3B: {
        uint16_t c = p->cgram[p->cgAddr];
        uint8_t v;
        if (!p->cgHigh) {
            v = c & 0xFF;
        } else {
            v = (c >> 8) & 0x7F;
            p->cgAddr++;
        }
        p->cgHigh = !p->cgHigh;
        return v;
    }
    case 0x3C: {
        uint8_t v = p->hLatchHigh ? (p->hLatch >> 8) & 1 : p->hLatch & 0xFF;
        p->hLatchHigh = !p->hLatchHigh;
        return v;
    }
    case 0x3D: {
        uint8_t v = p->vLatchHigh ? (p->vLatch >> 8) & 1 : p->vLatch & 0xFF;
        p->vLatchHigh = !p->vLatchHigh;
        return v;
    }
    case 0x3E: return 0x01; // PPU1 version
    case 0x3F: {
        uint8_t v = 0x03 | (p->hvLatched ? 0x40 : 0) | ((s->frames & 1) ? 0x80 : 0);
        p->hvLatched = false;
        p->hLatchHigh = p->vLatchHigh = false;
        return v;
    }
    default: return s->openBus;
    }
}

static void update_mpy(Ppu *p) {
    int32_t r = (int32_t)p->m7a * (int8_t)(p->m7b >> 8);
    p->mpyResult[0] = r & 0xFF;
    p->mpyResult[1] = (r >> 8) & 0xFF;
    p->mpyResult[2] = (r >> 16) & 0xFF;
}

void ppu_write(Snes *s, uint8_t reg, uint8_t v) {
    Ppu *p = &s->ppu;
    switch (reg) {
    case 0x00:
        if ((p->inidisp & 0x80) && !(v & 0x80) && s->vpos == VISIBLE_LINES + 1)
            p->oamAddr = p->oamAddrReload;
        p->inidisp = v;
        break;
    case 0x01: p->obsel = v; break;
    case 0x02:
        p->oamAddrReload = (p->oamAddrReload & 0x200) | (v << 1);
        p->oamAddr = p->oamAddrReload;
        break;
    case 0x03:
        p->oamAddrReload = (p->oamAddrReload & 0x1FE) | ((v & 1) << 9);
        p->oamPriorityRotation = v & 0x80;
        p->oamAddr = p->oamAddrReload;
        break;
    case 0x04:
        if (p->oamAddr >= 0x200) {
            p->oam[0x200 | (p->oamAddr & 0x1F)] = v;
        } else if (!(p->oamAddr & 1)) {
            p->oamLatch = v;
        } else {
            p->oam[p->oamAddr - 1] = p->oamLatch;
            p->oam[p->oamAddr] = v;
        }
        p->oamAddr = (p->oamAddr + 1) & 0x3FF;
        break;
    case 0x05: p->bgmode = v; break;
    case 0x06:
        p->mosaic = v;
        p->mosaicStartLine = s->vpos;
        break;
    case 0x07: case 0x08: case 0x09: case 0x0A: p->bgsc[reg - 7] = v; break;
    case 0x0B: case 0x0C: p->bgnba[reg - 0x0B] = v; break;
    case 0x0D:
        p->m7hofs = (int16_t)((v << 8) | p->m7latch);
        p->m7latch = v;
        /* fallthrough */
    case 0x0F: case 0x11: case 0x13: {
        int bg = (reg - 0x0D) >> 1;
        // The low 3 bits come from the byte written to an H-scroll register
        // just before (the PPU's second latch), kept here as bits 8-10.
        // Masking to 10 bits lost bit 10, so every scroll value with bit 2
        // set showed 4 px off: layers jumped back 3 and ahead 5 while scrolling.
        // Readers mask to 10 bits.
        p->hofs[bg] = ((v << 8) | (p->ofsLatch & ~7) | ((p->hofs[bg] >> 8) & 7)) & 0x7FF;
        p->ofsLatch = v;
        break;
    }
    case 0x0E:
        p->m7vofs = (int16_t)((v << 8) | p->m7latch);
        p->m7latch = v;
        /* fallthrough */
    case 0x10: case 0x12: case 0x14: {
        int bg = (reg - 0x0E) >> 1;
        p->vofs[bg] = ((v << 8) | p->ofsLatch) & 0x3FF;
        p->ofsLatch = v;
        break;
    }
    case 0x15: p->vmain = v; break;
    case 0x16:
        p->vramAddr = (p->vramAddr & 0xFF00) | v;
        p->vramPrefetch = p->vram[vram_remap(p) & 0x7FFF];
        break;
    case 0x17:
        p->vramAddr = (p->vramAddr & 0x00FF) | (v << 8);
        p->vramPrefetch = p->vram[vram_remap(p) & 0x7FFF];
        break;
    case 0x18: {
        uint16_t a = vram_remap(p) & 0x7FFF;
        p->vram[a] = (p->vram[a] & 0xFF00) | v;
        if (!(p->vmain & 0x80)) p->vramAddr += vram_increment(p);
        break;
    }
    case 0x19: {
        uint16_t a = vram_remap(p) & 0x7FFF;
        p->vram[a] = (p->vram[a] & 0x00FF) | (v << 8);
        if (p->vmain & 0x80) p->vramAddr += vram_increment(p);
        break;
    }
    case 0x1A: p->m7sel = v; break;
    case 0x1B: p->m7a = (int16_t)((v << 8) | p->m7latch); p->m7latch = v; update_mpy(p); break;
    case 0x1C: p->m7b = (int16_t)((v << 8) | p->m7latch); p->m7latch = v; update_mpy(p); break;
    case 0x1D: p->m7c = (int16_t)((v << 8) | p->m7latch); p->m7latch = v; break;
    case 0x1E: p->m7d = (int16_t)((v << 8) | p->m7latch); p->m7latch = v; break;
    case 0x1F: p->m7x = (int16_t)((v << 8) | p->m7latch); p->m7latch = v; break;
    case 0x20: p->m7y = (int16_t)((v << 8) | p->m7latch); p->m7latch = v; break;
    case 0x21:
        p->cgAddr = v;
        p->cgHigh = false;
        break;
    case 0x22:
        if (!p->cgHigh) {
            p->cgLatch = v;
        } else {
            p->cgram[p->cgAddr] = ((v & 0x7F) << 8) | p->cgLatch;
            p->cgAddr++;
        }
        p->cgHigh = !p->cgHigh;
        break;
    case 0x23: case 0x24: case 0x25: p->wsel[reg - 0x23] = v; break;
    case 0x26: case 0x27: case 0x28: case 0x29: p->wh[reg - 0x26] = v; break;
    case 0x2A: p->wbglog = v; break;
    case 0x2B: p->wobjlog = v; break;
    case 0x2C: p->tm = v; break;
    case 0x2D: p->ts = v; break;
    case 0x2E: p->tmw = v; break;
    case 0x2F: p->tsw = v; break;
    case 0x30: p->cgwsel = v; break;
    case 0x31: p->cgadsub = v; break;
    case 0x32: {
        int intensity = v & 0x1F;
        if (v & 0x20) p->fixedColor = (p->fixedColor & ~0x001F) | intensity;
        if (v & 0x40) p->fixedColor = (p->fixedColor & ~0x03E0) | (intensity << 5);
        if (v & 0x80) p->fixedColor = (p->fixedColor & ~0x7C00) | (intensity << 10);
        break;
    }
    case 0x33: p->setini = v; break;
    default: break;
    }
}

// ---------------------------------------------------------------- windows
// Window ranges are 0..255. Widescreen margin columns take the window state of
// the nearest classic edge column, so full-screen masks and iris/keyhole
// effects extend naturally.
static void window_mask(Ppu *p, int layer, uint8_t *out, int x0, int width) {
    uint8_t cfg;
    uint8_t logic;
    if (layer < 4) {
        cfg = (p->wsel[layer >> 1] >> ((layer & 1) * 4)) & 0xF;
        logic = (p->wbglog >> (layer * 2)) & 3;
    } else {
        cfg = (p->wsel[2] >> ((layer - 4) * 4)) & 0xF;
        logic = (p->wobjlog >> ((layer - 4) * 2)) & 3;
    }
    bool w1 = cfg & 2, w1inv = cfg & 1;
    bool w2 = cfg & 8, w2inv = cfg & 4;
    if (!w1 && !w2) {
        memset(out, 0, width);
        return;
    }
    int l1 = p->wh[0], r1 = p->wh[1], l2 = p->wh[2], r2 = p->wh[3];
    for (int i = 0; i < width; i++) {
        int x = x0 + i;
        if (x < 0) x = 0;
        if (x > 255) x = 255;
        bool a = (x >= l1 && x <= r1) != w1inv;
        bool b = (x >= l2 && x <= r2) != w2inv;
        bool r;
        if (w1 && !w2) r = a;
        else if (!w1 && w2) r = b;
        else {
            switch (logic) {
            case 0: r = a || b; break;
            case 1: r = a && b; break;
            case 2: r = a != b; break;
            default: r = a == b; break;
            }
        }
        out[i] = r;
    }
}

// --------------------------------------------------------------- BG layers
static inline int floor_mod(int a, int m) { return ((a % m) + m) % m; }

static void render_bg(Ppu *p, int bg, int bpp, int line, LayerLine *out, int palOffset, int x0, int width,
                      bool edgeExtend) {
    int mosaicSize = (p->mosaic & (1 << bg)) ? ((p->mosaic >> 4) + 1) : 1;
    int y = line;
    if (mosaicSize > 1) y = line - floor_mod(line - p->mosaicStartLine, mosaicSize);
    y += p->vofs[bg];
    bool big = p->bgmode & (0x10 << bg);
    int tileShift = big ? 4 : 3;
    uint16_t mapBase = (p->bgsc[bg] & 0xFC) << 8;
    int scSize = p->bgsc[bg] & 3;
    uint16_t charBase = ((p->bgnba[bg >> 1] >> ((bg & 1) * 4)) & 0xF) << 12;
    int wordsPerTile = bpp * 4;
    int hofs = p->hofs[bg];

    int cachedKey = -1;
    uint8_t cachedRow[8];
    uint8_t cachedPal = 0, cachedPri = 0;
    for (int i = 0; i < width; i++) {
        int sx = x0 + i;
        // Screen frames: continue the frame's outer pattern strip (the left
        // 16 columns; the right edge carries the map's drop shadow).
        if (edgeExtend && (sx < 0 || sx > 255)) sx = floor_mod(sx, 16);
        if (mosaicSize > 1) sx -= floor_mod(sx, mosaicSize);
        int px = (sx + hofs) & 0x3FF;
        int py = y & 0x3FF;
        int tx = px >> tileShift;
        int key = (tx << 1) | ((px >> 3) & 1);
        if (key != cachedKey) {
            cachedKey = key;
            int ty = py >> tileShift;
            uint16_t addr = mapBase + ((ty & 31) << 5) + (tx & 31);
            if ((tx & 32) && (scSize & 1)) addr += 0x400;
            if ((ty & 32) && (scSize & 2)) addr += (scSize & 1) ? 0x800 : 0x400;
            uint16_t entry = p->vram[addr & 0x7FFF];
            int tile = entry & 0x3FF;
            bool hflip = entry & 0x4000, vflip = entry & 0x8000;
            cachedPal = (entry >> 10) & 7;
            cachedPri = (entry >> 13) & 1;
            int fy = py & 7;
            if (big) {
                int col = ((px >> 3) & 1) ^ (hflip ? 1 : 0);
                int row = ((py >> 3) & 1) ^ (vflip ? 1 : 0);
                tile += col + row * 16;
            }
            if (vflip) fy = 7 - fy;
            uint16_t ca = charBase + tile * wordsPerTile + fy;
            uint16_t w0 = p->vram[ca & 0x7FFF];
            uint16_t w1 = bpp >= 4 ? p->vram[(ca + 8) & 0x7FFF] : 0;
            uint16_t w2 = bpp >= 8 ? p->vram[(ca + 16) & 0x7FFF] : 0;
            uint16_t w3 = bpp >= 8 ? p->vram[(ca + 24) & 0x7FFF] : 0;
            for (int k = 0; k < 8; k++) {
                int bit = hflip ? k : 7 - k;
                int c = ((w0 >> bit) & 1) | (((w0 >> (bit + 8)) & 1) << 1);
                if (bpp >= 4) c |= (((w1 >> bit) & 1) << 2) | (((w1 >> (bit + 8)) & 1) << 3);
                if (bpp >= 8)
                    c |= (((w2 >> bit) & 1) << 4) | (((w2 >> (bit + 8)) & 1) << 5) |
                         (((w3 >> bit) & 1) << 6) | (((w3 >> (bit + 8)) & 1) << 7);
                cachedRow[k] = c;
            }
        }
        int c = cachedRow[px & 7];
        if (c == 0) {
            out->pix[i] = 0;
            continue;
        }
        if (bpp == 2) out->pix[i] = palOffset + cachedPal * 4 + c;
        else if (bpp == 4) out->pix[i] = cachedPal * 16 + c;
        else out->pix[i] = c;
        out->pri[i] = cachedPri;
    }
}

static inline int sclip13(int v) { return (v & 0x1000) ? (v | ~0x1FFF) : (v & 0x1FFF); }

static void render_mode7(Ppu *p, int line, LayerLine *out, int x0, int width) {
    int a = p->m7a, b = p->m7b, c = p->m7c, d = p->m7d;
    int hofs = sclip13(p->m7hofs), vofs = sclip13(p->m7vofs);
    int cx = sclip13(p->m7x), cy = sclip13(p->m7y);
    int y = (p->m7sel & 2) ? 255 - line : line;
    if (p->mosaic & 1) {
        int size = (p->mosaic >> 4) + 1;
        y -= floor_mod(y - p->mosaicStartLine, size);
    }
#define CLIP10(n) (((n) & 0x2000) ? ((n) | ~1023) : ((n) & 1023))
    int ox = CLIP10(hofs - cx);
    int oy = CLIP10(vofs - cy);
#undef CLIP10
    int psx = ((a * ox) & ~63) + ((b * oy) & ~63) + ((b * y) & ~63) + (cx << 8);
    int psy = ((c * ox) & ~63) + ((d * oy) & ~63) + ((d * y) & ~63) + (cy << 8);
    int repeat = p->m7sel >> 6;
    for (int i = 0; i < width; i++) {
        int x = x0 + i;
        int sx = (p->m7sel & 1) ? 255 - x : x;
        int px = (psx + a * sx) >> 8;
        int py = (psy + c * sx) >> 8;
        int tile;
        bool outside = (px | py) & ~1023;
        if (outside && repeat == 2) {
            out->pix[i] = 0;
            continue;
        }
        if (outside && repeat == 3) {
            tile = 0;
        } else {
            tile = p->vram[(((py >> 3) & 127) << 7) | ((px >> 3) & 127)] & 0xFF;
        }
        out->pix[i] = p->vram[(tile << 6) | ((py & 7) << 3) | (px & 7)] >> 8;
        out->pri[i] = 0;
    }
}

// ---------------------------------------------------------------- sprites
static const uint8_t kObjSizes[8][2][2] = {
    {{8, 8}, {16, 16}},  {{8, 8}, {32, 32}},  {{8, 8}, {64, 64}},   {{16, 16}, {32, 32}},
    {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}},
};

typedef struct {
    int w, h, x, y, row;
    uint8_t tile, attr;
} ObjInfo;

static void obj_info(Ppu *p, int i, int sizeSel, int line, ObjInfo *o) {
    uint8_t hi = p->oam[0x200 + (i >> 2)] >> ((i & 3) * 2);
    int big = (hi >> 1) & 1;
    o->w = kObjSizes[sizeSel][big][0];
    o->h = kObjSizes[sizeSel][big][1];
    o->x = p->oam[i * 4] | ((hi & 1) << 8);
    o->y = p->oam[i * 4 + 1];
    o->tile = p->oam[i * 4 + 2];
    o->attr = p->oam[i * 4 + 3];
    o->row = (uint8_t)(line - 1 - o->y);
}

// Draws one sprite row. Tiles inside the classic 256 columns count towards
// the 34-tile limit (if `tiles` is given); margin tiles are free.
static void draw_obj(Ppu *p, const ObjInfo *o, int x, int x0, int width, LayerLine *out, int *tiles) {
    uint16_t base = (p->obsel & 7) << 13;
    uint16_t gap = (((p->obsel >> 3) & 3) + 1) << 12;
    int r = o->row;
    if (o->attr & 0x80) r = o->h - 1 - r;
    bool hflip = o->attr & 0x40;
    int pal = (o->attr >> 1) & 7;
    int pri = (o->attr >> 4) & 3;
    uint16_t nameBase = base + ((o->attr & 1) ? gap : 0);
    int tileRow = ((o->tile >> 4) + (r >> 3)) & 0xF;
    for (int col = 0; col < o->w / 8; col++) {
        int sx = x + col * 8;
        bool onScreen = sx > -8 && sx < 256;
        if (sx + 8 <= x0 || sx >= x0 + width) continue;
        if (tiles && onScreen && ++*tiles > 34) return;
        int tc = hflip ? (o->w / 8 - 1 - col) : col;
        int tileCol = ((o->tile & 0xF) + tc) & 0xF;
        uint16_t addr = nameBase + (((tileRow << 4) | tileCol) << 4) + (r & 7);
        uint16_t w0 = p->vram[addr & 0x7FFF];
        uint16_t w1 = p->vram[(addr + 8) & 0x7FFF];
        for (int b = 0; b < 8; b++) {
            int i = sx + b - x0;
            if (i < 0 || i >= width) continue;
            int bit = hflip ? b : 7 - b;
            int c = ((w0 >> bit) & 1) | (((w0 >> (bit + 8)) & 1) << 1) | (((w1 >> bit) & 1) << 2) |
                    (((w1 >> (bit + 8)) & 1) << 3);
            if (!c) continue;
            out->pix[i] = 128 + pal * 16 + c;
            out->pri[i] = pri;
        }
    }
}

static void render_sprites(Ppu *p, int line, LayerLine *out, int x0, int width) {
    memset(out->pix, 0, width);
    int sizeSel = p->obsel >> 5;
    int first = p->oamPriorityRotation ? ((p->oamAddrReload >> 2) & 0x7F) : 0;
    int extra = -x0;

    // Hardware range evaluation (32 sprites) exactly as on the console.
    int inRange[32];
    bool listed[128] = {false};
    int count = 0;
    bool overflow = false;
    for (int n = 0; n < 128; n++) {
        int i = (first + n) & 0x7F;
        ObjInfo o;
        obj_info(p, i, sizeSel, line, &o);
        int x = o.x >= 256 ? o.x - 512 : o.x;
        if (o.row >= o.h) continue;
        if (x <= -o.w && x != -256) continue;
        if (count == 32) {
            overflow = true;
            break;
        }
        listed[i] = true;
        inRange[count++] = i;
    }

    // Widescreen-only sprites (entirely off the classic screen) go underneath.
    if (extra > 0 && !overflow) {
        for (int n = 127; n >= 0; n--) {
            int i = (first + n) & 0x7F;
            if (listed[i]) continue;
            ObjInfo o;
            obj_info(p, i, sizeSel, line, &o);
            if (o.row >= o.h) continue;
            int x = o.x;
            if (x >= 256 + extra) x -= 512;
            if (x + o.w <= x0 || x >= x0 + width) continue;
            draw_obj(p, &o, x, x0, width, out, NULL);
        }
    }

    // Lowest OAM index wins: draw from the end of the list forwards.
    int tiles = 0;
    for (int k = count - 1; k >= 0 && tiles < 34; k--) {
        ObjInfo o;
        obj_info(p, inRange[k], sizeSel, line, &o);
        int x = o.x >= 256 ? o.x - 512 : o.x;
        draw_obj(p, &o, x, x0, width, out, &tiles);
    }
}

// -------------------------------------------------------------- composite
// Pixel i covers samples i*factor .. i*factor+factor-1. A layer shifted by
// `shift` samples reads its pixel at (sample + shift) / factor; `pad` is 1 for
// lines rendered with the extra left pixel.
static void merge(Screen *scr, const LayerLine *l, int layer, const uint8_t z[4], const uint8_t *mask,
                  const Ppu *p, int from, int to, int factor, int shift, int pad) {
    if (shift == 0) { // unshifted layers: one lookup per pixel
        for (int i = from; i < to; i++) {
            uint8_t c = l->pix[i + pad];
            if (!c || (mask && mask[i])) continue;
            uint8_t zz = z[layer == L_OBJ ? l->pri[i + pad] : (l->pri[i + pad] & 1)];
            uint16_t color = p->cgram[c];
            bool math = layer != L_OBJ || c >= 192;
            for (int j = i * factor, end = j + factor; j < end; j++) {
                if (zz > scr->z[j]) {
                    scr->z[j] = zz;
                    scr->color[j] = color;
                    scr->layer[j] = layer;
                    scr->objMath[j] = math;
                }
            }
        }
        return;
    }
    for (int i = from; i < to; i++) {
        if (mask && mask[i]) continue;
        for (int k = 0; k < factor; k++) {
            int src = i + (k + shift) / factor + pad;
            uint8_t c = l->pix[src];
            if (!c) continue;
            int j = i * factor + k;
            uint8_t zz = z[layer == L_OBJ ? l->pri[src] : (l->pri[src] & 1)];
            if (zz > scr->z[j]) {
                scr->z[j] = zz;
                scr->color[j] = p->cgram[c];
                scr->layer[j] = layer;
                scr->objMath[j] = layer != L_OBJ || c >= 192;
            }
        }
    }
}

static inline uint16_t blend(uint16_t a, uint16_t b, bool sub, bool half) {
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    int r, g, bl;
    if (sub) {
        r = ar - br; g = ag - bg; bl = ab - bb;
        if (r < 0) r = 0;
        if (g < 0) g = 0;
        if (bl < 0) bl = 0;
        if (half) { r >>= 1; g >>= 1; bl >>= 1; }
    } else {
        r = ar + br; g = ag + bg; bl = ab + bb;
        if (half) { r >>= 1; g >>= 1; bl >>= 1; }
        if (r > 31) r = 31;
        if (g > 31) g = 31;
        if (bl > 31) bl = 31;
    }
    return r | (g << 5) | (bl << 10);
}

static inline bool window_region(int mode, bool inside) {
    switch (mode) {
    case 0: return false;
    case 1: return !inside;
    case 2: return inside;
    default: return true;
    }
}

void ppu_render_line(Snes *s, int line) {
    if (!s->framebuffer || line < 1 || line > VISIBLE_LINES) return;
    if (s->capture) interp_capture_line(s, line);
    if (line == 1 && snes_subpixel(s)->factor > 1) subpixel_frame(s);
    ppu_draw_line(s, &s->ppu, line, s->framebuffer + (size_t)(line - 1) * s->fbPitch);
}

void ppu_draw_line(Snes *s, Ppu *p, int line, uint32_t *dst) {
    const int extra = s->wide.extra;
    const int x0 = -extra;
    const int width = 256 + 2 * extra;
    const SubpixelScroll *sub = snes_subpixel(s);
    const int factor = sub->factor > 1 ? sub->factor : 1;
    const int samples = width * factor;
    if (p->inidisp & 0x80) {
        for (int i = 0; i < samples; i++) dst[i] = 0xFF000000u;
        return;
    }

    // Columns where foreground layers hold valid data this frame (game-aware,
    // see widescreen.c). Outside them only background layers and the
    // backdrop show, so the screen is always filled with game content.
    int fgFrom = extra - s->wide.fgLeft;
    int fgTo = extra + 256 + s->wide.fgRight;
    if (fgFrom < 0) fgFrom = 0;
    if (fgTo > width) fgTo = width;
    bool bg3InMargins = line >= s->wide.bg3MarginStartLine;

    static Screen mainS, subS;
    static LayerLine layers[5];
    static uint8_t masks[6][MAXW];

    int mode = p->bgmode & 7;
    bool bg3hi = p->bgmode & 8;
    // z-order per layer for tile priority 0/1 (BG) or 0-3 (OBJ). Larger = front.
    uint8_t z[5][4] = {{0}};
    int bpp[4] = {0, 0, 0, 0};
    switch (mode) {
    case 0:
        bpp[0] = bpp[1] = bpp[2] = bpp[3] = 2;
        z[L_BG1][0] = 8; z[L_BG1][1] = 11;
        z[L_BG2][0] = 7; z[L_BG2][1] = 10;
        z[L_BG3][0] = 2; z[L_BG3][1] = 5;
        z[L_BG4][0] = 1; z[L_BG4][1] = 4;
        break;
    case 1:
        bpp[0] = bpp[1] = 4; bpp[2] = 2;
        z[L_BG1][0] = 8; z[L_BG1][1] = 11;
        z[L_BG2][0] = 7; z[L_BG2][1] = 10;
        z[L_BG3][0] = 2; z[L_BG3][1] = bg3hi ? 13 : 5;
        break;
    case 2: case 3: case 4: case 5: case 6:
        bpp[0] = (mode == 3 || mode == 4) ? 8 : 4;
        bpp[1] = (mode == 4 || mode == 5) ? 2 : (mode == 6 ? 0 : 4);
        z[L_BG1][0] = 7; z[L_BG1][1] = 11;
        z[L_BG2][0] = 5; z[L_BG2][1] = 9;
        break;
    case 7:
        z[L_BG1][0] = z[L_BG1][1] = 5;
        break;
    }
    z[L_OBJ][0] = 3; z[L_OBJ][1] = 6; z[L_OBJ][2] = 9; z[L_OBJ][3] = 12;
    if (mode >= 2) { z[L_OBJ][0] = 6; z[L_OBJ][1] = 8; z[L_OBJ][2] = 10; z[L_OBJ][3] = 12; }
    if (mode == 7) { z[L_OBJ][0] = 1; z[L_OBJ][1] = 6; z[L_OBJ][2] = 9; z[L_OBJ][3] = 12; }

    for (int i = 0; i < samples; i++) {
        mainS.z[i] = 0;
        mainS.color[i] = p->cgram[0];
        mainS.layer[i] = L_BACK;
        mainS.objMath[i] = 1;
        subS.z[i] = 0;
        subS.color[i] = p->fixedColor;
        subS.layer[i] = L_BACK;
    }

    const uint8_t tm = p->tm & ~s->debugHideLayers, ts = p->ts & ~s->debugHideLayers;
    uint8_t used = tm | ts;
    for (int l = 0; l < 6; l++) window_mask(p, l, masks[l], x0, width);

    for (int bg = 0; bg < 4; bg++) {
        if (!(used & (1 << bg))) continue;
        int shift = 0, pad = 0;
        if (mode == 7) {
            if (bg != 0) continue;
            render_mode7(p, line, &layers[0], x0, width);
        } else {
            if (!bpp[bg]) continue;
            // Smoothed parallax layers: whole pixels go into the scroll, the
            // remainder becomes a sample shift.
            uint16_t gameHofs = p->hofs[bg];
            if (factor > 1 && sub->valid && !sub->locked[bg]) {
                int total = gameHofs * factor + (int)(sub->offset[bg] * factor + 0.5f);
                p->hofs[bg] = (uint16_t)((total / factor) & 0x3FF);
                shift = total % factor;
            }
            render_bg(p, bg, bpp[bg], line, &layers[bg], mode == 0 ? bg * 32 : 0, x0 - 1, width + 2,
                      s->wide.edgeExtendMask & (1 << bg));
            p->hofs[bg] = gameHofs;
            pad = 1;
        }
        int from = 0, to = width;
        if (s->wide.fgMask & (1 << bg)) {
            from = fgFrom;
            to = fgTo;
        }
        // BG3 carries the HUD and screen frames: keep it centred where needed.
        if (bg == 2 && !bg3InMargins) {
            from = extra;
            to = extra + 256;
        }
        if (tm & (1 << bg))
            merge(&mainS, &layers[bg], bg, z[bg], (p->tmw & (1 << bg)) ? masks[bg] : NULL, p, from, to,
                  factor, shift, pad);
        if (ts & (1 << bg))
            merge(&subS, &layers[bg], bg, z[bg], (p->tsw & (1 << bg)) ? masks[bg] : NULL, p, from, to,
                  factor, shift, pad);
    }
    if (used & 0x10) {
        render_sprites(p, line, &layers[L_OBJ], x0, width);
        int from = (s->wide.fgMask & 0x10) ? fgFrom : 0;
        int to = (s->wide.fgMask & 0x10) ? fgTo : width;
        if (tm & 0x10)
            merge(&mainS, &layers[L_OBJ], L_OBJ, z[L_OBJ], (p->tmw & 0x10) ? masks[4] : NULL, p, from, to,
                  factor, 0, 0);
        if (ts & 0x10)
            merge(&subS, &layers[L_OBJ], L_OBJ, z[L_OBJ], (p->tsw & 0x10) ? masks[4] : NULL, p, from, to,
                  factor, 0, 0);
    }

    // Brightness lookup: 5-bit channel -> 8-bit with master brightness.
    int bright = p->inidisp & 0xF;
    uint8_t lut[32];
    for (int i = 0; i < 32; i++) {
        int v = (i << 3) | (i >> 2);
        lut[i] = (uint8_t)(v * bright / 15);
    }

    int clipMode = p->cgwsel >> 6;
    int preventMode = (p->cgwsel >> 4) & 3;
    bool addSub = p->cgwsel & 2;
    bool subtract = p->cgadsub & 0x80;
    bool halfEnable = p->cgadsub & 0x40;
    const uint8_t *colorWin = masks[5];

    for (int j = 0; j < samples; j++) {
        int i = j / factor;
        uint16_t c = mainS.color[j];
        int layer = mainS.layer[j];
        bool inside = colorWin[i];
        bool clipped = window_region(clipMode, inside);
        if (clipped) c = 0;
        bool mathOn = !window_region(preventMode, inside) && (p->cgadsub & (1 << layer)) && mainS.objMath[j];
        if (mathOn) {
            uint16_t other;
            bool half = halfEnable && !clipped;
            if (addSub && subS.layer[j] != L_BACK) {
                other = subS.color[j];
            } else {
                other = p->fixedColor;
                if (addSub) half = false;
            }
            c = blend(c, other, subtract, half);
        }
        if (s->debugTransparentBackdrop && layer == L_BACK) {
            dst[j] = 0;
            continue;
        }
        dst[j] = 0xFF000000u | ((uint32_t)lut[c & 31] << 16) | ((uint32_t)lut[(c >> 5) & 31] << 8) |
                 lut[(c >> 10) & 31];
    }
}
