/* zf1_poly.c - context pronunciation rules (CPolyphony, resource e849e61b/4a3c699e) and homograph CRF taggers
 * (CCRFPolyphonyTaggerList, resource d49f77b9/3e135d7f) of the OneCore en-US frontend.  Portable C99.
 *
 * CPolyphony (0x1800c9e30 -> 0x18001ee6c -> 0x18001efa0): for every word (type 0, frag action 0/4, non-empty text)
 *   whose Text is in the rule table, the rules are tried in order; a rule = AND of conditions {param, op, value};
 *   the first rule that holds gives a pronunciation string ("dh ih 1") that becomes the word's current
 *   pronunciation, with pron source (+0x278) 13.
 * CRF (0x180060158): for every word (type 0, action 0/4, +0x278 not 13/14) whose upper-cased Text names one of the
 *   13 models: PreviousCharacters/NextCharacters are set (3 chars each, '^' padded), the linear-chain CRF (a single
 *   token sequence, unigram templates over Token.Prev/NextToken.CaseInsensitiveText) gives label probabilities;
 *   if p(best) > 0.6 the label pronunciation is applied with pron source 22.
 * See notes/fe1_pos.md. */
#include "zf1_pos.h"
#include "zf1_wb.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef ZF1_POLY_DEBUG
#include <stdio.h>
#endif

typedef struct crfmodel {
    char name[16];
    const uint8_t *blob;
    uint32_t nlabels, ntempl;
    float scale;
    const uint8_t *labels[8];           /* UTF-16LE strings in the pool */
    struct { char name[8]; int npart; int dist[4]; int kind[4]; int text[4]; int fn[4]; char *arg[4]; } tpl[32];
    /* kind 0: token at dist (-3..+3 Prev/NextToken), property CaseInsensitiveText or Text (text=1), optional function
       fn 1 IsNumber(), 2 IsInList(arg: 0-separated, 00-terminated), 3 EndWith(arg) -> "1"/"0";
       kind 1/2: Previous/NextCharacters.GetCharacter(dist) */   /* kind 0: token at dist (-3..+3 Prev/NextToken); 1/2: Previous/NextCharacters.GetCharacter(dist) */
    /* vocabulary trie (CVocabTrie) */
    const uint8_t *trie;
    uint16_t nA, nC, nD;
    const uint8_t *A, *C, *D, *E, *G, *H, *root;
    uint32_t nE, nG;
    const uint8_t *w;                   /* weights (float LE), [id * nlabels + label] */
} crfmodel;

typedef struct ruletab {
    const uint8_t *b;                   /* CPolyphony resource (e849e61b/4a3c699e) */
    size_t n;
    int npar, nwords;
    const uint8_t *ptype, *words;
} ruletab;

#define ZF1_NDOM 8
typedef struct polydata {
    const uint8_t *b;                   /* default CPolyphony resource (main .dat) */
    size_t n;
    int npar, nwords;
    const uint8_t *ptype, *words;
    zf1_phoneset ps;
    int nmodels;
    crfmodel m[16];
    /* domain data (enUS.<Domain>.dat, zf1_acro_init): per-domain CPolyphony tables and the acronym CRFs */
    int ndom;
    char domname[ZF1_NDOM][16];
    zf1_dat domdat[ZF1_NDOM];
    ruletab dom[ZF1_NDOM];
    int acro_dom;                       /* index of the domain holding the acronym CRF list, -1 none */
    int nacro;
    crfmodel acro[8];
} polydata;

static void ruletab_init(ruletab *t, const uint8_t *b, size_t n)
{
    memset(t, 0, sizeof *t);
    t->b = b;
    t->n = n;
    if (b) {
        t->npar = (int)zf_rd32(b + 4);
        t->nwords = (int)zf_rd32(b + 8);
        t->ptype = b + zf_rd32(b + 0xc);
        t->words = b + zf_rd32(b + 0x10);
    }
}

/* ------------------------------------------------------------------------------------------------ helpers */
static int wl16(const zf_char *s) { int n = 0; if (s) while (s[n] && n < 0x800) n++; return n; }

static zf_char lc_ascii(zf_char c) { return (c >= 'A' && c <= 'Z') ? (zf_char)(c + 32) : c; }   /* UCRT towlower, C locale */
static zf_char uc_ascii(zf_char c) { return (c >= 'a' && c <= 'z') ? (zf_char)(c - 32) : c; }

/* UCRT iswupper / iswlower (C locale) ranges */
#include "zf1_wcase.h"
static int in_rng(const uint16_t *r, int n, zf_char c)
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
static int is_upper(zf_char c) { return in_rng(zf_wupper_ranges, zf_wupper_N, c); }
static int is_lower(zf_char c) { return in_rng(zf_wlower_ranges, zf_wlower_N, c); }

/* FUN_18005391c: compare first min(n1,n2) units; returns difference or n1-n2 */
static int cmpn(const zf_char *a, const zf_char *b, int n1, int n2)
{
    int n = n1 < n2 ? n1 : n2, i;
    if (!a || !b) return n1 - n2;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    return n1 - n2;
}

/* FUN_18015ef84: same with towlower */
static int cmpni(const zf_char *a, const zf_char *b, int n1, int n2)
{
    int n = n1 < n2 ? n1 : n2, i;
    if (!a || !b) return n1 - n2;
    for (i = 0; i < n; i++) {
        zf_char x = lc_ascii(a[i]), y = lc_ascii(b[i]);
        if (x != y) return (int)x - (int)y;
    }
    return n1 - n2;
}

/* FUN_18015f15c / FUN_180053a04: KMP search of pat (m) in txt (n); returns index or -1 */
static int kmp(const zf_char *txt, const zf_char *pat, int n, int m, int ci)
{
    int f[1024], i, k, j, cnt;
    if (m <= 0 || m > 1023) return -1;
    f[0] = -1;
    k = -1;
    i = 0;
    while (i < m - 1) {
        if (k == -1 || pat[i] == pat[k]) {
            k++;
            i++;
            f[i] = (pat[i] == pat[k]) ? f[k] : k;
        } else {
            k = f[k];
        }
    }
    j = 0;
    i = 0;
    cnt = 0;
    while (i < n) {
        if (j >= m) return cnt - m;
        if (j == -1 || (ci ? lc_ascii(txt[i]) == lc_ascii(pat[j]) : txt[i] == pat[j])) { cnt++; i++; j++; }
        else j = f[j];
    }
    if (j >= m) return cnt - m;
    return -1;
}

/* ------------------------------------------------------------------------------------------------ pron list */
static int pcmp(const zf_char *a, const zf_char *b)
{
    static const zf_char e = 0;
    if (!a) a = &e;
    if (!b) b = &e;
    for (;; a++, b++) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) return 0;
    }
}

/* CTTSPronunciationList vt+0x18 (0x18004d8d0) + word vt+0x88 (0x18014b210): select an existing equal
 * pronunciation or insert it sorted (u16 compare), make it current; set pron source */
void zf1_pron_set(const zf1_phoneset *ps, zf_word *w, const uint16_t *ids, int n, int source)
{
    zf_char *s = (zf_char *)malloc(sizeof(zf_char) * (size_t)(n + 1));
    int i, at;
    if (w->i278 == 0x14) { free(s); return; }
    for (i = 0; i < n; i++) s[i] = ids[i];
    s[n] = 0;
    for (at = 0; at < w->nprons; at++) {
        int c = pcmp(s, w->prons[at]);
        if (c == 0) { w->cur_pron = at; free(s); s = NULL; break; }
        if (c < 0) break;
    }
    if (s) {
        w->prons = (zf_char **)realloc(w->prons, sizeof(zf_char *) * (size_t)(w->nprons + 1));
        memmove(w->prons + at + 1, w->prons + at, sizeof(zf_char *) * (size_t)(w->nprons - at));
        w->prons[at] = s;
        w->nprons++;
        w->cur_pron = at;
    }
    free(w->pron);
    w->pron = n ? zf1_phones_to_str(ps, ids, n) : NULL;
    if (source != 0x17) w->i278 = source;
}

/* "dh ih 1" -> phone ids (phone converter vt+0xf0) */
static int str_to_ids(const zf1_phoneset *ps, const zf_char *s, int n, uint16_t *out, int cap)
{
    int i = 0, k = 0;
    while (i < n) {
        char nm[32];
        int j = 0, id;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        while (i < n && s[i] != ' ' && s[i] != '\t' && j < 31) nm[j++] = (char)s[i++];
        if (!j) break;
        id = zf1_phone_id(ps, nm, j);
        if (id >= 0 && k < cap) out[k++] = (uint16_t)id;
    }
    return k;
}

/* ------------------------------------------------------------------------------------------------ CPolyphony */
typedef struct pval { int type; int ival; const zf_char *s; int slen; zf_char buf[4096]; } pval;

typedef struct pctx {
    const zf1_pos *p;
    const polydata *pd;
    zf_wordlist *wl;
    int cur;
} pctx;

static int prev_ns(const zf_wordlist *wl, int i) { for (i--; i >= 0; i--) if (wl->w[i].type != 3) return i; return -1; }
static int next_ns(const zf_wordlist *wl, int i) { for (i++; i < wl->n; i++) if (wl->w[i].type != 3) return i; return -1; }

static const zf_char EMPTYS[] = { 0 };
static const zf_char *wtext(const zf_word *w) { return (w->text && w->text[0]) ? w->text : NULL; }

/* FUN_1800ca5ac */
static int caseclass(const zf_char *s)
{
    int n = wl16(s), i;
    if (!s || n <= 0) return -1;
    if (!is_upper(s[0])) {
        if (!is_lower(s[0])) return 5;
        for (i = 0; i < n && is_lower(s[i]); i++) {}
        return i == n ? 0x32 : 0x1e;
    }
    for (i = 0; i < n; i++) if (!is_upper(s[i])) break;
    if (i == n) return 0x78;
    if (i == n - 1 && is_lower(s[i])) return 0x6e;
    if (i != 1) return 0x5a;
    for (; i < n && is_lower(s[i]); i++) {}
    return i == n ? 0x46 : 0x5a;
}

/* POS table vt+8: attribute name of a POS id, "unknown" if not in the POS table */
static void posname(const zf1_pos *p, uint16_t pos, pval *v)
{
    const char *nm = NULL;
    int k;
    if (pos < p->nattr) {
        int a = pos, g = 0;
        while (a != 0 && a != 0xFFFF && g++ < 64) a = zf_rd16(p->lx->attrdefs + 10 * a + 4);
        if (a == 0) nm = zf1_lex_attrname(p->lx, pos);
    }
    if (!nm) nm = "unknown";
    for (k = 0; nm[k] && k < 63; k++) v->buf[k] = (unsigned char)nm[k];
    v->buf[k] = 0;
    v->s = v->buf;
    v->slen = k;
    v->type = 1;
}

static void setstr(pval *v, const zf_char *s)
{
    v->type = 1;
    v->s = s ? s : EMPTYS;
    v->slen = wl16(v->s);
}

/* FUN_180055270: " w1 w2 ... " over [from, to) skipping silence words */
static void ctxstr(const zf_wordlist *wl, int from, int to, pval *v)
{
    int k = 0, i;
    if (from >= 0)
        for (i = from; i < to && i < wl->n; i++) {
            const zf_char *t = wtext(&wl->w[i]);
            int n = wl16(t);
            if (wl->w[i].type == 3) continue;
            if (k + n + 2 >= 4096) break;
            v->buf[k++] = ' ';
            if (t) { memcpy(v->buf + k, t, sizeof(zf_char) * (size_t)n); k += n; }
        }
    if (k < 4095) v->buf[k++] = ' ';
    v->buf[k] = 0;
    v->s = v->buf;
    v->slen = k;
    v->type = 1;
}

/* FUN_1800b7fd8: 1 if a and b are separated (or belong to the same non-default NE token), 0 if adjacent */
static int separated(const zf_word *a, const zf_word *b)
{
    static const char dflt[] = "sp:default";
    if (!a || !b) return 0;
    if (a->ne_type && a->ne_type[0] && b->ne_type && b->ne_type[0] && a->src_off == b->src_off &&
        zf_strcmp_a(a->ne_type, dflt) != 0)
        return 1;
    return !((unsigned)b->src_off <= (unsigned)(a->src_off + a->src_len));
}

/* CPolyphony vt+8 (0x1800c9ef0): compute parameter id */
static int param_eval(pctx *c, int id, pval *v)
{
    const zf_wordlist *wl = c->wl;
    const zf_word *cur = &wl->w[c->cur];
    int ip = prev_ns(wl, c->cur), in = next_ns(wl, c->cur);
    const zf_word *pw = ip >= 0 ? &wl->w[ip] : NULL, *nw = in >= 0 ? &wl->w[in] : NULL;
    switch (id) {
    case 0: setstr(v, wtext(cur)); return 0;
    case 1: v->type = 0; v->ival = caseclass(wtext(cur) ? wtext(cur) : EMPTYS); return 0;
    case 2: posname(c->p, cur->pos, v); return 0;
    case 3: setstr(v, pw ? wtext(pw) : NULL); return 0;
    case 4: v->type = 0; v->ival = caseclass(pw && wtext(pw) ? wtext(pw) : EMPTYS); return 0;
    case 5: posname(c->p, pw ? pw->pos : 0xFFFF, v); return 0;
    case 6: setstr(v, nw ? wtext(nw) : NULL); return 0;
    case 7: v->type = 0; v->ival = caseclass(nw && wtext(nw) ? wtext(nw) : EMPTYS); return 0;
    case 8: posname(c->p, nw ? nw->pos : 0xFFFF, v); return 0;
    case 9: {   /* first letter of the name of the next word's first phone */
        uint16_t ph = 0x20;
        const char *nm;
        if (nw && nw->cur_pron >= 0 && nw->prons[nw->cur_pron] && nw->prons[nw->cur_pron][0]) ph = nw->prons[nw->cur_pron][0];
        nm = zf1_phone_name(&c->pd->ps, ph);
        v->buf[0] = nm ? (unsigned char)nm[0] : 0;
        v->buf[1] = 0;
        v->s = v->buf;
        v->slen = v->buf[0] ? 1 : 0;
        v->type = 1;
        return 0;
    }
    case 10: ctxstr(wl, wl->n ? 0 : -1, wl->n, v); return 0;
    case 11: v->type = 0; v->ival = 1; return 0;
    case 12: ctxstr(wl, 0, c->cur, v); return 0;
    case 13: ctxstr(wl, c->cur + 1 < wl->n ? c->cur + 1 : -1, wl->n, v); return 0;
    case 17: v->type = 0; v->ival = separated(pw, cur); return 0;
    case 18: v->type = 0; v->ival = separated(cur, nw); return 0;
    default: return -1;   /* 14-16, 19-21 not used by the en-US data */
    }
}

static int op_eval(int op, const pval *pv, const uint8_t *base, int32_t cval)
{
    const zf_char *P = pv->s, *V;
    zf_char vb[2048];
    int lp = pv->slen, lv, i;
    if (op >= 1 && op <= 5) {
        int a = pv->ival;
        switch (op) {
        case 1: return a == cval;
        case 2: return a < cval;
        case 3: return a <= cval;
        case 4: return cval < a;
        default: return cval <= a;
        }
    }
    lv = zf_rd16(base + cval);
    if (lv > 2047) lv = 2047;
    for (i = 0; i < lv; i++) vb[i] = zf_rd16(base + cval + 2 + 2 * i);
    vb[lv] = 0;
    V = vb;
    switch (op) {
    case 6: return cmpn(P, V, lp, lv) == 0;
    case 7: return cmpn(P, V, lp < lv ? lp : lv, lv) == 0;
    case 8: return lv <= lp && lv >= 0 && cmpn(P + (lp - lv), V, lv, lv) == 0;
    case 9: return kmp(P, V, lp, lv, 0) != -1;
    case 10: return (lp > 0 || lv < 1) && kmp(V, P, lv, lp, 0) != -1;
    case 11: return cmpni(P, V, lp, lv) == 0;
    case 12: return cmpni(P, V, lp < lv ? lp : lv, lv) == 0;
    case 13: return kmp(P, V, lp, lv, 1) != -1;
    case 14: return (lp > 0 || lv < 1) && kmp(V, P, lv, lp, 1) != -1;
    case 15: return cmpn(P, V, lp, lv) != 0;
    case 18: return cmpni(P, V, lp, lv) != 0;
    case 19: return cmpni(P, V, lp < lv ? lp : lv, lv) != 0;
    case 21: return lv <= lp && lv >= 0 && cmpni(P + (lp - lv), V, lv, lv) == 0;
    default: return 0;   /* ops 16, 17, 20 not used by the en-US data */
    }
}

static int find_word(const ruletab *pd, const zf_char *t)
{
    int lo = 0, hi = pd->nwords - 1, n = wl16(t);
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c, i, m;
        const uint8_t *e = pd->b + zf_rd32(pd->words + 16 * mid + 4);
        int el = zf_rd16(e);
        c = n - el;
        m = n < el ? n : el;
        for (i = 0; i < m; i++) {
            zf_char a = t[i], b = zf_rd16(e + 2 + 2 * i);
            if (a != b) { c = (int)a - (int)b; break; }
        }
        if (c < 0) hi = mid - 1;
        else if (c > 0) lo = mid + 1;
        else return mid;
    }
    return -1;
}

/* FUN_18001efa0 on one rule table: returns 1 if the word is not in the table (then the next table is tried) */
static int poly_word_rt(const zf1_pos *p, polydata *pdx, const ruletab *pd, zf_wordlist *wl, int wi, int source)
{
    zf_word *w = &wl->w[wi];
    pctx c;
    pval *cache;
    int k, nr, r, npar = pd->npar;
    const uint8_t *rules;
    k = find_word(pd, w->text);
    if (k < 0) return 1;
    nr = (int)zf_rd32(pd->words + 16 * k + 8);
    rules = pd->b + zf_rd32(pd->words + 16 * k + 12);
    c.p = p; c.pd = pdx; c.wl = wl; c.cur = wi;
    cache = (pval *)malloc(sizeof(pval) * (size_t)npar);
    for (r = 0; r < npar; r++) cache[r].type = 3;
    for (r = 0; r < nr; r++) {
        const uint8_t *ru = rules + 16 * r;
        int nc = (int)zf_rd32(ru + 8), j, ok = 1;
        const uint8_t *cd = pd->b + zf_rd32(ru + 12);
        for (j = 0; j < nc && ok; j++) {
            int par = (int)zf_rd32(cd + 16 * j), op = (int)zf_rd32(cd + 16 * j + 4);
            int32_t val = (int32_t)zf_rd32(cd + 16 * j + 12);
            int want;
            pval *v;
            if (par < 0 || par >= npar) { ok = 0; break; }
            v = &cache[par];
            if (v->type == 3 && param_eval(&c, par, v) < 0) { v->type = 3; ok = 0; break; }
            want = (int)zf_rd32(pd->b + zf_rd32(pd->b + 0xc) + 4 * par);
            if (v->type == 0 ? want != 0 : want != 1) { ok = 0; break; }
            ok = op_eval(op, v, pd->b, val);
        }
        if (ok) {
            uint16_t ids[0x182];
            int n = 0;
#ifdef ZF1_POLY_DEBUG
            { char *u = zf_to_utf8(w->text, -1); fprintf(stderr, "POLY %s rule %d/%d\n", u, r, nr); free(u); }
#endif
            if (zf_rd32(ru) == 1) {
                const uint8_t *s = pd->b + zf_rd32(ru + 4);
                int sl = zf_rd16(s), i;
                zf_char tmp[1024];
                if (sl > 1023) sl = 1023;
                for (i = 0; i < sl; i++) tmp[i] = zf_rd16(s + 2 + 2 * i);
                n = str_to_ids(&pdx->ps, tmp, sl, ids, 0x181);
            }
            zf1_pron_set(&pdx->ps, w, ids, n, source);
            break;
        }
    }
    free(cache);
    return 0;
}

static int dom_index(const polydata *pd, const zf_char *dom)
{
    int i;
    if (!dom || !dom[0]) return -1;
    for (i = 0; i < pd->ndom; i++) if (!zf_strcmp_a(dom, pd->domname[i])) return i;
    return -1;
}

/* FUN_18001ee6c: domain table of the word's domain (+0x1b0) first (source 14), then the default table (13) */
static void poly_word(const zf1_pos *p, polydata *pd, zf_wordlist *wl, int wi)
{
    zf_word *w = &wl->w[wi];
    ruletab def;
    int d;
    if (w->type != 0 || !wtext(w) || (w->fs.action & ~4) != 0) return;
    d = dom_index(pd, w->s1b0);
    if (d >= 0 && pd->dom[d].b && poly_word_rt(p, pd, &pd->dom[d], wl, wi, 0xe) != 1) return;
    ruletab_init(&def, pd->b, pd->n);
    if (def.b) poly_word_rt(p, pd, &def, wl, wi, 0xd);
}

/* ------------------------------------------------------------------------------------------------ CRF */
static uint32_t tdec(const uint8_t *tab, const uint8_t **pp)
{
    const uint8_t *p = *pp;
    uint32_t v = p[0], acc = 0;
    int k = 0;
    while (zf_rd16(tab + 2 * k) <= v) {
        acc += zf_rd16(tab + 2 * k);
        v = (v - zf_rd16(tab + 2 * k)) * 256 + p[k + 1];
        k++;
    }
    *pp = p + k + 1;
    return (acc + v) & 0xFFFF;
}

typedef struct tnode { uint16_t ch, fl; uint32_t cnt; const uint8_t *p12, *q, *cb; } tnode;

static void trie_node(const crfmodel *m, const uint8_t *p, const uint8_t *cb, tnode *o)
{
    uint32_t idx = tdec(m->A, &p);
    o->ch = zf_rd16(m->E + 4 * idx);
    o->fl = zf_rd16(m->E + 4 * idx + 2);
    o->cnt = 0;
    if (o->fl & 4) {
        uint32_t b = p[0];
        if (b >= 0xc0) { o->cnt = (((uint32_t)(p[1] & 0x7f)) | ((b & 0x3f) << 7)) << 8 | p[2]; p += 3; }
        else if (b >= 0x80) { o->cnt = (b & 0x7f) << 8 | p[1]; p += 2; }
        else { o->cnt = b; p += 1; }
    }
    o->p12 = p;
    switch (o->fl & 0x1d0) {
    case 0x10: {
        uint32_t i = tdec(m->D, &p);
        if (!cb) cb = p;
        cb = cb + zf_rd32(m->H + 4 * i);
        break;
    }
    case 0x50: cb = p; break;
    case 0x90: tdec(m->C, &p); break;
    case 0x110: p += 3; break;
    default: break;
    }
    o->q = p;
    o->cb = cb;
}

static const uint8_t *trie_child(const crfmodel *m, const tnode *t)
{
    const uint8_t *p = t->p12;
    switch (t->fl & 0x1c0) {
    case 0: return t->cb;
    case 0x40: return p;
    case 0x80: { uint32_t i = tdec(m->C, &p); return m->root + zf_rd32(m->G + 4 * i); }
    case 0x100: return m->root + ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]);
    default: return NULL;
    }
}

/* FUN_1800e4e8c / FUN_18002c5f0: feature string -> id (-1 if not a full match) */
static int trie_lookup(const crfmodel *m, const zf_char *key, int L)
{
    int it13 = -1, it14 = -1, d8 = 0, pos = 0;
    const uint8_t *cur = m->root;
    uint16_t flags = 0x10;
    while (pos != L && (flags & 0x10) && cur) {
        tnode t;
        trie_node(m, cur, NULL, &t);
        if (t.fl & 1) it13++;
        flags = t.fl;
        while (t.ch != key[pos]) {
            int i1 = (int)t.cnt + it13;
            const uint8_t *cb = t.cb;
            if (flags & 2) {
                if (!(t.fl & 0x200)) return (d8 == L) ? it14 : -1;
                cb = NULL;
            }
            trie_node(m, t.q, cb, &t);
            it13 = i1 + ((t.fl & 1) ? 1 : 0);
            flags = t.fl;
        }
        cur = trie_child(m, &t);
        pos++;
        if (t.fl & 1) { d8 = pos; it14 = it13; }
    }
    return (d8 == L) ? it14 : -1;
}

/* one CRF tagger list resource (polyphony d49f77b9 / acronym cea1be6f):
 * u32 namePoolOff, u32 modelsBase, u32 n, u32 modelOff[n], u32 nameOff[n] */
static int crf_load_list(crfmodel *mm, int max, const uint8_t *r)
{
    uint32_t sp, mb, n, i;
    if (!r) return -1;
    sp = zf_rd32(r);
    mb = zf_rd32(r + 4);
    n = zf_rd32(r + 8);
    if (n > (uint32_t)max) n = (uint32_t)max;
    for (i = 0; i < n; i++) {
        crfmodel *m = &mm[i];
        const uint8_t *o = r + mb + zf_rd32(r + 12 + 4 * i), *nm = r + sp + zf_rd32(r + 12 + 4 * n + 4 * i);
        const uint8_t *pool, *t;
        uint32_t k, triesz;
        memset(m, 0, sizeof *m);
        for (k = 0; k < 15 && zf_rd16(nm + 2 * k); k++) m->name[k] = (char)zf_rd16(nm + 2 * k);
        m->blob = o;
        pool = o + zf_rd32(o + 0x24);
        m->nlabels = zf_rd32(o + 0x2c);
        m->ntempl = zf_rd32(o + 0x30);
        triesz = zf_rd32(o + 0x34);
        {
            uint32_t fb = zf_rd32(o + 0x40);
            memcpy(&m->scale, &fb, 4);
        }
        if (m->nlabels > 8 || m->ntempl > 32) return -2;
        for (k = 0; k < m->nlabels; k++) m->labels[k] = pool + zf_rd32(o + 0x44 + 4 * k);
        for (k = 0; k < m->ntempl; k++) {
            const uint8_t *s = pool + zf_rd32(o + 0x44 + 4 * m->nlabels + 4 * k);
            char buf[2048];
            int j = 0, part = 0;
            while (zf_rd16(s + 2 * j) && j < 2047) { buf[j] = (char)zf_rd16(s + 2 * j); j++; }
            buf[j] = 0;
            /* "U00:Token.PrevToken.PrevToken.CaseInsensitiveText/Token.PrevToken.CaseInsensitiveText" */
            for (j = 0; buf[j] && buf[j] != ':' && j < 7; j++) m->tpl[k].name[j] = buf[j];
            m->tpl[k].name[j] = 0;
            {
                char *q = strchr(buf, ':');
                q = q ? q + 1 : buf;
                while (*q && part < 4) {
                    char *e = q, *x, *end;
                    int inq = 0, kind = 0, dist = 0;
                    while (*e && (inq || *e != '/')) { if (*e == '"') inq = !inq; e++; }
                    end = e;
                    m->tpl[k].fn[part] = 0;
                    m->tpl[k].text[part] = 0;
                    m->tpl[k].arg[part] = NULL;
                    for (x = q; x < end; x++) {
                        if (!strncmp(x, "PrevToken", 9)) { dist--; x += 8; }
                        else if (!strncmp(x, "NextToken", 9)) { dist++; x += 8; }
                        else if (!strncmp(x, "PreviousCharacters.GetCharacter(", 32)) { kind = 1; dist = x[32] - '0'; x += 32; }
                        else if (!strncmp(x, "NextCharacters.GetCharacter(", 28)) { kind = 2; dist = x[28] - '0'; x += 28; }
                        else if (!strncmp(x, ".Text", 5) && (x[5] == '.' || x + 5 == end)) { m->tpl[k].text[part] = 1; x += 4; }
                        else if (!strncmp(x, "IsNumber(", 9)) { m->tpl[k].fn[part] = 1; x += 8; }
                        else if (!strncmp(x, "IsInList(", 9) || !strncmp(x, "EndWith(", 8)) {
                            char *a = (char *)calloc((size_t)(end - x) + 2, 1), *d = a;
                            int in = 0;
                            m->tpl[k].fn[part] = x[0] == 'I' ? 2 : 3;
                            for (x = strchr(x, '(') + 1; x < end && (in || *x != ')'); x++) {
                                if (*x == '"') { if (in) *d++ = 0; in = !in; continue; }
                                if (in) *d++ = *x;
                            }
                            *d = 0;
                            m->tpl[k].arg[part] = a;
                            x = end - 1;
                        }
                    }
                    m->tpl[k].kind[part] = kind;
                    m->tpl[k].dist[part++] = dist;
                    if (!*end) break;
                    q = end + 1;
                }
                m->tpl[k].npart = part;
            }
        }
        t = o + 0x44 + 4 * (m->nlabels + m->ntempl);
        m->trie = t;
        m->nA = zf_rd16(t + 0xc);
        m->nC = zf_rd16(t + 0x10);
        m->nD = zf_rd16(t + 0x12);
        m->nE = zf_rd32(t + 0x1c);
        m->nG = zf_rd32(t + 0x24);
        {
            size_t p = 0x30;
#define AL(q) (((q) & 2) ? (q) + 2 : (q))
            m->A = t + p; p = AL(p + 2 * (size_t)m->nA);
            p = AL(p + 2 * (size_t)zf_rd16(t + 0xe));
            m->C = t + p; p = AL(p + 2 * (size_t)m->nC);
            m->D = t + p; p = AL(p + 2 * (size_t)m->nD);
            m->E = t + p; p += 4 * (size_t)m->nE;
            p += 4 * (size_t)zf_rd32(t + 0x20);
            m->G = t + p; p += 4 * (size_t)m->nG;
            m->H = t + p;
#undef AL
            m->root = t + zf_rd32(t + 0x28);
        }
        m->w = t + triesz;
    }
    return (int)n;
}

static void crf_free_list(crfmodel *mm, int n)
{
    int i, k, j;
    for (i = 0; i < n; i++)
        for (k = 0; k < (int)mm[i].ntempl && k < 32; k++)
            for (j = 0; j < 4; j++) { free(mm[i].tpl[k].arg[j]); mm[i].tpl[k].arg[j] = NULL; }
}

static int load_models(polydata *pd, const zf1_dat *d)
{
    size_t sz = 0;
    int n = crf_load_list(pd->m, 16, zf1_dat_get(d, 0xd49f77b9, 0x3e135d7f, &sz));
    if (n < 0) return n;
    pd->nmodels = n;
    return 0;
}

static float rdf(const uint8_t *p) { uint32_t v = zf_rd32(p); float f; memcpy(&f, &v, 4); return f; }

/* score the CRF on token wi of a sequence (ci[]/tx[] per token); returns the best label, *probp = p(best)
 * (FUN_1800e34c8 -> FUN_1800e2fa8 -> FUN_1800e46b0 for a one-token sequence) */
static int crf_eval(const crfmodel *m, const zf_char *const *ci, const zf_char *const *tx, int n, int wi,
                    const zf_char *pc, const zf_char *nc, float *probp)
{
    float sc[8];
    int k, j, best = 0;
    for (k = 0; k < (int)m->nlabels; k++) sc[k] = 0.0f;
    for (k = 0; k < (int)m->ntempl; k++) {
        zf_char key[1024];
        int L = 0, id;
        for (j = 0; m->tpl[k].name[j]; j++) key[L++] = (unsigned char)m->tpl[k].name[j];
        key[L++] = ':';
        for (j = 0; j < m->tpl[k].npart; j++) {
            int pos = wi + m->tpl[k].dist[j];
            if (j) key[L++] = '/';
            if (m->tpl[k].kind[j]) {
                const zf_char *cs = m->tpl[k].kind[j] == 1 ? pc : nc;
                key[L++] = cs ? cs[m->tpl[k].dist[j] & 3] : '^';
            } else if (pos < 0 || pos >= n) {
                int beyond = pos < 0 ? -pos : pos - (n - 1);
                key[L++] = '_'; key[L++] = 'B';
                if (beyond <= 2) { key[L++] = pos < 0 ? '-' : '+'; key[L++] = (zf_char)('0' + beyond); }
            } else {
                const zf_char *v = m->tpl[k].text[j] ? tx[pos] : ci[pos];
                int l = wl16(v);
                if (m->tpl[k].fn[j]) {
                    int r = 0, a;
                    const char *s = m->tpl[k].arg[j];
                    switch (m->tpl[k].fn[j]) {
                    case 1:   /* FUN_18008f6d8: non-empty, all iswdigit */
                        r = l > 0;
                        for (a = 0; a < l; a++) if (v[a] < '0' || v[a] > '9') r = 0;
                        break;
                    case 2:   /* IsInList (FUN_1801531e0): exact match with one of the arguments */
                        for (; s && *s; s += strlen(s) + 1) {
                            int sl = (int)strlen(s);
                            if (sl == l) {
                                for (a = 0; a < l && v[a] == (unsigned char)s[a]; a++) {}
                                if (a == l) { r = 1; break; }
                            }
                        }
                        break;
                    default: {  /* EndWith (FUN_180152fd0 -> FUN_180049e44) */
                        int sl = s ? (int)strlen(s) : 0;
                        if (s && sl <= l) {
                            for (a = 0; a < sl && v[l - sl + a] == (unsigned char)s[a]; a++) {}
                            r = a == sl;
                        }
                        break;
                    }
                    }
                    key[L++] = r ? '1' : '0';
                } else {
                    if (L + l > 1000) l = 0;
                    if (v) memcpy(key + L, v, sizeof(zf_char) * (size_t)l);
                    L += l;
                }
            }
        }
        key[L] = 0;
        id = trie_lookup(m, key, L);
        if (id != -1)
            for (j = 0; j < (int)m->nlabels; j++) sc[j] += rdf(m->w + 4 * ((size_t)id * m->nlabels + (size_t)j));
    }
    {
        double lz = 0;
        float lzf = 0.0f;
        for (k = 0; k < (int)m->nlabels; k++) {
            sc[k] *= m->scale;
            if (k == 0) lz = sc[0];
            else {
                double a = lzf, b = sc[k], mn = a <= b ? a : b, mx = a <= b ? b : a;
                lz = mx;
                if (-50.0 < mn - mx) lz = log(exp(mn - mx) + 1.0) + mx;
            }
            lzf = (float)lz;
            if (sc[k] > sc[best]) best = k;
        }
        *probp = (float)exp((double)(sc[best] - lzf));
    }
    return best;
}

static void crf_word(const zf1_pos *p, polydata *pd, zf_wordlist *wl, int wi)
{
    zf_word *w = &wl->w[wi];
    char up[64];
    int k, mi = -1, i, n;
    const crfmodel *m;
    zf_char pc[4], nc[4];

    if (w->type != 0 || (w->fs.action != 0 && w->fs.action != 4) || w->i278 == 0xd || w->i278 == 0xe) return;
    if (!wtext(w)) return;
    n = wl16(w->text);
    if (n > 60) return;
    for (k = 0; k < n; k++) {
        zf_char c = uc_ascii(w->text[k]);
        if (c > 0x7f) return;
        up[k] = (char)c;
    }
    up[n] = 0;
    for (k = 0; k < pd->nmodels; k++) if (!strcmp(pd->m[k].name, up)) { mi = k; break; }
    if (mi < 0) return;
    m = &pd->m[mi];
    /* FUN_1800cadb0: PreviousCharacters (previous words' Text, last char first) / NextCharacters */
    {
        int c = 0;
        for (i = wi - 1; i >= 0 && c < 3; i--) {
            const zf_char *t = wtext(&wl->w[i]);
            int l = wl16(t), j;
            for (j = l - 1; j >= 0 && c < 3; j--) pc[c++] = t[j];
        }
        while (c < 3) pc[c++] = '^';
        pc[3] = 0;
        c = 0;
        for (i = wi + 1; i < wl->n && c < 3; i++) {
            const zf_char *t = wtext(&wl->w[i]);
            int l = wl16(t), j;
            for (j = 0; j < l && c < 3; j++) nc[c++] = t[j];
        }
        while (c < 3) nc[c++] = '^';
        nc[3] = 0;
        zf_setstr(&w->prev_chars, pc);
        zf_setstr(&w->next_chars, nc);
    }
    {
        const zf_char **ci = (const zf_char **)malloc(sizeof(zf_char *) * (size_t)wl->n * 2);
        float prob;
        int best;
        for (i = 0; i < wl->n; i++) { ci[i] = wl->w[i].ci; ci[wl->n + i] = wl->w[i].text; }
        best = crf_eval(m, ci, ci + wl->n, wl->n, wi, pc, nc, &prob);
        free((void *)ci);
        if (p->crf_trace) p->crf_trace(p->crf_trace_ctx, wi, prob, best);
        if ((double)prob > 0.6) {
            const uint8_t *ls = m->labels[best];
            zf_char tmp[512];
            uint16_t ids[0x182];
            int l = 0, cnt;
            while (zf_rd16(ls + 2 * l) && l < 511) { zf_char c = zf_rd16(ls + 2 * l); tmp[l++] = c == '%' ? ' ' : c; }
            cnt = str_to_ids(&pd->ps, tmp, l, ids, 0x181);
            zf1_pron_set(&pd->ps, w, ids, cnt, 0x16);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ API */
static polydata *poly_get(zf1_pos *p)
{
    polydata *pd;
    size_t n = 0;
    const uint8_t *ph;
    if (p->poly) return (polydata *)p->poly;
    pd = (polydata *)calloc(1, sizeof *pd);
    if (!pd) return NULL;
    pd->b = zf1_dat_get(p->dat, 0xe849e61b, 0x4a3c699e, &pd->n);
    if (pd->b) {
        pd->npar = (int)zf_rd32(pd->b + 4);
        pd->nwords = (int)zf_rd32(pd->b + 8);
        pd->ptype = pd->b + zf_rd32(pd->b + 0xc);
        pd->words = pd->b + zf_rd32(pd->b + 0x10);
    }
    ph = zf1_dat_get(p->dat, 0x29a5584b, 0x153f1b64, &n);
    zf1_phoneset_init(&pd->ps, ph, n);
    load_models(pd, p->dat);
    pd->acro_dom = -1;
    p->poly = pd;
    return pd;
}

int zf1_poly_apply(zf1_pos *p, zf_wordlist *wl)
{
    polydata *pd = poly_get(p);
    int i;
    if (!pd) return -1;
    if (pd->b)
        for (i = 0; i < wl->n; i++) poly_word(p, pd, wl, i);
    for (i = 0; i < wl->n; i++) crf_word(p, pd, wl, i);
    return 0;
}

int zf1_poly_rules(zf1_pos *p, zf_wordlist *wl)
{
    polydata *pd = poly_get(p);
    int i;
    if (!pd || !pd->b) return -1;
    for (i = 0; i < wl->n; i++) poly_word(p, pd, wl, i);
    return 0;
}

int zf1_poly_crf(zf1_pos *p, zf_wordlist *wl)
{
    polydata *pd = poly_get(p);
    int i;
    if (!pd) return -1;
    for (i = 0; i < wl->n; i++) crf_word(p, pd, wl, i);
    return 0;
}

void zf1_poly_free(zf1_pos *p)
{
    polydata *pd = (polydata *)p->poly;
    int i;
    if (pd) {
        crf_free_list(pd->m, pd->nmodels);
        crf_free_list(pd->acro, pd->nacro);
        for (i = 0; i < pd->ndom; i++) zf1_dat_free(&pd->domdat[i]);
    }
    free(p->poly);
    p->poly = NULL;
}

/* ------------------------------------------------------------------------------------------------ domains + acronyms */
int zf1_acro_init(zf1_pos *p, const char *datdir) { return zf1_acro_init_lcid(p, datdir, 1033); }

int zf1_acro_init_lcid(zf1_pos *p, const char *datdir, int lcid)
{
    static const char *const doms[][2] = { { "address", "Address" }, { "name", "Name" }, { "message", "Message" },
                                            { "media", "Media" }, { "computer", "Computer" },
                                            { "companyname", "CompanyName" } };
    polydata *pd = poly_get(p);
    size_t i, nd = sizeof doms / sizeof *doms;
    char path[1024];
    const zf1_domfile *loc;
    int nloc = zf1_domain_files(lcid, &loc);   /* ClassicVoices patch: a non-en-US locale's own list */
    if (!pd) return -1;
    pd->acro_dom = -1;
    if (!datdir) datdir = "C:\\Windows\\Speech_OneCore\\Engines\\TTS\\en-US\\";
    if (nloc) nd = (size_t)nloc;
    for (i = 0; i < nd && pd->ndom < ZF1_NDOM; i++) {
        size_t l = strlen(datdir), n = 0;
        const uint8_t *r;
        zf1_dat *dd = &pd->domdat[pd->ndom];
        if (l + 40 >= sizeof path) return -2;
        memcpy(path, datdir, l);
        if (l && path[l - 1] != '\\' && path[l - 1] != '/') path[l++] = '/';
        if (nloc) {
            size_t k;
            snprintf(path + l, sizeof path - l, "%s", loc[i].file);
            if (zf1_dat_load_ci(dd, path) != 0) continue;
            /* the upstream's poly domain names are lower case ("companyname") */
            for (k = 0; loc[i].domain[k] && k < sizeof pd->domname[0] - 1; k++)
                pd->domname[pd->ndom][k] = (char)(loc[i].domain[k] >= 'A' && loc[i].domain[k] <= 'Z' ? loc[i].domain[k] + 32 : loc[i].domain[k]);
            pd->domname[pd->ndom][k] = 0;
        } else {
            sprintf(path + l, "enUS.%s.dat", doms[i][1]);
            if (zf1_dat_load(dd, path) != 0) continue;
            strcpy(pd->domname[pd->ndom], doms[i][0]);
        }
        r = zf1_dat_get(dd, 0xe849e61b, 0x4a3c699e, &n);
        ruletab_init(&pd->dom[pd->ndom], r, n);
        r = zf1_dat_get(dd, 0xcea1be6f, 0x0fbd602c, &n);
        if (r && pd->acro_dom < 0) {
            int m = crf_load_list(pd->acro, 8, r);
            if (m > 0) { pd->nacro = m; pd->acro_dom = pd->ndom; }
        }
        pd->ndom++;
    }
    return pd->ndom;
}

static zf_char *upper_ascii_dup(const zf_char *s, int n)
{
    zf_char *r = (zf_char *)malloc(sizeof(zf_char) * (size_t)(n + 1));
    int i;
    for (i = 0; i < n; i++) r[i] = uc_ascii(s[i]);
    r[n] = 0;
    return r;
}

typedef struct acrotok { int word, off, len; zf_char *text, *ci; const zf_char *dom; zf_char *label; } acrotok;
typedef struct acrobrk { acrotok *t; int n, cap, word; const zf_char *src; const zf_char *dom; } acrobrk;

static int acro_emit(void *ctx, int off, int len)
{
    acrobrk *b = (acrobrk *)ctx;
    acrotok *t;
    if (b->n == b->cap) {
        b->cap = b->cap ? 2 * b->cap : 64;
        b->t = (acrotok *)realloc(b->t, sizeof(acrotok) * (size_t)b->cap);
    }
    t = &b->t[b->n++];
    memset(t, 0, sizeof *t);
    t->word = b->word;
    t->off = off;
    t->len = len;
    t->text = zf_strndup(b->src + off, (size_t)len);
    t->ci = upper_ascii_dup(b->src + off, len);
    t->dom = b->dom;
    return 0;
}

/* CAcronymHandler (FUN_180079408): temp sentence = plain words + entity tokens re-broken by the word breaker
 * (FUN_180065c34); acronym CRF of the token's domain (domain handler vt+0x38 = FUN_1800e1270, model by upper-case
 * text) on each token (FUN_1800c3940); labels (p > 0.6, '_' -> ' ') replace the text: inside entities the entity
 * text is rebuilt around the labelled tokens (FUN_18002957c), plain words get the label as Text (split at ' ')
 * (FUN_180063dc4). */
int zf1_acro_apply(zf1_pos *p, zf_wordlist *wl, const zf1_wb *wb)
{
    polydata *pd = poly_get(p);
    acrobrk b;
    const zf_char **ci, **tx;
    int i, k, changed = 0;
    if (!pd || pd->acro_dom < 0 || !pd->nacro) return 0;
    memset(&b, 0, sizeof b);
    for (i = 0; i < wl->n; i++) {
        zf_word *w = &wl->w[i];
        b.word = i;
        b.dom = w->s1b0;
        if (w->ne_type && w->ne_type[0]) {
            if (w->text && w->text[0] && wb) {
                b.src = w->text;
                zf1_wb_break(wb, w->text, (int)zf_strlen(w->text), i == wl->n - 1, acro_emit, &b);
            }
        } else {
            int n = (int)zf_strlen(w->text);
            b.src = w->text;
            acro_emit(&b, 0, n);
            free(b.t[b.n - 1].ci);
            b.t[b.n - 1].ci = zf_strdup(w->ci);
            b.t[b.n - 1].off = -1;   /* whole plain word */
        }
    }
    ci = (const zf_char **)malloc(sizeof(zf_char *) * (size_t)(b.n + 1) * 2);
    tx = ci + b.n + 1;
    for (k = 0; k < b.n; k++) { ci[k] = b.t[k].ci; tx[k] = b.t[k].text; }
    for (k = 0; k < b.n; k++) {
        acrotok *t = &b.t[k];
        char up[64];
        int n = t->text ? (int)zf_strlen(t->text) : 0, j, mi = -1;
        float prob;
        int best;
        if (!n || n > 60 || dom_index(pd, t->dom) != pd->acro_dom) continue;
        for (j = 0; j < n; j++) { zf_char c = uc_ascii(t->text[j]); if (c > 0x7f) break; up[j] = (char)c; }
        if (j < n) continue;
        up[n] = 0;
        for (j = 0; j < pd->nacro; j++) if (!strcmp(pd->acro[j].name, up)) { mi = j; break; }
        if (mi < 0) continue;
        best = crf_eval(&pd->acro[mi], ci, tx, b.n, k, NULL, NULL, &prob);
        if (p->crf_trace) p->crf_trace(p->crf_trace_ctx, -1 - k, prob, best);
        if ((double)prob > 0.6) {
            const uint8_t *ls = pd->acro[mi].labels[best];
            int l = 0;
            zf_char tmp[512];
            while (zf_rd16(ls + 2 * l) && l < 511) { zf_char c = zf_rd16(ls + 2 * l); tmp[l++] = c == '_' ? ' ' : c; }
            tmp[l] = 0;
            t->label = zf_strdup(tmp);
            changed = 1;
        }
    }
    free((void *)ci);
    if (changed) {
        /* rebuild, from the last word backwards so that inserted words do not shift pending indices */
        for (i = wl->n - 1; i >= 0; i--) {
            zf_word *w = &wl->w[i];
            if (w->ne_type && w->ne_type[0]) {
                int any = 0, pos = 0, L = 0, cap, tl = (int)zf_strlen(w->text);
                zf_char *nt;
                for (k = 0; k < b.n; k++) if (b.t[k].word == i && b.t[k].label) any = 1;
                if (!any) continue;
                cap = tl + 1;
                for (k = 0; k < b.n; k++) if (b.t[k].word == i && b.t[k].label) cap += (int)zf_strlen(b.t[k].label);
                nt = (zf_char *)malloc(sizeof(zf_char) * (size_t)cap);
                for (k = 0; k < b.n; k++) {
                    acrotok *t = &b.t[k];
                    int ll;
                    if (t->word != i || !t->label) continue;
                    if (t->off > pos) { memcpy(nt + L, w->text + pos, sizeof(zf_char) * (size_t)(t->off - pos)); L += t->off - pos; }
                    ll = (int)zf_strlen(t->label);
                    memcpy(nt + L, t->label, sizeof(zf_char) * (size_t)ll);
                    L += ll;
                    pos = t->off + t->len;
                }
                if (tl > pos) { memcpy(nt + L, w->text + pos, sizeof(zf_char) * (size_t)(tl - pos)); L += tl - pos; }
                nt[L] = 0;
                free(w->text);
                w->text = nt;
                free(w->ci);
                w->ci = upper_ascii_dup(nt, L);
            } else {
                const zf_char *lab = NULL, *sp;
                for (k = 0; k < b.n; k++) if (b.t[k].word == i) { lab = b.t[k].label; break; }
                if (!lab) continue;
                sp = lab;
                while (*sp && *sp != ' ') sp++;
                {
                    /* first part replaces this word, the rest become new words after it */
                    zf_char *first = zf_strndup(lab, (size_t)(sp - lab));
                    int at = i;
                    free(w->text);
                    w->text = first;
                    free(w->ci);
                    w->ci = upper_ascii_dup(first, (int)zf_strlen(first));
                    while (*sp == ' ') {
                        const zf_char *q = sp + 1, *e = q;
                        zf_word *nw, tmpw;
                        while (*e && *e != ' ') e++;
                        zf_word_copy(&tmpw, &wl->w[i]);
                        nw = zf_wl_insert(wl, ++at);
                        zf_word_free(nw);
                        *nw = tmpw;
                        free(nw->text);
                        nw->text = zf_strndup(q, (size_t)(e - q));
                        free(nw->ci);
                        nw->ci = upper_ascii_dup(nw->text, (int)(e - q));
                        sp = e;
                    }
                }
            }
        }
    }
    for (k = 0; k < b.n; k++) { free(b.t[k].text); free(b.t[k].ci); free(b.t[k].label); }
    free(b.t);
    return 0;
}
