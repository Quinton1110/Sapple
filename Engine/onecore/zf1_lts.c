/* zf1_lts.c - CART letter-to-sound.  See zf1_lts.h.  Portable C99 (float math must be IEEE single, no excess
 * precision: FLT_EVAL_METHOD 0, as on x64/SSE). */
#include "zf1_lts.h"
#include <stdlib.h>
#include <string.h>

/* ClassicVoices patch: function-level scratch is per thread (two voices may speak on two threads at once). */
#ifndef ZF_TLS
#define ZF_TLS __thread
#endif

/* ---------------- parse ---------------- */
static int parse_symtab(const uint8_t *res, size_t size, size_t *pp, uint32_t *n, const uint8_t **off,
                        const uint8_t **pool)
{
    size_t p = *pp, st = p;
    uint32_t sec, pb;
    if (p + 8 > size) return -1;
    sec = zf_rd32(res + p);
    *n = zf_rd32(res + p + 4);
    p += 8;
    *off = res + p;
    p += 4 * (size_t)*n;
    if (p + 4 > size) return -1;
    pb = zf_rd32(res + p);
    p += 4;
    *pool = res + p;
    *pp = st + sec;
    (void)pb;
    return *pp <= size ? 0 : -1;
}

int zf1_cart_init(zf1_cart *c, const uint8_t *res, size_t size)
{
    size_t p = 0;
    int k;
    uint32_t i;
    memset(c, 0, sizeof *c);
    if (!res) return -1;
    if (parse_symtab(res, size, &p, &c->nlet, &c->letoff, &c->letpool)) return -2;
    if (parse_symtab(res, size, &p, &c->nout, &c->outoff, &c->outpool)) return -3;
    for (k = 0; k < 2; k++) {
        c->mn[k] = zf_rd32(res + p);
        c->mm[k] = zf_rd32(res + p + 4);
        p += 8;
        c->mat[k] = res + p;
        p += 4 * (size_t)c->mn[k] * c->mm[k];
    }
    {
        size_t st = p;
        uint32_t sec = zf_rd32(res + p);
        c->nqd = zf_rd32(res + p + 4);
        c->qd = res + p + 8;
        p = st + sec;
    }
    if (c->nlet > 64) return -4;
    for (i = 1; i < c->nlet; i++) {
        size_t st = p;
        uint32_t sec = zf_rd32(res + p);
        zf1_cart_tree *t = &c->tree[i];
        p += 4;
        t->nnodes = zf_rd32(res + p); p += 4;
        t->nodes = res + p; p += 4 * (size_t)t->nnodes;
        t->nleaf = zf_rd32(res + p); p += 4;
        t->leaf = res + p; p += t->nleaf;
        t->nq = zf_rd32(res + p); p += 4;
        t->q = res + p; p += t->nq;
        p = st + sec;
        if (p > size) return -5;
    }
    return 0;
}

/* ---------------- beam search ---------------- */
#define NBEAM 32
#define NLAB 0x82                        /* lab[0] = position -1 (always 0), lab[1+pos] */

typedef struct cstate {
    float score;
    uint16_t lab[NLAB];
} cstate;                               /* 0x108 bytes, like the engine */

typedef struct cbuf {
    int count, worst;
    cstate s[NBEAM];
} cbuf;

static uint16_t sym_len(const uint8_t *off, const uint8_t *pool, uint32_t k, const uint8_t **str)
{
    const uint8_t *s = pool + 2 * (size_t)zf_rd32(off + 4 * k);
    uint16_t n = 0;
    *str = s;
    while (zf_rd16(s + 2 * n)) n++;
    return n;
}

/* FUN_180047308 with param_4 == 0.  letters/labels point at the current position. */
static uint16_t tree_walk(const zf1_cart *c, const uint16_t *letters, uint16_t *labels)
{
    const zf1_cart_tree *t = &c->tree[*letters];
    uint32_t node = 0;
    *labels = 1;
    for (;;) {
        uint16_t a = zf_rd16(t->nodes + 4 * node), b = zf_rd16(t->nodes + 4 * node + 2);
        uint32_t qp;
        int res;
        if ((a & 0x3fff) == 0) return b;
        qp = b;
        for (;;) {
            uint16_t term = zf_rd16(t->q + 2 * qp);
            uint16_t qd = zf_rd16(c->qd + 2 * (term & 0x3ff));
            const uint16_t *pv = (qd & 0x4000) ? labels : letters;
            unsigned steps = (qd >> 9) & 0xf, k = 0, op = term >> 13, len = (term >> 10) & 7;
            int mi = (qd >> 14) & 1, bit;
            uint16_t v;
            if (qd & 0x2000) {
                while (k < steps && *pv) { k++; pv++; }
            } else {
                while (k < steps && *pv) { k++; pv--; }
            }
            v = *pv;
            {
                const uint8_t *row = c->mat[mi] + 4 * (size_t)c->mm[mi] * (qd & 0x1ff);
                bit = (int)((zf_rd32(row + 4 * (v >> 5)) >> (v & 31)) & 1);
            }
            if (!bit) {
                if (op == 0) { qp += len + 1; continue; }
                if (op == 1) { res = 1; break; }
                if (op == 2 || op == 5) { res = 0; break; }
                if (op == 3 || op == 4) { qp += len; continue; }
                if (op == 6) { res = 1; break; }
                continue;   /* op 7: engine loops on the same term */
            } else {
                if (op <= 2) { qp += len; continue; }
                if (op == 3 || op == 5) { res = 1; break; }
                if (op == 4 || op == 6) { res = 0; break; }
                continue;
            }
        }
        node += (a & 0x3fff) + (res ? 0 : 1);
    }
}

static void recompute_worst(cbuf *d)
{
    float w = d->s[0].score;
    int i;
    d->worst = 0;
    for (i = 1; i < d->count; i++)
        if (d->s[i].score < w) { w = d->s[i].score; d->worst = i; }
}

/* FUN_180160300 */
static int beam_step(const zf1_cart *c, cbuf *src, cbuf *dst, const uint16_t *letters, int pos)
{
    int i;
    for (i = 0; i < src->count; i++) {
        cstate *ss = &src->s[i];
        uint16_t out = tree_walk(c, letters + pos, &ss->lab[1 + pos]);
        if (out < 0x400) {
            int slot = dst->count;
            if (slot < NBEAM) dst->count++;
            else {
                slot = dst->worst;
                if (ss->score <= dst->s[slot].score) continue;
            }
            dst->s[slot].score = ss->score;
            if (pos > 0) memcpy(&dst->s[slot].lab[1], &ss->lab[1], 2 * (size_t)pos);
            dst->s[slot].lab[1 + pos] = out;
            if (dst->count == NBEAM) recompute_worst(dst);
        } else {
            const zf1_cart_tree *t = &c->tree[letters[pos]];
            const uint8_t *e = t->leaf + 4 * (size_t)(out - 0x400);
            unsigned n = zf_rd16(e) & 0x3f, k;
            int tot = 0;
            if (!n) return -1;
            for (k = 0; k < n; k++) tot += zf_rd16(e + 4 * k + 2);
            if (tot < 1) return -1;
            for (k = 0; k < n; k++) {
                float fr = (float)(int)zf_rd16(e + 4 * k + 2);
                float sc = (fr * ss->score) / (float)tot;
                int slot;
                if (dst->count == NBEAM && !(dst->s[dst->worst].score < sc)) continue;
                if (!(0.0f <= fr / (float)tot)) continue;
                slot = dst->count;
                if (slot < NBEAM) dst->count++;
                else slot = dst->worst;
                dst->s[slot].score = sc;
                if (pos > 0) memcpy(&dst->s[slot].lab[1], &ss->lab[1], 2 * (size_t)pos);
                dst->s[slot].lab[1 + pos] = (uint16_t)(zf_rd16(e + 4 * k) >> 6);
                if (dst->count == NBEAM) recompute_worst(dst);
            }
        }
    }
    return 0;
}

static int cmp_score_desc(const void *a, const void *b)
{
    float x = ((const cstate *)a)->score, y = ((const cstate *)b)->score;
    if (y < x) return -1;
    return x < y;
}

int zf1_cart_predict(const zf1_cart *c, const zf_char *word, zf_char *out, int cap)
{
    uint16_t L[0x90];
    int n = 0, pos, i, best;
    static ZF_TLS cbuf bufs[2];         /* ClassicVoices patch: per thread (was shared by every engine); the engine allocates per call */
    cbuf *src, *dst;
    float cand_score[10];
    zf_char cand[10][0x400];
    int ncand = 0;
    out[0] = 0;
    memset(L, 0, sizeof L);
    for (; *word && n <= 0x7e; word++) {
        zf_char ch = zf_tolower(*word);
        uint32_t k;
        for (k = 1; k < c->nlet; k++) {
            const uint8_t *s;
            uint16_t ln = sym_len(c->letoff, c->letpool, k, &s);
            if (ln == 1 && zf_rd16(s) == ch) { L[1 + n++] = (uint16_t)k; break; }
        }
    }
    if (n <= 0) return 1;               /* engine: FUN_1800575d8 returns NULL -> no output */
    memset(bufs, 0, sizeof bufs);
    src = &bufs[0];
    dst = &bufs[1];
    src->count = 1;
    src->s[0].score = 1.0f;
    for (pos = 0; pos < n; pos++) {
        if (beam_step(c, src, dst, L + 1, pos) < 0) return -1;
        src->count = 0;
        { cbuf *t = src; src = dst; dst = t; }
    }
    /* FUN_180160c08 on src */
    {
        float sum = 0.0f, f = 100.0f, s10 = 0.0f;
        int cnt = src->count, lim, k;
        for (k = 0; k < cnt; k++) sum += src->s[k].score;
        if (sum != 0.0f) f = 1.0f / sum;
        for (k = 0; k < cnt; k++) src->s[k].score = f * src->s[k].score;
        zf1_ms_qsort(src->s, (size_t)cnt, sizeof(cstate), cmp_score_desc);
        lim = 10;
        if (cnt < 11) lim = cnt;
        else {
            for (k = 0; k < 10; k++) s10 += src->s[k].score;
            for (k = 0; k < 10; k++) src->s[k].score = (1.0f / s10) * src->s[k].score;
        }
        for (k = 0; k < lim && 0.005f <= src->s[k].score; k++) {
            zf_char tmp[1024];
            int tl = 0, j, ol = 0;
            const uint16_t *lab = &src->s[k].lab[1];
            tmp[0] = 0;
            for (j = 0; lab[j]; j++) {
                if (lab[j] <= c->nout) {
                    const uint8_t *s;
                    uint16_t ln = sym_len(c->outoff, c->outpool, lab[j], &s), m;
                    for (m = 0; m < ln && tl < 1022; m++) tmp[tl++] = zf_rd16(s + 2 * m);
                    if (tl < 1022) tmp[tl++] = ' ';
                }
            }
            tmp[tl] = 0;
            for (j = 0; tmp[j] && ol < 0x3ff; ) {
                if (tmp[j] == '#') { j += 2; continue; }   /* engine skips '#' and the following char */
                cand[ncand][ol++] = (tmp[j] == '&') ? ' ' : tmp[j];
                j++;
            }
            if (ol > 0) ol--;               /* drop the trailing separator */
            cand[ncand][ol] = 0;
            cand_score[ncand] = src->s[k].score;
            ncand++;
        }
    }
    if (!ncand) return 0;
    best = 0;
    for (i = 1; i < ncand; i++)
        if (!(cand_score[i] <= cand_score[best])) best = i;
    for (i = 0; i < cap - 1 && cand[best][i]; i++) out[i] = cand[best][i];
    out[i] = 0;
    return 0;
}

/* ---------------- phones ---------------- */
int zf1_phone_flags(const zf1_phoneset *ps, int id, uint32_t *flags)
{
    if (id < 0 || id >= 128 || ps->byid[id] < 0) return 0;
    *flags = ps->flags[ps->byid[id]];
    return 1;
}

int zf1_has_vowel(const zf1_phoneset *ps, const uint16_t *ph, int n)
{
    int i;
    uint32_t f;
    for (i = 0; i < n; i++)
        if (zf1_phone_flags(ps, ph[i], &f) && (f & 1)) return 1;
    return 0;
}

static int lookup_tok(const zf1_phoneset *ps, const zf_char *t, int n)
{
    char a[24];
    int i;
    if (n <= 0 || n > 20) return -1;
    for (i = 0; i < n; i++) a[i] = (t[i] < 128) ? (char)t[i] : '?';
    return zf1_phone_id(ps, a, n);
}

int zf1_phonestr_to_ids(const zf1_phoneset *ps, const zf_char *s, uint16_t *ph, int cap)
{
    zf_char tok[0x14];
    int tn = 0, k = 0, id;
    for (; *s && k + 1 < cap; s++) {
        zf_char ch = *s;
        if (ch == ' ') {
            if (tn) {
                id = lookup_tok(ps, tok, tn);
                if (id >= 0) ph[k++] = (uint16_t)id;
                tn = 0;
            }
        } else if (ch == '-' || ch == '1' || ch == '2') {
            if (tn) {
                id = lookup_tok(ps, tok, tn);
                if (id >= 0) ph[k++] = (uint16_t)id;
                tn = 0;
            }
            if (k + 1 < cap) {
                id = lookup_tok(ps, &ch, 1);
                if (id >= 0) ph[k++] = (uint16_t)id;
            }
        } else if (tn + 1 < 0x14) {
            tok[tn++] = ch;
        }
    }
    if (tn && k + 1 < cap) {
        id = lookup_tok(ps, tok, tn);
        if (id >= 0) ph[k++] = (uint16_t)id;
    }
    ph[k] = 0;
    return k;
}

/* ---------------- MSVC qsort ---------------- */
static void swapb(char *a, char *b, size_t w)
{
    if (a != b)
        while (w--) { char t = *a; *a++ = *b; *b++ = t; }
}

static void shortsort(char *lo, char *hi, size_t w, int (*comp)(const void *, const void *))
{
    while (hi > lo) {
        char *mx = lo, *p;
        for (p = lo + w; p <= hi; p += w)
            if (comp(p, mx) > 0) mx = p;
        swapb(mx, hi, w);
        hi -= w;
    }
}

void zf1_ms_qsort(void *base, size_t num, size_t w, int (*comp)(const void *, const void *))
{
    char *lo, *hi, *mid, *loguy, *higuy;
    char *lostk[64], *histk[64];
    int sp = 0;
    size_t size;
    if (num < 2) return;
    lo = (char *)base;
    hi = (char *)base + w * (num - 1);
recurse:
    size = (size_t)(hi - lo) / w + 1;
    if (size <= 8) {
        shortsort(lo, hi, w, comp);
    } else {
        mid = lo + (size / 2) * w;
        if (comp(lo, mid) > 0) swapb(lo, mid, w);
        if (comp(lo, hi) > 0) swapb(lo, hi, w);
        if (comp(mid, hi) > 0) swapb(mid, hi, w);
        loguy = lo;
        higuy = hi;
        for (;;) {
            if (mid > loguy) {
                do { loguy += w; } while (loguy < mid && comp(loguy, mid) <= 0);
            }
            if (mid <= loguy) {
                do { loguy += w; } while (loguy <= hi && comp(loguy, mid) <= 0);
            }
            do { higuy -= w; } while (higuy > mid && comp(higuy, mid) > 0);
            if (higuy < loguy) break;
            swapb(loguy, higuy, w);
            if (mid == higuy) mid = loguy;
        }
        higuy += w;
        if (mid < higuy) {
            do { higuy -= w; } while (higuy > mid && comp(higuy, mid) == 0);
        }
        if (mid >= higuy) {
            do { higuy -= w; } while (higuy > lo && comp(higuy, mid) == 0);
        }
        if (higuy - lo >= hi - loguy) {
            if (lo < higuy) { lostk[sp] = lo; histk[sp] = higuy; ++sp; }
            if (loguy < hi) { lo = loguy; goto recurse; }
        } else {
            if (loguy < hi) { lostk[sp] = loguy; histk[sp] = hi; ++sp; }
            if (lo < higuy) { hi = higuy; goto recurse; }
        }
    }
    --sp;
    if (sp >= 0) { lo = lostk[sp]; hi = histk[sp]; goto recurse; }
}
