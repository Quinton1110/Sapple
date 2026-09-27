/* eva_crf.c - Eva's CRF prosody models (BR2 breaks, TON boundary tones, ACL pitch accents).  See eva_crf.h and
 * notes/eva_prosody.md.  [S] = transcribed from the decompile, [G] = inferred. */
#include "eva_crf.h"
#include "zf2_int.h"
#include "eva_nn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *zf1_pos_name(unsigned pos);   /* zf1_post.c: POS table object vt+8 (names) */
int zf1_pos_class(unsigned pos);           /*             vt+0x10 (classes) */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ------------------------------------------------------------------ CVocabTrie (Loc FUN_18001160c layout,
 * reverse lookup FUN_180005e80 = depth-first rank of the terminal nodes; walk as tools/vocabtrie.py) */
typedef struct { const uint8_t *b; long T1, T3, T4, NODES, T7, T8, ROOT; } Trie;
static void trie_init(Trie *t, const uint8_t *b, long off) {
#define H(k) rd16(b + off + 2 * (k))
#define U(k) rd32(b + off + 2 * (k))
    long p = off + 0x30;
    t->b = b; t->T1 = p;
    p += 2 * H(6); p += (p & 2); /* T2 */
    p += 2 * H(7); p += (p & 2); t->T3 = p;
    p += 2 * H(8); p += (p & 2); t->T4 = p;
    p += 2 * H(9); p += (p & 2); t->NODES = p;
    p += 4 * (long)U(0xe);       /* T6 */
    t->T7 = p + 4 * (long)U(0x10);
    t->T8 = t->T7 + 4 * (long)U(0x12);
    t->ROOT = off + (long)U(0x14);
#undef H
#undef U
}
static unsigned tdec(const Trie *t, long T, long *pp) {
    long p = *pp; unsigned v = t->b[p], acc = 0; int i = 0; unsigned th = rd16(t->b + T);
    while (th <= v) { acc += th; i++; v = ((v - th) * 256 + t->b[p + i]) & 0xFFFF; th = rd16(t->b + T + 2 * i); }
    *pp = p + i + 1;
    return (acc + v) & 0xFFFF;
}
/* node: char, flags, and (flag 4) the number of words below it (the skip count of the reverse lookup
 * FUN_180005e80) */
static void tnode(const Trie *t, long *pp, unsigned *c, unsigned *f, unsigned *val) {
    unsigned idx = tdec(t, t->T1, pp);
    *c = rd16(t->b + t->NODES + 4 * idx); *f = rd16(t->b + t->NODES + 4 * idx + 2);
    *val = 0;
    if (*f & 4) {
        const uint8_t *q = t->b + *pp; unsigned b0 = q[0];
        if (b0 >= 0xc0) { *val = (((unsigned)q[1] & 0x7f) | ((b0 & 0x3f) << 7)) << 8 | q[2]; *pp += 3; }
        else if (b0 >= 0x80) { *val = (b0 & 0x7f) << 8 | q[1]; *pp += 2; }
        else { *val = b0; *pp += 1; }
    }
}
static long tchild(const Trie *t, unsigned f, long p12, long f0) {
    unsigned k = f & 0x1c0;
    if (k == 0) return f0;
    if (k == 0x40) return p12;
    if (k == 0x80) { long q = p12; unsigned idx = tdec(t, t->T3, &q); return t->ROOT + (long)rd32(t->b + t->T7 + 4 * idx); }
    if (k == 0x100) return t->ROOT + ((long)t->b[p12] << 16 | (long)t->b[p12 + 1] << 8 | t->b[p12 + 2]);
    return -1;
}
static char *cv_strdup(const char *a) { size_t n = strlen(a) + 1; char *r = malloc(n); if (r) memcpy(r, a, n); return r; }
static int put_utf8(char *o, unsigned c) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xc0 | c >> 6); o[1] = (char)(0x80 | (c & 0x3f)); return 2; }
    o[0] = (char)(0xe0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 0x3f)); o[2] = (char)(0x80 | (c & 0x3f)); return 3;
}
/* iterate the sibling list at base (transliteration of the matcher, tools/vocabtrie.py level()) */
typedef int (*sib_fn)(void *ctx, unsigned c, unsigned f, unsigned val, long child);
static int tsiblings(const Trie *t, long base, sib_fn fn, void *ctx) {
    unsigned c, f, val; long p = base, p12, p11, f0 = -1, q;
    if (base < 0) return 0;
    tnode(t, &p, &c, &f, &val);
    p12 = p;
    unsigned k = f & 0x1d0;
    if (k == 0x10) { q = p; unsigned idx = tdec(t, t->T4, &q); f0 = q + (long)rd32(t->b + t->T8 + 4 * idx); p = q; }
    else if (k == 0x90) { q = p; tdec(t, t->T3, &q); p = q; }
    else if (k == 0x110) p += 3;
    p11 = p;
    for (;;) {
        int r = fn(ctx, c, f, val, tchild(t, f, p12, f0));
        if (r) return r;
        if (f & 2) { if (!(f & 0x200)) return 0; f0 = -1; }
        tnode(t, &p11, &c, &f, &val);
        p12 = p11;
        long p4 = f0;
        k = f & 0x1d0;
        if (k == 0x10) { q = p12; unsigned idx = tdec(t, t->T4, &q); if (f0 < 0) f0 = q; p11 = q; p4 = f0 + (long)rd32(t->b + t->T8 + 4 * idx); }
        else if (k == 0x50) p4 = p12;
        else if (k == 0x90) { q = p12; tdec(t, t->T3, &q); p11 = q; }
        else if (k == 0x110) p11 = p12 + 3;
        f0 = p4;
    }
}

/* string -> feature id = depth-first rank of its terminal node (a node's own word before the words below it) */
typedef struct { const uint16_t *s; int n, e; long rank; long next; int found; unsigned f; } Look;
static int look_fn(void *ctx, unsigned c, unsigned f, unsigned val, long child) {
    Look *L = (Look *)ctx;
    if (c == L->s[L->e]) { L->found = 1; L->f = f; L->next = child; return 1; }
    L->rank += (long)(f & 1) + (long)val;
    return 0;
}
static long trie_rank(const Trie *t, const uint16_t *s, int n) {
    Look L; long base = t->ROOT;
    memset(&L, 0, sizeof L); L.s = s; L.n = n;
    if (n <= 0) return -1;
    for (L.e = 0; L.e < n; L.e++) {
        L.found = 0;
        tsiblings(t, base, look_fn, &L);
        if (!L.found) return -1;
        if (L.e == n - 1) return (L.f & 1) ? L.rank : -1;
        L.rank += L.f & 1;
        if (!(L.f & 0x10) || L.next < 0) return -1;
        base = L.next;
    }
    return -1;
}

/* depth-first enumeration (self-check: every word's rank must equal its position) */
typedef struct { const Trie *t; uint16_t buf[1024]; int len; long count, bad; } Dump;
static int dump_fn(void *ctx, unsigned c, unsigned f, unsigned val, long child);
static void tdump(Dump *D, long base, int depth) {
    if (depth > 400 || D->len >= 1023) return;
    struct { Dump *D; int depth; } x = {D, depth};
    (void)x;
    tsiblings(D->t, base, dump_fn, D);
}
static int dump_fn(void *ctx, unsigned c, unsigned f, unsigned val, long child) {
    Dump *D = (Dump *)ctx;
    (void)val;
    D->buf[D->len++] = (uint16_t)c;
    if (f & 1) { if (trie_rank(D->t, D->buf, D->len) != D->count) D->bad++; D->count++; }
    if ((f & 0x10) && child >= 0 && D->len < 1023) tdump(D, child, 0);
    D->len--;
    return 0;
}

/* ------------------------------------------------------------------ model */
struct EvaCrf {
    uint8_t *file; long size; size_t maplen;
    int nlabel, ntempl, nfeat;
    char **label, **templ;
    Trie trie;
    const float *w;       /* nfeat x nlabel */
    const float *x;       /* nlabel x nlabel transitions */
    float costf;
    int acl_boundary;     /* the ACL extractor spells out-of-range positions "Boundary" (its vocabulary has no "_B") */
    int nsuf; char **suf; /* RegularText suffix classes ("-tion", ...) found in the BR2 vocabulary [G] */
};

static char *wdup(const uint8_t *p, const uint8_t *end) {
    size_t n = 0; while (p + 2 * n + 1 < end && rd16(p + 2 * n)) n++;
    char *s = malloc(3 * n + 1); int o = 0;
    for (size_t i = 0; i < n; i++) o += put_utf8(s + o, rd16(p + 2 * i));
    s[o] = 0;
    return s;
}

EvaCrf *eva_crf_load(const char *path, char *err, int errlen) {
    EvaCrf *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->file = eva_map_file(path, &m->size, &m->maplen);
    if (!m->file) { snprintf(err, errlen, "cannot read %s", path); free(m); return NULL; }
    const uint8_t *d = m->file, *end = d + m->size;
    /* FUN_180033c80: 0x18-byte container header (magic, GUID, size), u16 LCID, u16, "CRF\0", u32 version,
     * then 8 u32: strings off/size, nLabels, nTemplates, trie size, weight size, transition size, cost factor */
    if (m->size < 0x60 || memcmp(d + 0x1c, "CRF", 3)) { eva_crf_free(m); snprintf(err, errlen, "%s: not a CRF model", path); return NULL; }
    long p = 0x24;
    uint32_t stroff = rd32(d + p), strsize = rd32(d + p + 4);
    m->nlabel = (int)rd32(d + p + 8); m->ntempl = (int)rd32(d + p + 12);
    uint32_t triesz = rd32(d + p + 16), wsz = rd32(d + p + 20), xsz = rd32(d + p + 24);
    memcpy(&m->costf, d + p + 28, 4);
    p += 32;
    if ((long)stroff + (long)strsize > m->size || m->nlabel < 1 || m->nlabel > 64 || m->ntempl < 1 || m->ntempl > 256) { eva_crf_free(m); snprintf(err, errlen, "bad CRF header"); return NULL; }
    m->label = calloc((size_t)m->nlabel, sizeof(char *));
    m->templ = calloc((size_t)m->ntempl, sizeof(char *));
    for (int i = 0; i < m->nlabel; i++, p += 4) m->label[i] = wdup(d + stroff + rd32(d + p), end);
    for (int i = 0; i < m->ntempl; i++, p += 4) m->templ[i] = wdup(d + stroff + rd32(d + p), end);
    trie_init(&m->trie, d, p);
    m->nfeat = (int)(wsz / 4 / (uint32_t)m->nlabel);
    m->acl_boundary = !memcmp(d, "ACL", 3);
    /* RegularText suffix classes: the "U01:-xxx" entries (the only feature strings starting that way) */
    {
        static const char *const SUF[] = {"able", "age", "ance", "ancy", "ant", "cal", "crat", "dom", "ed", "ee",
            "eer", "en", "ence", "ency", "ent", "ese", "ess", "est", "ful", "hood", "ify", "ile", "ine", "ing", "ise",
            "ish", "ism", "ist", "ive", "ize", "less", "let", "like", "lity", "logy", "ment", "ness", "ous", "rian",
            "ship", "some", "stic", "tion", "ure"};
        for (size_t i = 0; i < sizeof SUF / sizeof *SUF; i++) {
            char key[32]; snprintf(key, sizeof key, "U01:-%s", SUF[i]);
            if (eva_crf_lookup(m, key) >= 0) {
                m->suf = realloc(m->suf, sizeof(char *) * (size_t)(m->nsuf + 1));
                m->suf[m->nsuf++] = cv_strdup(SUF[i]);
            }
        }
    }
    p += triesz;
    m->w = (const float *)(d + p); p += wsz;
    m->x = (const float *)(d + p); p += xsz;
    if ((long)wsz != 4L * m->nfeat * m->nlabel || (long)xsz != 4L * m->nlabel * m->nlabel || p > m->size
        || (uint32_t)(m->nlabel * m->nlabel) > 4096) {
        snprintf(err, errlen, "%s: %d features x %d labels does not match weight size %u", path, m->nfeat, m->nlabel, wsz);
        eva_crf_free(m); return NULL;
    }
    return m;
}

void eva_crf_free(EvaCrf *m) {
    if (!m) return;
    for (int i = 0; m->label && i < m->nlabel; i++) free(m->label[i]);
    for (int i = 0; m->templ && i < m->ntempl; i++) free(m->templ[i]);
    for (int i = 0; i < m->nsuf; i++) free(m->suf[i]);
    free(m->suf);
    free(m->label); free(m->templ); eva_unmap_file(m->file, m->maplen); free(m);
}
int eva_crf_nlabels(const EvaCrf *m) { return m->nlabel; }
const char *eva_crf_label(const EvaCrf *m, int i) { return m->label[i]; }
int eva_crf_ntemplates(const EvaCrf *m) { return m->ntempl; }
const char *eva_crf_template(const EvaCrf *m, int i) { return m->templ[i]; }
int eva_crf_nfeatures(const EvaCrf *m) { return m->nfeat; }
int eva_crf_lookup(const EvaCrf *m, const char *s) {
    uint16_t w[1024]; int n = 0;
    const unsigned char *u = (const unsigned char *)s;
    while (*u && n < 1023) {   /* UTF-8 -> UTF-16 (feature strings are ASCII apart from rare word text) */
        unsigned c = *u++;
        if (c >= 0xe0 && u[0] && u[1]) { c = (c & 15) << 12 | (u[0] & 63u) << 6 | (u[1] & 63u); u += 2; }
        else if (c >= 0xc0 && u[0]) { c = (c & 31) << 6 | (u[0] & 63u); u += 1; }
        w[n++] = (uint16_t)c;
    }
    long r = trie_rank(&m->trie, w, n);
    return r >= 0 && r < m->nfeat ? (int)r : -1;
}

long eva_crf_selfcheck(const EvaCrf *m, long *count) {
    Dump *D = calloc(1, sizeof *D);
    long bad;
    D->t = &m->trie;
    tdump(D, m->trie.ROOT, 0);
    *count = D->count; bad = D->bad;
    free(D);
    return bad;
}

/* Viterbi [G on tie-breaking and summation order; CRF++ semantics: node = costf * sum of unigram weights,
 * edge = transition x[prev][cur]] */
void eva_crf_decode(const EvaCrf *m, const int *feats, int T, int *labels) {
    int L = m->nlabel, K = m->ntempl;
    if (T <= 0) return;
    float *node = calloc((size_t)T * L, sizeof(float)), *best = calloc((size_t)T * L, sizeof(float));
    int *bp = calloc((size_t)T * L, sizeof(int));
    for (int t = 0; t < T; t++)
        for (int y = 0; y < L; y++) {
            float s = 0.0f;
            for (int k = 0; k < K; k++) { int f = feats[t * K + k]; if (f >= 0) s = s + m->w[(size_t)f * L + y]; }
            node[t * L + y] = m->costf * s;
        }
    for (int y = 0; y < L; y++) best[y] = node[y];
    for (int t = 1; t < T; t++)
        for (int y = 0; y < L; y++) {
            int arg = 0; float bv = 0.0f;
            for (int q = 0; q < L; q++) {
                float v = best[(t - 1) * L + q] + m->x[q * L + y];
                if (q == 0 || v > bv) { bv = v; arg = q; }
            }
            best[t * L + y] = bv + node[t * L + y];
            bp[t * L + y] = arg;
        }
    int y = 0;
    for (int q = 1; q < L; q++) if (best[(T - 1) * L + q] > best[(T - 1) * L + y]) y = q;
    for (int t = T - 1; t >= 0; t--) { labels[t] = y; if (t) y = bp[t * L + y]; }
    free(node); free(best); free(bp);
}

/* ------------------------------------------------------------------ features on the zf2 sentence */
typedef struct { const Z2Sent *s; int *tok; int ntok; int *tokof; const EvaCrf *m; } Seq;   /* tokof[word] = token index or -1 */

static int is_tok(const Z2W *w) { return z2_is_rw(w); }   /* FUN_18001c490 + FUN_180030418 [S] */
static int is_word04(const Z2W *w) { return w->type == 0 || w->type == 4; }

static void lower_utf8(const zf_char *t, char *o, int cap, int up) {
    int n = 0;
    for (; t && *t && n + 4 < cap; t++) {
        unsigned c = *t;
        if (!up && c >= 'A' && c <= 'Z') c += 32;
        if (up && c >= 'a' && c <= 'z') c -= 32;
        n += put_utf8(o + n, c);
    }
    o[n] = 0;
}

/* navigation result */
typedef struct { int level; int i; int oob; } Pos;   /* level 0 word, 1 phrase, 2 IP, 3 sentence; oob = _B offset */
enum { LV_WORD, LV_PHRASE, LV_IP, LV_SENT };

static int ph_is_break(const Z2Sent *s, int i) { return z2_phrase_is_break(s, i); }
static int rw_between(const Z2Sent *s, int a, int b) { int n = 0; for (int k = a; k <= b; k++) if (z2_is_rw(&s->w[k])) n++; return n; }

/* engine word navigation skips words failing FUN_18001a070 (punctuation, breaks, empty words) [S via zf2] */
static int word_move(const Z2Sent *s, int i, int dir) {
    for (i += dir; i >= 0 && i < s->nw; i += dir) if (!z2_word_skip(&s->w[i])) return i;
    return -1;
}
static int phrase_move(const Z2Sent *s, int i, int dir) {
    for (i += dir; i >= 0 && i < s->nph; i += dir) if (!ph_is_break(s, i)) return i;
    return -1;
}

static int ip_of_phrase(const Z2Sent *s, int ph) { return ph >= 0 ? s->ph[ph].ip : -1; }

/* property -> string; returns 0 ok, -1 null */
static int word_prop(const Seq *q, int wi, const char *prop, char *out, int cap) {
    const Z2Sent *s = q->s; const Z2W *w = &s->w[wi];
    if (!strcmp(prop, "RegularText") || !strcmp(prop, "Text")) {
        /* Text = word+0x98, RegularText = word+0xb8 [S]; how the frontend fills +0xb8 is not traced:
         * lower-case text [G] (the model's vocabulary is lower case; Word.Text values keep capitals) */
        lower_utf8(w->text, out, cap, 0);
        if (prop[0] == 'R' && q->m && q->m->nsuf) {
            /* [G] words outside the model's word list back off to a suffix class ("-tion"), as the vocabulary
             * suggests; the frontend code that fills word+0xb8 has not been traced */
            char key[600]; snprintf(key, sizeof key, "U01:%s", out);
            if (eva_crf_lookup(q->m, key) < 0) {
                size_t n = strlen(out); int best = -1; size_t bl = 0;
                for (int k = 0; k < q->m->nsuf; k++) {
                    size_t l = strlen(q->m->suf[k]);
                    if (l < n && l > bl && !strcmp(out + n - l, q->m->suf[k])) { best = k; bl = l; }
                }
                if (best >= 0) snprintf(out, cap, "-%s", q->m->suf[best]);
            }
        }
        if (!strcmp(prop, "Text") && w->text) { int n = 0; for (const zf_char *t = w->text; *t && n + 4 < cap; t++) n += put_utf8(out + n, *t); out[n] = 0; }
        return out[0] ? 0 : -1;
    }
    if (!strcmp(prop, "CaseInsensitiveText")) { lower_utf8(w->text, out, cap, 1); return out[0] ? 0 : -1; }
    if (!strcmp(prop, "POS")) { snprintf(out, cap, "%s", zf1_pos_name(w->pos)); return 0; }
    if (!strcmp(prop, "POSTaggerPOS")) { snprintf(out, cap, "%s", zf1_pos_name(w->tpos)); return 0; }
    int v = 0;
    if (!strcmp(prop, "BoundedSyllableNumber")) v = w->ns < 5 ? w->ns : 5;                  /* FUN_180151cf0 */
    else if (!strcmp(prop, "BreakIndex")) v = w->bi;
    else if (!strcmp(prop, "SyllableNumber")) v = w->ns;
    else if (!strcmp(prop, "NumOfWordsToPunc") || !strcmp(prop, "BoundedNumOfWordsToPunc")) {   /* FUN_180153fa0 / FUN_1801520f0 */
        int cap8 = prop[0] == 'B';
        for (int k = wi + 1; k < s->nw && s->w[k].type != 1; k++) {
            if (is_word04(&s->w[k])) v++;
            if (cap8 && v > 8) { v = 8; break; }
        }
    } else if (!strcmp(prop, "NumOfWordsFromPunc") || !strcmp(prop, "BoundedNumOfWordsFromPunc")) {  /* FUN_180153ef0 / FUN_180151ee0 */
        int cap8 = prop[0] == 'B';
        for (int k = wi; k >= 0 && s->w[k].type != 1; k--) {
            if (is_word04(&s->w[k])) v++;
            if (cap8 && v > 8) { v = 8; break; }
        }
    } else if (!strcmp(prop, "BoundedNumOfSyllablesToPunc")) {   /* FUN_180151e10 */
        for (int k = wi + 1; k < s->nw && s->w[k].type != 1; k++) {
            if (is_word04(&s->w[k])) v += s->w[k].ns;
            if (v > 15) { v = 15; break; }
        }
    } else if (!strcmp(prop, "BoundedNumOfSyllablesFromPunc")) { /* FUN_180151d40 */
        for (int k = wi; k >= 0 && s->w[k].type != 1; k--) {
            if (is_word04(&s->w[k])) v += s->w[k].ns;
            if (v > 15) { v = 15; break; }
        }
    } else if (!strcmp(prop, "BoundedNumOfWordsToVerb") || !strcmp(prop, "BoundedNumOfWordsFromVerb")) {
        /* FUN_1801521b0 / FUN_180151fa0: POS class 2 (verbs) or 6 (auxiliaries) stops; runs off the end -> 8 */
        int to = prop[16] == 'T', k = to ? wi + 1 : wi, found = 0;
        v = 8;
        int cnt = 0;
        for (; k >= 0 && k < s->nw; k += to ? 1 : -1) {
            int c = zf1_pos_class(s->w[k].pos);
            if (c == 2 || c == 6) { found = 1; break; }
            if (is_word04(&s->w[k])) cnt++;
            if (cnt > 8) break;
        }
        if (found) v = cnt;
    } else if (!strcmp(prop, "IsFollowedByPunc")) {             /* FUN_180152c70 */
        int k = wi + 1;
        for (; k < s->nw; k++) {
            const Z2W *x = &s->w[k];
            if (!z2_is_break(x) && (x->type == 1 || z2_is_rw(x))) break;
        }
        v = (k < s->nw && s->w[k].type != 1) ? 0 : 1;
    } else if (!strcmp(prop, "IsLastContentWordInPhrase")) {    /* FUN_180152ef0: POS classes 1,2,3,7,9 */
        if (w->phrase < 0) return -1;
        for (int k = s->ph[w->phrase].w1; k >= s->ph[w->phrase].w0; k--) {
            int c = zf1_pos_class(s->w[k].pos);
            if (c < 10 && ((0x28e >> c) & 1)) { v = k == wi; break; }
        }
    } else if (!strcmp(prop, "FwPosInIntonationPhrase") || !strcmp(prop, "BwPosInIntonationPhrase")) {
        int ip = ip_of_phrase(s, w->phrase);
        if (ip < 0) return -1;
        int a = s->ph[s->ip[ip].ph0].w0, b = s->ph[s->ip[ip].ph1].w1;
        v = prop[0] == 'F' ? rw_between(s, a, wi) : rw_between(s, wi, b);
    } else return -1;
    snprintf(out, cap, "%d", v);
    return 0;
}

static int phrase_prop(const Z2Sent *s, int pi, const char *prop, char *out, int cap) {
    const Z2PH *p = &s->ph[pi];
    int v = 0;
    if (!strcmp(prop, "WordNumber")) v = rw_between(s, p->w0, p->w1);
    else if (!strcmp(prop, "SyllableNumber")) { for (int k = p->w0; k <= p->w1; k++) if (z2_is_rw(&s->w[k])) v += s->w[k].ns; }
    else if (!strcmp(prop, "ToBIFinalBoundaryTone")) v = z2_is_rw(&s->w[p->w1]) ? s->w[p->w1].tone : 0;
    else if (!strcmp(prop, "FwPosInIntonationPhrase") || !strcmp(prop, "BwPosInIntonationPhrase")) {
        int ip = p->ip; if (ip < 0) return -1;
        int a = s->ip[ip].ph0, b = s->ip[ip].ph1;
        if (prop[0] == 'F') { for (int k = a; k <= pi; k++) if (!ph_is_break(s, k)) v++; }
        else { for (int k = pi; k <= b; k++) if (!ph_is_break(s, k)) v++; }
    } else return -1;
    snprintf(out, cap, "%d", v);
    return 0;
}

/* evaluate one attribute path ("Word.PrevWord.POS.SmallerThan(2)") for token word wi */
static void eval_attr(const Seq *q, int wi, const char *path, char *out, int cap) {
    const Z2Sent *s = q->s;
    char tok[64][256]; int nt = 0;
    /* split on '.' outside parentheses/quotes */
    { const char *a = path; int depth = 0, inq = 0, n = 0;
      for (; *a && nt < 64; a++) {
          if (*a == '"') inq = !inq;
          if (!inq && *a == '(') depth++;
          if (!inq && *a == ')') depth--;
          if (*a == '.' && !depth && !inq) { tok[nt][n] = 0; nt++; n = 0; continue; }
          if (n < 255) tok[nt][n++] = *a;
      }
      tok[nt][n] = 0; nt++; }
    Pos x = {LV_WORD, wi, 0};
    int k = 1;                    /* tok[0] == "Word" */
    for (; k < nt; k++) {
        const char *t = tok[k];
        if (!strcmp(t, "PrevWord") || !strcmp(t, "NextWord")) {
            int dir = t[0] == 'P' ? -1 : 1;
            if (x.oob) { x.oob += dir; continue; }
            int j = word_move(s, x.i, dir);
            if (j < 0) x.oob = dir; else x.i = j;
        } else if (!strcmp(t, "Phrase")) {
            if (x.oob) break;
            x.level = LV_PHRASE; x.i = s->w[x.i].phrase; if (x.i < 0) { snprintf(out, cap, "-1"); return; }
        } else if (!strcmp(t, "PrevPhrase") || !strcmp(t, "NextPhrase")) {
            int dir = t[0] == 'P' ? -1 : 1;
            if (x.oob) { x.oob += dir; continue; }
            int j = phrase_move(s, x.i, dir);
            if (j < 0) x.oob = dir; else x.i = j;
        } else if (!strcmp(t, "IntonationPhrase")) {
            if (!x.oob) { x.level = LV_IP; x.i = ip_of_phrase(s, x.i); }
        } else if (!strcmp(t, "Sentence")) {
            x.level = LV_SENT; x.oob = 0;
        } else break;             /* property */
    }
    if (x.oob && x.level != LV_SENT) {
        int hasfn = 0;
        for (int j = k + 1; j < nt; j++) if (strchr(tok[j], '(')) hasfn = 1;
        if (hasfn) { snprintf(out, cap, "-1"); return; }                    /* vocabulary: "0/-1/-1" */
        if (q->m && q->m->acl_boundary) { snprintf(out, cap, "Boundary"); return; }
        /* FUN_180004ea0: "_B-2".."_B+2" (table 0x180074020) */
        int o = x.oob < -2 ? -2 : x.oob > 2 ? 2 : x.oob;
        snprintf(out, cap, "_B%s%d", o < 0 ? "-" : "+", o < 0 ? -o : o);
        return;
    }
    if (k >= nt) { out[0] = 0; return; }
    const char *prop = tok[k];
    char val[512]; int ok;
    if (x.level == LV_SENT) {
        if (!strcmp(prop, "SentenceType")) { snprintf(val, sizeof val, "%d", s->type); ok = 0; } else ok = -1;
    } else if (x.level == LV_PHRASE) ok = phrase_prop(s, x.i, prop, val, sizeof val);
    else if (x.level == LV_WORD) ok = word_prop(q, x.i, prop, val, sizeof val);
    else ok = -1;
    /* post-functions */
    for (k++; k < nt; k++) {
        const char *f = tok[k];
        if (!strncmp(f, "SmallerThan(", 12)) {
            if (ok) { snprintf(val, sizeof val, "-1"); ok = 0; continue; }
            snprintf(val, sizeof val, "%d", atoi(val) < atoi(f + 12) ? 1 : 0);
        } else if (!strncmp(f, "IsInList(", 9) || !strncmp(f, "EndWith(", 8)) {
            if (ok) { snprintf(val, sizeof val, "-1"); ok = 0; continue; }
            int hit = 0; const char *a = strchr(f, '(') + 1;
            while (*a) {
                const char *b = strchr(a, '"'); if (!b) break;
                const char *e = strchr(b + 1, '"'); if (!e) break;
                size_t n = (size_t)(e - b - 1), vl = strlen(val);
                if (f[0] == 'I') { if (n == vl && !strncmp(val, b + 1, n)) hit = 1; }
                else if (vl >= n && !strncmp(val + vl - n, b + 1, n)) hit = 1;
                a = e + 1;
            }
            snprintf(val, sizeof val, "%d", hit);
        } else if (!strncmp(f, "Equal(", 6)) {
            /* [G] the TON vocabulary for "NextWord.POSTaggerPOS.Equal(32)" holds POS names, so the value passes through */
        }
    }
    if (ok) { out[0] = 0; return; }
    snprintf(out, cap, "%s", val);
}

static int *build_feats(const EvaCrf *m, const Seq *q) {
    int K = m->ntempl;
    int *f = malloc(sizeof(int) * (size_t)(q->ntok ? q->ntok : 1) * (size_t)K);
    char key[4096], part[1024];
    for (int t = 0; t < q->ntok; t++)
        for (int k = 0; k < K; k++) {
            const char *tp = m->templ[k];
            f[t * K + k] = -1;
            if (tp[0] != 'U') continue;                  /* "B" = the transition matrix */
            const char *colon = strchr(tp, ':');
            if (!colon) continue;
            int n = (int)(colon - tp) + 1;
            memcpy(key, tp, (size_t)n); key[n] = 0;
            /* split the attribute list on '/' outside parentheses and quotes */
            const char *a = colon + 1; int depth = 0, inq = 0, pl = 0, empty = 0, first = 1;
            char attr[1024];
            for (;; a++) {
                if (*a == '"') inq = !inq;
                if (!inq && *a == '(') depth++;
                if (!inq && *a == ')') depth--;
                if (!*a || (*a == '/' && !depth && !inq)) {
                    attr[pl] = 0;
                    eval_attr(q, q->tok[t], attr, part, sizeof part);
                    if (!part[0]) empty = 1;
                    if (!first) strcat(key, "/");
                    strncat(key, part, sizeof key - strlen(key) - 1);
                    first = 0; pl = 0;
                    if (!*a) break;
                    continue;
                }
                if (pl < 1023) attr[pl++] = *a;
            }
            if (!empty) f[t * K + k] = eva_crf_lookup(m, key);
        }
    return f;
}

static void seq_init(Seq *q, const Z2Sent *s) {
    q->s = s; q->ntok = 0;
    q->tok = malloc(sizeof(int) * (size_t)(s->nw + 1));
    q->tokof = malloc(sizeof(int) * (size_t)(s->nw + 1));
    for (int i = 0; i < s->nw; i++) { q->tokof[i] = -1; if (is_tok(&s->w[i])) { q->tokof[i] = q->ntok; q->tok[q->ntok++] = i; } }
}
static void seq_free(Seq *q) { free(q->tok); free(q->tokof); }

static void word_name(const Z2W *w, char *o, int cap) { int n = 0; for (const zf_char *t = w->text; t && *t && n + 4 < cap; t++) n += put_utf8(o + n, *t); o[n] = 0; }

/* accent setter = word vt+0x138 as the rules use it (zf2_prosody.c rule_accents) */
static void set_accent(Z2Sent *s, int i, int v, int force) {
    Z2W *w = &s->w[i];
    if (w->i230 == 0 || force) {
        w->i22c = v;
        for (int k = 0; k < w->ns; k++) s->s[w->s0 + k].tobi = 0;
        z2_set_tobi_accent(s, i);
    }
    w->i230 = 1;
}

#ifdef CV_NO_DEBUG_ENV
#define VERB(P) 0      /* the shipped extension never prints spoken words */
#else
#define VERB(P) ((P)->verbose)
#endif
static void dump_words(const Z2Sent *s, const char *tag) {

    char nm[256];
    fprintf(stderr, "%s:", tag);
    for (int i = 0; i < s->nw; i++) {
        const Z2W *w = &s->w[i];
        if (!z2_is_rw(w)) continue;
        word_name(w, nm, sizeof nm);
        fprintf(stderr, " %s[BI%d%s%s]", nm, w->bi, w->tone ? (w->tone == 1000 ? " T1000" : w->tone == 1001 ? " T1001" : w->tone == 1002 ? " T1002" : w->tone == 1003 ? " T1003" : w->tone == 1004 ? " T1004" : w->tone == 1005 ? " T1005" : " T?") : "",
                w->i22c ? (w->i22c == 1 ? " A1" : w->i22c == 5 ? " A5" : w->i22c == 6 ? " A6" : " A?") : "");
    }
    fprintf(stderr, "\n");
}

void eva_prosody_hook(void *ctx, void *z2, int stage) {
    EvaProsody *P = (EvaProsody *)ctx;
    Z2Sent *s = (Z2Sent *)z2;
    EvaCrf *m = P->off ? NULL : stage == 1 ? P->br2 : stage == 2 ? P->ton : P->acl;
    if (!m) {
        if (stage == 3 && VERB(P)) dump_words(s, "rules ");
        return;
    }
    if (stage >= 2) z2_build_phrases(s, 1);   /* break predictor post step: loc vt+0x30 phrase build [S]; the p2=1 form
                                                 (punctuation outside phrases) matches the vocabulary [G] */
    if (stage == 2)   /* [G] words ending a phrase carry the neutral tone 1000 before TON runs (the ACL vocabulary never
                       * sees a phrase tone below 1000); the site that sets it is not traced */
        for (int i = 0; i < s->nw; i++) if (z2_is_rw(&s->w[i]) && s->w[i].bi > 2 && s->w[i].tone == 0 && !s->w[i].tone_lock) s->w[i].tone = 1000;
    Seq q; seq_init(&q, s); q.m = m;
    if (!q.ntok) { seq_free(&q); return; }
    int *feats = build_feats(m, &q), *lab = malloc(sizeof(int) * (size_t)q.ntok);
    eva_crf_decode(m, feats, q.ntok, lab);
    char nm[256];
    for (int t = 0; t < q.ntok; t++) {
        int i = q.tok[t]; Z2W *w = &s->w[i];
        if (stage == 1) {
            /* CCRFBreakTaggerImpl FUN_18002fe90: label "#n" -> BI n+1; only unlocked words whose BI is 2 or 3;
             * BI 3 also sets pause class 1 (IntermPhrase), BI 4 would set 3 [S] */
            int bi = atoi(m->label[lab[t]] + 1) + 1;
            if (!w->bi_lock && w->bi > 1 && w->bi < 4) {
                w->bi = bi;
                if (bi == 3) w->pause = Z2_PC_INTERM;
                else if (bi == 4) w->pause = Z2_PC_NONPUNCIP;
            }
        } else if (stage == 2) {
            /* CCRFBoundaryToneTaggerImpl FUN_18005c2b0: only words with BI > 2 whose tone is 1000 or 1002;
             * label index -> {0, 1000, ..., 1006} (0x180088968) [S] */
            static const int code[8] = {0, 1000, 1001, 1002, 1003, 1004, 1005, 1006};
            if (w->bi > 2 && (w->tone == 1000 || w->tone == 0x3ea) && lab[t] < 8 && !w->tone_lock) w->tone = code[lab[t]];
        }
        if (VERB(P)) { word_name(w, nm, sizeof nm); fprintf(stderr, "  %s %-14s %s\n", stage == 1 ? "BR2" : stage == 2 ? "TON" : "ACL", nm, m->label[lab[t]]); }
    }
    if (stage == 3) {
        /* CCRFPitchAccentLocTaggerImpl FUN_180067360: accented words get 1 or 5 (first accent of the first phrase:
         * 5, or 1 in sentence type 2) / 1 (first of a later phrase) / 6 (others); then FUN_180067590 turns every
         * 5 after the first into 1 [S] */
        int first = 1, firstphr = 1, nphr = 0;
        for (int t = 0; t < q.ntok; t++) {
            int i = q.tok[t];
            if (lab[t] != 0) {
                int v = 6;
                if (first) {
                    if (firstphr && nphr == 0) { v = s->type == 2 ? 1 : 5; firstphr = 0; }
                    else v = 1;
                    first = 0;
                }
                set_accent(s, i, v, 0);
            }
            if (s->w[i].bi > 3) { nphr++; first = 1; }
        }
        int seen5 = 0;
        for (int i = 0; i < s->nw; i++)
            if (s->w[i].i22c == 5) { if (!seen5) seen5 = 1; else set_accent(s, i, 1, 1); }
    }
    if (stage == 3 && VERB(P)) dump_words(s, "models");
    free(feats); free(lab); seq_free(&q);
}
