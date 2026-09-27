// i386 user-mode interpreter. See x86.h.
// Scope: what 1990s MSVC-compiled engine code uses - the integer ISA (no BCD, no I/O, no far
// transfers, no 16-bit addressing), string ops, the 0F map subset (jcc, setcc, cmov, movzx/sx,
// bt*, bsf/bsr, shld/shrd, bswap, xadd, cmpxchg, cpuid, rdtsc) and the full x87 FPU modelled
// with IEEE doubles (the Win32 default control word selects 53-bit precision anyway).
#include "x86.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <fenv.h>
#include <stdlib.h>
#include <sys/mman.h>

#define R c->s.r
#define EIP c->s.eip

// PF: 1 when the low byte has an even number of set bits (compile-time table: no shared mutable state)
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
static const uint8_t parity_tab[256] = { P6(1), P6(0), P6(0), P6(1) };
#undef P2
#undef P4
#undef P6

void x86_init(X86 *c, uint8_t *mem, uint32_t mem_size) {
    memset(c, 0, sizeof(*c));
    c->mem = mem;
    c->mem_size = mem_size;
    c->s.fpu.cw = 0x027F;
    for (int i = 0; i < 8; i++) c->s.fpu.empty[i] = 1;
}

void x86_fault(X86 *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->fault_msg, sizeof c->fault_msg, fmt, ap);
    va_end(ap);
    c->faulted = 1;
    c->stop = 1;
    if (c->fault_jmp) longjmp(*c->fault_jmp, 1);
#ifndef CV_NO_DEBUG_ENV
    fprintf(stderr, "x86 fault outside run: %s\n", c->fault_msg);
#endif
    abort();   // unreachable: every entry point (sapi4_tts, emu_call, emu_pump) installs a handler
}

void x86_dump(X86 *c) {
#ifdef CV_NO_DEBUG_ENV
    (void)c;
#else
    fprintf(stderr, "EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x EIP=%08x (insn %08x)\n",
            R[EAX], R[ECX], R[EDX], R[EBX], R[ESP], R[EBP], R[ESI], R[EDI], EIP, c->cur_insn);
    fprintf(stderr, "flags CF=%d PF=%d AF=%d ZF=%d SF=%d OF=%d DF=%d  fpu top=%d cw=%04x sw=%04x icount=%llu\n",
            c->s.cf, c->s.pf, c->s.af, c->s.zf, c->s.sf, c->s.of, c->s.df, c->s.fpu.top, c->s.fpu.cw, c->s.fpu.sw,
            (unsigned long long)c->icount);
    fprintf(stderr, "recent EIPs:");
    for (unsigned i = 0; i < 64; i++) {
        unsigned k = (c->hist_pos + i) & 63;
        if (c->hist[k]) fprintf(stderr, " %08x", c->hist[k]);
    }
    fprintf(stderr, "\n");
#endif
}

// ---------------------------------------------------------------- fetch / operands
static inline uint8_t fetch8(X86 *c) { uint8_t v = c->mem[EIP]; EIP++; return v; }
static inline uint16_t fetch16(X86 *c) { uint16_t v; memcpy(&v, c->mem + EIP, 2); EIP += 2; return v; }
static inline uint32_t fetch32(X86 *c) { uint32_t v; memcpy(&v, c->mem + EIP, 4); EIP += 4; return v; }

typedef struct { uint32_t addr; int mod, reg, rm; } MRM;

static inline void modrm(X86 *c, MRM *m, uint32_t seg) {
    uint8_t b = fetch8(c);
    m->mod = b >> 6; m->reg = (b >> 3) & 7; m->rm = b & 7;
    if (m->mod == 3) return;
    uint32_t a;
    if (m->rm == 4) {
        uint8_t sib = fetch8(c);
        int scale = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        if (base == 5 && m->mod == 0) a = fetch32(c);
        else a = R[base];
        if (idx != 4) a += R[idx] << scale;
    } else if (m->rm == 5 && m->mod == 0) {
        a = fetch32(c);
    } else {
        a = R[m->rm];
    }
    if (m->mod == 1) a += (uint32_t)(int32_t)(int8_t)fetch8(c);
    else if (m->mod == 2) a += fetch32(c);
    m->addr = a + seg;
}

static inline uint32_t getreg(X86 *c, int r, int sz) {
    if (sz == 4) return R[r];
    if (sz == 2) return R[r] & 0xFFFF;
    return (r < 4) ? (R[r] & 0xFF) : ((R[r - 4] >> 8) & 0xFF);
}
static inline void setreg(X86 *c, int r, int sz, uint32_t v) {
    if (sz == 4) R[r] = v;
    else if (sz == 2) R[r] = (R[r] & 0xFFFF0000u) | (v & 0xFFFF);
    else if (r < 4) R[r] = (R[r] & 0xFFFFFF00u) | (v & 0xFF);
    else R[r - 4] = (R[r - 4] & 0xFFFF00FFu) | ((v & 0xFF) << 8);
}
static inline uint32_t rdmem(X86 *c, uint32_t a, int sz) {
    return sz == 4 ? rd32(c, a) : sz == 2 ? rd16(c, a) : rd8(c, a);
}
static inline void wrmem(X86 *c, uint32_t a, int sz, uint32_t v) {
    if (sz == 4) wr32(c, a, v); else if (sz == 2) wr16(c, a, (uint16_t)v); else wr8(c, a, (uint8_t)v);
}
static inline uint32_t rdE(X86 *c, MRM *m, int sz) { return m->mod == 3 ? getreg(c, m->rm, sz) : rdmem(c, m->addr, sz); }
static inline void wrE(X86 *c, MRM *m, int sz, uint32_t v) { if (m->mod == 3) setreg(c, m->rm, sz, v); else wrmem(c, m->addr, sz, v); }
static inline uint32_t fetchimm(X86 *c, int sz) { return sz == 4 ? fetch32(c) : sz == 2 ? fetch16(c) : fetch8(c); }

#define MASK(sz) ((sz) == 4 ? 0xFFFFFFFFu : (sz) == 2 ? 0xFFFFu : 0xFFu)
#define SIGN(sz) ((sz) == 4 ? 0x80000000u : (sz) == 2 ? 0x8000u : 0x80u)
static inline uint32_t sext(uint32_t v, int sz) { return sz == 4 ? v : sz == 2 ? (uint32_t)(int32_t)(int16_t)v : (uint32_t)(int32_t)(int8_t)v; }

// ---------------------------------------------------------------- flags
static inline void szp(X86 *c, uint32_t r, int sz) {
    r &= MASK(sz);
    c->s.zf = r == 0;
    c->s.sf = (r & SIGN(sz)) != 0;
    c->s.pf = parity_tab[r & 0xFF];
}

static uint32_t alu(X86 *c, int op, uint32_t a, uint32_t b, int sz) {
    uint32_t mask = MASK(sz), sign = SIGN(sz), r;
    a &= mask; b &= mask;
    switch (op) {
    case 0: r = (a + b) & mask; c->s.cf = r < a; c->s.of = (((a ^ r) & (b ^ r)) & sign) != 0; c->s.af = ((a ^ b ^ r) >> 4) & 1; break;
    case 1: r = a | b; c->s.cf = c->s.of = c->s.af = 0; break;
    case 2: { uint32_t ci = c->s.cf; r = (a + b + ci) & mask; c->s.cf = ci ? (r <= a) : (r < a);
              c->s.of = (((a ^ r) & (b ^ r)) & sign) != 0; c->s.af = ((a ^ b ^ r) >> 4) & 1; break; }
    case 3: { uint32_t ci = c->s.cf; r = (a - b - ci) & mask; c->s.cf = ci ? (a <= b) : (a < b);
              c->s.of = (((a ^ b) & (a ^ r)) & sign) != 0; c->s.af = ((a ^ b ^ r) >> 4) & 1; break; }
    case 4: r = a & b; c->s.cf = c->s.of = c->s.af = 0; break;
    case 6: r = a ^ b; c->s.cf = c->s.of = c->s.af = 0; break;
    default: /* 5 sub, 7 cmp */
        r = (a - b) & mask; c->s.cf = a < b; c->s.of = (((a ^ b) & (a ^ r)) & sign) != 0; c->s.af = ((a ^ b ^ r) >> 4) & 1; break;
    }
    szp(c, r, sz);
    return r;
}

static inline uint32_t inc_dec(X86 *c, uint32_t a, int sz, int dec) {
    uint32_t mask = MASK(sz), sign = SIGN(sz), r;
    a &= mask;
    if (!dec) { r = (a + 1) & mask; c->s.of = r == sign; c->s.af = (r & 0xF) == 0; }
    else { r = (a - 1) & mask; c->s.of = a == sign; c->s.af = (a & 0xF) == 0; }
    szp(c, r, sz);
    return r;
}

static inline int cond(X86 *c, int cc) {
    int r;
    switch (cc >> 1) {
    case 0: r = c->s.of; break;
    case 1: r = c->s.cf; break;
    case 2: r = c->s.zf; break;
    case 3: r = c->s.cf | c->s.zf; break;
    case 4: r = c->s.sf; break;
    case 5: r = c->s.pf; break;
    case 6: r = c->s.sf != c->s.of; break;
    default: r = c->s.zf | (c->s.sf != c->s.of); break;
    }
    return (cc & 1) ? !r : r;
}

static inline uint32_t get_eflags(X86 *c) {
    return 0x2 | c->s.cf | (c->s.pf << 2) | (c->s.af << 4) | (c->s.zf << 6) | (c->s.sf << 7) | (1u << 9) |
           (c->s.df << 10) | (c->s.of << 11);
}
static inline void set_eflags(X86 *c, uint32_t f) {
    c->s.cf = f & 1; c->s.pf = (f >> 2) & 1; c->s.af = (f >> 4) & 1; c->s.zf = (f >> 6) & 1;
    c->s.sf = (f >> 7) & 1; c->s.df = (f >> 10) & 1; c->s.of = (f >> 11) & 1;
}

static uint32_t shift_op(X86 *c, int op, uint32_t a, unsigned cnt, int sz) {
    unsigned bits = sz * 8;
    uint32_t mask = MASK(sz), sign = SIGN(sz), r = a & mask;
    cnt &= 0x1F;
    if (!cnt) return r;
    a &= mask;
    switch (op) {
    case 0: { // ROL
        unsigned n = cnt % bits;
        r = n ? ((a << n) | (a >> (bits - n))) & mask : a;
        c->s.cf = r & 1; c->s.of = ((r & sign) != 0) ^ c->s.cf; return r; }
    case 1: { // ROR
        unsigned n = cnt % bits;
        r = n ? ((a >> n) | (a << (bits - n))) & mask : a;
        c->s.cf = (r & sign) != 0; c->s.of = ((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1; return r; }
    case 2: { // RCL
        unsigned n = cnt % (bits + 1);
        r = a;
        for (unsigned i = 0; i < n; i++) { uint32_t out = (r & sign) != 0; r = ((r << 1) | c->s.cf) & mask; c->s.cf = out; }
        c->s.of = ((r & sign) != 0) ^ c->s.cf; return r; }
    case 3: { // RCR
        unsigned n = cnt % (bits + 1);
        r = a;
        c->s.of = ((r & sign) != 0) ^ c->s.cf;
        for (unsigned i = 0; i < n; i++) { uint32_t out = r & 1; r = (r >> 1) | (c->s.cf ? sign : 0); c->s.cf = out; }
        return r; }
    case 4: case 6: // SHL/SAL
        if (cnt <= bits) { c->s.cf = cnt == bits ? (a & 1) : (a >> (bits - cnt)) & 1; r = cnt == 32 ? 0 : (a << cnt) & mask; }
        else { c->s.cf = 0; r = 0; }
        c->s.of = ((r & sign) != 0) ^ c->s.cf; c->s.af = 0;
        szp(c, r, sz); return r;
    case 5: // SHR
        c->s.cf = cnt <= bits ? (a >> (cnt - 1)) & 1 : 0;
        r = cnt >= bits ? 0 : a >> cnt;
        c->s.of = (a & sign) != 0; c->s.af = 0;
        szp(c, r, sz); return r;
    default: { // SAR
        int32_t sa = (int32_t)sext(a, sz);
        unsigned n = cnt >= bits ? bits - 1 : cnt;
        c->s.cf = (sa >> (cnt >= bits ? bits - 1 : cnt - 1)) & 1;
        r = (uint32_t)(sa >> n) & mask;
        c->s.of = 0; c->s.af = 0;
        szp(c, r, sz); return r; }
    }
}

// ---------------------------------------------------------------- FPU
#define FPU c->s.fpu
#define ST(i) FPU.st[(FPU.top + (i)) & 7]
#define C0 0x0100
#define C1 0x0200
#define C2 0x0400
#define C3 0x4000

void x86_fpu_push(X86 *c, double v) {
    FPU.top = (FPU.top - 1) & 7;
    FPU.st[FPU.top] = v;
    FPU.empty[FPU.top] = 0;
}
double x86_fpu_pop(X86 *c) {
    double v = FPU.st[FPU.top];
    FPU.empty[FPU.top] = 1;
    FPU.top = (FPU.top + 1) & 7;
    return v;
}
#define fpush(v) x86_fpu_push(c, (v))
#define fpop() x86_fpu_pop(c)

static inline uint16_t fpu_status(X86 *c) { return (uint16_t)((FPU.sw & ~0x3800) | ((FPU.top & 7) << 11)); }

static void fcom_set(X86 *c, double a, double b) {
    FPU.sw &= ~(C0 | C1 | C2 | C3);
    if (isnan(a) || isnan(b)) FPU.sw |= C0 | C2 | C3;
    else if (a < b) FPU.sw |= C0;
    else if (a == b) FPU.sw |= C3;
}
static void fcomi_set(X86 *c, double a, double b) {
    c->s.of = c->s.sf = c->s.af = 0;
    if (isnan(a) || isnan(b)) { c->s.zf = c->s.pf = c->s.cf = 1; }
    else { c->s.zf = a == b; c->s.pf = 0; c->s.cf = a < b; }
}

static double fround(X86 *c, double v) {
    switch ((FPU.cw >> 10) & 3) {
    case 0: return nearbyint(v);   // host default rounding mode is nearest-even
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
static int64_t fto_int(X86 *c, double v, int bits) {
    double r = fround(c, v);
    double lim = ldexp(1.0, bits - 1);
    if (!(r >= -lim && r < lim)) return bits == 64 ? INT64_MIN : -((int64_t)1 << (bits - 1)); // integer indefinite
    return (int64_t)r;
}

static double f80_load(X86 *c, uint32_t a) {
    uint64_t mant = rd64(c, a);
    uint16_t se = rd16(c, a + 8);
    int exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}
static void f80_store(X86 *c, uint32_t a, double v) {
    uint64_t mant; uint16_t se;
    int neg = signbit(v) != 0;
    if (v == 0) { mant = 0; se = 0; }
    else if (isinf(v)) { mant = 1ull << 63; se = 0x7FFF; }
    else if (isnan(v)) { mant = 0xC000000000000000ull; se = 0x7FFF; }
    else { int e; double f = frexp(fabs(v), &e); mant = (uint64_t)ldexp(f, 64); se = (uint16_t)(e - 1 + 16383); }
    if (neg) se |= 0x8000;
    wr64(c, a, mant);
    wr16(c, a + 8, se);
}

static uint16_t fpu_tagword(X86 *c) {
    uint16_t tw = 0;
    for (int i = 0; i < 8; i++) {
        int t;
        if (FPU.empty[i]) t = 3;
        else if (FPU.st[i] == 0) t = 1;
        else if (!isfinite(FPU.st[i])) t = 2;
        else t = 0;
        tw |= (uint16_t)(t << (2 * i));
    }
    return tw;
}
static void fpu_stenv(X86 *c, uint32_t a) {
    wr32(c, a, 0xFFFF0000u | FPU.cw); wr32(c, a + 4, 0xFFFF0000u | fpu_status(c));
    wr32(c, a + 8, 0xFFFF0000u | fpu_tagword(c));
    wr32(c, a + 12, 0); wr32(c, a + 16, 0); wr32(c, a + 20, 0); wr32(c, a + 24, 0);
}
static void fpu_ldenv(X86 *c, uint32_t a) {
    FPU.cw = rd16(c, a);
    uint16_t sw = rd16(c, a + 4);
    FPU.top = (sw >> 11) & 7; FPU.sw = sw & ~0x3800;
    uint16_t tw = rd16(c, a + 8);
    for (int i = 0; i < 8; i++) FPU.empty[i] = ((tw >> (2 * i)) & 3) == 3;
}

static double farith(int op, double d, double s) {
    // op: 0 add 1 mul 4 sub(d-s) 5 subr(s-d) 6 div(d/s) 7 divr(s/d)
    switch (op) {
    case 0: return d + s;
    case 1: return d * s;
    case 4: return d - s;
    case 5: return s - d;
    case 6: return d / s;
    default: return s / d;
    }
}

static void fpu_exec(X86 *c, uint8_t op, uint32_t seg) {
    MRM m;
    modrm(c, &m, seg);
    int sub = m.reg;
    if (m.mod != 3) {
        uint32_t a = m.addr;
        switch (op) {
        case 0xD8: case 0xDC: case 0xDA: case 0xDE: {
            float f32 = 0;
            if (op == 0xD8) memcpy(&f32, x86_ptr(c, a, 4), 4);
            double v = op == 0xD8 ? (double)f32
                     : op == 0xDC ? rdf64(c, a)
                     : op == 0xDA ? (double)(int32_t)rd32(c, a)
                                  : (double)(int16_t)rd16(c, a);
            if (sub == 2 || sub == 3) { fcom_set(c, ST(0), v); if (sub == 3) fpop(); }
            else ST(0) = farith(sub, ST(0), v);
            return; }
        case 0xD9:
            switch (sub) {
            case 0: { float f; memcpy(&f, x86_ptr(c, a, 4), 4); fpush((double)f); return; }
            case 2: case 3: { float f = (float)ST(0); uint32_t b; memcpy(&b, &f, 4); wr32(c, a, b); if (sub == 3) fpop(); return; }
            case 4: fpu_ldenv(c, a); return;
            case 5: FPU.cw = rd16(c, a); return;
            case 6: fpu_stenv(c, a); return;
            case 7: wr16(c, a, FPU.cw); return;
            }
            break;
        case 0xDB:
            switch (sub) {
            case 0: fpush((double)(int32_t)rd32(c, a)); return;
            case 1: wr32(c, a, (uint32_t)(int32_t)fto_int(c, trunc(ST(0)), 32)); fpop(); return;
            case 2: case 3: wr32(c, a, (uint32_t)(int32_t)fto_int(c, ST(0), 32)); if (sub == 3) fpop(); return;
            case 5: fpush(f80_load(c, a)); return;
            case 7: f80_store(c, a, ST(0)); fpop(); return;
            }
            break;
        case 0xDD:
            switch (sub) {
            case 0: fpush(rdf64(c, a)); return;
            case 1: { int64_t v = fto_int(c, trunc(ST(0)), 64); wr64(c, a, (uint64_t)v); fpop(); return; }
            case 2: case 3: { double d = ST(0); uint64_t b; memcpy(&b, &d, 8); wr64(c, a, b); if (sub == 3) fpop(); return; }
            case 4: { // frstor
                fpu_ldenv(c, a);
                for (int i = 0; i < 8; i++) FPU.st[(FPU.top + i) & 7] = f80_load(c, a + 28 + 10 * i);
                return; }
            case 6: { // fnsave
                fpu_stenv(c, a);
                for (int i = 0; i < 8; i++) f80_store(c, a + 28 + 10 * i, FPU.st[(FPU.top + i) & 7]);
                FPU.cw = 0x037F; FPU.sw = 0; FPU.top = 0;
                for (int i = 0; i < 8; i++) FPU.empty[i] = 1;
                return; }
            case 7: wr16(c, a, fpu_status(c)); return;
            }
            break;
        case 0xDF:
            switch (sub) {
            case 0: fpush((double)(int16_t)rd16(c, a)); return;
            case 1: wr16(c, a, (uint16_t)(int16_t)fto_int(c, trunc(ST(0)), 16)); fpop(); return;
            case 2: case 3: wr16(c, a, (uint16_t)(int16_t)fto_int(c, ST(0), 16)); if (sub == 3) fpop(); return;
            case 5: fpush((double)(int64_t)rd64(c, a)); return;
            case 7: wr64(c, a, (uint64_t)fto_int(c, ST(0), 64)); fpop(); return;
            }
            break;
        }
        x86_fault(c, "unsupported x87 memory op %02x /%d", op, sub);
    }
    int i = m.rm;
    switch (op) {
    case 0xD8:
        if (sub == 2 || sub == 3) { fcom_set(c, ST(0), ST(i)); if (sub == 3) fpop(); }
        else ST(0) = farith(sub, ST(0), ST(i));
        return;
    case 0xDC: case 0xDE:
        if (sub == 2 || sub == 3) { // FCOM/FCOMP aliases, DE D9 = FCOMPP
            fcom_set(c, ST(0), ST(i));
            if (op == 0xDE) { if (i == 1 && sub == 3) { fpop(); fpop(); return; } break; }
            if (sub == 3) fpop();
            return;
        }
        {   // ST(i) = ST(i) op ST(0) with reversed sub/div encodings
            int o = sub;
            if (o == 4) o = 5; else if (o == 5) o = 4; else if (o == 6) o = 7; else if (o == 7) o = 6;
            ST(i) = farith(o, ST(i), ST(0));
            if (op == 0xDE) fpop();
            return;
        }
    case 0xD9:
        switch (sub) {
        case 0: { double v = ST(i); fpush(v); return; }
        case 1: { double t = ST(0); ST(0) = ST(i); ST(i) = t; return; }
        case 2: if (i == 0) return; break; // fnop
        case 3: { double v = ST(0); ST(i) = v; fpop(); return; } // fstp1 alias
        case 4:
            switch (i) {
            case 0: ST(0) = -ST(0); return;
            case 1: ST(0) = fabs(ST(0)); return;
            case 4: fcom_set(c, ST(0), 0.0); return;
            case 5: { // fxam
                double v = ST(0);
                FPU.sw &= ~(C0 | C1 | C2 | C3);
                if (signbit(v)) FPU.sw |= C1;
                if (FPU.empty[FPU.top]) FPU.sw |= C3 | C0;
                else if (isnan(v)) FPU.sw |= C0;
                else if (isinf(v)) FPU.sw |= C2 | C0;
                else if (v == 0) FPU.sw |= C3;
                else if (fpclassify(v) == FP_SUBNORMAL) FPU.sw |= C3 | C2;
                else FPU.sw |= C2;
                return; }
            }
            break;
        case 5:
            switch (i) {
            case 0: fpush(1.0); return;
            case 1: fpush(3.321928094887362347870319429489390175864831393); return; // log2(10)
            case 2: fpush(1.442695040888963407359924681001892137426645954); return; // log2(e)
            case 3: fpush(M_PI); return;
            case 4: fpush(0.301029995663981195213738894724493026768189881); return; // log10(2)
            case 5: fpush(M_LN2); return;
            case 6: fpush(0.0); return;
            }
            break;
        case 6:
            switch (i) {
            case 0: ST(0) = exp2(ST(0)) - 1.0; return;
            case 1: { double x = ST(0), y = ST(1); fpop(); ST(0) = y * log2(x); return; }
            case 2: { ST(0) = tan(ST(0)); fpush(1.0); FPU.sw &= ~C2; return; }
            case 3: { double x = ST(0), y = ST(1); fpop(); ST(0) = atan2(y, x); return; }
            case 4: { double v = ST(0); int e; double f = frexp(v, &e); ST(0) = (double)(e - 1); fpush(f * 2.0); return; }
            case 5: { double q = remainder(ST(0), ST(1)); ST(0) = q; FPU.sw &= ~(C0 | C1 | C2 | C3); return; }
            case 6: FPU.top = (FPU.top - 1) & 7; return;
            case 7: FPU.top = (FPU.top + 1) & 7; return;
            }
            break;
        case 7:
            switch (i) {
            case 0: { // fprem
                double a = ST(0), b = ST(1);
                double q = trunc(a / b);
                ST(0) = fmod(a, b);
                long long qi = (long long)fabs(q);
                FPU.sw &= ~(C0 | C1 | C2 | C3);
                if (qi & 1) FPU.sw |= C1;
                if (qi & 2) FPU.sw |= C3;
                if (qi & 4) FPU.sw |= C0;
                return; }
            case 1: { double x = ST(0), y = ST(1); fpop(); ST(0) = y * log2(x + 1.0); return; }
            case 2: ST(0) = sqrt(ST(0)); return;
            case 3: { double v = ST(0); ST(0) = sin(v); fpush(cos(v)); FPU.sw &= ~C2; return; }
            case 4: ST(0) = fround(c, ST(0)); return;
            case 5: ST(0) = ldexp(ST(0), (int)trunc(ST(1))); return;
            case 6: ST(0) = sin(ST(0)); FPU.sw &= ~C2; return;
            case 7: ST(0) = cos(ST(0)); FPU.sw &= ~C2; return;
            }
            break;
        }
        break;
    case 0xDA:
        if (m.reg < 4) { // fcmovb/e/be/u
            int t = m.reg == 0 ? c->s.cf : m.reg == 1 ? c->s.zf : m.reg == 2 ? (c->s.cf | c->s.zf) : c->s.pf;
            if (t) ST(0) = ST(i);
            return;
        }
        if (m.reg == 5 && i == 1) { fcom_set(c, ST(0), ST(1)); fpop(); fpop(); return; } // fucompp
        break;
    case 0xDB:
        if (m.reg < 4) {
            int t = m.reg == 0 ? c->s.cf : m.reg == 1 ? c->s.zf : m.reg == 2 ? (c->s.cf | c->s.zf) : c->s.pf;
            if (!t) ST(0) = ST(i);
            return;
        }
        if (m.reg == 4) {
            if (i == 2) { FPU.sw &= 0x7F00; return; }               // fnclex
            if (i == 3) { FPU.cw = 0x037F; FPU.sw = 0; FPU.top = 0; for (int k = 0; k < 8; k++) FPU.empty[k] = 1; return; } // fninit
            if (i == 0 || i == 1 || i == 4) return;                 // feni/fdisi/fsetpm: no-ops
        }
        if (m.reg == 5 || m.reg == 6) { fcomi_set(c, ST(0), ST(i)); return; }
        break;
    case 0xDD:
        switch (sub) {
        case 0: FPU.empty[(FPU.top + i) & 7] = 1; return;       // ffree
        case 2: ST(i) = ST(0); return;                           // fst
        case 3: { double v = ST(0); ST(i) = v; fpop(); return; } // fstp
        case 4: fcom_set(c, ST(0), ST(i)); return;               // fucom
        case 5: fcom_set(c, ST(0), ST(i)); fpop(); return;       // fucomp
        }
        break;
    case 0xDF:
        if (sub == 4 && i == 0) { R[EAX] = (R[EAX] & 0xFFFF0000u) | fpu_status(c); return; } // fnstsw ax
        if (sub == 5 || sub == 6) { fcomi_set(c, ST(0), ST(i)); fpop(); return; }             // fucomip/fcomip
        if (sub == 0) { FPU.empty[(FPU.top + i) & 7] = 1; fpop(); return; }                  // ffreep
        break;
    }
    x86_fault(c, "unsupported x87 register op %02x %02x", op, 0xC0 | (sub << 3) | i);
}

// ---------------------------------------------------------------- string ops
static void string_op(X86 *c, uint8_t op, int osz, int rep, uint32_t seg) {
    int sz = (op & 1) ? osz : 1;
    int step = c->s.df ? -sz : sz;
    uint32_t src_seg = seg; // DS default (flat) or override; ES:EDI is always flat
    for (;;) {
        if (rep && R[ECX] == 0) return;
        switch (op) {
        case 0xA4: case 0xA5: wrmem(c, R[EDI], sz, rdmem(c, R[ESI] + src_seg, sz)); R[ESI] += step; R[EDI] += step; break;
        case 0xA6: case 0xA7: alu(c, 7, rdmem(c, R[ESI] + src_seg, sz), rdmem(c, R[EDI], sz), sz); R[ESI] += step; R[EDI] += step; break;
        case 0xAA: case 0xAB: wrmem(c, R[EDI], sz, R[EAX]); R[EDI] += step; break;
        case 0xAC: case 0xAD: setreg(c, EAX, sz, rdmem(c, R[ESI] + src_seg, sz)); R[ESI] += step; break;
        case 0xAE: case 0xAF: alu(c, 7, getreg(c, EAX, sz), rdmem(c, R[EDI], sz), sz); R[EDI] += step; break;
        }
        if (!rep) return;
        R[ECX]--;
        if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
            if (rep == 0xF3 && !c->s.zf) return;   // repe: stop when not equal
            if (rep == 0xF2 && c->s.zf) return;    // repne: stop when equal
        }
    }
}

// fast path for rep movsd / rep stosd with DF=0 (very common in MSVC code)
static int __attribute__((unused)) fast_rep(X86 *c, uint8_t op, int osz) {
    if (c->s.df || osz != 4) return 0;
    uint32_t n = R[ECX];
    if (!n) return 1;
    if (n > 0x3FFFFFFFu) return 0;
    uint32_t bytes = n * 4;
    if (op == 0xA5) {
        uint8_t *d = x86_ptr(c, R[EDI], bytes), *s = x86_ptr(c, R[ESI], bytes);
        // x86 semantics copy forward dword by dword; overlapping forward copies replicate
        if (d > s && d < s + bytes) return 0;
        memmove(d, s, bytes);
        R[ESI] += bytes; R[EDI] += bytes; R[ECX] = 0;
        return 1;
    }
    if (op == 0xAB) {
        uint8_t *d = x86_ptr(c, R[EDI], bytes);
        uint32_t v = R[EAX];
        if (v == 0) memset(d, 0, bytes);
        else for (uint32_t i = 0; i < n; i++) memcpy(d + 4 * i, &v, 4);
        R[EDI] += bytes; R[ECX] = 0;
        return 1;
    }
    return 0;
}


#ifdef X86_PROFILE
// Mac dev only: opcode histogram (0..255 primary, 256+ = 0F xx, 512+ = x87 op*16 + (reg form ? 8 : 0) + reg)
uint64_t x86_prof_op[1024], x86_prof_gen[1024];
uint32_t *x86_prof_eip;     // per byte of the first image (16 MB window)
static void x86_profile_insn(X86 *c, uint32_t eip, int generic) {
    const uint8_t *p = c->mem + eip;
    while (*p == 0x66 || *p == 0xF2 || *p == 0xF3 || *p == 0x64 || *p == 0x26 || *p == 0x2E || *p == 0x36 || *p == 0x3E || *p == 0x65 || *p == 0xF0) p++;
    unsigned k = p[0];
    if (k == 0x0F) k = 256 + p[1];
    else if (k >= 0xD8 && k <= 0xDF) k = 512 + (k - 0xD8) * 16 + ((p[1] >> 6) == 3 ? 8 : 0) + ((p[1] >> 3) & 7);
    x86_prof_op[k]++;
    if (generic) x86_prof_gen[k]++;
    if (!x86_prof_eip) x86_prof_eip = calloc(0x1000000, 4);
    uint32_t off = eip - 0x01000000u;
    if (off < 0x1000000u) x86_prof_eip[off]++;
}
#endif

// ---------------------------------------------------------------- generic path: one instruction at EIP
// The complete interpreter: every prefix, operand size and opcode this file supports. The fast path
// below hands anything it has not specialised to this function.
static void exec_generic(X86 *c) {
    if ((uint32_t)(EIP - X86_LOW_LIMIT) > c->mem_size - X86_LOW_LIMIT - 16) x86_fault(c, "EIP out of range %08x", EIP);
    c->cur_insn = EIP;
    int osz = 4, rep = 0;
    uint32_t seg = 0;
    uint8_t op;
    for (;;) {
        op = fetch8(c);
        if (op == 0x66) osz = 2;
        else if (op == 0xF3 || op == 0xF2) rep = op;
        else if (op == 0x64) seg = c->s.fs_base;
        else if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0x65 || op == 0xF0) { /* flat / lock */ }
        else if (op == 0x67) x86_fault(c, "16-bit addressing not supported");
        else break;
    }
    MRM m;
    switch (op) {
    // ---- ALU group 00-3F
    case 0x00: case 0x01: case 0x08: case 0x09: case 0x10: case 0x11: case 0x18: case 0x19:
    case 0x20: case 0x21: case 0x28: case 0x29: case 0x30: case 0x31: case 0x38: case 0x39: {
        int sz = (op & 1) ? osz : 1, aop = op >> 3;
        modrm(c, &m, seg);
        uint32_t r = alu(c, aop, rdE(c, &m, sz), getreg(c, m.reg, sz), sz);
        if (aop != 7) wrE(c, &m, sz, r);
        break; }
    case 0x02: case 0x03: case 0x0A: case 0x0B: case 0x12: case 0x13: case 0x1A: case 0x1B:
    case 0x22: case 0x23: case 0x2A: case 0x2B: case 0x32: case 0x33: case 0x3A: case 0x3B: {
        int sz = (op & 1) ? osz : 1, aop = op >> 3;
        modrm(c, &m, seg);
        uint32_t r = alu(c, aop, getreg(c, m.reg, sz), rdE(c, &m, sz), sz);
        if (aop != 7) setreg(c, m.reg, sz, r);
        break; }
    case 0x04: case 0x05: case 0x0C: case 0x0D: case 0x14: case 0x15: case 0x1C: case 0x1D:
    case 0x24: case 0x25: case 0x2C: case 0x2D: case 0x34: case 0x35: case 0x3C: case 0x3D: {
        int sz = (op & 1) ? osz : 1, aop = op >> 3;
        uint32_t imm = fetchimm(c, sz);
        uint32_t r = alu(c, aop, getreg(c, EAX, sz), imm, sz);
        if (aop != 7) setreg(c, EAX, sz, r);
        break; }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        setreg(c, op & 7, osz, inc_dec(c, getreg(c, op & 7, osz), osz, 0)); break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        setreg(c, op & 7, osz, inc_dec(c, getreg(c, op & 7, osz), osz, 1)); break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        if (osz == 2) { R[ESP] -= 2; wr16(c, R[ESP], (uint16_t)R[op & 7]); }
        else { uint32_t v = R[op & 7]; x86_push(c, v); }
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        if (osz == 2) { uint16_t v = rd16(c, R[ESP]); R[ESP] += 2; setreg(c, op & 7, 2, v); }
        else { uint32_t v = x86_pop(c); R[op & 7] = v; }
        break;
    case 0x60: { uint32_t sp = R[ESP];
        x86_push(c, R[EAX]); x86_push(c, R[ECX]); x86_push(c, R[EDX]); x86_push(c, R[EBX]);
        x86_push(c, sp); x86_push(c, R[EBP]); x86_push(c, R[ESI]); x86_push(c, R[EDI]); break; }
    case 0x61:
        R[EDI] = x86_pop(c); R[ESI] = x86_pop(c); R[EBP] = x86_pop(c); x86_pop(c);
        R[EBX] = x86_pop(c); R[EDX] = x86_pop(c); R[ECX] = x86_pop(c); R[EAX] = x86_pop(c); break;
    case 0x68: if (osz == 2) { uint16_t v = fetch16(c); R[ESP] -= 2; wr16(c, R[ESP], v); } else x86_push(c, fetch32(c)); break;
    case 0x6A: x86_push(c, (uint32_t)(int32_t)(int8_t)fetch8(c)); break;
    case 0x69: case 0x6B: {
        modrm(c, &m, seg);
        int32_t a = (int32_t)sext(rdE(c, &m, osz), osz);
        int32_t b = op == 0x69 ? (int32_t)sext(fetchimm(c, osz), osz) : (int32_t)(int8_t)fetch8(c);
        int64_t p = (int64_t)a * b;
        uint32_t r = (uint32_t)p & MASK(osz);
        c->s.cf = c->s.of = (int64_t)(int32_t)sext(r, osz) != p;
        szp(c, r, osz);
        setreg(c, m.reg, osz, r);
        break; }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d = (int8_t)fetch8(c);
        if (cond(c, op & 15)) EIP += (uint32_t)(int32_t)d;
        break; }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        int sz = (op == 0x80 || op == 0x82) ? 1 : osz;
        modrm(c, &m, seg);
        uint32_t imm = op == 0x83 ? sext(fetch8(c), 1) : fetchimm(c, sz);
        uint32_t r = alu(c, m.reg, rdE(c, &m, sz), imm, sz);
        if (m.reg != 7) wrE(c, &m, sz, r);
        break; }
    case 0x84: case 0x85: { int sz = (op & 1) ? osz : 1; modrm(c, &m, seg); alu(c, 4, rdE(c, &m, sz), getreg(c, m.reg, sz), sz); break; }
    case 0x86: case 0x87: {
        int sz = (op & 1) ? osz : 1;
        modrm(c, &m, seg);
        uint32_t a = rdE(c, &m, sz), b = getreg(c, m.reg, sz);
        wrE(c, &m, sz, b); setreg(c, m.reg, sz, a);
        break; }
    case 0x88: case 0x89: { int sz = (op & 1) ? osz : 1; modrm(c, &m, seg); wrE(c, &m, sz, getreg(c, m.reg, sz)); break; }
    case 0x8A: case 0x8B: { int sz = (op & 1) ? osz : 1; modrm(c, &m, seg); setreg(c, m.reg, sz, rdE(c, &m, sz)); break; }
    case 0x8C: modrm(c, &m, seg); wrE(c, &m, 2, m.reg == 4 ? 0x3B : 0x23); break; // mov r/m, sreg (fake selectors)
    case 0x8D: modrm(c, &m, 0); if (m.mod == 3) x86_fault(c, "lea with register operand"); setreg(c, m.reg, osz, m.addr); break;
    case 0x8E: modrm(c, &m, seg); break; // mov sreg, r/m: ignored (flat)
    case 0x8F: { uint32_t v = x86_pop(c); modrm(c, &m, seg); wrE(c, &m, osz, v); break; } // (esp-relative addressing uses the popped esp)
    case 0x90: break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        uint32_t a = getreg(c, EAX, osz), b = getreg(c, op & 7, osz);
        setreg(c, EAX, osz, b); setreg(c, op & 7, osz, a); break; }
    case 0x98: if (osz == 2) setreg(c, EAX, 2, (uint32_t)(int32_t)(int8_t)R[EAX]); else R[EAX] = (uint32_t)(int32_t)(int16_t)R[EAX]; break;
    case 0x99: if (osz == 2) setreg(c, EDX, 2, (R[EAX] & 0x8000) ? 0xFFFF : 0); else R[EDX] = (R[EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0; break;
    case 0x9B: break; // fwait
    case 0x9C: x86_push(c, get_eflags(c)); break;
    case 0x9D: set_eflags(c, x86_pop(c)); break;
    case 0x9E: { uint32_t ah = (R[EAX] >> 8) & 0xFF; c->s.cf = ah & 1; c->s.pf = (ah >> 2) & 1; c->s.af = (ah >> 4) & 1; c->s.zf = (ah >> 6) & 1; c->s.sf = (ah >> 7) & 1; break; }
    case 0x9F: setreg(c, 4, 1, get_eflags(c) & 0xFF); break; // AH = flags
    case 0xA0: case 0xA1: { int sz = (op & 1) ? osz : 1; uint32_t a = fetch32(c) + seg; setreg(c, EAX, sz, rdmem(c, a, sz)); break; }
    case 0xA2: case 0xA3: { int sz = (op & 1) ? osz : 1; uint32_t a = fetch32(c) + seg; wrmem(c, a, sz, getreg(c, EAX, sz)); break; }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
#ifndef X86_LOCKSTEP
        if (rep && seg == 0 && (op == 0xA5 || op == 0xAB) && fast_rep(c, op, osz)) break;
#endif
        string_op(c, op, osz, rep, seg);
        break;
    case 0xA8: case 0xA9: { int sz = (op & 1) ? osz : 1; alu(c, 4, getreg(c, EAX, sz), fetchimm(c, sz), sz); break; }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        setreg(c, op & 7, 1, fetch8(c)); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        setreg(c, op & 7, osz, fetchimm(c, osz)); break;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int sz = (op & 1) ? osz : 1;
        modrm(c, &m, seg);
        unsigned cnt = (op <= 0xC1) ? fetch8(c) : (op <= 0xD1) ? 1 : (R[ECX] & 0xFF);
        uint32_t v = rdE(c, &m, sz);
        uint32_t r = shift_op(c, m.reg, v, cnt, sz);
        if (cnt & 0x1F) wrE(c, &m, sz, r);
        break; }
    case 0xC2: { uint16_t n = fetch16(c); EIP = x86_pop(c); R[ESP] += n; break; }
    case 0xC3: EIP = x86_pop(c); break;
    case 0xC6: case 0xC7: { int sz = (op & 1) ? osz : 1; modrm(c, &m, seg); wrE(c, &m, sz, fetchimm(c, sz)); break; }
    case 0xC8: { // enter
        uint16_t size = fetch16(c); uint8_t level = fetch8(c) & 31;
        x86_push(c, R[EBP]);
        uint32_t frame = R[ESP];
        if (level) {
            for (int i = 1; i < level; i++) { R[EBP] -= 4; x86_push(c, rd32(c, R[EBP])); }
            x86_push(c, frame);
        }
        R[EBP] = frame; R[ESP] -= size;
        break; }
    case 0xC9: R[ESP] = R[EBP]; R[EBP] = x86_pop(c); break;
    case 0xCC: x86_fault(c, "int3 executed");
    case 0xCD: x86_fault(c, "int %02x executed", fetch8(c));
    case 0xD7: setreg(c, EAX, 1, rd8(c, R[EBX] + (R[EAX] & 0xFF) + seg)); break; // xlat
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        fpu_exec(c, op, seg); break;
    case 0xE0: case 0xE1: case 0xE2: { // loopne / loope / loop
        int8_t d = (int8_t)fetch8(c);
        R[ECX]--;
        int t = R[ECX] != 0;
        if (op == 0xE0) t = t && !c->s.zf; else if (op == 0xE1) t = t && c->s.zf;
        if (t) EIP += (uint32_t)(int32_t)d;
        break; }
    case 0xE3: { int8_t d = (int8_t)fetch8(c); if (R[ECX] == 0) EIP += (uint32_t)(int32_t)d; break; }
    case 0xE8: { int32_t d = (int32_t)fetch32(c); x86_push(c, EIP); EIP += (uint32_t)d; break; }
    case 0xE9: { int32_t d = (int32_t)fetch32(c); EIP += (uint32_t)d; break; }
    case 0xEB: { int8_t d = (int8_t)fetch8(c); EIP += (uint32_t)(int32_t)d; break; }
    case 0xF1: { // host call trap: F1 lo hi
        uint32_t idx = fetch16(c);
        EIP = c->cur_insn;   // host function decides: x86_ret() to return, or leave EIP to retry later
        c->host_call(c, idx);
        break; }
    case 0xF5: c->s.cf ^= 1; break;
    case 0xF6: case 0xF7: {
        int sz = (op & 1) ? osz : 1;
        modrm(c, &m, seg);
        uint32_t v = rdE(c, &m, sz), mask = MASK(sz);
        switch (m.reg) {
        case 0: case 1: alu(c, 4, v, fetchimm(c, sz), sz); break;
        case 2: wrE(c, &m, sz, ~v); break;
        case 3: { uint32_t r = alu(c, 5, 0, v, sz); c->s.cf = (v & mask) != 0; wrE(c, &m, sz, r); break; }
        case 4: { // mul
            if (sz == 1) { uint32_t p = (R[EAX] & 0xFF) * v; setreg(c, EAX, 2, p); c->s.cf = c->s.of = (p >> 8) != 0; szp(c, p, 1); }
            else if (sz == 2) { uint32_t p = (R[EAX] & 0xFFFF) * v; setreg(c, EAX, 2, p); setreg(c, EDX, 2, p >> 16); c->s.cf = c->s.of = (p >> 16) != 0; szp(c, p, 2); }
            else { uint64_t p = (uint64_t)R[EAX] * v; R[EAX] = (uint32_t)p; R[EDX] = (uint32_t)(p >> 32); c->s.cf = c->s.of = R[EDX] != 0; szp(c, R[EAX], 4); }
            break; }
        case 5: { // imul
            if (sz == 1) { int32_t p = (int8_t)R[EAX] * (int8_t)v; setreg(c, EAX, 2, (uint32_t)p); c->s.cf = c->s.of = p != (int8_t)p; szp(c, (uint32_t)p, 1); }
            else if (sz == 2) { int32_t p = (int16_t)R[EAX] * (int16_t)v; setreg(c, EAX, 2, (uint32_t)p); setreg(c, EDX, 2, (uint32_t)p >> 16); c->s.cf = c->s.of = p != (int16_t)p; szp(c, (uint32_t)p, 2); }
            else { int64_t p = (int64_t)(int32_t)R[EAX] * (int32_t)v; R[EAX] = (uint32_t)p; R[EDX] = (uint32_t)((uint64_t)p >> 32); c->s.cf = c->s.of = p != (int32_t)p; szp(c, R[EAX], 4); }
            break; }
        case 6: { // div
            if (v == 0) x86_fault(c, "divide by zero");
            if (sz == 1) { uint32_t n = R[EAX] & 0xFFFF, q = n / v; if (q > 0xFF) x86_fault(c, "div overflow"); setreg(c, EAX, 1, q); setreg(c, 4, 1, n % v); }
            else if (sz == 2) { uint32_t n = ((R[EDX] & 0xFFFF) << 16) | (R[EAX] & 0xFFFF), q = n / v; if (q > 0xFFFF) x86_fault(c, "div overflow"); setreg(c, EAX, 2, q); setreg(c, EDX, 2, n % v); }
            else { uint64_t n = ((uint64_t)R[EDX] << 32) | R[EAX], q = n / v; if (q > 0xFFFFFFFFull) x86_fault(c, "div overflow"); R[EAX] = (uint32_t)q; R[EDX] = (uint32_t)(n % v); }
            break; }
        default: { // idiv
            if (v == 0) x86_fault(c, "divide by zero");
            if (sz == 1) { int32_t n = (int16_t)R[EAX], d = (int8_t)v, q = n / d; if (q != (int8_t)q) x86_fault(c, "idiv overflow"); setreg(c, EAX, 1, (uint32_t)q); setreg(c, 4, 1, (uint32_t)(n % d)); }
            else if (sz == 2) { int32_t n = (int32_t)(((R[EDX] & 0xFFFF) << 16) | (R[EAX] & 0xFFFF)), d = (int16_t)v, q = n / d; if (q != (int16_t)q) x86_fault(c, "idiv overflow"); setreg(c, EAX, 2, (uint32_t)q); setreg(c, EDX, 2, (uint32_t)(n % d)); }
            else { int64_t n = (int64_t)(((uint64_t)R[EDX] << 32) | R[EAX]), d = (int32_t)v;
                   if (d == -1 && n == INT64_MIN) x86_fault(c, "idiv overflow");
                   int64_t q = n / d; if (q != (int32_t)q) x86_fault(c, "idiv overflow"); R[EAX] = (uint32_t)q; R[EDX] = (uint32_t)(n % d); }
            break; }
        }
        break; }
    case 0xF8: c->s.cf = 0; break;
    case 0xF9: c->s.cf = 1; break;
    case 0xFA: case 0xFB: break; // cli/sti
    case 0xFC: c->s.df = 0; break;
    case 0xFD: c->s.df = 1; break;
    case 0xFE: {
        modrm(c, &m, seg);
        if (m.reg > 1) x86_fault(c, "bad FE /%d", m.reg);
        wrE(c, &m, 1, inc_dec(c, rdE(c, &m, 1), 1, m.reg));
        break; }
    case 0xFF: {
        modrm(c, &m, seg);
        switch (m.reg) {
        case 0: case 1: wrE(c, &m, osz, inc_dec(c, rdE(c, &m, osz), osz, m.reg)); break;
        case 2: { uint32_t t = rdE(c, &m, 4); x86_push(c, EIP); EIP = t; break; }
        case 4: EIP = rdE(c, &m, 4); break;
        case 6: { uint32_t v = rdE(c, &m, osz); if (osz == 2) { R[ESP] -= 2; wr16(c, R[ESP], (uint16_t)v); } else x86_push(c, v); break; }
        default: x86_fault(c, "unsupported FF /%d (far call/jmp)", m.reg);
        }
        break; }
    case 0x0F: {
        uint8_t op2 = fetch8(c);
        switch (op2) {
        case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
            int32_t d = (int32_t)fetch32(c);
            if (cond(c, op2 & 15)) EIP += (uint32_t)d;
            break; }
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
            modrm(c, &m, seg); wrE(c, &m, 1, cond(c, op2 & 15)); break;
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            modrm(c, &m, seg);
            uint32_t v = rdE(c, &m, osz);
            if (cond(c, op2 & 15)) setreg(c, m.reg, osz, v);
            break; }
        case 0xB6: modrm(c, &m, seg); setreg(c, m.reg, osz, rdE(c, &m, 1)); break;
        case 0xB7: modrm(c, &m, seg); setreg(c, m.reg, osz, rdE(c, &m, 2)); break;
        case 0xBE: modrm(c, &m, seg); setreg(c, m.reg, osz, sext(rdE(c, &m, 1), 1)); break;
        case 0xBF: modrm(c, &m, seg); setreg(c, m.reg, osz, sext(rdE(c, &m, 2), 2)); break;
        case 0xAF: {
            modrm(c, &m, seg);
            int64_t p = (int64_t)(int32_t)sext(getreg(c, m.reg, osz), osz) * (int32_t)sext(rdE(c, &m, osz), osz);
            uint32_t r = (uint32_t)p & MASK(osz);
            c->s.cf = c->s.of = (int64_t)(int32_t)sext(r, osz) != p;
            szp(c, r, osz);
            setreg(c, m.reg, osz, r);
            break; }
        case 0xA4: case 0xA5: case 0xAC: case 0xAD: { // shld / shrd
            modrm(c, &m, seg);
            unsigned cnt = ((op2 & 1) ? R[ECX] : fetch8(c)) & 31;
            if (!cnt) break;
            uint32_t d = rdE(c, &m, osz), s = getreg(c, m.reg, osz), r;
            unsigned bits = osz * 8;
            if (osz == 2 && cnt > 16) x86_fault(c, "shld/shrd 16-bit count > 16");
            if (op2 <= 0xA5) {
                uint64_t w = ((uint64_t)d << bits) | s;
                c->s.cf = (d >> (bits - cnt)) & 1;
                r = (uint32_t)((w << cnt) >> bits) & MASK(osz);
            } else {
                uint64_t w = ((uint64_t)s << bits) | d;
                c->s.cf = (d >> (cnt - 1)) & 1;
                r = (uint32_t)(w >> cnt) & MASK(osz);
            }
            c->s.of = ((r ^ d) & SIGN(osz)) != 0;
            szp(c, r, osz);
            wrE(c, &m, osz, r);
            break; }
        case 0xA3: case 0xAB: case 0xB3: case 0xBB: { // bt/bts/btr/btc r/m, r
            modrm(c, &m, seg);
            int32_t bit = (int32_t)getreg(c, m.reg, osz);
            if (osz == 2) bit = (int16_t)bit;
            uint32_t v; int b;
            if (m.mod == 3) { b = bit & (osz * 8 - 1); v = getreg(c, m.rm, osz); }
            else { m.addr += (uint32_t)((bit >> 5) * 4); b = bit & 31; v = rd32(c, m.addr); }
            c->s.cf = (v >> b) & 1;
            if (op2 == 0xA3) break;
            if (op2 == 0xAB) v |= 1u << b; else if (op2 == 0xB3) v &= ~(1u << b); else v ^= 1u << b;
            if (m.mod == 3) setreg(c, m.rm, osz, v); else wr32(c, m.addr, v);
            break; }
        case 0xBA: {
            modrm(c, &m, seg);
            unsigned b = fetch8(c) & (osz * 8 - 1);
            uint32_t v = rdE(c, &m, osz);
            c->s.cf = (v >> b) & 1;
            if (m.reg == 4) break;
            if (m.reg == 5) v |= 1u << b; else if (m.reg == 6) v &= ~(1u << b); else if (m.reg == 7) v ^= 1u << b;
            else x86_fault(c, "bad 0F BA /%d", m.reg);
            wrE(c, &m, osz, v);
            break; }
        case 0xBC: case 0xBD: {
            modrm(c, &m, seg);
            uint32_t v = rdE(c, &m, osz);
            if (!v) { c->s.zf = 1; break; }
            c->s.zf = 0;
            setreg(c, m.reg, osz, op2 == 0xBC ? (uint32_t)__builtin_ctz(v) : (uint32_t)(31 - __builtin_clz(v)));
            break; }
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
            R[op2 & 7] = __builtin_bswap32(R[op2 & 7]); break;
        case 0xC0: case 0xC1: { // xadd
            int sz = (op2 & 1) ? osz : 1;
            modrm(c, &m, seg);
            uint32_t d = rdE(c, &m, sz), s = getreg(c, m.reg, sz);
            uint32_t r = alu(c, 0, d, s, sz);
            setreg(c, m.reg, sz, d); wrE(c, &m, sz, r);
            break; }
        case 0xB0: case 0xB1: { // cmpxchg
            int sz = (op2 & 1) ? osz : 1;
            modrm(c, &m, seg);
            uint32_t d = rdE(c, &m, sz);
            alu(c, 7, getreg(c, EAX, sz), d, sz);
            if (c->s.zf) wrE(c, &m, sz, getreg(c, m.reg, sz)); else setreg(c, EAX, sz, d);
            break; }
        case 0xA2: // cpuid: pretend to be a plain Pentium (no MMX/SSE so engines pick scalar paths)
            if (R[EAX] == 0) { R[EAX] = 1; R[EBX] = 0x756e6547; R[EDX] = 0x49656e69; R[ECX] = 0x6c65746e; }
            else { R[EAX] = 0x0543; R[EBX] = 0; R[ECX] = 0; R[EDX] = 0x00000011; } // FPU + TSC
            break;
        case 0x31: { uint64_t t = c->icount; R[EAX] = (uint32_t)t; R[EDX] = (uint32_t)(t >> 32); break; }
        case 0x1F: modrm(c, &m, seg); break; // nop r/m
        default:
            x86_fault(c, "unsupported opcode 0F %02x", op2);
        }
        break; }
    default:
        x86_fault(c, "unsupported opcode %02x", op);
    }
}

// ---------------------------------------------------------------- fast path: predecoded instructions
// The engine's .text is never written (Windows maps it read-only), so each instruction there is
// decoded once into an X86DInsn and executed from that on every later visit: no prefix scan, no
// ModRM/SIB parsing, no immediate fetches, 32-bit forms specialised, and an address computed without
// branches as R[base] + (R[index] << scale) + disp, where register 8 is a constant zero.
// Anything not specialised (prefixes, 8/16-bit forms, rare opcodes, host-call traps) is decoded as
// K_GENERIC and runs through exec_generic, so semantics are defined in exactly one place per form:
// every fast handler below mirrors the generic code for the same bytes (and the Unicorn lockstep test
// checks both).
struct X86DInsn {
    uint8_t kind, len, r, src;      // r: destination / primary register; src: second register
    uint8_t base, index, scale, op; // op: ALU op, condition code, x87 sub-op or ST(i) index
    uint32_t disp;
    uint32_t imm;                   // immediate, branch target or RET pop count
};

enum {
    K_NONE = 0, K_GENERIC,
    K_NOP, K_MOV_RR, K_MOV_RM, K_MOV_MR, K_MOV_RI, K_MOV_MI, K_LEA, K_MOV8_RM, K_MOV8_MR,
    K_ALU_RR, K_ALU_RM, K_ALU_MR, K_ALU_RI, K_ALU_MI, K_TEST_RR, K_TEST_MR, K_TEST_RI, K_TEST_MI,
    K_INC_R, K_DEC_R, K_PUSH_R, K_POP_R, K_PUSH_I, K_PUSH_M,
    K_JCC, K_JMP, K_CALL, K_RET, K_CALL_R, K_CALL_M, K_JMP_R, K_JMP_M, K_LEAVE,
    K_MOVZX8_R, K_MOVZX8_M, K_MOVZX16_R, K_MOVZX16_M, K_MOVSX8_R, K_MOVSX8_M, K_MOVSX16_R, K_MOVSX16_M,
    K_CDQ, K_SAHF, K_SHL_RI, K_SHR_RI, K_SAR_RI, K_IMUL_RR, K_IMUL_RM, K_IMUL_RRI, K_IMUL_RMI,
    K_FLD32, K_FST32, K_FLD64, K_FST64, K_FILD32, K_FARITH32, K_FARITH64, K_FCOM32, K_FCOM64,
    K_FLDST, K_FXCH, K_FST0_OP, K_FSTI_OP, K_FSTST, K_FCHS, K_FABS, K_FLD1, K_FLDZ, K_FNSTSW_AX,
    // per-operation specialisations (kind = base + op); decode() maps the generic kinds above onto them
    K_ALU_RR_0, K_ALU_RR_1, K_ALU_RR_2, K_ALU_RR_3, K_ALU_RR_4, K_ALU_RR_5, K_ALU_RR_6, K_ALU_RR_7,
    K_ALU_RM_0, K_ALU_RM_1, K_ALU_RM_2, K_ALU_RM_3, K_ALU_RM_4, K_ALU_RM_5, K_ALU_RM_6, K_ALU_RM_7,
    K_ALU_MR_0, K_ALU_MR_1, K_ALU_MR_2, K_ALU_MR_3, K_ALU_MR_4, K_ALU_MR_5, K_ALU_MR_6, K_ALU_MR_7,
    K_ALU_RI_0, K_ALU_RI_1, K_ALU_RI_2, K_ALU_RI_3, K_ALU_RI_4, K_ALU_RI_5, K_ALU_RI_6, K_ALU_RI_7,
    K_ALU_MI_0, K_ALU_MI_1, K_ALU_MI_2, K_ALU_MI_3, K_ALU_MI_4, K_ALU_MI_5, K_ALU_MI_6, K_ALU_MI_7,
    K_FARITH32_0, K_FARITH32_1, K_FARITH32_2, K_FARITH32_3, K_FARITH32_4, K_FARITH32_5, K_FARITH32_6, K_FARITH32_7,
    K_FARITH64_0, K_FARITH64_1, K_FARITH64_2, K_FARITH64_3, K_FARITH64_4, K_FARITH64_5, K_FARITH64_6, K_FARITH64_7,
    K_FST0_OP_0, K_FST0_OP_1, K_FST0_OP_2, K_FST0_OP_3, K_FST0_OP_4, K_FST0_OP_5, K_FST0_OP_6, K_FST0_OP_7,
    K_COUNT
};

#define ZR 8   // the constant-zero register slot

int x86_set_code_cache(X86 *c, uint32_t base, uint32_t size) {
    x86_free_code_cache(c);
    if (!size || base < X86_LOW_LIMIT || (uint64_t)base + size > c->mem_size) return -1;
    void *p = mmap(NULL, (size_t)size * sizeof(X86DInsn), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return -1;
    c->dcache = p;
    c->dc_base = base;
    c->dc_size = size;
    return 0;
}

void x86_free_code_cache(X86 *c) {
    if (c->dcache) munmap(c->dcache, (size_t)c->dc_size * sizeof(X86DInsn));
    c->dcache = NULL;
    c->dc_base = c->dc_size = 0;
}

static inline uint32_t rd32u(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// Decodes the ModRM (+SIB, displacement) at p into d. Returns bytes consumed, or 0 for a register
// operand (mod == 3; d->src = rm).
static int decode_modrm(const uint8_t *p, X86DInsn *d, int *mod, int *reg) {
    uint8_t b = p[0];
    int md = b >> 6, rg = (b >> 3) & 7, rm = b & 7, n = 1;
    *mod = md; *reg = rg;
    if (md == 3) { d->src = (uint8_t)rm; return 1; }
    d->base = ZR; d->index = ZR; d->scale = 0; d->disp = 0;
    if (rm == 4) {
        uint8_t sib = p[1];
        int sc = sib >> 6, idx = (sib >> 3) & 7, bs = sib & 7;
        n = 2;
        if (bs == 5 && md == 0) { d->disp = rd32u(p + 2); n += 4; }
        else d->base = (uint8_t)bs;
        if (idx != 4) { d->index = (uint8_t)idx; d->scale = (uint8_t)sc; }
    } else if (rm == 5 && md == 0) {
        d->disp = rd32u(p + 1);
        n += 4;
    } else {
        d->base = (uint8_t)rm;
    }
    if (md == 1) { d->disp += (uint32_t)(int32_t)(int8_t)p[n]; n += 1; }
    else if (md == 2) { d->disp += rd32u(p + n); n += 4; }
    return n;
}

static void decode_basic(X86 *c, X86DInsn *d, uint32_t eip) {
    const uint8_t *p = c->mem + eip;
    int mod, reg, n;
    uint8_t op = p[0];
    d->kind = K_GENERIC;
    d->len = 0;
    // no prefixes, no 16-bit or byte-sized ALU forms: those stay generic
    switch (op) {
    case 0x90: d->kind = K_NOP; d->len = 1; return;
    case 0x8B: case 0x89: case 0x8D: case 0x8A: case 0x88: case 0x85:
    case 0x01: case 0x09: case 0x21: case 0x29: case 0x31: case 0x39:
    case 0x03: case 0x0B: case 0x23: case 0x2B: case 0x33: case 0x3B: {
        n = decode_modrm(p + 1, d, &mod, &reg);
        d->len = (uint8_t)(1 + n);
        int m3 = mod == 3;
        switch (op) {
        case 0x8B: d->r = (uint8_t)reg; d->kind = m3 ? K_MOV_RR : K_MOV_RM; return;
        case 0x89: if (m3) { d->r = d->src; d->src = (uint8_t)reg; d->kind = K_MOV_RR; } else { d->r = (uint8_t)reg; d->kind = K_MOV_MR; } return;
        case 0x8D: if (m3) { d->kind = K_GENERIC; return; } d->r = (uint8_t)reg; d->kind = K_LEA; return;
        case 0x8A: if (m3) { d->kind = K_GENERIC; return; } d->r = (uint8_t)reg; d->kind = K_MOV8_RM; return;
        case 0x88: if (m3) { d->kind = K_GENERIC; return; } d->r = (uint8_t)reg; d->kind = K_MOV8_MR; return;
        case 0x85: if (m3) { d->r = d->src; d->src = (uint8_t)reg; d->kind = K_TEST_RR; } else { d->r = (uint8_t)reg; d->kind = K_TEST_MR; } return;
        default:
            d->op = (uint8_t)(op >> 3);
            if (op & 2) {   // r32 op= r/m32
                d->r = (uint8_t)reg;
                d->kind = m3 ? K_ALU_RR : K_ALU_RM;
            } else {        // r/m32 op= r32
                if (m3) { d->r = d->src; d->src = (uint8_t)reg; d->kind = K_ALU_RR; }
                else { d->r = (uint8_t)reg; d->kind = K_ALU_MR; }
            }
            return;
        }
    }
    case 0x05: case 0x0D: case 0x25: case 0x2D: case 0x35: case 0x3D:
        d->op = (uint8_t)(op >> 3); d->r = EAX; d->imm = rd32u(p + 1); d->len = 5; d->kind = K_ALU_RI; return;
    case 0xA9: d->r = EAX; d->imm = rd32u(p + 1); d->len = 5; d->kind = K_TEST_RI; return;
    case 0x81: case 0x83: {
        n = decode_modrm(p + 1, d, &mod, &reg);
        if (reg == 2 || reg == 3) return;   // adc / sbb
        d->op = (uint8_t)reg;
        if (op == 0x83) { d->imm = (uint32_t)(int32_t)(int8_t)p[1 + n]; d->len = (uint8_t)(2 + n); }
        else { d->imm = rd32u(p + 1 + n); d->len = (uint8_t)(5 + n); }
        if (mod == 3) { d->r = d->src; d->kind = K_ALU_RI; } else d->kind = K_ALU_MI;
        return; }
    case 0xF7: {
        n = decode_modrm(p + 1, d, &mod, &reg);
        if (reg > 1) return;                // not/neg/mul/imul/div/idiv: generic
        d->imm = rd32u(p + 1 + n); d->len = (uint8_t)(5 + n);
        if (mod == 3) { d->r = d->src; d->kind = K_TEST_RI; } else d->kind = K_TEST_MI;
        return; }
    case 0xC7: {
        n = decode_modrm(p + 1, d, &mod, &reg);
        d->imm = rd32u(p + 1 + n); d->len = (uint8_t)(5 + n);
        if (mod == 3) { d->r = d->src; d->kind = K_MOV_RI; } else d->kind = K_MOV_MI;
        return; }
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        d->r = op & 7; d->imm = rd32u(p + 1); d->len = 5; d->kind = K_MOV_RI; return;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        d->r = op & 7; d->len = 1; d->kind = K_INC_R; return;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        d->r = op & 7; d->len = 1; d->kind = K_DEC_R; return;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        d->r = op & 7; d->len = 1; d->kind = K_PUSH_R; return;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        d->r = op & 7; d->len = 1; d->kind = K_POP_R; return;
    case 0x68: d->imm = rd32u(p + 1); d->len = 5; d->kind = K_PUSH_I; return;
    case 0x6A: d->imm = (uint32_t)(int32_t)(int8_t)p[1]; d->len = 2; d->kind = K_PUSH_I; return;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        d->op = op & 15; d->len = 2; d->imm = eip + 2 + (uint32_t)(int32_t)(int8_t)p[1]; d->kind = K_JCC; return;
    case 0xEB: d->len = 2; d->imm = eip + 2 + (uint32_t)(int32_t)(int8_t)p[1]; d->kind = K_JMP; return;
    case 0xE9: d->len = 5; d->imm = eip + 5 + rd32u(p + 1); d->kind = K_JMP; return;
    case 0xE8: d->len = 5; d->imm = eip + 5 + rd32u(p + 1); d->kind = K_CALL; return;
    case 0xC3: d->len = 1; d->imm = 0; d->kind = K_RET; return;
    case 0xC2: d->len = 3; d->imm = (uint32_t)p[1] | ((uint32_t)p[2] << 8); d->kind = K_RET; return;
    case 0xC9: d->len = 1; d->kind = K_LEAVE; return;
    case 0x99: d->len = 1; d->kind = K_CDQ; return;
    case 0x9E: d->len = 1; d->kind = K_SAHF; return;
    case 0xFF: {
        n = decode_modrm(p + 1, d, &mod, &reg);
        d->len = (uint8_t)(1 + n);
        if (reg == 2) d->kind = mod == 3 ? K_CALL_R : K_CALL_M;
        else if (reg == 4) d->kind = mod == 3 ? K_JMP_R : K_JMP_M;
        else if (reg == 6 && mod != 3) d->kind = K_PUSH_M;
        else d->kind = K_GENERIC;
        return; }
    case 0xC1: case 0xD1: {
        n = decode_modrm(p + 1, d, &mod, &reg);
        if (mod != 3 || (reg != 4 && reg != 5 && reg != 7)) return;
        d->r = d->src;
        if (op == 0xC1) { d->imm = p[1 + n] & 0x1F; d->len = (uint8_t)(2 + n); } else { d->imm = 1; d->len = (uint8_t)(1 + n); }
        d->kind = reg == 4 ? K_SHL_RI : reg == 5 ? K_SHR_RI : K_SAR_RI;
        return; }
    case 0x69: case 0x6B: {
        n = decode_modrm(p + 1, d, &mod, &reg);
        d->r = (uint8_t)reg;
        if (op == 0x6B) { d->imm = (uint32_t)(int32_t)(int8_t)p[1 + n]; d->len = (uint8_t)(2 + n); }
        else { d->imm = rd32u(p + 1 + n); d->len = (uint8_t)(5 + n); }
        d->kind = mod == 3 ? K_IMUL_RRI : K_IMUL_RMI;
        return; }
    case 0x0F: {
        uint8_t op2 = p[1];
        if (op2 >= 0x80 && op2 <= 0x8F) { d->op = op2 & 15; d->len = 6; d->imm = eip + 6 + rd32u(p + 2); d->kind = K_JCC; return; }
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF || op2 == 0xAF) {
            n = decode_modrm(p + 2, d, &mod, &reg);
            d->r = (uint8_t)reg;
            d->len = (uint8_t)(2 + n);
            int m3 = mod == 3;
            switch (op2) {
            case 0xB6: d->kind = m3 ? K_MOVZX8_R : K_MOVZX8_M; break;
            case 0xB7: d->kind = m3 ? K_MOVZX16_R : K_MOVZX16_M; break;
            case 0xBE: d->kind = m3 ? K_MOVSX8_R : K_MOVSX8_M; break;
            case 0xBF: d->kind = m3 ? K_MOVSX16_R : K_MOVSX16_M; break;
            default: d->kind = m3 ? K_IMUL_RR : K_IMUL_RM; break;
            }
        }
        return; }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        n = decode_modrm(p + 1, d, &mod, &reg);
        d->len = (uint8_t)(1 + n);
        d->op = (uint8_t)reg;
        if (mod != 3) {
            switch (op) {
            case 0xD8: d->kind = (reg == 2 || reg == 3) ? K_FCOM32 : K_FARITH32; return;
            case 0xDC: d->kind = (reg == 2 || reg == 3) ? K_FCOM64 : K_FARITH64; return;
            case 0xD9: if (reg == 0) d->kind = K_FLD32; else if (reg == 2 || reg == 3) d->kind = K_FST32; return;
            case 0xDD: if (reg == 0) d->kind = K_FLD64; else if (reg == 2 || reg == 3) d->kind = K_FST64; return;
            case 0xDB: if (reg == 0) d->kind = K_FILD32; return;
            default: return;
            }
        }
        int i = d->src;
        d->src = (uint8_t)i;
        switch (op) {
        case 0xD8: if (reg != 2 && reg != 3) d->kind = K_FST0_OP; return;
        case 0xDC: if (reg != 2 && reg != 3) { d->kind = K_FSTI_OP; d->r = 0; } return;
        case 0xDE: if (reg != 2 && reg != 3) { d->kind = K_FSTI_OP; d->r = 1; } return;   // r = pop afterwards
        case 0xD9:
            if (reg == 0) d->kind = K_FLDST;
            else if (reg == 1) d->kind = K_FXCH;
            else if (reg == 4 && i == 0) d->kind = K_FCHS;
            else if (reg == 4 && i == 1) d->kind = K_FABS;
            else if (reg == 5 && i == 0) d->kind = K_FLD1;
            else if (reg == 5 && i == 6) d->kind = K_FLDZ;
            return;
        case 0xDD: if (reg == 2 || reg == 3) d->kind = K_FSTST; return;
        case 0xDF: if (reg == 4 && i == 0) d->kind = K_FNSTSW_AX; return;
        default: return;
        }
    }
    default:
        return;
    }
}

static void decode(X86 *c, X86DInsn *d, uint32_t eip) {
    decode_basic(c, d, eip);
    switch (d->kind) {
    case K_ALU_RR: d->kind = (uint8_t)(K_ALU_RR_0 + d->op); break;
    case K_ALU_RM: d->kind = (uint8_t)(K_ALU_RM_0 + d->op); break;
    case K_ALU_MR: d->kind = (uint8_t)(K_ALU_MR_0 + d->op); break;
    case K_ALU_RI: d->kind = (uint8_t)(K_ALU_RI_0 + d->op); break;
    case K_ALU_MI: d->kind = (uint8_t)(K_ALU_MI_0 + d->op); break;
    case K_FARITH32: d->kind = (uint8_t)(K_FARITH32_0 + d->op); break;
    case K_FARITH64: d->kind = (uint8_t)(K_FARITH64_0 + d->op); break;
    case K_FST0_OP: d->kind = (uint8_t)(K_FST0_OP_0 + d->op); break;
    default: break;
    }
}

static inline void szp32(X86 *c, uint32_t r) { c->s.zf = r == 0; c->s.sf = r >> 31; c->s.pf = parity_tab[r & 0xFF]; }

// alu() for 32-bit operands and the ops the fast path decodes (never ADC / SBB)
static inline uint32_t alu32(X86 *c, int op, uint32_t a, uint32_t b) {
    uint32_t r;
    switch (op) {
    case 0: r = a + b; c->s.cf = r < a; c->s.of = (((a ^ r) & (b ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; break;
    case 1: r = a | b; c->s.cf = c->s.of = c->s.af = 0; break;
    case 4: r = a & b; c->s.cf = c->s.of = c->s.af = 0; break;
    case 6: r = a ^ b; c->s.cf = c->s.of = c->s.af = 0; break;
    default: r = a - b; c->s.cf = a < b; c->s.of = (((a ^ b) & (a ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; break;
    }
    szp32(c, r);
    return r;
}

#define EA(d) (R[(d)->base] + (R[(d)->index] << (d)->scale) + (d)->disp)

// ---------------------------------------------------------------- main loop
// The fast path keeps EIP, the instruction count, the x87 stack top and the memory bounds in locals
// (registers): guest stores go through byte pointers, which would otherwise force every one of them
// to be reloaded from *c after each store. They are written back before anything else looks at them:
// the generic path, a return, or a lockstep hook. (After a fault the CPU state is not used again.)
int x86_run(X86 *c, uint64_t max_insns) {
    jmp_buf jb;
    jmp_buf *prev = c->fault_jmp;
    c->fault_jmp = &jb;
    c->stop = 0;
    if (setjmp(jb)) {
        c->fault_jmp = prev;
        return -1;
    }
    const uint64_t end = max_insns > UINT64_MAX - c->icount ? UINT64_MAX : c->icount + max_insns;
    X86DInsn *const dc = c->no_fast ? NULL : c->dcache;
    const uint32_t dcb = c->dc_base, dcs = dc ? c->dc_size : 0;
    uint8_t *const mem = c->mem;
    const uint32_t lim = c->mem_size - X86_LOW_LIMIT;
    uint32_t *const reg = c->s.r;
    double *const fst = c->s.fpu.st;
    uint8_t *const fempty = c->s.fpu.empty;
    uint32_t eip = EIP;
    uint64_t icount = c->icount;
    int top = c->s.fpu.top;
    int rc = 0;
#define SYNC_OUT() do { EIP = eip; c->icount = icount; c->s.fpu.top = top; } while (0)
#define SYNC_IN() do { eip = EIP; icount = c->icount; top = c->s.fpu.top; } while (0)
#define FP(a, n) ({ uint32_t a_ = (a); if ((uint32_t)(a_ - X86_LOW_LIMIT) > lim - (n)) { SYNC_OUT(); x86_fault(c, "bad memory access %08x (+%u)", a_, (unsigned)(n)); } mem + a_; })
#define LD32(a) ({ uint32_t v_; memcpy(&v_, FP((a), 4), 4); v_; })
#define LD16(a) ({ uint16_t v_; memcpy(&v_, FP((a), 2), 2); v_; })
#define LD8(a) (*FP((a), 1))
#define LDF32(a) ({ float v_; memcpy(&v_, FP((a), 4), 4); (double)v_; })
#define LDF64(a) ({ double v_; memcpy(&v_, FP((a), 8), 8); v_; })
#ifdef X86_LOCKSTEP
#define ST32(a, v) do { uint32_t a2_ = (a), v2_ = (v); X86_LOGW(c, a2_, 4); memcpy(FP(a2_, 4), &v2_, 4); } while (0)
#define ST8(a, v) do { uint32_t a2_ = (a); uint8_t v2_ = (uint8_t)(v); X86_LOGW(c, a2_, 1); *FP(a2_, 1) = v2_; } while (0)
#define ST64(a, v) do { uint32_t a2_ = (a); uint64_t v2_ = (v); X86_LOGW(c, a2_, 8); memcpy(FP(a2_, 8), &v2_, 8); } while (0)
#else
#define ST32(a, v) do { uint32_t v2_ = (v); memcpy(FP((a), 4), &v2_, 4); } while (0)
#define ST8(a, v) do { *FP((a), 1) = (uint8_t)(v); } while (0)
#define ST64(a, v) do { uint64_t v2_ = (v); memcpy(FP((a), 8), &v2_, 8); } while (0)
#endif
#define PUSH(v) do { uint32_t pv_ = (v); reg[ESP] -= 4; ST32(reg[ESP], pv_); } while (0)
#define POP() ({ uint32_t pv_ = LD32(reg[ESP]); reg[ESP] += 4; pv_; })
#define FST(i) fst[(top + (i)) & 7]
#define FPUSH(v) do { double fv_ = (v); top = (top - 1) & 7; fst[top] = fv_; fempty[top] = 0; } while (0)
#define FPOP() do { fempty[top] = 1; top = (top + 1) & 7; } while (0)
#define XEA(d) (reg[(d)->base] + (reg[(d)->index] << (d)->scale) + (d)->disp)

    const X86DInsn *d;
    uint32_t next, off;
    static const void *const jt[K_COUNT] = {
        [K_NONE] = &&L_K_NONE,
        [K_GENERIC] = &&L_K_GENERIC,
        [K_NOP] = &&L_K_NOP,
        [K_MOV_RR] = &&L_K_MOV_RR,
        [K_MOV_RM] = &&L_K_MOV_RM,
        [K_MOV_MR] = &&L_K_MOV_MR,
        [K_MOV_RI] = &&L_K_MOV_RI,
        [K_MOV_MI] = &&L_K_MOV_MI,
        [K_LEA] = &&L_K_LEA,
        [K_MOV8_RM] = &&L_K_MOV8_RM,
        [K_MOV8_MR] = &&L_K_MOV8_MR,
        [K_ALU_RR] = &&L_K_GENERIC,
        [K_ALU_RM] = &&L_K_GENERIC,
        [K_ALU_MR] = &&L_K_GENERIC,
        [K_ALU_RI] = &&L_K_GENERIC,
        [K_ALU_MI] = &&L_K_GENERIC,
        [K_TEST_RR] = &&L_K_TEST_RR,
        [K_TEST_MR] = &&L_K_TEST_MR,
        [K_TEST_RI] = &&L_K_TEST_RI,
        [K_TEST_MI] = &&L_K_TEST_MI,
        [K_INC_R] = &&L_K_INC_R,
        [K_DEC_R] = &&L_K_DEC_R,
        [K_PUSH_R] = &&L_K_PUSH_R,
        [K_POP_R] = &&L_K_POP_R,
        [K_PUSH_I] = &&L_K_PUSH_I,
        [K_PUSH_M] = &&L_K_PUSH_M,
        [K_JCC] = &&L_K_JCC,
        [K_JMP] = &&L_K_JMP,
        [K_CALL] = &&L_K_CALL,
        [K_RET] = &&L_K_RET,
        [K_CALL_R] = &&L_K_CALL_R,
        [K_CALL_M] = &&L_K_CALL_M,
        [K_JMP_R] = &&L_K_JMP_R,
        [K_JMP_M] = &&L_K_JMP_M,
        [K_LEAVE] = &&L_K_LEAVE,
        [K_MOVZX8_R] = &&L_K_MOVZX8_R,
        [K_MOVZX8_M] = &&L_K_MOVZX8_M,
        [K_MOVZX16_R] = &&L_K_MOVZX16_R,
        [K_MOVZX16_M] = &&L_K_MOVZX16_M,
        [K_MOVSX8_R] = &&L_K_MOVSX8_R,
        [K_MOVSX8_M] = &&L_K_MOVSX8_M,
        [K_MOVSX16_R] = &&L_K_MOVSX16_R,
        [K_MOVSX16_M] = &&L_K_MOVSX16_M,
        [K_CDQ] = &&L_K_CDQ,
        [K_SAHF] = &&L_K_SAHF,
        [K_SHL_RI] = &&L_K_SHL_RI,
        [K_SHR_RI] = &&L_K_SHR_RI,
        [K_SAR_RI] = &&L_K_SAR_RI,
        [K_IMUL_RR] = &&L_K_IMUL_RR,
        [K_IMUL_RM] = &&L_K_IMUL_RM,
        [K_IMUL_RRI] = &&L_K_IMUL_RRI,
        [K_IMUL_RMI] = &&L_K_IMUL_RMI,
        [K_FLD32] = &&L_K_FLD32,
        [K_FST32] = &&L_K_FST32,
        [K_FLD64] = &&L_K_FLD64,
        [K_FST64] = &&L_K_FST64,
        [K_FILD32] = &&L_K_FILD32,
        [K_FARITH32] = &&L_K_GENERIC,
        [K_FARITH64] = &&L_K_GENERIC,
        [K_FCOM32] = &&L_K_FCOM32,
        [K_FCOM64] = &&L_K_FCOM64,
        [K_FLDST] = &&L_K_FLDST,
        [K_FXCH] = &&L_K_FXCH,
        [K_FST0_OP] = &&L_K_GENERIC,
        [K_FSTI_OP] = &&L_K_FSTI_OP,
        [K_FSTST] = &&L_K_FSTST,
        [K_FCHS] = &&L_K_FCHS,
        [K_FABS] = &&L_K_FABS,
        [K_FLD1] = &&L_K_FLD1,
        [K_FLDZ] = &&L_K_FLDZ,
        [K_FNSTSW_AX] = &&L_K_FNSTSW_AX,
        [K_ALU_RR_0] = &&L_K_ALU_RR_0,
        [K_ALU_RR_1] = &&L_K_ALU_RR_1,
        [K_ALU_RR_2] = &&L_K_GENERIC,
        [K_ALU_RR_3] = &&L_K_GENERIC,
        [K_ALU_RR_4] = &&L_K_ALU_RR_4,
        [K_ALU_RR_5] = &&L_K_ALU_RR_5,
        [K_ALU_RR_6] = &&L_K_ALU_RR_6,
        [K_ALU_RR_7] = &&L_K_ALU_RR_7,
        [K_ALU_RM_0] = &&L_K_ALU_RM_0,
        [K_ALU_RM_1] = &&L_K_ALU_RM_1,
        [K_ALU_RM_2] = &&L_K_GENERIC,
        [K_ALU_RM_3] = &&L_K_GENERIC,
        [K_ALU_RM_4] = &&L_K_ALU_RM_4,
        [K_ALU_RM_5] = &&L_K_ALU_RM_5,
        [K_ALU_RM_6] = &&L_K_ALU_RM_6,
        [K_ALU_RM_7] = &&L_K_ALU_RM_7,
        [K_ALU_MR_0] = &&L_K_ALU_MR_0,
        [K_ALU_MR_1] = &&L_K_ALU_MR_1,
        [K_ALU_MR_2] = &&L_K_GENERIC,
        [K_ALU_MR_3] = &&L_K_GENERIC,
        [K_ALU_MR_4] = &&L_K_ALU_MR_4,
        [K_ALU_MR_5] = &&L_K_ALU_MR_5,
        [K_ALU_MR_6] = &&L_K_ALU_MR_6,
        [K_ALU_MR_7] = &&L_K_ALU_MR_7,
        [K_ALU_RI_0] = &&L_K_ALU_RI_0,
        [K_ALU_RI_1] = &&L_K_ALU_RI_1,
        [K_ALU_RI_2] = &&L_K_GENERIC,
        [K_ALU_RI_3] = &&L_K_GENERIC,
        [K_ALU_RI_4] = &&L_K_ALU_RI_4,
        [K_ALU_RI_5] = &&L_K_ALU_RI_5,
        [K_ALU_RI_6] = &&L_K_ALU_RI_6,
        [K_ALU_RI_7] = &&L_K_ALU_RI_7,
        [K_ALU_MI_0] = &&L_K_ALU_MI_0,
        [K_ALU_MI_1] = &&L_K_ALU_MI_1,
        [K_ALU_MI_2] = &&L_K_GENERIC,
        [K_ALU_MI_3] = &&L_K_GENERIC,
        [K_ALU_MI_4] = &&L_K_ALU_MI_4,
        [K_ALU_MI_5] = &&L_K_ALU_MI_5,
        [K_ALU_MI_6] = &&L_K_ALU_MI_6,
        [K_ALU_MI_7] = &&L_K_ALU_MI_7,
        [K_FARITH32_0] = &&L_K_FARITH32_0,
        [K_FARITH32_1] = &&L_K_FARITH32_1,
        [K_FARITH32_2] = &&L_K_GENERIC,
        [K_FARITH32_3] = &&L_K_GENERIC,
        [K_FARITH32_4] = &&L_K_FARITH32_4,
        [K_FARITH32_5] = &&L_K_FARITH32_5,
        [K_FARITH32_6] = &&L_K_FARITH32_6,
        [K_FARITH32_7] = &&L_K_FARITH32_7,
        [K_FARITH64_0] = &&L_K_FARITH64_0,
        [K_FARITH64_1] = &&L_K_FARITH64_1,
        [K_FARITH64_2] = &&L_K_GENERIC,
        [K_FARITH64_3] = &&L_K_GENERIC,
        [K_FARITH64_4] = &&L_K_FARITH64_4,
        [K_FARITH64_5] = &&L_K_FARITH64_5,
        [K_FARITH64_6] = &&L_K_FARITH64_6,
        [K_FARITH64_7] = &&L_K_FARITH64_7,
        [K_FST0_OP_0] = &&L_K_FST0_OP_0,
        [K_FST0_OP_1] = &&L_K_FST0_OP_1,
        [K_FST0_OP_2] = &&L_K_GENERIC,
        [K_FST0_OP_3] = &&L_K_GENERIC,
        [K_FST0_OP_4] = &&L_K_FST0_OP_4,
        [K_FST0_OP_5] = &&L_K_FST0_OP_5,
        [K_FST0_OP_6] = &&L_K_FST0_OP_6,
        [K_FST0_OP_7] = &&L_K_FST0_OP_7,
    };

#if defined(X86_LOCKSTEP) || defined(X86_PROFILE) || !defined(CV_NO_DEBUG_ENV)
#define DEBUG_PRE() do { if (d->kind > K_GENERIC) { SYNC_OUT(); c->cur_insn = eip; DEBUG_PRE2(); } } while (0)
#else
#define DEBUG_PRE() ((void)0)
#endif
#ifndef CV_NO_DEBUG_ENV
#define DEBUG_HIST() do { c->hist[c->hist_pos++ & 63] = eip; \
        if (c->trace && icount >= c->trace_from && icount < c->trace_to) fprintf(stderr, "@%08x\n", eip); } while (0)
#else
#define DEBUG_HIST() ((void)0)
#endif
#ifdef X86_PROFILE
#define DEBUG_PROF() x86_profile_insn(c, eip, 0)
#else
#define DEBUG_PROF() ((void)0)
#endif
#ifdef X86_LOCKSTEP
#define DEBUG_PRE2() do { DEBUG_HIST(); DEBUG_PROF(); if (c->step_pre) c->step_pre(c); } while (0)
#define LOCKSTEP_POST() do { SYNC_OUT(); if (c->step_post) c->step_post(c); } while (0)
#else
#define DEBUG_PRE2() do { DEBUG_HIST(); DEBUG_PROF(); } while (0)
#define LOCKSTEP_POST() ((void)0)
#endif
    // Threaded dispatch: every handler ends with its own copy of this, so each has its own indirect
    // jump for the branch predictor to learn.
#define DISPATCH() do { \
        if (__builtin_expect(icount >= end, 0)) { rc = 1; goto L_EXIT; } \
        off = eip - dcb; \
        if (__builtin_expect(off >= dcs, 0)) goto L_K_GENERIC; \
        d = &dc[off]; \
        DEBUG_PRE(); \
        next = eip + d->len; \
        goto *jt[d->kind]; } while (0)
// Straight-line handlers skip the instruction-budget test; control transfers (NEXT_BR) do it, so a
// guest loop still reaches the watchdog within one basic block.
#define DISPATCH_NOCHECK() do { \
        off = eip - dcb; \
        if (__builtin_expect(off >= dcs, 0)) goto L_K_GENERIC; \
        d = &dc[off]; \
        DEBUG_PRE(); \
        next = eip + d->len; \
        goto *jt[d->kind]; } while (0)
#define NEXT() do { icount++; eip = next; LOCKSTEP_POST(); DISPATCH_NOCHECK(); } while (0)
#define NEXT_BR() do { icount++; eip = next; LOCKSTEP_POST(); DISPATCH(); } while (0)

    DISPATCH();

L_K_NONE:
    decode(c, (X86DInsn *)d, eip);
    DEBUG_PRE();
    next = eip + d->len;
    goto *jt[d->kind];

L_K_GENERIC:
    // the generic path (also host-call traps): it works on *c
    SYNC_OUT();
#ifndef CV_NO_DEBUG_ENV
    c->hist[c->hist_pos++ & 63] = eip;
    if (c->trace && icount >= c->trace_from && icount < c->trace_to) fprintf(stderr, "@%08x\n", eip);
#endif
#ifdef X86_LOCKSTEP
    c->cur_insn = eip;
    if (c->step_pre) c->step_pre(c);
#endif
#ifdef X86_PROFILE
    x86_profile_insn(c, eip, 1);
#endif
    c->icount++;
    exec_generic(c);
#ifdef X86_LOCKSTEP
    if (c->step_post) c->step_post(c);
#endif
    SYNC_IN();
    if (c->stop) goto L_EXIT;
    DISPATCH();

    {
            L_K_NOP: NEXT();
            L_K_MOV_RR: reg[d->r] = reg[d->src]; NEXT();
            L_K_MOV_RM: reg[d->r] = LD32(XEA(d)); NEXT();
            L_K_MOV_MR: ST32(XEA(d), reg[d->r]); NEXT();
            L_K_MOV_RI: reg[d->r] = d->imm; NEXT();
            L_K_MOV_MI: ST32(XEA(d), d->imm); NEXT();
            L_K_LEA: reg[d->r] = XEA(d); NEXT();
            L_K_MOV8_RM: setreg(c, d->r, 1, LD8(XEA(d))); NEXT();
            L_K_MOV8_MR: ST8(XEA(d), getreg(c, d->r, 1)); NEXT();
            L_K_TEST_RR: alu32(c, 4, reg[d->r], reg[d->src]); NEXT();
            L_K_TEST_MR: alu32(c, 4, LD32(XEA(d)), reg[d->r]); NEXT();
            L_K_TEST_RI: alu32(c, 4, reg[d->r], d->imm); NEXT();
            L_K_TEST_MI: alu32(c, 4, LD32(XEA(d)), d->imm); NEXT();
            L_K_INC_R: { uint32_t r = reg[d->r] + 1; c->s.of = r == 0x80000000u; c->s.af = (r & 0xF) == 0; szp32(c, r); reg[d->r] = r; NEXT(); }
            L_K_DEC_R: { uint32_t a = reg[d->r], r = a - 1; c->s.of = a == 0x80000000u; c->s.af = (a & 0xF) == 0; szp32(c, r); reg[d->r] = r; NEXT(); }
            L_K_PUSH_R: { uint32_t v = reg[d->r]; PUSH(v); NEXT(); }
            L_K_POP_R: { uint32_t v = POP(); reg[d->r] = v; NEXT(); }
            L_K_PUSH_I: PUSH(d->imm); NEXT();
            L_K_PUSH_M: { uint32_t v = LD32(XEA(d)); PUSH(v); NEXT(); }
            L_K_JCC: if (cond(c, d->op)) next = d->imm; NEXT_BR();
            L_K_JMP: next = d->imm; NEXT_BR();
            L_K_CALL: PUSH(next); next = d->imm; NEXT_BR();
            L_K_RET: next = POP(); reg[ESP] += d->imm; NEXT_BR();
            L_K_CALL_R: { uint32_t t = reg[d->src]; PUSH(next); next = t; NEXT_BR(); }
            L_K_CALL_M: { uint32_t t = LD32(XEA(d)); PUSH(next); next = t; NEXT_BR(); }
            L_K_JMP_R: next = reg[d->src]; NEXT_BR();
            L_K_JMP_M: next = LD32(XEA(d)); NEXT_BR();
            L_K_LEAVE: reg[ESP] = reg[EBP]; reg[EBP] = POP(); NEXT();
            L_K_MOVZX8_R: reg[d->r] = getreg(c, d->src, 1); NEXT();
            L_K_MOVZX8_M: reg[d->r] = LD8(XEA(d)); NEXT();
            L_K_MOVZX16_R: reg[d->r] = reg[d->src] & 0xFFFF; NEXT();
            L_K_MOVZX16_M: reg[d->r] = LD16(XEA(d)); NEXT();
            L_K_MOVSX8_R: reg[d->r] = (uint32_t)(int32_t)(int8_t)getreg(c, d->src, 1); NEXT();
            L_K_MOVSX8_M: reg[d->r] = (uint32_t)(int32_t)(int8_t)LD8(XEA(d)); NEXT();
            L_K_MOVSX16_R: reg[d->r] = (uint32_t)(int32_t)(int16_t)reg[d->src]; NEXT();
            L_K_MOVSX16_M: reg[d->r] = (uint32_t)(int32_t)(int16_t)LD16(XEA(d)); NEXT();
            L_K_CDQ: reg[EDX] = (reg[EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0; NEXT();
            L_K_SAHF: { uint32_t ah = (reg[EAX] >> 8) & 0xFF; c->s.cf = ah & 1; c->s.pf = (ah >> 2) & 1; c->s.af = (ah >> 4) & 1; c->s.zf = (ah >> 6) & 1; c->s.sf = (ah >> 7) & 1; NEXT(); }
            L_K_SHL_RI: {
                unsigned n = d->imm;
                if (!n) NEXT();
                uint32_t a = reg[d->r], r = a << n;
                c->s.cf = (a >> (32 - n)) & 1; c->s.of = (r >> 31) ^ c->s.cf; c->s.af = 0; szp32(c, r); reg[d->r] = r;
                NEXT(); }
            L_K_SHR_RI: {
                unsigned n = d->imm;
                if (!n) NEXT();
                uint32_t a = reg[d->r], r = a >> n;
                c->s.cf = (a >> (n - 1)) & 1; c->s.of = a >> 31; c->s.af = 0; szp32(c, r); reg[d->r] = r;
                NEXT(); }
            L_K_SAR_RI: {
                unsigned n = d->imm;
                if (!n) NEXT();
                int32_t a = (int32_t)reg[d->r];
                uint32_t r = (uint32_t)(a >> n);
                c->s.cf = (a >> (n - 1)) & 1; c->s.of = 0; c->s.af = 0; szp32(c, r); reg[d->r] = r;
                NEXT(); }
            L_K_IMUL_RR: L_K_IMUL_RM: L_K_IMUL_RRI: L_K_IMUL_RMI: {
                int32_t a, b;
                if (d->kind == K_IMUL_RR) { a = (int32_t)reg[d->r]; b = (int32_t)reg[d->src]; }
                else if (d->kind == K_IMUL_RM) { a = (int32_t)reg[d->r]; b = (int32_t)LD32(XEA(d)); }
                else if (d->kind == K_IMUL_RRI) { a = (int32_t)reg[d->src]; b = (int32_t)d->imm; }
                else { a = (int32_t)LD32(XEA(d)); b = (int32_t)d->imm; }
                int64_t p = (int64_t)a * b;
                uint32_t r = (uint32_t)p;
                c->s.cf = c->s.of = (int64_t)(int32_t)r != p;
                szp32(c, r);
                reg[d->r] = r;
                NEXT(); }
            // ---- x87 (mirrors fpu_exec)
            L_K_FLD32: { double v = LDF32(XEA(d)); FPUSH(v); NEXT(); }
            L_K_FST32: { float f = (float)FST(0); uint32_t b; memcpy(&b, &f, 4); ST32(XEA(d), b); if (d->op == 3) FPOP(); NEXT(); }
            L_K_FLD64: { double v = LDF64(XEA(d)); FPUSH(v); NEXT(); }
            L_K_FST64: { double v = FST(0); uint64_t b; memcpy(&b, &v, 8); ST64(XEA(d), b); if (d->op == 3) FPOP(); NEXT(); }
            L_K_FILD32: { double v = (double)(int32_t)LD32(XEA(d)); FPUSH(v); NEXT(); }
            L_K_FCOM32: { double v = LDF32(XEA(d)); fcom_set(c, FST(0), v); if (d->op == 3) FPOP(); NEXT(); }
            L_K_FCOM64: { double v = LDF64(XEA(d)); fcom_set(c, FST(0), v); if (d->op == 3) FPOP(); NEXT(); }
            L_K_FLDST: { double v = FST(d->src); FPUSH(v); NEXT(); }
            L_K_FXCH: { double t = FST(0); FST(0) = FST(d->src); FST(d->src) = t; NEXT(); }
            L_K_FSTI_OP: {
                int o = d->op;
                if (o == 4) o = 5; else if (o == 5) o = 4; else if (o == 6) o = 7; else if (o == 7) o = 6;
                FST(d->src) = farith(o, FST(d->src), FST(0));
                if (d->r) FPOP();
                NEXT(); }
            L_K_FSTST: { double v = FST(0); FST(d->src) = v; if (d->op == 3) FPOP(); NEXT(); }
            L_K_FCHS: FST(0) = -FST(0); NEXT();
            L_K_FABS: FST(0) = fabs(FST(0)); NEXT();
            L_K_FLD1: FPUSH(1.0); NEXT();
            L_K_FLDZ: FPUSH(0.0); NEXT();
            L_K_FNSTSW_AX: reg[EAX] = (reg[EAX] & 0xFFFF0000u) | (uint16_t)((c->s.fpu.sw & ~0x3800) | ((top & 7) << 11)); NEXT();

            // ---- specialised ALU forms (alu() for 32 bits, one operation each)
            L_K_ALU_RR_0: { uint32_t a = reg[d->r], b = reg[d->src]; uint32_t r = a + b; c->s.cf = r < a; c->s.of = (((a ^ r) & (b ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RR_1: { uint32_t a = reg[d->r], b = reg[d->src]; uint32_t r = a | b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RR_4: { uint32_t a = reg[d->r], b = reg[d->src]; uint32_t r = a & b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RR_5: { uint32_t a = reg[d->r], b = reg[d->src]; uint32_t r = a - b; c->s.cf = a < b; c->s.of = (((a ^ b) & (a ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RR_6: { uint32_t a = reg[d->r], b = reg[d->src]; uint32_t r = a ^ b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RR_7: { uint32_t a = reg[d->r], b = reg[d->src]; uint32_t r = a - b; c->s.cf = a < b; c->s.of = (((a ^ b) & (a ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); (void)r; } NEXT();
            L_K_ALU_RM_0: { uint32_t a = reg[d->r], b = LD32(XEA(d)); uint32_t r = a + b; c->s.cf = r < a; c->s.of = (((a ^ r) & (b ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RM_1: { uint32_t a = reg[d->r], b = LD32(XEA(d)); uint32_t r = a | b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RM_4: { uint32_t a = reg[d->r], b = LD32(XEA(d)); uint32_t r = a & b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RM_5: { uint32_t a = reg[d->r], b = LD32(XEA(d)); uint32_t r = a - b; c->s.cf = a < b; c->s.of = (((a ^ b) & (a ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RM_6: { uint32_t a = reg[d->r], b = LD32(XEA(d)); uint32_t r = a ^ b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RM_7: { uint32_t a = reg[d->r], b = LD32(XEA(d)); uint32_t r = a - b; c->s.cf = a < b; c->s.of = (((a ^ b) & (a ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); (void)r; } NEXT();
            L_K_ALU_MR_0: { uint32_t ad = XEA(d); uint32_t a = LD32(ad), b = reg[d->r]; uint32_t r = a + b; c->s.cf = r < a; c->s.of = (((a ^ r) & (b ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); ST32(ad, r); } NEXT();
            L_K_ALU_MR_1: { uint32_t ad = XEA(d); uint32_t a = LD32(ad), b = reg[d->r]; uint32_t r = a | b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); ST32(ad, r); } NEXT();
            L_K_ALU_MR_4: { uint32_t ad = XEA(d); uint32_t a = LD32(ad), b = reg[d->r]; uint32_t r = a & b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); ST32(ad, r); } NEXT();
            L_K_ALU_MR_5: { uint32_t ad = XEA(d); uint32_t a = LD32(ad), b = reg[d->r]; uint32_t r = a - b; c->s.cf = a < b; c->s.of = (((a ^ b) & (a ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); ST32(ad, r); } NEXT();
            L_K_ALU_MR_6: { uint32_t ad = XEA(d); uint32_t a = LD32(ad), b = reg[d->r]; uint32_t r = a ^ b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); ST32(ad, r); } NEXT();
            L_K_ALU_MR_7: { uint32_t ad = XEA(d); uint32_t a = LD32(ad), b = reg[d->r]; uint32_t r = a - b; c->s.cf = a < b; c->s.of = (((a ^ b) & (a ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); (void)r; } NEXT();
            L_K_ALU_RI_0: { uint32_t a = reg[d->r], b = d->imm; uint32_t r = a + b; c->s.cf = r < a; c->s.of = (((a ^ r) & (b ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RI_1: { uint32_t a = reg[d->r], b = d->imm; uint32_t r = a | b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RI_4: { uint32_t a = reg[d->r], b = d->imm; uint32_t r = a & b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RI_5: { uint32_t a = reg[d->r], b = d->imm; uint32_t r = a - b; c->s.cf = a < b; c->s.of = (((a ^ b) & (a ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RI_6: { uint32_t a = reg[d->r], b = d->imm; uint32_t r = a ^ b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); reg[d->r] = r; } NEXT();
            L_K_ALU_RI_7: { uint32_t a = reg[d->r], b = d->imm; uint32_t r = a - b; c->s.cf = a < b; c->s.of = (((a ^ b) & (a ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); (void)r; } NEXT();
            L_K_ALU_MI_0: { uint32_t ad = XEA(d); uint32_t a = LD32(ad), b = d->imm; uint32_t r = a + b; c->s.cf = r < a; c->s.of = (((a ^ r) & (b ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); ST32(ad, r); } NEXT();
            L_K_ALU_MI_1: { uint32_t ad = XEA(d); uint32_t a = LD32(ad), b = d->imm; uint32_t r = a | b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); ST32(ad, r); } NEXT();
            L_K_ALU_MI_4: { uint32_t ad = XEA(d); uint32_t a = LD32(ad), b = d->imm; uint32_t r = a & b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); ST32(ad, r); } NEXT();
            L_K_ALU_MI_5: { uint32_t ad = XEA(d); uint32_t a = LD32(ad), b = d->imm; uint32_t r = a - b; c->s.cf = a < b; c->s.of = (((a ^ b) & (a ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); ST32(ad, r); } NEXT();
            L_K_ALU_MI_6: { uint32_t ad = XEA(d); uint32_t a = LD32(ad), b = d->imm; uint32_t r = a ^ b; c->s.cf = c->s.of = c->s.af = 0; szp32(c, r); ST32(ad, r); } NEXT();
            L_K_ALU_MI_7: { uint32_t ad = XEA(d); uint32_t a = LD32(ad), b = d->imm; uint32_t r = a - b; c->s.cf = a < b; c->s.of = (((a ^ b) & (a ^ r)) >> 31); c->s.af = ((a ^ b ^ r) >> 4) & 1; szp32(c, r); (void)r; } NEXT();
            // ---- specialised x87 arithmetic (farith(op, ST(0), src), one operation each)
            L_K_FARITH32_0: { double x = FST(0), y = LDF32(XEA(d)); FST(0) = x + y; } NEXT();
            L_K_FARITH64_0: { double x = FST(0), y = LDF64(XEA(d)); FST(0) = x + y; } NEXT();
            L_K_FST0_OP_0: { double x = FST(0), y = FST(d->src); FST(0) = x + y; } NEXT();
            L_K_FARITH32_1: { double x = FST(0), y = LDF32(XEA(d)); FST(0) = x * y; } NEXT();
            L_K_FARITH64_1: { double x = FST(0), y = LDF64(XEA(d)); FST(0) = x * y; } NEXT();
            L_K_FST0_OP_1: { double x = FST(0), y = FST(d->src); FST(0) = x * y; } NEXT();
            L_K_FARITH32_4: { double x = FST(0), y = LDF32(XEA(d)); FST(0) = x - y; } NEXT();
            L_K_FARITH64_4: { double x = FST(0), y = LDF64(XEA(d)); FST(0) = x - y; } NEXT();
            L_K_FST0_OP_4: { double x = FST(0), y = FST(d->src); FST(0) = x - y; } NEXT();
            L_K_FARITH32_5: { double x = FST(0), y = LDF32(XEA(d)); FST(0) = y - x; } NEXT();
            L_K_FARITH64_5: { double x = FST(0), y = LDF64(XEA(d)); FST(0) = y - x; } NEXT();
            L_K_FST0_OP_5: { double x = FST(0), y = FST(d->src); FST(0) = y - x; } NEXT();
            L_K_FARITH32_6: { double x = FST(0), y = LDF32(XEA(d)); FST(0) = x / y; } NEXT();
            L_K_FARITH64_6: { double x = FST(0), y = LDF64(XEA(d)); FST(0) = x / y; } NEXT();
            L_K_FST0_OP_6: { double x = FST(0), y = FST(d->src); FST(0) = x / y; } NEXT();
            L_K_FARITH32_7: { double x = FST(0), y = LDF32(XEA(d)); FST(0) = y / x; } NEXT();
            L_K_FARITH64_7: { double x = FST(0), y = LDF64(XEA(d)); FST(0) = y / x; } NEXT();
            L_K_FST0_OP_7: { double x = FST(0), y = FST(d->src); FST(0) = y / x; } NEXT();
    }

L_EXIT:
    SYNC_OUT();
    c->fault_jmp = prev;
    if (rc) return 1;
    return c->faulted ? -1 : 0;
#undef SYNC_OUT
#undef SYNC_IN
}
