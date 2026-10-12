// Sony SPC700 audio CPU with its timers and I/O ports. Runs in catch-up mode:
// the main CPU drives it forward whenever it touches $2140-$217F.
#include "snes.h"

enum { PC_ = 0x01, PZ = 0x02, PI = 0x04, PH = 0x08, PB = 0x10, PP = 0x20, PV = 0x40, PN = 0x80 };

// 64-byte boot ROM mapped at $FFC0 while CONTROL bit 7 is set.
static const uint8_t kIplRom[64] = {
    0xCD, 0xEF, 0xBD, 0xE8, 0x00, 0xC6, 0x1D, 0xD0, 0xFC, 0x8F, 0xAA, 0xF4, 0x8F, 0xBB, 0xF5, 0x78,
    0xCC, 0xF4, 0xD0, 0xFB, 0x2F, 0x19, 0xEB, 0xF4, 0xD0, 0xFC, 0x7E, 0xF4, 0xD0, 0x0B, 0xE4, 0xF5,
    0xCB, 0xF4, 0xD7, 0x00, 0xFC, 0xD0, 0xF3, 0xAB, 0x01, 0x10, 0xEF, 0x7E, 0xF4, 0x10, 0xEB, 0xBA,
    0xF6, 0xDA, 0x00, 0xBA, 0xF4, 0xC4, 0xF4, 0xDD, 0x5D, 0xD0, 0xDB, 0x1F, 0x00, 0x00, 0xC0, 0xFF,
};

static const uint8_t kCycles[256] = {
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 6, 8,
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 4, 6,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 5, 4,
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 3, 8,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 6, 6,
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 4, 5, 2, 2, 4, 3,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 5, 5,
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 6,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 2, 4, 5,
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 12, 5,
    3, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 2, 4, 4,
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 4,
    3, 8, 4, 5, 4, 5, 4, 7, 2, 5, 6, 4, 5, 2, 4, 9,
    2, 8, 4, 5, 5, 6, 6, 7, 4, 5, 5, 5, 2, 2, 6, 3,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 4, 5, 3, 4, 3, 4, 3,
    2, 8, 4, 5, 4, 5, 5, 6, 3, 4, 5, 4, 2, 2, 4, 3,
};

#define S (&snes->spc)

static void timers_advance(Snes *snes, int cycles) {
    for (int t = 0; t < 3; t++) {
        if (!S->timer[t].enabled) continue;
        S->timer[t].stage += cycles;
        while (S->timer[t].stage >= S->timer[t].period) {
            S->timer[t].stage -= S->timer[t].period;
            S->timer[t].counter++;
            if (S->timer[t].counter == S->timer[t].target) {
                S->timer[t].counter = 0;
                S->timer[t].output = (S->timer[t].output + 1) & 0xF;
            }
        }
    }
}

static uint8_t mem_read(Snes *snes, uint16_t a) {
    if (a >= 0xF0 && a <= 0xFF) {
        switch (a) {
        case 0xF2: return S->dspAddr;
        case 0xF3: return dsp_read(snes, S->dspAddr & 0x7F);
        case 0xF4: case 0xF5: case 0xF6: case 0xF7: return S->portIn[a - 0xF4];
        case 0xF8: case 0xF9: return S->ram[a];
        case 0xFD: case 0xFE: case 0xFF: {
            uint8_t v = S->timer[a - 0xFD].output;
            S->timer[a - 0xFD].output = 0;
            return v;
        }
        default: return 0;
        }
    }
    if (a >= 0xFFC0 && S->iplEnabled) return kIplRom[a - 0xFFC0];
    return S->ram[a];
}

static void mem_write(Snes *snes, uint16_t a, uint8_t v) {
    if (a >= 0xF0 && a <= 0xFF) {
        switch (a) {
        case 0xF1:
            for (int t = 0; t < 3; t++) {
                bool en = v & (1 << t);
                if (en && !S->timer[t].enabled) {
                    S->timer[t].counter = 0;
                    S->timer[t].output = 0;
                    S->timer[t].stage = 0;
                }
                S->timer[t].enabled = en;
            }
            if (v & 0x10) S->portIn[0] = S->portIn[1] = 0;
            if (v & 0x20) S->portIn[2] = S->portIn[3] = 0;
            S->iplEnabled = v & 0x80;
            S->control = v;
            break;
        case 0xF2: S->dspAddr = v; break;
        case 0xF3:
            if (!(S->dspAddr & 0x80)) dsp_write(snes, S->dspAddr, v);
            break;
        case 0xF4: case 0xF5: case 0xF6: case 0xF7: S->portOut[a - 0xF4] = v; break;
        case 0xFA: case 0xFB: case 0xFC: S->timer[a - 0xFA].target = v; break;
        default: break;
        }
        S->ram[a] = v;
        return;
    }
    S->ram[a] = v;
}

void spc_reset(Snes *snes) {
    memset(S, 0, sizeof(Spc));
    S->pc = kIplRom[0x3E] | (kIplRom[0x3F] << 8);
    S->sp = 0xEF;
    S->iplEnabled = true;
    S->timer[0].period = S->timer[1].period = 128;
    S->timer[2].period = 16;
    for (int t = 0; t < 3; t++) S->timer[t].target = 0; // 0 == 256
    S->sampleClock = 32;
    dsp_reset(&snes->dsp);
}

uint8_t spc_cpu_read_port(Snes *snes, int port) { return S->portOut[port]; }

void spc_cpu_write_port(Snes *snes, int port, uint8_t value) { S->portIn[port] = value; }

// ------------------------------------------------------------ CPU helpers
static inline uint16_t dp(Snes *snes, uint8_t a) { return ((S->psw & PP) ? 0x100 : 0) | a; }

static inline uint8_t fetch(Snes *snes) { return mem_read(snes, S->pc++); }

static inline uint16_t fetch16(Snes *snes) {
    uint16_t lo = fetch(snes);
    return lo | (fetch(snes) << 8);
}

static inline uint8_t rd_dp(Snes *snes, uint8_t a) { return mem_read(snes, dp(snes, a)); }

static inline void wr_dp(Snes *snes, uint8_t a, uint8_t v) { mem_write(snes, dp(snes, a), v); }

static inline uint16_t rd16_dp(Snes *snes, uint8_t a) {
    uint16_t lo = rd_dp(snes, a);
    return lo | (rd_dp(snes, (uint8_t)(a + 1)) << 8);
}

static inline void push(Snes *snes, uint8_t v) { mem_write(snes, 0x100 | S->sp--, v); }

static inline uint8_t pop(Snes *snes) { return mem_read(snes, 0x100 | ++S->sp); }

static inline void nz(Snes *snes, uint8_t v) {
    S->psw = (S->psw & ~(PN | PZ)) | (v & 0x80) | (v ? 0 : PZ);
}

static uint8_t op_adc(Snes *snes, uint8_t a, uint8_t b) {
    int r = a + b + (S->psw & PC_);
    S->psw &= ~(PV | PH | PC_);
    if (~(a ^ b) & (a ^ r) & 0x80) S->psw |= PV;
    if ((a ^ b ^ r) & 0x10) S->psw |= PH;
    if (r > 0xFF) S->psw |= PC_;
    nz(snes, (uint8_t)r);
    return (uint8_t)r;
}

static uint8_t op_sbc(Snes *snes, uint8_t a, uint8_t b) { return op_adc(snes, a, ~b); }

static void op_cmp(Snes *snes, uint8_t a, uint8_t b) {
    int r = a - b;
    S->psw = (S->psw & ~PC_) | (r >= 0 ? PC_ : 0);
    nz(snes, (uint8_t)r);
}

static uint8_t op_or(Snes *snes, uint8_t a, uint8_t b) { nz(snes, a | b); return a | b; }
static uint8_t op_and(Snes *snes, uint8_t a, uint8_t b) { nz(snes, a & b); return a & b; }
static uint8_t op_eor(Snes *snes, uint8_t a, uint8_t b) { nz(snes, a ^ b); return a ^ b; }

static uint8_t op_asl(Snes *snes, uint8_t v) {
    S->psw = (S->psw & ~PC_) | ((v >> 7) & 1);
    v <<= 1;
    nz(snes, v);
    return v;
}

static uint8_t op_lsr(Snes *snes, uint8_t v) {
    S->psw = (S->psw & ~PC_) | (v & 1);
    v >>= 1;
    nz(snes, v);
    return v;
}

static uint8_t op_rol(Snes *snes, uint8_t v) {
    uint8_t c = S->psw & PC_;
    S->psw = (S->psw & ~PC_) | ((v >> 7) & 1);
    v = (v << 1) | c;
    nz(snes, v);
    return v;
}

static uint8_t op_ror(Snes *snes, uint8_t v) {
    uint8_t c = S->psw & PC_;
    S->psw = (S->psw & ~PC_) | (v & 1);
    v = (v >> 1) | (c << 7);
    nz(snes, v);
    return v;
}

static uint8_t op_inc(Snes *snes, uint8_t v) { v++; nz(snes, v); return v; }
static uint8_t op_dec(Snes *snes, uint8_t v) { v--; nz(snes, v); return v; }

static int branch(Snes *snes, bool cond) {
    int8_t off = (int8_t)fetch(snes);
    if (cond) {
        S->pc += off;
        return 2;
    }
    return 0;
}

static void call(Snes *snes, uint16_t target) {
    push(snes, S->pc >> 8);
    push(snes, S->pc & 0xFF);
    S->pc = target;
}

static inline uint16_t ya(Snes *snes) { return (S->y << 8) | S->a; }

// Addressing helpers returning an address for the "A, operand" ALU column.
static uint16_t addr_for(Snes *snes, int col) {
    switch (col) {
    case 0x4: return dp(snes, fetch(snes));                       // d
    case 0x5: return fetch16(snes);                               // !a
    case 0x6: return dp(snes, S->x);                              // (X)
    case 0x7: {                                                   // [d+X]
        uint8_t d = fetch(snes) + S->x;
        return rd16_dp(snes, d);
    }
    case 0x14: return dp(snes, (uint8_t)(fetch(snes) + S->x));    // d+X
    case 0x15: return fetch16(snes) + S->x;                       // !a+X
    case 0x16: return fetch16(snes) + S->y;                       // !a+Y
    case 0x17: return rd16_dp(snes, fetch(snes)) + S->y;          // [d]+Y
    default: return 0;
    }
}

typedef uint8_t (*AluFn)(Snes *, uint8_t, uint8_t);

// Executes the 8 regular ALU ops (OR AND EOR CMP ADC SBC) across their columns.
static bool alu_group(Snes *snes, uint8_t op) {
    int row = op >> 5;        // 0 OR,1 AND,2 EOR,3 CMP,4 ADC,5 SBC
    int col = op & 0x1F;
    if (row > 5) return false;
    static const AluFn fns[6] = {op_or, op_and, op_eor, NULL, op_adc, op_sbc};
    AluFn fn = fns[row];
    uint8_t operand;
    switch (col) {
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x14: case 0x15: case 0x16: case 0x17:
        operand = mem_read(snes, addr_for(snes, col));
        break;
    case 0x08: operand = fetch(snes); break;
    case 0x09: { // d, d
        uint8_t src = rd_dp(snes, fetch(snes));
        uint8_t dst = fetch(snes);
        uint8_t v = rd_dp(snes, dst);
        if (row == 3) op_cmp(snes, v, src);
        else wr_dp(snes, dst, fn(snes, v, src));
        return true;
    }
    case 0x18: { // d, #i
        uint8_t imm = fetch(snes);
        uint8_t dst = fetch(snes);
        uint8_t v = rd_dp(snes, dst);
        if (row == 3) op_cmp(snes, v, imm);
        else wr_dp(snes, dst, fn(snes, v, imm));
        return true;
    }
    case 0x19: { // (X), (Y)
        uint8_t src = mem_read(snes, dp(snes, S->y));
        uint8_t v = mem_read(snes, dp(snes, S->x));
        if (row == 3) op_cmp(snes, v, src);
        else mem_write(snes, dp(snes, S->x), fn(snes, v, src));
        return true;
    }
    default: return false;
    }
    if (row == 3) op_cmp(snes, S->a, operand);
    else S->a = fn(snes, S->a, operand);
    return true;
}

static int spc_step(Snes *snes) {
    uint8_t op = fetch(snes);
    int extra = 0;
    uint8_t lowNibble = op & 0x0F;

    // TCALL n / SET1 / CLR1 / BBS / BBC share column patterns.
    if (lowNibble == 0x01) {
        int n = op >> 4;
        uint16_t vec = 0xFFDE - n * 2;
        call(snes, mem_read(snes, vec) | (mem_read(snes, vec + 1) << 8));
        return kCycles[op];
    }
    if (lowNibble == 0x02) {
        int bit = op >> 5;
        uint8_t d = fetch(snes);
        uint8_t v = rd_dp(snes, d);
        if (op & 0x10) v &= ~(1 << bit);
        else v |= (1 << bit);
        wr_dp(snes, d, v);
        return kCycles[op];
    }
    if (lowNibble == 0x03) {
        int bit = op >> 5;
        uint8_t d = fetch(snes);
        uint8_t v = rd_dp(snes, d);
        bool set = v & (1 << bit);
        extra = branch(snes, (op & 0x10) ? !set : set);
        return kCycles[op] + extra;
    }
    if (alu_group(snes, op)) return kCycles[op];

    switch (op) {
    case 0x00: break; // NOP
    case 0xEF: case 0xFF: S->stopped = true; S->pc--; break; // SLEEP / STOP

    // Branches
    case 0x10: extra = branch(snes, !(S->psw & PN)); break;
    case 0x30: extra = branch(snes, S->psw & PN); break;
    case 0x50: extra = branch(snes, !(S->psw & PV)); break;
    case 0x70: extra = branch(snes, S->psw & PV); break;
    case 0x90: extra = branch(snes, !(S->psw & PC_)); break;
    case 0xB0: extra = branch(snes, S->psw & PC_); break;
    case 0xD0: extra = branch(snes, !(S->psw & PZ)); break;
    case 0xF0: extra = branch(snes, S->psw & PZ); break;
    case 0x2F: S->pc += (int8_t)fetch(snes); break;
    case 0x2E: { // CBNE d, r
        uint8_t v = rd_dp(snes, fetch(snes));
        extra = branch(snes, S->a != v);
        break;
    }
    case 0xDE: { // CBNE d+X, r
        uint8_t v = rd_dp(snes, (uint8_t)(fetch(snes) + S->x));
        extra = branch(snes, S->a != v);
        break;
    }
    case 0x6E: { // DBNZ d, r
        uint8_t d = fetch(snes);
        uint8_t v = rd_dp(snes, d) - 1;
        wr_dp(snes, d, v);
        extra = branch(snes, v != 0);
        break;
    }
    case 0xFE: S->y--; extra = branch(snes, S->y != 0); break;

    // Jumps / calls
    case 0x5F: S->pc = fetch16(snes); break;
    case 0x1F: {
        uint16_t a = fetch16(snes) + S->x;
        S->pc = mem_read(snes, a) | (mem_read(snes, a + 1) << 8);
        break;
    }
    case 0x3F: { uint16_t t = fetch16(snes); call(snes, t); break; }
    case 0x4F: { uint8_t u = fetch(snes); call(snes, 0xFF00 | u); break; }
    case 0x6F: {
        uint16_t lo = pop(snes);
        S->pc = lo | (pop(snes) << 8);
        break;
    }
    case 0x7F: {
        S->psw = pop(snes);
        uint16_t lo = pop(snes);
        S->pc = lo | (pop(snes) << 8);
        break;
    }
    case 0x0F: // BRK
        call(snes, mem_read(snes, 0xFFDE) | (mem_read(snes, 0xFFDF) << 8));
        push(snes, S->psw);
        S->psw = (S->psw | PB) & ~PI;
        break;

    // Stack
    case 0x0D: push(snes, S->psw); break;
    case 0x2D: push(snes, S->a); break;
    case 0x4D: push(snes, S->x); break;
    case 0x6D: push(snes, S->y); break;
    case 0x8E: S->psw = pop(snes); break;
    case 0xAE: S->a = pop(snes); break;
    case 0xCE: S->x = pop(snes); break;
    case 0xEE: S->y = pop(snes); break;

    // Shifts / inc / dec on memory
    case 0x0B: { uint8_t d = fetch(snes); wr_dp(snes, d, op_asl(snes, rd_dp(snes, d))); break; }
    case 0x0C: { uint16_t a = fetch16(snes); mem_write(snes, a, op_asl(snes, mem_read(snes, a))); break; }
    case 0x1B: { uint8_t d = fetch(snes) + S->x; wr_dp(snes, d, op_asl(snes, rd_dp(snes, d))); break; }
    case 0x1C: S->a = op_asl(snes, S->a); break;
    case 0x2B: { uint8_t d = fetch(snes); wr_dp(snes, d, op_rol(snes, rd_dp(snes, d))); break; }
    case 0x2C: { uint16_t a = fetch16(snes); mem_write(snes, a, op_rol(snes, mem_read(snes, a))); break; }
    case 0x3B: { uint8_t d = fetch(snes) + S->x; wr_dp(snes, d, op_rol(snes, rd_dp(snes, d))); break; }
    case 0x3C: S->a = op_rol(snes, S->a); break;
    case 0x4B: { uint8_t d = fetch(snes); wr_dp(snes, d, op_lsr(snes, rd_dp(snes, d))); break; }
    case 0x4C: { uint16_t a = fetch16(snes); mem_write(snes, a, op_lsr(snes, mem_read(snes, a))); break; }
    case 0x5B: { uint8_t d = fetch(snes) + S->x; wr_dp(snes, d, op_lsr(snes, rd_dp(snes, d))); break; }
    case 0x5C: S->a = op_lsr(snes, S->a); break;
    case 0x6B: { uint8_t d = fetch(snes); wr_dp(snes, d, op_ror(snes, rd_dp(snes, d))); break; }
    case 0x6C: { uint16_t a = fetch16(snes); mem_write(snes, a, op_ror(snes, mem_read(snes, a))); break; }
    case 0x7B: { uint8_t d = fetch(snes) + S->x; wr_dp(snes, d, op_ror(snes, rd_dp(snes, d))); break; }
    case 0x7C: S->a = op_ror(snes, S->a); break;
    case 0x8B: { uint8_t d = fetch(snes); wr_dp(snes, d, op_dec(snes, rd_dp(snes, d))); break; }
    case 0x8C: { uint16_t a = fetch16(snes); mem_write(snes, a, op_dec(snes, mem_read(snes, a))); break; }
    case 0x9B: { uint8_t d = fetch(snes) + S->x; wr_dp(snes, d, op_dec(snes, rd_dp(snes, d))); break; }
    case 0x9C: S->a = op_dec(snes, S->a); break;
    case 0xAB: { uint8_t d = fetch(snes); wr_dp(snes, d, op_inc(snes, rd_dp(snes, d))); break; }
    case 0xAC: { uint16_t a = fetch16(snes); mem_write(snes, a, op_inc(snes, mem_read(snes, a))); break; }
    case 0xBB: { uint8_t d = fetch(snes) + S->x; wr_dp(snes, d, op_inc(snes, rd_dp(snes, d))); break; }
    case 0xBC: S->a = op_inc(snes, S->a); break;
    case 0x1D: S->x = op_dec(snes, S->x); break;
    case 0x3D: S->x = op_inc(snes, S->x); break;
    case 0xDC: S->y = op_dec(snes, S->y); break;
    case 0xFC: S->y = op_inc(snes, S->y); break;

    // Compare X / Y
    case 0x1E: op_cmp(snes, S->x, mem_read(snes, fetch16(snes))); break;
    case 0x3E: op_cmp(snes, S->x, rd_dp(snes, fetch(snes))); break;
    case 0xC8: op_cmp(snes, S->x, fetch(snes)); break;
    case 0x5E: op_cmp(snes, S->y, mem_read(snes, fetch16(snes))); break;
    case 0x7E: op_cmp(snes, S->y, rd_dp(snes, fetch(snes))); break;
    case 0xAD: op_cmp(snes, S->y, fetch(snes)); break;

    // TSET1 / TCLR1
    case 0x0E: {
        uint16_t a = fetch16(snes);
        uint8_t v = mem_read(snes, a);
        nz(snes, S->a - v);
        mem_write(snes, a, v | S->a);
        break;
    }
    case 0x4E: {
        uint16_t a = fetch16(snes);
        uint8_t v = mem_read(snes, a);
        nz(snes, S->a - v);
        mem_write(snes, a, v & ~S->a);
        break;
    }

    // Bit operations on m.b
    case 0x0A: case 0x2A: case 0x4A: case 0x6A: case 0x8A: case 0xAA: case 0xCA: case 0xEA: {
        uint16_t operand = fetch16(snes);
        uint16_t a = operand & 0x1FFF;
        int bit = operand >> 13;
        uint8_t v = mem_read(snes, a);
        bool b = (v >> bit) & 1;
        bool c = S->psw & PC_;
        switch (op) {
        case 0x0A: c = c || b; break;
        case 0x2A: c = c || !b; break;
        case 0x4A: c = c && b; break;
        case 0x6A: c = c && !b; break;
        case 0x8A: c = c != b; break;
        case 0xAA: c = b; break;
        case 0xCA:
            v = (v & ~(1 << bit)) | ((c ? 1 : 0) << bit);
            mem_write(snes, a, v);
            break;
        case 0xEA:
            mem_write(snes, a, v ^ (1 << bit));
            break;
        }
        S->psw = (S->psw & ~PC_) | (c ? PC_ : 0);
        break;
    }

    // 16-bit ops
    case 0x1A: case 0x3A: { // DECW / INCW
        uint8_t d = fetch(snes);
        uint16_t v = rd16_dp(snes, d) + (op == 0x3A ? 1 : -1);
        wr_dp(snes, d, v & 0xFF);
        wr_dp(snes, (uint8_t)(d + 1), v >> 8);
        S->psw = (S->psw & ~(PN | PZ)) | ((v >> 8) & 0x80) | (v ? 0 : PZ);
        break;
    }
    case 0x5A: { // CMPW
        uint16_t w = rd16_dp(snes, fetch(snes));
        int r = ya(snes) - w;
        S->psw = (S->psw & ~(PN | PZ | PC_)) | ((r >> 8) & 0x80) | ((r & 0xFFFF) ? 0 : PZ) | (r >= 0 ? PC_ : 0);
        break;
    }
    case 0x7A: case 0x9A: { // ADDW / SUBW
        uint16_t w = rd16_dp(snes, fetch(snes));
        uint16_t v = ya(snes);
        int r;
        S->psw &= ~(PN | PZ | PC_ | PV | PH);
        if (op == 0x7A) {
            r = v + w;
            if (r > 0xFFFF) S->psw |= PC_;
            if (~(v ^ w) & (v ^ r) & 0x8000) S->psw |= PV;
            if ((v ^ w ^ r) & 0x1000) S->psw |= PH;
        } else {
            r = v - w;
            if (r >= 0) S->psw |= PC_;
            if ((v ^ w) & (v ^ r) & 0x8000) S->psw |= PV;
            if (!((v ^ w ^ r) & 0x1000)) S->psw |= PH;
        }
        r &= 0xFFFF;
        S->psw |= ((r >> 8) & 0x80) | (r ? 0 : PZ);
        S->a = r & 0xFF;
        S->y = r >> 8;
        break;
    }
    case 0xBA: { // MOVW YA, d
        uint16_t w = rd16_dp(snes, fetch(snes));
        S->a = w & 0xFF;
        S->y = w >> 8;
        S->psw = (S->psw & ~(PN | PZ)) | ((w >> 8) & 0x80) | (w ? 0 : PZ);
        break;
    }
    case 0xDA: { // MOVW d, YA
        uint8_t d = fetch(snes);
        wr_dp(snes, d, S->a);
        wr_dp(snes, (uint8_t)(d + 1), S->y);
        break;
    }
    case 0xCF: { // MUL YA
        uint16_t r = S->y * S->a;
        S->a = r & 0xFF;
        S->y = r >> 8;
        nz(snes, S->y);
        break;
    }
    case 0x9E: { // DIV YA, X
        uint32_t v = ya(snes);
        uint8_t x = S->x;
        S->psw &= ~(PV | PH);
        if (S->y >= x) S->psw |= PV;
        if ((S->y & 15) >= (x & 15)) S->psw |= PH;
        if (S->y < (x << 1)) {
            S->a = x ? v / x : 0xFF;
            S->y = x ? v % x : (v >> 8);
        } else {
            S->a = 255 - (v - (x << 9)) / (256 - x);
            S->y = x + (v - (x << 9)) % (256 - x);
        }
        nz(snes, S->a);
        break;
    }
    case 0xDF: // DAA
        if ((S->psw & PC_) || S->a > 0x99) {
            S->a += 0x60;
            S->psw |= PC_;
        }
        if ((S->psw & PH) || (S->a & 15) > 9) S->a += 6;
        nz(snes, S->a);
        break;
    case 0xBE: // DAS
        if (!(S->psw & PC_) || S->a > 0x99) {
            S->a -= 0x60;
            S->psw &= ~PC_;
        }
        if (!(S->psw & PH) || (S->a & 15) > 9) S->a -= 6;
        nz(snes, S->a);
        break;
    case 0x9F: S->a = (S->a >> 4) | (S->a << 4); nz(snes, S->a); break;

    // Flags
    case 0x20: S->psw &= ~PP; break;
    case 0x40: S->psw |= PP; break;
    case 0x60: S->psw &= ~PC_; break;
    case 0x80: S->psw |= PC_; break;
    case 0xA0: S->psw |= PI; break;
    case 0xC0: S->psw &= ~PI; break;
    case 0xE0: S->psw &= ~(PV | PH); break;
    case 0xED: S->psw ^= PC_; break;

    // Register moves
    case 0x5D: S->x = S->a; nz(snes, S->x); break;
    case 0x7D: S->a = S->x; nz(snes, S->a); break;
    case 0x9D: S->x = S->sp; nz(snes, S->x); break;
    case 0xBD: S->sp = S->x; break;
    case 0xDD: S->a = S->y; nz(snes, S->a); break;
    case 0xFD: S->y = S->a; nz(snes, S->y); break;

    // Loads
    case 0xE8: S->a = fetch(snes); nz(snes, S->a); break;
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xF4: case 0xF5: case 0xF6: case 0xF7:
        S->a = mem_read(snes, addr_for(snes, op & 0x1F));
        nz(snes, S->a);
        break;
    case 0xBF: S->a = mem_read(snes, dp(snes, S->x++)); nz(snes, S->a); break;
    case 0xCD: S->x = fetch(snes); nz(snes, S->x); break;
    case 0xF8: S->x = rd_dp(snes, fetch(snes)); nz(snes, S->x); break;
    case 0xF9: S->x = rd_dp(snes, (uint8_t)(fetch(snes) + S->y)); nz(snes, S->x); break;
    case 0xE9: S->x = mem_read(snes, fetch16(snes)); nz(snes, S->x); break;
    case 0x8D: S->y = fetch(snes); nz(snes, S->y); break;
    case 0xEB: S->y = rd_dp(snes, fetch(snes)); nz(snes, S->y); break;
    case 0xFB: S->y = rd_dp(snes, (uint8_t)(fetch(snes) + S->x)); nz(snes, S->y); break;
    case 0xEC: S->y = mem_read(snes, fetch16(snes)); nz(snes, S->y); break;

    // Stores
    case 0xC4: case 0xC5: case 0xC6: case 0xC7: case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        mem_write(snes, addr_for(snes, op & 0x1F), S->a);
        break;
    case 0xAF: mem_write(snes, dp(snes, S->x++), S->a); break;
    case 0xD8: wr_dp(snes, fetch(snes), S->x); break;
    case 0xD9: wr_dp(snes, (uint8_t)(fetch(snes) + S->y), S->x); break;
    case 0xC9: mem_write(snes, fetch16(snes), S->x); break;
    case 0xCB: wr_dp(snes, fetch(snes), S->y); break;
    case 0xDB: wr_dp(snes, (uint8_t)(fetch(snes) + S->x), S->y); break;
    case 0xCC: mem_write(snes, fetch16(snes), S->y); break;
    case 0x8F: { uint8_t imm = fetch(snes); wr_dp(snes, fetch(snes), imm); break; }
    case 0xFA: { uint8_t v = rd_dp(snes, fetch(snes)); wr_dp(snes, fetch(snes), v); break; }

    default: break;
    }
    return kCycles[op] + extra;
}

void spc_run_until(Snes *snes, int64_t target) {
    while (S->cycles < target) {
        int c = S->stopped ? 2 : spc_step(snes);
        S->cycles += c;
        timers_advance(snes, c);
        S->sampleClock -= c;
        while (S->sampleClock <= 0) {
            S->sampleClock += 32;
            dsp_sample(snes);
        }
    }
}
