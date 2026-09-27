// The C runtime / Win32 string behaviour msttssyn.dll depends on, in one place: the emulator's MSVCRT
// and USER32/KERNEL32 shims call these, and so does the decompiled code, so both always agree. (The
// emulator with these definitions is bit-exact against the genuine engine on the tetyys references.)
#include "vcrt.h"
#include <string.h>
#include <stdlib.h>

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
uint16_t vc_ctype(int ch) {
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


int vc_tolower(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; }
int vc_toupper(int ch) { return (ch >= 'a' && ch <= 'z') ? ch - 32 : ch; }

int vc_stricmp_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = vc_tolower((uint8_t)a[i]), y = vc_tolower((uint8_t)b[i]);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}
static int sgn(int r) { return r < 0 ? -1 : r > 0; }
int vc_strcmp(const char *a, const char *b) { return sgn(strcmp(a, b)); }
int vc_strncmp(const char *a, const char *b, size_t n) { return sgn(strncmp(a, b, n)); }
int vc_stricmp(const char *a, const char *b) { return sgn(vc_stricmp_n(a, b, (size_t)-1)); }
int vc_strnicmp(const char *a, const char *b, size_t n) { return sgn(vc_stricmp_n(a, b, n)); }
int vc_memcmp(const void *a, const void *b, size_t n) { int r = n ? memcmp(a, b, n) : 0; return r < 0 ? -1 : r > 0; }
char *vc_strrev(char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n / 2; i++) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; }
    return s;
}
int32_t vc_atoi(const char *s) { return (int32_t)strtol(s, NULL, 10); }
char *vc_lstrcpynA(char *d, const char *s, uint32_t n) {
    if (!n) return d;
    size_t L = strlen(s);
    if (L > n - 1) L = n - 1;
    memmove(d, s, L);
    d[L] = 0;
    return d;
}
uint32_t vc_CharLowerBuffA(char *s, uint32_t n) { for (uint32_t i = 0; i < n; i++) s[i] = (char)cp1252_tolower((uint8_t)s[i]); return n; }
uint32_t vc_CharUpperBuffA(char *s, uint32_t n) { for (uint32_t i = 0; i < n; i++) s[i] = (char)cp1252_toupper((uint8_t)s[i]); return n; }

// Approximation of the Win32 "word sort" used by CompareStringA/lstrcmp(i)A: hyphen and apostrophe
// are ignored on the first pass; case differences only break ties (lowercase first).
int vc_word_cmp(const uint8_t *a, int na, const uint8_t *b, int nb, int ignore_case) {
    int i = 0, j = 0;
    for (;;) {
        while (i < na && (a[i] == '-' || a[i] == '\'')) i++;
        while (j < nb && (b[j] == '-' || b[j] == '\'')) j++;
        if (i >= na || j >= nb) break;
        int x = cp1252_tolower(a[i]), y = cp1252_tolower(b[j]);
        if (x != y) return x < y ? -1 : 1;
        i++; j++;
    }
    if (i < na) return 1;
    if (j < nb) return -1;
    if (!ignore_case) {
        // tie-break on case: lowercase sorts before uppercase
        for (i = 0, j = 0; i < na && j < nb; i++, j++) {
            if (a[i] != b[j]) {
                int ua = cp1252_isupper(a[i]), ub = cp1252_isupper(b[j]);
                if (ua != ub) return ua ? 1 : -1;
            }
        }
    }
    // tie-break on the ignored punctuation
    int la = 0, lb = 0;
    for (i = 0; i < na; i++) la += a[i] == '-' || a[i] == '\'';
    for (j = 0; j < nb; j++) lb += b[j] == '-' || b[j] == '\'';
    if (la != lb) return la < lb ? -1 : 1;
    return 0;
}
int vc_lstrcmpA(const char *a, const char *b) { return vc_word_cmp((const uint8_t *)a, (int)strlen(a), (const uint8_t *)b, (int)strlen(b), 0); }
int vc_lstrcmpiA(const char *a, const char *b) { return vc_word_cmp((const uint8_t *)a, (int)strlen(a), (const uint8_t *)b, (int)strlen(b), 1); }
void vc_CharLowerA_str(char *s) { for (; *s; s++) *s = (char)cp1252_tolower((uint8_t)*s); }
int vc_MultiByteToWideChar_cp1252(const uint8_t *s, int n, uint16_t *d) { for (int i = 0; i < n; i++) d[i] = cp1252_to_uni[s[i]]; return n; }

int vc_wcsicmp(const uint16_t *a, const uint16_t *b) {
    for (size_t i = 0;; i++) {
        int x = a[i], y = b[i];
        if (x < 128) x = vc_tolower(x);
        if (y < 128) y = vc_tolower(y);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

int vc_IsCharAlphaNumericA(uint8_t ch) {
    return cp1252_isalpha(ch) || (ch >= '0' && ch <= '9') || ch == 0xB2 || ch == 0xB3 || ch == 0xB9;
}

const void *vc_bsearch(const void *key, const void *base, uint32_t num, uint32_t width,
                       int (*cmp)(const void *, const void *)) {
    if (!num) return NULL;
    const char *lo = base, *hi = (const char *)base + (size_t)(num - 1) * width;
    while (lo <= hi) {
        uint32_t half = num / 2;
        if (half) {
            const char *mid = lo + (size_t)(num & 1 ? half : half - 1) * width;
            int r = cmp(key, mid);
            if (!r) return mid;
            if (r < 0) {
                hi = mid - width;
                num = num & 1 ? half : half - 1;
            } else {
                lo = mid + width;
                num = half;
            }
        } else return cmp(key, lo) ? NULL : lo;
    }
    return NULL;
}

int vc_scan_eq_int(const char *s, int32_t *v) {
    if (*s != '=') return 0;
    s++;
    while (vc_isspace((uint8_t)*s)) s++;
    if (!*s) return -1;
    char buf[1024];
    int n = 0;
    while (s[n] && n < 1023 && ((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f') ||
                                (s[n] >= 'A' && s[n] <= 'F') || (n == 0 && (s[n] == '-' || s[n] == '+')) ||
                                s[n] == 'x' || s[n] == 'X')) {
        buf[n] = s[n];
        n++;
    }
    buf[n] = 0;
    if (!n) return 0;
    char *end;
    long long r = strtoll(buf, &end, 0);
    if (end == buf) return 0;
    *v = (int32_t)(uint32_t)(unsigned long long)r;
    return 1;
}


char *vc_strtok(char *s, const char *delim) {
    static _Thread_local char *next;
    if (!s) s = next;
    if (!s) return NULL;
    s += strspn(s, delim);
    if (!*s) {
        next = NULL;
        return NULL;
    }
    char *tok = s;
    s += strcspn(s, delim);
    if (*s) {
        *s = 0;
        next = s + 1;
    } else next = NULL;
    return tok;
}
