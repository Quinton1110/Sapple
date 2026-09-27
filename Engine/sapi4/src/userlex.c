// The user lexicon file: <engine directory>\Lex\<user name>.lex, "[MSTTSUSERLEXVER100]" then lines of
// "word<TAB>pronunciation<CR><LF>" in ascending (case-insensitive) order. Each pronunciation is
// converted two ways (the older phone codes and back) only to check it; the entry keeps the text.
#include "fe_init.h"
#include "fe_reader.h"
#include "crt_vc.h"
#include "vcrt.h"
#include <stdio.h>
#include <string.h>

#define CS DLLVAR(void, 0x63738ba8)
#define B(addr) (*DLLVAR(signed char, addr))

int32_t module_dir(char *buf, uint32_t n) {
    if (!vc_GetModuleFileNameA(*DLLVAR(uint32_t, 0x63738b88), buf, n)) return (int32_t)0x8007000e;
    char *last = NULL;
    for (char *p = buf; *p; p++)
        if (*p == '\\') last = p;
    if (last) *last = 0;
    return 0;
}

int32_t lex_word_bad(const char *s) {
    while (*s && (vc_isalpha((signed char)*s) || strchr(DLLVAR(const char, 0x6371c574), (signed char)*s))) s++;
    return *s != 0;
}

uint8_t old_code(const char *s) {
    char c = (char)vc_tolower((signed char)s[0]);
    int32_t idx = 1;
    for (const char *e = DLLVAR(const char, 0x6369e35b); e < DLLVAR(const char, 0x6369e3f7); e += 3, idx++)
        if (*e == c && vc_stricmp(e, s) == 0) return (uint8_t)idx;
    return 0;
}

int32_t old_codes(const char *pron, GPTR(char) *out) {
    if (*DLLVAR(int32_t, 0x6373850c) == 0) {
        static const uint32_t names[11] = { 0x6371c0b0, 0x6371c0ac, 0x6371c0a8, 0x6371c0a4, 0x6371c0a0, 0x6371c09c,
                                            0x6371c098, 0x6371c094, 0x6371c090, 0x6371c08c, 0x6371c088 };
        static const uint32_t dest[11] = { 0x637384f0, 0x637384f4, 0x63738500, 0x63738508, 0x637384f8, 0x637384fc,
                                           0x63738504, 0x637384e4, 0x637384ec, 0x637384e8, 0x637384e0 };
        for (int k = 0; k < 11; k++) {
            uint8_t v = old_code(DLLVAR(const char, names[k]));
            if (k == 10) (*DLLVAR(int32_t, 0x6373850c))++;
            *DLLVAR(uint8_t, dest[k]) = v;
        }
    }
    int32_t flag = 0;
    signed char st = 0;
    char *b = vc_malloc(strlen(pron) * 2 + 2), *o0 = vc_malloc(strlen(pron) * 2 + 2), *o = o0;
    strcpy(b, pron);
    b[strlen(b) + 1] = 0;
    char *s = b;
    if (*s == 0) goto done;
    do {
        if (*s == 0x1b) {                   // an escape: copied through the next blank, marked 0xff
            *o++ = (char)0xff;
            while (*s && *s != ' ') *o++ = *s++;
            char ch = *s;
            *o++ = ch;
            s++;
            if (ch == 0) {
                flag = 1;
                o[-1] = ' ';
                goto done;
            }
            continue;
        }
        if (strchr(DLLVAR(const char, 0x6371c084), *s)) *s = ' ';
        char tmp[3] = { *s, 0, 0 };     // (a third byte, zero: the pair is a string too)
        uint8_t c1 = old_code(tmp);
        char *next = s + 1;
        tmp[1] = *next;
        uint8_t c2 = old_code(tmp);
        if (c1 == 0) {
            if (c2 == 0) { flag = 1; goto done; }
            s = next;
            *o = (char)c2;
        } else if (c2 == 0) {
            *o = (char)c1;
        } else {
            int32_t nh = 0;
            for (char *q = s + 2; *q == 'h' || *q == 'H'; q++) nh++;
            if (nh % 2 != 0) *o = (char)c1;
            else {
                s = next;
                *o = (char)c2;
            }
        }
        signed char ch = (signed char)*o;
        if (st == 0) {
            if (ch == B(0x637384e4) || ch == B(0x637384ec)) st = ch;
        } else if (B(0x637384e8) > ch) {
            st = 0;
        } else if (B(0x637384e0) <= ch) {
            flag = 1;
            goto done;
        }
        s++;
        o++;
    } while (*s);
done:
    *o = 0;
    if (st) flag = 1;
    // merge pairs of codes that the second form writes as one
    const char *src = o0;
    char *dst = b;
    char dl = *src;
    *dst = dl;
    while (dl) {
        dl = *src;
        signed char t = (signed char)dl;
        if ((uint8_t)dl == 0xff) {
            *dst |= dl;
            for (;;) {
                dst++;
                src++;
                if (dl == ' ') break;
                dl = *src;
                *dst = dl;
            }
        } else {
            signed char nx = (signed char)src[1];
            if (nx) {
                signed char m = 0;
                int merge = 0;
                if (t == B(0x637384f0)) {
                    if (nx == B(0x63738500)) m = B(0x637384fc), merge = 1;
                    else if (nx == B(0x63738508)) m = B(0x637384f8), merge = 1;
                } else if (t == B(0x637384f4) && nx == B(0x63738500)) {
                    m = B(0x63738504), merge = 1;
                }
                if (merge) {
                    *dst = (char)m;
                    src++;
                }
            }
            src++;
            dst++;
        }
        dl = *src;
        *dst = dl;
    }
    char *r = vc_malloc(strlen(b) + 1);
    GPSET(*out, r);
    strcpy(r, b);
    vc_free(b);
    vc_free(o0);
    return flag;
}

int32_t lex_pron_convert(const char *src, GPTR(char) *out) {
    char *d = vc_malloc(strlen(src) + 1), *o = d;
    GPSET(*out, d);
    *o = 0;
    char *w = vc_malloc(strlen(src) + 1), *t = w;
    strcpy(w, src);
    for (;;) {
        char *sp = strchr(t, ' ');
        if (!sp) break;
        *sp = 0;
        char *next = sp + 1;
        if (vc_stricmp(DLLVAR(const char, 0x6371c0c4), t) == 0) {
            strcpy(o, DLLVAR(const char, 0x6371c0c0));
            o += 2;
        } else if (vc_stricmp(DLLVAR(const char, 0x6371c0bc), t) == 0) {
            strcpy(o, DLLVAR(const char, 0x6371c0b8));
            o += 2;
        } else {
            if (sp[-1] == '1') *o++ = '1';
            if (sp[-1] == '1' || sp[-1] == '0') sp[-1] = 0;
            char ch;
            do {
                ch = (char)vc_tolower((signed char)*t++);
                *o++ = ch;
            } while (ch);
            o--;
        }
        t = next;
    }
    vc_free(w);
    return 0;
}

int32_t lex_pron_names(char *s0, GPTR(char) *out) {
    GPTR(char) sg;
    int32_t r = old_codes(s0, &sg);
    char *s = GP(char, sg);
    char *d = vc_malloc(strlen(s) * 4 + 1), *o = d;
    GPSET(*out, d);
    signed char st = 0;
    const char *tab = DLLVAR(const char, 0x6369e3f8);
    for (const char *p = s; *p; p++) {
        signed char al = (signed char)*p;
        if (al == B(0x637384e4) || al == B(0x637384ec)) {
            st = al;
            continue;
        }
        strcpy(o, tab + 3 * al);
        o += strlen(tab + 3 * al);
        if (B(0x637384e8) > al) {
            if (st == 0) *o++ = '0';
            else {
                strcpy(o, tab + 3 * st);
                o += strlen(tab + 3 * st);
                st = 0;
            }
        }
        *o++ = ' ';
    }
    *o = 0;
    vc_free(s);
    return r;
}

int32_t free_if_a(void *p) {
    if (p) vc_free(p);
    return 0;
}
int32_t free_if_b(void *p) {
    if (p) vc_free(p);
    return 0;
}

int32_t user_lex_load(UserLex *t) {
    char dir[0x100], path[0x100], user[0x104];
    int32_t r = module_dir(dir, 0x100);
    if (r < 0) return r;
    uint32_t n = 0x101;
    if (!vc_GetUserNameA(user, &n)) return (int32_t)0x80004005;
    sprintf(path, DLLVAR(const char, 0x6371c578), dir, user);        // "%s\Lex\%s.lex"
    uint32_t h = vc_CreateFileA(path, 0x80000000u, 1, 3, 0x80);
    if (h == 0xffffffffu) {
        uint32_t e = vc_GetLastError();
        if (e == 3 || e == 2) return 0;
        return (int32_t)e;
    }
    uint32_t size = vc_GetFileSize(h);
    if (size == 0xffffffffu) return (int32_t)0x80004005;
    size++;
    char *buf = vc_malloc(size);
    if (!buf) return (int32_t)0x8007000e;
    uint32_t got;
    if (!vc_ReadFile(h, buf, size - 1, &got)) {
        uint32_t e = vc_GetLastError();
        vc_free(buf);
        vc_CloseHandle(h);
        return (int32_t)e;
    }
    vc_CloseHandle(h);
    const char *hdr = DLLVAR(const char, 0x6369e650);       // "[MSTTSUSERLEXVER100]\r\n"
    if (size - 1 != got) goto bad_file;
    buf[size - 1] = 0;
    if (strncmp(hdr, buf, strlen(hdr)) != 0) goto bad_file;
    int32_t lines = -1;
    for (char *p = strchr(buf, '\r'); p; p = strchr(p + 1, '\r')) lines++;
    char *s = buf + strlen(hdr);
    t->_0c = 0;
    t->count = lines;
    t->cap = lines;
    GPTR(LexEntry) *ents = vc_realloc(GP(void, t->entries), (size_t)(uint32_t)lines * sizeof(GPTR(LexEntry)));
    GPSET(t->entries, ents);
    if (!ents) goto oom;
    const char *prev = DLLVAR(const char, 0x6373851c);
    int32_t i = 0;
    LexEntry *e = NULL;
    for (; i < t->count; i++) {
        e = vc_malloc(sizeof(LexEntry));
        if (!e) goto oom;
        memset(e, 0, sizeof *e);
        char *tab = strchr(s, '\t');
        if (!tab) goto check;
        size_t l = (size_t)(tab - s);
        char *w = vc_malloc(l + 1);
        GPSET(e->word, w);
        if (!w) goto oom;
        memcpy(w, s, l);
        w[l] = 0;
        if (lex_word_bad(w)) goto check;
        if (vc_stricmp(prev, w) >= 0) goto check;
        prev = w;
        s = tab + 1;
        char *cr = strchr(s, '\r');
        if (!cr || cr[1] != '\n') goto check;
        l = (size_t)(cr - s);
        char *pr = vc_malloc(l + 1);
        GPSET(e->pron, pr);
        if (!pr) goto oom;
        memcpy(pr, s, l);
        pr[l] = 0;
        GPTR(char) t1;
        GPTR(char) t2;
        lex_pron_convert(pr, &t1);
        int32_t bad = lex_pron_names(GP(char, t1), &t2);
        free_if_a(GPN(char, t1));
        free_if_b(GPN(char, t2));
        if (bad) goto check;
        s = cr + 2;
        GPSET(GP(GPTR(LexEntry), t->entries)[i], e);
        e = NULL;
    }
check:
    if (i >= t->count && *s == 0) {
        vc_free(buf);
        return 0;
    }
    if (e) {
        vc_free(GPN(char, e->word));
        vc_free(GPN(char, e->pron));
        vc_free(e);
    }
    while (--i >= 0) {
        LexEntry *x = GP(LexEntry, GP(GPTR(LexEntry), t->entries)[i]);
        vc_free(GPN(char, x->word));
        vc_free(GPN(char, x->pron));
        vc_free(x);
    }
    t->count = 0;
    GPSET(t->entries, (GPTR(LexEntry) *)vc_realloc(GP(void, t->entries), (size_t)(uint32_t)t->cap * sizeof(GPTR(LexEntry))));
    vc_free(buf);
    return (int32_t)0x80004005;
bad_file:
    vc_free(buf);
    return (int32_t)0x80004005;
oom:
    vc_free(buf);
    return (int32_t)0x8007000e;
}
