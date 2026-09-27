/* Letter-to-sound for the Anna frontend: port of MSTTSCommon.dll ILTS (M1033DSK.LTS).
 * File format and algorithm: notes/decwrp_common.md section 2; Python model lts.py (verified 4000/4000).
 *   GetPronunciation 0x41484e7d -> 0x41485bb8 -> beam 0x414852d6 / tree walk 0x414850f4 -> finalize 0x414858e2
 * Scores are float32 as in the DLL (x87 at 53-bit precision, results stored to float). */
#include "anna_lex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BEAM 32
#define MAXL 127

typedef struct {
    const uint32_t *nodes;
    uint32_t nnodes;
    const uint8_t *dist;
    uint32_t ndist;
    const uint8_t *ql;
    uint32_t nql;
} tree;

struct anna_lts {
    uint8_t *data;
    size_t size;
    int nletters, nphones;
    uint16_t letters[64];   /* single-char letters (index 0 = boundary) */
    char **phones;          /* phone strings (ASCII) */
    const uint32_t *lsets, *psets;
    uint32_t nls, wls, nps, wps;
    const uint16_t *qs;
    uint32_t nq;
    tree trees[64];
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* string table section: u32 size; u32 n; u32 off[n]; u32 nbytes; wchar buf[] (+pad) */
static int strtab(const uint8_t *d, size_t size, size_t *pos, int *n_out, char ***strs, uint16_t *single, int maxsingle)
{
    size_t p = *pos, sec, used, bufo;
    uint32_t n, nb, i;
    if (p + 8 > size) return 0;
    sec = rd32(d + p);
    n = rd32(d + p + 4);
    if (p + 8 + 4 * (size_t)n + 4 > size) return 0;
    nb = rd32(d + p + 8 + 4 * n);
    bufo = p + 12 + 4 * (size_t)n;
    if (bufo + nb > size) return 0;
    used = nb + 12 + (size_t)n * 4; /* including the size field */
    if (used > sec || sec - used >= 4) return 0;
    if (strs) {
        *strs = (char **)calloc(n, sizeof(char *));
        if (!*strs) return 0;
    }
    for (i = 0; i < n; i++) {
        size_t o = bufo + 2 * (size_t)rd32(d + p + 8 + 4 * i), k = 0;
        char tmp[64];
        while (o + 2 * k + 1 < bufo + nb && rd16(d + o + 2 * k) && k < 63) {
            tmp[k] = (char)rd16(d + o + 2 * k);
            k++;
        }
        tmp[k] = 0;
        if (single && (int)i < maxsingle) single[i] = (uint16_t)(k ? rd16(d + o) : 0);
        if (strs) {
            (*strs)[i] = (char *)malloc(k + 1);
            if (!(*strs)[i]) return 0;
            memcpy((*strs)[i], tmp, k + 1);
        }
    }
    *n_out = (int)n;
    *pos = p + sec; /* the size counts its own u32 */
    return 1;
}

anna_lts *anna_lts_load_mem(const void *data, size_t size)
{
    anna_lts *m = (anna_lts *)calloc(1, sizeof *m);
    size_t p = 0;
    int L;
    const uint8_t *d;
    if (!m) return NULL;
    m->data = (uint8_t *)malloc(size + 4);
    if (!m->data) {
        free(m);
        return NULL;
    }
    memcpy(m->data, data, size);
    m->size = size;
    d = m->data;
    if (!strtab(d, size, &p, &m->nletters, NULL, m->letters, 64) || m->nletters > 64 ||
        !strtab(d, size, &p, &m->nphones, &m->phones, NULL, 0))
        goto fail;
    /* feature sets (no size prefix) */
    if (p + 8 > size) goto fail;
    m->nls = rd32(d + p);
    m->wls = rd32(d + p + 4);
    p += 8;
    m->lsets = (const uint32_t *)(d + p);
    p += 4 * (size_t)m->nls * m->wls;
    if (p + 8 > size) goto fail;
    m->nps = rd32(d + p);
    m->wps = rd32(d + p + 4);
    p += 8;
    m->psets = (const uint32_t *)(d + p);
    p += 4 * (size_t)m->nps * m->wps;
    /* questions */
    if (p + 8 > size) goto fail;
    {
        size_t sec = rd32(d + p), used;
        m->nq = rd32(d + p + 4);
        m->qs = (const uint16_t *)(d + p + 8);
        used = (size_t)m->nq * 2 + 8;
        if (used > sec || sec - used >= 4) goto fail;
        p += sec;
    }
    for (L = 1; L < m->nletters; L++) {
        tree *t = &m->trees[L];
        size_t sec, q;
        if (p + 8 > size) goto fail;
        sec = rd32(d + p);
        t->nnodes = rd32(d + p + 4);
        t->nodes = (const uint32_t *)(d + p + 8);
        q = p + 8 + 4 * (size_t)t->nnodes;
        if (q + 4 > size) goto fail;
        t->ndist = rd32(d + q);
        t->dist = d + q + 4;
        q += 4 + t->ndist;
        if (q + 4 > size) goto fail;
        t->nql = rd32(d + q);
        t->ql = d + q + 4;
        q += 4 + t->nql;
        if (q > size || q - p > sec) goto fail;
        p += sec;
    }
    return m;
fail:
    anna_lts_free(m);
    return NULL;
}

anna_lts *anna_lts_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *buf;
    anna_lts *m;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = n > 0 ? malloc((size_t)n) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    m = anna_lts_load_mem(buf, (size_t)n);
    free(buf);
    return m;
}

void anna_lts_free(anna_lts *m)
{
    int i;
    if (!m) return;
    if (m->phones)
        for (i = 0; i < m->nphones; i++) free(m->phones[i]);
    free(m->phones);
    free(m->data);
    free(m);
}

/* the file's arrays are little-endian and 4-byte aligned in the copy */
static uint32_t U32(const uint32_t *p, size_t i) { return rd32((const uint8_t *)(p + i)); }

static int setmem(const uint32_t *sets, uint32_t w, uint32_t s, uint16_t id) { return (U32(sets, (size_t)s * w + (id >> 5)) >> (id & 31)) & 1; }

/* 0x414850f4: walk the tree of the letter at letters[li]; hist = phone history, hist[hi] = current slot */
static uint32_t walk(const anna_lts *m, const tree *t, const uint16_t *letters, int li, uint16_t *hist, int hi)
{
    uint32_t k = 0;
    hist[hi] = 1;
    for (;;) {
        uint32_t node, a, b, qp;
        if (k >= t->nnodes) return 0;
        node = U32(t->nodes, k);
        a = node & 0xffff;
        b = node >> 16;
        if ((a & 0x3fff) == 0) return b;
        qp = b;
        for (;;) {
            uint16_t q, Q;
            const uint16_t *seq;
            int idx, off, step, j = 0, tv, op, ln, act;
            if (2 * (size_t)qp + 2 > t->nql) return 0;
            q = rd16(t->ql + 2 * qp);
            Q = rd16((const uint8_t *)(m->qs + (q & 0x3ff)));
            if (Q & 0x4000) {
                seq = hist;
                idx = hi;
            } else {
                seq = letters;
                idx = li;
            }
            off = (Q >> 9) & 15;
            step = (Q & 0x2000) ? 1 : -1;
            while (j < off && seq[idx] != 0) {
                idx += step;
                j++;
            }
            tv = (Q & 0x4000) ? setmem(m->psets, m->wps, Q & 0x1ff, seq[idx]) : setmem(m->lsets, m->wls, Q & 0x1ff, seq[idx]);
            op = q >> 13;
            ln = (q >> 10) & 7;
            /* act: 0 skip ln+1, 1 skip ln, 2 yes, 3 no */
            if (!tv) act = op == 0 ? 0 : op == 1 ? 2 : op == 2 ? 3 : op == 3 ? 1 : op == 4 ? 1 : op == 5 ? 3 : op == 6 ? 2 : 1;
            else act = op <= 2 ? 1 : op == 3 ? 2 : op == 4 ? 3 : op == 5 ? 2 : op == 6 ? 3 : 1;
            if (act == 2) {
                k += a & 0x3fff;
                break;
            }
            if (act == 3) {
                k += (a & 0x3fff) + 1;
                break;
            }
            qp += act == 0 ? (uint32_t)ln + 1 : (uint32_t)ln;
        }
    }
}

typedef struct {
    float sc;
    uint16_t h[MAXL + 3];
} hyp;

typedef struct {
    hyp e[BEAM];
    int n, w;
} beam;

static void recompute_worst(beam *b)
{
    int i;
    float v = b->e[0].sc;
    b->w = 0;
    for (i = 1; i < b->n; i++)
        if (b->e[i].sc < v) {
            v = b->e[i].sc;
            b->w = i;
        }
}

static void insert(beam *b, float sc, const uint16_t *h, int p, uint16_t ph)
{
    int i = b->n < BEAM ? b->n++ : b->w;
    b->e[i].sc = sc;
    memcpy(b->e[i].h, h, sizeof b->e[i].h);
    b->e[i].h[p] = ph;
    if (b->n == BEAM) recompute_worst(b);
}

/* MS CRT qsort (quicksort, median of 3, shortsort <= 8 elements), descending by score */
static int cmp(const hyp *x, const hyp *y) { return y->sc < x->sc ? -1 : (x->sc < y->sc ? 1 : 0); }
static void swp(hyp *a, int i, int j)
{
    hyp t = a[i];
    a[i] = a[j];
    a[j] = t;
}
static void ms_qsort(hyp *a, int num)
{
    int lostk[30], histk[30], sp = 0, lo = 0, hi = num - 1;
    if (num < 2) return;
recurse:
    {
        int size = hi - lo + 1;
        if (size <= 8) {
            int h2 = hi;
            while (h2 > lo) {
                int mx = lo, p;
                for (p = lo + 1; p <= h2; p++)
                    if (cmp(&a[p], &a[mx]) > 0) mx = p;
                swp(a, mx, h2);
                h2--;
            }
        } else {
            int mid = lo + size / 2, lg, hg;
            if (cmp(&a[lo], &a[mid]) > 0) swp(a, lo, mid);
            if (cmp(&a[lo], &a[hi]) > 0) swp(a, lo, hi);
            if (cmp(&a[mid], &a[hi]) > 0) swp(a, mid, hi);
            lg = lo;
            hg = hi;
            for (;;) {
                if (mid > lg) {
                    do lg++;
                    while (lg < mid && cmp(&a[lg], &a[mid]) <= 0);
                }
                if (mid <= lg) {
                    do lg++;
                    while (lg <= hi && cmp(&a[lg], &a[mid]) <= 0);
                }
                do hg--;
                while (hg > mid && cmp(&a[hg], &a[mid]) > 0);
                if (hg < lg) break;
                swp(a, lg, hg);
                if (mid == hg) mid = lg;
            }
            hg++;
            if (mid < hg) {
                do hg--;
                while (hg > mid && cmp(&a[hg], &a[mid]) == 0);
            }
            if (mid >= hg) {
                do hg--;
                while (hg > lo && cmp(&a[hg], &a[mid]) == 0);
            }
            if (hg - lo >= hi - lg) {
                if (lo < hg) {
                    lostk[sp] = lo;
                    histk[sp] = hg;
                    sp++;
                }
                if (lg < hi) {
                    lo = lg;
                    goto recurse;
                }
            } else {
                if (lg < hi) {
                    lostk[sp] = lg;
                    histk[sp] = hi;
                    sp++;
                }
                if (lo < hg) {
                    hi = hg;
                    goto recurse;
                }
            }
        }
    }
    if (--sp >= 0) {
        lo = lostk[sp];
        hi = histk[sp];
        goto recurse;
    }
}

int anna_lts_nbest(const anna_lts *m, const uint16_t *word, double *score, char (*prons)[1024], int max)
{
    uint16_t ids[MAXL + 3];
    static ANNA_TLS beam B[2]; /* ClassicVoices patch: per thread (was: not reentrant) */
    beam *cur = &B[0], *nw = &B[1];
    int n = 0, p, i, cnt, out = 0;
    float sum, k;
    size_t wl = anna_wlen(word);
    ids[0] = 0;
    for (i = 0; i < (int)wl && i < MAXL; i++) {
        uint16_t c = anna_wlower(word[i]);
        int L;
        for (L = 1; L < m->nletters; L++)
            if (m->letters[L] == c) break;
        if (L < m->nletters) ids[++n] = (uint16_t)L;
    }
    ids[n + 1] = 0;
    if (n == 0) return 0;
    memset(cur, 0, sizeof *cur);
    cur->n = 1;
    cur->e[0].sc = 1.0f;
    for (p = 0; p < n; p++) {
        const tree *t = &m->trees[ids[p + 1]];
        nw->n = 0;
        nw->w = 0;
        for (i = 0; i < cur->n; i++) {
            float sc = cur->e[i].sc;
            uint16_t *h = cur->e[i].h;
            uint32_t leaf = walk(m, t, ids, p + 1, h, p + 1);
            if (leaf < 0x400) {
                if (nw->n == BEAM && sc <= nw->e[nw->w].sc) continue;
                insert(nw, sc, h, p + 1, (uint16_t)leaf);
            } else {
                size_t o = (size_t)(leaf - 0x400) * 4;
                int c, ne;
                uint32_t tot = 0;
                float ftot;
                if (o + 4 > t->ndist) continue;
                ne = rd16(t->dist + o) & 0x3f;
                for (c = 0; c < ne; c++) tot += rd16(t->dist + o + 4 * (size_t)c + 2);
                ftot = (float)(int)tot;
                for (c = 0; c < ne; c++) {
                    uint16_t x = rd16(t->dist + o + 4 * (size_t)c), cn = rd16(t->dist + o + 4 * (size_t)c + 2);
                    float f = (float)((double)cn * (double)sc / (double)ftot);
                    if ((nw->n != BEAM || nw->e[nw->w].sc < f) && (double)cn / (double)ftot >= 0.0)
                        insert(nw, f, h, p + 1, (uint16_t)(x >> 6));
                }
            }
        }
        {
            beam *t2 = cur;
            cur = nw;
            nw = t2;
        }
    }
    /* finalize 0x414858e2 */
    sum = 0.0f;
    for (i = 0; i < cur->n; i++) sum = (float)(cur->e[i].sc + sum);
    k = sum != 0.0f ? (float)(1.0 / (double)sum) : 100.0f;
    for (i = 0; i < cur->n; i++) cur->e[i].sc = (float)(k * cur->e[i].sc);
    ms_qsort(cur->e, cur->n);
    cnt = cur->n;
    if (cnt > 10) {
        cnt = 10;
        sum = 0.0f;
        for (i = 0; i < 10; i++) sum = (float)(cur->e[i].sc + sum);
        k = (float)(1.0 / (double)sum);
        for (i = 0; i < 10; i++) cur->e[i].sc = (float)(cur->e[i].sc * k);
    }
    for (i = 0; i < cnt && out < max; i++) {
        size_t len = 0;
        int j;
        if (cur->e[i].sc < 0.00499999988824129f) break;
        for (j = 1; j <= n; j++) {
            uint16_t x = cur->e[i].h[j];
            if (x && x < m->nphones) {
                size_t sl = strlen(m->phones[x]);
                if (len + sl + 1 < 1023) {
                    memcpy(prons[out] + len, m->phones[x], sl);
                    len += sl;
                    prons[out][len++] = ' ';
                }
            }
        }
        /* '#x' pairs dropped, '&' -> ' ' (the en-US data has neither) */
        {
            size_t r = 0, w = 0;
            while (r < len) {
                char ch = prons[out][r];
                if (ch == '#') {
                    r += 2;
                    continue;
                }
                prons[out][w++] = ch == '&' ? ' ' : ch;
                r++;
            }
            len = w;
        }
        if (len) len--; /* last char chopped */
        prons[out][len] = 0;
        score[out] = cur->e[i].sc;
        out++;
    }
    return out;
}

int anna_lts_pronounce(const anna_lts *m, const uint16_t *word, char *out, size_t outsize)
{
    static ANNA_TLS char pr[10][1024]; /* ClassicVoices patch: per thread */
    double sc[10];
    int n = anna_lts_nbest(m, word, sc, pr, 10), best = 0, i;
    size_t o = 0;
    const char *s;
    if (!outsize) return 0;
    out[0] = 0;
    if (n <= 0) return 0;
    for (i = 1; i < n; i++)
        if (sc[best] < sc[i]) best = i;
    for (s = pr[best]; *s && o + 2 < outsize; s++) {
        if (*s > '0' && *s < '9') out[o++] = ' ';
        out[o++] = *s;
    }
    out[o] = 0;
    return (int)o;
}
