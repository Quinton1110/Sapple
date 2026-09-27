// MSVCRT.DLL subset, implemented natively on guest memory.
#include "emu_internal.h"
#include <ctype.h>
#include <math.h>

#define A(i) x86_arg(c, (i))
#define M (e->mem)
#define S(a) emu_str(e, (a))
#define PTR(a, n) ((char *)emu_ptr(e, (a), (n)))

// ------------------------------------------------------------------ CP1252
const uint16_t cp1252_to_uni[256] = {
#define R8(b) b, b + 1, b + 2, b + 3, b + 4, b + 5, b + 6, b + 7
    R8(0x00), R8(0x08), R8(0x10), R8(0x18), R8(0x20), R8(0x28), R8(0x30), R8(0x38),
    R8(0x40), R8(0x48), R8(0x50), R8(0x58), R8(0x60), R8(0x68), R8(0x70), R8(0x78),
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    R8(0xA0), R8(0xA8), R8(0xB0), R8(0xB8), R8(0xC0), R8(0xC8), R8(0xD0), R8(0xD8),
    R8(0xE0), R8(0xE8), R8(0xF0), R8(0xF8),
#undef R8
};
uint8_t uni_to_cp1252(uint16_t u) {
    if (u < 0x80 || (u >= 0xA0 && u <= 0xFF)) return (uint8_t)u;
    for (int i = 0x80; i < 0xA0; i++) if (cp1252_to_uni[i] == u) return (uint8_t)i;
    return '?';
}
int cp1252_isupper(uint8_t ch) {
    return (ch >= 'A' && ch <= 'Z') || ch == 0x8A || ch == 0x8C || ch == 0x8E || ch == 0x9F ||
           (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7);
}
int cp1252_islower(uint8_t ch) {
    return (ch >= 'a' && ch <= 'z') || ch == 0x83 || ch == 0x9A || ch == 0x9C || ch == 0x9E || ch == 0xAA || ch == 0xB5 ||
           ch == 0xBA || (ch >= 0xDF && ch != 0xF7);
}
int cp1252_isalpha(uint8_t ch) { return cp1252_isupper(ch) || cp1252_islower(ch); }
uint8_t cp1252_toupper(uint8_t ch) {
    if (ch >= 'a' && ch <= 'z') return ch - 32;
    if (ch == 0x9A || ch == 0x9C || ch == 0x9E) return ch - 16;
    if (ch == 0xFF) return 0x9F;
    if (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7) return ch - 32;
    return ch;
}
uint8_t cp1252_tolower(uint8_t ch) {
    if (ch >= 'A' && ch <= 'Z') return ch + 32;
    if (ch == 0x8A || ch == 0x8C || ch == 0x8E) return ch + 16;
    if (ch == 0x9F) return 0xFF;
    if (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) return ch + 32;
    return ch;
}

// ------------------------------------------------------------------ C-locale ctype (MSVCRT _ctype)
#define _UPPER 0x1
#define _LOWER 0x2
#define _DIGIT 0x4
#define _SPACE 0x8
#define _PUNCT 0x10
#define _CONTROL 0x20
#define _BLANK 0x40
#define _HEX 0x80
#define _ALPHA_BIT 0x100
static uint16_t ctype_of(int ch) {
    if (ch < 0 || ch > 127) return 0;
    uint16_t t = 0;
    if (ch < 32 || ch == 127) t |= _CONTROL;
    if (ch >= 9 && ch <= 13) t |= _SPACE;
    if (ch == ' ') t |= _SPACE | _BLANK;
    if (ch >= '0' && ch <= '9') t |= _DIGIT | _HEX;
    if (ch >= 'A' && ch <= 'Z') t |= _UPPER | _ALPHA_BIT | (ch <= 'F' ? _HEX : 0);
    if (ch >= 'a' && ch <= 'z') t |= _LOWER | _ALPHA_BIT | (ch <= 'f' ? _HEX : 0);
    if (ch > 32 && ch < 127 && !(t & (_DIGIT | _UPPER | _LOWER))) t |= _PUNCT;
    return t;
}
static inline int c_tolower(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; }
static inline int c_toupper(int ch) { return (ch >= 'a' && ch <= 'z') ? ch - 32 : ch; }

// ------------------------------------------------------------------ memory
static void h_malloc(Emu *e, X86 *c) { E_RET(emu_malloc(e, A(0))); }
static void h_free(Emu *e, X86 *c) { emu_free(e, A(0)); }
static void h_calloc(Emu *e, X86 *c) {
    uint64_t n = (uint64_t)A(0) * A(1);
    if (n > 0x7FFFFFFF) { E_RET(0); return; }
    uint32_t p = emu_malloc(e, (uint32_t)n);
    memset(M + p, 0, (size_t)n);
    E_RET(p);
}
static void h_realloc(Emu *e, X86 *c) { E_RET(emu_realloc(e, A(0), A(1))); }
static void h_memcpy(Emu *e, X86 *c) { uint32_t n = A(2); if (n) memmove(PTR(A(0), n), PTR(A(1), n), n); E_RET(A(0)); }
static void h_memset(Emu *e, X86 *c) { uint32_t n = A(2); if (n) memset(PTR(A(0), n), (int)(A(1) & 0xFF), n); E_RET(A(0)); }
static void h_memcmp(Emu *e, X86 *c) { uint32_t n = A(2); int r = n ? memcmp(PTR(A(0), n), PTR(A(1), n), n) : 0; E_RET(r < 0 ? -1 : r > 0); }
static void h_heapchk(Emu *e, X86 *c) { (void)e; E_RET(-2); }

// ------------------------------------------------------------------ strings
static void h_strlen(Emu *e, X86 *c) { E_RET(strlen(S(A(0)))); }
static void h_strcpy(Emu *e, X86 *c) { const char *s = S(A(1)); size_t n = strlen(s) + 1; memmove(PTR(A(0), (uint32_t)n), s, n); E_RET(A(0)); }
static void h_strcat(Emu *e, X86 *c) {
    const char *s = S(A(1)); size_t n = strlen(s) + 1, d = strlen(S(A(0)));
    memmove(PTR(A(0) + (uint32_t)d, (uint32_t)n), s, n); E_RET(A(0));
}
static void h_strncpy(Emu *e, X86 *c) {
    uint32_t n = A(2); char *d = n ? PTR(A(0), n) : NULL; const char *s = S(A(1));
    size_t L = strlen(s);
    for (uint32_t i = 0; i < n; i++) d[i] = i < L ? s[i] : 0;
    E_RET(A(0));
}
static int sgn(int r) { return r < 0 ? -1 : r > 0; }
static void h_strcmp(Emu *e, X86 *c) { E_RET(sgn(strcmp(S(A(0)), S(A(1))))); }
static void h_strncmp(Emu *e, X86 *c) { E_RET(sgn(strncmp(S(A(0)), S(A(1)), A(2)))); }
static int c_stricmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = c_tolower((uint8_t)a[i]), y = c_tolower((uint8_t)b[i]);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}
static void h_stricmp(Emu *e, X86 *c) {
#ifndef CV_NO_DEBUG_ENV
    static int trace = -1;
    if (trace < 0) trace = getenv("SAPI4_TRACE_STR") != NULL;
    if (trace) fprintf(stderr, "  _stricmp('%s', '%s') @%llu\n", S(A(0)), S(A(1)), (unsigned long long)c->icount);
#endif
    E_RET(sgn(c_stricmp(S(A(0)), S(A(1)), (size_t)-1)));
}
static void h_strnicmp(Emu *e, X86 *c) { E_RET(sgn(c_stricmp(S(A(0)), S(A(1)), A(2)))); }
static void h_strchr(Emu *e, X86 *c) {
    const char *s = S(A(0)); int ch = (int)(A(1) & 0xFF);
    const char *p = ch ? strchr(s, ch) : s + strlen(s);
    E_RET(p ? A(0) + (uint32_t)(p - s) : 0);
}
static void h_strpbrk(Emu *e, X86 *c) { const char *s = S(A(0)); const char *p = strpbrk(s, S(A(1))); E_RET(p ? A(0) + (uint32_t)(p - s) : 0); }
static void h_strspn(Emu *e, X86 *c) { E_RET(strspn(S(A(0)), S(A(1)))); }
static void h_strcspn(Emu *e, X86 *c) { E_RET(strcspn(S(A(0)), S(A(1)))); }
static void h_strupr(Emu *e, X86 *c) { char *s = (char *)S(A(0)); for (; *s; s++) *s = (char)c_toupper((uint8_t)*s); E_RET(A(0)); }
static void h_strrev(Emu *e, X86 *c) {
    char *s = (char *)S(A(0)); size_t n = strlen(s);
    for (size_t i = 0; i < n / 2; i++) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; }
    E_RET(A(0));
}
static void h_strtok(Emu *e, X86 *c) {
    uint32_t s = A(0);
    const char *delim = S(A(1));
    if (!s) s = e->cur->strtok_next;
    if (!s) { E_RET(0); return; }
    char *p = (char *)S(s);
    p += strspn(p, delim);
    if (!*p) { e->cur->strtok_next = 0; E_RET(0); return; }
    uint32_t tok = (uint32_t)(p - (char *)M);
    p += strcspn(p, delim);
    if (*p) { *p = 0; e->cur->strtok_next = (uint32_t)(p + 1 - (char *)M); }
    else e->cur->strtok_next = 0;
    E_RET(tok);
}
static void h_tolower(Emu *e, X86 *c) { (void)e; int ch = (int)A(0); E_RET((ch >= 0 && ch < 256) ? c_tolower(ch) : ch); }
static void h_toupper(Emu *e, X86 *c) { (void)e; int ch = (int)A(0); E_RET((ch >= 0 && ch < 256) ? c_toupper(ch) : ch); }
static void h_isalpha(Emu *e, X86 *c) { (void)e; E_RET(ctype_of((int)A(0)) & (_UPPER | _LOWER | _ALPHA_BIT)); }
static void h_isdigit(Emu *e, X86 *c) { (void)e; E_RET(ctype_of((int)A(0)) & _DIGIT); }
static void h_isctype(Emu *e, X86 *c) { (void)e; E_RET(ctype_of((int)A(0)) & A(1)); }
static void h_atoi(Emu *e, X86 *c) { E_RET((uint32_t)(int32_t)strtol(S(A(0)), NULL, 10)); }
static void h_atof(Emu *e, X86 *c) { x86_fpu_push(c, strtod(S(A(0)), NULL)); }
static void h_abs(Emu *e, X86 *c) { (void)e; int32_t v = (int32_t)A(0); E_RET(v < 0 ? -(uint32_t)v : (uint32_t)v); }
static void h_itoa(Emu *e, X86 *c) {
    int32_t v = (int32_t)A(0); uint32_t radix = A(2);
    char buf[40]; int n = 0; int neg = 0;
    uint32_t u = (uint32_t)v;
    if (radix == 10 && v < 0) { neg = 1; u = (uint32_t)(-(int64_t)v); }
    if (radix < 2 || radix > 36) radix = 10;
    do { uint32_t d = u % radix; buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= radix; } while (u);
    char out[42]; int o = 0;
    if (neg) out[o++] = '-';
    while (n) out[o++] = buf[--n];
    out[o] = 0;
    memcpy(PTR(A(1), (uint32_t)o + 1), out, (size_t)o + 1);
    E_RET(A(1));
}
// wide
static void h_wcslen(Emu *e, X86 *c) { size_t n; emu_wstr(e, A(0), &n); E_RET(n); }
static void h_wcscpy(Emu *e, X86 *c) { size_t n; const uint16_t *s = emu_wstr(e, A(1), &n); memmove(PTR(A(0), (uint32_t)(2 * n + 2)), s, 2 * n + 2); E_RET(A(0)); }
static void h_wcscat(Emu *e, X86 *c) {
    size_t n, d; const uint16_t *s = emu_wstr(e, A(1), &n); emu_wstr(e, A(0), &d);
    memmove(PTR(A(0) + (uint32_t)(2 * d), (uint32_t)(2 * n + 2)), s, 2 * n + 2); E_RET(A(0));
}
static void h_wcsicmp(Emu *e, X86 *c) {
    size_t n1, n2; const uint16_t *a = emu_wstr(e, A(0), &n1), *b = emu_wstr(e, A(1), &n2);
    for (size_t i = 0;; i++) {
        int x = a[i], y = b[i];
        if (x < 128) x = c_tolower(x);
        if (y < 128) y = c_tolower(y);
        if (x != y) { E_RET(x < y ? -1 : 1); return; }
        if (!x) { E_RET(0); return; }
    }
}

// ------------------------------------------------------------------ math (results in ST0)
static double argd(X86 *c, int i) { return rdf64(c, c->s.r[ESP] + 4 + 8 * (uint32_t)i); }
static void h_exp(Emu *e, X86 *c) { (void)e; x86_fpu_push(c, exp(argd(c, 0))); }
static void h_log(Emu *e, X86 *c) { (void)e; x86_fpu_push(c, log(argd(c, 0))); }
static void h_pow(Emu *e, X86 *c) { (void)e; x86_fpu_push(c, pow(argd(c, 0), argd(c, 1))); }
static void h_sqrt(Emu *e, X86 *c) { (void)e; x86_fpu_push(c, sqrt(argd(c, 0))); }
static void h_sin(Emu *e, X86 *c) { (void)e; x86_fpu_push(c, sin(argd(c, 0))); }
static void h_cos(Emu *e, X86 *c) { (void)e; x86_fpu_push(c, cos(argd(c, 0))); }
static void h_floor(Emu *e, X86 *c) { (void)e; x86_fpu_push(c, floor(argd(c, 0))); }
static void h_ceil(Emu *e, X86 *c) { (void)e; x86_fpu_push(c, ceil(argd(c, 0))); }
static void h_ftol(Emu *e, X86 *c) {
    (void)e;
    double v = x86_fpu_pop(c);
    int64_t r;
    if (isnan(v) || v >= 9223372036854775808.0 || v < -9223372036854775808.0) r = INT64_MIN;
    else r = (int64_t)v;  // truncation
    c->s.r[EAX] = (uint32_t)r;
    c->s.r[EDX] = (uint32_t)((uint64_t)r >> 32);
}
static void h_rand(Emu *e, X86 *c) {
    (void)c;
    Thread *t = e->cur;
    t->rand_seed = t->rand_seed * 214013u + 2531011u;
    E_RET((t->rand_seed >> 16) & 0x7FFF);
}

// ------------------------------------------------------------------ setjmp / longjmp / EH
static void h_setjmp3(Emu *e, X86 *c) {
    uint32_t jb = A(0);
    uint32_t esp = c->s.r[ESP];
    wr32(c, jb + 0, c->s.r[EBP]);
    wr32(c, jb + 4, c->s.r[EBX]);
    wr32(c, jb + 8, c->s.r[EDI]);
    wr32(c, jb + 12, c->s.r[ESI]);
    wr32(c, jb + 16, esp + 4);
    wr32(c, jb + 20, rd32(c, esp));
    wr32(c, jb + 24, rd32(c, c->s.fs_base));
    wr32(c, jb + 28, 0xFFFFFFFFu);
    wr32(c, jb + 32, 0x56433230u); // 'VC20'
    (void)e;
    E_RET(0);
}
static void h_longjmp(Emu *e, X86 *c) {
    (void)e;
    uint32_t jb = A(0), val = A(1);
    c->s.r[EBP] = rd32(c, jb + 0);
    c->s.r[EBX] = rd32(c, jb + 4);
    c->s.r[EDI] = rd32(c, jb + 8);
    c->s.r[ESI] = rd32(c, jb + 12);
    c->s.r[ESP] = rd32(c, jb + 16);
    c->s.eip = rd32(c, jb + 20);
    wr32(c, c->s.fs_base, rd32(c, jb + 24));
    c->s.r[EAX] = val ? val : 1;
}
static void h_eh_prolog(Emu *e, X86 *c) {
    (void)e;
    uint32_t esp = c->s.r[ESP];
    uint32_t ret = rd32(c, esp);
    wr32(c, esp, c->s.r[EBP]);
    uint32_t slot = esp;
    x86_push(c, 0xFFFFFFFFu);
    x86_push(c, c->s.r[EAX]);
    x86_push(c, rd32(c, c->s.fs_base));
    wr32(c, c->s.fs_base, c->s.r[ESP]);
    c->s.r[EBP] = slot;
    c->s.eip = ret;
}
static void h_cxxthrow(Emu *e, X86 *c) { (void)e; x86_fault(c, "C++ exception thrown (object %08x, info %08x) - not supported", A(0), A(1)); }
static void h_cxxframehandler(Emu *e, X86 *c) { (void)e; x86_fault(c, "__CxxFrameHandler called (exception dispatch not supported)"); }
static void h_purecall(Emu *e, X86 *c) { (void)e; x86_fault(c, "pure virtual function call"); }
static void h_exit(Emu *e, X86 *c) { (void)e; x86_fault(c, "exit(%d) called", (int)A(0)); }
static void h_nop_ret0(Emu *e, X86 *c) { (void)e; E_RET(0); }
static void h_typeinfo_dtor(Emu *e, X86 *c) { (void)e; (void)c; }

static void h_initterm(Emu *e, X86 *c) {
    uint32_t p = A(0), end = A(1);
    for (; p < end; p += 4) {
        uint32_t fn = rd32(c, p);
        if (fn) emu_call(e, fn, 0, NULL);
    }
}
static void h_onexit(Emu *e, X86 *c) {
    if (e->nonexit < 64) e->onexit[e->nonexit++] = A(0);
    E_RET(A(0));
}
static void h_dllonexit(Emu *e, X86 *c) {
    if (e->nonexit < 64) e->onexit[e->nonexit++] = A(0);
    E_RET(A(0));
}

// ------------------------------------------------------------------ qsort / bsearch (VC CRT algorithm)
static int cmp_call(Emu *e, uint32_t fn, uint32_t a, uint32_t b) {
    uint32_t args[2] = { a, b };
    return (int32_t)emu_call(e, fn, 2, args);
}
static void swap_elems(Emu *e, uint32_t a, uint32_t b, uint32_t w) {
    if (a == b) return;
    char *pa = PTR(a, w), *pb = PTR(b, w);
    for (uint32_t i = 0; i < w; i++) { char t = pa[i]; pa[i] = pb[i]; pb[i] = t; }
}
static void shortsort(Emu *e, uint32_t lo, uint32_t hi, uint32_t w, uint32_t fn) {
    while (hi > lo) {
        uint32_t max = lo;
        for (uint32_t p = lo + w; p <= hi; p += w)
            if (cmp_call(e, fn, p, max) > 0) max = p;
        swap_elems(e, max, hi, w);
        hi -= w;
    }
}
static void h_qsort(Emu *e, X86 *c) {
    uint32_t base = A(0), num = A(1), w = A(2), fn = A(3);
    if (num < 2 || w == 0) return;
    uint32_t lostk[32], histk[32];
    int sp = 0;
    uint32_t lo = base, hi = base + w * (num - 1);
recurse:;
    uint32_t size = (hi - lo) / w + 1;
    if (size <= 8) {
        shortsort(e, lo, hi, w, fn);
    } else {
        uint32_t mid = lo + (size / 2) * w;
        swap_elems(e, mid, lo, w);
        uint32_t loguy = lo, higuy = hi + w;
        for (;;) {
            do { loguy += w; } while (loguy <= hi && cmp_call(e, fn, loguy, lo) <= 0);
            do { higuy -= w; } while (higuy > lo && cmp_call(e, fn, higuy, lo) >= 0);
            if (higuy < loguy) break;
            swap_elems(e, loguy, higuy, w);
        }
        swap_elems(e, lo, higuy, w);
        if (higuy - 1 - lo >= hi - loguy) {
            if (lo + w < higuy) { lostk[sp] = lo; histk[sp] = higuy - w; ++sp; }
            if (loguy < hi) { lo = loguy; goto recurse; }
        } else {
            if (loguy < hi) { lostk[sp] = loguy; histk[sp] = hi; ++sp; }
            if (lo + w < higuy) { hi = higuy - w; goto recurse; }
        }
    }
    --sp;
    if (sp >= 0) { lo = lostk[sp]; hi = histk[sp]; goto recurse; }
}
static void h_bsearch(Emu *e, X86 *c) {
    uint32_t key = A(0), base = A(1), num = A(2), w = A(3), fn = A(4);
    uint32_t lo = base, hi = base + (num - 1) * w;
    if (!num) { E_RET(0); return; }
    while (lo <= hi) {
        uint32_t half = num / 2;
        if (half) {
            uint32_t mid = lo + (num & 1 ? half : (half - 1)) * w;
            int r = cmp_call(e, fn, key, mid);
            if (!r) { E_RET(mid); return; }
            else if (r < 0) { hi = mid - w; num = num & 1 ? half : half - 1; }
            else { lo = mid + w; num = half; }
        } else if (num) {
            E_RET(cmp_call(e, fn, key, lo) ? 0 : lo);
            return;
        } else break;
    }
    E_RET(0);
}

// ------------------------------------------------------------------ printf family
// Formats with guest varargs at `va`. wide_fmt: fmt came from a wide function (%s = wide, %S = narrow).
int emu_format(Emu *e, char *out, size_t outsz, const char *fmt, uint32_t va, int wide_fmt) {
    X86 *c = &e->cpu;
    size_t o = 0;
#define PUT(ch) do { if (o + 1 < outsz) out[o] = (char)(ch); o++; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { PUT(*p); continue; }
        const char *start = p++;
        if (*p == '%') { PUT('%'); continue; }
        char spec[32]; int sl = 0;
        spec[sl++] = '%';
        while (strchr("-+ #0", *p)) { if (sl < 20) spec[sl++] = *p; p++; }
        if (*p == '*') { int w = (int32_t)rd32(c, va); va += 4; sl += snprintf(spec + sl, sizeof spec - (size_t)sl, "%d", w); p++; }
        else while (isdigit((uint8_t)*p)) { if (sl < 24) spec[sl++] = *p; p++; }
        if (*p == '.') {
            spec[sl++] = *p++;
            if (*p == '*') { int pr = (int32_t)rd32(c, va); va += 4; sl += snprintf(spec + sl, sizeof spec - (size_t)sl, "%d", pr); p++; }
            else while (isdigit((uint8_t)*p)) { if (sl < 28) spec[sl++] = *p; p++; }
        }
        int lng = 0, shrt = 0, i64 = 0, wideflag = 0;
        for (;;) {
            if (*p == 'l') { lng++; p++; }
            else if (*p == 'h') { shrt = 1; p++; }
            else if (*p == 'L') { lng = 2; p++; }
            else if (*p == 'w') { wideflag = 1; p++; }
            else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { i64 = 1; p += 3; }
            else break;
        }
        char conv = *p;
        char tmp[512];
        spec[sl] = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            if (i64 || lng >= 2) {
                uint64_t v = rd64(c, va); va += 8;
                spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
                snprintf(tmp, sizeof tmp, spec, v);
            } else {
                uint32_t v = rd32(c, va); va += 4;
                spec[sl++] = conv; spec[sl] = 0;
                if (conv == 'd' || conv == 'i') snprintf(tmp, sizeof tmp, spec, shrt ? (int)(int16_t)v : (int)(int32_t)v);
                else snprintf(tmp, sizeof tmp, spec, shrt ? (unsigned)(uint16_t)v : (unsigned)v);
            }
            break; }
        case 'c': case 'C': {
            uint32_t v = rd32(c, va); va += 4;
            int widechar = (conv == 'C') != (wide_fmt != 0);
            if (lng || wideflag) widechar = 1; else if (shrt) widechar = 0;
            char ch = widechar ? (char)uni_to_cp1252((uint16_t)v) : (char)v;
            spec[sl++] = 'c'; spec[sl] = 0;
            snprintf(tmp, sizeof tmp, spec, ch);
            break; }
        case 's': case 'S': {
            uint32_t a = rd32(c, va); va += 4;
            int widestr = (conv == 'S') != (wide_fmt != 0);
            if (lng || wideflag) widestr = 1; else if (shrt) widestr = 0;
            char sbuf[2048];
            if (!a) snprintf(sbuf, sizeof sbuf, "(null)");
            else if (widestr) {
                size_t n; const uint16_t *w = emu_wstr(e, a, &n);
                size_t k = 0;
                for (; k < n && k < sizeof sbuf - 1; k++) sbuf[k] = (char)uni_to_cp1252(w[k]);
                sbuf[k] = 0;
            } else snprintf(sbuf, sizeof sbuf, "%s", emu_str(e, a));
            spec[sl++] = 's'; spec[sl] = 0;
            snprintf(tmp, sizeof tmp, spec, sbuf);
            break; }
        case 'f': case 'e': case 'E': case 'g': case 'G': {
            double v = rdf64(c, va); va += 8;
            spec[sl++] = conv; spec[sl] = 0;
            snprintf(tmp, sizeof tmp, spec, v);
            break; }
        case 'p': { uint32_t v = rd32(c, va); va += 4; snprintf(tmp, sizeof tmp, "%08X", v); break; }
        case 'n': { uint32_t a = rd32(c, va); va += 4; wr32(c, a, (uint32_t)o); tmp[0] = 0; break; }
        default:
            // unknown: copy literally
            snprintf(tmp, sizeof tmp, "%.*s", (int)(p - start + 1), start);
        }
        for (char *q = tmp; *q; q++) PUT(*q);
    }
    if (outsz) out[o < outsz ? o : outsz - 1] = 0;
    return (int)o;
#undef PUT
}

static void h_sprintf(Emu *e, X86 *c) {
    char buf[8192];
    int n = emu_format(e, buf, sizeof buf, S(A(1)), c->s.r[ESP] + 12, 0);
    if (n >= (int)sizeof buf) x86_fault(c, "sprintf output too long");
    memcpy(PTR(A(0), (uint32_t)n + 1), buf, (size_t)n + 1);
    E_RET(n);
}
static void h_printf(Emu *e, X86 *c) {
    char buf[8192];
    int n = emu_format(e, buf, sizeof buf, S(A(0)), c->s.r[ESP] + 8, 0);
    EMU_LOG("[guest printf] %s", buf);
    E_RET(n);
}
static void h_swprintf(Emu *e, X86 *c) {
    size_t fl; const uint16_t *wf = emu_wstr(e, A(1), &fl);
    char f[2048]; size_t k = 0;
    for (; k < fl && k < sizeof f - 1; k++) f[k] = (char)uni_to_cp1252(wf[k]);
    f[k] = 0;
    char buf[8192];
    int n = emu_format(e, buf, sizeof buf, f, c->s.r[ESP] + 12, 1);
    if (n >= (int)sizeof buf) x86_fault(c, "swprintf output too long");
    uint16_t *d = (uint16_t *)(void *)PTR(A(0), 2u * (uint32_t)n + 2);
    for (int i = 0; i <= n; i++) d[i] = cp1252_to_uni[(uint8_t)buf[i]];
    E_RET(n);
}

// ------------------------------------------------------------------ sscanf
static int scan_impl(Emu *e, const char *str, const char *fmt, uint32_t va, int *consumed);
int emu_scan(Emu *e, const char *str, const char *fmt, uint32_t va) { return scan_impl(e, str, fmt, va, NULL); }
static int scan_impl(Emu *e, const char *str, const char *fmt, uint32_t va, int *consumed) {
#define SCAN_RET(v) do { if (consumed) *consumed = (int)(s - str); return (v); } while (0)
    X86 *c = &e->cpu;
    const char *s = str;
    int assigned = 0;
    for (const char *p = fmt; *p; p++) {
        if (isspace((uint8_t)*p)) { while (isspace((uint8_t)*s)) s++; continue; }
        if (*p != '%') { if (*s != *p) SCAN_RET(assigned); s++; continue; }
        p++;
        if (*p == '%') { if (*s != '%') SCAN_RET(assigned); s++; continue; }
        int suppress = 0, width = 0, lng = 0, shrt = 0;
        if (*p == '*') { suppress = 1; p++; }
        while (isdigit((uint8_t)*p)) width = width * 10 + (*p++ - '0');
        for (;;) { if (*p == 'l') { lng++; p++; } else if (*p == 'h') { shrt = 1; p++; } else if (*p == 'L') { lng = 2; p++; } else break; }
        char conv = *p;
        if (conv != 'c' && conv != '[' && conv != 'n') while (isspace((uint8_t)*s)) s++;
        if (!*s && conv != 'n') SCAN_RET(assigned ? assigned : -1);
        char buf[1024];
        int w = width ? width : (int)sizeof buf - 1;
        if (w > (int)sizeof buf - 1) w = (int)sizeof buf - 1;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            int n = 0;
            while (s[n] && n < w && (isxdigit((uint8_t)s[n]) || (n == 0 && (s[n] == '-' || s[n] == '+')) || s[n] == 'x' || s[n] == 'X')) {
                if ((conv == 'd' || conv == 'u') && !isdigit((uint8_t)s[n]) && !(n == 0 && (s[n] == '-' || s[n] == '+'))) break;
                buf[n] = s[n]; n++;
            }
            buf[n] = 0;
            if (!n) SCAN_RET(assigned);
            char *end;
            int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            long long v = strtoll(buf, &end, base);
            s += end - buf;
            if (end == buf) SCAN_RET(assigned);
            if (!suppress) {
                uint32_t a = rd32(c, va); va += 4;
                if (shrt) wr16(c, a, (uint16_t)v); else wr32(c, a, (uint32_t)v);
                assigned++;
            }
            break; }
        case 'f': case 'e': case 'g': case 'E': case 'G': {
            int n = 0;
            while (s[n] && n < w && (isdigit((uint8_t)s[n]) || strchr("+-.eE", s[n]))) { buf[n] = s[n]; n++; }
            buf[n] = 0;
            char *end;
            double v = strtod(buf, &end);
            if (end == buf) SCAN_RET(assigned);
            s += end - buf;
            if (!suppress) {
                uint32_t a = rd32(c, va); va += 4;
                if (lng) { memcpy(emu_ptr(e, a, 8), &v, 8); } else { float f = (float)v; memcpy(emu_ptr(e, a, 4), &f, 4); }
                assigned++;
            }
            break; }
        case 's': {
            int n = 0;
            while (s[n] && !isspace((uint8_t)s[n]) && n < w) { buf[n] = s[n]; n++; }
            buf[n] = 0; s += n;
            if (!suppress) { uint32_t a = rd32(c, va); va += 4; memcpy(emu_ptr(e, a, (uint32_t)n + 1), buf, (size_t)n + 1); assigned++; }
            break; }
        case 'c': {
            int n = width ? width : 1;
            for (int i = 0; i < n; i++) if (!s[i]) SCAN_RET(assigned);
            if (!suppress) { uint32_t a = rd32(c, va); va += 4; memcpy(emu_ptr(e, a, (uint32_t)n), s, (size_t)n); assigned++; }
            s += n;
            break; }
        case '[': {
            p++;
            int neg = 0;
            if (*p == '^') { neg = 1; p++; }
            char set[256] = { 0 };
            if (*p == ']') { set[']'] = 1; p++; }
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') { for (int ch = (uint8_t)p[0]; ch <= (uint8_t)p[2]; ch++) set[ch] = 1; p += 3; }
                else set[(uint8_t)*p++] = 1;
            }
            int n = 0;
            while (s[n] && n < w && (set[(uint8_t)s[n]] != 0) != neg) { buf[n] = s[n]; n++; }
            if (!n) SCAN_RET(assigned);
            buf[n] = 0; s += n;
            if (!suppress) { uint32_t a = rd32(c, va); va += 4; memcpy(emu_ptr(e, a, (uint32_t)n + 1), buf, (size_t)n + 1); assigned++; }
            break; }
        case 'n': if (!suppress) { uint32_t a = rd32(c, va); va += 4; wr32(c, a, (uint32_t)(s - str)); } break;
        default: x86_fault(c, "sscanf: unsupported conversion %%%c", conv);
        }
    }
    SCAN_RET(assigned);
#undef SCAN_RET
}
static void h_sscanf(Emu *e, X86 *c) {
    char str[4096];
    snprintf(str, sizeof str, "%s", S(A(0)));
    int r = emu_scan(e, str, S(A(1)), c->s.r[ESP] + 12);
    if (emu_trace_api) EMU_LOG("  sscanf('%s', '%s') -> %d\n", str, S(A(1)), r);
    E_RET(r);
}

// ------------------------------------------------------------------ more (TruVoice)
static void h_islower(Emu *e, X86 *c) { (void)e; E_RET(ctype_of((int)A(0)) & _LOWER); }
static void h_isupper(Emu *e, X86 *c) { (void)e; E_RET(ctype_of((int)A(0)) & _UPPER); }
static void h_isalnum(Emu *e, X86 *c) { (void)e; E_RET(ctype_of((int)A(0)) & (_UPPER | _LOWER | _DIGIT | _ALPHA_BIT)); }
static void h_isspace(Emu *e, X86 *c) { (void)e; E_RET(ctype_of((int)A(0)) & _SPACE); }
static void h_atol(Emu *e, X86 *c) { E_RET((uint32_t)(int32_t)strtol(S(A(0)), NULL, 10)); }
static void h_mbscmp(Emu *e, X86 *c) { E_RET(sgn(strcmp(S(A(0)), S(A(1))))); }   // unsigned byte compare, like strcmp
static void h_strlwr(Emu *e, X86 *c) { char *s = (char *)S(A(0)); for (; *s; s++) *s = (char)c_tolower((uint8_t)*s); E_RET(A(0)); }
static void h_strncat(Emu *e, X86 *c) {
    const char *src = S(A(1)); size_t d = strlen(S(A(0))), n = strlen(src);
    if (n > A(2)) n = A(2);
    char *dst = PTR(A(0) + (uint32_t)d, (uint32_t)n + 1);
    memmove(dst, src, n);
    dst[n] = 0;
    E_RET(A(0));
}
static void h_strstr(Emu *e, X86 *c) { const char *h = S(A(0)); const char *p = strstr(h, S(A(1))); E_RET(p ? A(0) + (uint32_t)(p - h) : 0); }
static void h_strdup(Emu *e, X86 *c) { E_RET(A(0) ? emu_strdup(e, S(A(0))) : 0); }
// _CIpow: x in ST(1), y in ST(0); leaves x^y in ST(0)
static void h_CIpow(Emu *e, X86 *c) { (void)e; double y = x86_fpu_pop(c), x = x86_fpu_pop(c); x86_fpu_push(c, pow(x, y)); }

// stdio: read-only streams over mapped paths (the engines only ever read optional user dictionaries)
static void h_fopen(Emu *e, X86 *c) {
    const char *name = S(A(0)), *mode = S(A(1));
    char host[1100];
    if (strpbrk(mode, "wa+")) {
        if (emu_trace_api) EMU_LOG("[emu] fopen('%s', '%s') refused (write)\n", name, mode);
        E_RET(0);
        return;
    }
    if (!emu_host_path(e, name, host, sizeof host, 1)) { if (emu_trace_api) EMU_LOG("[emu] fopen('%s') -> not found\n", name); E_RET(0); return; }
    int slot = -1;
    for (int i = 0; i < 16; i++) if (!e->crt_files[i].fp) { slot = i; break; }
    if (slot < 0) { E_RET(0); return; }
    FILE *f = fopen(host, strchr(mode, 'b') ? "rb" : "r");
    if (emu_trace_api) EMU_LOG("[emu] fopen('%s') -> %s%s\n", name, host, f ? "" : " (failed)");
    if (!f) { E_RET(0); return; }
    uint32_t g = emu_malloc(e, 32);
    memset(PTR(g, 32), 0, 32);
    e->crt_files[slot].g = g;
    e->crt_files[slot].fp = f;
    E_RET(g);
}
static FILE *crt_file(Emu *e, uint32_t g) {
    for (int i = 0; i < 16; i++) if (e->crt_files[i].fp && e->crt_files[i].g == g) return e->crt_files[i].fp;
    return NULL;
}
static void h_fclose(Emu *e, X86 *c) {
    for (int i = 0; i < 16; i++) if (e->crt_files[i].fp && e->crt_files[i].g == A(0)) {
        fclose(e->crt_files[i].fp);
        e->crt_files[i].fp = NULL;
        emu_free(e, e->crt_files[i].g);
        E_RET(0);
        return;
    }
    E_RET(0xFFFFFFFFu);
}
static void h_fscanf(Emu *e, X86 *c) {
    FILE *f = crt_file(e, A(0));
    if (!f) { E_RET(0xFFFFFFFFu); return; }
    char buf[4096];
    long pos = ftell(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    if (!n) { E_RET(0xFFFFFFFFu); return; }
    int used = 0;
    int r = scan_impl(e, buf, S(A(1)), c->s.r[ESP] + 12, &used);
    fseek(f, pos + used, SEEK_SET);
    E_RET((uint32_t)r);
}

// MSVCRT's _iob[20]: MSVCP50's static initialisers wrap stdin/stdout/stderr in filebufs. Nothing is
// ever read from or written to them here (fputc/fwrite etc. are not provided).
static void h_p_iob(Emu *e, X86 *c) {
    if (!e->iob) {
        e->iob = emu_static(e, 20 * 32);
        for (uint32_t i = 0; i < 3; i++) wr32(c, e->iob + 32 * i + 16, i);   // _file
    }
    E_RET(e->iob);
}

// MSVCRT data exports imported by MSVCP50.dll: the "C" locale, no setlocale ever running.
static const struct { const char *name; uint32_t size; } crt_data_defs[] = {
    { "__setlc_active", 4 }, { "__unguarded_readlc_active", 4 }, { "__lc_handle", 24 }, { "__lc_codepage", 4 },
};
uint32_t emu_crt_data_import(Emu *e, const char *name) {
    for (int i = 0; i < (int)(sizeof crt_data_defs / sizeof crt_data_defs[0]); i++) {
        if (strcmp(name, crt_data_defs[i].name)) continue;
        if (!e->crt_data[i]) e->crt_data[i] = emu_static(e, crt_data_defs[i].size);   // zeroed
        return e->crt_data[i];
    }
    return 0;
}
static void h_lock_nop(Emu *e, X86 *c) { (void)e; (void)c; }   // _lock/_unlock: guest threads are cooperative

// ------------------------------------------------------------------ registration
static const ApiDef crt_apis[] = {
    { "MSVCRT.dll", "malloc", h_malloc, 0 },
    { "MSVCRT.dll", "free", h_free, 0 },
    { "MSVCRT.dll", "calloc", h_calloc, 0 },
    { "MSVCRT.dll", "realloc", h_realloc, 0 },
    { "MSVCRT.dll", "??2@YAPAXI@Z", h_malloc, 0 },
    { "MSVCRT.dll", "??3@YAXPAX@Z", h_free, 0 },
    { "MSVCRT.dll", "memcpy", h_memcpy, 0 },
    { "MSVCRT.dll", "memmove", h_memcpy, 0 },
    { "MSVCRT.dll", "memset", h_memset, 0 },
    { "MSVCRT.dll", "memcmp", h_memcmp, 0 },
    { "MSVCRT.dll", "_heapchk", h_heapchk, 0 },
    { "MSVCRT.dll", "strlen", h_strlen, 0 },
    { "MSVCRT.dll", "strcpy", h_strcpy, 0 },
    { "MSVCRT.dll", "strcat", h_strcat, 0 },
    { "MSVCRT.dll", "strncpy", h_strncpy, 0 },
    { "MSVCRT.dll", "strcmp", h_strcmp, 0 },
    { "MSVCRT.dll", "strncmp", h_strncmp, 0 },
    { "MSVCRT.dll", "_stricmp", h_stricmp, 0 },
    { "MSVCRT.dll", "_strnicmp", h_strnicmp, 0 },
    { "MSVCRT.dll", "strchr", h_strchr, 0 },
    { "MSVCRT.dll", "strpbrk", h_strpbrk, 0 },
    { "MSVCRT.dll", "strspn", h_strspn, 0 },
    { "MSVCRT.dll", "strcspn", h_strcspn, 0 },
    { "MSVCRT.dll", "_strupr", h_strupr, 0 },
    { "MSVCRT.dll", "_strrev", h_strrev, 0 },
    { "MSVCRT.dll", "strtok", h_strtok, 0 },
    { "MSVCRT.dll", "tolower", h_tolower, 0 },
    { "MSVCRT.dll", "toupper", h_toupper, 0 },
    { "MSVCRT.dll", "isalpha", h_isalpha, 0 },
    { "MSVCRT.dll", "isdigit", h_isdigit, 0 },
    { "MSVCRT.dll", "_isctype", h_isctype, 0 },
    { "MSVCRT.dll", "atoi", h_atoi, 0 },
    { "MSVCRT.dll", "atof", h_atof, 0 },
    { "MSVCRT.dll", "abs", h_abs, 0 },
    { "MSVCRT.dll", "_itoa", h_itoa, 0 },
    { "MSVCRT.dll", "wcslen", h_wcslen, 0 },
    { "MSVCRT.dll", "wcscpy", h_wcscpy, 0 },
    { "MSVCRT.dll", "wcscat", h_wcscat, 0 },
    { "MSVCRT.dll", "_wcsicmp", h_wcsicmp, 0 },
    { "MSVCRT.dll", "exp", h_exp, 0 },
    { "MSVCRT.dll", "log", h_log, 0 },
    { "MSVCRT.dll", "pow", h_pow, 0 },
    { "MSVCRT.dll", "sqrt", h_sqrt, 0 },
    { "MSVCRT.dll", "sin", h_sin, 0 },
    { "MSVCRT.dll", "cos", h_cos, 0 },
    { "MSVCRT.dll", "floor", h_floor, 0 },
    { "MSVCRT.dll", "ceil", h_ceil, 0 },
    { "MSVCRT.dll", "_ftol", h_ftol, 0 },
    { "MSVCRT.dll", "rand", h_rand, 0 },
    { "MSVCRT.dll", "_setjmp3", h_setjmp3, 0 },
    { "MSVCRT.dll", "longjmp", h_longjmp, -1 },
    { "MSVCRT.dll", "_EH_prolog", h_eh_prolog, -1 },
    { "MSVCRT.dll", "_CxxThrowException", h_cxxthrow, 0 },
    { "MSVCRT.dll", "__CxxFrameHandler", h_cxxframehandler, 0 },
    { "MSVCRT.dll", "_purecall", h_purecall, 0 },
    { "MSVCRT.dll", "exit", h_exit, 0 },
    { "MSVCRT.dll", "??1type_info@@UAE@XZ", h_typeinfo_dtor, 0 },
    { "MSVCRT.dll", "_initterm", h_initterm, 0 },
    { "MSVCRT.dll", "_onexit", h_onexit, 0 },
    { "MSVCRT.dll", "__dllonexit", h_dllonexit, 0 },
    { "MSVCRT.dll", "qsort", h_qsort, 0 },
    { "MSVCRT.dll", "bsearch", h_bsearch, 0 },
    { "MSVCRT.dll", "sprintf", h_sprintf, 0 },
    { "MSVCRT.dll", "printf", h_printf, 0 },
    { "MSVCRT.dll", "swprintf", h_swprintf, 0 },
    { "MSVCRT.dll", "sscanf", h_sscanf, 0 },
    { "MSVCRT.dll", "_controlfp", h_nop_ret0, 0 },
    { "MSVCRT.dll", "islower", h_islower, 0 },
    { "MSVCRT.dll", "isupper", h_isupper, 0 },
    { "MSVCRT.dll", "isalnum", h_isalnum, 0 },
    { "MSVCRT.dll", "isspace", h_isspace, 0 },
    { "MSVCRT.dll", "atol", h_atol, 0 },
    { "MSVCRT.dll", "_mbscmp", h_mbscmp, 0 },
    { "MSVCRT.dll", "_strlwr", h_strlwr, 0 },
    { "MSVCRT.dll", "strncat", h_strncat, 0 },
    { "MSVCRT.dll", "strstr", h_strstr, 0 },
    { "MSVCRT.dll", "_strdup", h_strdup, 0 },
    { "MSVCRT.dll", "_CIpow", h_CIpow, 0 },
    { "MSVCRT.dll", "fopen", h_fopen, 0 },
    { "MSVCRT.dll", "fclose", h_fclose, 0 },
    { "MSVCRT.dll", "fscanf", h_fscanf, 0 },
    { "MSVCRT.dll", "__p__iob", h_p_iob, 0 },
    { "MSVCRT.dll", "_lock", h_lock_nop, 0 },
    { "MSVCRT.dll", "_unlock", h_lock_nop, 0 },
};

void emu_register_crt_apis(void) { emu_add_apis(crt_apis, (int)(sizeof crt_apis / sizeof crt_apis[0])); }

void emu_register_crt(Emu *e) {
    // data imports
    e->ctype_table = emu_static(e, 257 * 2);
    for (int i = -1; i < 256; i++) wr16(&e->cpu, e->ctype_table + 2u * (uint32_t)(i + 1), ctype_of(i));
    e->pctype_var = emu_static(e, 4);
    wr32(&e->cpu, e->pctype_var, e->ctype_table + 2);
    e->mb_cur_max = emu_static(e, 4);
    wr32(&e->cpu, e->mb_cur_max, 1);
    e->adjust_fdiv = emu_static(e, 4);
}
