/* zf1_pos.c - POS tagger of the OneCore en-US frontend (CPOSTaggerImpl 0x1800569d0 + CPosTagger 0x180062190).
 *
 * The model (resource 0cb71848/827032e3) is a Brill transformation-based tagger:
 *   table 1 = 219 lexical rules for unknown words (templates 0x101..0x116), table 2 = 1436 contextual rules
 *   (templates 0x201..0x21e), 12-byte records {u16 kind, u16 from, u16 to, i16 arg, u32 strOff}, each table ended
 *   by a zero record; strings are UTF-16 at resource+strOff.  "S-T-A-R-T" is the boundary word, tag -2 (0xFFFE)
 *   the boundary tag.  There is NO n-gram/HMM decoder: FUN_18002ef70 is the contextual-rule pass.
 * Portable C99.  See notes/fe1_pos.md. */
#include "zf1_pos.h"
#include <stdlib.h>
#include <string.h>

static const zf_char START[] = { 'S', '-', 'T', '-', 'A', '-', 'R', '-', 'T', 0 };
static const zf_char EMPTY[] = { 0 };
static const zf_char SPACE[] = { ' ', 0 };

/* ---------------- string helpers on possibly unaligned UTF-16LE pool strings ---------------- */
/* pool strings are 2-byte aligned inside the 8-aligned resource, so direct zf_char access is fine on LE hosts;
 * to stay portable we copy them out. */
typedef struct { zf_char s[256]; int n; } pstr;

static void pool_get(const uint8_t *p, pstr *o)
{
    int n = 0;
    if (p)
        while (n < 255) {
            zf_char c = zf_rd16(p + 2 * n);
            if (!c) break;
            o->s[n++] = c;
        }
    o->s[n] = 0;
    o->n = n;
}

static int wcmp(const zf_char *a, const zf_char *b)
{
    if (!a) a = EMPTY;
    if (!b) b = EMPTY;
    while (*a && *a == *b) a++, b++;
    return (int)*a - (int)*b;
}

static int wlen(const zf_char *a) { int n = 0; if (a) while (a[n]) n++; return n; }

static int main_lookup(void *ctx, const zf_char *w, int n, uint16_t *sets, int cap);

/* ---------------- init ---------------- */
int zf1_pos_init(zf1_pos *p, const zf1_dat *d, const zf1_lex *lx)
{
    size_t sz = 0, off;
    const uint8_t *r = zf1_dat_get(d, 0x0cb71848, 0x827032e3, &sz);
    const uint8_t *pt;
    size_t ptsz = 0;
    int t, i, n22;
    uint16_t list22[64];
    memset(p, 0, sizeof *p);
    p->lx = lx;
    p->dat = d;
    p->unknown = ZF_POS_UNKNOWN;
    p->lookup = main_lookup;
    p->lookup_ctx = (void *)lx;
    if (!r) return -1;
    /* rule tables */
    off = 0;
    for (t = 0; t < 2; t++) {
        int cnt = 0, cap = 256;
        zf1_brill_rule *rs = (zf1_brill_rule *)malloc(sizeof *rs * (size_t)cap);
        while (off + 12 <= sz) {
            const uint8_t *q = r + off;
            int zero = 1;
            for (i = 0; i < 12; i++) if (q[i]) { zero = 0; break; }
            off += 12;
            if (zero) break;
            if (cnt == cap) { cap *= 2; rs = (zf1_brill_rule *)realloc(rs, sizeof *rs * (size_t)cap); }
            rs[cnt].kind = zf_rd16(q);
            rs[cnt].from = zf_rd16(q + 2);
            rs[cnt].to = zf_rd16(q + 4);
            rs[cnt].arg = (int16_t)zf_rd16(q + 6);
            rs[cnt].str = r + zf_rd32(q + 8);
            cnt++;
        }
        if (t == 0) { p->lexr = rs; p->nlex = cnt; } else { p->ctxr = rs; p->nctx = cnt; }
    }
    /* POS table: main POS = nearest ancestor-or-self listed in f81fd1d1 (FUN_1800379ac), for ids below "POS" */
    pt = zf1_dat_get(d, 0xf81fd1d1, 0xed1348b2, &ptsz);
    n22 = pt ? (int)zf_rd32(pt) : 0;
    if (n22 > 64) n22 = 64;
    for (i = 0; i < n22; i++) list22[i] = zf_rd16(pt + 4 + 2 * i);
    for (i = 0; i < 256; i++) p->mainpos[i] = 0xFFFF;
    p->nattr = lx->nattrdefs < 256 ? lx->nattrdefs : 256;
    for (i = 0; i < p->nattr; i++) {
        int id = i, guard = 0, k;
        for (k = 0; k < n22; k++) if (list22[k] == i) p->ismain[i] = 1;
        while (id >= 0 && id < p->nattr && guard++ < 64) {
            for (k = 0; k < n22; k++) if (list22[k] == id) break;
            if (k < n22) { p->mainpos[i] = (uint16_t)id; break; }
            id = zf_rd16(lx->attrdefs + 10 * id + 4);   /* parent */
            if (id == 0xFFFF) break;
        }
    }
    return 0;
}

void zf1_pos_free(zf1_pos *p)
{
    zf1_poly_free(p);
    free(p->lexr);
    free(p->ctxr);
    p->lexr = p->ctxr = NULL;
}

uint16_t zf1_pos_main(const zf1_pos *p, uint16_t id)
{
    /* POS table vt+0x20: entry.main if the id is in the table, else unknown.  Ids outside the POS subtree are
     * not in the table; within the subtree ids without main POS return 0xFFFF. */
    if (id >= p->nattr) return p->unknown;
    if (p->mainpos[id] == 0xFFFF) {
        /* is the id under the POS root at all? (root = attr id 0 "POS") */
        int a = id, g = 0;
        while (a != 0 && a != 0xFFFF && g++ < 64) a = zf_rd16(p->lx->attrdefs + 10 * a + 4);
        return a == 0 ? 0xFFFF : p->unknown;
    }
    return p->mainpos[id];
}

void zf1_pos_set(const zf1_pos *p, zf_word *w, uint16_t pos)
{
    w->pos = pos;
    w->tpos = zf1_pos_main(p, pos);
}

static int has_main(const zf1_pos *p, uint16_t id) { return id < p->nattr && p->mainpos[id] != 0xFFFF; }

/* ---------------- candidates ---------------- */
static int main_lookup(void *ctx, const zf_char *w, int n, uint16_t *sets, int cap)
{
    const zf1_lex *lx = (const zf1_lex *)ctx;
    zf1_lexent e;
    int j, k, m = 0;
    if (!zf1_lex_lookup(lx, w, n, &e)) return -1;
    for (j = 0; j < e.n; j++)
        for (k = 0; k < e.p[j].nattr; k++) {
            const uint8_t *a = zf1_lex_attrset(lx, e.p[j].attrset[k]);
            for (; a && zf_rd16(a) && m < cap - 1; a += 2) sets[m++] = zf_rd16(a);
            if (m < cap) sets[m++] = 0;
        }
    return m;
}

void zf1_pos_set_lookup(zf1_pos *p, zf1_lexlookup_fn fn, void *ctx)
{
    p->lookup = fn ? fn : main_lookup;
    p->lookup_ctx = fn ? ctx : (void *)p->lx;
}

static int lex_exists(const zf1_pos *p, const zf_char *s, int n)
{
    uint16_t sets[512];
    return p->lookup(p->lookup_ctx, s, n, sets, 512) >= 0;
}

int zf1_pos_candidates(const zf1_pos *p, const zf_char *text, int n, uint16_t *out, int cap)
{
    uint16_t sets[512];
    int ns = p->lookup(p->lookup_ctx, text, n, sets, 512), i = 0, cnt = 0;
    if (ns < 0) return -1;
    while (i < ns) {
        /* FUN_18005b104: last accepted id with a main POS, strictly increasing */
        uint16_t best = 0xFFFF;
        int m;
        for (; i < ns && sets[i]; i++) {
            uint16_t id = sets[i];
            if (!has_main(p, id)) continue;
            if (best != 0xFFFF && id <= best) continue;
            best = id;
        }
        i++;
        if (best == 0xFFFF) continue;
        for (m = 0; m < cnt; m++) if (out[m] == best) break;
        if (m == cnt && cnt < cap) out[cnt++] = best;
    }
    return cnt;
}

/* ---------------- lexical rules (FUN_18003d1e0) for one unknown node ---------------- */
static int starts_with(const zf_char *w, const zf_char *s)
{
    int lw = wlen(w), ls = wlen(s), i;
    if (!w || !s || ls > lw) return 0;
    for (i = 0; i < ls; i++) if (w[i] != s[i]) return 0;
    return 1;
}

static int ends_with(const zf_char *w, const zf_char *s)
{
    int lw = wlen(w), ls = wlen(s), i;
    if (!w || !s || ls > lw) return 0;
    for (i = 0; i < ls; i++) if (w[lw - ls + i] != s[i]) return 0;
    return 1;
}

static void lexical_rules(const zf1_pos *p, zf1_posnode *nd, const zf1_posnode *prev, const zf1_posnode *next)
{
    int r;
    const zf_char *w = nd->text;
    if (!w) return;
    for (r = 0; r < p->nlex; r++) {
        const zf1_brill_rule *ru = &p->lexr[r];
        uint16_t np = nd->pos;
        pstr s;
        zf_char buf[300];
        int k = ru->kind, hit = 0, lw, ls;
        if (nd->pos != ru->from && ru->from != 0) continue;
        pool_get(ru->str, &s);
        lw = wlen(w);
        ls = s.n;
        switch (k) {
        case 0x101: hit = 1; break;
        case 0x102: {   /* wcschr(str, word[0]) */
            int i;
            if (w[0] == 0) hit = 1;
            for (i = 0; i < ls && !hit; i++) if (s.s[i] == w[0]) hit = 1;
            break;
        }
        case 0x105: case 0x106: {   /* wcsstr(word, str) */
            int i, j;
            if (ls == 0) hit = 1;
            for (i = 0; i + ls <= lw && !hit; i++) {
                for (j = 0; j < ls && w[i + j] == s.s[j]; j++) {}
                if (j == ls) hit = 1;
            }
            break;
        }
        case 0x107: case 0x10b:   /* str + word in lexicon */
            if (ls + lw < 300) {
                memcpy(buf, s.s, sizeof(zf_char) * (size_t)ls);
                memcpy(buf + ls, w, sizeof(zf_char) * (size_t)lw);
                hit = lex_exists(p, buf, ls + lw);
            }
            break;
        case 0x108: case 0x10c:   /* word + str in lexicon */
            if (lw < 0x80 && ls + lw < 300) {
                memcpy(buf, w, sizeof(zf_char) * (size_t)lw);
                memcpy(buf + lw, s.s, sizeof(zf_char) * (size_t)ls);
                hit = lex_exists(p, buf, ls + lw);
            }
            break;
        case 0x109: case 0x10d:   /* delete prefix -> in lexicon */
            if (starts_with(w, s.s)) hit = lex_exists(p, w + ls, lw - ls);
            break;
        case 0x10a: case 0x10e:   /* delete suffix -> in lexicon */
            if (ends_with(w, s.s) && lw - ls < 0x80) hit = lex_exists(p, w, lw - ls);
            break;
        case 0x10f: case 0x111: hit = starts_with(w, s.s); break;
        case 0x110: case 0x112: hit = ends_with(w, s.s); break;
        case 0x113: case 0x115:
            if (next && wcmp(next->text, s.s) == 0) hit = 1;
            else if (!next && wcmp(START, s.s) == 0) hit = 1;
            break;
        case 0x114: case 0x116:
            if (prev && wcmp(prev->text, s.s) == 0) hit = 1;
            else if (!prev && wcmp(START, s.s) == 0) hit = 1;
            break;
        default: break;
        }
        if (hit) np = ru->to;
        if (nd->pos != np) nd->pos = np;
    }
}

/* ---------------- contextual rules (FUN_18002ef70) ---------------- */
#define TAG(x) ((x) ? (x)->pos : 0)
#define WD(x) ((x)->text ? (x)->text : EMPTY)

static int ctx_match(const zf1_brill_rule *ru, const zf1_posnode *cur,
                     const zf1_posnode *p1, const zf1_posnode *p2, const zf1_posnode *p3,
                     const zf1_posnode *n1, const zf1_posnode *n2, const zf1_posnode *n3)
{
    uint16_t arg = (uint16_t)ru->arg;
    int bnd = ru->arg == -2;
    pstr s, pre;
    const zf_char *w2;
    int k;
    pool_get(ru->str, &s);
    switch (ru->kind) {
    case 0x201: return n1 ? TAG(n1) == arg : bnd;
    case 0x202: return n2 ? TAG(n2) == arg : bnd;
    case 0x203:
        if ((n1 && TAG(n1) == arg) || (n2 && TAG(n2) == arg)) return 1;
        return bnd && !n2;
    case 0x204:
        if ((n1 && TAG(n1) == arg) || (n2 && TAG(n2) == arg) || (n3 && TAG(n3) == arg)) return 1;
        return bnd && !n3;
    case 0x205:
        if (p1 && TAG(p1) == arg) return 1;
        return bnd && !p1;
    case 0x207:
        if (p1 && TAG(p1) == arg) return 1;
        /* fall through */
    case 0x206:
        if (p2 && TAG(p2) == arg) return 1;
        return bnd && !p2;
    case 0x208:
        if ((p1 && TAG(p1) == arg) || (p2 && TAG(p2) == arg) || (p3 && TAG(p3) == arg)) return 1;
        return bnd && !p3;
    case 0x209: return wcmp(WD(cur), s.s) == 0;
    case 0x20a: return n1 ? wcmp(WD(n1), s.s) == 0 : wcmp(s.s, START) == 0;
    case 0x20b: return n2 ? wcmp(WD(n2), s.s) == 0 : wcmp(s.s, START) == 0;
    case 0x20c:
        if (n1 && wcmp(WD(n1), s.s) == 0) return 1;
        return n2 ? wcmp(WD(n2), s.s) == 0 : wcmp(s.s, START) == 0;
    case 0x20d:
        if (n1 && wcmp(WD(n1), s.s) == 0) return 1;
        if (n2 && wcmp(WD(n2), s.s) == 0) return 1;
        return n3 ? wcmp(WD(n3), s.s) == 0 : wcmp(s.s, START) == 0;
    case 0x20e: return p1 ? wcmp(WD(p1), s.s) == 0 : wcmp(s.s, START) == 0;
    case 0x20f: return p2 ? wcmp(WD(p2), s.s) == 0 : wcmp(s.s, START) == 0;
    case 0x210:
        if (p1 && wcmp(WD(p1), s.s) == 0) return 1;
        return p2 ? wcmp(WD(p2), s.s) == 0 : wcmp(s.s, START) == 0;
    case 0x211:
        if (p1 && wcmp(WD(p1), s.s) == 0) return 1;
        if (p2 && wcmp(WD(p2), s.s) == 0) return 1;
        return p3 ? wcmp(WD(p3), s.s) == 0 : wcmp(s.s, START) == 0;
    case 0x212:
        if (!((p1 && TAG(p1) == arg) || (bnd && !p1))) return 0;
        return n1 ? TAG(n1) == s.s[0] : s.s[0] == 0xFFFE;
    case 0x213:
        if (!(p1 ? TAG(p1) == s.s[0] : s.s[0] == 0xFFFE)) return 0;
        return p2 ? TAG(p2) == arg : bnd;
    case 0x214:
        if (!(n1 ? TAG(n1) == arg : bnd)) return 0;
        return n2 ? TAG(n2) == s.s[0] : s.s[0] == 0xFFFE;
    case 0x216:
        if (!((p1 && TAG(p1) == arg) || (bnd && !p1))) return 0;
        return wcmp(WD(cur), s.s) == 0;
    case 0x218:
        if (!(n1 ? TAG(n1) == arg : bnd)) return 0;
        return wcmp(WD(cur), s.s) == 0;
    case 0x21a:
        if (!((p2 && TAG(p2) == arg) || (bnd && !p2))) return 0;
        return wcmp(WD(cur), s.s) == 0;
    case 0x21c:
        if (!(n2 ? TAG(n2) == arg : bnd)) return 0;
        return wcmp(WD(cur), s.s) == 0;
    default: break;
    }
    /* two-word templates: str = w1 w2 split at arg; pre = first arg chars (max 127) */
    k = (uint16_t)ru->arg;
    if (k > s.n) k = s.n;
    memcpy(pre.s, s.s, sizeof(zf_char) * (size_t)k);
    pre.s[k] = 0;
    pre.n = k;
    w2 = s.s + k;
    switch (ru->kind) {
    case 0x215:   /* cur == w2 && prev1 == pre */
        if (wcmp(WD(cur), w2) != 0) return 0;
        return p1 ? wcmp(WD(p1), pre.s) == 0 : wcmp(START, pre.s) == 0;
    case 0x219:   /* cur == w2 && prev2 == pre */
        if (wcmp(WD(cur), w2) != 0) return 0;
        return p2 ? wcmp(WD(p2), pre.s) == 0 : wcmp(START, pre.s) == 0;
    case 0x217:   /* next1 == w2 && cur == pre */
        if (!(n1 ? wcmp(WD(n1), w2) == 0 : wcmp(START, w2) == 0)) return 0;
        return wcmp(WD(cur), pre.s) == 0;
    case 0x21b:   /* next2 == w2 && cur == pre */
        if (!(n2 ? wcmp(WD(n2), w2) == 0 : wcmp(START, w2) == 0)) return 0;
        return wcmp(WD(cur), pre.s) == 0;
    case 0x21d:   /* prev2 == w2 && prev1 == pre */
        if (!(p2 ? wcmp(WD(p2), w2) == 0 : wcmp(START, w2) == 0)) return 0;
        return p1 ? wcmp(WD(p1), pre.s) == 0 : wcmp(START, pre.s) == 0;
    case 0x21e:   /* next2 == w2 && next1 == pre */
        if (!(n2 ? wcmp(WD(n2), w2) == 0 : wcmp(START, w2) == 0)) return 0;
        return n1 ? wcmp(WD(n1), pre.s) == 0 : wcmp(START, pre.s) == 0;
    default: return 0;
    }
}

void zf1_pos_run(const zf1_pos *p, zf1_posnode *nd, int n)
{
    int i, r;
    /* FUN_1800621e8: initial tags */
    for (i = 0; i < n; i++) {
        zf1_posnode *x = &nd[i];
        uint16_t t;
        if (x->kind == 3) continue;
        t = x->ncand ? x->cand[0] : p->unknown;
        if (t == p->unknown) x->kind = 2;
        else x->kind = (x->ncand == 1) ? 3 : 1;
        x->pos = (x->kind == 2) ? p->unknown : t;
    }
    /* lexical rules for unknown words */
    for (i = 0; i < n; i++)
        if (nd[i].kind == 2) lexical_rules(p, &nd[i], i ? &nd[i - 1] : NULL, i + 1 < n ? &nd[i + 1] : NULL);
    /* contextual rules */
    for (r = 0; r < p->nctx; r++) {
        const zf1_brill_rule *ru = &p->ctxr[r];
        for (i = 0; i < n; i++) {
            zf1_posnode *x = &nd[i];
            if (x->kind == 3 || x->pos != ru->from) continue;
            if (x->kind != 2) {
                int c, ok = 0;
                for (c = 0; c < x->ncand; c++) if (x->cand[c] == ru->to) { ok = 1; break; }
                if (!ok) continue;
            }
            if (ctx_match(ru, x, i > 0 ? &nd[i - 1] : NULL, i > 1 ? &nd[i - 2] : NULL, i > 2 ? &nd[i - 3] : NULL,
                          i + 1 < n ? &nd[i + 1] : NULL, i + 2 < n ? &nd[i + 2] : NULL, i + 3 < n ? &nd[i + 3] : NULL))
                x->pos = ru->to;
        }
    }
    /* FUN_18005bc60: map back to the lexicon POS */
    for (i = 0; i < n; i++) {
        zf1_posnode *x = &nd[i];
        if (x->kind == 2) continue;
        if (x->kind == 3 && x->ncand != 1) continue;
        if (!(x->pos < p->nattr && p->ismain[x->pos])) { x->pos = p->unknown; continue; }
        {
            int c;
            for (c = 0; c < x->nlexc; c++)
                if (zf1_pos_main(p, x->lexc[c]) == x->pos) { x->pos = x->lexc[c]; break; }
        }
    }
}

/* ---------------- CPOSTaggerImpl::Tag ---------------- */
int zf1_pos_tag(zf1_pos *p, zf_wordlist *wl, int from, int to)
{
    zf1_posnode *nd;
    int *idx, n = 0, i;
    if (to > wl->n) to = wl->n;
    if (from >= to) return 0;
    nd = (zf1_posnode *)calloc((size_t)(to - from), sizeof *nd);
    idx = (int *)malloc(sizeof(int) * (size_t)(to - from));
    if (!nd || !idx) { free(nd); free(idx); return -1; }
    for (i = from; i < to; i++) {
        zf_word *w = &wl->w[i];
        zf1_posnode *x;
        if (!w->text || !w->text[0]) continue;   /* vt+0x80 == NULL */
        x = &nd[n];
        idx[n++] = i;
        if (w->type != 0) {
            x->text = SPACE;
            x->pos = p->unknown;
            x->kind = 3;
            continue;
        }
        x->text = w->text;
        if (w->pos == 0xFFFF || w->pos == p->unknown) { x->pos = p->unknown; x->kind = 0; }
        else { x->pos = w->pos; x->kind = 3; }
        {
            uint16_t lc[32];
            int c = -1, k, m;
            if (p->word_sets) {
                zf1_lexlookup_fn save = p->lookup;
                void *savec = p->lookup_ctx;
                uint16_t sets[512];
                int ns = p->word_sets(p->word_sets_ctx, w, sets, 512);
                if (ns >= 0) {   /* reuse the set->POS reduction of zf1_pos_candidates on the attached entry */
                    int i2 = 0;
                    c = 0;
                    while (i2 < ns) {
                        uint16_t best = 0xFFFF;
                        for (; i2 < ns && sets[i2]; i2++)
                            if (has_main(p, sets[i2]) && (best == 0xFFFF || sets[i2] > best)) best = sets[i2];
                        i2++;
                        if (best == 0xFFFF) continue;
                        for (m = 0; m < c; m++) if (lc[m] == best) break;
                        if (m == c && c < 32) lc[c++] = best;
                    }
                }
                (void)save; (void)savec;
            }
            if (c < 0) c = zf1_pos_candidates(p, w->text, (int)zf_strlen(w->text), lc, 32);
            if (c < 0) c = 0;
            for (k = 0; k < c; k++) {
                uint16_t mp;
                if (lc[k] == p->unknown) continue;
                x->lexc[x->nlexc++] = lc[k];
                mp = zf1_pos_main(p, lc[k]);
                for (m = 0; m < x->ncand; m++) if (x->cand[m] == mp) break;
                if (m == x->ncand) x->cand[x->ncand++] = mp;
            }
        }
    }
    zf1_pos_run(p, nd, n);
    for (i = 0; i < n; i++) zf1_pos_set(p, &wl->w[idx[i]], nd[i].pos);
    free(nd);
    free(idx);
    return 0;
}

/* ---------------- RegularText suffix classes (FUN_18001bf24, run after Analyze in 0x18005c794) ---------------- */
/* POS category (CPOSTag vt+0x10): index of the top-level attribute below "POS" in the engine's name table */
static int pos_category(const zf1_pos *p, uint16_t pos)
{
    static const char *const cat[18] = { "unknown", "NOM", "VER", "ADJ", "DET", "NUM", "AUX", "PRO", "ART", "ADV",
                                         "CON", "ADP", "INT", "PAR", "MEW", "AUW", "SYM", "OTH" };
    int a = pos, prev = -1, g = 0, k;
    const char *nm;
    if (pos >= p->nattr) return 0;
    while (a != 0 && a != 0xFFFF && g++ < 64) { prev = a; a = zf_rd16(p->lx->attrdefs + 10 * a + 4); }
    if (a != 0 || prev < 0) return 0;
    nm = zf1_lex_attrname(p->lx, prev);
    for (k = 0; nm && k < 18; k++) if (!strcmp(cat[k], nm)) return k;
    return 0;
}

static zf_char lower_c(zf_char c) { return (c >= 'A' && c <= 'Z') ? (zf_char)(c + 32) : c; }   /* UCRT towlower, C locale */

/* last occurrence of s[ls] in r[lr] (FUN_18001c3f8), -1 if none */
static int last_occ(const zf_char *r, int lr, const zf_char *s, int ls)
{
    int i, j;
    if (lr <= 0 || ls <= 0) return -1;
    for (i = lr - ls; i >= 0; i--) {
        for (j = 0; j < ls && r[i + j] == s[j]; j++) {}
        if (j == ls) return i;
    }
    return -1;
}

int zf1_pos_regular(const zf1_pos *p, zf_wordlist *wl, int from, int to)
{
    size_t sz = 0;
    const uint8_t *r = zf1_dat_get(p->dat, 0x5554ba64, 0x09397c04, &sz);
    const uint8_t *tab[4];
    uint32_t cnt[4], rs, t, o;
    int i;
    if (!r) return 0;
    rs = zf_rd32(r + 4);
    o = 8;
    for (t = 0; t < 4; t++) {
        cnt[t] = zf_rd32(r + o);
        tab[t] = r + o + 4;
        o += 4 + rs * cnt[t];
    }
    if (to > wl->n) to = wl->n;
    for (i = from; i < to; i++) {
        zf_word *w = &wl->w[i];
        int n = (int)zf_strlen(w->text), k, done = 0, c;
        zf_char *reg = (zf_char *)malloc(sizeof(zf_char) * (size_t)(n + 32));
        for (k = 0; k < n; k++) reg[k] = lower_c(w->text[k]);
        reg[n] = 0;
        /* list 3 ("'"): keep the string from the last occurrence on ("doesn't" -> "'t") */
        for (k = 0; k < (int)cnt[3] && !done; k++) {
            zf_char s[16];
            int ls = 0, at;
            while (ls < 10 && zf_rd16(tab[3] + rs * k + 2 * ls)) { s[ls] = zf_rd16(tab[3] + rs * k + 2 * ls); ls++; }
            at = last_occ(reg, n, s, ls);
            if (at >= 0) {
                memmove(reg, reg + at, sizeof(zf_char) * (size_t)(n - at + 1));
                n -= at;
                done = 1;
            }
        }
        if (!done) {
            c = pos_category(p, w->pos);
            t = c == 1 ? 0 : c == 3 ? 1 : c == 2 ? 2 : 99;
            if (t < 3)
                for (k = 0; k < (int)cnt[t]; k++) {
                    zf_char s[16];
                    int ls = 0;
                    while (ls < 10 && zf_rd16(tab[t] + rs * k + 2 * ls)) { s[ls] = zf_rd16(tab[t] + rs * k + 2 * ls); ls++; }
                    if (ls <= n && last_occ(reg, n, s, ls) == n - ls) {
                        reg[0] = '-';
                        memcpy(reg + 1, s, sizeof(zf_char) * (size_t)ls);
                        reg[ls + 1] = 0;
                        break;
                    }
                }
        }
        free(w->regular);
        w->regular = reg[0] ? reg : (free(reg), (zf_char *)NULL);
    }
    return 0;
}
