/* zf1_fst.c - TransducerNetwork runtime (see zf1_fst.h, notes/fe1_tn.md).  Portable C99.
 *
 * Engine references (MSTTSEngine_OneCore.dll 10.3.21207.0):
 *   reader     FUN_18013f810 (magic) + FUN_1800302b8 (header, 8 record arrays, 3 string/byte pools)
 *   lattice    FUN_1801390a0 (text copy), tokenizer PTR_FUN_180178870: FUN_1801427e0 start / FUN_1801425f0 end,
 *              FUN_180138c84 next token, FUN_180068130 "special token", FUN_180138ec0 / FUN_180138d28 lexicon words
 *   parser     FUN_18000f2d4 (driver, agenda), FUN_18000fc40 (process one edge), FUN_180011ad0 (add edge),
 *              FUN_1800114e0 / FUN_180011690 (processed-edge tables), edge hash FUN_18003a9e0 / eq FUN_18004a0b0
 *   tree       FUN_18000e440 / FUN_18000e708 / FUN_18000ecb4, leaves FUN_180066868 -> FUN_18013f1c0 ->
 *              FUN_18013f220 -> FUN_18013ef00, categories FUN_1800433c8
 */
#include "zf1_fst.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ClassicVoices patch: function-level scratch is per thread (two voices may speak on two threads at once). */
#ifndef ZF_TLS
#define ZF_TLS __thread
#endif

int zf1_net_trace = 0;
/* ClassicVoices patch: statistics counters per thread (every engine incremented the same globals) */
ZF_TLS long zf1_net_stat_edges = 0, zf1_net_stat_parses = 0;
ZF_TLS long long zf1_st_act = 0, zf1_st_comp = 0, zf1_st_gather = 0, zf1_st_term = 0, zf1_st_lex = 0, zf1_st_tree = 0;

/* ------------------------------------------------------------------ character classes (UCRT tables) */
static const uint16_t zf_wspace_ranges[] = {0x0009,0x000d,0x0020,0x0020,0x0085,0x0085,0x00a0,0x00a0,0x1680,0x1680,
    0x180e,0x180e,0x2000,0x200a,0x2028,0x2029,0x202f,0x202f,0x205f,0x205f,0x3000,0x3000};
#include "zf1_walpha.h"
#include "zf1_wcase.h"

static int in_ranges(const uint16_t *r, int n, zf_char c)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        if (c < r[2 * m]) hi = m - 1;
        else if (c > r[2 * m + 1]) lo = m + 1;
        else return 1;
    }
    return 0;
}
int zf1_iswspace(zf_char c) { return in_ranges(zf_wspace_ranges, (int)(sizeof zf_wspace_ranges / 4), c); }
int zf1_iswalpha(zf_char c) { return in_ranges(zf_walpha_ranges, zf_walpha_N, c); }
int zf1_iswupper(zf_char c) { return in_ranges(zf_wupper_ranges, zf_wupper_N, c); }
int zf1_iswlower(zf_char c) { return in_ranges(zf_wlower_ranges, zf_wlower_N, c); }

/* ------------------------------------------------------------------ network */
typedef struct zrule {
    uint32_t name, bstate, nstates, cbase;
    uint16_t mask;                      /* +0x0a */
    uint32_t w10, script, cat, x1c, catflag, x24, attrs, x2c, cattype, first, nullable, x3c;
} zrule;

struct zf1_net {
    const uint8_t *blob;
    size_t size;
    uint32_t v1, v2;
    uint32_t nsym, nB, nC, nD, nE, nF, nG, nH;
    uint16_t d0, d2;
    zrule *rules;
    const uint8_t *B, *C, *D, *E, *F, *G, *H;
    zf_char *s1; uint32_t n1;           /* string pool 1 (labels, category names, first-char sets) */
    zf_char *s2; uint32_t n2;           /* string pool 2 (rule names) */
    const uint8_t *b3; uint32_t n3;     /* byte pool (arc lists, label lists, attribute lists) */
    int mask;
    zf1_net_lexfn lexfn;
    void *lexctx;
    void *ws;                           /* reusable parser buffers (struct zparse), see zf1_net_parse */
};

static const uint8_t NET_MAGIC[16] = {0x78,0x90,0x33,0x8c,0x86,0xfb,0xeb,0x40,0x84,0xde,0x7f,0xb7,0xad,0xd3,0x74,0x06};

zf1_net *zf1_net_load(const uint8_t *blob, size_t size)
{
    zf1_net *n;
    size_t p = 16;
    uint32_t i, cnt[7];
    const uint8_t *pools[3] = {0, 0, 0};
    uint32_t psz[3] = {0, 0, 0};
    if (!blob || size < 64 || memcmp(blob, NET_MAGIC, 16)) return NULL;
    n = (zf1_net *)calloc(1, sizeof *n);
    if (!n) return NULL;
    n->blob = blob;
    n->size = size;
    n->v1 = zf_rd32(blob + p); n->v2 = zf_rd32(blob + p + 4); p += 8;
    n->nsym = zf_rd16(blob + p); p += 4;
    for (i = 0; i < 7; i++, p += 4) cnt[i] = zf_rd32(blob + p);
    n->nB = cnt[0]; n->nC = cnt[1]; n->nD = cnt[2]; n->nE = cnt[3]; n->nF = cnt[4]; n->nG = cnt[5]; n->nH = cnt[6];
    n->d0 = zf_rd16(blob + p); n->d2 = zf_rd16(blob + p + 2); p += 4;
    {
        const uint8_t *A = blob + p;
        p += (size_t)64 * n->nsym;
        n->rules = (zrule *)calloc(n->nsym ? n->nsym : 1, sizeof(zrule));
        for (i = 0; i < n->nsym; i++) {
            const uint8_t *r = A + 64 * (size_t)i;
            zrule *z = &n->rules[i];
            z->name = zf_rd32(r); z->bstate = zf_rd32(r + 4); z->nstates = zf_rd16(r + 8); z->mask = zf_rd16(r + 10);
            z->cbase = zf_rd32(r + 12); z->w10 = zf_rd32(r + 16); z->script = zf_rd32(r + 20); z->cat = zf_rd32(r + 24);
            z->x1c = zf_rd32(r + 28); z->catflag = zf_rd32(r + 32); z->x24 = zf_rd32(r + 36); z->attrs = zf_rd32(r + 40);
            z->x2c = zf_rd32(r + 44); z->cattype = zf_rd32(r + 48); z->first = zf_rd32(r + 52);
            z->nullable = zf_rd32(r + 56); z->x3c = zf_rd32(r + 60);
        }
    }
    n->B = blob + p; p += (size_t)12 * n->nB;
    n->C = blob + p; p += (size_t)16 * n->nC;
    n->D = blob + p; p += (size_t)4 * n->nD;
    n->E = blob + p; p += (size_t)28 * n->nE;
    n->F = blob + p; p += (size_t)8 * n->nF;
    n->G = blob + p; p += (size_t)4 * n->nG;
    n->H = blob + p; p += (size_t)12 * n->nH;
    for (i = 0; i < 3; i++) {
        uint32_t f;
        if (p + 4 > size) goto bad;
        f = zf_rd32(blob + p); p += 4;
        if (f) {
            psz[i] = zf_rd32(blob + p); p += 4;
            pools[i] = blob + p;
            p += psz[i];
            if (p > size) goto bad;
        }
    }
    n->n1 = psz[0] / 2;
    n->s1 = (zf_char *)malloc((n->n1 + 1) * sizeof(zf_char));
    for (i = 0; i < n->n1; i++) n->s1[i] = zf_rd16(pools[0] + 2 * i);
    n->s1[n->n1] = 0;
    n->n2 = psz[1] / 2;
    n->s2 = (zf_char *)malloc((n->n2 + 1) * sizeof(zf_char));
    for (i = 0; i < n->n2; i++) n->s2[i] = zf_rd16(pools[1] + 2 * i);
    n->s2[n->n2] = 0;
    n->b3 = pools[2];
    n->n3 = psz[2];
    n->mask = 1;
    return n;
bad:
    zf1_net_free(n);
    return NULL;
}

static void ws_free(void *ws);
void zf1_net_free(zf1_net *n)
{
    if (!n) return;
    ws_free(n->ws);
    free(n->rules);
    free(n->s1);
    free(n->s2);
    free(n);
}

void zf1_net_set_lexicon(zf1_net *n, zf1_net_lexfn fn, void *ctx) { n->lexfn = fn; n->lexctx = ctx; }
void zf1_net_set_mask(zf1_net *n, int mask) { n->mask = mask; }

/* string pool 1 access (FUN_18003aa40): offset 0 or out of range -> NULL */
static const zf_char *s1str(const zf1_net *n, uint32_t off) { return (off && off < n->n1) ? n->s1 + off : NULL; }

/* arcs */
static inline const uint8_t *ARC(const zf1_net *n, uint32_t i) { return n->C + 16 * (size_t)i; }
/* D list: offset into b3: {u16 count, u16, u32 items[count]} */
static inline const uint8_t *DLIST(const zf1_net *n, uint32_t di) { return n->b3 + zf_rd32(n->D + 4 * (size_t)di); }
static inline const uint8_t *LABEL(const zf1_net *n, uint32_t ei) { return n->E + 28 * (size_t)ei; }

/* ------------------------------------------------------------------ lattice (mode 0) */
/* tokenizer predicates (lattice tokenizer object, FUN_1801393d0): mode 0/1 = PTR_FUN_180178870
 * (FUN_1801427e0 / FUN_1801425f0), mode 3 = PTR_FUN_180178840 (FUN_1801426c0 / FUN_1801424e0).
 * Modes 2 and 4 (PTR_FUN_180178858 / 180178828) are not used by the en-US TN/NE and are not ported. */
static int m3_end_punct(const zf_char *t, int len, int pos)   /* FUN_180142344 */
{
    zf_char c = t[pos];
    unsigned u = (unsigned)(uint16_t)(c - 0x29);
    if ((u <= 0x34 && ((0x10000000200001ULL >> u) & 1)) || c == '}') return 1;
    if (c > 0x3f) return 0;
    if (!((0x8c00508600000000ULL >> c) & 1)) return 0;
    if ((len <= pos + 2 || !(t[pos] == '.' && t[pos + 1] == '.' && t[pos + 2] == '.')) && pos + 1 < len)
        return zf1_iswspace(t[pos + 1]);
    return 1;
}

int zf1_lat_start_ok(int tokmode, const zf_char *t, int len, int pos)
{
    if (pos == 0) return 1;
    if (pos >= len) return 0;
    if (tokmode == 3) {
        zf_char p = t[pos - 1];
        if (zf1_iswspace(p)) return 1;
        if (p == '(' || p == '<' || p == '[' || p == '{') return 1;
        if (p != '"' && p != 0x27) return 0;
        if (pos - 1 != 0 && !zf1_iswspace(t[pos - 2])) return 0;
        return 1;
    }
    if (zf1_iswalpha(t[pos - 1]) && zf1_iswalpha(t[pos])) return 0;
    return 1;
}

int zf1_lat_end_ok(int tokmode, const zf_char *t, int len, int pos)
{
    if (pos == 0) return 0;
    if (pos == len) return 1;
    if (pos > len) return 0;
    if (tokmode == 3) {
        if (zf1_iswspace(t[pos - 1])) return 1;
        if (zf1_iswspace(t[pos])) return 1;
        return m3_end_punct(t, len, pos);
    }
    return !zf1_iswalpha(t[pos - 1]) || !zf1_iswalpha(t[pos]);
}

int zf1_lat_next(int tokmode, const zf_char *t, int len, int pos)   /* FUN_180139680 -> FUN_180138c84 */
{
    zf_char c = t[pos];
    int p;
    if ((uint16_t)(c + 0x2800u) < 0x400) return pos + 2;
    p = pos + 1;
    if (c == ' ') {
        while (p < len && t[p] == ' ') p++;
    } else {
        while (p < len && !zf1_lat_start_ok(tokmode, t, len, p)) p++;
        while (p < len && t[p] == ' ') p++;
    }
    return p;
}

/* ------------------------------------------------------------------ parser state */
typedef struct zedge {
    uint8_t flags;
    uint16_t rule, state, start, end, cost, c1, c2, next;
} zedge;

typedef struct zlist { uint16_t *v; int n, cap; } zlist;

typedef struct zagnode { int next, prev; uint16_t id; } zagnode;

typedef struct zparse {
    const zf1_net *net;
    const zf_char *text;
    int len;
    int mode;                           /* engine param_4 */
    int tokmode;                        /* lattice tokenizer mode */
    uint32_t catoff;                    /* category offset in pool 1 (engine param_5) */
    uint16_t start;
    uint16_t base, maxn;                /* edge ids: base .. base+maxn-1 */
    zedge *e;
    int ne;
    /* processed-edge key table (open addressing on edge ids) */
    uint32_t *ht;
    uint32_t htcap, htn;
    uint32_t *htused;                   /* occupied slots (cleared at the next parse) */
    uint32_t htusedcap;
    int ecap;
    zlist *comp_by_start, *act_by_end;  /* per position */
    int npos;
    /* agenda: singly linked list of nodes */
    zagnode *ag;
    int agn, agcap, aghead, agfree;
    int *bhead, *btail;                 /* agenda buckets per cost */
    uint64_t *bmap;                     /* non-empty buckets */
    int bmin;                           /* lowest bitmap word that may be non-empty */
    /* index of processed active edges by (end position, awaited rule): entries in (edge, arc) order */
    struct zmap {
        uint32_t *key; int *val; uint32_t cap, n;
        struct zwlist { struct zwent { uint16_t aid, next, cost; } *v; int n, cap; } *l;
        int nl, capl;
    } wm, cm;                           /* wm: waiting actives by (end, rule); cm: complete edges by (start, rule) */
    uint16_t best;
    /* scratch */
    struct { uint16_t sym, next, cost; } *nt;
    int ntcap;
} zparse;

#define E_OF(P, id) (&(P)->e[(uint16_t)((id) - (P)->base)])
#define ERR_FULL (-2)       /* 0x80041003: stop, keep best */
#define ERR_FATAL (-1)

static uint32_t key_hash(const zedge *e)
{
    return (uint32_t)e->rule * 0x7e0f81u + (uint32_t)e->end * 0x162c613fu + ((e->flags >> 1) & 1u) * 0x2e86d0bfu +
           (uint32_t)e->start * 0x1003fu + (e->flags & 1u) * 0x43ec5f01u + (uint32_t)e->state;
}

static int key_eq(const zedge *a, const zedge *b)
{
    return a->end == b->end && !((a->flags ^ b->flags) & 3) && a->rule == b->rule && a->start == b->start &&
           a->state == b->state;
}

/* Key map: edge key -> slot value.  value = edge id (processed edge, FUN_1800114e0 tables) or
 * AGF | agenda node index (edge waiting in the agenda).  Keys are never removed within one parse. */
#define AGF 0x80000000u

static uint32_t ht_slot(zparse *P, const zedge *k)   /* slot index of key k (occupied or first empty) */
{
    uint32_t h = key_hash(k) & (P->htcap - 1);
    while (P->ht[h]) {
        uint32_t v = P->ht[h];
        const zedge *x = (v & AGF) ? E_OF(P, P->ag[v & ~AGF].id) : E_OF(P, v);
        if (key_eq(x, k)) return h;
        h = (h + 1) & (P->htcap - 1);
    }
    return h;
}

static void ht_grow(zparse *P)
{
    uint32_t *old = P->ht, oc = P->htcap, i;
    P->htcap = oc * 2;
    P->ht = (uint32_t *)calloc(P->htcap, sizeof(uint32_t));
    P->htn = 0;
    for (i = 0; i < oc; i++) {
        if (old[i]) {
            uint32_t v = old[i];
            const zedge *x = (v & AGF) ? E_OF(P, P->ag[v & ~AGF].id) : E_OF(P, v);
            uint32_t h = ht_slot(P, x);
            P->ht[h] = v;
            if (P->htn >= P->htusedcap) {
                P->htusedcap = P->htusedcap ? P->htusedcap * 2 : 1024;
                P->htused = (uint32_t *)realloc(P->htused, sizeof(uint32_t) * P->htusedcap);
            }
            P->htused[P->htn++] = h;
        }
    }
    free(old);
}

/* store value v for the key of edge x (new key or overwrite) */
static void ht_set(zparse *P, const zedge *x, uint32_t v)
{
    uint32_t h;
    if ((P->htn + 1) * 2 > P->htcap) ht_grow(P);
    h = ht_slot(P, x);
    if (!P->ht[h]) {
        if (P->htn >= P->htusedcap) {
            P->htusedcap = P->htusedcap ? P->htusedcap * 2 : 1024;
            P->htused = (uint32_t *)realloc(P->htused, sizeof(uint32_t) * P->htusedcap);
        }
        P->htused[P->htn++] = h;
    }
    P->ht[h] = v;
}

/* processed edge (FUN_180011690 insert, replaces the agenda entry of the same key) */
static void ht_insert(zparse *P, uint16_t id) { ht_set(P, E_OF(P, id), id); }

static void list_add(zlist *l, uint16_t v)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = (uint16_t *)realloc(l->v, sizeof(uint16_t) * (size_t)l->cap);
    }
    l->v[l->n++] = v;
}

static int new_edge(zparse *P, const zedge *src)   /* returns id or ERR_FULL */
{
    if (P->ne >= P->maxn) return ERR_FULL;
    if (P->ne >= P->ecap) {
        P->ecap = P->ecap ? P->ecap * 2 : 1024;
        if (P->ecap > P->maxn) P->ecap = P->maxn;
        P->e = (zedge *)realloc(P->e, sizeof(zedge) * (size_t)P->ecap);
    }
    P->e[P->ne] = *src;
    return P->base + P->ne++;
}

/* ---- agenda: the engine keeps one list sorted by cost (new edges before the first strictly greater cost,
 * cheaper replacements before the first cost >= theirs).  Equivalent here: one FIFO bucket per cost value,
 * new edges appended to their bucket, moved edges put at the head of their new bucket, pop = head of the
 * cheapest non-empty bucket. */
static int ag_alloc(zparse *P, uint16_t id)
{
    int k;
    if (P->agfree >= 0) {
        k = P->agfree;
        P->agfree = P->ag[k].next;
    } else {
        if (P->agn == P->agcap) {
            P->agcap = P->agcap ? P->agcap * 2 : 256;
            P->ag = (zagnode *)realloc(P->ag, sizeof(zagnode) * (size_t)P->agcap);
        }
        k = P->agn++;
    }
    P->ag[k].next = -1;
    P->ag[k].prev = -1;
    P->ag[k].id = id;
    return k;
}

static void bucket_mark(zparse *P, uint16_t c) { P->bmap[c >> 6] |= 1ULL << (c & 63); }

static void ag_unlink(zparse *P, int k, uint16_t c)
{
    int p = P->ag[k].prev, n = P->ag[k].next;
    if (p >= 0) P->ag[p].next = n; else P->bhead[c] = n;
    if (n >= 0) P->ag[n].prev = p; else P->btail[c] = p;
    if (P->bhead[c] < 0) P->bmap[c >> 6] &= ~(1ULL << (c & 63));
}

static void ag_push_tail(zparse *P, int k, uint16_t c)
{
    if (!(P->bmap[c >> 6] & (1ULL << (c & 63)))) { P->bhead[c] = P->btail[c] = -1; bucket_mark(P, c); }
    P->ag[k].next = -1;
    P->ag[k].prev = P->btail[c];
    if (P->btail[c] >= 0) P->ag[P->btail[c]].next = k; else P->bhead[c] = k;
    P->btail[c] = k;
}

static void ag_push_head(zparse *P, int k, uint16_t c)
{
    if (!(P->bmap[c >> 6] & (1ULL << (c & 63)))) { P->bhead[c] = P->btail[c] = -1; bucket_mark(P, c); }
    P->ag[k].prev = -1;
    P->ag[k].next = P->bhead[c];
    if (P->bhead[c] >= 0) P->ag[P->bhead[c]].prev = k; else P->btail[c] = k;
    P->bhead[c] = k;
}

static int ag_pop(zparse *P)   /* node index or -1 */
{
    int w, k;
    for (w = P->bmin; w < 1024; w++) {
        uint64_t m = P->bmap[w];
        if (m) {
            int b = 0;
            uint16_t c;
            while (!(m & 1)) { m >>= 1; b++; }
            c = (uint16_t)(w * 64 + b);
            k = P->bhead[c];
            ag_unlink(P, k, c);
            P->bmin = w;
            return k;
        }
    }
    P->bmin = 1024;
    return -1;
}

/* FUN_180011ad0: add a derived edge */
static struct zwlist *map_list(struct zmap *M, uint32_t key, int create)
{
    uint32_t h;
    if (create && (M->n + 1) * 2 > M->cap) {
        uint32_t *ok = M->key, oc = M->cap, i;
        int *ov = M->val;
        M->cap = oc ? oc * 2 : 1024;
        M->key = (uint32_t *)calloc(M->cap, sizeof(uint32_t));
        M->val = (int *)calloc(M->cap, sizeof(int));
        for (i = 0; i < oc; i++)
            if (ok[i]) {
                h = (ok[i] * 2654435761u) & (M->cap - 1);
                while (M->key[h]) h = (h + 1) & (M->cap - 1);
                M->key[h] = ok[i]; M->val[h] = ov[i];
            }
        free(ok); free(ov);
    }
    if (!M->cap) return NULL;
    key++;   /* 0 = empty */
    h = (key * 2654435761u) & (M->cap - 1);
    while (M->key[h]) {
        if (M->key[h] == key) return &M->l[M->val[h]];
        h = (h + 1) & (M->cap - 1);
    }
    if (!create) return NULL;
    if (M->nl == M->capl) {
        int o = M->capl;
        M->capl = o ? o * 2 : 256;
        M->l = realloc(M->l, sizeof(struct zwlist) * (size_t)M->capl);
        memset(M->l + o, 0, sizeof(struct zwlist) * (size_t)(M->capl - o));
    }
    M->l[M->nl].n = 0;
    M->key[h] = key; M->val[h] = M->nl; M->n++;
    return &M->l[M->nl++];
}

static void map_add(struct zmap *M, uint32_t key, uint16_t a, uint16_t b, uint16_t c)
{
    struct zwlist *w = map_list(M, key, 1);
    if (w->n == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 8;
        w->v = realloc(w->v, sizeof(struct zwent) * (size_t)w->cap);
    }
    w->v[w->n].aid = a; w->v[w->n].next = b; w->v[w->n].cost = c;
    w->n++;
}

static void map_reset(struct zmap *M) { if (M->key) memset(M->key, 0, sizeof(uint32_t) * M->cap); M->n = 0; M->nl = 0; }

static void map_free(struct zmap *M)
{
    int i;
    free(M->key); free(M->val);
    for (i = 0; i < M->capl; i++) free(M->l[i].v);
    free(M->l);
}

static int add_edge(zparse *P, const zedge *ne)
{
    uint32_t h, v;
    if ((P->htn + 1) * 2 > P->htcap) ht_grow(P);
    h = ht_slot(P, ne);
    v = P->ht[h];
    if (v && !(v & AGF)) {
        zedge *X = E_OF(P, v);
        int nid;
        if (X->c1 == ne->c1 && X->c2 == ne->c2) return 0;
        nid = new_edge(P, ne);
        if (nid < 0) return nid;
        X = E_OF(P, v);
        E_OF(P, nid)->next = X->next;
        X->next = (uint16_t)nid;
        return 0;
    }
    if (v) {
        int k = (int)(v & ~AGF), alt = -1;
        zedge *X = E_OF(P, P->ag[k].id);
        uint16_t oldc = X->cost;
        if (X->c1 != ne->c1 || X->c2 != ne->c2) {
            alt = new_edge(P, ne);
            if (alt < 0) return alt;
            X = E_OF(P, P->ag[k].id);
        }
        if (ne->cost < oldc) {
            if (alt < 0) *X = *ne;
            else {
                E_OF(P, alt)->next = P->ag[k].id;
                P->ag[k].id = (uint16_t)alt;
            }
            ag_unlink(P, k, oldc);
            ag_push_head(P, k, ne->cost);
            if ((ne->cost >> 6) < P->bmin) P->bmin = ne->cost >> 6;
        } else if (alt >= 0) {
            E_OF(P, alt)->next = X->next;
            X->next = (uint16_t)alt;
        }
        return 0;
    }
    {
        int id = new_edge(P, ne), k;
        if (id < 0) return id;
        k = ag_alloc(P, (uint16_t)id);
        ag_push_tail(P, k, ne->cost);
        if ((ne->cost >> 6) < P->bmin) P->bmin = ne->cost >> 6;
        if (!P->ht[h]) {
            if (P->htn >= P->htusedcap) {
                P->htusedcap = P->htusedcap ? P->htusedcap * 2 : 1024;
                P->htused = (uint32_t *)realloc(P->htused, sizeof(uint32_t) * P->htusedcap);
            }
            P->htused[P->htn++] = h;
        }
        P->ht[h] = AGF | (uint32_t)k;
    }
    return 0;
}

static zlist *poslist(zparse *P, zlist **arr, int pos)
{
    if (pos >= P->npos) {
        int nn = pos + 21, i;
        P->comp_by_start = (zlist *)realloc(P->comp_by_start, sizeof(zlist) * (size_t)nn);
        P->act_by_end = (zlist *)realloc(P->act_by_end, sizeof(zlist) * (size_t)nn);
        for (i = P->npos; i < nn; i++) {
            memset(&P->comp_by_start[i], 0, sizeof(zlist));
            memset(&P->act_by_end[i], 0, sizeof(zlist));
        }
        P->npos = nn;
    }
    return &(*arr)[pos];
}

/* rule filter used for category rules (+0x20 flag) */
static int cat_ok(const zparse *P, const zrule *R)
{
    if (!R->catflag) return 1;
    if ((uint32_t)P->mode == R->cattype || P->mode == 0 || R->cattype == 0)
        return P->catoff ? (R->cat == P->catoff) : 1;
    return 0;
}

static int mask_ok(const zparse *P, const zrule *R)
{
    if (P->net->mask == 0 || R->mask == 0) return 1;
    return (R->mask & (uint16_t)P->net->mask) != 0;
}

static int first_ok(const zparse *P, const zrule *R, int pos)
{
    const zf_char *fc;
    zf_char ch;
    if (R->nullable || R->x3c) return 1;
    if (pos >= P->len) return 0;
    fc = s1str(P->net, R->first);
    if (!fc) return 0;
    ch = P->text[pos];
    if (!ch) return 0;   /* wcschr would match the terminator; pos < len so ch != 0 unless embedded NUL */
    for (; *fc; fc++)
        if (*fc == ch) return 1;
    return 0;
}

/* gather the rule-call arcs of (rule,state): (sym, next, cost), arcs whose cost == 0xffff are skipped */
static int gather_nt(zparse *P, const zrule *R, uint16_t state)
{
    zf1_st_gather++;
    const zf1_net *n = P->net;
    const uint8_t *st = n->B + 12 * (size_t)(R->bstate + state);
    uint32_t off = zf_rd32(st + 4);
    uint16_t cnt = zf_rd16(st + 8), i;
    const uint8_t *lst = n->b3 + off + 4;
    int k = 0;
    if (P->ntcap < cnt) {
        P->ntcap = cnt + 16;
        P->nt = realloc(P->nt, sizeof(*P->nt) * (size_t)P->ntcap);
    }
    for (i = 0; i < cnt; i++) {
        const uint8_t *a = ARC(n, R->cbase + zf_rd16(lst + 4 * i));
        if (zf_rd16(a + 12) != 0xFFFF) {
            P->nt[k].sym = zf_rd16(a + (P->mode == 1 && 0 ? 10 : 8));   /* lattice mode 0: +8 */
            P->nt[k].next = zf_rd16(a + 2);
            P->nt[k].cost = zf_rd16(a + 12);
            k++;
        }
    }
    return k;
}

static int wcsnicmp_ascii(const zf_char *a, const zf_char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        zf_char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 1;
        if (!x) return 0;
    }
    return 0;
}

static int wcsncmp16(const zf_char *a, const zf_char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return 1;
        if (!a[i]) return 0;
    }
    return 0;
}

/* FUN_18013ee34: constraint check of an attribute list against the network attribute list G[g] */
static int attr_ok(const zf1_net *n, uint32_t g, const uint32_t *pairs, int np)
{
    const uint8_t *gl;
    int gc, gi = 0, pi = 0;
    if (g == 0xFFFFFFFFu || g >= n->nG) return 1;
    gl = n->b3 + zf_rd32(n->G + 4 * (size_t)g);
    gc = zf_rd16(gl);
    if (gc == 0 || np == 0) return 1;
    while (gi < gc && pi < np) {
        const uint8_t *hr = n->H + 12 * (size_t)zf_rd32(gl + 4 + 4 * gi);
        uint16_t hid = zf_rd16(hr), hval = zf_rd16(hr + 2), pol = zf_rd16(hr + 8);
        uint16_t pid = (uint16_t)(pairs[pi] & 0xFFFF), pval = (uint16_t)(pairs[pi] >> 16);
        if (hid == pid) {
            if (hval != 0 && (uint16_t)(hval == pval) != (uint16_t)(pol & 0xFF)) return 0;
            gi++;
        } else if (pid <= hid) {
            pi++;
        } else {
            gi++;
        }
    }
    return 1;
}

#define LEX_MAXIT 16
#define LEX_MAXP 16
/* word lookup cache for one parse (FUN_180138d28 keeps the entries on the lattice) */
static int lex_lookup(zparse *P, int pos, int wl, uint32_t *pairs, int *counts)
{
    zf1_st_lex++;
    const zf1_net *n = P->net;
    return n->lexfn(n->lexctx, P->text + pos, wl, pairs, counts, LEX_MAXIT, LEX_MAXP);
}

/* lexicon word at pos (lattice vt+0x38 = FUN_180138ec0, FUN_180139b5c).  Engine quirk kept: the final lookup
 * uses the length INCLUDING the leading whitespace, starting after the whitespace (FUN_180138d28 bounds-checks
 * it against the text length). */
static int lex_match(zparse *P, int pos, const uint8_t *label, int *mlen)
{
    const zf1_net *n = P->net;
    int ws = 0, L, wl, ni, i;
    uint32_t pairs[LEX_MAXIT * LEX_MAXP];
    int counts[LEX_MAXIT];
    uint32_t g = zf_rd32(label + 24);
    if (!n->lexfn || pos > P->len) return 0;
    while (pos + ws < P->len && zf1_iswspace(P->text[pos + ws])) ws++;
    L = ws;
    while (pos + L < P->len && P->text[pos + L] && !zf1_iswspace(P->text[pos + L])) L++;
    wl = L - ws;
    if (wl > 1 && (pos + ws >= P->len || lex_lookup(P, pos + ws, wl, pairs, counts) < 0)) {
        zf_char c = P->text[pos + L - 1];
        unsigned u = (unsigned)(uint16_t)(c - 0x21);
        if ((u < 0x3d && ((0x1000000046002903ULL >> u) & 1)) || c == 0x7d) {
            if (lex_lookup(P, pos + ws, wl - 1, pairs, counts) >= 0) L--;
        }
    }
    if (pos + ws >= P->len || pos + ws + L > P->len || L <= 0) return 0;
    ni = lex_lookup(P, pos + ws, L, pairs, counts);
    if (ni < 0) return 0;
    *mlen = L;
    if (ni == 0 || g == 0xFFFFFFFFu) return 1;
    for (i = 0; i < ni; i++)
        if (attr_ok(n, g, pairs + i * LEX_MAXP, counts[i])) return 1;
    return 0;
}

/* FUN_18000fc40 */
static int process(zparse *P, uint16_t id)
{
    const zf1_net *n = P->net;
    zedge e = *E_OF(P, id);
    if (zf1_net_trace & 1)
        fprintf(stderr, "EDGE %u fl=%u rule=%u st=%u %u-%u cost=%u c=%u,%u\n", id, e.flags, e.rule, e.state, e.start,
                e.end, e.cost, e.c1, e.c2);
    if (e.state == 0xFFFF) {
        /* complete edge */
        const zrule *R = e.rule < n->nsym ? &n->rules[e.rule] : NULL;
        int ok1 = 1, ok2 = 1, j, slow;
        zlist *al;
        list_add(poslist(P, &P->comp_by_start, e.start), id);
        map_add(&P->cm, ((uint32_t)e.start << 16) | e.rule, id, 0, 0);
        ht_insert(P, id);
        if (e.rule == 0 && e.start == P->start && !(e.flags & 3)) {
            const zedge *B = P->best != 0xFFFF ? E_OF(P, P->best) : NULL;
            if ((!B || B->end < e.end || (B->end == e.end && e.cost < B->cost)) && zf1_lat_end_ok(P->tokmode, P->text, P->len, e.end))
                P->best = id;
        }
        if (R) {
            ok1 = cat_ok(P, R);
            if (P->net->mask && R->mask) ok2 = (R->mask & (uint16_t)P->net->mask) != 0;
        }
        if (e.start >= P->npos) return 0;
        al = &P->act_by_end[e.start];
        slow = !ok1;
        for (j = 0; j < al->n; j++) {
            uint16_t aid = al->v[j];
            zf1_st_act++;
            zedge A = *E_OF(P, aid);
            if (!ok1) {
                const zrule *RA = A.rule < n->nsym ? &n->rules[A.rule] : NULL;
                if (!RA || !RA->catflag || RA->name != 1) ok1 = 1;
                else continue;
            }
            if (ok2 && ok1 && !slow) {
                /* fast path: only the active edges waiting for this rule (same order as the full scan) */
                struct zwlist *w = map_list(&P->wm, ((uint32_t)e.start << 16) | e.rule, 0);
                int q;
                for (q = 0; w && q < w->n; q++) {
                    uint16_t aid2 = w->v[q].aid;
                    zedge A2 = *E_OF(P, aid2);
                    if (!(((e.flags >> 1) ^ A2.flags) & 1)) {
                        uint16_t c = (uint16_t)(A2.cost + e.cost), c2;
                        zedge ne;
                        int r;
                        if (c < A2.cost) return ERR_FULL;
                        c2 = (uint16_t)(c + w->v[q].cost);
                        if (c2 < c) return ERR_FULL;
                        ne.flags = (uint8_t)((e.flags & 1) | (A2.flags & 2));
                        ne.rule = A2.rule; ne.state = w->v[q].next; ne.start = A2.start; ne.end = e.end;
                        ne.cost = c2; ne.c1 = aid2; ne.c2 = id; ne.next = 0xFFFF;
                        r = add_edge(P, &ne);
                        if (r < 0) return r;
                    }
                }
                return 0;
            }
            if (ok2) {
                int k, cnt = gather_nt(P, &n->rules[A.rule], A.state);
                for (k = 0; k < cnt; k++) {
                    if (P->nt[k].sym == e.rule && !(((e.flags >> 1) ^ A.flags) & 1)) {
                        uint16_t c = (uint16_t)(A.cost + e.cost), c2;
                        zedge ne;
                        int r;
                        if (c < A.cost) return ERR_FULL;
                        c2 = (uint16_t)(c + P->nt[k].cost);
                        if (c2 < c) return ERR_FULL;
                        ne.flags = (uint8_t)((e.flags & 1) | (A.flags & 2));
                        ne.rule = A.rule; ne.state = P->nt[k].next; ne.start = A.start; ne.end = e.end;
                        ne.cost = c2; ne.c1 = aid; ne.c2 = id; ne.next = 0xFFFF;
                        r = add_edge(P, &ne);
                        if (r < 0) return r;
                    }
                }
            }
        }
        return 0;
    } else {
        /* active edge */
        const zrule *R = &n->rules[e.rule];
        int cnt, k, topflag;
        list_add(poslist(P, &P->act_by_end, e.end), id);
        ht_insert(P, id);
        cnt = gather_nt(P, R, e.state);
        for (k = 0; k < cnt; k++) map_add(&P->wm, ((uint32_t)e.end << 16) | P->nt[k].sym, id, P->nt[k].next, P->nt[k].cost);
        if (cnt) {
            zlist *cl = e.end < P->npos ? &P->comp_by_start[e.end] : NULL;
            topflag = (R->catflag && R->name == 1);
            for (k = 0; k < cnt; k++) {
                int matched = 0, j;
                uint16_t sym = P->nt[k].sym;
                struct zwlist *cw = map_list(&P->cm, ((uint32_t)e.end << 16) | sym, 0);
                int ncl = cw ? cw->n : 0;
                (void)cl;
                for (j = 0; j < ncl; j++) {
                    uint16_t cid = cw->v[j].aid;
                    zf1_st_comp++;
                    zedge Cc = *E_OF(P, cid);
                    if (sym == Cc.rule && !(((Cc.flags >> 1) ^ e.flags) & 1)) {
                        uint16_t c = (uint16_t)(e.cost + Cc.cost), c2;
                        zedge ne;
                        int r;
                        if (c < e.cost) return ERR_FULL;
                        c2 = (uint16_t)(c + P->nt[k].cost);
                        if (c2 < c) return ERR_FULL;
                        ne.flags = (uint8_t)((e.flags & 2) | (Cc.flags & 1));
                        ne.rule = e.rule; ne.state = P->nt[k].next; ne.start = e.start; ne.end = Cc.end;
                        ne.cost = c2; ne.c1 = id; ne.c2 = cid; ne.next = 0xFFFF;
                        matched = 1;
                        r = add_edge(P, &ne);
                        if (r < 0) return r;
                    }
                }
                if (!matched) {
                    /* predict rule sym at e.end */
                    const zrule *S = sym < n->nsym ? &n->rules[sym] : NULL;
                    zedge ne;
                    int r;
                    if (S) {
                        if (!mask_ok(P, S)) continue;
                        if (topflag && !cat_ok(P, S)) continue;
                        if (!first_ok(P, S, e.end)) continue;
                    }
                    ne.flags = (e.flags & 1) ? 3 : 0;
                    ne.rule = sym; ne.state = 0; ne.start = e.end; ne.end = e.end; ne.cost = 0;
                    ne.c1 = 0; ne.c2 = 0; ne.next = 0xFFFF;
                    r = add_edge(P, &ne);
                    if (r < 0) return r;
                }
            }
        }
        /* terminal arcs */
        {
            const uint8_t *st = n->B + 12 * (size_t)(R->bstate + e.state);
            uint32_t off = zf_rd32(st + 4);
            int nnt = zf_rd16(st + 8), total = zf_rd16(n->b3 + off), a;
            const uint8_t *lst = n->b3 + off + 4;
            struct { uint16_t arcid, next, len, cost; } m[64];
            int nm = 0, i;
            for (a = nnt; a < total; a++) {
                const uint8_t *arc = ARC(n, R->cbase + zf_rd16(lst + 4 * a));
                uint32_t kind = zf_rd32(arc + 4);
                const uint8_t *inL = DLIST(n, zf_rd32(arc + 8)), *outL = DLIST(n, zf_rd32(arc + 12));
                int nout = zf_rd16(outL), nin = zf_rd16(inL), o;
                uint16_t mc = 0xFFFF;
                int anyout = 0;
                (void)kind;
                for (o = 0; o < nout; o++) {
                    uint16_t c = zf_rd16(LABEL(n, zf_rd32(outL + 4 + 4 * o)) + 20);
                    if (c != 0xFFFF) { anyout = 1; if (mc == 0xFFFF || c < mc) mc = c; }
                }
                if (!anyout) continue;
                for (i = 0; i < nin; i++) {
                    const uint8_t *L = LABEL(n, zf_rd32(inL + 4 + 4 * i));
                    zf1_st_term++;
                    uint32_t lk = zf_rd32(L + 4);
                    int mlen = 0, ok = 0;
                    if (lk == 1) {
                        const zf_char *s = s1str(n, zf_rd32(L + 8));
                        int ll = zf_rd16(L + 12);
                        if (e.end + ll <= P->len) {
                            if (ll == 0) ok = 1;
                            else if (s && (!wcsncmp16(P->text + e.end, s, ll) ||
                                           (zf_rd32(L + 16) && !wcsnicmp_ascii(P->text + e.end, s, ll)))) ok = 1;
                        }
                        mlen = ll;
                    } else if (lk == 2) {
                        ok = lex_match(P, e.end, L, &mlen);
                    } else {
                        ok = 0;   /* regex labels (net+0xd8) are not present in the en-US networks */
                    }
                    if (ok && nm < 64) {
                        m[nm].arcid = zf_rd16(arc);
                        m[nm].next = zf_rd16(arc + 2);
                        m[nm].len = (uint16_t)mlen;
                        m[nm].cost = mc;
                        nm++;
                    }
                }
            }
            for (i = 0; i < nm; i++) {
                uint16_t c = (uint16_t)(e.cost + m[i].cost);
                zedge ne;
                int r;
                if (c < e.cost) return ERR_FULL;
                ne.flags = (uint8_t)(e.flags & 2);
                ne.rule = e.rule; ne.state = m[i].next; ne.start = e.start; ne.end = (uint16_t)(e.end + m[i].len);
                ne.cost = c; ne.c1 = id; ne.c2 = m[i].arcid; ne.next = 0xFFFF;
                r = add_edge(P, &ne);
                if (r < 0) return r;
            }
        }
        return 0;
    }
}

/* ------------------------------------------------------------------ result extraction */
static int is_empty(zparse *P, uint16_t id)   /* FUN_18000ecb4 */
{
    const zedge *e;
    if (id < P->base) return 0;
    if (id == 0xFFFF) return 1;
    e = E_OF(P, id);
    for (;;) {
        if (!e || e->c1 == 0) return 1;
        if (!is_empty(P, e->c1)) return 0;
        if (!is_empty(P, e->c2)) return 0;
        if (e->next == 0xFFFF) return 1;
        e = E_OF(P, e->next);
    }
}

/* result tree.  FUN_18000e440 / FUN_18000e708 build a binary tree per rule chain (ambiguous edges -> ALT nodes
 * whose children are the alternatives in chain order, children[1..] sorted by cost with the MS CRT qsort,
 * comparator FUN_180136900).  The MultiResult 1-best (A* over the word graph, FUN_180041cd0) takes the cheapest
 * path; equal-cost alternatives: the later one wins (queue insertion before equal scores); agreement variables
 * (rule +0x2c lists / attributes of kind 1, FUN_18005f84c / FUN_18005fb80) must unify (FUN_180141830), else
 * the next path is tried.  FUN_1800437fc flattens same-rule chains below rules with a permutation (+0x1c) or
 * a +0x2c list, FUN_180136474 permutes the children of rules with a permutation list. */
typedef struct tnode {
    uint16_t rule;
    int leaf;                           /* 1 = terminal leaf */
    int alt;                            /* 1 = ambiguity node (children = alternatives) */
    int altid;                          /* preorder number of an ALT node */
    int cost;                           /* ALT child: edge cost (sort key) */
    uint32_t g;                         /* attribute list (G index) of the node (+0x38), 0xFFFFFFFF = none */
    int newrule;                        /* rule differs from the parent's (attributes / category / permutation) */
    int nch, cap;
    struct tnode **ch;
    zf1_leaf lf;
    int nlp;                            /* kind-2 (lexicon) leaf: attribute pairs of the lexicon item */
    uint32_t lp[16];
} tnode;

typedef struct zwalk {
    zparse *P;
    zf1_netres *res;
    int depth;
    int nalt;
    int *choice;                        /* per ALT node: chosen child index */
    long nodes;                         /* ClassicVoices patch: tree nodes built so far (see ZF1_TREE_CAP) */
    int overflow;                       /* ClassicVoices patch: the cap was hit; the parse gives no result */
} zwalk;

/* ClassicVoices patch: the result tree unshares the parse forest, so ambiguous inputs grow it exponentially - a
 * run of 13 digits builds 212k nodes, 15 digits 896k (1.3 GB and seconds), "5555-5555-5555-5555-5555" 1.3M. Above
 * this many nodes the parse is abandoned as if no consistent path existed (the text is kept as it was, the engine's
 * own fallback). The largest tree over the upstream's 866 test lines is 30,688 nodes; a 12-digit number 102,943. */
#define ZF1_TREE_CAP 131072L

static tnode *tn_new(uint16_t rule)
{
    tnode *t = (tnode *)calloc(1, sizeof(tnode));
    t->rule = rule;
    t->g = 0xFFFFFFFFu;
    return t;
}

static void tn_add(tnode *p, tnode *c)
{
    if (!c) return;
    if (p->nch == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 4;
        p->ch = (tnode **)realloc(p->ch, sizeof(tnode *) * (size_t)p->cap);
    }
    p->ch[p->nch++] = c;
}

static void tn_free(tnode *t)
{
    int i;
    if (!t) return;
    for (i = 0; i < t->nch; i++) tn_free(t->ch[i]);
    free(t->ch);
    free(t);
}

static tnode *make_leaf(zwalk *W, uint16_t rule, uint16_t arcid, int start, int end)
{
    zparse *P = W->P;
    const zf1_net *n = P->net;
    const zrule *R = &n->rules[rule];
    const uint8_t *arc = ARC(n, R->cbase + arcid);
    const uint8_t *outL = DLIST(n, zf_rd32(arc + 12));
    int nout = zf_rd16(outL), o, k = 0;
    const uint8_t *lab = NULL;
    tnode *t = tn_new(rule);
    zf1_leaf *lf = &t->lf;
    if (++W->nodes > ZF1_TREE_CAP) W->overflow = 1;   /* ClassicVoices patch */
    t->leaf = 1;
    lf->rule = rule; lf->arc = arcid; lf->start = start; lf->end = end;
    for (o = 0; o < nout; o++) {
        const uint8_t *L = LABEL(n, zf_rd32(outL + 4 + 4 * o));
        if (zf_rd16(L + 20) != 0xFFFF) { if (!lab) lab = L; k++; }
    }
    if (k >= 1 && lab) {
        uint32_t type = zf_rd32(lab), kind = zf_rd32(lab + 4);
        lf->glue = type == 3 ? 1 : type == 4 ? 2 : type == 5 ? 3 : 0;
        lf->cost = zf_rd16(lab + 20);
        t->g = zf_rd32(lab + 24);
        if (type == 1) {
            int s = start, e = end;
            while (e > s && zf1_iswspace(P->text[e - 1])) e--;
            while (s < e && zf1_iswspace(P->text[s])) s++;
            lf->text = P->text + s;
            lf->len = e - s;   /* engine upper-cases a leading a-z here (type 1 labels are not used by en-US) */
        } else if (kind == 1) {
            lf->text = s1str(n, zf_rd32(lab + 8));
            lf->len = lf->text ? zf_rd16(lab + 12) : 0;
        } else if (kind == 2) {
            int ws = 0;
            while (start + ws < P->len && zf1_iswspace(P->text[start + ws])) ws++;
            lf->text = P->text + start + ws;
            lf->len = end - start - ws;
            if (lf->len < 0) lf->len = 0;
        }
    }
    if (zf_rd32(arc + 4) == 2 && n->lexfn && lf->len > 0) {
        /* FUN_18013f510: the leaf carries the attributes of the lexicon item (observed: the first item that
           satisfies the input label's constraint) */
        const uint8_t *inL = DLIST(n, zf_rd32(arc + 8));
        uint32_t gin = zf_rd16(inL) ? zf_rd32(LABEL(n, zf_rd32(inL + 4)) + 24) : 0xFFFFFFFFu;
        uint32_t pairs[LEX_MAXIT * LEX_MAXP];
        int counts[LEX_MAXIT], ni, i;
        ni = n->lexfn(n->lexctx, lf->text, lf->len, pairs, counts, LEX_MAXIT, LEX_MAXP);
        for (i = 0; i < ni; i++) {
            if (attr_ok(n, gin, pairs + i * LEX_MAXP, counts[i])) {
                t->nlp = counts[i] < 16 ? counts[i] : 16;
                memcpy(t->lp, pairs + i * LEX_MAXP, sizeof(uint32_t) * (size_t)t->nlp);
                break;
            }
        }
    }
    return t;
}

static tnode *build440(zwalk *W, uint16_t id, uint16_t parent_rule);

static void rule_node(zwalk *W, tnode *t, uint16_t rule, uint16_t parent_rule)
{
    const zf1_net *n = W->P->net;
    if (rule >= n->nsym || rule == parent_rule) return;
    t->newrule = 1;
    t->g = n->rules[rule].attrs;
}

static tnode *build708(zwalk *W, uint16_t id, uint16_t parent_rule)
{
    zparse *P = W->P;
    zedge e = *E_OF(P, id);
    int ne1 = (e.c1 != 0) && !is_empty(P, e.c1);
    int ne2 = !is_empty(P, e.c2);
    tnode *t = tn_new(e.rule);
    if (++W->nodes > ZF1_TREE_CAP) W->overflow = 1;   /* ClassicVoices patch */
    t->cost = e.cost;
    rule_node(W, t, e.rule, parent_rule);
    if (ne1 + ne2 == 1) {
        uint16_t ch = ne2 ? e.c2 : e.c1;
        if (ch >= P->base) tn_add(t, build440(W, ch, e.rule));
        else tn_add(t, make_leaf(W, e.rule, ch, e.start, e.end));
    } else {
        int mid = e.start;
        if (e.c1 >= P->base) {
            tn_add(t, build440(W, e.c1, e.rule));
            mid = E_OF(P, e.c1)->end;
        }
        if (e.c2 >= P->base) tn_add(t, build440(W, e.c2, e.rule));
        else tn_add(t, make_leaf(W, e.rule, e.c2, mid, e.end));
    }
    return t;
}

/* MS CRT qsort for small arrays (shortsort: move the maximum to the end), comparator = cost difference */
static void ms_shortsort(tnode **a, int n)
{
    int hi;
    for (hi = n - 1; hi > 0; hi--) {
        int mx = 0, p;
        tnode *x;
        for (p = 1; p <= hi; p++)
            if (a[p]->cost - a[mx]->cost > 0) mx = p;
        x = a[mx]; a[mx] = a[hi]; a[hi] = x;
    }
}

static tnode *build440(zwalk *W, uint16_t id, uint16_t parent_rule)
{
    zparse *P = W->P;
    int nalt = 0;
    uint16_t x = id;
    tnode *t;
    if (W->overflow) return NULL;   /* ClassicVoices patch: stop building once over ZF1_TREE_CAP */
    if (++W->depth > 0x200) { W->depth--; return NULL; }
    while (x != 0xFFFF && x >= P->base) {
        if (!is_empty(P, x)) nalt++;
        x = E_OF(P, x)->next;
    }
    if (nalt > 1) {
        tnode *a = tn_new(E_OF(P, id)->rule);
        if (++W->nodes > ZF1_TREE_CAP) W->overflow = 1;   /* ClassicVoices patch */
        a->alt = 1;
        a->altid = W->nalt++;
        x = id;
        while (x != 0xFFFF && x >= P->base && a->nch < nalt) {
            if (!is_empty(P, x)) tn_add(a, build708(W, x, parent_rule));
            x = E_OF(P, x)->next;
        }
        if (a->nch > 2) ms_shortsort(a->ch + 1, a->nch - 1);   /* children 1..n-1 only (qsort of +8) */
        t = a;
    } else {
        t = build708(W, id, parent_rule);
    }
    W->depth--;
    return t;
}

static int tree_cost(zwalk *W, const tnode *t)
{
    int i, c = 0;
    if (t->leaf) return t->lf.cost;
    if (t->alt) return tree_cost(W, t->ch[W->choice[t->altid]]);
    for (i = 0; i < t->nch; i++) c += tree_cost(W, t->ch[i]);
    return c;
}

/* greedy 1-best without constraints: cheapest alternative, ties -> the later one */
static int choose_greedy(zwalk *W, tnode *t)
{
    int i, c = 0;
    if (t->leaf) return t->lf.cost;
    if (t->alt) {
        int best = -1, bc = 0;
        for (i = 0; i < t->nch; i++) {
            int ci = choose_greedy(W, t->ch[i]);
            if (best < 0 || ci <= bc) { best = i; bc = ci; }
        }
        W->choice[t->altid] = best;
        return bc;
    }
    for (i = 0; i < t->nch; i++) c += choose_greedy(W, t->ch[i]);
    return c;
}

/* concrete copy of the chosen path (shares nothing) */
static tnode *materialize(zwalk *W, const tnode *t)
{
    tnode *c;
    int i;
    if (t->alt) {
        c = materialize(W, t->ch[W->choice[t->altid]]);
        return c;
    }
    c = tn_new(t->rule);
    c->leaf = t->leaf; c->g = t->g; c->newrule = t->newrule; c->cost = t->cost; c->lf = t->lf;
    c->nlp = t->nlp; memcpy(c->lp, t->lp, sizeof c->lp);
    for (i = 0; i < t->nch; i++) tn_add(c, materialize(W, t->ch[i]));
    return c;
}

static int has_x2c(zwalk *W, const tnode *t)
{
    const zf1_net *n = W->P->net;
    int i;
    if (!t->leaf && t->rule < n->nsym && t->newrule) {
        const zrule *R = &n->rules[t->rule];
        if (R->x2c && zf_rd16(n->b3 + R->x2c)) return 1;
    }
    for (i = 0; i < t->nch; i++) if (has_x2c(W, t->ch[i])) return 1;
    return 0;
}

/* FUN_1800437fc (sequence nodes only) */
static void flatten(zwalk *W, tnode *t, int inherited)
{
    const zf1_net *n = W->P->net;
    const zrule *R = t->rule < n->nsym ? &n->rules[t->rule] : NULL;
    int bperm = 0, bx2c = inherited, merge, i;
    if (t->leaf) return;
    if (R) {
        bperm = R->x1c && zf_rd32(n->b3 + R->x1c) != 0;
        if (!inherited) bx2c = R->x2c && zf_rd16(n->b3 + R->x2c) != 0;
    }
    merge = bperm || bx2c;
    for (i = 0; i < t->nch; i++) {
        tnode *c = t->ch[i];
        flatten(W, c, bx2c);
        if (!merge || c->rule != t->rule || c->g != 0xFFFFFFFFu || c->nlp) continue;
        if (c->leaf) {
            if ((!c->lf.text || !c->lf.len || !c->lf.text[0]) && c->lf.glue < 2) {
                memmove(t->ch + i, t->ch + i + 1, sizeof(tnode *) * (size_t)(t->nch - i - 1));
                t->nch--;
                tn_free(c);
                i--;
            }
        } else if (c->nch == 0) {
            memmove(t->ch + i, t->ch + i + 1, sizeof(tnode *) * (size_t)(t->nch - i - 1));
            t->nch--;
            tn_free(c);
            i--;
        } else {
            int k = c->nch, j;
            if (t->nch - 1 + k > t->cap) {
                t->cap = t->nch - 1 + k + 4;
                t->ch = (tnode **)realloc(t->ch, sizeof(tnode *) * (size_t)t->cap);
            }
            memmove(t->ch + i + k, t->ch + i + 1, sizeof(tnode *) * (size_t)(t->nch - i - 1));
            for (j = 0; j < k; j++) t->ch[i + j] = c->ch[j];
            t->nch += k - 1;
            c->nch = 0;
            tn_free(c);
            i += k - 1;
        }
    }
}

/* ---- agreement variables (FUN_18005f84c / FUN_18005fb80), on a concrete flattened tree ---- */
#define MAXVAR 32
typedef struct zvars {
    int n;
    uint16_t id[MAXVAR];                /* attribute id of variable i */
    int bound[MAXVAR];
    uint32_t set[MAXVAR][4];            /* value sets as bitmaps of values 0..127 */
    int fail;
} zvars;
typedef struct zctx { int n; int v[MAXVAR]; } zctx;

static void var_bind(zvars *V, int v, const uint32_t *s)
{
    int k;
    if (!V->bound[v]) {
        memcpy(V->set[v], s, sizeof V->set[v]);
        V->bound[v] = 1;
        return;
    }
    for (k = 0; k < 4; k++) V->set[v][k] &= s[k];
    if (!(V->set[v][0] | V->set[v][1] | V->set[v][2] | V->set[v][3])) V->fail = 1;
}

static void attrs_of(const zf1_net *n, uint32_t g, uint16_t *id, uint16_t *val, uint32_t *kind, int *na)
{
    const uint8_t *gl;
    int c, i;
    *na = 0;
    if (g == 0xFFFFFFFFu || g >= n->nG) return;
    gl = n->b3 + zf_rd32(n->G + 4 * (size_t)g);
    c = zf_rd16(gl);
    for (i = 0; i < c && *na < 16; i++) {
        const uint8_t *h = n->H + 12 * (size_t)zf_rd32(gl + 4 + 4 * i);
        id[*na] = zf_rd16(h); val[*na] = zf_rd16(h + 2); kind[*na] = zf_rd32(h + 4);
        (*na)++;
    }
}

static void vars_walk(zwalk *W, const tnode *t, zctx *ctx, zvars *V)
{
    const zf1_net *n = W->P->net;
    uint16_t id[16], val[16];
    uint32_t kind[16];
    int na = 0, i, j;
    attrs_of(n, t->g, id, val, kind, &na);
    for (i = 0; i < t->nlp && na < 16; i++) {
        id[na] = (uint16_t)(t->lp[i] & 0xFFFF); val[na] = (uint16_t)(t->lp[i] >> 16); kind[na] = 0; na++;
    }
    /* bind context variables at the first node with a kind-0 attribute of the same id */
    for (i = 0; i < ctx->n; i++) {
        int v = ctx->v[i], hit = 0;
        uint32_t s[4] = {0, 0, 0, 0};
        for (j = 0; j < na; j++)
            if (kind[j] == 0 && id[j] == V->id[v]) { hit = 1; if (val[j] < 128) s[val[j] >> 5] |= 1u << (val[j] & 31); }
        if (hit) {
            var_bind(V, v, s);
            memmove(ctx->v + i, ctx->v + i + 1, sizeof(int) * (size_t)(ctx->n - i - 1));
            ctx->n--;
            i--;
        }
    }
    if (t->leaf) return;
    if (t->nch < 2 || !t->newrule || t->rule >= n->nsym) {
        for (i = 0; i < t->nch; i++) {
            zctx c2 = *ctx;
            vars_walk(W, t->ch[i], &c2, V);
        }
        return;
    }
    {
        const zrule *R = &n->rules[t->rule];
        int base = V->n, nx = R->x2c ? zf_rd16(n->b3 + R->x2c) : 0, k;
        for (k = 0; k < nx && V->n < MAXVAR; k++) {
            uint32_t fi = zf_rd32(n->b3 + R->x2c + 4 + 4 * k);
            V->id[V->n] = (uint16_t)zf_rd32(n->F + 8 * (size_t)fi);
            V->bound[V->n] = 0;
            V->n++;
        }
        for (i = 0; i < t->nch; i++) {
            zctx c2;
            c2.n = 0;
            for (k = 0; k < nx && base + k < V->n; k++) {
                uint32_t fi = zf_rd32(n->b3 + R->x2c + 4 + 4 * k);
                const uint8_t *fl = n->b3 + zf_rd32(n->F + 8 * (size_t)fi + 4);
                int fc = zf_rd16(fl), q;
                for (q = 0; q < fc; q++)
                    if ((int)zf_rd32(fl + 4 + 4 * q) == i) { c2.v[c2.n++] = base + k; break; }
            }
            for (k = 0; k < ctx->n; k++)
                for (j = 0; j < na; j++)
                    if (kind[j] == 1 && val[j] == i && id[j] == V->id[ctx->v[k]] && c2.n < MAXVAR) {
                        c2.v[c2.n++] = ctx->v[k];
                        break;
                    }
            vars_walk(W, t->ch[i], &c2, V);
        }
    }
}

static int consistent(zwalk *W, const tnode *t)
{
    zvars V;
    zctx c;
    memset(&V, 0, sizeof V);
    c.n = 0;
    vars_walk(W, t, &c, &V);
    return !V.fail;
}

/* FUN_180136670 / FUN_180136474 (lattice mode 0: new[i] = old[perm[i]]) */
static void permute(zwalk *W, tnode *t, int newrule)
{
    const zf1_net *n = W->P->net;
    int i;
    if (t->leaf) return;
    for (i = 0; i < t->nch; i++) permute(W, t->ch[i], t->ch[i]->rule != t->rule);
    if (newrule && t->rule < n->nsym) {
        const zrule *R = &n->rules[t->rule];
        if (R->x1c) {
            uint32_t cnt = zf_rd32(n->b3 + R->x1c);
            if (cnt && cnt == (uint32_t)t->nch) {
                tnode **old = (tnode **)malloc(sizeof(tnode *) * cnt);
                uint32_t k;
                memcpy(old, t->ch, sizeof(tnode *) * cnt);
                for (k = 0; k < cnt; k++) {
                    uint32_t p = zf_rd32(n->b3 + R->x1c + 4 + 4 * k);
                    if (p < cnt) t->ch[k] = old[p];
                }
                free(old);
            }
        }
    }
}

/* category + attributes of the outermost category rule of the chosen path (FUN_1800433c8) */
static int find_category(zwalk *W, const tnode *t)
{
    const zf1_net *n = W->P->net;
    int i;
    if (!t->leaf && t->newrule && t->rule < n->nsym) {
        const zrule *R = &n->rules[t->rule];
        if (R->catflag && R->name != 1) {
            W->res->category = s1str(n, R->cat);
            W->res->catrule = t->rule;
            W->res->nattrs = 0;
            if (R->attrs != 0xFFFFFFFFu && R->attrs < n->nG) {
                const uint8_t *gl = n->b3 + zf_rd32(n->G + 4 * (size_t)R->attrs);
                int c = zf_rd16(gl), k;
                for (k = 0; k < c && W->res->nattrs < 16; k++)
                    W->res->attrs[W->res->nattrs++] = zf_rd32(n->H + 12 * (size_t)zf_rd32(gl + 4 + 4 * k));
            }
            return 1;
        }
    }
    for (i = 0; i < t->nch; i++) if (find_category(W, t->ch[i])) return 1;
    return 0;
}

static void tn_dump(const tnode *t, int d)
{
    int i;
    fprintf(stderr, "%*s%s rule=%u n=%d g=%d", d * 2, "", t->leaf ? "L" : t->alt ? "A" : "N", t->rule, t->nch, (int)t->g);
    if (t->leaf) fprintf(stderr, " %d-%d len=%d glue=%d cost=%d", t->lf.start, t->lf.end, t->lf.len, t->lf.glue, t->lf.cost);
    fputc(10, stderr);
    for (i = 0; i < t->nch; i++) tn_dump(t->ch[i], d + 1);
}

static void collect(zwalk *W, const tnode *t)
{
    int i;
    if (t->leaf) {
        zf1_netres *r = W->res;
        if (r->nleaves == r->capleaves) {
            r->capleaves = r->capleaves ? r->capleaves * 2 : 32;
            r->leaves = (zf1_leaf *)realloc(r->leaves, sizeof(zf1_leaf) * (size_t)r->capleaves);
        }
        r->leaves[r->nleaves++] = t->lf;
        return;
    }
    for (i = 0; i < t->nch; i++) collect(W, t->ch[i]);
}

/* choice vectors in preference order when agreement variables are present */
static tnode *choose_constrained(zwalk *W, tnode *root)
{
    int *best = NULL, bestcost = 0, i, total = 1;
    int *cnt = (int *)calloc((size_t)W->nalt + 1, sizeof(int));
    tnode *res = NULL;
    /* count options per ALT node */
    {
        tnode *stack[1024];
        int sp = 0;
        stack[sp++] = root;
        while (sp) {
            tnode *t = stack[--sp];
            if (t->alt) cnt[t->altid] = t->nch;
            for (i = 0; i < t->nch && sp < 1024; i++) stack[sp++] = t->ch[i];
        }
    }
    for (i = 0; i < W->nalt; i++) { total *= cnt[i] ? cnt[i] : 1; if (total > 4096) break; }
    if (total > 4096) { free(cnt); return NULL; }
    memset(W->choice, 0, sizeof(int) * (size_t)W->nalt);
    best = (int *)malloc(sizeof(int) * ((size_t)W->nalt + 1));
    for (;;) {
        int c = tree_cost(W, root), better = 0;
        if (!res || c < bestcost) better = 1;
        else if (c == bestcost) {
            /* tie: prefer later alternatives (compare choice vectors in ALT preorder) */
            for (i = 0; i < W->nalt; i++) {
                if (W->choice[i] != best[i]) { better = W->choice[i] > best[i]; break; }
            }
        }
        if (better) {
            tnode *m = materialize(W, root);
            flatten(W, m, 0);
            if (consistent(W, m)) {
                if (res) tn_free(res);
                res = m;
                bestcost = c;
                memcpy(best, W->choice, sizeof(int) * (size_t)W->nalt);
            } else {
                tn_free(m);
            }
        }
        for (i = W->nalt - 1; i >= 0; i--) {
            if (++W->choice[i] < (cnt[i] ? cnt[i] : 1)) break;
            W->choice[i] = 0;
        }
        if (i < 0) break;
    }
    free(cnt);
    free(best);
    return res;
}

/* build the chosen, flattened, permuted result tree and collect leaves / category */
static void make_result(zwalk *W, uint16_t best)
{
    tnode *root = build440(W, best, 0xFFFF), *m = NULL;
    if (W->overflow) {   /* ClassicVoices patch: too ambiguous to unfold (ZF1_TREE_CAP): no consistent path */
        tn_free(root);
        W->res->nores = 1;
        return;
    }
    if (!root) return;
    W->choice = (int *)calloc((size_t)W->nalt + 1, sizeof(int));
    if (W->nalt && has_x2c(W, root)) {
        m = choose_constrained(W, root);
        if (!m) W->res->nores = 1;
    } else {
        choose_greedy(W, root);
        m = materialize(W, root);
        flatten(W, m, 0);
    }
    if (m) {
        if (zf1_net_trace & 2) tn_dump(m, 0);
        find_category(W, m);
        permute(W, m, 1);
        collect(W, m);
        tn_free(m);
    }
    tn_free(root);
    free(W->choice);
}

/* ------------------------------------------------------------------ driver */
static uint32_t find_cat(const zf1_net *n, const zf_char *cat, int *found)
{
    uint32_t i = 0;
    *found = 0;
    if (!cat) { *found = 1; return 0; }
    while (i < n->n1) {
        const zf_char *s = n->s1 + i;
        size_t L = zf_strlen(s);
        if (!zf_strcmp(s, cat)) { *found = 1; return i; }
        i += (uint32_t)L + 1;
    }
    return 0xFFFFFFFFu;
}

static int special_token(const zf_char *t, int len, int pos, int *toklen)   /* FUN_180068130 */
{
    int k = 0;
    if (pos != len)
        while (k < len - pos && !zf1_iswspace(t[pos + k])) k++;
    *toklen = k;
    if (pos < len && t[pos] == '/' && k > 0 && t[pos + k - 1] == ';') {
        int i;
        for (i = pos + 2; i < len && t[i]; i++)
            if (t[i] == '/') return 1;
    }
    return 0;
}

int zf1_net_parse(zf1_net *net, const zf_char *text, int len, int start, int mode, const zf_char *category,
                  int tokmode, zf1_netres *res)
{
    zparse P;
    int found, rc = 0, tl;
    memset(res, 0, sizeof *res);
    res->catrule = -1;
    if (!net || !text || start < 0 || start >= len || len > 0xFFFE) return -1;
    if (special_token(text, len, start, &tl)) {
        res->found = 1; res->special = 1; res->start = start; res->len = tl;
        res->capleaves = 1;
        res->leaves = (zf1_leaf *)calloc(1, sizeof(zf1_leaf));
        res->leaves[0].text = text + start; res->leaves[0].len = tl;
        res->leaves[0].start = start; res->leaves[0].end = start + tl;
        res->nleaves = 1;
        return 0;
    }
    memset(&P, 0, sizeof P);
    P.net = net; P.text = text; P.len = len; P.mode = mode; P.tokmode = tokmode; P.start = (uint16_t)start;
    P.catoff = find_cat(net, category, &found);
    if (!found) P.catoff = 0xFFFFFFFFu;
    P.base = net->d0;
    P.maxn = (uint16_t)~net->d0;
    if (net->ws) {
        /* reuse the buffers of the previous parse (avoids heap churn) */
        zparse *W = (zparse *)net->ws;
        int i;
        P.e = W->e; P.ecap = W->ecap;
        P.ht = W->ht; P.htcap = W->htcap;
        P.bhead = W->bhead; P.btail = W->btail; P.bmap = W->bmap;
        P.wm = W->wm; P.cm = W->cm;
        map_reset(&P.wm); map_reset(&P.cm);
        P.htused = W->htused; P.htusedcap = W->htusedcap;
        for (i = 0; i < (int)W->htn; i++) P.ht[P.htused[i]] = 0;
        P.comp_by_start = W->comp_by_start; P.act_by_end = W->act_by_end; P.npos = W->npos;
        for (i = 0; i < P.npos; i++) { P.comp_by_start[i].n = 0; P.act_by_end[i].n = 0; }
        P.ag = W->ag; P.agcap = W->agcap;
        P.nt = W->nt; P.ntcap = W->ntcap;
    } else {
        net->ws = calloc(1, sizeof(zparse));
        P.htcap = 1024;
        P.ht = (uint32_t *)calloc(P.htcap, sizeof(uint32_t));
        P.bhead = (int *)malloc(sizeof(int) * 65536);
        P.btail = (int *)malloc(sizeof(int) * 65536);
        P.bmap = (uint64_t *)malloc(sizeof(uint64_t) * 1024);
    }
    P.aghead = -1; P.agfree = -1;
    memset(P.bmap, 0, sizeof(uint64_t) * 1024);
    P.bmin = 0;
    P.best = 0xFFFF;
    poslist(&P, &P.comp_by_start, len + 1);
    /* top edge */
    if (net->nsym && first_ok(&P, &net->rules[0], start)) {
        zedge t;
        int id, k;
        memset(&t, 0, sizeof t);
        t.start = t.end = (uint16_t)start; t.next = 0xFFFF;
        id = new_edge(&P, &t);
        k = ag_alloc(&P, (uint16_t)id);
        ag_push_tail(&P, k, 0);
        ht_set(&P, E_OF(&P, id), AGF | (uint32_t)k);
        for (;;) {
            int k2 = ag_pop(&P);
            uint16_t eid;
            if (k2 < 0) break;
            eid = P.ag[k2].id;
            P.ag[k2].next = P.agfree;
            P.agfree = k2;
            rc = process(&P, eid);
            if (rc < 0) break;
        }
    }
    res->nedges = P.ne;
    zf1_net_stat_edges += P.ne;
    zf1_net_stat_parses++;
    if (rc == ERR_FATAL) {
        res->err = -1;
    } else if (P.best != 0xFFFF) {
        zwalk W;
        const zedge *B = E_OF(&P, P.best);
        res->found = 1;
        res->start = B->start;
        res->len = B->end - B->start;
        res->cost = B->cost;
        memset(&W, 0, sizeof W);
        W.P = &P; W.res = res;
        make_result(&W, P.best);
    }
    {
        zparse *W = (zparse *)net->ws;
        W->e = P.e; W->ecap = P.ecap; W->ht = P.ht; W->htcap = P.htcap;
        W->bhead = P.bhead; W->btail = P.btail; W->bmap = P.bmap;
        W->wm = P.wm; W->cm = P.cm;
        W->htused = P.htused; W->htusedcap = P.htusedcap; W->htn = P.htn;
        W->comp_by_start = P.comp_by_start; W->act_by_end = P.act_by_end; W->npos = P.npos;
        W->ag = P.ag; W->agcap = P.agcap; W->nt = P.nt; W->ntcap = P.ntcap;
    }
    return 0;
}

static void ws_free(void *ws)
{
    zparse *W = (zparse *)ws;
    int i;
    if (!W) return;
    for (i = 0; i < W->npos; i++) { free(W->comp_by_start[i].v); free(W->act_by_end[i].v); }
    free(W->comp_by_start); free(W->act_by_end); free(W->e); free(W->ht); free(W->htused); free(W->bhead); free(W->btail); free(W->bmap);
    map_free(&W->wm); map_free(&W->cm); free(W->ag); free(W->nt);
    free(W);
}

/* FUN_18013e6c4: category test of one rule by name */
static int rule_cat_is(const zf1_net *n, const zrule *S, int mode, const zf_char *name)
{
    const zf_char *c;
    if (!S->catflag) return 1;
    if (!((uint32_t)mode == S->cattype || mode == 0 || S->cattype == 0)) return 0;
    if (!name) return 1;
    c = s1str(n, S->cat);
    return c && !zf_strcmp(c, name);
}

/* FUN_180136700 (network vt+0x38): is category `name` reachable from the top rule in `mode`? */
int zf1_net_category_supported(zf1_net *n, const zf_char *name, int mode)
{
    const zrule *R0;
    if (!n || !n->nsym) return 0;
    R0 = &n->rules[0];
    if (!R0->catflag || R0->name == 1) {
        const uint8_t *st = n->B + 12 * (size_t)R0->bstate;
        uint32_t so = zf_rd32(st + 4);
        int cnt = zf_rd16(st + 8), i;
        const uint8_t *lst = n->b3 + so + 4;
        for (i = 0; i < cnt; i++) {
            const uint8_t *a = ARC(n, R0->cbase + zf_rd16(lst + 4 * i));
            uint16_t sym;
            if (zf_rd16(a + 12) == 0xFFFF) continue;
            sym = zf_rd16(a + 8);
            if (sym >= n->nsym) return 0;
            if (rule_cat_is(n, &n->rules[sym], mode, name)) return 1;
        }
        return 0;
    }
    return rule_cat_is(n, R0, mode, name);
}

void zf1_netres_free(zf1_netres *res)
{
    free(res->leaves);
    memset(res, 0, sizeof *res);
}

zf_char *zf1_netres_text(const zf1_netres *res)
{
    size_t cap = 16, k = 0;
    int i, pendglue = 1;
    zf_char *out;
    for (i = 0; i < res->nleaves; i++) cap += (size_t)res->leaves[i].len + 1;
    out = (zf_char *)malloc(cap * sizeof(zf_char));
    for (i = 0; i < res->nleaves; i++) {
        const zf1_leaf *l = &res->leaves[i];
        if (l->len <= 0) {
            if (l->glue & 2) pendglue = 1;
            continue;
        }
        if (k && !pendglue && !(l->glue & 1)) out[k++] = ' ';
        memcpy(out + k, l->text, sizeof(zf_char) * (size_t)l->len);
        k += (size_t)l->len;
        pendglue = (l->glue & 2) != 0;
    }
    out[k] = 0;
    return out;
}
