#include "fe_lexer.h"
#include "vcrt.h"
#include <string.h>

LAYOUT(Lexer, pending, 0x10);

int32_t lex_find(const char *key, uint32_t len, GPTR(char) *table, int32_t count) {
    int32_t lo = 0, hi = count - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        const char *s = GP(char, table[mid]);
        uint32_t i = 0;
        int dir = 0;                        // -1: key below, 1: key above
        for (; i < len; i++, s++) {
            if (*s == 0) break;
            int8_t d = (int8_t)key[i], b = (int8_t)*s;
            if (d < b) { dir = -1; break; }
            if (d > b) { dir = 1; break; }
        }
        if (dir == 0) {
            if (i != len) dir = 1;
            else if (*s == 0) return mid;
            else dir = -1;
        }
        if (dir < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return -1;
}

int lex_is_dash(char c) {
    if (c == '-') return 1;
    char x = *DLLVAR(char, 0x637399db);
    return x && x == c;
}

#define T1 DLLPTR(char, 0x63733c28), *DLLVAR(int32_t, 0x63735244)

int lex_abbrev(char *s, uint32_t len) {
    char buf[0x200];
    char *e = s + len - 1;
    while (e > s) {
        if (lex_is_dash(*e)) { e++; break; }
        e--;
    }
    if (*e == 0) e = s;
    uint32_t n = len - (uint32_t)(e - s);
    int r = lex_find(e, n, T1) >= 0;
    if (r) return r;
    if (!cp1252_isupper((uint8_t)*e)) return 0;
    vc_lstrcpynA(buf, e, n + 1);
    if (vc_CharLowerBuffA(buf, n) == n) r = lex_find(buf, n, T1) >= 0;
    if (r) return r;
    if (n > 2) {
        vc_lstrcpynA(buf, e, n + 1);
        int32_t vowels = 0, lower = 0;
        for (char *p = buf; *p; p++) {
            switch ((int8_t)*p) {
            case 'A': case 'E': case 'I': case 'O': case 'U': case 'Y':
            case 'a': case 'e': case 'i': case 'o': case 'u': case 'y': vowels++;
            }
            lower |= cp1252_islower((uint8_t)*p);
        }
        r = !vowels && lower;
    }
    if (r) return r;
    if (n >= 4 && !(n & 1)) {
        uint32_t k = 0;
        for (; k < n; k++)
            if ((k & 1) ? buf[k] != '.' : !cp1252_isalpha((uint8_t)buf[k])) break;
        if (k >= n) r = 1;
    }
    return r;
}

int lex_title(const char *s, uint32_t len, int32_t upper) {
    char buf[0x1f8];
    vc_lstrcpynA(buf, s, len + 1);
    if (upper && vc_CharUpperBuffA(buf, 1) != 1) return 0;
    return lex_find(buf, len, DLLPTR(char, 0x63735248), *DLLVAR(int32_t, 0x637353a4)) >= 0;
}

int lex_common(const char *s, uint32_t len) {
    if (!cp1252_isupper((uint8_t)*s)) return 0;
    return lex_find(s, len, DLLPTR(char, 0x637353e0), *DLLVAR(int32_t, 0x63735560)) >= 0;
}

int lex_emoticon(const char *s, int32_t i, int32_t *len) {
    const char *p = s + i;
    if (*p == '>' || *p == '}') p++;
    if (*p != ':' && *p != ';' && *p != '8' && *p != '%') return 0;
    p++;
    if (*p != '-' && *p != '^') return 0;
    p++;
    char c = *p;
    if (c != ')' && c != '(' && c != '>' && c != 'I' && c != 'O' && c != 'o' && c != '0') return 0;
    *len = (int32_t)(p - s - i + 1);
    return 1;
}

static void span_set(TextSpan *t, GPTR(char) buf, int32_t kind, int32_t pos, int32_t len, int32_t f10) {
    t->buf = buf;
    t->f4 = kind;
    t->pos = pos;
    t->len = len;
    t->f10 = f10;
}

void lex_init(Lexer *lx, GPTR(char) buf, int32_t end) {
    lx->start = 0;
    lx->cur = 0;
    lx->buf = buf;
    lx->end = end;
    memset(&lx->pending, 0, sizeof lx->pending);
}

#define CLASS(c) DLLVAR(const int32_t, LEX_CLASS)[(uint8_t)(c)]

void lex_next(Lexer *lx, TextSpan *out) {
    const char *b = GP(char, lx->buf);
    if (lx->pending.f4) { *out = lx->pending; goto done; }
    lx->cur = lx->start;
    if (lx->cur == lx->end) { span_set(out, lx->buf, 0xf, lx->cur, 0, 0); goto done; }
    int32_t k = CLASS(b[lx->cur]);
#define ONE(kind) do { span_set(out, lx->buf, kind, lx->cur, 1, 0); lx->cur++; goto done; } while (0)
    if (k == 0x11) ONE(0xe);
    if (k == 0x12) {
        if (CLASS(b[lx->cur + 1]) == 0x13) { span_set(out, lx->buf, 0xe, lx->cur, 2, 0); lx->cur += 2; goto done; }
        ONE(0xd);
    }
    int32_t n;
    if (lex_emoticon(b, lx->cur, &n)) { span_set(out, lx->buf, 0x10, lx->cur, n, 0); lx->cur += n; goto done; }
    if (k == 0xf) ONE(6);
    if (k == 4 || k == 5) ONE(0xa);
    if (k == 7) {
        int32_t start = lx->cur, esc = 0;
        lx->cur++;
        while (lx->cur < lx->end) {
            uint8_t c = (uint8_t)b[lx->cur];
            if (CLASS(c) != 7) {
                if (c != 0x1b) break;
                esc = 1;
                do { esc++; lx->cur++; } while (CLASS(b[lx->cur]) != 7);
            }
            lx->cur++;
        }
        span_set(out, lx->buf, 1, start, lx->cur - esc - start, 0);
        goto done;
    }
    if (k == 0x14) {
        int32_t start = lx->cur;
        lx->cur++;
        while (lx->cur < lx->end && CLASS(b[lx->cur]) == 0x14) lx->cur++;
        span_set(out, lx->buf, 2, start, lx->cur - start, 0);
        goto done;
    }
    if (k == 0xa) ONE(8);
    if (k == 0xb) ONE(9);
    if (k == 9 || k == 8 || k == 0xc || k == 0xd) ONE(0xb);
    if (k == 0xe) ONE(0xc);
    if (k == 6) {                           // numbers: digit groups and separators
        int32_t start = lx->cur, st = 1;
        lx->cur++;
        int32_t mark = lx->cur;
        while (st > 0) {
            int32_t cur = lx->cur;
            int32_t c = cur < lx->end ? CLASS(b[cur]) : 0;
            switch (st) {
            case 1:
                if (c == 6) { lx->cur++; st = 2; }
                else if (c == 7) { lx->cur++; st = 3; }
                else st = -1;
                break;
            case 2:
                if ((uint32_t)(cur - start) >= 3) { mark = cur; st = -1; }
                else if (c == 6) lx->cur++;
                else st = -1;
                break;
            case 3:
                if (c == 6) { lx->cur++; st = 4; }
                else st = -1;
                break;
            case 4: {
                uint32_t g = ((uint32_t)(cur - start) >> 1) + 1;
                if (g == 3 || g == 5) mark = cur;
                else if (g == 7) { st = -1; mark = cur; }
                if (st > 0) {
                    if (c == 7) { lx->cur++; st = 3; }
                    else st = -1;
                }
                break;
            }
            }
        }
        lx->cur = mark;
        int32_t len = mark - start;
        span_set(out, lx->buf, len == 1 ? 0xa : len != 3 ? 7 : 6, start, len, 0);
        goto done;
    }
    if (k != 2 && k != 1 && k != 3) ONE(0xd);
    {                                       // words: letters, apostrophes, hyphens, dots
        int32_t start = lx->cur, st = 0;
        for (;;) {
            int32_t cur = lx->cur;
            int32_t c = cur < lx->end ? CLASS(b[cur]) : 0;
            char ch = b[cur];
#define GO(s) do { lx->cur++; st = (s); } while (0)
            if (st == 0) {
                if (c == 1) GO(0xa);
                else if (c == 2) GO(0xb);
                else if (c == 3) GO(0x3c);
            } else if (st == 0xa || st == 0xb) {
                if (c == 1 || c == 3) GO(0xa);
                else if (c == 2) GO(st == 0xa ? 0xb : st);
                else if (c == 6) GO(0x14);
                else if (c == 8) { if (ch == '\'') GO(0x28); else st = -4; }
                else if (c == 0x10) GO(0xb);
                else if (c == 0x15 && ch == '-') GO(0xa);
                else st = -4;
            } else if (st == 0x14) {
                if (c == 1 || c == 3) GO(0x1e);
                else if (c == 2) GO(0x1f);
                else if (c == 8 && ch == '\'') GO(0x28);
                else st = 0x32;
            } else if (st == 0x1e || st == 0x1f) {
                if (c == 1 || c == 3) GO(0x1e);
                else if (c == 2) GO(st == 0x1e ? 0x1f : st);
                else if (c == 6) GO(0x14);
                else if (c == 0x10) GO(0x1f);
                else if (c == 0x15 && ch == '-') GO(0x1e);
                else st = -4;
            } else if (st == 0x28 || st == 0x29) {
                if (c == 1 || c == 3) GO(0x28);
                else if (c == 2) GO(st == 0x28 ? 0x29 : st);
                else if (c == 0x10) GO(0x29);
                else if (c == 0x15 && ch == '-') GO(0x28);
                else if (b[cur - 1] == '\'' && b[cur - 2] == '.') { lx->cur--; st = 0x32; }
                else st = -4;
            } else if (st == 0x32) {
                const char *w = b + start;
                uint32_t n2 = (uint32_t)(lx->cur - start);
                if (lex_title(w, n2, 0)) st = -3;
                else if (lex_abbrev((char *)w, n2)) st = -2;
                else if (lex_title(w, n2, 1)) st = -3;
                else { lx->cur--; st = -1; }
            } else if (st == 0x3c) {
                if (c == 3) lx->cur++;
                else if (c == 6) GO(0x3d);
                else if (c == 0xe && (ch == ':' || ch == ',')) GO(0x3d);
                else if (c == 0x15 && ch == '-') GO(0x3d);
                else st = 0x42;
            } else if (st == 0x3d) {
                if (c == 3) GO(0x3c);
                else { lx->cur--; st = 0x42; }
            } else if (st == 0x42) {
                if (c == 1) GO(0xb);
                else if (c == 2) GO(0xa);
                else if (c == 8) { if (ch == '\'') GO(0x28); else st = -1; }
                else st = -4;
            }
#undef GO
            if (st < 0) break;
        }
        int32_t kind;
        if (st == -4) {
            char w3[4];
            memcpy(w3, DLLVAR(const char, 0x63738398), 4);
            if ((uint32_t)(lx->cur + 3) <= (uint32_t)lx->end && !vc_memcmp(b + lx->cur, w3, 3)) lx->cur += 3;
            kind = 3;
        } else kind = st == -3 ? 5 : st == -2 ? 4 : 3;
        span_set(out, lx->buf, kind, start, lx->cur - start, 1);
    }
done:
    lx->pending = *out;
#undef ONE
}

void lex_classes(void) {
    int32_t *cls = DLLVAR(int32_t, LEX_CLASS);
    const uint8_t *cfg = DLLVAR(const uint8_t, 0x637399d8);
    memcpy(cls, DLLVAR(const int32_t, 0x63733828), 0x400);
    if (cfg[0]) cls[cfg[0]] = 0x14;
    if (cfg[1]) cls[cfg[1]] = 0x14;
    if (cfg[2]) cls[cfg[2]] = 0x10;
    if (cfg[3]) cls[cfg[3]] = 0x15;
    if (cfg[4]) cls[cfg[4]] = 0x15;
    if (cfg[5]) cls[cfg[5]] = 0x15;
    if (cfg[6]) cls[cfg[6]] = 0xf;
    uint8_t a = cfg[9], b = cfg[8];
    if (a && b != a) { cls[b] = 0x12; cls[cfg[9]] = 0x13; }
    else cls[b] = 0x11;
    char *spaces = DLLVAR(char, 0x63733818);
    size_t n = strlen(spaces);
    spaces[n] = (char)cfg[0];
    spaces[n + 1] = (char)0xa0;
}
