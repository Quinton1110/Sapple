/* zf1_morph.c - CEnMorph, the English morphological lexicon of MSTTSLoc_OneCore.dll (see zf1_morph.h).
 *
 * Engine: loc!18005c8e0 (lookup) -> loc!180008e50 (suffix stripping + stem lookups) -> loc!18005bbd4 (suffix
 * phones / POS rules, fallback loc!18005c648) -> loc!18005d19c (group nodes by pronunciation into an entry).
 * Stem lookups (loc!18000db0c) query the main lexicon and keep only pronunciations that pass the length filter
 * loc!180056b64 (phones < letters * 1.2475 + extra).  Tables: zf1_morph_tab.h (generated from the Loc DLL).
 * Portable C99. */
#include "zf1_morph.h"
#include <stdlib.h>
#include <string.h>

#include "zf1_morph_tab.h"

#define NOTFOUND (-2147450857)          /* 0x80048017 */
#define PHN 0x182

typedef struct mnode {
    uint32_t type;
    uint16_t pos;
    uint16_t ph[PHN];
} mnode;

typedef struct mlist {
    int n, cap;
    mnode *v;
} mlist;

struct zf1_morph {
    const zf1_lex *lx;
    const zf1_phoneset *ps;
    uint16_t sufph[32][21];             /* rule suffix phones (ids) */
    uint16_t from[32][4], to[32][4];
    int nrule[32];
    unsigned flags[32];
    uint16_t ptab[17][8];               /* +8 .. +0x88 */
    uint16_t posids[64];
    int nposids;
    uint16_t idsil, idmark, idsep;
    uint16_t xk[4], yuw[3][4];
};

/* ---------------- helpers ---------------- */
static int u16len(const uint16_t *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static uint32_t phfl(const struct zf1_morph *m, uint16_t id)
{
    int k;
    if (id >= 128) return 0;
    k = m->ps->byid[id];
    return k < 0 ? 0 : m->ps->flags[k];
}

/* CPhoneme vt+0xf0 (FUN_18004c370): names separated by blanks -> ids */
static int names_to_ids(const struct zf1_morph *m, const char *s, uint16_t *out, int cap)
{
    int k = 0;
    while (*s) {
        const char *b;
        int id;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        b = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        id = zf1_phone_id(m->ps, b, (int)(s - b));
        if (id < 0) return -1;
        if (k < cap - 1) out[k++] = (uint16_t)id;
    }
    out[k] = 0;
    return k;
}

static uint16_t posid(const struct zf1_morph *m, const char *name)
{
    int i;
    if (!name) return 0xFFFF;
    for (i = 0; i < m->lx->nattrnames; i++)
        if (!strcmp(m->lx->attrname[i], name)) return (uint16_t)i;
    return 0xFFFF;
}

static int is_posid(const struct zf1_morph *m, uint16_t id)
{
    int i;
    for (i = 0; i < m->nposids; i++)
        if (m->posids[i] == id) return 1;
    return 0;
}

static void ml_free(mlist *l)
{
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

static mnode *ml_push(mlist *l)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        mnode *nv = (mnode *)realloc(l->v, sizeof(mnode) * (size_t)nc);
        if (!nv) return NULL;
        l->v = nv;
        l->cap = nc;
    }
    memset(&l->v[l->n], 0, sizeof(mnode));
    return &l->v[l->n++];
}

/* ---------------- init ---------------- */
struct zf1_morph *zf1_morph_create(const zf1_dat *d, const zf1_lex *lx, const zf1_phoneset *ps)
{
    struct zf1_morph *m;
    size_t n;
    const uint8_t *r;
    int k, j;
    if (!lx || !lx->L || !ps) return NULL;
    m = (struct zf1_morph *)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->lx = lx;
    m->ps = ps;
    r = zf1_dat_get(d, 0xf81fd1d1, 0xed1348b2, &n);    /* POS id list (u32 n; u16 ids) */
    if (r && n >= 4) {
        uint32_t c = zf_rd32(r);
        for (k = 0; k < (int)c && k < 64 && 4 + 2 * (size_t)k + 2 <= n; k++) m->posids[m->nposids++] = zf_rd16(r + 4 + 2 * k);
    }
    for (k = 0; k < 32; k++) {
        if (names_to_ids(m, zf1_morph_rule[k].phones, m->sufph[k], 21) < 0) { free(m); return NULL; }
        m->nrule[k] = zf1_morph_rule[k].n;
        m->flags[k] = zf1_morph_rule[k].flags;
        for (j = 0; j < 4; j++) {
            m->from[k][j] = posid(m, zf1_morph_rule[k].from[j]);
            m->to[k][j] = posid(m, zf1_morph_rule[k].to[j]);
        }
    }
    for (k = 0; k < 17; k++)
        if (names_to_ids(m, zf1_morph_ptab[k], m->ptab[k], 8) < 0) { free(m); return NULL; }
    m->idsil = (uint16_t)zf1_phone_id(ps, "-SIL-", 5);
    m->idmark = (uint16_t)zf1_phone_id(ps, "-", 1);
    m->idsep = (uint16_t)zf1_phone_id(ps, "&", 1);
    names_to_ids(m, "k s", m->xk, 4);
    names_to_ids(m, "y uw", m->yuw[0], 4);
    names_to_ids(m, "y uh", m->yuw[1], 4);
    names_to_ids(m, "y ax", m->yuw[2], 4);
    return m;
}

void zf1_morph_destroy(struct zf1_morph *m) { free(m); }

#define PT(m, off) ((m)->ptab[((off) / 8) - 1])

/* ---------------- stem lookup with the length filter ---------------- */
static int is_alpha(zf_char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    if (c == 0xAA || c == 0xB5 || c == 0xBA) return 1;
    if (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7) return 1;
    return c >= 0x100;
}

static const uint16_t *u16str(const uint16_t *h, const uint16_t *nd)
{
    int ln = u16len(nd), i;
    if (!ln) return h;
    for (; *h; h++) {
        for (i = 0; i < ln && h[i] == nd[i]; i++) {}
        if (i == ln) return h;
    }
    return NULL;
}

static const zf_char *find_ci(const zf_char *s, zf_char c)
{
    zf_char lc = zf_tolower(c);
    for (; *s; s++)
        if (zf_tolower(*s) == lc) return s;
    return NULL;
}

/* loc!180056890 for a rule with a single letter */
static int extra_rule(const zf_char *word, const uint16_t *pp, zf_char letter, const uint16_t (*seq)[4], int nseq)
{
    int cnt = 0;
    const zf_char *w = word;
    while (pp) {
        const zf_char *p = find_ci(w, letter);
        const uint16_t *q = NULL;
        int k, hit = -1;
        if (!p) return cnt;
        w = p + 1;
        for (k = 0; k < nseq; k++) {
            q = u16str(pp, seq[k]);
            if (q) { hit = k; break; }
        }
        if (hit < 0) return cnt;
        cnt += 1;
        pp = q + u16len(seq[hit]);
    }
    return cnt;
}

static int keep_pron(const struct zf1_morph *m, const zf_char *word, const uint16_t *ph)   /* loc!180056b64 */
{
    uint16_t pp[PHN + 2];
    int np = 0, nl = 0, extra, i;
    uint16_t xs[1][4];
    for (i = 0; ph[i] && np < PHN; i++)
        if (phfl(m, ph[i]) & 3) pp[np++] = ph[i];
    pp[np] = 0;
    for (i = 0; word[i]; i++)
        if (is_alpha(word[i])) nl++;
    memcpy(xs[0], m->xk, sizeof xs[0]);
    extra = extra_rule(word, pp, 'x', (const uint16_t (*)[4])xs, 1);
    extra += extra_rule(word, pp, 'u', (const uint16_t (*)[4])m->yuw, 3);
    return (double)np < (double)nl * 1.2475 + (double)extra;
}

/* loc!18000db0c: lookup stem w[0..n) in the lexicon, one node per (pron, POS), filtered */
static int stem_lookup(const struct zf1_morph *m, const zf_char *w, int n, mlist *out)
{
    zf1_lexent *e;
    zf_char stem[130];
    int i, k, j, kept = 0;
    out->n = 0;
    if (n > 0x7f || n <= 0) return NOTFOUND;
    memcpy(stem, w, sizeof(zf_char) * (size_t)n);
    stem[n] = 0;
    e = (zf1_lexent *)malloc(sizeof *e);
    if (!e) return -1;
    if (!zf1_lex_lookup(m->lx, stem, n, e)) { free(e); return NOTFOUND; }
    for (i = 0; i < e->n; i++) {
        uint16_t plist[16];
        int np = 0;
        for (k = 0; k < e->p[i].nattr; k++) {
            const uint8_t *a = zf1_lex_attrset(m->lx, e->p[i].attrset[k]);
            uint16_t best = 0xFFFF;
            for (; a && zf_rd16(a); a += 2) {
                uint16_t id = zf_rd16(a);
                if (!is_posid(m, id)) continue;
                if (best != 0xFFFF && id <= best) continue;
                best = id;
            }
            if (best == 0xFFFF) continue;
            for (j = 0; j < np; j++)
                if (plist[j] == best) break;
            if (j == np && np < 16) plist[np++] = best;
        }
        for (j = 0; j < np; j++) {
            mnode *nd;
            if (!keep_pron(m, stem, e->p[i].ph)) continue;
            nd = ml_push(out);
            if (!nd) { free(e); return -1; }
            nd->type = 0x1000;
            nd->pos = plist[j];
            memcpy(nd->ph, e->p[i].ph, sizeof(uint16_t) * (size_t)e->p[i].nph);
            nd->ph[e->p[i].nph] = 0;
            kept++;
        }
    }
    free(e);
    return kept ? 0 : NOTFOUND;
}

/* stem = w[0..n) with the last char replaced (repl != 0) or a char appended (app != 0) */
static int stem_variant(const struct zf1_morph *m, const zf_char *w, int n, zf_char repl, zf_char app, mlist *out)
{
    zf_char t[140];
    if (n <= 0 || n > 0x80) return NOTFOUND;
    memcpy(t, w, sizeof(zf_char) * (size_t)n);
    if (repl) t[n - 1] = repl;
    if (app) t[n++] = app;
    return stem_lookup(m, t, n, out);
}

/* ---------------- suffix phones (loc!18005bbd4) ---------------- */
static uint16_t last_phone(const struct zf1_morph *m, const uint16_t *b, int n)   /* CPhoneme vt+0x128 */
{
    int i;
    for (i = n - 1; i >= 0; i--) {
        uint32_t f = phfl(m, b[i]);
        if (f & 0x10000000u) continue;
        if (f & 0x20000000u) continue;
        if (f & 0x80000000u) continue;
        if (b[i] == m->idmark || b[i] == m->idsep || b[i] == m->idsil) continue;
        return b[i];
    }
    return 0;
}

static void cat(uint16_t *b, const uint16_t *s)
{
    int n = u16len(b), k = u16len(s), i;
    if (n + k >= 0x180) return;
    for (i = 0; i <= k; i++) b[n + i] = s[i];
}

static void plural(const struct zf1_morph *m, uint16_t *b)          /* loc!18005cb58 */
{
    uint16_t l = last_phone(m, b, u16len(b));
    uint32_t f = phfl(m, l);
    if (l == PT(m, 0x58)[0]) cat(b, PT(m, 0x30));
    else if (!((f >> 15) & 1) && l != PT(m, 0x28)[0] && l != PT(m, 0x30)[0]) {
        if ((f & 10) != 2) cat(b, PT(m, 0x30));
        else cat(b, PT(m, 0x28));
    } else cat(b, PT(m, 0x10));
}

static void past(const struct zf1_morph *m, uint16_t *b)            /* loc!18005ca0c */
{
    uint16_t l = last_phone(m, b, u16len(b));
    uint32_t f = phfl(m, l);
    if (l == PT(m, 0x50)[0] || l == PT(m, 0x38)[0]) cat(b, PT(m, 0x48));
    else if (!(f & 8) && l != PT(m, 0x30)[0] && l != PT(m, 0x58)[0] && l != PT(m, 0x60)[0] && l != PT(m, 0x68)[0] &&
             l != PT(m, 0x70)[0] && l != PT(m, 0x78)[0])
        cat(b, PT(m, 0x50));
    else cat(b, PT(m, 0x38));
}

/* rules[] = rule indices, rules[0] = innermost (list head) */
static int apply_rules(const struct zf1_morph *m, const int *rules, int nr, mlist *res)
{
    mlist out = {0, 0, NULL};
    uint16_t outpos[5];
    int nout = 0, any = 0, i;
    for (i = 0; i < res->n; i++) {
        uint16_t buf[0x182];
        uint16_t cur[5];
        int ncur = 1, ok = 1, r;
        memcpy(buf, res->v[i].ph, sizeof buf);
        buf[0x181] = 0;
        cur[0] = res->v[i].pos;
        for (r = 0; r < nr && ok; r++) {
            int rule = rules[r], u, j, nn = 0, applied = 0;
            uint16_t nw[5];
            const uint16_t *suf = m->sufph[rule];
            for (u = 0; u < ncur && u <= 4; u++) {
                for (j = 0; j < m->nrule[rule] && j <= 3; j++) {
                    int k;
                    if (m->from[rule][j] != cur[u] || nn >= 5) continue;
                    for (k = 0; k < nn; k++)
                        if (nw[k] == m->to[rule][j]) break;
                    if (k < nn) continue;
                    nw[nn++] = m->to[rule][j];
                    if (nn == 1) {
                        applied = 1;
                        if (suf[0] == PT(m, 0x28)[0] && suf[1] == 0) plural(m, buf);
                        else if (suf[0] != PT(m, 0x38)[0] || suf[1] != 0) {
                            if (rule == 29 || rule == 30) {
                                int bl = u16len(buf);
                                if (bl > 0) buf[bl - 1] = PT(m, 0x28)[0];
                            }
                            if (u16len(suf) + u16len(buf) < 0x180) cat(buf, suf);
                        } else past(m, buf);
                    }
                }
            }
            ok = applied;
            if (nn) { memcpy(cur, nw, sizeof(uint16_t) * (size_t)nn); ncur = nn; }
        }
        if (ok && ncur) {
            int u;
            for (u = 0; u < ncur; u++) {
                int k;
                mnode *nd;
                if (nout >= 5) break;
                for (k = 0; k < nout; k++)
                    if (outpos[k] == cur[u]) break;
                if (k < nout) continue;
                outpos[nout++] = cur[u];
                nd = ml_push(&out);
                nd->type = 0x4000;
                nd->pos = cur[u];
                memcpy(nd->ph, buf, sizeof buf);
                any = 1;
            }
        }
    }
    if (!any) {
        /* loc!18005c648: first node + all suffix phones, POS = distinct targets of the outermost rule */
        uint16_t buf[0x182];
        int r, j, last;
        ml_free(&out);
        if (res->n == 0) return NOTFOUND;
        memcpy(buf, res->v[0].ph, sizeof buf);
        for (r = 0; r < nr; r++)
            if (u16len(m->sufph[rules[r]]) + u16len(buf) < 0x180) cat(buf, m->sufph[rules[r]]);
        last = rules[nr - 1];
        for (j = 0; j < m->nrule[last]; j++) {
            int k;
            mnode *nd;
            for (k = 0; k < j; k++)
                if (m->to[last][k] == m->to[last][j]) break;
            if (k < j) continue;
            nd = ml_push(&out);
            nd->type = 0x4000;
            nd->pos = m->to[last][j];
            memcpy(nd->ph, buf, sizeof buf);
        }
    }
    ml_free(res);
    *res = out;
    return 0;
}

/* ---------------- main analysis (loc!180008e50) ---------------- */
static int analyse(struct zf1_morph *m, const zf_char *W, int L0, mlist *res)
{
    int rules[64], nr = 0;
    int L = L0, found = 0, cont = 1, r = 0, iv9 = 0, i;
    res->n = 0;
    for (;;) {
        int rule = -1, S;
        iv9 = r;
        if (found || !cont) break;
        if (L <= 1) { cont = 0; r = NOTFOUND; continue; }
        for (i = 0; i < 33 && rule == -1; i++) {
            int sl = zf1_morph_suffix[i].len, k;
            if (!(sl + 1 < L)) continue;
            for (k = 0; k < sl; k++)
                if (W[L - sl + k] != (zf_char)(unsigned char)zf1_morph_suffix[i].suffix[k]) break;
            if (k == sl) { rule = zf1_morph_suffix[i].rule; L -= sl; }
        }
        if (rule == -1) { cont = 0; r = NOTFOUND; continue; }
        memmove(rules + 1, rules, sizeof(int) * (size_t)(nr < 63 ? nr : 63));
        rules[0] = rule;
        if (nr < 64) nr++;
        S = L;
        switch (rule) {
        case 0: {
            zf_char last = W[S - 1];
            if (last == 's') { nr = 0; cont = 0; r = NOTFOUND; L = S; continue; }
            r = stem_lookup(m, W, S, res);
            if (r < 0) {
                if (r == NOTFOUND) {
                    if (last == 'e') {
                        int r2 = NOTFOUND;
                        if (S > 1 && W[S - 1] == 'e' && W[S - 2] == 'i') r2 = stem_variant(m, W, S - 1, 'y', 0, res);
                        r = r2;
                        if (r2 < 0) {
                            if (r2 == NOTFOUND) {
                                r = stem_lookup(m, W, S - 1, res);
                                if (r < 0) { if (r != NOTFOUND) cont = 0; }
                                else found = 1;
                            } else cont = 0;
                        } else found = 1;
                    }
                } else cont = 0;
            } else found = 1;
            L = S;
            continue;
        }
        case 0x18: {
            r = stem_variant(m, W, S + 4, 'e', 0, res);   /* "...ably" -> "...able" */
            if (r >= 0) {
                int k;
                for (k = 0; k < res->n; k++) {
                    uint16_t *ph = res->v[k].ph;
                    int n = u16len(ph);
                    if (n > 2 && ph[n - 2] == PT(m, 0x08)[0] && ph[n - 1] == PT(m, 0x08)[1]) {
                        ph[n - 2] = PT(m, 0x88)[0];
                        ph[n - 1] = PT(m, 0x88)[1];
                    }
                }
                nr = 1;
                rules[0] = 11;
                found = 1;
                continue;
            }
            if (r != NOTFOUND) { cont = 0; continue; }
            goto L9090;
        }
        case 0x1a: {
            r = stem_lookup(m, W, S + 2, res);
            if (r < 0) {
                L = S + 2;
                if (r == NOTFOUND) continue;
                cont = 0;
                continue;
            }
            found = 1;
            L = S;
            continue;
        }
        case 0x1c: {
            r = stem_variant(m, W, S, 0, 'y', res);
            if (r >= 0) {
                int k;
                for (k = 0; k < res->n; k++) {
                    int n = u16len(res->v[k].ph);
                    if (n && res->v[k].ph[n - 1] == PT(m, 0x80)[0]) res->v[k].ph[n - 1] = 0;
                }
                found = 1;
            } else if (r != NOTFOUND) cont = 0;
            continue;
        }
        case 0x1d: case 0x1e: case 0x1f: {
            r = stem_lookup(m, W, S + 2, res);
            if (r < 0) {
                L = S + 2;
                if (r == NOTFOUND) continue;
                cont = 0;
                continue;
            }
            {
                int k;
                for (k = 0; k < res->n; k++) {
                    int n = u16len(res->v[k].ph);
                    if (n) res->v[k].ph[n - 1] = PT(m, 0x28)[0];
                }
            }
            found = 1;
            L = S;
            continue;
        }
        default:
            if (iv9 >= 0 || iv9 == NOTFOUND) goto L9090;
            goto L9210;
        }
    L9090:
        if (m->flags[rule] & 1) {
            zf_char c = W[S - 1];
            int doit = 0;
            if (c != 'o') {
                if (c == 'e') { if (rule == 1) doit = 1; }
                else if (c != 'w' && c != 'y') doit = 1;
            }
            if (doit) {
                r = stem_variant(m, W, S, 0, 'e', res);
                if (r >= 0 && S != 0 && W[S - 1] == 'l') {
                    int k;
                    for (k = 0; k < res->n; k++) {
                        uint16_t *ph = res->v[k].ph;
                        int n = u16len(ph);
                        if (n > 1 && ph[n - 2] == PT(m, 0x08)[0] && ph[n - 1] == PT(m, 0x08)[1]) {
                            ph[n - 2] = ph[n - 1];
                            ph[n - 1] = 0;
                        }
                    }
                }
                if (r >= 0) { found = 1; continue; }
                if (r != NOTFOUND) { cont = 0; continue; }
            }
        }
    L9210:
        if (rule == 0xb && W[S - 1] == 'y') { nr = 0; r = NOTFOUND; cont = 0; L = S; continue; }
        if (r < 0 && r != NOTFOUND) goto L932c;
        r = NOTFOUND;
        if (S < 0x80) r = stem_lookup(m, W, S, res);
        if (r < 0) {
            if (r != NOTFOUND) { cont = 0; L = S; continue; }
            if (!(m->flags[rule] & 2)) goto L9335;
            r = NOTFOUND;
            if (S != 0 && W[S - 1] == 'i') r = stem_variant(m, W, S, 'y', 0, res);
            if (r >= 0) { found = 1; L = S; continue; }
            if (r == NOTFOUND) goto L932c;
            cont = 0;
            L = S;
            continue;
        }
        found = 1;
        L = S;
        continue;
    L932c:
        if (r != NOTFOUND) { L = S; continue; }
    L9335:
        if (m->flags[rule] & 4) {
            int r2 = NOTFOUND;
            if (S > 1) {
                zf_char c = W[S - 1];
                int excl = (c == 'o' || c == 'a' || c == 'e' || c == 'f' || c == 'h' || c == 'i' || c == 's' ||
                            c == 'u' || c == 'w' || c == 'y' || c == 'k' || c == 'z');
                if (!excl && c == W[S - 2]) r2 = stem_lookup(m, W, S - 1, res);
            }
            r = r2;
            if (r2 >= 0) { found = 1; L = S; continue; }
            if (r2 != NOTFOUND) { cont = 0; L = S; continue; }
        }
        if (m->flags[rule] & 0x10) {
            r = stem_variant(m, W, S, 0, 'l', res);
            if (r >= 0) {
                int k;
                for (k = 0; k < res->n; k++) {
                    int n = u16len(res->v[k].ph);
                    if (n && res->v[k].ph[n - 1] == PT(m, 0x88)[0]) res->v[k].ph[n - 1] = 0;
                }
                found = 1;
            } else if (r != NOTFOUND) cont = 0;
        }
        L = S;
    }
    if (iv9 >= 0 && found && nr) {
        int n = L0;
        r = apply_rules(m, rules, nr, res);
        if (r >= 0 && n > 4) {
            int p = n - 4, k;
            if (W[p] == 'l') p = n - 5;
            for (k = 0; k < res->n; k++) {
                uint16_t *ph = res->v[k].ph;
                int pl = u16len(ph), tl = u16len(PT(m, 0x18));
                if (pl > tl && !memcmp(ph + pl - tl, PT(m, 0x18), sizeof(uint16_t) * (size_t)tl) &&
                    ((W[p] - 0x61u) & 0xfffbu) != 0) {
                    int k2, nl = u16len(PT(m, 0x20));
                    for (k2 = 0; k2 < nl && k2 < tl - 1; k2++) ph[pl - tl + k2] = PT(m, 0x20)[k2];
                    ph[pl - tl + k2] = 0;
                }
            }
        }
        return r;
    }
    return r < 0 ? r : NOTFOUND;
}

int zf1_morph_lookup(struct zf1_morph *m, const zf_char *w, int n, zf1_lexhit *out)
{
    mlist res = {0, 0, NULL};
    int r, i, k;
    out->n = 0;
    if (!m) return 0;
    r = analyse(m, w, n, &res);
    if (r < 0 || res.n == 0) { ml_free(&res); return 0; }
    /* loc!18005d19c: group nodes with identical phones, attribute sets in node order */
    for (i = 0; i < res.n; i++) {
        zf1_lhpron *hp = NULL;
        for (k = 0; k < out->n; k++)
            if (!memcmp(out->p[k].ph, res.v[i].ph, sizeof(uint16_t) * (size_t)(u16len(res.v[i].ph) + 1))) {
                hp = &out->p[k];
                break;
            }
        if (!hp) {
            if (out->n >= ZF1_LEX_MAXPRON) continue;
            hp = &out->p[out->n++];
            memset(hp, 0, sizeof *hp);
            hp->nph = u16len(res.v[i].ph);
            memcpy(hp->ph, res.v[i].ph, sizeof(uint16_t) * (size_t)(hp->nph + 1));
        }
        hp->source = res.v[i].type;
        if (hp->nset < ZF1_LH_MAXSET) {
            /* FUN_1800572d0: chain pos -> grandparent -> ... (root first) using the attribute definitions */
            uint16_t chain[ZF1_LH_SETLEN];
            int nc = 0, j;
            uint16_t id = res.v[i].pos;
            while (id < (uint16_t)m->lx->nattrdefs && nc < ZF1_LH_SETLEN - 1) {
                uint16_t par = zf_rd16(m->lx->attrdefs + 10 * (size_t)id + 4);
                chain[nc++] = id;
                if (par >= (uint16_t)m->lx->nattrdefs) break;
                id = zf_rd16(m->lx->attrdefs + 10 * (size_t)par + 4);
            }
            for (j = 0; j < nc; j++) hp->set[hp->nset][j] = chain[nc - 1 - j];
            hp->set[hp->nset][nc] = 0;
            hp->nset++;
        }
    }
    ml_free(&res);
    return out->n > 0;
}
