#include "fe_phrase.h"
#include "fe_vm.h"
#include "fe_word.h"
#include "crt_vc.h"
#include <string.h>

#define WOBJ GP(uint8_t, *DLLPTR(uint8_t, 0x63739274))
#define TA GP(uint8_t, *DLLPTR(uint8_t, 0x63739268))
#define T2 GP(uint8_t, *DLLPTR(uint8_t, 0x63738ca8))
#define MARKS DLLVAR(uint8_t, 0x63738cf8)
#define CLS(c) DLLVAR(const uint8_t, 0x636a0322)[(uint8_t)(c)]
#define FLG(c) DLLVAR(const uint8_t, 0x636a03ca)[(uint8_t)(c)]
#define G32(a) (*DLLVAR(int32_t, a))
#define G16(a) (*DLLVAR(int16_t, a))
#define GU16(a) (*DLLVAR(uint16_t, a))
#define G_CC0 0x63738cc0            // int16: the attack part of the current segment
#define G_CC4 0x63738cc4            // int16: pitch base
#define G_CC8 0x63738cc8            // int16: the steady part
#define G_CCC 0x63738ccc            // uint16: time so far
#define G_CEC 0x63738cec
#define G_9280 0x63739280           // int16: the release part
#define G_92E4 0x637392e4
#define G_9270 0x63739270


// ---- the phrase event list

LAYOUT(EvNode, next, 0x18);
#define EV_BLOCK (32 * sizeof(EvNode))

EvNode *ev_alloc(void) {
    GPTR(EvNode) *fl = DLLPTR(EvNode, 0x63738c9c);
    EvNode *n;
    if (*fl) {
        n = GP(EvNode, *fl);
        *fl = n->next;
        GPSET(n->next, (EvNode *)NULL);
    } else {
        uint8_t *b = vc_malloc(EV_BLOCK + sizeof(GPTR(uint8_t)));
        if (!b) return NULL;
        GPTR(uint8_t) *blocks = DLLPTR(uint8_t, 0x63738ca0);
        GPTR(uint8_t) *link = (GPTR(uint8_t) *)(void *)(b + EV_BLOCK);
        *link = *blocks;
        GPSET(*blocks, b);
        EvNode *nodes = (EvNode *)(void *)b;
        GPSET(nodes[0].next, (EvNode *)NULL);
        for (int k = 1; k <= 30; k++) GPSET(nodes[k].next, &nodes[k + 1]);
        nodes[31].next = *fl;
        GPSET(*fl, &nodes[1]);
        n = &nodes[0];
    }
    n->kind = 0;
    n->len = 0;
    n->pos = 0;
    n->b = 0;
    n->a = 0;
    return n;
}

void ev_clear(uint8_t *w) {
    while (WPTR(w, 0xb4ac)) {
        EvNode *h = GP(EvNode, WPTR(w, 0xb4ac));
        WPTR(w, 0xb4ac) = h->next;
        freelist_push((FreeNode *)(void *)h);
    }
    GPSET(WPTR(w, 0xb4ac), (EvNode *)NULL);
    GPSET(WPTR(w, 0xb4b0), (EvNode *)NULL);
    W32(w, 0xb4b4) = 0;
    memset(w + 5, 0, 0x500);
}

// the original reads last2->pos even when there is no kind-2 event yet: address 0x0c, which the
// hook build reads from guest memory as the original does (a portable build reads 0)
static int32_t pos_of(GPTR(EvNode) n) {
    if (!n) {
#ifdef DECOMP_HOOK
        int32_t v;
        memcpy(&v, decomp_guest_mem + 0xc, 4);
        return v;
#else
        return 0;
#endif
    }
    return GP(EvNode, n)->pos;
}

// (list links are kept as the stored values: a null link is 0 in both builds)
int32_t ev_place(uint8_t *w) {
    int32_t hi = W16(w, 2), s = W16(w, 0);
    EvLink ed = WPTR(w, 0xb4ac);
    EvLink first = 0, prev = 0, last1 = 0, last2 = 0;
    int32_t cnt = 0, acc = 0;
    if (ed) {
        if (GP(EvNode, ed)->kind == 1) {
            if (w[s + 4] == 0) {
                int32_t e = s;
                while (e <= hi && w[e + 4] == 0) e++;
                w[s + 4] = 1;
                w[e + 4]--;
            }
        } else if (w[0x584 + s] == '#' && w[s + 5]) {
            w[s + 4] = (uint8_t)(w[s + 4] + w[s + 5]);
            w[s + 5] = 0;
        }
    }
    for (int32_t i = s - 1; i < hi; i++) {
        uint8_t *pc = &w[i + 5];
        int16_t *pd = (int16_t *)(void *)(w + 0x1b8c + 2 * i);
        if (*pc) {
            do {
                if (!ed) break;
                if (cnt) GP(EvNode, prev)->next = ed;
                else first = ed;
                cnt++;
                EvLink cur = ed;
                EvNode *c = GP(EvNode, cur);
                ed = c->next;
                prev = cur;
                c->pos = acc;
                if (c->kind == 1) {
                    if (last1) GP(EvNode, last1)->len = acc - pos_of(last2);
                    last1 = cur;
                }
                if (c->kind == 2) {
                    if (last2) GP(EvNode, last2)->len = acc - GP(EvNode, last2)->pos;
                    last2 = cur;
                }
            } while (--*pc != 0);
        }
        if ((w[0xb4b8] & 4) && w[0x585 + i] != '-') {
            EvNode *n = ev_alloc();
            if (!n) {
                WPTR(w, 0xb4ac) = first;
                GP(EvNode, prev)->next = ed;
                ev_clear(w);
                return -1;
            }
            n->kind = 4;
            n->ch = w[0x585 + i];
            n->pos = acc;
            n->len = *pd;
            EvLink nn;
            GPSET(nn, n);
            if (cnt) GP(EvNode, prev)->next = nn;
            else first = nn;
            cnt++;
            prev = nn;
        }
        acc += *pd;
    }
    while (ed) {
        EvNode *x = GP(EvNode, ed);
        ed = x->next;
        freelist_push((FreeNode *)(void *)x);
    }
    if (cnt) {
        if (last1) { GP(EvNode, last1)->pos = 0; GP(EvNode, last1)->len = W32(w, 0xb580); }
        if (last2) GP(EvNode, last2)->len = acc - GP(EvNode, last2)->pos;
        GP(EvNode, prev)->next = 0;
        WPTR(w, 0xb4ac) = first;
        W32(w, 0xb4b4) = cnt;
    }
    return cnt;
}

// ---- marks, factors and durations

uint8_t ph_near_dash(const uint8_t *t, int32_t lo, int32_t hi, int32_t pos) {
    int32_t e = pos;
    if (pos < hi)
        for (;;) {
            uint8_t c = t[e - 1];
            if (c == '#' || c == ' ') break;
            uint8_t nx = t[e];
            e++;
            if (nx == '-') return 1;
            if (!(e < hi)) break;
        }
    int32_t d = pos;
    if (pos > lo)
        for (;;) {
            uint8_t c = t[d - 1];
            if (c == '#' || c == ' ') break;
            uint8_t pv = t[d - 2];
            d--;
            if (pv == '-') return 1;
            if (!(d > lo)) break;
        }
    return 0;
}

void ph_stress_marks(const uint8_t *t0, uint8_t *t2, int16_t lo, int16_t hi, uint8_t flag) {
    uint8_t bl = ' ';
    for (int32_t i = lo; i <= hi; i++) {
        uint8_t c = t0[i - 1];
        if (CLS(c) >= 0x61)
            switch (c) {
            case 0x20: case 0x2d: bl = ' '; break;
            case 0x22: bl = 'Z'; break;
            case 0x27: bl = 'P'; break;
            case 0x60: bl = 'S'; break;
            }
        if (FLG(t2[i - 1]) >= 0x80 && flag) {
            if (bl == 'P' && ph_near_dash(t0, lo, hi, i)) t2[i - 1] = 'S';
            else t2[i - 1] = bl == 'Z' ? 'Z' : 'F';
        } else t2[i - 1] = bl;
        if (CLS(t0[i - 1]) == 'V') bl = ' ';
    }
}

void ph_marks(int16_t lo, int16_t hi, uint8_t flag) {
    G32(0x63739e84) = 0;
    if (G32(0x63738c98) == 1) { G32(0x63739e80) = 0x6b; G32(0x63739e88) = 0x78; }
    else G32(0x63739e80) = G32(0x63739e88) = 0x64;
    uint8_t *m = MARKS;
    memset(m, ' ', 0x500);
    int32_t cnt = 0, last = -1;
    const uint8_t *t = TA;
    if (flag) {
        uint8_t bl = ' ';
        for (int32_t i = hi; i >= lo; i--) {
            uint8_t c = t[i - 1];
            if (CLS(c) == 0x73) {
                if (c == ' ') {
                    cnt = 0;
                    const uint8_t *w = WOBJ;
                    int32_t k = i + 1;
                    uint8_t b3 = w[0x1604 + i + 1];
                    if (b3 == '~' || b3 == ' ') k = i + 2;
                    bl = w[0x1604 + k] != '~' ? 'P' : 'F';
                } else if (c == '#') {
                    cnt = 0;
                    bl = '#';
                } else if (c == '-') {
                    if (cnt < 2) cnt++;
                    bl = ' ';
                    last = i;
                    continue;
                } else {
                    m[i - 1] = bl;
                    continue;
                }
                if (last != -1 && i < last) memset(m + i, 'I', (size_t)(last - i));
                last = -1;
                continue;
            }
            m[i - 1] = bl;
        }
    } else {
        uint8_t val = 'F';
        for (int32_t i = hi; i >= lo; i--) {
            uint8_t c = t[i - 1];
            if (CLS(c) == 0x73) {
                if (c == ' ' || c == '#') {
                    cnt = 0;
                    val = 'F';
                    if (last != -1 && i < last) memset(m + i, 'I', (size_t)(last - i));
                    last = -1;
                } else if (c == '-') {
                    if (cnt < 3) cnt++;
                    val = 'M';
                    last = i;
                }
            }
            m[i - 1] = val;
        }
    }
}

uint8_t ph_vowel_factor(int32_t pos, int32_t *out) {
    const uint8_t *t = TA;
    int32_t e = pos;
    while (t[e - 1] == '-' || t[e - 1] == '~') e++;
    uint8_t c = t[e - 1];
    int32_t v;
    if (FLG(c) & 1) {
        uint8_t n;
        do { n = t[e]; e++; } while (n == '-' || n == '~');
        v = DLLVAR(const uint8_t, 0x636a0662)[t[e - 1]];
    } else v = DLLVAR(const uint8_t, 0x636a070a)[c];
    *out = v;
    return v != 0;
}

void ph_final_factor(int32_t pos, int32_t *out) {
    const uint8_t *t = TA;
    *out = 0x46;
    uint8_t c = t[pos - 1];
    if ((CLS(c) == 'V' || c == 0x4c || c == 0xa4) && MARKS[pos - 1] == ' ') *out = 0x32;
    c = t[pos - 1];
    if ((FLG(c) & 1) && c != 'h' && CLS(t[pos]) == 'V') *out = 0x28;
}

uint8_t ph_cluster_factor(int32_t pos, int32_t *out) {
    const uint8_t *t = TA;
    const uint8_t *m = MARKS;
    *out = 0x64;
    int32_t e = pos + 1;
    uint8_t bl = 0;
    while (t[e - 1] == '~' || t[e - 1] == '-') e++;
    uint8_t dl = t[e - 1];
    if (dl == ' ') bl = m[pos - 1] == '#' || m[pos - 1] == 'P';
    else if (dl == '#') bl = 1;
    while (FLG(dl) == 8) { dl = t[e]; e++; }
    uint8_t c1 = CLS(t[pos - 1]);
    if (c1 == 'V' && CLS(t[e - 1]) == c1) *out = 0x78;
    else if (c1 <= 0x51 && CLS(t[e - 1]) <= 0x51 && !bl) *out = 0x46;
    e = pos - 1;
    bl = 0;
    while ((t[e - 1] == '~' || t[e - 1] == '-') && e > 2) e--;
    dl = t[e - 1];
    if (dl == ' ' || dl == '=') bl = m[e - 2] == '#' || m[e - 2] == 'P';
    else if (dl == '#') bl = 1;
    while (FLG(dl) == 8) { dl = t[e - 2]; e--; }
    uint8_t c2 = CLS(t[e - 1]);
    if (c2 == 'V' && CLS(t[pos - 1]) == c2) { *out = 0x46; return 1; }
    if (c2 <= 0x51 && CLS(t[pos - 1]) <= 0x51 && !bl) {
        *out = *out == 0x64 ? 0x46 : 0x32;
        return 1;
    }
    return *out != 0x64;
}

void ph_split(int32_t pos) {
    uint8_t *w = WOBJ;
    if (TA[pos - 1] == '#') {
        G16(G_CC0) = 0;
        G16(G_9280) = 0;
        G16(G_CC8) = W16(w, 0x9916 + 2 * pos);
        return;
    }
    int16_t dx = (int16_t)(W16(w, 0x9916 + 2 * pos) >> 1);
    uint8_t *b = &w[0xa317 + pos];
    if ((int16_t)*b > dx) *b = (uint8_t)dx;
    int32_t v = ((int32_t)W16(w, 0x9916 + 2 * pos) - *b) >> 2;
    int16_t ax = (int16_t)v;
    G16(G_CC0) = ax;
    if (ax == 0) { ax = 1; G16(G_CC0) = 1; }
    G16(G_9280) = *b ? (int16_t)*b : ax;
    G16(G_CC8) = (int16_t)(W16(w, 0x9916 + 2 * pos) - ax - G16(G_9280));
}

#define DIV(a, b) ((int32_t)(a) / (int32_t)(b))

void ph_durations(int16_t lo, int16_t hi, uint8_t flag) {
    for (int32_t i = lo - 1; i < hi; i++) {
        uint8_t *w = WOBJ;
        int16_t *p = (int16_t *)(void *)(w + 0x1b8c + 2 * i);
        int16_t d = *p;
        if (d > 0xbb8) { *p = (int16_t)(d - 0xbb8); continue; }
        const uint8_t *t = TA;
        const uint8_t *m = MARKS;
        int32_t l14 = 100, l18 = 100, l1c, l24;
        uint8_t dl = t[i];
        if (dl == '#') {
            if (i + 1 == lo || i + 1 == hi) *p = 0xf;
            else {
                uint8_t c3 = w[0x1605 + i];
                *p = c3 == '.' ? 0x190 : c3 != ',' ? 0x96 : 0xc8;
            }
            goto add;
        }
        if (dl == '?') { *p = t[i - 1] != '#' ? 0x46 : 0xf; goto add; }
        if (FLG(dl) == 8) goto add;
        if ((dl == 'i' || dl == 'U' || dl == 'I') && t[i + 1] == 'r') { l24 = 0xb4; l1c = 0x64; }
        else { l24 = DLLVAR(const int16_t, 0x636a0464)[dl]; l1c = DLLVAR(const uint8_t, 0x636a05ba)[dl]; }
        if (m[i] == '#') {
            if (CLS(dl) == 'V' || dl == 0x4c || dl == 0xa4) l14 = 0xb4;
            else if (i > 1) {
                int32_t k = i;
                if (m[i - 1] == '#')
                    for (;;) {
                        if (CLS(t[k - 1]) > 0x51) break;
                        k--;
                        if (m[k - 1] != '#') break;
                    }
                if (m[k - 1] == '#' && CLS(t[k - 1]) == 'V') l14 = 0xb4;
            }
            if (l14 != 100) l18 = DIV(l14 * 100, 100);
        }
        uint8_t c = t[i], l13 = CLS(c);
        if (m[i] != '#' && m[i] != 'P') {
            if (l13 == 'V' || c == 0x4c || c == 0xa4) l18 = DIV(l18 * 3, 5);
        } else if (l13 == 0x4c && CLS(t[i - 1]) == 'V') l18 = DIV(l18 * 7, 5);
        int vow = l13 == 'V' || c == 0x4c || c == 0xa4;
        if (vow && m[i] != 'F' && m[i] != '#' && m[i] != 'P') l18 = DIV(l18 * 0x11, 0x14);
        int32_t eb = l18;
        if (vow && ph_near_dash(TA, lo, hi, i + 1)) eb = DIV(l18 * 4, 5);
        t = TA;
        if (CLS(t[i]) <= 0x51 && m[i] != 'I') eb = DIV(eb * 0x11, 0x14);
        w = WOBJ;
        uint8_t a2 = w[0x1085 + i];
        if (a2 == ' ' || a2 == 'F' || a2 == 'S') {
            if (a2 != 'S') l1c = DIV(l1c, 2);
            ph_final_factor(i + 1, &l14);
            eb = DIV(l14 * eb, 100);
        }
        c = TA[i];
        if (CLS(c) == 'V' || (FLG(c) & 1)) {
            if (ph_vowel_factor(i + 2, &l14)) {
                if (CLS(TA[i]) == 'V' && m[i] != '#' && m[i] != 'P') l14 = DIV(l14 * 3, 10) + 0x46;
                eb = DIV(l14 * eb, 100);
            }
        }
        if (ph_cluster_factor(i + 1, &l14)) eb = DIV(l14 * eb, 100);
        w = WOBJ;
        p = (int16_t *)(void *)(w + 0x1b8c + 2 * i);
        *p = (int16_t)(DIV((l24 - l1c) * eb, 100) + l1c);
        if (*p < l1c) *p = (int16_t)l1c;
        t = TA;
        if (CLS(t[i]) <= 0x51 && w[0xb05 + i] == '+') *p = (int16_t)DIV(*p * 18, 10);
        dl = t[i];
        if (FLG(dl) == 0x10 && (t[i + 1] == '#' || t[i + 1] == 0x19)) *p = (int16_t)DIV(dl == 'z' ? *p * 16 : *p * 15, 10);
        c = t[i];
        if ((CLS(c) == 'P' || CLS(c) == 'Q') && (t[i + 1] == '#' || t[i + 1] == 0x19)) {
            int16_t mn = 0;
            switch (c) {
            case 'b': mn = 0x9b; break;
            case 'd': mn = 0xa5; break;
            case 'g': mn = 0x82; break;
            case 'k': case 'p': mn = 0xb4; break;
            case 't': mn = 0x8c; break;
            }
            if (mn && *p < mn) *p = mn;
        }
        if (w[0xb05 + i] == '*') *p = (int16_t)DIV(*p * 13, 10);
        if (!flag) {
            *p = (int16_t)DIV(*p * 11, 10);
            if (t[i - 1] == '#') {
                if (t[i] == 'm') *p = 0x4b;
                else if (t[i] == 'l' && t[i + 1] != 'A') *p = 0x82;
            }
            *p = (int16_t)DIV(*p * 100, 100);
        } else {
            switch (w[0x1085 + i]) {
            case ' ': case 'F':
                if (m[i] != '#') *p = (int16_t)DIV(*p * 100, 100);
                else if (t[i] == '$') *p = (int16_t)DIV(*p * 0x4b, 100);
                break;
            case 'P': case 'Z': {
                uint8_t c4 = w[0x438e + i];
                int f = c4 == 0x31 || c4 == 0x46 || c4 == 0x48 || c4 == 0x63 || c4 == 0x66 || c4 == 0x6c;
                *p = (int16_t)DIV(*p * (f ? 0x5a : 0x55), 100);
                break;
            }
            case 'S': *p = (int16_t)DIV(*p * 0x5f, 100); break;
            }
            if (m[i] == 'P' || m[i] == '#') *p = (int16_t)DIV(*p * G32(0x63739e88), 100);
            *p = (int16_t)DIV(*p * G32(0x63739e80), 100);
            if (TA[i] == 0x19) { TA[i] = '#'; *p = 0; }
        }
        *p = (int16_t)DIV((int32_t)W16(w, 0xb58a) * *p, 32);
    add:
        *p = (int16_t)(*p + d);
    }
}

void ph_segments(int16_t lo, int16_t hi, uint8_t flag) {
    uint8_t *w = WOBJ;
    W16(w, 0x5d14) = 0;
    W16(w, 0x9916) = 0;
    uint16_t si = 1;
    int32_t rate = DIV((int32_t)(flag ? W16(w, 0xb58a) : W16(w, 0xb588)) * 10, 8);
    int16_t r16 = (int16_t)rate;
    int16_t first = (int16_t)(lo - 1);
    if (first < hi) {
        int32_t i = first, pos = i + 1;
        for (int32_t cnt = hi - i; cnt; cnt--, i++, pos++) {
            w = WOBJ;
            if (W16(w, 0x9918 + 2 * i) == 0) continue;
            int16_t *p = (int16_t *)(void *)(w + 0x1b8c + 2 * i);
            if (*p <= 0) *p = 1;
            ph_split(pos);
            w = WOBJ;
            p = (int16_t *)(void *)(w + 0x1b8c + 2 * i);
            int16_t a10 = (int16_t)(*p * 10);
            int32_t ebx = 0;
            int32_t side = G16(G_CC0) + G16(G_9280);
            if (G16(G_CC8) > 0) ebx = DIV(a10 - side * r16, G16(G_CC8));
            if ((int16_t)ebx < G32(0x63739e84)) {
                ebx = G32(0x63739e84);
                *p = (int16_t)DIV(side * r16 + (int16_t)ebx * G16(G_CC8) + 5, 10);
            }
            int16_t di = W16(w, 0x9916);
            if (di < 0xefc) {
                int32_t edx = 0;
                if (G16(G_CC0) > 0) {
                    W16(w, 0x5d16 + 4 * di) = (int16_t)si;
                    si = (uint16_t)(si + G16(G_CC0));
                    edx = 1;
                    W16(w, 0x5d18 + 4 * W16(w, 0x9916)) = r16;
                }
                if (G16(G_CC8) > 0) {
                    edx++;
                    int32_t k = W16(w, 0x9916) + edx;
                    W16(w, 0x5d12 + 4 * k) = (int16_t)si;
                    si = (uint16_t)(si + G16(G_CC8));
                    W16(w, 0x5d14 + 4 * (W16(w, 0x9916) + edx)) = (int16_t)ebx;
                }
                if (G16(G_9280) > 0) {
                    edx++;
                    int32_t k = W16(w, 0x9916) + edx;
                    W16(w, 0x5d12 + 4 * k) = (int16_t)si;
                    si = (uint16_t)(si + G16(G_9280));
                    W16(w, 0x5d14 + 4 * (W16(w, 0x9916) + edx)) = r16;
                }
                W16(w, 0x9916) = (int16_t)(W16(w, 0x9916) + edx);
            } else {
                int32_t dx = DIV((int32_t)W16(w, 0xb58a) * 10, 8);
                if (di < 0xeff) {
                    di++;
                    W16(w, 0x9916) = di;
                    si = 0x7d00;
                    W16(w, 0x5d12 + 4 * di) = 0x7d00;
                    W16(w, 0x5d14 + 4 * di) = (int16_t)dx;
                }
                *p = (int16_t)DIV((int32_t)W16(w, 0x9918 + 2 * i) * (int16_t)dx, 10);
            }
        }
    }
    w = WOBJ;
    int32_t n = W16(w, 0x9916);
    si++;
    W16(w, 0x5d16 + 4 * n) = (int16_t)si;
    W16(w, 0x5d18 + 4 * n) = r16;
}

// ---- pitch targets: entries of 6 bytes (kind, frequency, time) from word object + 0x4908, the index
// of the last one at + 0x580e

#define EK(w, n) (*(uint8_t *)((w) + 0x4908 + 6 * (n)))
#define EF(w, n) (*(int16_t *)(void *)((w) + 0x490a + 6 * (n)))
#define ET(w, n) (*(uint16_t *)(void *)((w) + 0x490c + 6 * (n)))
#define NCNT(w) W16(w, 0x580e)

void ph_target(int32_t k, int32_t t) {
    uint8_t *w = WOBJ;
    const uint8_t *r = DLLVAR(const uint8_t, 0x63721d18 + 16u * (uint32_t)k);
    uint8_t kind = r[0];
    int16_t f_add, f_off, toff, gadd;
    uint16_t maxhalf;
    int32_t mode;
    memcpy(&f_add, r + 2, 2); memcpy(&f_off, r + 4, 2); memcpy(&maxhalf, r + 6, 2);
    memcpy(&mode, r + 8, 4); memcpy(&toff, r + 0xc, 2); memcpy(&gadd, r + 0xe, 2);
    int16_t n = NCNT(w);
    if (n >= 0x26c) return;
    uint16_t tlast = ET(w, n);
    int32_t edi, time, half;
    if (mode == 1) edi = (int32_t)GU16(G_CCC) + t + toff;
    else edi = toff + (int32_t)GU16(G_CCC);
    if (edi < 0) {
        half = -edi;
        time = 0;
    } else {
        time = edi;
        if (tlast > (uint16_t)edi) {
            half = DIV((int32_t)tlast - (uint16_t)edi, 2);
            if (maxhalf < (uint16_t)half) return;
            time = half + edi;
            if (n >= 2 && (uint16_t)time < ET(w, n - 1)) return;
            ET(w, n) = (uint16_t)time;
        } else half = 0;
    }
    n = NCNT(w);
    uint16_t told = ET(w, n);
    if ((uint16_t)time > told) {
        int32_t slope = DIV((int32_t)EF(w, n) - G16(G_CC4) - f_off, (int16_t)(time - told));
        uint16_t tl = tlast;
        int16_t di = G16(G_CC4);
        if ((int32_t)tl + 0x9c4 < (int32_t)(uint16_t)time) {
            do {
                NCNT(w)++;
                tl = (uint16_t)(tl + 0x960);
                int32_t m = NCNT(w);
                ET(w, m) = tl;
                uint16_t dt = (uint16_t)(ET(w, m) - told);
                EF(w, m) = (int16_t)(EF(w, m - 1) - (int16_t)((uint32_t)dt * (uint32_t)slope));
                EK(w, m) = '0';
            } while ((int32_t)tl + 0x9c4 < (int32_t)(uint16_t)time);
            di = G16(G_CC4);
        }
        int32_t m = NCNT(w);
        EF(w, m + 1) = (int16_t)(di + f_off);
        ET(w, m + 1) = (uint16_t)time;
        EK(w, m + 1) = kind;
        NCNT(w)++;
    } else EK(w, n) = kind;
    G16(G_CC4) = (int16_t)(G16(G_CC4) + gadd);
    int32_t m = NCNT(w);
    EF(w, m + 1) = (int16_t)(f_add + f_off + G16(G_CC4));
    ET(w, m + 1) = (uint16_t)(maxhalf - (uint16_t)half + time);
    EK(w, m + 1) = ' ';
    NCNT(w)++;
}

void ph_insert_tag(int32_t k, int32_t j) {
    uint8_t *w = WOBJ;
    int32_t c = NCNT(w) + 1;
    if (c >= k) memmove(&EK(w, k + 1), &EK(w, k), (size_t)(6 * (c - k + 1)));
    w = WOBJ;
    EF(w, k) = W16(w, 0x2f88 + 8 * j);
    ET(w, k) = (uint16_t)W16(w, 0x2f84 + 8 * j);
    EK(w, k) = '*';
    NCNT(w)++;
}

void ph_pitch(uint8_t *w, int32_t lo, int32_t hi) {
    int32_t mode = 4, j = 0;
    GPSET(*DLLPTR(uint8_t, 0x63739274), w);
    G16(G_CC4) = 0;
    G16(G_CEC) = 0;
    int16_t dx = (int16_t)(lo - 1);
    if (dx < (int16_t)hi) {
        for (int32_t i = dx, n = (int16_t)hi - i; n; n--, i++) {
            int16_t bx = W16(w, 0x258c + 2 * i);
            if (bx != 0) {
                if ((int16_t)j < bx) {
                    for (int16_t di = (int16_t)j; di < W16(w, 0x258c + 2 * i); di++)
                        W32(w, 0x2f8c + 8 * di) += G16(G_CEC);
                }
                j = (uint16_t)W16(w, 0x258c + 2 * i);
            }
            G16(G_CEC) = (int16_t)(G16(G_CEC) + W16(w, 0x1b8c + 2 * i));
        }
    }
    GU16(G_CCC) = 0;
    NCNT(w) = 1;
    EF(w, 1) = 0;
    ET(w, 1) = 0;
    EK(w, 1) = '0';
    if (dx < (int16_t)hi) {
        for (int32_t i = dx, n = (int16_t)hi - i; n; n--, i++) {
            int32_t d = W16(w, 0x1b8c + 2 * i);
            switch (w[0x438e + i]) {
            case 0x28: ph_target(0x13, d); break;
            case 0x31: ph_target(0, d); break;
            case 0x29: ph_target(0x14, d); break;
            case 'W': mode = 3; break;
            case 'F': ph_target(0x12, d); break;
            case '7': ph_target(0xa, d); ph_target(5, W16(w, 0x1b8c + 2 * i)); break;
            case '2': ph_target(4, d); break;
            case '3': ph_target(2, d); break;
            case '4': ph_target(3, d); break;
            case '5': ph_target(1, d); break;
            case '8': ph_target(5, d); break;
            case '9': ph_target(6, d); break;
            case 'A': ph_target(0xd, d); break;
            case 'B': ph_target(0x11, d); break;
            case 'P': ph_target(0, d); ph_target(9, W16(w, 0x1b8c + 2 * i)); break;
            case 'H': ph_target(4, d); ph_target(0xf, W16(w, 0x1b8c + 2 * i)); break;
            case 'I': ph_target(3, d); ph_target(0xf, W16(w, 0x1b8c + 2 * i)); break;
            case 'L': ph_target(0x1e, d); break;
            case 'O': ph_target(0x1d, d); break;
            case 'S': mode = 0; break;
            case 'T': ph_target(0x18, d); break;
            case 'U': ph_target(0x1a, d); break;
            case 'h': ph_target(1, d); ph_target(9, W16(w, 0x1b8c + 2 * i)); break;
            case 'a': ph_target(0xa, d); break;
            case 'X': ph_target(0x1f, d); break;
            case 'Y': ph_target(0x16, d); break;
            case 'Z': ph_target(0x20, d); break;
            case ']': ph_target(0x15, d); break;
            case 'b': ph_target(0x10, d); break;
            case 'c': ph_target(8, d); break;
            case 'f': ph_target(0, d); ph_target(0x12, W16(w, 0x1b8c + 2 * i)); break;
            case 'p': ph_target(2, d); ph_target(0xe, W16(w, 0x1b8c + 2 * i)); break;
            case 'j': ph_target(7, d); break;
            case 'l': ph_target(0xb, d); break;
            case 'q': mode = 2; break;
            case 't': ph_target(0x17, d); break;
            case 'u': ph_target(0x19, d); break;
            case 'x': ph_target(0x1b, d); break;
            case 'z': ph_target(0x1c, d); break;
            }
            GU16(G_CCC) = (uint16_t)(GU16(G_CCC) + W16(w, 0x1b8c + 2 * i));
        }
    }
    for (int16_t bx = 1; bx <= W16(w, 0x438c); bx++) {
        int32_t v = W32(w, 0x2f84 + 8 * bx);
        int16_t si = 0;
        do si++; while (!(v < ET(w, si)) && si <= NCNT(w));
        ph_insert_tag(si, bx);
    }
    uint16_t cc = GU16(G_CCC);
    while ((int32_t)cc - ET(w, NCNT(w)) > 0xc1c) {
        int32_t n = NCNT(w);
        EF(w, n + 1) = EF(w, n);
        ET(w, n + 1) = (uint16_t)(ET(w, n) + 0xbb8);
        EK(w, n + 1) = EK(w, n) == '*' ? '*' : '0';
        NCNT(w)++;
        cc = GU16(G_CCC);
    }
    if (cc > ET(w, NCNT(w))) {
        int32_t n = NCNT(w);
        ET(w, n + 1) = cc;
        EF(w, n + 1) = EF(w, n);
        EK(w, n + 1) = EK(w, n) == '*' ? '*' : '0';
        NCNT(w)++;
    }
    // the mean segment duration, halved until it is at most 512 (D counts the halvings as a power of 2)
    uint16_t si = 0;
    G16(G_CEC) = 0;
    int16_t lo16 = (int16_t)lo;
    for (int16_t k = lo16; k <= (int16_t)hi - 2; k++) {
        si = (uint16_t)(si + W16(w, 0x1b8c + 2 * k));
        G16(G_CEC) = (int16_t)si;
    }
    int16_t ax = (int16_t)(W16(w, 0x1b8a + 2 * lo16) + si);
    uint16_t D = 1;
    G16(G_92E4) = ax;
    if (ax > 0x200) {
        do { ax = (int16_t)DIV(ax, 2); D = (uint16_t)(D + D); } while (ax > 0x200);
        G16(G_92E4) = ax;
    }
    int32_t q = DIV(si, D);
    int32_t R = DIV(1000, D);
    G16(G_CEC) = (int16_t)q;
    uint16_t qq = (uint16_t)q;
    uint16_t t1 = DLLVAR(const uint16_t, 0x636a3288)[mode], t2 = DLLVAR(const uint16_t, 0x636a3298)[mode];
    uint16_t rr = (uint16_t)R;
    int32_t prod = (int32_t)(uint16_t)(100 - t1) * rr;
    int32_t bp = (int32_t)qq * t2 > prod ? DIV(prod, qq) : t2;
    G16(G_9270) = (int16_t)t1;
    if (G16(G_CC4) > 0) {
        int32_t pb = (uint16_t)G16(G_CC4);
        if (pb > DIV((int32_t)(uint16_t)bp * qq, rr)) bp = DIV(pb * rr, qq) + 1;
    }
    ET(w, NCNT(w) + 1) = GU16(G_CCC);
    for (int16_t n = 1; n <= NCNT(w); n++) {
        if (EK(w, n) != '*') {
            int32_t tq = DIV((int32_t)ET(w, n), D);
            int16_t delta;
            if (G16(G_92E4) > (int32_t)(uint16_t)tq)
                delta = (int16_t)(DIV((int32_t)(uint16_t)(G16(G_92E4) - tq) * (uint16_t)bp, rr) - G16(G_CC4) + G16(G_9270));
            else delta = (int16_t)(G16(G_9270) - G16(G_CC4));
            EF(w, n) = (int16_t)(EF(w, n) + delta);
        }
        int16_t v = (int16_t)(ET(w, n + 1) * 10 - ET(w, n) * 10);
        ET(w, n) = (uint16_t)(v ? v : 1);
    }
    ET(w, NCNT(w)) = 0;
}

int32_t ph_run(uint8_t *w) {
    uint8_t st = w[0xb4bc];
    if (st == 4) return 0;
    if (st != 2) return (int32_t)0xffffff38;
    static const uint32_t off[5] = { 0x585, 0xb05, 0x1085, 0x1605, 0x438e };
    GPSET(*DLLPTR(uint8_t, 0x63739274), w);
    for (int q = 0; q < 5; q++) {
        GPSET(*DLLPTR(char, rule_vm[0].tape[q]), (char *)w + off[q]);
        GPSET(*DLLPTR(char, 0x63739208 + 4u * (uint32_t)q), (char *)w + off[q] - 1);
    }
    GPSET(*DLLPTR(char, rule_vm[0].tape[5]), (char *)w + 5);
    int16_t s = W16(w, 0), e = W16(w, 2);
    G32(0x6373939c) = 0;
    w[4] = (uint8_t)vm_proc(0, 5, &s, &e, 0);
    vm_proc(0, 10, &s, &e, 0);
    ph_stress_marks(TA, T2, s, e, w[4]);
    vm_proc(0, 16, &s, &e, 0);
    if (G32(0x6373939c)) return (int32_t)0xffffff38;
    ph_marks(s, e, 1);
    ph_durations(s, e, w[4]);
    const uint8_t *t = TA;
    int32_t k = (int16_t)(s - 1);
    while (k < e && (t[k] == '#' || t[k] == '~' || t[k] == ' ')) k++;
    if (k == e) { w[0xb4bc] = 0; return (int32_t)0xffffff38; }
    for (k = (int32_t)s - 1; k < e; k++)
        if (TA[k] == ' ') TA[k] = '-';
    if (w[0x584 + e] == '-') e--;
    W16(w, 0) = s;
    W16(w, 2) = e;
    if (!word_durations(w)) { w[0xb4bc] = 0; return (int32_t)0xffffff37; }
    ph_segments(s, e, w[4]);
    int32_t sum = 0;
    for (k = (int32_t)s - 1; k < e; k++) sum += W16(w, 0x1b8c + 2 * k);
    int32_t v = W32(w, 0xb584) ? sum + W32(w, 0xb4d0) * 200 : sum;
    W32(w, 0xb580) = v;
    W32(w, 0xb494) = 0;
    W32(w, 0xb498) = v * 10;
    ev_place(w);
    ph_pitch(w, s, e);
    for (uint8_t *p = w; W16(p, 0x4912) != 0; p += 6) W16(p, 0x4910) = (int16_t)(W16(p, 0x4910) + W16(w, 0x5810));
    w[0xb4bc] = 3;
    W16(w, 0) = s;
    W16(w, 2) = e;
    return 0;
}
