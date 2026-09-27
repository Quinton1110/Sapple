// Minimal i386 user-mode interpreter (pure C, no JIT) for running 32-bit Windows engine DLLs.
// Flat 32-bit address space mapped onto one host buffer; FS segment base for the TEB.
// Opcode F1 xx xx is reserved as a "host call" trap (thunks for Win32 / CRT imports).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

#define X86_LOW_LIMIT 0x10000u   // guest addresses below this fault (null-pointer guard)

typedef struct X86Fpu {
    double st[8];
    uint8_t empty[8];   // 1 = tag empty
    int top;
    uint16_t cw;        // control word
    uint16_t sw;        // status word WITHOUT the TOP field (C0..C3, exception bits)
} X86Fpu;

typedef struct X86State {
    uint32_t r[9];          // r[8] is always 0: the "no register" slot of predecoded addresses
    uint32_t eip;
    uint8_t cf, pf, af, zf, sf, of, df, _pad;
    uint32_t fs_base;
    X86Fpu fpu;
} X86State;

typedef struct X86 X86;
typedef void (*X86HostCall)(X86 *c, uint32_t index);
typedef struct X86DInsn X86DInsn;

struct X86 {
    X86State s;
    uint8_t *mem;
    uint32_t mem_size;          // bytes of guest memory (plus 16 bytes slack allocated after)
    X86HostCall host_call;
    void *user;
    int stop;                   // host call sets this to make x86_run return
    uint64_t icount;            // instructions executed
    int faulted;
    char fault_msg[512];
    jmp_buf *fault_jmp;
    uint32_t cur_insn;          // EIP of the instruction being executed
    // debug: ring of recent EIPs
    uint32_t hist[64];
    unsigned hist_pos;
    int trace;                  // >0: print each instruction EIP to stderr (icount in [trace_from, trace_to))
    uint64_t trace_from, trace_to;
    void (*step_pre)(X86 *c);   // lockstep testing hooks (Mac dev builds only)
    void (*step_post)(X86 *c);
    // predecoded-instruction cache over one read-only code section (see x86_set_code_cache)
    X86DInsn *dcache;
    uint32_t dc_base, dc_size;
    int no_fast;                // 1 = never use the fast path (A/B testing)
};

void x86_init(X86 *c, uint8_t *mem, uint32_t mem_size);
// Enables the decoded-instruction cache for [base, base+size): code there must never be written
// (true for a PE .text section, which Windows maps read-only + execute). One cache per X86.
int x86_set_code_cache(X86 *c, uint32_t base, uint32_t size);
void x86_free_code_cache(X86 *c);
// Run until c->stop is set by a host call, a fault occurs (returns -1), or max_insns executed (returns 1).
int x86_run(X86 *c, uint64_t max_insns);
void x86_fault(X86 *c, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void x86_dump(X86 *c);

// guest memory helpers
static inline uint8_t *x86_ptr(X86 *c, uint32_t a, uint32_t n) {
    if ((uint32_t)(a - X86_LOW_LIMIT) > c->mem_size - X86_LOW_LIMIT - n)
        x86_fault(c, "bad memory access %08x (+%u)", a, n);
    return c->mem + a;
}
static inline uint8_t rd8(X86 *c, uint32_t a) { return *x86_ptr(c, a, 1); }
static inline uint16_t rd16(X86 *c, uint32_t a) { uint16_t v; __builtin_memcpy(&v, x86_ptr(c, a, 2), 2); return v; }
static inline uint32_t rd32(X86 *c, uint32_t a) { uint32_t v; __builtin_memcpy(&v, x86_ptr(c, a, 4), 4); return v; }
static inline uint64_t rd64(X86 *c, uint32_t a) { uint64_t v; __builtin_memcpy(&v, x86_ptr(c, a, 8), 8); return v; }
#ifdef X86_LOCKSTEP
void x86_log_write(X86 *c, uint32_t a, uint32_t n);   // records old bytes before a guest write
#define X86_LOGW(c, a, n) x86_log_write((c), (a), (n))
#else
#define X86_LOGW(c, a, n) ((void)0)
#endif
static inline void wr8(X86 *c, uint32_t a, uint8_t v) { X86_LOGW(c, a, 1); *x86_ptr(c, a, 1) = v; }
static inline void wr16(X86 *c, uint32_t a, uint16_t v) { X86_LOGW(c, a, 2); __builtin_memcpy(x86_ptr(c, a, 2), &v, 2); }
static inline void wr32(X86 *c, uint32_t a, uint32_t v) { X86_LOGW(c, a, 4); __builtin_memcpy(x86_ptr(c, a, 4), &v, 4); }
static inline void wr64(X86 *c, uint32_t a, uint64_t v) { X86_LOGW(c, a, 8); __builtin_memcpy(x86_ptr(c, a, 8), &v, 8); }
static inline double rdf64(X86 *c, uint32_t a) { double v; __builtin_memcpy(&v, x86_ptr(c, a, 8), 8); return v; }

static inline void x86_push(X86 *c, uint32_t v) { c->s.r[ESP] -= 4; wr32(c, c->s.r[ESP], v); }
static inline uint32_t x86_pop(X86 *c) { uint32_t v = rd32(c, c->s.r[ESP]); c->s.r[ESP] += 4; return v; }
// stack argument i (0-based) as seen on entry to a stdcall/cdecl function (return address at [esp])
static inline uint32_t x86_arg(X86 *c, int i) { return rd32(c, c->s.r[ESP] + 4 + 4 * (uint32_t)i); }
// emulate "ret n": pop return address, drop n bytes of arguments
static inline void x86_ret(X86 *c, uint32_t argbytes) { c->s.eip = x86_pop(c); c->s.r[ESP] += argbytes; }

// FPU helpers for host code (CRT math returns in ST0)
void x86_fpu_push(X86 *c, double v);
double x86_fpu_pop(X86 *c);
static inline double x86_fpu_st0(X86 *c) { return c->s.fpu.st[c->s.fpu.top & 7]; }
