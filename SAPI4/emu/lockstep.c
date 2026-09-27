// Mac-only differential tester: every guest instruction is executed by our interpreter AND by
// Unicorn (QEMU TCG) from the same state; registers, flags, memory writes and x87 state are
// compared. Unicorn is only a test oracle here - it is never part of the app.
// Build: make build/sapi4_lockstep ; run with SAPI4_LOCKSTEP=1 [SAPI4_LOCKSTEP_FROM=n SAPI4_LOCKSTEP_MAX=n]
#ifdef X86_LOCKSTEP
#include "emu_internal.h"
#include <math.h>
#include <unicorn/unicorn.h>

// Unicorn reads/writes x87 registers as 10 bytes: 64-bit mantissa then 16-bit sign+exponent
typedef struct { uint64_t mantissa; uint16_t exponent; uint8_t pad[6]; } uc_x86_float80;

typedef struct { uint32_t a; uint8_t n; uint8_t old[8]; } WLog;
static WLog g_wlog[4096];
static int g_nw;
static int g_logging;

typedef struct { uint32_t a; int n; uint8_t old[8]; } ULog;
static ULog g_ulog[4096];
static int g_nu;

static uc_engine *g_uc;
static X86State g_pre;
static int g_skip;
static uint64_t g_from, g_max, g_checked, g_mismatches;
static int g_rep_insn;
static uint8_t g_opc[4];

void x86_log_write(X86 *c, uint32_t a, uint32_t n) {
    if (!g_logging || g_nw >= 4096) return;
    if ((uint32_t)(a - X86_LOW_LIMIT) > c->mem_size - X86_LOW_LIMIT - n) return;
    WLog *w = &g_wlog[g_nw++];
    w->a = a; w->n = (uint8_t)n;
    memcpy(w->old, c->mem + a, n);
}

static void uc_write_hook(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user) {
    (void)uc; (void)type; (void)value;
    X86 *c = user;
    if (g_nu >= 4096 || address + (uint64_t)size > c->mem_size) return;
    ULog *u = &g_ulog[g_nu++];
    u->a = (uint32_t)address; u->n = size;
    memcpy(u->old, c->mem + address, (size_t)size);
}

static void set_uc_state(X86 *c, const X86State *s) {
    (void)c;
    static const int regs[8] = { UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                                 UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI };
    for (int i = 0; i < 8; i++) { uint32_t v = s->r[i]; uc_reg_write(g_uc, regs[i], &v); }
    uint32_t eip = s->eip; uc_reg_write(g_uc, UC_X86_REG_EIP, &eip);
    uint32_t fl = 0x202 | s->cf | (s->pf << 2) | (s->af << 4) | (s->zf << 6) | (s->sf << 7) | (s->df << 10) | (s->of << 11);
    uc_reg_write(g_uc, UC_X86_REG_EFLAGS, &fl);
    // x87: physical registers FP0..7, TOP in FPSW
    for (int i = 0; i < 8; i++) {
        uc_x86_float80 f;
        double v = s->fpu.st[i];
        uint64_t mant = 0; uint16_t se = 0;
        if (v == 0) { mant = 0; se = 0; }
        else if (isinf(v)) { mant = 1ull << 63; se = 0x7FFF; }
        else if (isnan(v)) { mant = 0xC000000000000000ull; se = 0x7FFF; }
        else { int e; double fr = frexp(fabs(v), &e); mant = (uint64_t)ldexp(fr, 64); se = (uint16_t)(e - 1 + 16383); }
        if (signbit(v)) se |= 0x8000;
        f.mantissa = mant; f.exponent = se;
        uc_reg_write(g_uc, UC_X86_REG_FP0 + i, &f);
    }
    uint32_t sw = (s->fpu.sw & ~0x3800) | ((uint32_t)(s->fpu.top & 7) << 11);
    uc_reg_write(g_uc, UC_X86_REG_FPSW, &sw);
    uint32_t cw = s->fpu.cw; uc_reg_write(g_uc, UC_X86_REG_FPCW, &cw);
    uint32_t tw = 0;
    for (int i = 0; i < 8; i++) tw |= (s->fpu.empty[i] ? 3u : 0u) << (2 * i);
    uc_reg_write(g_uc, UC_X86_REG_FPTAG, &tw);
}

static double f80_to_d(const uc_x86_float80 *f) {
    int exp = f->exponent & 0x7FFF;
    double v;
    if (exp == 0 && f->mantissa == 0) v = 0;
    else if (exp == 0x7FFF) v = (f->mantissa << 1) ? NAN : INFINITY;
    else v = ldexp((double)f->mantissa, exp - 16383 - 63);
    return (f->exponent & 0x8000) ? -v : v;
}

static void lock_pre(X86 *c) {
    g_skip = 1;
    if (c->icount < g_from || g_checked >= g_max) return;
    // decode prefixes to decide whether we can compare
    uint8_t *p = c->mem + c->s.eip;
    int k = 0, rep = 0;
    for (;;) {
        uint8_t b = p[k];
        if (b == 0x66 || b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0xF0) { k++; continue; }
        if (b == 0xF2 || b == 0xF3) { rep = 1; k++; continue; }
        if (b == 0x64 || b == 0x65) return;         // FS/GS based: skip (Unicorn has no TEB segment set up)
        break;
    }
    if (p[k] == 0xF1) return;                        // host call trap
    if (p[k] == 0x0F && (p[k + 1] == 0x31 || p[k + 1] == 0xA2)) return; // rdtsc / cpuid: host-defined
    memcpy(g_opc, p + k, 4);
    g_rep_insn = rep;
    g_pre = c->s;
    g_nw = 0;
    g_logging = 1;
    g_skip = 0;
}

static void report(X86 *c, const char *what, uint32_t mine, uint32_t theirs) {
    g_mismatches++;
    if (g_mismatches <= 40) {
        // the address as `llvm-objdump -d` of the module shows it (relative to its preferred base)
        Emu *e = (Emu *)c->user;
        const char *mod = "?";
        uint32_t orig = g_pre.eip;
        for (int i = 0; i < e->nmodules; i++)
            if (g_pre.eip >= e->modules[i].base && g_pre.eip < e->modules[i].base + e->modules[i].size) {
                mod = e->modules[i].name;
                orig = g_pre.eip - e->modules[i].base + e->modules[i].pref_base;
            }
        fprintf(stderr, "[lockstep] #%llu at %08x (%s %08x) opc %02x %02x %02x: %s mine=%08x unicorn=%08x\n",
                (unsigned long long)c->icount, g_pre.eip, mod, orig, g_opc[0], g_opc[1], g_opc[2], what, mine, theirs);
    }
}

static void lock_post(X86 *c) {
    if (g_skip) return;
    g_logging = 0;
    g_checked++;
    X86State mine = c->s;
    // my new bytes, then undo my writes (reverse order)
    static uint8_t mynew[4096][8];
    for (int i = 0; i < g_nw; i++) memcpy(mynew[i], c->mem + g_wlog[i].a, g_wlog[i].n);
    for (int i = g_nw - 1; i >= 0; i--) memcpy(c->mem + g_wlog[i].a, g_wlog[i].old, g_wlog[i].n);
    // run unicorn
    set_uc_state(c, &g_pre);
    g_nu = 0;
    uc_err err;
    if (g_rep_insn) err = uc_emu_start(g_uc, g_pre.eip, mine.eip, 0, 0);
    else err = uc_emu_start(g_uc, g_pre.eip, 0xFFFFFFFFull, 0, 1);
    if (err) {
        fprintf(stderr, "[lockstep] unicorn error %s at %08x\n", uc_strerror(err), g_pre.eip);
    }
    // compare registers
    static const int regs[8] = { UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                                 UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI };
    static const char *names[8] = { "EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI" };
    for (int i = 0; i < 8; i++) {
        uint32_t v; uc_reg_read(g_uc, regs[i], &v);
        if (v != mine.r[i]) report(c, names[i], mine.r[i], v);
    }
    uint32_t eip; uc_reg_read(g_uc, UC_X86_REG_EIP, &eip);
    if (eip != mine.eip) report(c, "EIP", mine.eip, eip);
    uint32_t fl; uc_reg_read(g_uc, UC_X86_REG_EFLAGS, &fl);
    // which flags are architecturally defined for this opcode
    uint32_t mask = 0x1 | 0x4 | 0x40 | 0x80 | 0x400 | 0x800; // CF PF ZF SF DF OF (AF ignored)
    uint8_t o = g_opc[0];
    int modreg = (g_opc[1] >> 3) & 7;
    if ((o == 0xF6 || o == 0xF7) && modreg >= 4) mask = (modreg >= 6) ? 0x400 : (0x1 | 0x800 | 0x400); // mul/imul: CF OF; div: none
    if (o == 0x69 || o == 0x6B || (o == 0x0F && g_opc[1] == 0xAF)) mask = 0x1 | 0x800 | 0x400;
    if (o == 0xC0 || o == 0xC1 || o == 0xD2 || o == 0xD3) mask &= ~0x800u;             // OF undefined for count != 1
    if ((o == 0xC0 || o == 0xC1 || o == 0xD0 || o == 0xD1 || o == 0xD2 || o == 0xD3) && modreg < 4) mask &= (0x1 | 0x800 | 0x400); // rotates: CF (OF)
    if (o == 0x0F && (g_opc[1] == 0xBC || g_opc[1] == 0xBD)) mask = 0x40 | 0x400;
    if (o == 0x0F && (g_opc[1] == 0xA3 || g_opc[1] == 0xAB || g_opc[1] == 0xB3 || g_opc[1] == 0xBB || g_opc[1] == 0xBA)) mask = 0x1 | 0x400;
    if (o == 0x0F && (g_opc[1] == 0xA4 || g_opc[1] == 0xA5 || g_opc[1] == 0xAC || g_opc[1] == 0xAD)) mask &= ~0x800u;
    uint32_t myfl = mine.cf | (mine.pf << 2) | (mine.zf << 6) | (mine.sf << 7) | (mine.df << 10) | (mine.of << 11);
    if ((fl & mask) != (myfl & mask)) report(c, "EFLAGS", myfl & mask, fl & mask);
    // x87
    uint32_t sw; uc_reg_read(g_uc, UC_X86_REG_FPSW, &sw);
    int utop = (sw >> 11) & 7;
    if (utop != mine.fpu.top) report(c, "FPU TOP", (uint32_t)mine.fpu.top, (uint32_t)utop);
    uint32_t mysw = mine.fpu.sw & 0x4700, usw = sw & 0x4700; // C0 C1 C2 C3
    if (o >= 0xD8 && o <= 0xDF && mysw != usw) report(c, "FPU C-bits", mysw, usw);
    if (o >= 0xD8 && o <= 0xDF) {
        for (int i = 0; i < 8; i++) {
            if (mine.fpu.empty[i]) continue;
            uc_x86_float80 f; uc_reg_read(g_uc, UC_X86_REG_FP0 + i, &f);
            double u = f80_to_d(&f), m = mine.fpu.st[i];
            double tol = 1e-9 * fmax(fabs(u), fabs(m));
            if (!(fabs(u - m) <= tol) && !(isnan(u) && isnan(m))) {
                uint32_t mb[2], ub[2]; memcpy(mb, &m, 8); memcpy(ub, &u, 8);
                report(c, "FPU reg (hi words)", mb[1], ub[1]);
                fprintf(stderr, "            phys %d: mine %.17g unicorn %.17g\n", i, m, u);
            }
        }
    }
    // memory: compare unicorn's result bytes against mine, at every address either side wrote
    uint8_t their[4096][8];
    for (int i = 0; i < g_nw; i++) memcpy(their[i], c->mem + g_wlog[i].a, g_wlog[i].n);
    for (int i = 0; i < g_nw; i++) {
        if (memcmp(their[i], mynew[i], g_wlog[i].n)) {
            uint32_t a = 0, b = 0;
            memcpy(&a, mynew[i], g_wlog[i].n > 4 ? 4 : g_wlog[i].n);
            memcpy(&b, their[i], g_wlog[i].n > 4 ? 4 : g_wlog[i].n);
            report(c, "memory write value", a, b);
        }
    }
    // unicorn-only writes
    for (int j = 0; j < g_nu; j++) {
        int covered = 0;
        for (int i = 0; i < g_nw; i++) if (g_ulog[j].a >= g_wlog[i].a && g_ulog[j].a + (uint32_t)g_ulog[j].n <= g_wlog[i].a + g_wlog[i].n) covered = 1;
        if (!covered) {
            uint32_t v = 0; memcpy(&v, c->mem + g_ulog[j].a, g_ulog[j].n > 4 ? 4 : (size_t)g_ulog[j].n);
            report(c, "unicorn wrote where we did not (addr)", g_ulog[j].a, v);
            memcpy(c->mem + g_ulog[j].a, g_ulog[j].old, (size_t)g_ulog[j].n);
        }
    }
    // restore my results
    for (int i = 0; i < g_nw; i++) memcpy(c->mem + g_wlog[i].a, mynew[i], g_wlog[i].n);
    if (g_mismatches && g_mismatches <= 40 && getenv("SAPI4_LOCKSTEP_STOP")) x86_fault(c, "lockstep mismatch");
}

void emu_lockstep_enable(Emu *e) {
    const char *s = getenv("SAPI4_LOCKSTEP");
    if (!s || *s != '1') return;
    X86 *c = emu_cpu(e);
    if (uc_open(UC_ARCH_X86, UC_MODE_32, &g_uc)) { fprintf(stderr, "uc_open failed\n"); return; }
    uc_err err = uc_mem_map_ptr(g_uc, 0, c->mem_size, UC_PROT_ALL, c->mem);
    if (err) { fprintf(stderr, "uc_mem_map_ptr: %s\n", uc_strerror(err)); return; }
    uc_hook hh;
    uc_hook_add(g_uc, &hh, UC_HOOK_MEM_WRITE, (void *)uc_write_hook, c, 1, 0);
    const char *f = getenv("SAPI4_LOCKSTEP_FROM"), *m = getenv("SAPI4_LOCKSTEP_MAX");
    g_from = f ? strtoull(f, NULL, 10) : 0;
    g_max = m ? strtoull(m, NULL, 10) : UINT64_MAX;
    c->step_pre = lock_pre;
    c->step_post = lock_post;
    fprintf(stderr, "[lockstep] enabled from insn %llu\n", (unsigned long long)g_from);
}
void emu_lockstep_report(void) {
    if (g_uc) fprintf(stderr, "[lockstep] checked %llu instructions, %llu mismatches\n", (unsigned long long)g_checked, (unsigned long long)g_mismatches);
}
#endif
