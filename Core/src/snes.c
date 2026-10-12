// System bus, memory map (LoROM), CPU-side I/O, DMA/HDMA and the scanline
// scheduler that ties the CPU, PPU and APU together.
#include <stdlib.h>

#include "smw_core.h"
#include "snes.h"

static const uint8_t kDmaOffsets[8][4] = {
    {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
    {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
};
static const uint8_t kDmaUnitSize[8] = {1, 2, 2, 4, 4, 4, 2, 4};

enum {
    RENDER_POS = 512,   // dot 128: registers written during the previous HBlank are visible
    HDMA_POS = 1104,    // dot 276
    HBLANK_START = 1096 // dot 274
};

struct SMWCore {
    Snes snes;
    uint8_t *romCopy;
    bool doneIrq, doneRender, doneHdma;
};

static inline SMWCore *core_of(Snes *s) { return (SMWCore *)s; }

// ------------------------------------------------------------- memory map
int bus_speed(Snes *s, uint32_t addr) {
    uint8_t bank = addr >> 16;
    uint16_t a = addr & 0xFFFF;
    if (bank >= 0x40 && bank < 0x80) return 8;
    if (bank >= 0xC0) return s->fastRom ? 6 : 8;
    if (a & 0x8000) return ((bank & 0x80) && s->fastRom) ? 6 : 8;
    if (a < 0x2000) return 8;
    if (a < 0x4000) return 6;
    if (a < 0x4200) return 12;
    if (a < 0x6000) return 6;
    return 8;
}

static inline uint8_t rom_read(Snes *s, uint8_t bank, uint16_t a) {
    uint32_t off = (((uint32_t)(bank & 0x7F)) << 15) | (a & 0x7FFF);
    return s->rom[off % s->romSize];
}

static inline bool is_sram(uint8_t bank, uint16_t a) {
    uint8_t b = bank & 0x7F;
    return b >= 0x70 && b < 0x7E && a < 0x8000;
}

static inline uint32_t sram_offset(Snes *s, uint8_t bank, uint16_t a) {
    return ((((uint32_t)(bank & 0x0F)) << 15) | a) & (s->sramSize - 1);
}

static bool in_hblank(Snes *s) { return s->hpos < 4 || s->hpos >= HBLANK_START; }

static void update_irq_line(Snes *s) {
    s->cpu.irqLine = s->irqFlag && (s->nmitimen & 0x30);
}

static int irq_position(Snes *s) {
    switch ((s->nmitimen >> 4) & 3) {
    case 1:
        return s->htime * 4 + 14;
    case 2:
        return s->vpos == s->vtime ? 10 : 1 << 30;
    case 3:
        return s->vpos == s->vtime ? s->htime * 4 + 14 : 1 << 30;
    default:
        return 1 << 30;
    }
}

static void schedule(Snes *s) {
    SMWCore *c = core_of(s);
    int next = LINE_CYCLES;
    if (!c->doneIrq) {
        int p = irq_position(s);
        if (p < next) next = p;
    }
    if (!c->doneRender && RENDER_POS < next) next = RENDER_POS;
    if (!c->doneHdma && HDMA_POS < next) next = HDMA_POS;
    s->nextEvent = next;
}

static uint8_t cpu_io_read(Snes *s, uint16_t a) {
    switch (a) {
    case 0x4210: {
        uint8_t v = (s->nmiFlag ? 0x80 : 0) | 0x02 | (s->openBus & 0x70);
        s->nmiFlag = false;
        return v;
    }
    case 0x4211: {
        uint8_t v = (s->irqFlag ? 0x80 : 0) | (s->openBus & 0x7F);
        s->irqFlag = false;
        update_irq_line(s);
        return v;
    }
    case 0x4212:
        return (s->inVBlank ? 0x80 : 0) | (in_hblank(s) ? 0x40 : 0) | (s->autoJoyBusy > 0 ? 1 : 0) |
               (s->openBus & 0x3E);
    case 0x4213: return s->wrio;
    case 0x4214: return s->divQuotient & 0xFF;
    case 0x4215: return s->divQuotient >> 8;
    case 0x4216: return s->mulDivResult & 0xFF;
    case 0x4217: return s->mulDivResult >> 8;
    default:
        if (a >= 0x4218 && a < 0x4220) {
            uint16_t j = s->joyLatched[(a - 0x4218) >> 1];
            return (a & 1) ? (j >> 8) : (j & 0xFF);
        }
        return s->openBus;
    }
}

static void cpu_io_write(Snes *s, uint16_t a, uint8_t v) {
    switch (a) {
    case 0x4200:
        if ((v & 0x80) && !(s->nmitimen & 0x80) && s->nmiFlag) s->cpu.nmiPending = true;
        s->nmitimen = v;
        if (!(v & 0x30)) s->irqFlag = false;
        update_irq_line(s);
        schedule(s);
        break;
    case 0x4201:
        if ((s->wrio & 0x80) && !(v & 0x80)) ppu_latch_counters(s);
        s->wrio = v;
        break;
    case 0x4202: s->mulA = v; break;
    case 0x4203:
        s->mulB = v;
        s->mulDivResult = s->mulA * v;
        s->divQuotient = v;
        break;
    case 0x4204: s->dividend = (s->dividend & 0xFF00) | v; break;
    case 0x4205: s->dividend = (s->dividend & 0x00FF) | (v << 8); break;
    case 0x4206:
        if (v == 0) {
            s->divQuotient = 0xFFFF;
            s->mulDivResult = s->dividend;
        } else {
            s->divQuotient = s->dividend / v;
            s->mulDivResult = s->dividend % v;
        }
        break;
    case 0x4207: s->htime = (s->htime & 0x100) | v; schedule(s); break;
    case 0x4208: s->htime = (s->htime & 0xFF) | ((v & 1) << 8); schedule(s); break;
    case 0x4209: s->vtime = (s->vtime & 0x100) | v; schedule(s); break;
    case 0x420A: s->vtime = (s->vtime & 0xFF) | ((v & 1) << 8); schedule(s); break;
    case 0x420B: dma_run(s, v); break;
    case 0x420C: s->hdmaen = v; break;
    case 0x420D: s->fastRom = v & 1; break;
    default: break;
    }
}

static uint8_t dma_reg_read(Snes *s, uint16_t a) {
    DmaChannel *d = &s->dma[(a >> 4) & 7];
    switch (a & 0xF) {
    case 0x0: return d->params;
    case 0x1: return d->bAddr;
    case 0x2: return d->aAddr & 0xFF;
    case 0x3: return d->aAddr >> 8;
    case 0x4: return d->aBank;
    case 0x5: return d->size & 0xFF;
    case 0x6: return d->size >> 8;
    case 0x7: return d->indBank;
    case 0x8: return d->tableAddr & 0xFF;
    case 0x9: return d->tableAddr >> 8;
    case 0xA: return d->lineCounter;
    case 0xB:
    case 0xF: return d->unused;
    default: return s->openBus;
    }
}

static void dma_reg_write(Snes *s, uint16_t a, uint8_t v) {
    DmaChannel *d = &s->dma[(a >> 4) & 7];
    switch (a & 0xF) {
    case 0x0: d->params = v; break;
    case 0x1: d->bAddr = v; break;
    case 0x2: d->aAddr = (d->aAddr & 0xFF00) | v; break;
    case 0x3: d->aAddr = (d->aAddr & 0x00FF) | (v << 8); break;
    case 0x4: d->aBank = v; break;
    case 0x5: d->size = (d->size & 0xFF00) | v; break;
    case 0x6: d->size = (d->size & 0x00FF) | (v << 8); break;
    case 0x7: d->indBank = v; break;
    case 0x8: d->tableAddr = (d->tableAddr & 0xFF00) | v; break;
    case 0x9: d->tableAddr = (d->tableAddr & 0x00FF) | (v << 8); break;
    case 0xA: d->lineCounter = v; break;
    case 0xB:
    case 0xF: d->unused = v; break;
    default: break;
    }
}

uint8_t bus_read(Snes *s, uint32_t addr) {
    uint8_t bank = addr >> 16;
    uint16_t a = addr & 0xFFFF;
    uint8_t v;
    if (bank == 0x7E || bank == 0x7F) {
        v = s->wram[addr & 0x1FFFF];
    } else if ((bank & 0x7F) < 0x40) {
        if (a < 0x2000) {
            v = s->wram[a];
        } else if (a >= 0x8000) {
            v = rom_read(s, bank, a);
        } else if (a >= 0x2100 && a < 0x2140) {
            v = ppu_read(s, a & 0xFF);
        } else if (a >= 0x2140 && a < 0x2180) {
            snes_catch_up_apu(s);
            v = spc_cpu_read_port(s, a & 3);
        } else if (a == 0x2180) {
            v = s->wram[s->wramAddr];
            s->wramAddr = (s->wramAddr + 1) & 0x1FFFF;
        } else if (a == 0x4016 || a == 0x4017) {
            v = (s->openBus & 0xFC);
        } else if (a >= 0x4200 && a < 0x4220) {
            v = cpu_io_read(s, a);
        } else if (a >= 0x4300 && a < 0x4380) {
            v = dma_reg_read(s, a);
        } else {
            v = s->openBus;
        }
    } else if (is_sram(bank, a)) {
        v = s->sramSize ? s->sram[sram_offset(s, bank, a)] : s->openBus;
    } else {
        v = rom_read(s, bank, a);
    }
    s->openBus = v;
    return v;
}

void bus_write(Snes *s, uint32_t addr, uint8_t v) {
    uint8_t bank = addr >> 16;
    uint16_t a = addr & 0xFFFF;
    s->openBus = v;
    if (s->debugForceCount) {
        uint32_t w = (bank == 0x7E || bank == 0x7F) ? (addr & 0x1FFFF)
                     : (((bank & 0x7F) < 0x40 && a < 0x2000) ? a : 0xFFFFFFFF);
        for (int i = 0; i < s->debugForceCount; i++)
            if (w == s->debugForceAddr[i]) v = s->debugForceValue[i];
    }
    if (bank == 0x7E || bank == 0x7F) {
        s->wram[addr & 0x1FFFF] = v;
    } else if ((bank & 0x7F) < 0x40) {
        if (a < 0x2000) {
            s->wram[a] = v;
        } else if (a >= 0x2100 && a < 0x2140) {
            ppu_write(s, a & 0xFF, v);
        } else if (a >= 0x2140 && a < 0x2180) {
            snes_catch_up_apu(s);
            spc_cpu_write_port(s, a & 3, v);
            int port = a & 3;
            if (v && v != s->apuPortWritten[port] && s->soundEventCount < 16)
                s->soundEvents[s->soundEventCount++] = (uint16_t)((port << 8) | v);
            s->apuPortWritten[port] = v;
        } else if (a == 0x2180) {
            s->wram[s->wramAddr] = v;
            s->wramAddr = (s->wramAddr + 1) & 0x1FFFF;
        } else if (a == 0x2181) {
            s->wramAddr = (s->wramAddr & 0x1FF00) | v;
        } else if (a == 0x2182) {
            s->wramAddr = (s->wramAddr & 0x100FF) | (v << 8);
        } else if (a == 0x2183) {
            s->wramAddr = (s->wramAddr & 0x0FFFF) | ((v & 1) << 16);
        } else if (a >= 0x4200 && a < 0x4220) {
            cpu_io_write(s, a, v);
        } else if (a >= 0x4300 && a < 0x4380) {
            dma_reg_write(s, a, v);
        }
    } else if (is_sram(bank, a)) {
        if (s->sramSize) {
            uint8_t *cell = &s->sram[sram_offset(s, bank, a)];
            if (*cell != v) s->sramDirty = true;
            *cell = v;
        }
    }
}

// -------------------------------------------------------------------- DMA
static bool a_bus_blocked(uint32_t addr) {
    uint8_t bank = addr >> 16;
    uint16_t a = addr & 0xFFFF;
    if ((bank & 0x7F) >= 0x40) return false;
    return (a >= 0x2100 && a < 0x2200) || (a >= 0x4000 && a < 0x4400);
}

void dma_run(Snes *s, uint8_t channels) {
    if (!channels) return;
    snes_tick(s, 8);
    for (int ch = 0; ch < 8; ch++) {
        if (!(channels & (1 << ch))) continue;
        DmaChannel *d = &s->dma[ch];
        snes_tick(s, 8);
        int mode = d->params & 7;
        bool toA = d->params & 0x80;
        bool fixed = d->params & 0x08;
        int step = (d->params & 0x10) ? -1 : 1;
        uint32_t count = d->size ? d->size : 0x10000;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t aAddr = ((uint32_t)d->aBank << 16) | d->aAddr;
            uint32_t bAddr = 0x2100 | (uint8_t)(d->bAddr + kDmaOffsets[mode][i & 3]);
            if (toA) {
                uint8_t v = bus_read(s, bAddr);
                if (!a_bus_blocked(aAddr)) bus_write(s, aAddr, v);
            } else {
                uint8_t v = a_bus_blocked(aAddr) ? s->openBus : bus_read(s, aAddr);
                bus_write(s, bAddr, v);
            }
            snes_tick(s, 8);
            if (!fixed) d->aAddr += step;
        }
        d->size = 0;
    }
}

static void hdma_init(Snes *s) {
    for (int ch = 0; ch < 8; ch++) {
        DmaChannel *d = &s->dma[ch];
        d->hdmaTerminated = true;
        if (!(s->hdmaen & (1 << ch))) continue;
        d->hdmaTerminated = false;
        d->tableAddr = d->aAddr;
        d->lineCounter = bus_read(s, ((uint32_t)d->aBank << 16) | d->tableAddr++);
        if (d->params & 0x40) {
            uint8_t lo = bus_read(s, ((uint32_t)d->aBank << 16) | d->tableAddr++);
            uint8_t hi = bus_read(s, ((uint32_t)d->aBank << 16) | d->tableAddr++);
            d->size = lo | (hi << 8);
        }
        if (d->lineCounter == 0) d->hdmaTerminated = true;
        d->hdmaDoTransfer = true;
        s->hpos += 8;
    }
}

static void hdma_run_line(Snes *s) {
    int cycles = 0;
    for (int ch = 0; ch < 8; ch++) {
        DmaChannel *d = &s->dma[ch];
        if (!(s->hdmaen & (1 << ch)) || d->hdmaTerminated) continue;
        int mode = d->params & 7;
        bool indirect = d->params & 0x40;
        cycles += 8;
        if (d->hdmaDoTransfer) {
            for (int i = 0; i < kDmaUnitSize[mode]; i++) {
                uint32_t src = indirect ? (((uint32_t)d->indBank << 16) | d->size++)
                                        : (((uint32_t)d->aBank << 16) | d->tableAddr++);
                uint8_t v = bus_read(s, src);
                bus_write(s, 0x2100 | (uint8_t)(d->bAddr + kDmaOffsets[mode][i]), v);
                cycles += 8;
            }
        }
        d->lineCounter--;
        d->hdmaDoTransfer = d->lineCounter & 0x80;
        if ((d->lineCounter & 0x7F) == 0) {
            d->lineCounter = bus_read(s, ((uint32_t)d->aBank << 16) | d->tableAddr++);
            if (indirect) {
                uint8_t lo = bus_read(s, ((uint32_t)d->aBank << 16) | d->tableAddr++);
                uint8_t hi = bus_read(s, ((uint32_t)d->aBank << 16) | d->tableAddr++);
                d->size = lo | (hi << 8);
                cycles += 16;
            }
            if (d->lineCounter == 0) d->hdmaTerminated = true;
            d->hdmaDoTransfer = true;
            cycles += 8;
        }
    }
    s->hpos += cycles;
    s->masterCycles += cycles;
}

// -------------------------------------------------------------- scheduler
static void start_line(Snes *s) {
    SMWCore *c = core_of(s);
    s->vpos++;
    if (s->vpos == FRAME_LINES) {
        s->vpos = 0;
        s->inVBlank = false;
        s->nmiFlag = false;
        s->frameDone = true;
        s->frames++;
        s->ppu.mosaicStartLine = 1;
    }
    c->doneIrq = false;
    c->doneRender = !(s->vpos >= 1 && s->vpos <= VISIBLE_LINES);
    c->doneHdma = s->vpos > VISIBLE_LINES;
    if (s->vpos == 0) hdma_init(s);
    if (s->vpos == VISIBLE_LINES + 1) {
        s->inVBlank = true;
        s->nmiFlag = true;
        if (s->nmitimen & 0x80) s->cpu.nmiPending = true;
        if (!(s->ppu.inidisp & 0x80)) s->ppu.oamAddr = s->ppu.oamAddrReload;
        if (s->nmitimen & 1) {
            for (int i = 0; i < 4; i++) s->joyLatched[i] = s->buttons[i];
            s->autoJoyBusy = 4224;
        }
    }
}

void snes_tick(Snes *s, int cycles) {
    s->hpos += cycles;
    s->masterCycles += cycles;
    if (s->autoJoyBusy > 0) s->autoJoyBusy -= cycles;
    while (s->hpos >= s->nextEvent) {
        SMWCore *c = core_of(s);
        int irqPos = c->doneIrq ? (1 << 30) : irq_position(s);
        int renderPos = c->doneRender ? (1 << 30) : RENDER_POS;
        int hdmaPos = c->doneHdma ? (1 << 30) : HDMA_POS;
        if (irqPos <= renderPos && irqPos <= hdmaPos && irqPos <= s->hpos && irqPos < LINE_CYCLES) {
            c->doneIrq = true;
            s->irqFlag = true;
            update_irq_line(s);
        } else if (renderPos <= hdmaPos && renderPos <= s->hpos) {
            c->doneRender = true;
            ppu_render_line(s, s->vpos);
        } else if (hdmaPos <= s->hpos) {
            c->doneHdma = true;
            hdma_run_line(s);
        } else if (s->hpos >= LINE_CYCLES) {
            s->hpos -= LINE_CYCLES;
            start_line(s);
        } else {
            c->doneIrq = true; // IRQ position beyond this line
        }
        schedule(s);
    }
}

void snes_catch_up_apu(Snes *s) {
    int64_t target = (int64_t)(s->masterCycles * 128000ull / 2684659ull);
    spc_run_until(s, target);
}

// ------------------------------------------------------------- public API
SMWCore *smw_create(const uint8_t *rom, size_t length) {
    if (!rom || length < 0x8000) return NULL;
    SMWCore *c = calloc(1, sizeof(SMWCore));
    if (!c) return NULL;
    c->romCopy = malloc(length);
    memcpy(c->romCopy, rom, length);
    Snes *s = &c->snes;
    s->rom = c->romCopy;
    s->romSize = (uint32_t)length;
    uint8_t sramCode = rom[0x7FD8];
    s->sramSize = sramCode ? (1024u << sramCode) : 0;
    if (s->sramSize > sizeof(s->sram)) s->sramSize = sizeof(s->sram);
    memset(s->sram, 0, sizeof(s->sram));
    smw_reset(c, true);
    return c;
}

void smw_destroy(SMWCore *c) {
    if (!c) return;
    free(c->snes.capture);
    free(c->romCopy);
    free(c);
}

void smw_reset(SMWCore *c, bool hard) {
    Snes *s = &c->snes;
    if (hard) {
        // Deterministic power-on WRAM pattern.
        memset(s->wram, 0x55, sizeof(s->wram));
    }
    memset(s->dma, 0xFF, sizeof(s->dma));
    for (int i = 0; i < 8; i++) {
        s->dma[i].hdmaTerminated = true;
        s->dma[i].hdmaDoTransfer = false;
    }
    s->nmitimen = 0;
    s->wrio = 0xFF;
    s->htime = s->vtime = 0x1FF;
    s->hdmaen = 0;
    s->fastRom = false;
    s->nmiFlag = s->irqFlag = false;
    s->inVBlank = false;
    s->autoJoyBusy = 0;
    s->wramAddr = 0;
    s->hpos = 0;
    s->vpos = 0;
    s->frameDone = false;
    c->doneIrq = c->doneRender = true;
    c->doneHdma = false;
    ppu_reset(&s->ppu);
    spc_reset(s);
    cpu_reset(s);
    schedule(s);
}

void smw_set_framebuffer(SMWCore *c, uint32_t *pixels, int pitch) {
    c->snes.framebuffer = pixels;
    c->snes.fbPitch = pitch;
}

void smw_set_widescreen(SMWCore *c, int extra) {
    if (extra < 0) extra = 0;
    if (extra > PPU_MAX_EXTRA) extra = PPU_MAX_EXTRA;
    c->snes.wide.extra = extra;
    if (c->snes.wide.fgLeft > extra) c->snes.wide.fgLeft = extra;
    if (c->snes.wide.fgRight > extra) c->snes.wide.fgRight = extra;
}

void smw_debug_layers(SMWCore *c, uint8_t hideMask, bool transparentBackdrop) {
    c->snes.debugHideLayers = hideMask;
    c->snes.debugTransparentBackdrop = transparentBackdrop;
}

void smw_debug_force_wram(SMWCore *c, uint32_t address, int value) {
    Snes *s = &c->snes;
    if (value < 0) {
        s->debugForceCount = 0;
        return;
    }
    if (s->debugForceCount < 4) {
        s->debugForceAddr[s->debugForceCount] = address & 0x1FFFF;
        s->debugForceValue[s->debugForceCount++] = (uint8_t)value;
        s->wram[address & 0x1FFFF] = (uint8_t)value;
    }
}

const uint16_t *smw_debug_cgram(const SMWCore *c) { return c->snes.ppu.cgram; }

int smw_frame_width(const SMWCore *c) { return 256 + 2 * c->snes.wide.extra; }

void smw_set_buttons(SMWCore *c, int port, uint16_t buttons) {
    if (port >= 0 && port < 4) c->snes.buttons[port] = buttons;
}

void smw_run_frame(SMWCore *c) {
    Snes *s = &c->snes;
    s->frameDone = false;
    s->soundEventCount = 0;
    widescreen_frame(s);
    while (!s->frameDone) cpu_step(s);
    snes_catch_up_apu(s);
}

int smw_read_audio(SMWCore *c, int16_t *out, int maxFrames) {
    Dsp *d = &c->snes.dsp;
    const uint32_t cap = sizeof(d->out) / sizeof(d->out[0]) / 2;
    int n = 0;
    while (n < maxFrames && d->outRead != d->outWrite) {
        uint32_t i = d->outRead % cap;
        out[n * 2] = d->out[i * 2];
        out[n * 2 + 1] = d->out[i * 2 + 1];
        d->outRead++;
        n++;
    }
    return n;
}

int smw_sound_event_count(const SMWCore *c) { return c->snes.soundEventCount; }

uint16_t smw_sound_event(const SMWCore *c, int index) {
    return (index >= 0 && index < c->snes.soundEventCount) ? c->snes.soundEvents[index] : 0;
}

void smw_set_audio_interpolation(SMWCore *c, SMWAudioInterpolation mode) { c->snes.dsp.interpMode = (uint8_t)mode; }

const uint8_t *smw_wram(const SMWCore *c) { return c->snes.wram; }

uint8_t *smw_sram(SMWCore *c, size_t *length) {
    if (length) *length = c->snes.sramSize;
    return c->snes.sram;
}

bool smw_sram_take_dirty(SMWCore *c) {
    bool dirty = c->snes.sramDirty;
    c->snes.sramDirty = false;
    return dirty;
}

uint64_t smw_frame_count(const SMWCore *c) { return c->snes.frames; }

uint32_t smw_state_wram_crc(const void *state) {
    // CRC-32 of the work RAM inside a state blob: identical on every device
    // that simulated the same frames (pointers and host fields are excluded).
    const uint8_t *p = (const uint8_t *)state + offsetof(Snes, wram);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < sizeof(((Snes *)0)->wram); i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

size_t smw_state_size(void) { return sizeof(Snes) + 3; }

void smw_save_state(const SMWCore *c, void *buffer) {
    uint8_t *p = buffer;
    memcpy(p, &c->snes, sizeof(Snes));
    p[sizeof(Snes)] = c->doneIrq;
    p[sizeof(Snes) + 1] = c->doneRender;
    p[sizeof(Snes) + 2] = c->doneHdma;
}

bool smw_load_state(SMWCore *c, const void *buffer, size_t length) {
    if (length != smw_state_size()) return false;
    Snes *s = &c->snes;
    const uint8_t *rom = s->rom;
    uint32_t romSize = s->romSize, sramSize = s->sramSize;
    uint32_t *fb = s->framebuffer;
    int pitch = s->fbPitch;
    Widescreen wide = s->wide;
    uint8_t hide = s->debugHideLayers;
    InterpCapture *capture = s->capture;
    bool sramDirty = s->sramDirty;
    // Battery RAM is never rolled back by a state load (or rewind/run-ahead):
    // the game's own save always stays the newest one.
    static uint8_t sram[sizeof(s->sram)];
    memcpy(sram, s->sram, sizeof(sram));
    bool clearBackdrop = s->debugTransparentBackdrop;
    const uint8_t *p = buffer;
    memcpy(s, p, sizeof(Snes));
    c->doneIrq = p[sizeof(Snes)];
    c->doneRender = p[sizeof(Snes) + 1];
    c->doneHdma = p[sizeof(Snes) + 2];
    s->rom = rom;
    s->romSize = romSize;
    s->sramSize = sramSize;
    s->framebuffer = fb;
    s->fbPitch = pitch;
    s->wide = wide;
    s->debugHideLayers = hide;
    s->capture = capture;
    s->sramDirty = sramDirty;
    memcpy(s->sram, sram, sizeof(sram));
    s->debugTransparentBackdrop = clearBackdrop;
    return true;
}
