// Internal machine model. Every struct here is plain data (no pointers except
// where noted in Snes) so a save state is a straight memory copy.
#ifndef SNES_INTERNAL_H
#define SNES_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MASTER_CLOCK 21477272
#define APU_CLOCK 1024000
#define LINE_CYCLES 1364
#define FRAME_LINES 262
#define VISIBLE_LINES 224
#define PPU_MAX_EXTRA 128 // widescreen margin per side, in pixels

typedef struct Snes Snes;

// ---------------------------------------------------------------- 65C816 CPU
typedef struct {
    uint16_t a, x, y, sp, dp, pc;
    uint8_t db, pb;
    uint8_t p;   // NVMXDIZC
    bool e;      // emulation mode
    bool waiting;
    bool stopped;
    bool nmiPending;
    bool irqLine;
} Cpu;

void cpu_reset(Snes *s);
void cpu_step(Snes *s);

// ------------------------------------------------------------- SPC700 + DSP
typedef struct {
    uint8_t ram[0x10000];
    uint16_t pc;
    uint8_t a, x, y, sp, psw;
    bool stopped;
    // I/O
    uint8_t portIn[4];   // written by the main CPU
    uint8_t portOut[4];  // written by the SPC
    uint8_t control;
    uint8_t dspAddr;
    bool iplEnabled;
    struct {
        bool enabled;
        uint8_t target;
        uint8_t counter;
        uint8_t output;
        int stage;      // cycles accumulated towards the next tick
        int period;     // 128 (8 kHz) or 16 (64 kHz)
    } timer[3];
    int64_t cycles;     // SPC cycles executed in total
    int sampleClock;    // cycles until the next DSP sample
} Spc;

typedef struct {
    int16_t buf[16];      // decoded samples of the current BRR block
    int16_t hist[3];      // BRR filter history (most recent first)
    int16_t prevTail[3];  // last three samples of the previous block
    uint16_t blockAddr;
    int blockPos;         // 0..15 within buf
    uint32_t interp;      // 12-bit fractional position between samples
    int env;              // 0..0x7FF
    int hiddenEnv;
    int envMode;          // 0 release, 1 attack, 2 decay, 3 sustain
    int konDelay;
    int prevOut;          // for pitch modulation
    bool active;
} DspVoice;

typedef struct {
    uint8_t regs[128];
    DspVoice voice[8];
    int counter;
    int noise;
    int echoOffset;
    int echoLength;
    int16_t echoHist[8][2];
    int echoHistPos;
    uint8_t konLatch;
    bool everyOther;
    uint8_t interpMode;
    // Output ring (interleaved stereo)
    int16_t out[8192 * 2];
    uint32_t outWrite, outRead;
} Dsp;

void spc_reset(Snes *s);
void spc_run_until(Snes *s, int64_t targetCycles);
uint8_t spc_cpu_read_port(Snes *s, int port);
void spc_cpu_write_port(Snes *s, int port, uint8_t value);
void dsp_reset(Dsp *d);
void dsp_write(Snes *s, uint8_t addr, uint8_t value);
uint8_t dsp_read(Snes *s, uint8_t addr);
void dsp_sample(Snes *s);

// ---------------------------------------------------------------------- PPU
typedef struct {
    uint16_t vram[0x8000];
    uint16_t cgram[256];
    uint8_t oam[544];

    uint8_t inidisp;
    uint8_t obsel;
    uint16_t oamAddr, oamAddrReload;
    bool oamPriorityRotation;
    uint8_t oamLatch;
    uint8_t bgmode;
    uint8_t mosaic;
    uint8_t bgsc[4];
    uint8_t bgnba[2];
    uint16_t hofs[4], vofs[4];
    uint8_t ofsLatch;
    uint8_t vmain;
    uint16_t vramAddr;
    uint16_t vramPrefetch;
    uint8_t m7sel;
    int16_t m7a, m7b, m7c, m7d, m7x, m7y, m7hofs, m7vofs;
    uint8_t m7latch;
    uint8_t cgAddr;
    bool cgHigh;
    uint8_t cgLatch;
    uint8_t wsel[3];     // W12SEL, W34SEL, WOBJSEL
    uint8_t wh[4];
    uint8_t wbglog, wobjlog;
    uint8_t tm, ts, tmw, tsw;
    uint8_t cgwsel, cgadsub;
    uint16_t fixedColor;
    uint8_t setini;
    uint8_t mpyResult[3];
    uint16_t hLatch, vLatch;
    bool hvLatched;
    bool hLatchHigh, vLatchHigh;
    uint8_t ppu1OpenBus, ppu2OpenBus;
    int mosaicStartLine;
} Ppu;

void ppu_reset(Ppu *p);
uint8_t ppu_read(Snes *s, uint8_t reg);
void ppu_write(Snes *s, uint8_t reg, uint8_t value);
void ppu_render_line(Snes *s, int line);
void ppu_draw_line(Snes *s, Ppu *p, int line, uint32_t *dst);
void ppu_latch_counters(Snes *s);

// ---------------------------------------------------------------------- DMA
typedef struct {
    uint8_t params;      // $43x0
    uint8_t bAddr;       // $43x1
    uint16_t aAddr;      // $43x2/3
    uint8_t aBank;       // $43x4
    uint16_t size;       // $43x5/6 (also HDMA indirect address)
    uint8_t indBank;     // $43x7
    uint16_t tableAddr;  // $43x8/9
    uint8_t lineCounter; // $43xA
    uint8_t unused;      // $43xB/$43xF
    bool hdmaDoTransfer;
    bool hdmaTerminated;
} DmaChannel;

// ----------------------------------------------------- Motion interpolation
// Per-line PPU state of the last two displayed frames, used to synthesise
// in-between frames for 120 Hz presentation (see interp.c).
#define PPU_REG_OFFSET offsetof(Ppu, inidisp)
#define PPU_REG_SIZE (sizeof(Ppu) - offsetof(Ppu, inidisp))

typedef struct {
    uint8_t regs[2][VISIBLE_LINES + 1][PPU_REG_SIZE];
    uint16_t cgram[2][VISIBLE_LINES + 1][256];
    uint16_t vram[2][0x8000];
    uint8_t oam[2][544];
    bool complete[2];
    int cur;
} InterpCapture;

void interp_capture_line(Snes *s, int line);

// ------------------------------------------------------------- Widescreen
typedef struct {
    int extra;              // margin rendered on each side (0 = classic 256 px)
    int fgLeft, fgRight;    // margin pixels where foreground layers are valid (animated)
    uint8_t fgMask;         // layers clipped to the foreground area (bits 0-3 BG1-4, bit 4 OBJ)
    int bg3MarginStartLine; // BG3 (HUD/frame layer) stays out of the margins above this line
    uint8_t edgeExtendMask; // BG layers whose margins repeat their outermost 16 columns
} Widescreen;

void widescreen_frame(Snes *s);

// ---------------------------------------------------------------- Machine
struct Snes {
    Cpu cpu;
    Ppu ppu;
    Spc spc;
    Dsp dsp;
    DmaChannel dma[8];

    uint8_t wram[0x20000];
    uint8_t sram[0x2000];
    uint32_t wramAddr;

    // CPU-side I/O
    uint8_t nmitimen;
    uint8_t wrio;
    uint16_t htime, vtime;
    uint8_t hdmaen;
    bool fastRom;
    uint8_t mulA, mulB;
    uint16_t dividend;
    uint16_t mulDivResult, divQuotient;
    bool nmiFlag;     // $4210 bit 7
    bool irqFlag;     // $4211 bit 7
    bool inVBlank;
    int autoJoyBusy;  // remaining master cycles
    uint16_t joyLatched[4];
    uint16_t buttons[4];
    uint8_t openBus;
    uint8_t apuPortWritten[4];
    uint16_t soundEvents[16];
    int soundEventCount;

    // Timing
    int hpos;           // master cycles into the current line
    int vpos;           // current scanline
    int nextEvent;
    int eventStage;
    uint64_t masterCycles;
    uint64_t frames;
    bool frameDone;
    bool irqFiredThisLine;

    // Not part of the save state — fixed up after a load.
    const uint8_t *rom;
    uint32_t romSize;
    uint32_t sramSize;
    uint32_t *framebuffer;
    int fbPitch;
    Widescreen wide;
    uint8_t debugHideLayers;      // tooling: bits 0-3 BG1-4, bit 4 OBJ
    bool debugTransparentBackdrop; // tooling: backdrop pixels get alpha 0
    InterpCapture *capture;        // motion interpolation (NULL = off)
    bool sramDirty;                // the game wrote battery-backed RAM
    int debugForceCount;           // tooling: WRAM bytes pinned to a value
    uint32_t debugForceAddr[4];
    uint8_t debugForceValue[4];
};

// Bus
uint8_t bus_read(Snes *s, uint32_t addr);
void bus_write(Snes *s, uint32_t addr, uint8_t value);
void snes_tick(Snes *s, int cycles);
void snes_catch_up_apu(Snes *s);
void dma_run(Snes *s, uint8_t channels);

#endif
