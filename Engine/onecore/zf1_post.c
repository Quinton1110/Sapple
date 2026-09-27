/* zf1_post.c - Analyze step 9 (word pattern machine) and the post-Analyze sentence steps.  See zf1_post.h and
 * notes/fe1_post.md.  Portable C99, clean-room from MSTTSEngine_OneCore.dll 10.3.21207.0. */
#include "zf1_post.h"
#include <stdlib.h>
#include <string.h>

/* ClassicVoices patch: function-level scratch is per thread (two voices may speak on two threads at once). */
#ifndef ZF_TLS
#define ZF_TLS __thread
#endif

/* ------------------------------------------------------------------------------------------------------------
 * POS table object (sentence+0x2d8 = CTTSEngine vt+0x50): vt+8 name, vt+0x10 class, vt+0x38 default (50).
 * Values observed at runtime (zftap1_post POSTAB); ids >= 50 -> "unknown", class 0. */
static const char *const POSNAME[50] = {
    "unknown", "NOM", "NOM_CLASS", "noun", "VER", "VER_TYPE", "verb", "verbing", "verbpastp", "ADJ",
    "ADJ_TYPE", "adj", "DET", "DET_TYPE", "det", "whdet", "NUM", "NUM_TYPE", "num", "AUX",
    "AUX_TYPE", "vaux", "PRO", "PRO_TYPE", "pron", "whpron", "ADV", "ADV_TYPE", "adv", "whadv",
    "CON", "CON_TYPE", "conj", "ADP", "ADP_TYPE", "prep", "INT", "INT_TYPE", "interjection", "PAR",
    "PAR_TYPE", "particle", "SYM", "SYM_TYPE", "symbol", "OTH", "OTH_TYPE", "listitem", "possend", "contr"};
static const unsigned char POSCLASS[50] = {0, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 6,
                                           6, 6, 7, 7, 7, 7, 9, 9, 9, 9, 10, 10, 10, 11, 11, 11, 12, 12, 12, 13,
                                           13, 13, 16, 16, 16, 17, 17, 17, 17, 17};
const char *zf1_pos_name(unsigned pos) { return pos < 50 ? POSNAME[pos] : "unknown"; }
int zf1_pos_class(unsigned pos) { return pos < 50 ? POSCLASS[pos] : 0; }

/* ------------------------------------------------------------------------------------------------------------ */
int zf1_post_init(zf1_post *p, const zf1_dat *d)
{
    size_t n;
    const uint8_t *r;
    int k;
    memset(p, 0, sizeof *p);
    /* sPMM pattern machine (CShareablePatternMachineData) */
    p->pm = zf1_dat_get(d, 0x00a2359eu, 0x4d4d5073u, &p->pmsize);
    /* suffix tables FUN_1800b80dc: u32 LCID, u32 recsize(=20), then 4 x {u32 n, rec[n]} */
    r = zf1_dat_get(d, 0x5554ba64u, 0x09397c04u, &n);
    if (r && n >= 8 && zf_rd32(r + 4) == 20) {
        const uint8_t *q = r + 8, *e = r + n;
        for (k = 0; k < 4 && q + 4 <= e; k++) {
            p->nsuf[k] = (int)zf_rd32(q);
            p->sufrec[k] = q + 4;
            q += 4 + 20 * (size_t)p->nsuf[k];
        }
        p->suf = r;
    }
    /* quote table FUN_1800b83f4: u32 LCID, u32 n, {u16 open, u16 close, u32 typographic}[n] */
    r = zf1_dat_get(d, 0xb54490e3u, 0xe4c81da4u, &n);
    if (r && n >= 8) {
        int ok = 1;
        p->nquote = (int)zf_rd32(r + 4);
        p->quote = r + 8;
        for (k = 0; k < p->nquote; k++) {
            const uint8_t *q = p->quote + 8 * k;
            if (zf_rd32(q + 4) == 0) {
                if (zf_rd16(q) != zf_rd16(q + 2)) ok = 0;
            }
        }
        p->quote_ok = ok && p->nquote > 0;
    }
    /* parallel-structure config FUN_1800b8f08: u32 LCID, u32 recsize(=0x2c), {u32 n, rec[n]} x 2 */
    r = zf1_dat_get(d, 0xd8951565u, 0x52a33b0bu, &n);
    if (r && n >= 12 && zf_rd32(r + 4) == 0x2c) {
        const uint8_t *q = r + 8;
        p->nsep = (int)zf_rd32(q);
        p->sep = q + 4;
        q += 4 + 0x2c * (size_t)p->nsep;
        p->nconj = (int)zf_rd32(q);
        p->conj = q + 4;
        p->par_ok = p->nsep > 0 && p->nconj > 0;
    }
    return 0;
}

void zf1_post_free(zf1_post *p) { memset(p, 0, sizeof *p); }

void zf1_postout_free(zf1_postout *o)
{
    free(o->segs);
    free(o->word_seg);
    memset(o, 0, sizeof *o);
}

/* ------------------------------------------------------------------------------------------------------------
 * small helpers: "C" locale towlower, string lengths as the engine computes them */
static zf_char lwr(zf_char c) { return (c >= 'A' && c <= 'Z') ? (zf_char)(c + 32) : c; }

/* wcsnlen capped at 0x800 like FUN_18004e81c / FUN_18001f36c; NULL -> -1, unterminated -> -1 */
static int slen800(const zf_char *s)
{
    int n = 0;
    if (!s) return -1;
    while (n < 0x800 && s[n]) n++;
    return n < 0x800 ? n : -1;
}

/* engine CTTSString getters return NULL for empty strings */
static const zf_char *nz(const zf_char *s) { return (s && *s) ? s : NULL; }

static int pronlen(const zf_word *w) { return (w->pron && w->pron[0]) ? (int)zf_strlen(w->pron) : 0; }
/* the word owns syllables (FUN_18001a644 builds them for words with a pronunciation) */
static int haskids(const zf_word *w) { return w->type != ZF_WT_PUNCT && pronlen(w) > 0; }
/* FUN_18001a070 == 0 : a "real" word */
static int realw(const zf_word *w) { return w->type != ZF_WT_SILENCE && w->type != ZF_WT_PUNCT && haskids(w); }
/* FUN_18001a144 */
static int a144(const zf_word *w) { return (w->type == 0 || w->type == 4) && pronlen(w) > 0; }
/* FUN_18001a56c */
static int a56c(const zf_word *w)
{
    return (w->type == 0 || w->type == 4 || w->type == ZF_WT_SILENCE) && haskids(w) && pronlen(w) > 0;
}

typedef struct wv { zf_word *w; int n; } wv;

static int prv(const wv *v, int i, int flag)
{
    for (i--; i >= 0; i--)
        if (!flag || realw(&v->w[i])) return i;
    return -1;
}
static int nxt(const wv *v, int i, int flag)
{
    for (i++; i < v->n; i++)
        if (!flag || realw(&v->w[i])) return i;
    return -1;
}

/* ------------------------------------------------------------------------------------------------------------
 * pattern machine (FUN_18001f36c lookup, FUN_18001f970 / FUN_18001fa3c / FUN_18001ffa8 run, ops 0x18016d530) */
#define PM32(o) ((int)zf_rd32(pm + (o)))

typedef struct fval {
    int type;                           /* 0 int, 1 string, 3 = not evaluated yet */
    int ival;
    const zf_char *s;
} fval;

typedef struct pmctx {
    const uint8_t *pm;
    wv v;
    int cur;                            /* current word ("param_1+8"), -1 = none */
    zf_char mode;                       /* 'S' or 'W' (last matcher, 0x180184218 / 0x180185250) */
    fval f[32];
    const int *wseg;                    /* word+0x1a8 segment of each word (S mode), NULL in W mode */
    const zf_segment *segs;
    zf_char *b11, *b18;                 /* +0x60 / +0x80 string buffers */
    int kmp[2048];
} pmctx;

static int pm_lookup(const uint8_t *pm, const zf_char *t)
{
    int lo = 0, hi = PM32(8) - 1, ents = PM32(0x10);
    int tl = slen800(t);
    while (lo <= hi) {
        int mid = (lo + hi) / 2, d, i;
        const uint8_t *key = pm + PM32(ents + 16 * mid + 4);
        int kl = zf_rd16(key);
        d = tl - kl;
        if (t) {
            int m = kl <= tl ? kl : tl;
            for (i = 0; i < m; i++) {
                zf_char a = t[i], b = zf_rd16(key + 2 + 2 * i);
                if (a != b) { d = (int)a - (int)b; break; }
            }
        }
        if (d < 0) hi = mid - 1;
        else if (d > 0) lo = mid + 1;
        else return mid;
    }
    return -1;
}

/* FUN_18005391c */
static int cmpn(const zf_char *a, const zf_char *b, int la, int lb)
{
    int r = la - lb, i, n;
    if (a && b) {
        n = lb <= la ? lb : la;
        for (i = 0; i < n; i++)
            if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return r;
}
/* FUN_18015ef84 (towlower) */
static int icmpn(const zf_char *a, const zf_char *b, int la, int lb)
{
    int r = la - lb, i, n;
    if (a && b) {
        n = lb <= la ? lb : la;
        for (i = 0; i < n; i++)
            if (lwr(a[i]) != lwr(b[i])) return (int)lwr(a[i]) - (int)lwr(b[i]);
    }
    return r;
}
/* FUN_18015f15c (ci = 0) / FUN_180053a04 (ci = 1): KMP; failure table always case-sensitive */
static int kmp(const zf_char *t, const zf_char *pt, int n, int m, int *tbl, int ci)
{
    int j = -1, k = 0, i = 0, cnt = 0;
    if (m <= 0 || m > 2047 || !pt) return -1;
    tbl[0] = -1;
    while (k < m - 1) {
        if (j == -1 || pt[k] == pt[j]) {
            j++;
            k++;
            tbl[k] = (pt[k] == pt[j]) ? tbl[j] : j;
        } else {
            j = tbl[j];
        }
    }
    j = 0;
    if (n > 0 && t) {
        do {
            if (m <= j) return cnt - m;
            if (j == -1 || (ci ? lwr(t[i]) == lwr(pt[j]) : t[i] == pt[j])) {
                cnt++;
                i++;
                j++;
            } else {
                j = tbl[j];
            }
        } while (i < n);
        if (m <= j) return cnt - m;
    }
    return -1;
}

static const zf_char EMPTY[1] = {0};
static const zf_char *wtext(const zf_word *w) { return nz(w->text); }
static const zf_char *text_or_empty(const pmctx *c, int i) { const zf_char *t = i >= 0 ? wtext(&c->v.w[i]) : NULL; return t ? t : EMPTY; }

static zf_char *u16name(const char *s)
{
    static ZF_TLS zf_char buf[50][24]; /* ClassicVoices patch: per thread (the lazy fill raced between engines) */
    static ZF_TLS int init;
    int k, i;
    if (!init) {
        for (k = 0; k < 50; k++) {
            for (i = 0; POSNAME[k][i] && i < 23; i++) buf[k][i] = (zf_char)POSNAME[k][i];
            buf[k][i] = 0;
        }
        init = 1;
    }
    for (k = 0; k < 50; k++)
        if (POSNAME[k] == s) return buf[k];
    return buf[0];
}
static const zf_char *posname_w(const pmctx *c, int i)
{
    return u16name(zf1_pos_name(i >= 0 ? c->v.w[i].pos : ZF1_POS_DEFAULT));
}

/* FUN_1800b790c: earliest type-0 word walking back from the word before cur (flag-1 steps) */
static int first_word(const pmctx *c, int cur)
{
    int p, r = -1;
    if (cur < 0) return -1;
    p = prv(&c->v, cur, 0);
    while (p >= 0) {
        if (c->v.w[p].type == 0) r = p;
        p = prv(&c->v, p, 1);
    }
    return r;
}
/* FUN_1800b7a80 / FUN_1800b7afc: next type-0 word after x (flag-1 steps) that is not cur */
static int second_after(const pmctx *c, int x, int cur)
{
    if (x < 0) return -1;
    for (;;) {
        int q = nxt(&c->v, x, 1);
        if (q < 0) return -1;
        x = q;
        if (c->v.w[x].type == 0 && x != cur) return x;
    }
}

/* FUN_1800b7990: first type-0 word of the last clause before cur (clause = after a punctuation word with
 * i180 in 1..3); jumps over segments (word+0x1a8) using the segment table */
static int clause_start(const pmctx *c, int cur, const int *wseg, const zf_segment *segs)
{
    int r, p;
    if (cur < 0) return -1;
    r = c->v.w[cur].type == 0 ? cur : -1;
    p = prv(&c->v, cur, 0);
    while (p >= 0) {
        int q = p, t;
        if (wseg && wseg[p] >= 0 && segs[wseg[p]].first != p) q = segs[wseg[p]].first;
        if (c->v.w[q].type == 1 && c->v.w[q].i180 >= 1 && c->v.w[q].i180 <= 3) return r;
        t = c->v.w[q].type;
        p = prv(&c->v, q, 0);
        if (t == 0) r = q;
    }
    return r;
}

/* FUN_1800b7fd8 */
static int gapflag(const pmctx *c, int a, int b)
{
    const zf_word *x, *y;
    if (a < 0 || b < 0) return 0;
    x = &c->v.w[a];
    y = &c->v.w[b];
    if (nz(x->ne_type) && nz(y->ne_type) && x->src_off == y->src_off && zf_strcmp_a(x->ne_type, "sp:default") != 0)
        return 1;
    return (unsigned)y->src_off > (unsigned)(x->src_off + x->src_len) ? 1 : 0;
}

static void sbuf_add(zf_char **b, const zf_char *s)
{
    size_t la = *b ? zf_strlen(*b) : 0, lb = s ? zf_strlen(s) : 0;
    zf_char *n = (zf_char *)realloc(*b, (la + lb + 1) * sizeof(zf_char));
    if (!n) return;
    if (lb) memcpy(n + la, s, lb * sizeof(zf_char));
    n[la + lb] = 0;
    *b = n;
}


static void feat(pmctx *c, int fid, fval *o)
{
    int cur = c->cur, x;
    o->type = 1;
    o->ival = 0;
    o->s = NULL;
    switch (fid) {
    case 0: o->s = text_or_empty(c, cur); break;
    case 1: o->s = posname_w(c, cur); break;
    case 2: o->s = text_or_empty(c, first_word(c, cur)); break;
    case 3: o->s = posname_w(c, first_word(c, cur)); break;
    case 4: o->s = text_or_empty(c, second_after(c, first_word(c, cur), cur)); break;
    case 5: o->s = posname_w(c, second_after(c, first_word(c, cur), cur)); break;
    case 6: o->s = text_or_empty(c, clause_start(c, cur, c->wseg, c->segs)); break;
    case 7: o->s = posname_w(c, clause_start(c, cur, c->wseg, c->segs)); break;
    case 8: o->s = text_or_empty(c, second_after(c, clause_start(c, cur, c->wseg, c->segs), cur)); break;
    case 9: o->s = posname_w(c, second_after(c, clause_start(c, cur, c->wseg, c->segs), cur)); break;
    case 10:
        o->type = 0;
        for (x = clause_start(c, cur, c->wseg, c->segs); x >= 0; x = nxt(&c->v, x, 0))
            if (c->v.w[x].type == 0 || c->v.w[x].type == 4) o->ival++;
        break;
    case 11:
        free(c->b11);
        c->b11 = NULL;
        { static const zf_char sp0[2] = {' ', 0}; sbuf_add(&c->b11, sp0); }   /* CTTSString vt+8(" ") */
        for (x = clause_start(c, cur, c->wseg, c->segs); x >= 0; x = nxt(&c->v, x, 0))
            if (c->v.w[x].type == 0) {
                sbuf_add(&c->b11, wtext(&c->v.w[x]));
                { static const zf_char sp[2] = {' ', 0}; sbuf_add(&c->b11, sp); }
            }
        o->s = nz(c->b11);
        break;
    case 12: o->s = text_or_empty(c, cur >= 0 ? prv(&c->v, cur, 0) : -1); break;
    case 13: o->s = text_or_empty(c, cur >= 0 ? nxt(&c->v, cur, 0) : -1); break;
    case 14: o->type = 0; o->ival = gapflag(c, cur >= 0 ? prv(&c->v, cur, 0) : -1, cur); break;
    case 15: o->type = 0; o->ival = gapflag(c, cur, cur >= 0 ? nxt(&c->v, cur, 0) : -1); break;
    case 16: o->s = posname_w(c, cur >= 0 ? prv(&c->v, cur, 0) : -1); break;
    case 17: o->s = posname_w(c, cur >= 0 ? nxt(&c->v, cur, 0) : -1); break;
    case 18:
        free(c->b18);
        c->b18 = NULL;
        for (x = clause_start(c, cur, c->wseg, c->segs); x >= 0; x = nxt(&c->v, x, 0))
            if (c->v.w[x].type == 0) {
                static const zf_char sl[2] = {'/', 0}, sp[2] = {' ', 0};
                const zf_char *t = wtext(&c->v.w[x]), *nm = posname_w(c, x);
                if (t && nm) {
                    sbuf_add(&c->b18, t);
                    sbuf_add(&c->b18, sl);
                    sbuf_add(&c->b18, nm);
                    sbuf_add(&c->b18, sp);
                }
            }
        o->s = nz(c->b18);
        break;
    default: o->type = 1; o->s = NULL; break;
    }
}

static int opeval(pmctx *c, int op, const fval *m, const uint8_t *cond)
{
    const uint8_t *pm = c->pm;
    int opnd = (int)zf_rd32(cond + 12);
    const zf_char *v = NULL, *o = NULL;
    int vl = 0, ol = 0;
    zf_char obuf[1024];
    if (op >= 1 && op <= 5) {
        switch (op) {
        case 1: return m->ival == opnd;
        case 2: return m->ival < opnd;
        case 3: return m->ival <= opnd;
        case 4: return opnd < m->ival;
        case 5: return opnd <= m->ival;
        }
    }
    /* FUN_18004e81c: value string + pool operand {u16 len, chars} */
    if (m->type == 1) { v = m->s; vl = slen800(v); }
    {
        const uint8_t *q = pm + opnd;
        int i;
        ol = zf_rd16(q);
        if (ol > 1023) ol = 1023;
        for (i = 0; i < ol; i++) obuf[i] = zf_rd16(q + 2 + 2 * i);
        obuf[ol] = 0;
        o = obuf;
    }
    switch (op) {
    case 6: return cmpn(v, o, vl, ol) == 0;
    case 7: return cmpn(v, o, vl < ol ? vl : ol, ol) == 0;
    case 8: return ol <= vl && ol >= 0 && cmpn(v + (vl - ol), o, ol, ol) == 0;
    case 9: return kmp(v, o, vl, ol, c->kmp, 0) != -1;
    case 10: return (vl > 0 || ol < 1) ? kmp(o, v, ol, vl, c->kmp, 0) != -1 : 0;
    case 11: return icmpn(v, o, vl, ol) == 0;
    case 12: return icmpn(v, o, vl < ol ? vl : ol, ol) == 0;
    case 13: return kmp(v, o, vl, ol, c->kmp, 1) != -1;
    case 14: return (vl > 0 || ol < 1) ? kmp(o, v, ol, vl, c->kmp, 1) != -1 : 0;
    case 15: return cmpn(v, o, vl, ol) != 0;
    case 16: return cmpn(v, o, vl < ol ? vl : ol, ol) != 0;
    case 17: return !(ol <= vl && ol >= 0 && cmpn(v + (vl - ol), o, ol, ol) == 0);
    case 18: return icmpn(v, o, vl, ol) != 0;
    case 19: return icmpn(v, o, vl < ol ? vl : ol, ol) != 0;
    }
    return 0;
}

/* returns 0 = rule matched (*val), 1 = no rule / no entry */
static int pm_run(pmctx *c, int *val)
{
    const uint8_t *pm = c->pm;
    int nf = PM32(4), ftab = PM32(0xc), ents = PM32(0x10), idx, r, k, nr, ro;
    const zf_char *t;
    if (c->cur < 0) return 1;
    t = wtext(&c->v.w[c->cur]);
    if (!t) return 1;
    idx = pm_lookup(pm, t);
    if (idx < 0) return 1;
    if (nf > 32) return 1;
    for (k = 0; k < nf; k++) c->f[k].type = 3;
    c->f[nf - 1].type = 1;
    {
        static zf_char S[2] = {'S', 0}, W[2] = {'W', 0};
        c->f[nf - 1].s = c->mode == 'S' ? S : W;
    }
    nr = PM32(ents + 16 * idx + 8);
    ro = PM32(ents + 16 * idx + 12);
    for (r = 0; r < nr; r++) {
        const uint8_t *rule = pm + ro + 16 * r;
        int nc = (int)zf_rd32(rule + 8), co = (int)zf_rd32(rule + 12), ok = 1;
        for (k = 0; k < nc && ok; k++) {
            const uint8_t *cond = pm + co + 16 * k;
            int fid = (int)zf_rd32(cond), op = (int)zf_rd32(cond + 4);
            fval *m;
            if (fid < 0 || fid >= nf) { ok = 0; break; }
            m = &c->f[fid];
            if (m->type == 3) feat(c, fid, m);
            if (m->type == 0) { if (PM32(ftab + 4 * fid) != 0) { ok = 0; break; } }
            else if (PM32(ftab + 4 * fid) != 1) { ok = 0; break; }
            if (!opeval(c, op, m, cond)) ok = 0;
        }
        if (ok) {
            if (zf_rd32(rule) != 0) return 1;   /* string results do not occur in the en-US data */
            *val = (int)zf_rd32(rule + 4);
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------
 * Analyze step 9 (FUN_180060c48 / FUN_1800200dc, "W" mode) */
int zf1_post_words(zf1_post *p, zf_wordlist *wl)
{
    pmctx *c;
    int i;
    if (!p->pm || !wl) return 0;
    c = (pmctx *)calloc(1, sizeof *c);
    if (!c) return -1;
    c->pm = p->pm;
    c->v.w = wl->w;
    c->v.n = wl->n;
    c->mode = 'W';
    for (i = 0; i < wl->n; i++) {
        int v = 0, r, pv;
        const zf_char *t = wtext(&wl->w[i]);
        c->cur = i;
        if (!t || pm_lookup(p->pm, t) < 0) continue;       /* no entry: nothing written */
        r = pm_run(c, &v);
        if (r == 0 && v != 8) {
            wl->w[i].i180 = v;
            wl->w[i].type = ZF_WT_PUNCT;
            pv = prv(&c->v, i, 0);
            if (pv >= 0 && wl->w[pv].type == 0) wl->w[pv].i180 = v;
        } else {
            wl->w[i].i180 = 8;
        }
    }
    free(c->b11);
    free(c->b18);
    free(c);
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------
 * sentence vt+0xb0 = FUN_1800b5aa0: segments */
static int seg_add(zf1_postout *o, int first, const zf_char *val)
{
    zf_segment *n = (zf_segment *)realloc(o->segs, sizeof(zf_segment) * (size_t)(o->nsegs + 1));
    if (!n) return -1;
    o->segs = n;
    n[o->nsegs].first = n[o->nsegs].last = first;
    n[o->nsegs].value = val;
    return o->nsegs++;
}

/* FUN_180019920(w, 0): next word that is not a silence token */
static int next_nonsil(const wv *v, int i)
{
    for (i++; i < v->n; i++)
        if (v->w[i].type != ZF_WT_SILENCE) return i;
    return -1;
}

static int segments(const wv *v, zf1_postout *o)
{
    int w;
    if (v->n <= 0) return 0;
    w = 0;
    if (!a56c(&v->w[0]) || !a144(&v->w[0])) w = next_nonsil(v, 0);
    while (w >= 0) {
        const zf_word *x = &v->w[w];
        if (!nz(x->ne_type)) {
            if (x->fs.sayas) {
                /* engine: frag+0x20 say-as struct exists and its InterpretAs != NULL */
                int s = seg_add(o, w, x->fs.sayas), prev = w, q = w;
                if (s < 0) return -1;
                for (;;) {
                    if (v->w[q].fs.sayas != v->w[prev].fs.sayas) break;
                    o->word_seg[q] = s;
                    prev = q;
                    q = nxt(v, q, 0);
                    if (q < 0) break;
                }
                o->segs[s].last = prev;
                w = q;
            } else {
                w = next_nonsil(v, w);
            }
        } else {
            int s = seg_add(o, w, x->ne_type), prev = w, q = w;
            if (s < 0) return -1;
            for (;;) {
                if (v->w[q].src_off != v->w[prev].src_off) break;
                o->word_seg[q] = s;
                prev = q;
                q = nxt(v, q, 0);
                if (q < 0) break;
            }
            o->segs[s].last = prev;
            w = q;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------
 * FUN_18001bf24: RegularText */
static const zf_char *last_occ(const zf_char *s, int ls, const zf_char *pt, int lp)
{
    int i, k;
    if (ls <= 0 || lp <= 0) return NULL;
    for (i = ls - lp; i >= 0; i--) {
        for (k = 0; k < lp && s[i + k] == pt[k]; k++) {}
        if (k == lp) return s + i;
    }
    return NULL;
}

static int rec_str(const uint8_t *rec, zf_char *out)
{
    int i;
    for (i = 0; i < 10; i++) {
        out[i] = zf_rd16(rec + 2 * i);
        if (!out[i]) break;
    }
    out[i < 10 ? i : 9] = 0;
    return i < 10 ? i : 9;
}

static void suffix_word(const zf1_post *p, zf_word *w)
{
    zf_char *r;
    int n = w->text ? (int)zf_strlen(w->text) : 0, i, k, cls;
    zf_char suf[11];
    r = (zf_char *)malloc(((size_t)n + 16) * sizeof(zf_char));
    if (!r) return;
    for (i = 0; i < n; i++) r[i] = lwr(w->text[i]);
    r[n] = 0;
    /* table 3 ("'"): keep from the last occurrence on */
    for (k = 0; k < p->nsuf[3]; k++) {
        int sl = rec_str(p->sufrec[3] + 20 * k, suf);
        const zf_char *at = last_occ(r, n, suf, sl);
        if (at) {
            memmove(r, at, (zf_strlen(at) + 1) * sizeof(zf_char));
            zf_setstr(&w->regular, r);
            free(r);
            return;
        }
    }
    cls = zf1_pos_class(w->pos);
    {
        static const int tabfor[4] = {-1, 0, 2, 1};   /* class -> table (1 noun, 2 verb, 3 adj) */
        int order[2], no = 0, t;
        if (cls == 1) order[no++] = 0;
        if (cls == 3) order[no++] = 1;
        if (cls == 2) order[no++] = 2;
        (void)tabfor;
        for (t = 0; t < no; t++) {
            int tb = order[t];
            for (k = 0; k < p->nsuf[tb]; k++) {
                int sl = rec_str(p->sufrec[tb] + 20 * k, suf);
                const zf_char *at = last_occ(r, n, suf, sl);
                if (at && at == r + (n - sl)) {
                    zf_char *nr = (zf_char *)malloc(((size_t)sl + 2) * sizeof(zf_char));
                    if (nr) {
                        nr[0] = '-';
                        memcpy(nr + 1, suf, ((size_t)sl + 1) * sizeof(zf_char));
                        zf_setstr(&w->regular, nr);
                        free(nr);
                    }
                    free(r);
                    return;
                }
            }
        }
    }
    zf_setstr(&w->regular, r);
    free(r);
}

/* ------------------------------------------------------------------------------------------------------------
 * FUN_1800b8608: quote pairing */
static int quote_entry(const zf1_post *p, zf_char ch)
{
    int k;
    for (k = 0; k < p->nquote; k++) {
        const uint8_t *q = p->quote + 8 * k;
        if (zf_rd16(q) == ch || zf_rd16(q + 2) == ch) return k;
    }
    return -1;
}

static int quotes(const zf1_post *p, const wv *v, zf_sentence *o)
{
    int stk_idx[512], stk_ent[512], sp = 0, i;
    if (!p->quote_ok) return 0;
    for (i = 0; i < v->n; i++) {
        const zf_word *w = &v->w[i];
        int e, e2, j;
        if (w->type != 0 && w->type != 1) continue;
        if (!w->text || !w->text[0] || w->text[1]) continue;
        e = quote_entry(p, w->text[0]);
        if (e < 0) continue;
        e2 = e;
        if (zf_rd32(p->quote + 8 * e + 4) != 0) e2 = quote_entry(p, zf_rd16(p->quote + 8 * e + 2));
        for (j = sp - 1; j >= 0; j--)
            if (stk_ent[j] == e2) break;
        if (j < 0) {
            if (sp < 512) { stk_idx[sp] = i; stk_ent[sp] = e; sp++; }
        } else {
            struct zf_quote *nq = (struct zf_quote *)realloc(o->quotes, sizeof(struct zf_quote) * (size_t)(o->nquotes + 1));
            if (!nq) return -1;
            o->quotes = nq;
            nq[o->nquotes].open = stk_idx[j];
            nq[o->nquotes].close = i;
            o->nquotes++;
            sp = j;   /* pop the matched item and everything above it */
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------
 * FUN_1800b90a8: parallel structures */
static int in_tab(const zf_word *w, const uint8_t *tab, int n)
{
    int k, i;
    if (!w || !w->text || !w->text[0]) return 0;
    for (k = 0; k < n; k++) {
        const uint8_t *r = tab + 0x2c * k;
        if (zf_rd32(r) != w->tpos) continue;
        /* _wcsicmp (C locale) */
        for (i = 0;; i++) {
            zf_char a = lwr(w->text[i]), b = lwr(zf_rd16(r + 4 + 2 * i));
            if (i >= 20) b = 0;
            if (a != b) break;
            if (!a) return 1;
        }
    }
    return 0;
}

/* FUN_1800b6688 */
static int prev_content(const wv *v, int i)
{
    for (;;) {
        i = prv(v, i, 0);
        if (i < 0) return -1;
        if (a56c(&v->w[i])) return i;
    }
}

typedef struct cl { struct zf_parel *c; int n, cap; } cl;
static int cl_insert(cl *l, int at, int s, int e, int k)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        struct zf_parel *x = (struct zf_parel *)realloc(l->c, sizeof(struct zf_parel) * (size_t)nc);
        if (!x) return -1;
        l->c = x;
        l->cap = nc;
    }
    memmove(l->c + at + 1, l->c + at, sizeof(struct zf_parel) * (size_t)(l->n - at));
    l->c[at].a = s;
    l->c[at].b = e;
    l->c[at].type = k;
    l->n++;
    return 0;
}

static int parallel(const zf1_post *p, const wv *v, const zf1_postout *out, zf_sentence *sp)
{
    int w, last = v->n - 1;
    if (!p->par_ok || v->n <= 0) return 0;
    for (w = 0; w >= 0 && w != last; w = nxt(v, w, 0)) {
        int pw, bound = -1, state = 0, found = 0, cstate = 0;
        cl ps = {0, 0, 0};
        int have = 0;
        if (!in_tab(&v->w[w], p->conj, p->nconj)) continue;
        pw = prev_content(v, w);
        have = pw >= 0;
        if (sp->npar > 0) {
            struct zf_parstruct *lp = &sp->par[sp->npar - 1];
            bound = lp->el[lp->nel - 1].b;
        }
        while (pw >= 0) {
            int start, q, hit, cnt;
            if (cstate == 1) break;
            if (v->w[pw].type == ZF_WT_SILENCE) { pw = prev_content(v, pw); continue; }
            /* FUN_1800b8d80 */
            start = -1;
            q = pw;
            hit = in_tab(&v->w[q], p->sep, p->nsep);
            cnt = 0;
            for (;;) {
                int t;
                if (q == bound || hit || v->w[q].type == 1) break;
                if (out->word_seg && out->word_seg[q] >= 0 && out->segs[out->word_seg[q]].first != q)
                    q = out->segs[out->word_seg[q]].first;
                t = v->w[q].type;
                start = q;
                q = prv(v, q, 0);
                hit = q >= 0 ? in_tab(&v->w[q], p->sep, p->nsep) : 0;
                if (t == 0) cnt++;
                if (q < 0) break;
            }
            found = 0;
            if (start < 0) break;
            if (q < 0 || !hit) {
                if (cnt == 0 || state != 2) break;
                state = 1;
                found = 1;
            } else {
                if (cnt == 0) break;
                if (state == 0) {
                    if (cnt > 2) break;
                    state = 2;
                    found = 1;
                } else if (state != 2) {
                    break;
                } else if (cnt < 3) {
                    found = 1;
                } else {
                    state = 1;
                    found = 1;
                }
            }
            cstate = state;
            if (cl_insert(&ps, 0, start, pw, state) < 0) return -1;
            pw = prev_content(v, start);
        }
        if (have && found) {
            if (ps.n > 1) {
                /* FUN_1800b8bb0: POS class of every conjunct's end word == that of the last conjunct */
                int k, lc = zf1_pos_class(v->w[ps.c[ps.n - 1].b].tpos);
                for (k = 0; k < ps.n - 1; k++)
                    if (zf1_pos_class(v->w[ps.c[k].b].tpos) != lc) { found = 0; break; }
            }
            if (found) {
                /* FUN_1800b8ca4: scan forward from the word after the conjunction */
                int q = nxt(v, w, 0), end = -1;
                while (q >= 0) {
                    if (in_tab(&v->w[q], p->sep, p->nsep)) break;
                    if (v->w[q].type == 1) break;
                    if (out->word_seg && out->word_seg[q] >= 0 && q != out->segs[out->word_seg[q]].last)
                        q = out->segs[out->word_seg[q]].last;
                    if (a144(&v->w[q])) end = q;
                    q = nxt(v, q, 0);
                }
                if (end >= 0) {
                    struct zf_parstruct *np;
                    if (cl_insert(&ps, ps.n, w, end, 3) < 0) return -1;
                    np = (struct zf_parstruct *)realloc(sp->par, sizeof(struct zf_parstruct) * (size_t)(sp->npar + 1));
                    if (!np) return -1;
                    sp->par = np;
                    np[sp->npar].nel = ps.n;
                    np[sp->npar].el = ps.c;
                    sp->npar++;
                    w = end;
                    continue;
                }
            }
        }
        free(ps.c);
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------ */
int zf1_post_sentence(zf1_post *p, zf_sentence *s, zf1_postout *o)
{
    wv v;
    int i;
    memset(o, 0, sizeof *o);
    v.w = s->words;
    v.n = s->nwords;
    o->word_seg = (int *)malloc(sizeof(int) * (size_t)(s->nwords > 0 ? s->nwords : 1));
    if (!o->word_seg) return -1;
    for (i = 0; i < s->nwords; i++) o->word_seg[i] = -1;
    /* sentence vt+0xb0 */
    if (segments(&v, o) < 0) return -1;
    /* CSentAndPuncDetector (S mode on the last word) */
    if (p->pm && s->nwords > 0) {
        pmctx *c = (pmctx *)calloc(1, sizeof *c);
        int val = 0;
        if (!c) return -1;
        c->pm = p->pm;
        c->v = v;
        c->cur = s->nwords - 1;
        c->mode = 'S';
        c->wseg = o->word_seg;
        c->segs = o->segs;
        s->type = pm_run(c, &val) == 0 ? val : 0;
        free(c->b11);
        free(c->b18);
        free(c);
    }
    /* FUN_18001bf24 */
    if (p->suf)
        for (i = 0; i < s->nwords; i++) suffix_word(p, &s->words[i]);
    for (i = 0; i < s->nwords; i++)
        s->words[i].ne_obj = o->word_seg[i] < 0 ? 0 : (o->segs[o->word_seg[i]].first == i ? 1 : 2);
    free(s->quotes);
    s->quotes = NULL;
    s->nquotes = 0;
    for (i = 0; i < s->npar; i++) free(s->par[i].el);
    free(s->par);
    s->par = NULL;
    s->npar = 0;
    if (quotes(p, &v, s) < 0) return -1;
    if (parallel(p, &v, o, s) < 0) return -1;
    return 0;
}
