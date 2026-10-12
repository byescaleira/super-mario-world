// WDC 65C816 interpreter. Each bus access is charged its real master-clock
// cost through bus_speed(), so lag frames and raster timing match hardware.
#include "snes.h"

int bus_speed(Snes *s, uint32_t addr);

enum { FC = 0x01, FZ = 0x02, FI = 0x04, FD = 0x08, FX = 0x10, FM = 0x20, FV = 0x40, FN = 0x80 };

// Bit 31 of an effective address marks accesses that wrap inside their
// 64 KiB bank (direct page, stack, immediates) instead of carrying into the
// next bank.
#define WRAP16 0x80000000u

#define C (&s->cpu)
#define MF (C->p & FM)
#define XF (C->p & FX)

static inline uint8_t rd(Snes *s, uint32_t a) {
    a &= 0xFFFFFF;
    snes_tick(s, bus_speed(s, a));
    return bus_read(s, a);
}

static inline void wr(Snes *s, uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    snes_tick(s, bus_speed(s, a));
    bus_write(s, a, v);
}

static inline void io(Snes *s) { snes_tick(s, 6); }

static inline uint32_t next_addr(uint32_t ea) {
    return (ea & WRAP16) ? ((ea & 0xFFFF0000u) | ((ea + 1) & 0xFFFF)) : ((ea + 1) & 0xFFFFFF);
}

static inline uint16_t rd16(Snes *s, uint32_t ea) {
    uint8_t lo = rd(s, ea);
    return lo | (rd(s, next_addr(ea)) << 8);
}

static inline void wr16(Snes *s, uint32_t ea, uint16_t v) {
    wr(s, ea, v & 0xFF);
    wr(s, next_addr(ea), v >> 8);
}

// 16-bit RMW writes go high byte first on hardware.
static inline void wr16_rev(Snes *s, uint32_t ea, uint16_t v) {
    wr(s, next_addr(ea), v >> 8);
    wr(s, ea, v & 0xFF);
}

static inline uint8_t fetch(Snes *s) {
    uint8_t v = rd(s, ((uint32_t)C->pb << 16) | C->pc);
    C->pc++;
    return v;
}

static inline uint16_t fetch16(Snes *s) {
    uint16_t lo = fetch(s);
    return lo | (fetch(s) << 8);
}

static inline uint32_t fetch24(Snes *s) {
    uint32_t lo = fetch16(s);
    return lo | ((uint32_t)fetch(s) << 16);
}

static inline void push8(Snes *s, uint8_t v) {
    wr(s, C->sp, v);
    C->sp--;
    if (C->e) C->sp = 0x100 | (C->sp & 0xFF);
}

static inline uint8_t pull8(Snes *s) {
    C->sp++;
    if (C->e) C->sp = 0x100 | (C->sp & 0xFF);
    return rd(s, C->sp);
}

static inline void push16(Snes *s, uint16_t v) {
    push8(s, v >> 8);
    push8(s, v & 0xFF);
}

static inline uint16_t pull16(Snes *s) {
    uint16_t lo = pull8(s);
    return lo | (pull8(s) << 8);
}

static inline void nz8(Snes *s, uint8_t v) {
    C->p = (C->p & ~(FN | FZ)) | (v & 0x80) | (v ? 0 : FZ);
}

static inline void nz16(Snes *s, uint16_t v) {
    C->p = (C->p & ~(FN | FZ)) | ((v >> 8) & 0x80) | (v ? 0 : FZ);
}

static void set_p(Snes *s, uint8_t v) {
    C->p = v;
    if (C->e) C->p |= FM | FX;
    if (C->p & FX) {
        C->x &= 0xFF;
        C->y &= 0xFF;
    }
}

// ------------------------------------------------------- addressing modes
static inline void dp_penalty(Snes *s) {
    if (C->dp & 0xFF) io(s);
}

static uint32_t imm_m(Snes *s) {
    uint32_t ea = ((uint32_t)C->pb << 16) | C->pc | WRAP16;
    C->pc += MF ? 1 : 2;
    return ea;
}

static uint32_t imm_x(Snes *s) {
    uint32_t ea = ((uint32_t)C->pb << 16) | C->pc | WRAP16;
    C->pc += XF ? 1 : 2;
    return ea;
}

static uint32_t adr_dp(Snes *s) {
    uint8_t op = fetch(s);
    dp_penalty(s);
    return ((C->dp + op) & 0xFFFF) | WRAP16;
}

static uint32_t adr_dpx(Snes *s) {
    uint8_t op = fetch(s);
    dp_penalty(s);
    io(s);
    return ((C->dp + op + C->x) & 0xFFFF) | WRAP16;
}

static uint32_t adr_dpy(Snes *s) {
    uint8_t op = fetch(s);
    dp_penalty(s);
    io(s);
    return ((C->dp + op + C->y) & 0xFFFF) | WRAP16;
}

static uint32_t adr_idp(Snes *s) {
    uint16_t ptr = rd16(s, adr_dp(s));
    return ((uint32_t)C->db << 16) | ptr;
}

static uint32_t adr_idpx(Snes *s) {
    uint16_t ptr = rd16(s, adr_dpx(s));
    return ((uint32_t)C->db << 16) | ptr;
}

static uint32_t adr_idpy(Snes *s, bool write) {
    uint16_t ptr = rd16(s, adr_dp(s));
    uint32_t base = ((uint32_t)C->db << 16) | ptr;
    uint32_t ea = (base + C->y) & 0xFFFFFF;
    if (write || !XF || ((base ^ ea) & 0xFF00)) io(s);
    return ea;
}

static uint32_t adr_ildp(Snes *s) {
    uint32_t p = adr_dp(s);
    uint32_t lo = rd(s, p);
    p = next_addr(p);
    uint32_t hi = rd(s, p);
    p = next_addr(p);
    uint32_t bank = rd(s, p);
    return lo | (hi << 8) | (bank << 16);
}

static uint32_t adr_ildpy(Snes *s) {
    return (adr_ildp(s) + C->y) & 0xFFFFFF;
}

static uint32_t adr_abs(Snes *s) {
    return ((uint32_t)C->db << 16) | fetch16(s);
}

static uint32_t adr_absx(Snes *s, bool write) {
    uint32_t base = ((uint32_t)C->db << 16) | fetch16(s);
    uint32_t ea = (base + C->x) & 0xFFFFFF;
    if (write || !XF || ((base ^ ea) & 0xFF00)) io(s);
    return ea;
}

static uint32_t adr_absy(Snes *s, bool write) {
    uint32_t base = ((uint32_t)C->db << 16) | fetch16(s);
    uint32_t ea = (base + C->y) & 0xFFFFFF;
    if (write || !XF || ((base ^ ea) & 0xFF00)) io(s);
    return ea;
}

static uint32_t adr_long(Snes *s) { return fetch24(s); }

static uint32_t adr_longx(Snes *s) { return (fetch24(s) + C->x) & 0xFFFFFF; }

static uint32_t adr_sr(Snes *s) {
    uint8_t op = fetch(s);
    io(s);
    return ((C->sp + op) & 0xFFFF) | WRAP16;
}

static uint32_t adr_isry(Snes *s) {
    uint16_t ptr = rd16(s, adr_sr(s));
    io(s);
    return ((((uint32_t)C->db << 16) | ptr) + C->y) & 0xFFFFFF;
}

// ------------------------------------------------------------- operations
static void op_lda(Snes *s, uint32_t ea) {
    if (MF) {
        uint8_t v = rd(s, ea);
        C->a = (C->a & 0xFF00) | v;
        nz8(s, v);
    } else {
        C->a = rd16(s, ea);
        nz16(s, C->a);
    }
}

static void op_ldx(Snes *s, uint32_t ea) {
    if (XF) {
        C->x = rd(s, ea);
        nz8(s, C->x);
    } else {
        C->x = rd16(s, ea);
        nz16(s, C->x);
    }
}

static void op_ldy(Snes *s, uint32_t ea) {
    if (XF) {
        C->y = rd(s, ea);
        nz8(s, C->y);
    } else {
        C->y = rd16(s, ea);
        nz16(s, C->y);
    }
}

static void op_sta(Snes *s, uint32_t ea) {
    if (MF) wr(s, ea, C->a & 0xFF);
    else wr16(s, ea, C->a);
}

static void op_stx(Snes *s, uint32_t ea) {
    if (XF) wr(s, ea, C->x & 0xFF);
    else wr16(s, ea, C->x);
}

static void op_sty(Snes *s, uint32_t ea) {
    if (XF) wr(s, ea, C->y & 0xFF);
    else wr16(s, ea, C->y);
}

static void op_stz(Snes *s, uint32_t ea) {
    if (MF) wr(s, ea, 0);
    else wr16(s, ea, 0);
}

static void op_ora(Snes *s, uint32_t ea) {
    if (MF) {
        uint8_t v = (C->a | rd(s, ea)) & 0xFF;
        C->a = (C->a & 0xFF00) | v;
        nz8(s, v);
    } else {
        C->a |= rd16(s, ea);
        nz16(s, C->a);
    }
}

static void op_and(Snes *s, uint32_t ea) {
    if (MF) {
        uint8_t v = C->a & rd(s, ea);
        C->a = (C->a & 0xFF00) | v;
        nz8(s, v);
    } else {
        C->a &= rd16(s, ea);
        nz16(s, C->a);
    }
}

static void op_eor(Snes *s, uint32_t ea) {
    if (MF) {
        uint8_t v = (C->a ^ rd(s, ea)) & 0xFF;
        C->a = (C->a & 0xFF00) | v;
        nz8(s, v);
    } else {
        C->a ^= rd16(s, ea);
        nz16(s, C->a);
    }
}

static void adc8(Snes *s, uint8_t data) {
    int a = C->a & 0xFF;
    int result;
    if (!(C->p & FD)) {
        result = a + data + (C->p & FC);
    } else {
        result = (a & 0x0F) + (data & 0x0F) + (C->p & FC);
        if (result > 0x09) result += 0x06;
        int carry = result > 0x0F;
        result = (a & 0xF0) + (data & 0xF0) + (carry << 4) + (result & 0x0F);
    }
    C->p &= ~(FV | FC);
    if (~(a ^ data) & (a ^ result) & 0x80) C->p |= FV;
    if ((C->p & FD) && result > 0x9F) result += 0x60;
    if (result > 0xFF) C->p |= FC;
    C->a = (C->a & 0xFF00) | (result & 0xFF);
    nz8(s, result & 0xFF);
}

static void adc16(Snes *s, uint16_t data) {
    int a = C->a;
    int result;
    if (!(C->p & FD)) {
        result = a + data + (C->p & FC);
    } else {
        result = (a & 0x000F) + (data & 0x000F) + (C->p & FC);
        if (result > 0x0009) result += 0x0006;
        int carry = result > 0x000F;
        result = (a & 0x00F0) + (data & 0x00F0) + (carry << 4) + (result & 0x000F);
        if (result > 0x009F) result += 0x0060;
        carry = result > 0x00FF;
        result = (a & 0x0F00) + (data & 0x0F00) + (carry << 8) + (result & 0x00FF);
        if (result > 0x09FF) result += 0x0600;
        carry = result > 0x0FFF;
        result = (a & 0xF000) + (data & 0xF000) + (carry << 12) + (result & 0x0FFF);
    }
    C->p &= ~(FV | FC);
    if (~(a ^ data) & (a ^ result) & 0x8000) C->p |= FV;
    if ((C->p & FD) && result > 0x9FFF) result += 0x6000;
    if (result > 0xFFFF) C->p |= FC;
    C->a = result & 0xFFFF;
    nz16(s, C->a);
}

static void sbc8(Snes *s, uint8_t data) {
    int a = C->a & 0xFF;
    data ^= 0xFF;
    int result;
    if (!(C->p & FD)) {
        result = a + data + (C->p & FC);
    } else {
        result = (a & 0x0F) + (data & 0x0F) + (C->p & FC);
        if (result <= 0x0F) result -= 0x06;
        int carry = result > 0x0F;
        result = (a & 0xF0) + (data & 0xF0) + (carry << 4) + (result & 0x0F);
    }
    C->p &= ~(FV | FC);
    if (~(a ^ data) & (a ^ result) & 0x80) C->p |= FV;
    if ((C->p & FD) && result <= 0xFF) result -= 0x60;
    if (result > 0xFF) C->p |= FC;
    C->a = (C->a & 0xFF00) | (result & 0xFF);
    nz8(s, result & 0xFF);
}

static void sbc16(Snes *s, uint16_t data) {
    int a = C->a;
    data ^= 0xFFFF;
    int result;
    if (!(C->p & FD)) {
        result = a + data + (C->p & FC);
    } else {
        result = (a & 0x000F) + (data & 0x000F) + (C->p & FC);
        if (result <= 0x000F) result -= 0x0006;
        int carry = result > 0x000F;
        result = (a & 0x00F0) + (data & 0x00F0) + (carry << 4) + (result & 0x000F);
        if (result <= 0x00FF) result -= 0x0060;
        carry = result > 0x00FF;
        result = (a & 0x0F00) + (data & 0x0F00) + (carry << 8) + (result & 0x00FF);
        if (result <= 0x0FFF) result -= 0x0600;
        carry = result > 0x0FFF;
        result = (a & 0xF000) + (data & 0xF000) + (carry << 12) + (result & 0x0FFF);
    }
    C->p &= ~(FV | FC);
    if (~(a ^ data) & (a ^ result) & 0x8000) C->p |= FV;
    if ((C->p & FD) && result <= 0xFFFF) result -= 0x6000;
    if (result > 0xFFFF) C->p |= FC;
    C->a = result & 0xFFFF;
    nz16(s, C->a);
}

static void op_adc(Snes *s, uint32_t ea) {
    if (MF) adc8(s, rd(s, ea));
    else adc16(s, rd16(s, ea));
}

static void op_sbc(Snes *s, uint32_t ea) {
    if (MF) sbc8(s, rd(s, ea));
    else sbc16(s, rd16(s, ea));
}

static void compare(Snes *s, uint16_t reg, uint16_t v, bool small) {
    if (small) {
        int r = (reg & 0xFF) - v;
        C->p = (C->p & ~FC) | (r >= 0 ? FC : 0);
        nz8(s, r & 0xFF);
    } else {
        int r = reg - v;
        C->p = (C->p & ~FC) | (r >= 0 ? FC : 0);
        nz16(s, r & 0xFFFF);
    }
}

static void op_cmp(Snes *s, uint32_t ea) {
    if (MF) compare(s, C->a, rd(s, ea), true);
    else compare(s, C->a, rd16(s, ea), false);
}

static void op_cpx(Snes *s, uint32_t ea) {
    if (XF) compare(s, C->x, rd(s, ea), true);
    else compare(s, C->x, rd16(s, ea), false);
}

static void op_cpy(Snes *s, uint32_t ea) {
    if (XF) compare(s, C->y, rd(s, ea), true);
    else compare(s, C->y, rd16(s, ea), false);
}

static void op_bit(Snes *s, uint32_t ea) {
    if (MF) {
        uint8_t v = rd(s, ea);
        C->p = (C->p & ~(FN | FV | FZ)) | (v & 0xC0) | ((v & C->a & 0xFF) ? 0 : FZ);
    } else {
        uint16_t v = rd16(s, ea);
        C->p = (C->p & ~(FN | FV | FZ)) | ((v >> 8) & 0xC0) | ((v & C->a) ? 0 : FZ);
    }
}

static void op_bit_imm(Snes *s) {
    if (MF) {
        uint8_t v = fetch(s);
        C->p = (C->p & ~FZ) | ((v & C->a & 0xFF) ? 0 : FZ);
    } else {
        uint16_t v = fetch16(s);
        C->p = (C->p & ~FZ) | ((v & C->a) ? 0 : FZ);
    }
}

// Read-modify-write helpers: compute on 16-bit values, `bits` is 8 or 16.
typedef uint32_t (*RmwFn)(Snes *s, uint32_t v, int bits);

static uint32_t f_asl(Snes *s, uint32_t v, int bits) {
    uint32_t top = 1u << (bits - 1);
    C->p = (C->p & ~FC) | ((v & top) ? FC : 0);
    return (v << 1) & ((1u << bits) - 1);
}

static uint32_t f_lsr(Snes *s, uint32_t v, int bits) {
    (void)bits;
    C->p = (C->p & ~FC) | (v & 1);
    return v >> 1;
}

static uint32_t f_rol(Snes *s, uint32_t v, int bits) {
    uint32_t top = 1u << (bits - 1);
    uint32_t carry = C->p & FC;
    C->p = (C->p & ~FC) | ((v & top) ? FC : 0);
    return ((v << 1) | carry) & ((1u << bits) - 1);
}

static uint32_t f_ror(Snes *s, uint32_t v, int bits) {
    uint32_t carry = C->p & FC;
    C->p = (C->p & ~FC) | (v & 1);
    return (v >> 1) | (carry << (bits - 1));
}

static uint32_t f_inc(Snes *s, uint32_t v, int bits) {
    (void)s;
    return (v + 1) & ((1u << bits) - 1);
}

static uint32_t f_dec(Snes *s, uint32_t v, int bits) {
    (void)s;
    return (v - 1) & ((1u << bits) - 1);
}

static void rmw(Snes *s, uint32_t ea, RmwFn fn) {
    if (MF) {
        uint8_t v = rd(s, ea);
        io(s);
        v = fn(s, v, 8);
        nz8(s, v);
        wr(s, ea, v);
    } else {
        uint16_t v = rd16(s, ea);
        io(s);
        v = fn(s, v, 16);
        nz16(s, v);
        wr16_rev(s, ea, v);
    }
}

static void rmw_acc(Snes *s, RmwFn fn) {
    io(s);
    if (MF) {
        uint8_t v = fn(s, C->a & 0xFF, 8);
        C->a = (C->a & 0xFF00) | v;
        nz8(s, v);
    } else {
        C->a = fn(s, C->a, 16);
        nz16(s, C->a);
    }
}

static void op_tsb(Snes *s, uint32_t ea) {
    if (MF) {
        uint8_t v = rd(s, ea);
        io(s);
        C->p = (C->p & ~FZ) | ((v & C->a & 0xFF) ? 0 : FZ);
        wr(s, ea, v | (C->a & 0xFF));
    } else {
        uint16_t v = rd16(s, ea);
        io(s);
        C->p = (C->p & ~FZ) | ((v & C->a) ? 0 : FZ);
        wr16_rev(s, ea, v | C->a);
    }
}

static void op_trb(Snes *s, uint32_t ea) {
    if (MF) {
        uint8_t v = rd(s, ea);
        io(s);
        C->p = (C->p & ~FZ) | ((v & C->a & 0xFF) ? 0 : FZ);
        wr(s, ea, v & ~(C->a & 0xFF));
    } else {
        uint16_t v = rd16(s, ea);
        io(s);
        C->p = (C->p & ~FZ) | ((v & C->a) ? 0 : FZ);
        wr16_rev(s, ea, v & ~C->a);
    }
}

static void branch(Snes *s, bool cond) {
    int8_t off = (int8_t)fetch(s);
    if (cond) {
        io(s);
        uint16_t target = C->pc + off;
        if (C->e && ((target ^ C->pc) & 0xFF00)) io(s);
        C->pc = target;
    }
}

static void interrupt(Snes *s, uint16_t nativeVec, uint16_t emuVec, bool software) {
    if (!software) {
        io(s);
        io(s);
    }
    if (!C->e) push8(s, C->pb);
    push16(s, C->pc);
    uint8_t p = C->p;
    if (C->e) p = software ? (p | 0x10) : (p & ~0x10);
    push8(s, p);
    C->p = (C->p | FI) & ~FD;
    C->pb = 0;
    C->pc = rd16(s, C->e ? emuVec : nativeVec);
}

static void inc_index(Snes *s, uint16_t *r, int delta) {
    io(s);
    if (XF) {
        *r = (*r + delta) & 0xFF;
        nz8(s, *r);
    } else {
        *r = (*r + delta) & 0xFFFF;
        nz16(s, *r);
    }
}

static void transfer_to_index(Snes *s, uint16_t *dst, uint16_t src) {
    io(s);
    if (XF) {
        *dst = src & 0xFF;
        nz8(s, *dst);
    } else {
        *dst = src;
        nz16(s, *dst);
    }
}

static void transfer_to_a(Snes *s, uint16_t src) {
    io(s);
    if (MF) {
        C->a = (C->a & 0xFF00) | (src & 0xFF);
        nz8(s, src & 0xFF);
    } else {
        C->a = src;
        nz16(s, src);
    }
}

static void block_move(Snes *s, int delta) {
    uint8_t dst = fetch(s);
    uint8_t src = fetch(s);
    C->db = dst;
    uint8_t v = rd(s, ((uint32_t)src << 16) | C->x);
    wr(s, ((uint32_t)dst << 16) | C->y, v);
    io(s);
    io(s);
    if (XF) {
        C->x = (C->x + delta) & 0xFF;
        C->y = (C->y + delta) & 0xFF;
    } else {
        C->x += delta;
        C->y += delta;
    }
    C->a--;
    if (C->a != 0xFFFF) C->pc -= 3;
}

void cpu_reset(Snes *s) {
    memset(C, 0, sizeof(Cpu));
    C->e = true;
    C->p = FM | FX | FI;
    C->sp = 0x01FF;
    C->pc = bus_read(s, 0xFFFC) | (bus_read(s, 0xFFFD) << 8);
}

#define GROUP1(base, fn, w)                                \
    case base + 0x01: fn(s, adr_idpx(s)); break;           \
    case base + 0x03: fn(s, adr_sr(s)); break;             \
    case base + 0x05: fn(s, adr_dp(s)); break;             \
    case base + 0x07: fn(s, adr_ildp(s)); break;           \
    case base + 0x0D: fn(s, adr_abs(s)); break;            \
    case base + 0x0F: fn(s, adr_long(s)); break;           \
    case base + 0x11: fn(s, adr_idpy(s, w)); break;        \
    case base + 0x12: fn(s, adr_idp(s)); break;            \
    case base + 0x13: fn(s, adr_isry(s)); break;           \
    case base + 0x15: fn(s, adr_dpx(s)); break;            \
    case base + 0x17: fn(s, adr_ildpy(s)); break;          \
    case base + 0x19: fn(s, adr_absy(s, w)); break;        \
    case base + 0x1D: fn(s, adr_absx(s, w)); break;        \
    case base + 0x1F: fn(s, adr_longx(s)); break;

#define RMW4(o_dp, o_abs, o_dpx, o_absx, fn)               \
    case o_dp: rmw(s, adr_dp(s), fn); break;               \
    case o_abs: rmw(s, adr_abs(s), fn); break;             \
    case o_dpx: rmw(s, adr_dpx(s), fn); break;             \
    case o_absx: rmw(s, adr_absx(s, true), fn); break;

void cpu_step(Snes *s) {
    if (C->stopped) {
        snes_tick(s, 24);
        return;
    }
    if (C->nmiPending) {
        C->nmiPending = false;
        C->waiting = false;
        interrupt(s, 0xFFEA, 0xFFFA, false);
        return;
    }
    if (C->irqLine) {
        C->waiting = false;
        if (!(C->p & FI)) {
            interrupt(s, 0xFFEE, 0xFFFE, false);
            return;
        }
    }
    if (C->waiting) {
        int skip = s->nextEvent - s->hpos;
        snes_tick(s, skip > 6 ? skip : 6);
        return;
    }

    uint8_t op = fetch(s);
    switch (op) {
    GROUP1(0x00, op_ora, false)
    GROUP1(0x20, op_and, false)
    GROUP1(0x40, op_eor, false)
    GROUP1(0x60, op_adc, false)
    GROUP1(0x80, op_sta, true)
    GROUP1(0xA0, op_lda, false)
    GROUP1(0xC0, op_cmp, false)
    GROUP1(0xE0, op_sbc, false)

    case 0x09: op_ora(s, imm_m(s)); break;
    case 0x29: op_and(s, imm_m(s)); break;
    case 0x49: op_eor(s, imm_m(s)); break;
    case 0x69: op_adc(s, imm_m(s)); break;
    case 0x89: op_bit_imm(s); break;
    case 0xA9: op_lda(s, imm_m(s)); break;
    case 0xC9: op_cmp(s, imm_m(s)); break;
    case 0xE9: op_sbc(s, imm_m(s)); break;

    RMW4(0x06, 0x0E, 0x16, 0x1E, f_asl)
    RMW4(0x26, 0x2E, 0x36, 0x3E, f_rol)
    RMW4(0x46, 0x4E, 0x56, 0x5E, f_lsr)
    RMW4(0x66, 0x6E, 0x76, 0x7E, f_ror)
    RMW4(0xC6, 0xCE, 0xD6, 0xDE, f_dec)
    RMW4(0xE6, 0xEE, 0xF6, 0xFE, f_inc)
    case 0x0A: rmw_acc(s, f_asl); break;
    case 0x2A: rmw_acc(s, f_rol); break;
    case 0x4A: rmw_acc(s, f_lsr); break;
    case 0x6A: rmw_acc(s, f_ror); break;
    case 0x1A: rmw_acc(s, f_inc); break;
    case 0x3A: rmw_acc(s, f_dec); break;

    case 0x04: op_tsb(s, adr_dp(s)); break;
    case 0x0C: op_tsb(s, adr_abs(s)); break;
    case 0x14: op_trb(s, adr_dp(s)); break;
    case 0x1C: op_trb(s, adr_abs(s)); break;

    case 0x24: op_bit(s, adr_dp(s)); break;
    case 0x2C: op_bit(s, adr_abs(s)); break;
    case 0x34: op_bit(s, adr_dpx(s)); break;
    case 0x3C: op_bit(s, adr_absx(s, false)); break;

    case 0xA2: op_ldx(s, imm_x(s)); break;
    case 0xA6: op_ldx(s, adr_dp(s)); break;
    case 0xAE: op_ldx(s, adr_abs(s)); break;
    case 0xB6: op_ldx(s, adr_dpy(s)); break;
    case 0xBE: op_ldx(s, adr_absy(s, false)); break;
    case 0xA0: op_ldy(s, imm_x(s)); break;
    case 0xA4: op_ldy(s, adr_dp(s)); break;
    case 0xAC: op_ldy(s, adr_abs(s)); break;
    case 0xB4: op_ldy(s, adr_dpx(s)); break;
    case 0xBC: op_ldy(s, adr_absx(s, false)); break;

    case 0x86: op_stx(s, adr_dp(s)); break;
    case 0x8E: op_stx(s, adr_abs(s)); break;
    case 0x96: op_stx(s, adr_dpy(s)); break;
    case 0x84: op_sty(s, adr_dp(s)); break;
    case 0x8C: op_sty(s, adr_abs(s)); break;
    case 0x94: op_sty(s, adr_dpx(s)); break;
    case 0x64: op_stz(s, adr_dp(s)); break;
    case 0x74: op_stz(s, adr_dpx(s)); break;
    case 0x9C: op_stz(s, adr_abs(s)); break;
    case 0x9E: op_stz(s, adr_absx(s, true)); break;

    case 0xE0: op_cpx(s, imm_x(s)); break;
    case 0xE4: op_cpx(s, adr_dp(s)); break;
    case 0xEC: op_cpx(s, adr_abs(s)); break;
    case 0xC0: op_cpy(s, imm_x(s)); break;
    case 0xC4: op_cpy(s, adr_dp(s)); break;
    case 0xCC: op_cpy(s, adr_abs(s)); break;

    case 0xE8: inc_index(s, &C->x, 1); break;
    case 0xC8: inc_index(s, &C->y, 1); break;
    case 0xCA: inc_index(s, &C->x, -1); break;
    case 0x88: inc_index(s, &C->y, -1); break;

    // Branches
    case 0x10: branch(s, !(C->p & FN)); break;
    case 0x30: branch(s, C->p & FN); break;
    case 0x50: branch(s, !(C->p & FV)); break;
    case 0x70: branch(s, C->p & FV); break;
    case 0x90: branch(s, !(C->p & FC)); break;
    case 0xB0: branch(s, C->p & FC); break;
    case 0xD0: branch(s, !(C->p & FZ)); break;
    case 0xF0: branch(s, C->p & FZ); break;
    case 0x80: branch(s, true); break;
    case 0x82: {
        int16_t off = (int16_t)fetch16(s);
        io(s);
        C->pc += off;
        break;
    }

    // Jumps and calls
    case 0x4C: C->pc = fetch16(s); break;
    case 0x5C: {
        uint32_t t = fetch24(s);
        C->pc = t & 0xFFFF;
        C->pb = t >> 16;
        break;
    }
    case 0x6C: {
        uint16_t ptr = fetch16(s);
        C->pc = rd16(s, ptr | WRAP16);
        break;
    }
    case 0x7C: {
        uint16_t ptr = fetch16(s) + C->x;
        io(s);
        C->pc = rd16(s, ((uint32_t)C->pb << 16) | ptr | WRAP16);
        break;
    }
    case 0xDC: {
        uint16_t ptr = fetch16(s);
        C->pc = rd16(s, ptr | WRAP16);
        C->pb = rd(s, (ptr + 2) & 0xFFFF);
        break;
    }
    case 0x20: {
        uint16_t t = fetch16(s);
        io(s);
        push16(s, C->pc - 1);
        C->pc = t;
        break;
    }
    case 0xFC: {
        uint16_t t = fetch16(s);
        push16(s, C->pc - 1);
        io(s);
        C->pc = rd16(s, ((uint32_t)C->pb << 16) | ((t + C->x) & 0xFFFF) | WRAP16);
        break;
    }
    case 0x22: {
        uint16_t t = fetch16(s);
        push8(s, C->pb);
        io(s);
        uint8_t bank = fetch(s);
        push16(s, C->pc - 1);
        C->pc = t;
        C->pb = bank;
        break;
    }
    case 0x60:
        io(s);
        io(s);
        C->pc = pull16(s) + 1;
        io(s);
        break;
    case 0x6B:
        io(s);
        io(s);
        C->pc = pull16(s) + 1;
        C->pb = pull8(s);
        break;
    case 0x40:
        io(s);
        io(s);
        set_p(s, pull8(s));
        C->pc = pull16(s);
        if (!C->e) C->pb = pull8(s);
        break;
    case 0x00:
        fetch(s);
        interrupt(s, 0xFFE6, 0xFFFE, true);
        break;
    case 0x02:
        fetch(s);
        interrupt(s, 0xFFE4, 0xFFF4, true);
        break;

    // Stack
    case 0x48:
        io(s);
        if (MF) push8(s, C->a & 0xFF);
        else push16(s, C->a);
        break;
    case 0x68:
        io(s);
        io(s);
        if (MF) {
            uint8_t v = pull8(s);
            C->a = (C->a & 0xFF00) | v;
            nz8(s, v);
        } else {
            C->a = pull16(s);
            nz16(s, C->a);
        }
        break;
    case 0xDA:
        io(s);
        if (XF) push8(s, C->x & 0xFF);
        else push16(s, C->x);
        break;
    case 0xFA:
        io(s);
        io(s);
        if (XF) {
            C->x = pull8(s);
            nz8(s, C->x);
        } else {
            C->x = pull16(s);
            nz16(s, C->x);
        }
        break;
    case 0x5A:
        io(s);
        if (XF) push8(s, C->y & 0xFF);
        else push16(s, C->y);
        break;
    case 0x7A:
        io(s);
        io(s);
        if (XF) {
            C->y = pull8(s);
            nz8(s, C->y);
        } else {
            C->y = pull16(s);
            nz16(s, C->y);
        }
        break;
    case 0x08: io(s); push8(s, C->p); break;
    case 0x28: io(s); io(s); set_p(s, pull8(s)); break;
    case 0x8B: io(s); push8(s, C->db); break;
    case 0xAB:
        io(s);
        io(s);
        C->db = pull8(s);
        nz8(s, C->db);
        break;
    case 0x0B: io(s); push16(s, C->dp); break;
    case 0x2B:
        io(s);
        io(s);
        C->dp = pull16(s);
        nz16(s, C->dp);
        break;
    case 0x4B: io(s); push8(s, C->pb); break;
    case 0xF4: push16(s, fetch16(s)); break;
    case 0xD4: push16(s, rd16(s, adr_dp(s))); break;
    case 0x62: {
        uint16_t off = fetch16(s);
        io(s);
        push16(s, C->pc + off);
        break;
    }

    // Transfers
    case 0xAA: transfer_to_index(s, &C->x, C->a); break;
    case 0xA8: transfer_to_index(s, &C->y, C->a); break;
    case 0xBA: transfer_to_index(s, &C->x, C->sp); break;
    case 0x9B: transfer_to_index(s, &C->y, C->x); break;
    case 0xBB: transfer_to_index(s, &C->x, C->y); break;
    case 0x8A: transfer_to_a(s, C->x); break;
    case 0x98: transfer_to_a(s, C->y); break;
    case 0x9A:
        io(s);
        C->sp = C->e ? (0x100 | (C->x & 0xFF)) : C->x;
        break;
    case 0x5B: io(s); C->dp = C->a; nz16(s, C->dp); break;
    case 0x7B: io(s); C->a = C->dp; nz16(s, C->a); break;
    case 0x1B:
        io(s);
        C->sp = C->e ? (0x100 | (C->a & 0xFF)) : C->a;
        break;
    case 0x3B: io(s); C->a = C->sp; nz16(s, C->a); break;
    case 0xEB:
        io(s);
        io(s);
        C->a = (C->a >> 8) | (C->a << 8);
        nz8(s, C->a & 0xFF);
        break;

    // Flags
    case 0x18: io(s); C->p &= ~FC; break;
    case 0x38: io(s); C->p |= FC; break;
    case 0x58: io(s); C->p &= ~FI; break;
    case 0x78: io(s); C->p |= FI; break;
    case 0xD8: io(s); C->p &= ~FD; break;
    case 0xF8: io(s); C->p |= FD; break;
    case 0xB8: io(s); C->p &= ~FV; break;
    case 0xC2: {
        uint8_t v = fetch(s);
        io(s);
        set_p(s, C->p & ~v);
        break;
    }
    case 0xE2: {
        uint8_t v = fetch(s);
        io(s);
        set_p(s, C->p | v);
        break;
    }
    case 0xFB: {
        io(s);
        bool carry = C->p & FC;
        C->p = (C->p & ~FC) | (C->e ? FC : 0);
        C->e = carry;
        if (C->e) {
            C->p |= FM | FX;
            C->sp = 0x100 | (C->sp & 0xFF);
        }
        set_p(s, C->p);
        break;
    }

    // Misc
    case 0x54: block_move(s, 1); break;
    case 0x44: block_move(s, -1); break;
    case 0xCB: io(s); io(s); C->waiting = true; break;
    case 0xDB: io(s); io(s); C->stopped = true; break;
    case 0xEA: io(s); break;
    case 0x42: fetch(s); break;
    }
}
