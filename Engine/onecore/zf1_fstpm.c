/* zf1_fstpm.c - NE FST pattern matcher (fork B); see zf1_fstpm.h.
 *
 * File layout (FUN_1800dbde0, big endian header):
 *   u32 size, u8 compression (0x13/0x15), u8, u8 header length H (>= 20), u32 key, u16 H2, u24 network count,
 *   u8 lic, u8, u16, date string (H - 20 bytes), copyright (H2 - H bytes), u32 network offsets.
 *   license = lic ? ((key & 0xff) < 0x81 ? lic - (key & 0xff) : (key & 0xff) - lic) : 0; 1 = PRIVATE: every
 *   string length / character read at network offset o is stored shifted by (o & 0x7f) (< 0x40: -o, else +o).
 * Network (FUN_180069c00; offsets relative to the network start):
 *   varint states, varint arcs, varint flags (bit 8: start state final), varint property count, properties
 *   (string key + typed object, FUN_180022714), symbol list (FUN_180020500: list object, symbol ids from
 *   FUN_1800210d8; symbols 0 = EPSILON, 1 = OTHER = complement of the single-character symbols), varint label
 *   count, varint arity (1/2), labels (arity 2: two symbol indices, output side first, input side second),
 *   arcs in DFS order (see read_arcs), varint byte count of the network.
 * Varints (FUN_180022470): first byte 0x40 = negative, 0x3f = value, 0x80 = continuation (7 bits per byte). */
#include "zf1_fstpm.h"
#include <stdlib.h>
#include <string.h>

enum { SY_EPS, SY_OTHER, SY_CHAR, SY_LIST, SY_XLIST, SY_TAG, SY_NONE };

typedef struct { int kind, val; } pm_sym;

typedef struct pm_obj {
    int type;          /* 0 int, 1 char, 2 string, 4 list, 6 block */
    int ival;
    int n;             /* string length / list count */
    int *str;
    struct pm_obj **items;
} pm_obj;

struct zf1_pm {
    int nstates;
    uint32_t *first;       /* CSR: arcs of state s = [first[s], first[s + 1]) */
    uint16_t *alabel;
    uint32_t *atarget;
    uint8_t *final;
    int nlabels;
    uint64_t (*mask)[2];   /* ASCII characters accepted by the input side of each label */
    int16_t *ltag;         /* output-side tag index (-1 none) */
    uint8_t *leps;         /* input side is epsilon */
    int ntags;
    char **tags;
    uint64_t alpha[2], delim[2];  /* PMATCH_ALPHA_LIST / PMATCH_DELIM_LIST */
    uint64_t firstc[2];    /* characters that can start a match (a non-empty path from the start closure) */
    /* matcher scratch */
    uint32_t *stamp, gen;
    int *cur, *nxt;
};

typedef struct {
    const uint8_t *p, *end, *base;
    int scr, fmt, err;
} pm_rd;

static int rd_u8(pm_rd *r)
{
    if (r->p >= r->end) { r->err = 1; return 0; }
    return *r->p++;
}

static int32_t rd_var(pm_rd *r)
{
    int b = rd_u8(r), neg = b & 0x40;
    uint32_t v = (uint32_t)(b & 0x3f);
    while ((b & 0x80) && !r->err) {
        b = rd_u8(r);
        v = (v << 7) | (uint32_t)(b & 0x7f);
    }
    if (neg) v = 0u - v;
    return (int32_t)v;
}

static int32_t descr(pm_rd *r, long off, int32_t v)
{
    uint32_t o;
    if (!r->scr) return v;
    o = (uint32_t)off & 0x7f;
    return (int32_t)(o < 0x40 ? (uint32_t)v - o : (uint32_t)v + o);
}

static void obj_free(pm_obj *o)
{
    int i;
    if (!o) return;
    free(o->str);
    if (o->items) {
        for (i = 0; i < o->n; i++) obj_free(o->items[i]);
        free(o->items);
    }
    free(o);
}

static pm_obj *rd_string(pm_rd *r)   /* FUN_180022068 */
{
    pm_obj *o = (pm_obj *)calloc(1, sizeof *o);
    long off = (long)(r->p - r->base);
    int32_t n = descr(r, off, rd_var(r)), i;
    o->type = 2;
    if (n < 0 || n > (1 << 24)) { r->err = 1; return o; }
    o->n = n;
    o->str = (int *)malloc(sizeof(int) * (size_t)(n + 1));
    for (i = 0; i < n && !r->err; i++) {
        off = (long)(r->p - r->base);
        if (r->fmt < 0x15) {
            int a = rd_u8(r), b = rd_u8(r);
            o->str[i] = (a << 8) | b;
        } else {
            o->str[i] = descr(r, off, rd_var(r));
        }
    }
    o->str[n] = 0;
    return o;
}

static pm_obj *rd_typed(pm_rd *r, int t);

static pm_obj *rd_list(pm_rd *r)    /* FUN_180021d70 */
{
    pm_obj *o = (pm_obj *)calloc(1, sizeof *o);
    int32_t n = rd_var(r), et = rd_var(r), i;
    o->type = 4;
    if (n < 0 || n > (1 << 24)) { r->err = 1; return o; }
    o->n = n;
    o->items = (pm_obj **)calloc((size_t)n + 1, sizeof(pm_obj *));
    for (i = 0; i < n && !r->err; i++) o->items[i] = rd_typed(r, et == -1 ? rd_var(r) : et);
    return o;
}

static pm_obj *rd_typed(pm_rd *r, int t)   /* FUN_180021f70 */
{
    pm_obj *o;
    if (t == 2 || t == 3) return rd_string(r);
    if (t == 4 || t == 5) return rd_list(r);
    o = (pm_obj *)calloc(1, sizeof *o);
    o->type = t;
    if (t == 0) o->ival = rd_var(r);
    else if (t == 1) { int a = rd_u8(r), b = rd_u8(r); o->ival = (a << 8) | b; }
    else if (t == 6) {
        int32_t n = rd_var(r);
        if (n < 0 || r->p + n > r->end) r->err = 1; else r->p += n;
    } else r->err = 1;
    return o;
}

static int str_eq(const pm_obj *o, const char *s)
{
    int i;
    if (!o || o->type != 2) return 0;
    for (i = 0; i < o->n; i++)
        if (!s[i] || o->str[i] != (unsigned char)s[i]) return 0;
    return s[o->n] == 0;
}

static void mask_set(uint64_t m[2], int c)
{
    if (c >= 0 && c < 128) m[c >> 6] |= 1ULL << (c & 63);
}

static int mask_has(const uint64_t m[2], int c)
{
    return c >= 0 && c < 128 && (m[c >> 6] >> (c & 63)) & 1;
}

/* list members (DEFINED_LISTS entry named name) as an ASCII mask */
static int find_list(pm_obj *lists, const int *name, int nlen, uint64_t m[2])
{
    int i, k;
    m[0] = m[1] = 0;
    if (!lists || lists->type != 4) return 0;
    for (i = 0; i < lists->n; i++) {
        pm_obj *l = lists->items[i], *nm;
        if (!l || l->type != 4 || l->n < 1) continue;
        nm = l->items[0];
        if (!nm || nm->type != 2 || nm->n != nlen || memcmp(nm->str, name, sizeof(int) * (size_t)nlen)) continue;
        for (k = 1; k < l->n; k++)
            if (l->items[k] && l->items[k]->type == 2 && l->items[k]->n == 1) mask_set(m, l->items[k]->str[0]);
        return 1;
    }
    return 0;
}

/* FUN_1800210d8 (the symbol kinds this runtime supports) */
static pm_sym classify(zf1_pm *pm, const pm_obj *s, pm_obj *lists, uint64_t (*lmask)[2], int *nlm)
{
    pm_sym y;
    const int *c = s->str;
    int n = s->n;
    y.kind = SY_NONE;
    y.val = 0;
    if (n == 1) { y.kind = SY_CHAR; y.val = c[0]; return y; }
    if (n >= 3 && c[0] == '*' && c[n - 1] == '*') {
        if (str_eq(s, "*ALTCHAIN*") || str_eq(s, "**VECTOR_EPSILON**")) y.kind = SY_EPS;
        return y;
    }
    if (n >= 4 && c[0] == '@' && c[n - 1] == '@' && (c[1] == 'L' || c[1] == 'X') && c[2] == '.') {
        uint64_t m[2];
        if (find_list(lists, c + 3, n - 4, m)) {
            lmask[*nlm][0] = m[0];
            lmask[*nlm][1] = m[1];
            y.kind = c[1] == 'L' ? SY_LIST : SY_XLIST;
            y.val = (*nlm)++;
        }
        return y;
    }
    if (n >= 3 && c[0] == '<' && c[1] == '/' && c[n - 1] == '>') {
        int k;
        char *t = (char *)malloc((size_t)n - 2);
        for (k = 2; k < n - 1; k++) t[k - 2] = (char)(c[k] < 0x80 ? c[k] : '?');
        t[n - 3] = 0;
        pm->tags = (char **)realloc(pm->tags, sizeof(char *) * (size_t)(pm->ntags + 1));
        pm->tags[pm->ntags] = t;
        y.kind = SY_TAG;
        y.val = pm->ntags++;
        return y;
    }
    return y;   /* multi-character symbol, flag diacritic, ".#." ...: never matches ASCII input */
}

/* FUN_180069c00 arc stream: arcs of the state on top of a DFS stack; a byte with 0x40 carries the label index
 * (low 4 bits + 7 per 0x80 continuation), 0x20 = last arc of the state, 0x10 = target final; a byte without 0x40
 * is an arc with label 0.  The next byte decides the target: without 0x40 a varint state number (== the next
 * unused number: a new state without arcs), with 0x40 the next new state, whose arcs follow (pushed). */
static int read_arcs(pm_rd *r, int nstates, int narcs, uint32_t *src, uint16_t *lab, uint32_t *tgt, uint8_t *final)
{
    int *stack = (int *)malloc(sizeof(int) * (size_t)(nstates + 1)), sp = 0, next = 1, k = 0;
    stack[sp++] = 0;
    while (sp > 0 && !r->err) {
        int cur = stack[sp - 1], b = rd_u8(r), idx = 0, last = 0, fin = 0;
        if (b & 0x40) {
            idx = b & 0xf;
            last = b & 0x20;
            fin = b & 0x10;
            while ((b & 0x80) && !r->err) {
                b = rd_u8(r);
                idx = (idx << 7) | (b & 0x7f);
            }
        }
        if (k >= narcs || r->p >= r->end) { r->err = 1; break; }
        src[k] = (uint32_t)cur;
        lab[k] = (uint16_t)idx;
        if (!(*r->p & 0x40)) {
            int32_t t = rd_var(r);
            if (last) sp--;
            if (t == next) next++;
            if (t < 0 || t >= nstates) { r->err = 1; break; }
            tgt[k] = (uint32_t)t;
        } else {
            if (last) sp--;
            if (next >= nstates) { r->err = 1; break; }
            tgt[k] = (uint32_t)next;
            stack[sp++] = next++;
        }
        if (fin) final[tgt[k]] = 1;
        k++;
    }
    free(stack);
    return r->err ? -1 : k;
}

static uint32_t be(const uint8_t *p, int n)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

void zf1_pm_free(zf1_pm *pm)
{
    int i;
    if (!pm) return;
    free(pm->first); free(pm->alabel); free(pm->atarget); free(pm->final);
    free(pm->mask); free(pm->ltag); free(pm->leps);
    for (i = 0; i < pm->ntags; i++) free(pm->tags[i]);
    free(pm->tags);
    free(pm->stamp); free(pm->cur); free(pm->nxt);
    free(pm);
}

static void closure(zf1_pm *pm, int **setp, int *np, uint32_t *tagmask);

zf1_pm *zf1_pm_load(const uint8_t *blob, size_t size)
{
    zf1_pm *pm;
    pm_rd r;
    pm_obj *alpha = NULL, *delim = NULL, *lists = NULL, *syms = NULL;
    pm_sym *sy = NULL;
    uint64_t (*lmask)[2] = NULL;
    uint32_t *src = NULL;
    int32_t nstates, narcs, flags, nprops, nl, arity, i, nsym, nlm = 0, k;
    uint64_t sigma[2] = {0, 0};
    int hl, hl2, lic, key, netoff;
    if (!blob || size < 24) return NULL;
    if (blob[4] != 0x13 && blob[4] != 0x15) return NULL;
    hl = blob[6];
    key = (int)be(blob + 7, 4);
    hl2 = (int)be(blob + 11, 2);
    if (hl < 0x14 || hl2 < 0x14 || hl2 < hl || be(blob + 13, 3) < 1 || (size_t)hl2 + 4 > size) return NULL;
    lic = blob[16];
    if (lic) lic = (key & 0xff) < 0x81 ? lic - (key & 0xff) : (key & 0xff) - lic;
    netoff = (int)be(blob + hl2, 4);   /* first network */
    if (netoff <= 0 || (size_t)netoff >= size) return NULL;
    pm = (zf1_pm *)calloc(1, sizeof *pm);
    memset(&r, 0, sizeof r);
    r.base = blob + netoff;
    r.p = r.base;
    r.end = blob + size;
    r.fmt = blob[4];
    nstates = rd_var(&r);
    narcs = rd_var(&r);
    flags = rd_var(&r);
    nprops = rd_var(&r);
    if (nstates <= 0 || narcs < 0 || nprops < 0) r.err = 1;
    r.scr = lic == 1;
    for (i = 0; i < nprops && !r.err; i++) {
        pm_obj *kk = rd_string(&r), *v = rd_typed(&r, rd_var(&r));
        if (str_eq(kk, "PMATCH_ALPHA_LIST") && !alpha) { alpha = v; v = NULL; }
        else if (str_eq(kk, "PMATCH_DELIM_LIST") && !delim) { delim = v; v = NULL; }
        else if (str_eq(kk, "DEFINED_LISTS") && !lists) { lists = v; v = NULL; }
        obj_free(kk);
        obj_free(v);
    }
    if (!r.err) syms = rd_list(&r);
    nsym = syms ? syms->n + 2 : 2;
    sy = (pm_sym *)calloc((size_t)nsym, sizeof *sy);
    lmask = (uint64_t (*)[2])calloc((size_t)nsym + 1, sizeof *lmask);
    sy[0].kind = SY_EPS;
    sy[1].kind = SY_OTHER;
    for (i = 2; i < nsym && !r.err; i++) {
        pm_obj *s = syms->items[i - 2];
        sy[i].kind = SY_NONE;
        if (s && s->type == 2) sy[i] = classify(pm, s, lists, lmask, &nlm);
        if (sy[i].kind == SY_CHAR) mask_set(sigma, sy[i].val);
    }
    if (alpha && alpha->type == 4)
        for (i = 0; i < alpha->n; i++)
            if (alpha->items[i] && alpha->items[i]->type == 2 && alpha->items[i]->n == 1)
                mask_set(pm->alpha, alpha->items[i]->str[0]);
    if (delim && delim->type == 4)
        for (i = 0; i < delim->n; i++)
            if (delim->items[i] && delim->items[i]->type == 2 && delim->items[i]->n == 1)
                mask_set(pm->delim, delim->items[i]->str[0]);
    nl = r.err ? 0 : rd_var(&r);
    arity = r.err ? 0 : rd_var(&r);
    if (nl < 0 || nl > 0xffff || (arity != 1 && arity != 2)) r.err = 1;
    if (!r.err) {
        pm->nlabels = nl;
        pm->mask = (uint64_t (*)[2])calloc((size_t)nl + 1, sizeof *pm->mask);
        pm->ltag = (int16_t *)malloc(sizeof(int16_t) * ((size_t)nl + 1));
        pm->leps = (uint8_t *)calloc((size_t)nl + 1, 1);
        for (i = 0; i < nl && !r.err; i++) {
            int32_t up = rd_var(&r), in = arity == 2 ? rd_var(&r) : up;
            pm_sym a, b;
            if (up == in && up == -1) up = in = 1;
            else if (in == -1) in = up;
            if (up < 0 || up >= nsym || in < 0 || in >= nsym) { r.err = 1; break; }
            a = sy[up];
            b = sy[in];
            pm->ltag[i] = (int16_t)(a.kind == SY_TAG ? a.val : -1);
            switch (b.kind) {
            case SY_EPS: case SY_TAG: pm->leps[i] = 1; break;
            case SY_CHAR: mask_set(pm->mask[i], b.val); break;
            case SY_LIST: pm->mask[i][0] = lmask[b.val][0]; pm->mask[i][1] = lmask[b.val][1]; break;
            case SY_XLIST: pm->mask[i][0] = ~lmask[b.val][0]; pm->mask[i][1] = ~lmask[b.val][1]; break;
            case SY_OTHER: pm->mask[i][0] = ~sigma[0]; pm->mask[i][1] = ~sigma[1]; break;
            default: break;
            }
        }
    }
    if (!r.err) {
        pm->nstates = nstates;
        pm->final = (uint8_t *)calloc((size_t)nstates, 1);
        src = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(narcs + 1));
        pm->alabel = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)(narcs + 1));
        pm->atarget = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(narcs + 1));
        k = read_arcs(&r, nstates, narcs, src, pm->alabel, pm->atarget, pm->final);
        if (k != narcs) r.err = 1;
        if (flags & 0x100) pm->final[0] = 1;
        for (i = 0; i < narcs && !r.err; i++)
            if (pm->alabel[i] >= nl) r.err = 1;
    }
    if (!r.err) {
        /* byte count check (the count includes the varint itself), then CSR by source state (stable) */
        uint16_t *l2 = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)(narcs + 1));
        uint32_t *t2 = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(narcs + 1));
        int32_t chk = rd_var(&r);
        if (chk != (int32_t)(r.p - r.base)) r.err = 1;
        pm->first = (uint32_t *)calloc((size_t)nstates + 2, sizeof(uint32_t));
        for (i = 0; i < narcs; i++) pm->first[src[i] + 2]++;
        for (i = 0; i < nstates; i++) pm->first[i + 2] += pm->first[i + 1];
        for (i = 0; i < narcs; i++) {
            uint32_t d = pm->first[src[i] + 1]++;
            l2[d] = pm->alabel[i];
            t2[d] = pm->atarget[i];
        }
        free(pm->alabel);
        free(pm->atarget);
        pm->alabel = l2;
        pm->atarget = t2;
    }
    free(src);
    free(sy);
    free(lmask);
    obj_free(alpha);
    obj_free(delim);
    obj_free(lists);
    obj_free(syms);
    if (r.err) { zf1_pm_free(pm); return NULL; }
    pm->stamp = (uint32_t *)calloc((size_t)nstates, sizeof(uint32_t));
    pm->cur = (int *)malloc(sizeof(int) * (size_t)nstates);
    pm->nxt = (int *)malloc(sizeof(int) * (size_t)nstates);
    {
        /* characters with a transition out of the start closure */
        int n = 1, s, a;
        uint32_t tm = 0;
        pm->gen++;
        pm->cur[0] = 0;
        pm->stamp[0] = pm->gen;
        closure(pm, &pm->cur, &n, &tm);
        for (s = 0; s < n; s++)
            for (a = (int)pm->first[pm->cur[s]]; a < (int)pm->first[pm->cur[s] + 1]; a++) {
                pm->firstc[0] |= pm->mask[pm->alabel[a]][0];
                pm->firstc[1] |= pm->mask[pm->alabel[a]][1];
            }
    }
    return pm;
}

/* epsilon closure of set[0..*np) (members stamped with pm->gen); tag arcs set bits in *tagmask */
static void closure(zf1_pm *pm, int **setp, int *np, uint32_t *tagmask)
{
    int *set = *setp, i, a;
    for (i = 0; i < *np; i++) {
        int s = set[i];
        for (a = (int)pm->first[s]; a < (int)pm->first[s + 1]; a++) {
            int l = pm->alabel[a];
            uint32_t t;
            if (!pm->leps[l]) continue;
            t = pm->atarget[a];
            if (pm->ltag[l] >= 0) {
                if (pm->final[t] && pm->ltag[l] < 32) *tagmask |= 1u << pm->ltag[l];
                continue;
            }
            if (pm->stamp[t] != pm->gen) {
                pm->stamp[t] = pm->gen;
                set[(*np)++] = (int)t;
            }
        }
    }
}

/* a match may not start or end between two PMATCH_ALPHA characters nor between two characters that are neither
 * alpha nor PMATCH_DELIM ("other"); next to a delimiter any cut is allowed */
static int cut_ok(const zf1_pm *pm, int a, int b)
{
    int ca = mask_has(pm->alpha, a) ? 1 : mask_has(pm->delim, a) ? 2 : 0;
    int cb = mask_has(pm->alpha, b) ? 1 : mask_has(pm->delim, b) ? 2 : 0;
    return ca != cb || ca == 2;
}

int zf1_pm_next(zf1_pm *pm, const char *s, int n, int from, zf1_pm_match *out, int cap)
{
    int i;
    if (!pm) return 0;
    for (i = from; i < n; i++) {
        int j, nc, best = -1, k;
        uint32_t bestmask = 0, tm = 0;
        if (i > 0 && !cut_ok(pm, (unsigned char)s[i - 1], (unsigned char)s[i])) continue;
        if (!mask_has(pm->firstc, (unsigned char)s[i])) continue;
        pm->gen++;
        pm->cur[0] = 0;
        pm->stamp[0] = pm->gen;
        nc = 1;
        closure(pm, &pm->cur, &nc, &tm);
        for (j = i; j < n && nc; j++) {
            int c = (unsigned char)s[j], nn = 0, x, a;
            int *tmp;
            pm->gen++;
            for (x = 0; x < nc; x++) {
                int st = pm->cur[x];
                for (a = (int)pm->first[st]; a < (int)pm->first[st + 1]; a++) {
                    int l = pm->alabel[a];
                    uint32_t t = pm->atarget[a];
                    if (pm->leps[l] || !mask_has(pm->mask[l], c)) continue;
                    if (pm->stamp[t] != pm->gen) {
                        pm->stamp[t] = pm->gen;
                        pm->nxt[nn++] = (int)t;
                    }
                }
            }
            tm = 0;
            closure(pm, &pm->nxt, &nn, &tm);
            tmp = pm->cur; pm->cur = pm->nxt; pm->nxt = tmp;
            nc = nn;
            if (tm && (j + 1 >= n || cut_ok(pm, c, (unsigned char)s[j + 1]))) {
                best = j + 1;
                bestmask = tm;
            }
        }
        if (best < 0) continue;
        for (k = 0, j = 0; j < pm->ntags && j < 32; j++)
            if ((bestmask >> j) & 1) {
                if (k < cap) {
                    out[k].off = i;
                    out[k].len = best - i;
                    out[k].tag = pm->tags[j];
                }
                k++;
            }
        return k < cap ? k : cap;
    }
    return 0;
}
