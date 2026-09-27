/* Anna voice data (UNT, CRT, APL) and unit selection (MSTTSEngine FUN_081aa101 / FUN_081a9b0c). */
#include "anna_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *fn, size_t *len)
{
    FILE *f = fopen(fn, "rb");
    unsigned char *d;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc((size_t)n + 1);
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return d;
}

static int32_t rd32(const unsigned char *p) { return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); }
static int rd16s(const unsigned char *p) { return (int16_t)(p[0] | p[1] << 8); }
static float rdf(const unsigned char *p)
{
    uint32_t u = (uint32_t)rd32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* ---- UNT ---- */
static int load_unt(anna_voice *v, const char *fn)
{
    size_t len, i;
    unsigned char *d = slurp(fn, &len);
    if (!d || len < 0x514) goto fail;
    v->nunits = rd32(d + 0x14);
    if ((size_t)0x514 + (size_t)v->nunits * 20 > len) goto fail;
    for (i = 0; i < 261; i++) v->type0[i] = rd32(d + 0x100 + 4 * i);
    v->type0[261] = v->nunits;
    v->units = malloc(sizeof *v->units * (size_t)v->nunits);
    if (!v->units) goto fail;
    for (i = 0; i < (size_t)v->nunits; i++) {
        const unsigned char *r = d + 0x514 + 20 * i;
        uint32_t f = (uint32_t)rd32(r), l = (uint32_t)rd32(r + 4);
        static const int sh[9] = {0, 4, 7, 10, 16, 22, 25, 28, 30}, mk[9] = {15, 7, 7, 63, 63, 7, 7, 3, 3};
        int k;
        for (k = 0; k < 9; k++) v->units[i].f[k] = (unsigned char)((f >> sh[k]) & (unsigned)mk[k]);
        v->units[i].join = (unsigned char)(l & 1);
        v->units[i].len = (int32_t)((l >> 1) & 0xFFFFFF);
        v->units[i].start = (int32_t)rd32(r + 8);
        v->units[i].wih_off = (uint32_t)rd32(r + 12);
        v->units[i].wih_n = (uint16_t)(r[16] | r[17] << 8);
    }
    free(d);
    return 0;
fail:
    free(d);
    return -1;
}

/* ---- APL ---- */
static int load_apl(anna_voice *v, const char *fn)
{
    size_t len;
    unsigned char *d = slurp(fn, &len);
    const unsigned char *p;
    float *arr[10] = {0};
    int cnt[10], k, i, n1, n2, dims[9];
    if (!d || len < 16) goto fail;
    p = d + rd32(d + 8);
    n1 = rd32(p + 2);
    n2 = rd32(p + 6);
    p += 10;
    for (k = 0; k < 10; k++) {
        cnt[k] = rd32(p);
        p += 4;
        arr[k] = (float *)p; /* unaligned view, copied below */
        p += 4 * cnt[k];
    }
    if (cnt[9] != 9) goto fail;
    dims[0] = 12, dims[1] = 4, dims[2] = 5, dims[3] = n1, dims[4] = n1, dims[5] = n2, dims[6] = n2, dims[7] = 4, dims[8] = 2;
    for (k = 0; k < 9; k++) {
        float w = rdf((const unsigned char *)arr[9] + 4 * k);
        v->dim[k] = dims[k];
        v->tab[k] = NULL;
        if (!cnt[k]) continue;
        if (cnt[k] != dims[k] * dims[k]) goto fail;
        v->tab[k] = malloc(sizeof(float) * (size_t)cnt[k]);
        if (!v->tab[k]) goto fail;
        for (i = 0; i < cnt[k]; i++) v->tab[k][i] = rdf((const unsigned char *)arr[k] + 4 * i) * w; /* scaled in place (float) */
    }
    v->tw = rdf(p);
    p += 4;
    if (rd32(p) != 24) goto fail;
    for (i = 0; i < 24; i++) v->jt[i] = rdf(p + 4 + 4 * i);
    p += 4 + 96;
    p += 4 + 4 * rd32(p);
    v->jw = rdf(p);
    free(d);
    return 0;
fail:
    free(d);
    return -1;
}

/* ---- CRT ---- */
typedef struct {
    const unsigned char *d;
    size_t p, len;
    anna_voice *v;
    int bad;
} crt_reader;

static int32_t crt_i(crt_reader *r)
{
    int32_t x;
    if (r->p + 4 > r->len) {
        r->bad = 1;
        return 0;
    }
    x = rd32(r->d + r->p);
    r->p += 4;
    return x;
}

static int crt_push(anna_voice *v)
{
    if (v->nnodes == v->capnodes) {
        int c = v->capnodes ? v->capnodes * 2 : 4096;
        anna_node *n = realloc(v->nodes, sizeof *n * (size_t)c);
        if (!n) return -1;
        v->nodes = n;
        v->capnodes = c;
    }
    return v->nnodes++;
}

static int crt_node(crt_reader *r)
{
    anna_voice *v = r->v;
    int id = crt_push(v), kind;
    if (id < 0) return -1;
    kind = crt_i(r);
    if (kind == 1) {
        int ne = crt_i(r), root = crt_i(r), y, n;
        if (ne < 0 || r->p + 12 * (size_t)ne > r->len) {
            r->bad = 1;
            return -1;
        }
        v->nodes[id].leaf = 0;
        v->nodes[id].nexpr = ne;
        v->nodes[id].root = root;
        v->nodes[id].expr = r->d + r->p;
        r->p += 12 * (size_t)ne;
        y = crt_node(r);
        n = crt_node(r);
        v->nodes[id].yes = y;
        v->nodes[id].no = n;
    } else {
        int lo, hi, nb, nw;
        crt_i(r); /* bitset kind 0 */
        lo = crt_i(r);
        hi = crt_i(r);
        crt_i(r); /* 1 */
        nb = crt_i(r);
        crt_i(r); /* popcount */
        nw = (nb + 31) / 32;
        if (nb != hi - lo + 1 || r->p + 4 * (size_t)nw > r->len) {
            r->bad = 1;
            return -1;
        }
        v->nodes[id].leaf = 1;
        v->nodes[id].lo = lo;
        v->nodes[id].nbits = nb;
        v->nodes[id].bits = r->d + r->p;
        r->p += 4 * (size_t)nw;
    }
    return r->bad ? -1 : id;
}

static int load_crt(anna_voice *v, const char *fn)
{
    size_t len;
    unsigned char *d = slurp(fn, &len);
    crt_reader r;
    int nq, k, ntree;
    if (!d || len < 32) return -1;
    v->crt = d;
    r.d = d, r.len = len, r.v = v, r.bad = 0;
    r.p = (size_t)rd32(d + 4);
    nq = crt_i(&r);
    if (nq <= 0 || nq > 4096) return -1;
    v->nq = nq;
    v->qs = calloc((size_t)nq, sizeof *v->qs);
    if (!v->qs) return -1;
    for (k = 0; k < nq; k++) {
        int n = crt_i(&r), i;
        crt_i(&r); /* id */
        v->qs[k].feat = crt_i(&r);
        for (i = 0; i < n - 2; i++) {
            int val = crt_i(&r);
            if (val >= 0 && val < 64) v->qs[k].mask |= (uint64_t)1 << val;
        }
    }
    ntree = rd32(d + 16);
    for (k = 0; k < 262; k++) v->tree[k] = -1;
    for (k = 0; k < ntree && !r.bad; k++) {
        const unsigned char *e = d + rd32(d + 12) + 12 * k;
        int ty = rd16s(e);
        r.p = (size_t)rd32(d + 24) + (size_t)rd32(e + 4);
        if (ty > 0 && ty < 262) v->tree[ty] = crt_node(&r);
    }
    return r.bad ? -1 : 0;
}

anna_voice *anna_voice_load(const char *base, anna_decoder *dec, char *err, size_t errlen)
{
    anna_voice *v = calloc(1, sizeof *v);
    char fn[1024];
    if (!v) return NULL;
    v->dec = dec;
    v->default_rate = 2; /* Anna's token: DefaultRate = 2 */
    snprintf(fn, sizeof fn, "%s.UNT", base);
    if (load_unt(v, fn)) {
        snprintf(err, errlen, "cannot load %s", fn);
        goto fail;
    }
    snprintf(fn, sizeof fn, "%s.APL", base);
    if (load_apl(v, fn)) {
        snprintf(err, errlen, "cannot load %s", fn);
        goto fail;
    }
    snprintf(fn, sizeof fn, "%s.WIH", base);
    v->wih = fopen(fn, "rb"); /* optional: only needed for <pitch> */
    snprintf(fn, sizeof fn, "%s.CRT", base);
    if (load_crt(v, fn)) {
        snprintf(err, errlen, "cannot load %s", fn);
        goto fail;
    }
    return v;
fail:
    anna_voice_free(v);
    return NULL;
}

void anna_voice_free(anna_voice *v)
{
    int k;
    if (!v) return;
    free(v->units);
    for (k = 0; k < 9; k++) free(v->tab[k]);
    free(v->nodes);
    free(v->qs);
    free(v->crt);
    free(v->hann);
    if (v->wih) fclose((FILE *)v->wih);
    free(v);
}

/* the pitch changer's 5000-point Hann window, a table of doubles in MSTTSEngine.dll (file offset 0xd88) */
int anna_voice_load_pitch_table(anna_voice *v, const char *path, long offset)
{
    FILE *f = fopen(path, "rb");
    unsigned char b[8];
    int i;
    if (!f) return -1;
    v->hann = malloc(sizeof(double) * 5000);
    if (!v->hann || fseek(f, offset, SEEK_SET)) goto fail;
    for (i = 0; i < 5000; i++) {
        uint64_t u = 0;
        int k;
        if (fread(b, 1, 8, f) != 8) goto fail;
        for (k = 7; k >= 0; k--) u = u << 8 | b[k];
        memcpy(&v->hann[i], &u, 8);
    }
    fclose(f);
    if (v->hann[0] != 0.0 || v->hann[2500] != 1.0) { /* not the expected table */
        free(v->hann);
        v->hann = NULL;
        return -1;
    }
    return 0;
fail:
    fclose(f);
    free(v->hann);
    v->hann = NULL;
    return -1;
}

/* ---- unit selection ---- */

static int qtest(const anna_voice *v, int q, const int *f)
{
    int x;
    if (q < 0 || q >= v->nq || v->qs[q].feat < 0 || v->qs[q].feat > 8) return 0;
    x = f[v->qs[q].feat];
    return x >= 0 && x < 64 && (v->qs[q].mask >> x & 1);
}

/* FUN_081ab765: 0 false, 1 question, 3 or, 4 and, 5 not; flag bit0/bit1 = operand is a question */
static int expr(const anna_voice *v, const unsigned char *ex, int nexpr, int i, const int *f, int depth)
{
    const unsigned char *e;
    int op, fl, a, b, va, vb;
    if (i < 0 || i >= nexpr || depth > 64) return 0;
    e = ex + 12 * i;
    op = e[0] | e[1] << 8;
    fl = e[2] | e[3] << 8;
    a = rd32(e + 4);
    b = rd32(e + 8);
    if (op == 0) return 0;
    if (op == 1) return qtest(v, a, f);
    va = fl & 1 ? qtest(v, a, f) : expr(v, ex, nexpr, a, f, depth + 1);
    if (op == 5) return !va;
    vb = fl & 2 ? qtest(v, b, f) : expr(v, ex, nexpr, b, f, depth + 1);
    if (op == 3) return va || vb;
    if (op == 4) return va && vb;
    return 0;
}

/* walk the unit type's tree; append candidate unit indexes (ascending) to out */
static int candidates(const anna_voice *v, int type, const int *f, long *out)
{
    int n = type > 0 && type < 262 ? v->tree[type] : -1, cnt = 0, i;
    if (n < 0) return 0;
    while (!v->nodes[n].leaf) {
        const anna_node *q = &v->nodes[n];
        n = expr(v, q->expr, q->nexpr, q->root, f, 0) ? q->yes : q->no;
    }
    for (i = 0; i < v->nodes[n].nbits; i++)
        if (v->nodes[n].bits[i >> 3] >> (i & 7) & 1) out[cnt++] = v->type0[type] + v->nodes[n].lo + i;
    return cnt;
}

typedef struct {
    long *cand;
    double *tc, *acc;
    int *back;
    int n, best;
    double jc;
} lattice_pos;

int anna_select(const anna_voice *v, const anna_unitspec *specs, int n, long *units)
{
    lattice_pos *lat = calloc((size_t)n + 1, sizeof *lat);
    long *tmp = NULL, *tmp2 = NULL;
    int npos = 0, i, rc = 0, prev = -1;
    size_t maxc = 0;
    for (i = 0; i < 262; i++) { /* largest possible leaf, for scratch buffers */
        long c = v->type0[i + 1 < 262 ? i + 1 : 261] - v->type0[i];
        if (c > (long)maxc) maxc = (size_t)c;
    }
    tmp = malloc(sizeof *tmp * (maxc + 1));
    tmp2 = malloc(sizeof *tmp2 * (maxc + 1));
    if (!lat || !tmp || !tmp2) {
        rc = -1;
        goto done;
    }
    for (i = 0; i < n; i++) {
        const anna_unitspec *s = &specs[i];
        lattice_pos *L;
        int c, k, j;
        units[i] = -1;
        if (s->type == 0) continue;
        L = &lat[npos];
        if (s->f[2] == 2) { /* en-US: walk with f2 = 0 and f2 = 1 and merge (FUN_081a9d3a) */
            int g[9], na, nb, a = 0, b = 0;
            memcpy(g, s->f, sizeof g);
            g[2] = 0;
            na = candidates(v, s->type, g, tmp);
            g[2] = 1;
            nb = candidates(v, s->type, g, tmp2);
            L->cand = malloc(sizeof(long) * (size_t)(na + nb + 1));
            if (!L->cand) {
                rc = -1;
                goto done;
            }
            c = 0;
            while (a < na || b < nb) {
                long x = b >= nb || (a < na && tmp[a] <= tmp2[b]) ? tmp[a] : tmp2[b];
                if (a < na && tmp[a] == x) a++;
                if (b < nb && tmp2[b] == x) b++;
                L->cand[c++] = x;
            }
        } else {
            c = candidates(v, s->type, s->f, tmp);
            L->cand = malloc(sizeof(long) * (size_t)(c + 1));
            if (!L->cand) {
                rc = -1;
                goto done;
            }
            memcpy(L->cand, tmp, sizeof(long) * (size_t)c);
        }
        L->n = c;
        L->tc = malloc(sizeof(double) * (size_t)(c + 1));
        L->acc = malloc(sizeof(double) * (size_t)(c + 1));
        L->back = malloc(sizeof(int) * (size_t)(c + 1));
        if (!L->tc || !L->acc || !L->back) {
            rc = -1;
            goto done;
        }
        for (j = 0; j < c; j++) { /* target cost (FUN_081a997e) */
            const anna_unit *u = &v->units[L->cand[j]];
            double t = 0.0;
            for (k = 0; k < 9; k++)
                if (v->tab[k] && s->f[k] >= 0 && s->f[k] < v->dim[k]) t += (double)v->tab[k][s->f[k] * v->dim[k] + u->f[k]];
            L->tc[j] = t * (double)v->tw;
        }
        L->jc = 0.0;
        if (prev >= 0) { /* join cost of this boundary (FUN_081a9953) */
            const anna_unitspec *p = &specs[prev];
            int row = p->brk >= 0 && p->brk < 6 ? p->brk : 0;
            L->jc = (double)v->jw * (double)v->jt[row * 4 + ((s->ff & 0x10) ? 1 : 0) + 2 * ((p->bf & 0x10) ? 1 : 0)];
        }
        L->best = 0;
        prev = i;
        units[i] = npos; /* position index for now */
        npos++;
    }
    /* Viterbi (FUN_081a9b0c) */
    for (i = 0; i < npos; i++) {
        lattice_pos *L = &lat[i];
        int j;
        if (i == 0) {
            for (j = 0; j < L->n; j++) {
                L->acc[j] = L->tc[j];
                L->back[j] = -1;
            }
        } else {
            const lattice_pos *P = &lat[i - 1];
            double B = P->n ? P->acc[P->best] : 0.0;
            int q = 0;
            for (j = 0; j < L->n; j++) {
                const anna_unit *c = &v->units[L->cand[j]];
                for (; q < P->n; q++) {
                    const anna_unit *pu = &v->units[P->cand[q]];
                    long d = (long)pu->start + (pu->len & 0xFFFF) - c->start;
                    if (d < 0 || (d == 0 && !c->join)) continue;
                    break;
                }
                if (q < P->n) {
                    const anna_unit *pu = &v->units[P->cand[q]];
                    if ((long)pu->start + (pu->len & 0xFFFF) == c->start && c->join && (q == P->best || P->acc[q] <= B + L->jc)) {
                        L->acc[j] = P->acc[q] + L->tc[j];
                        L->back[j] = q;
                        continue;
                    }
                }
                L->acc[j] = B + L->jc + L->tc[j];
                L->back[j] = P->best;
            }
        }
        L->best = 0;
        for (j = 1; j < L->n; j++)
            if (L->acc[j] < L->acc[L->best]) L->best = j;
    }
    { /* backtrace */
        int b = npos ? lat[npos - 1].best : -1, pos;
        long *chosen = malloc(sizeof(long) * (size_t)(npos + 1));
        if (!chosen) {
            rc = -1;
            goto done;
        }
        for (pos = npos - 1; pos >= 0; pos--) {
            chosen[pos] = b >= 0 && b < lat[pos].n ? lat[pos].cand[b] : -1;
            b = b >= 0 && b < lat[pos].n ? lat[pos].back[b] : -1;
        }
        for (i = 0; i < n; i++)
            if (units[i] >= 0) units[i] = chosen[units[i]];
        free(chosen);
    }
done:
    if (lat)
        for (i = 0; i < npos + 1 && i <= n; i++) {
            free(lat[i].cand);
            free(lat[i].tc);
            free(lat[i].acc);
            free(lat[i].back);
        }
    free(lat);
    free(tmp);
    free(tmp2);
    return rc;
}
