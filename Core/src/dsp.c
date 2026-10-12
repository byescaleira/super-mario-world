// S-DSP: 8 BRR voices with ADSR/GAIN envelopes, gaussian interpolation,
// pitch modulation, noise and the 8-tap FIR echo unit. One call to
// dsp_sample() produces one 32 kHz stereo frame.
#include <math.h>

#include "snes.h"

enum {
    R_MVOLL = 0x0C, R_MVOLR = 0x1C, R_EVOLL = 0x2C, R_EVOLR = 0x3C,
    R_KON = 0x4C, R_KOFF = 0x5C, R_FLG = 0x6C, R_ENDX = 0x7C,
    R_EFB = 0x0D, R_PMON = 0x2D, R_NON = 0x3D, R_EON = 0x4D,
    R_DIR = 0x5D, R_ESA = 0x6D, R_EDL = 0x7D,
};

enum { ENV_RELEASE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN };

#define COUNTER_RANGE (2048 * 5 * 3)

static const int kCounterRates[32] = {
    COUNTER_RANGE + 1, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1,
};

static const int kCounterOffsets[32] = {
    1, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
    0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0,
};

static int16_t gGauss[512];
static bool gGaussReady;

// Builds the 4-tap interpolation kernel. The hardware table is a sampled
// gaussian whose taps sum to ~2048 for every phase; we derive it the same way.
static void build_gauss(void) {
    const double sigma2 = 0.398;
    double raw[512];
    for (int i = 0; i < 512; i++) {
        double t = (511 - i) / 256.0;
        raw[i] = exp(-(t * t) / (2 * sigma2));
    }
    for (int f = 0; f < 256; f++) {
        double sum = raw[255 - f] + raw[511 - f] + raw[256 + f] + raw[f];
        double k = 2048.0 / sum;
        gGauss[255 - f] = (int16_t)lround(raw[255 - f] * k);
        gGauss[511 - f] = (int16_t)lround(raw[511 - f] * k);
        gGauss[256 + f] = (int16_t)lround(raw[256 + f] * k);
        gGauss[f] = (int16_t)lround(raw[f] * k);
    }
    gGaussReady = true;
}

static inline int clamp16(int v) {
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return v;
}

void dsp_reset(Dsp *d) {
    if (!gGaussReady) build_gauss();
    memset(d, 0, sizeof(*d));
    d->regs[R_FLG] = 0xE0;
    d->noise = 0x4000;
    d->counter = 0;
}

void dsp_write(Snes *s, uint8_t addr, uint8_t v) {
    Dsp *d = &s->dsp;
    d->regs[addr] = v;
    if (addr == R_KON) d->konLatch |= v;
    else if (addr == R_ENDX) d->regs[R_ENDX] = 0;
}

uint8_t dsp_read(Snes *s, uint8_t addr) { return s->dsp.regs[addr]; }

static inline bool counter_tick(Dsp *d, int rate) {
    return ((unsigned)d->counter + kCounterOffsets[rate]) % kCounterRates[rate] == 0;
}

static void decode_block(Snes *s, DspVoice *v) {
    const uint8_t *ram = s->spc.ram;
    uint16_t addr = v->blockAddr;
    uint8_t header = ram[addr];
    int shift = header >> 4;
    int filter = header & 0x0C;
    int p1 = v->hist[0], p2 = v->hist[1];
    for (int i = 0; i < 16; i++) {
        uint8_t byte = ram[(uint16_t)(addr + 1 + (i >> 1))];
        int nib = (i & 1) ? (byte & 0x0F) : (byte >> 4);
        int smp = (int8_t)(nib << 4) >> 4;
        if (shift <= 12) smp = (smp << shift) >> 1;
        else smp = smp < 0 ? -2048 : 0;
        int q2 = p2 >> 1;
        if (filter >= 8) {
            smp += p1;
            smp -= q2;
            if (filter == 8) {
                smp += q2 >> 4;
                smp += (p1 * -3) >> 6;
            } else {
                smp += (p1 * -13) >> 7;
                smp += (q2 * 3) >> 4;
            }
        } else if (filter) {
            smp += p1 >> 1;
            smp += (-p1) >> 5;
        }
        smp = clamp16(smp);
        smp = (int16_t)(smp * 2);
        v->buf[i] = (int16_t)smp;
        p2 = p1;
        p1 = smp;
    }
    v->hist[0] = (int16_t)p1;
    v->hist[1] = (int16_t)p2;
}

static void key_on(Snes *s, int n) {
    Dsp *d = &s->dsp;
    DspVoice *v = &d->voice[n];
    const uint8_t *ram = s->spc.ram;
    uint16_t dir = (d->regs[R_DIR] << 8) + d->regs[n * 16 + 4] * 4;
    v->blockAddr = ram[dir] | (ram[(uint16_t)(dir + 1)] << 8);
    v->hist[0] = v->hist[1] = v->hist[2] = 0;
    v->prevTail[0] = v->prevTail[1] = v->prevTail[2] = 0;
    memset(v->buf, 0, sizeof(v->buf));
    v->interp = 0;
    v->blockPos = 0;
    v->env = 0;
    v->hiddenEnv = 0;
    v->envMode = ENV_ATTACK;
    v->konDelay = 5;
    v->active = true;
    decode_block(s, v);
    d->regs[R_ENDX] &= ~(1 << n);
}

static void next_block(Snes *s, DspVoice *v, int n) {
    Dsp *d = &s->dsp;
    const uint8_t *ram = s->spc.ram;
    uint8_t header = ram[v->blockAddr];
    if (header & 1) {
        d->regs[R_ENDX] |= 1 << n;
        if (header & 2) {
            uint16_t dir = (d->regs[R_DIR] << 8) + d->regs[n * 16 + 4] * 4;
            v->blockAddr = ram[(uint16_t)(dir + 2)] | (ram[(uint16_t)(dir + 3)] << 8);
        } else {
            v->envMode = ENV_RELEASE;
            v->env = 0;
            v->blockAddr += 9;
        }
    } else {
        v->blockAddr += 9;
    }
    decode_block(s, v);
}

static void run_envelope(Dsp *d, DspVoice *v, const uint8_t *vr) {
    int env = v->env;
    if (v->envMode == ENV_RELEASE) {
        env -= 8;
        if (env < 0) env = 0;
        v->env = env;
        return;
    }
    int rate;
    int envData = vr[6];
    uint8_t adsr0 = vr[5];
    if (adsr0 & 0x80) {
        if (v->envMode >= ENV_DECAY) {
            env--;
            env -= env >> 8;
            rate = envData & 0x1F;
            if (v->envMode == ENV_DECAY) rate = ((adsr0 >> 3) & 0x0E) + 0x10;
        } else {
            rate = (adsr0 & 0x0F) * 2 + 1;
            env += rate < 31 ? 0x20 : 0x400;
        }
    } else {
        envData = vr[7];
        int mode = envData >> 5;
        if (mode < 4) {
            env = envData * 0x10;
            rate = 31;
        } else {
            rate = envData & 0x1F;
            if (mode == 4) {
                env -= 0x20;
            } else if (mode < 6) {
                env--;
                env -= env >> 8;
            } else {
                env += 0x20;
                if (mode > 6 && (unsigned)v->hiddenEnv >= 0x600) env += 0x8 - 0x20;
            }
        }
    }
    if ((env >> 8) == (envData >> 5) && v->envMode == ENV_DECAY) v->envMode = ENV_SUSTAIN;
    v->hiddenEnv = env;
    if ((unsigned)env > 0x7FF) {
        env = env < 0 ? 0 : 0x7FF;
        if (v->envMode == ENV_ATTACK) v->envMode = ENV_DECAY;
    }
    if (counter_tick(d, rate)) v->env = env;
}

static inline int interpolate(const DspVoice *v, int window[4]) {
    int f = (v->interp >> 4) & 0xFF;
    const int16_t *fwd = gGauss + 255 - f;
    const int16_t *rev = gGauss + f;
    int out = (fwd[0] * window[0]) >> 11;
    out += (fwd[256] * window[1]) >> 11;
    out += (rev[256] * window[2]) >> 11;
    out = (int16_t)out;
    out += (rev[0] * window[3]) >> 11;
    return clamp16(out) & ~1;
}

// Catmull-Rom between window[1] and window[2]: keeps the high end the
// gaussian filter rolls off, for a cleaner "remastered" sound.
static inline int interpolate_cubic(const DspVoice *v, int window[4]) {
    int f = (v->interp >> 4) & 0xFF;
    int p0 = window[0], p1 = window[1], p2 = window[2], p3 = window[3];
    int64_t a = -p0 + 3 * p1 - 3 * p2 + p3;
    int64_t b = 2 * p0 - 5 * p1 + 4 * p2 - p3;
    int64_t c = p2 - p0;
    int64_t t = f;
    int64_t out = ((((a * t >> 8) + b) * t >> 8) + c) * t >> 9;
    return clamp16((int)(out + p1)) & ~1;
}

// Returns the sample at relative position k (0 = oldest of the 4-tap window).
static inline int window_sample(const DspVoice *v, int k) {
    int idx = v->blockPos - 3 + k;
    if (idx >= 0) return v->buf[idx];
    // Older samples come from the previous block's tail, kept in hist order.
    return v->prevTail[3 + idx];
}

void dsp_sample(Snes *s) {
    Dsp *d = &s->dsp;
    uint8_t *r = d->regs;
    uint8_t *ram = s->spc.ram;

    if (--d->counter < 0) d->counter = COUNTER_RANGE - 1;

    if (counter_tick(d, r[R_FLG] & 0x1F)) {
        int fb = (d->noise << 13) ^ (d->noise << 14);
        d->noise = (fb & 0x4000) ^ (d->noise >> 1);
    }

    d->everyOther = !d->everyOther;
    if (d->everyOther) {
        uint8_t kon = d->konLatch;
        d->konLatch = 0;
        for (int n = 0; n < 8; n++) {
            if (kon & (1 << n)) key_on(s, n);
            else if ((r[R_KOFF] & (1 << n)) && d->voice[n].konDelay == 0) d->voice[n].envMode = ENV_RELEASE;
        }
    }
    if (r[R_FLG] & 0x80) {
        for (int n = 0; n < 8; n++) {
            d->voice[n].envMode = ENV_RELEASE;
            d->voice[n].env = 0;
        }
    }

    int mainL = 0, mainR = 0, echoL = 0, echoR = 0;
    int prevOut = 0;
    for (int n = 0; n < 8; n++) {
        DspVoice *v = &d->voice[n];
        uint8_t *vr = &r[n * 16];
        int out = 0;
        if (v->active) {
            int pitch = vr[2] | ((vr[3] & 0x3F) << 8);
            if (n > 0 && (r[R_PMON] & (1 << n))) pitch += ((prevOut >> 5) * pitch) >> 10;
            if (pitch < 0) pitch = 0;
            if (pitch > 0x7FFF) pitch = 0x7FFF;

            if (v->konDelay > 0) {
                v->konDelay--;
                if (v->konDelay == 0) v->interp = 0;
            } else {
                int smp;
                if (r[R_NON] & (1 << n)) {
                    smp = (int16_t)(d->noise * 2);
                } else {
                    int window[4];
                    for (int k = 0; k < 4; k++) window[k] = window_sample(v, k);
                    smp = d->interpMode == 1 ? interpolate_cubic(v, window) : interpolate(v, window);
                }
                out = ((smp * v->env) >> 11) & ~1;
                run_envelope(d, v, vr);

                v->interp += pitch;
                while (v->interp >= 0x1000) {
                    v->interp -= 0x1000;
                    v->blockPos++;
                    if (v->blockPos >= 16) {
                        v->prevTail[0] = v->buf[13];
                        v->prevTail[1] = v->buf[14];
                        v->prevTail[2] = v->buf[15];
                        v->blockPos = 0;
                        next_block(s, v, n);
                    }
                }
            }
            if (v->envMode == ENV_RELEASE && v->env == 0 && v->konDelay == 0) v->active = false;
        }
        v->prevOut = out;
        prevOut = out;
        vr[8] = v->env >> 4;
        vr[9] = out >> 8;
        int l = (out * (int8_t)vr[0]) >> 7;
        int rr = (out * (int8_t)vr[1]) >> 7;
        mainL = clamp16(mainL + l);
        mainR = clamp16(mainR + rr);
        if (r[R_EON] & (1 << n)) {
            echoL = clamp16(echoL + l);
            echoR = clamp16(echoR + rr);
        }
    }

    // Echo unit
    uint16_t ea = (uint16_t)((r[R_ESA] << 8) + d->echoOffset);
    if (d->echoOffset == 0) d->echoLength = (r[R_EDL] & 0x0F) * 0x800;
    d->echoHistPos = (d->echoHistPos + 1) & 7;
    d->echoHist[d->echoHistPos][0] = (int16_t)(ram[ea] | (ram[(uint16_t)(ea + 1)] << 8)) >> 1;
    d->echoHist[d->echoHistPos][1] = (int16_t)(ram[(uint16_t)(ea + 2)] | (ram[(uint16_t)(ea + 3)] << 8)) >> 1;
    int fir[2];
    for (int ch = 0; ch < 2; ch++) {
        int sum = 0;
        for (int i = 0; i < 7; i++) {
            int h = d->echoHist[(d->echoHistPos + 1 + i) & 7][ch];
            sum += (h * (int8_t)r[0x0F + i * 0x10]) >> 6;
        }
        sum = (int16_t)sum;
        sum += (d->echoHist[d->echoHistPos][ch] * (int8_t)r[0x7F]) >> 6;
        fir[ch] = clamp16(sum) & ~1;
    }
    int outL = clamp16(((mainL * (int8_t)r[R_MVOLL]) >> 7) + ((fir[0] * (int8_t)r[R_EVOLL]) >> 7));
    int outR = clamp16(((mainR * (int8_t)r[R_MVOLR]) >> 7) + ((fir[1] * (int8_t)r[R_EVOLR]) >> 7));
    if (!(r[R_FLG] & 0x20)) {
        int inL = clamp16(echoL + ((fir[0] * (int8_t)r[R_EFB]) >> 7)) & ~1;
        int inR = clamp16(echoR + ((fir[1] * (int8_t)r[R_EFB]) >> 7)) & ~1;
        ram[ea] = inL & 0xFF;
        ram[(uint16_t)(ea + 1)] = (inL >> 8) & 0xFF;
        ram[(uint16_t)(ea + 2)] = inR & 0xFF;
        ram[(uint16_t)(ea + 3)] = (inR >> 8) & 0xFF;
    }
    d->echoOffset += 4;
    if (d->echoOffset >= d->echoLength) d->echoOffset = 0;

    if (r[R_FLG] & 0x40) outL = outR = 0;

    const uint32_t cap = sizeof(d->out) / sizeof(d->out[0]) / 2;
    if (d->outWrite - d->outRead >= cap) d->outRead++;
    uint32_t i = d->outWrite % cap;
    d->out[i * 2] = (int16_t)outL;
    d->out[i * 2 + 1] = (int16_t)outR;
    d->outWrite++;
}
