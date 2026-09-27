/* zf1_ana.c - CSentenceEnumerator + CSentenceAnalyzer core.  See zf1_ana.h and notes/fe1.md.  Portable C99. */
#include "zf1_ana.h"
#include <stdlib.h>
#include <string.h>

/* ClassicVoices patch: function-level scratch is per thread (two voices may speak on two threads at once). */
#ifndef ZF_TLS
#define ZF_TLS __thread
#endif

int zf1_tok_add(zf1_toklist *l, int off, int len, int entity, const zf_char *cat, uint16_t pos)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 64;
        zf1_tok *t = (zf1_tok *)realloc(l->t, sizeof(zf1_tok) * (size_t)nc);
        if (!t) return -1;
        l->t = t;
        l->cap = nc;
    }
    l->t[l->n].off = off;
    l->t[l->n].len = len;
    l->t[l->n].entity = entity;
    l->t[l->n].cat = cat;
    l->t[l->n].pos = pos;
    l->n++;
    return 0;
}

/* ================= CSentenceEnumerator (FUN_1800624e0) ================= */
static int is_text_action(int a) { return a < 10 && ((0x211u >> a) & 1); }   /* 0 speak, 4 spell, 9 */

static int node_ok(const zf1_frags *fr, int i, int group) { return i >= 0 && i < fr->nnd && fr->nd[i].group == group; }

void zf1_enum_start(zf1_enum *en, const zf1_frags *fr, int group)
{
    int i;
    en->group = group;
    en->node = -1;
    en->pos = 0;
    for (i = 0; i < fr->nnd; i++)
        if (fr->nd[i].group == group) { en->node = i; break; }
}

/* FUN_1800b682c: say-as categories that are not sentence-split */
static int sayas_splits(const zf_char *c)
{
    if (!c || !*c) return 1;
    return zf_strcmp_a(c, "address") != 0 && zf_strcmp_a(c, "name") != 0;
}

int zf1_enum_next(zf1_enum *en, const zf1_frags *fr, zf1_span *sp)
{
    int node = en->node, pos = en->pos, end, total = 0, done = 0, exhausted = 0;
    int s_node, s_pos;
    if (!node_ok(fr, node, en->group)) return 0;
    end = fr->nd[node].len;
    s_node = node;
    s_pos = pos;
    for (;;) {
        const zf1_node *nd;
        if (node < 0 || done) {
            en->node = node;
            en->pos = pos;
            sp->n0 = s_node;
            sp->p0 = s_pos;
            sp->n1 = node;
            sp->p1 = pos;
            return 1;
        }
        nd = &fr->nd[node];
        if (is_text_action(nd->action)) {
            if (pos < end) {
                int rem = end - pos, cons = rem, hard = 0, t2, before = pos;
                if (!nd->sayas || sayas_splits(nd->sayas) || rem > 499) {
                    if (en->sentsep) en->sentsep(en->ssctx, nd->text + pos, rem, &cons, &hard);
                    else { cons = rem; hard = 0; }
                }
                t2 = total + cons;
                if (hard || rem > 499) done = 1;
                pos += cons;
                if (t2 > 500) { t2 -= cons; done = 1; pos = before; }
                total = t2;
            }
            if (pos >= end) exhausted = 1;
            if (!done && exhausted && total != 0) {
                int nx = node + 1;
                if (node_ok(fr, nx, en->group) && (fr->nd[nx].action == 6 || fr->nd[nx].action == 7)) done = 1;
            }
            if (!exhausted) continue;
        }
        /* LAB_180062707: move on */
        exhausted = 0;
        if (done) {
            int nx = node + 1;
            if (node_ok(fr, nx, en->group)) {
                if (fr->nd[nx].action == 6 || fr->nd[nx].action == 7) node = nx;
                continue;
            }
            node = -1;
        } else {
            node = node + 1;
            if (node_ok(fr, node, en->group)) {
                pos = 0;
                end = fr->nd[node].len;
                continue;
            }
            node = -1;
        }
        pos = 0;
        end = 0;
    }
}

/* ================= CSentenceAnalyzer ================= */

/* FUN_18000be20 (ENU locale TA vt+0x20): fullwidth digits/letters -> ASCII, NBSP and some U+20xx blanks -> ' ' */
static void lta_normalize(zf_char *p, int n)
{
    int i;
    for (i = 0; i < n && p[i]; i++) {
        zf_char c = p[i];
        if ((zf_char)(c + 0xf0) < 10 || (zf_char)(c + 0xdf) < 0x1a || (c >= 0xff41 && c < 0xff5b)) {
            p[i] = (zf_char)(c + 0x120);
        } else if (c < 0xff41) {
            if (c == 0xa0 || ((zf_char)(c + 0xdffe) < 0x2e && ((0x2000000003e3ull >> (zf_char)(c + 0xdffe)) & 1)))
                p[i] = 0x20;
        }
    }
}

/* FUN_180062254: copy the sentence text pieces into the analyzer buffer (max 500) */
static int build_sbuf(zf1_ana *a)
{
    const zf1_frags *fr = a->fr;
    int i = a->sp.n0, first = 1;
    a->slen = 0;
    while (i >= 0 && i < fr->nnd) {
        const zf1_node *nd = &fr->nd[i];
        int from = first ? a->sp.p0 : 0;
        int to = (i == a->sp.n1) ? a->sp.p1 : nd->len;
        first = 0;
        if (is_text_action(nd->action) && nd->text) {
            int k;
            if (to - from > 500 - a->slen) return -1;
            for (k = from; k < to; k++) {
                a->sbuf[a->slen] = nd->text[k];
                a->mapn[a->slen] = i;
                a->mapi[a->slen] = k;
                a->slen++;
            }
        }
        if (i == a->sp.n1) break;
        i++;
        if (i >= fr->nnd || fr->nd[i].group != fr->nd[a->sp.n0].group) break;
    }
    a->sbuf[a->slen] = 0;
    a->mapn[a->slen] = a->slen ? a->mapn[a->slen - 1] : a->sp.n0;
    a->mapi[a->slen] = a->slen ? a->mapi[a->slen - 1] + 1 : 0;
    lta_normalize(a->sbuf, a->slen);
    return 0;
}

/* FUN_1800780e8: buffer offset -> source offset (node.src_off + index in node text) and node */
static void map_src(const zf1_ana *a, int off, int *src, int *node)
{
    int n, i;
    if (off < 0) off = 0;
    if (off > a->slen) off = a->slen;
    n = a->mapn[off];
    i = a->mapi[off];
    if (node) *node = n;
    if (src) *src = a->fr->nd[n].src_off + i;
}

static void fill_fs(zf_fragstate *fs, const zf1_node *nd)
{
    memset(fs, 0, sizeof *fs);
    fs->action = nd->action;
    fs->lang = nd->lang;
    fs->emph = nd->emph;
    fs->silence_ms = nd->silence_ms;
    fs->has_prosody = nd->has_prosody;
    fs->pitch_middle = nd->pitch;
    fs->rate = nd->rate;
    fs->volume = nd->vol;
    fs->pos = nd->pos;
    fs->sayas = nd->sayas;
    fs->src_off = nd->src_off;
    fs->src_len = nd->len;
}

/* towupper in the "C" locale (UCRT): ASCII only */
static zf_char c_towupper(zf_char c) { return (c >= 'a' && c <= 'z') ? (zf_char)(c - 32) : c; }

/* FUN_18006e874: create a word and append it to the analyzer list */
static zf_word *new_word(zf1_ana *a, int type, int node, const zf_char *text, int len, int src_off, int src_len)
{
    zf_word *w = zf_wl_push(&a->wl);
    const zf1_node *nd = &a->fr->nd[node];
    int k;
    if (!w) return NULL;
    fill_fs(&w->fs, nd);
    w->lang = 0x409;
    w->src_off = src_off;
    w->src_len = src_len;
    w->type = type;
    /* vt+0x70: Text and CaseInsensitiveText (towupper) */
    if (text && len > 0) {
        w->text = zf_strndup(text, (size_t)len);
        w->ci = zf_strndup(text, (size_t)len);
        for (k = 0; k < len; k++) w->ci[k] = c_towupper(w->ci[k]);
    }
    if (a->bm_pending) {        /* pending bookmark attaches to the next word (+0x2f0/+0x2f8/+0x318) */
        w->i2f0 = a->bm_pending;
        zf_setstr(&w->s2f8, a->bm_name);
        a->bm_pending = 0;
        free(a->bm_name);
        a->bm_name = NULL;
    }
    return w;
}

/* FUN_18004d0d4 */
static int has_digit(const zf_char *p, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        zf_char c = p[i];
        if ((c >= '0' && c <= '9') || (zf_char)(c + 0xdf8c) < 0x16 || c == 0xb9 || c == 0xb2 || c == 0xb3 || c == 0x2070)
            return 1;
        /* iswdigit also accepts the other Unicode decimal digit ranges (C1_DIGIT) */
        if ((c >= 0x660 && c <= 0x669) || (c >= 0x6f0 && c <= 0x6f9) || (c >= 0x966 && c <= 0x96f) || (c >= 0xff10 && c <= 0xff19))
            return 1;
    }
    return 0;
}

static const zf_char NONE_STR[] = {'n', 'o', 'n', 'e', 0};
static const zf_char SP_DEFAULT[] = {'s', 'p', ':', 'd', 'e', 'f', 'a', 'u', 'l', 't', 0};
static const zf_char SP_COMPOUND[] = {'s', 'p', ':', 'c', 'o', 'm', 'p', 'o', 'u', 'n', 'd', 'W', 'o', 'r', 'd', 0};

/* FUN_1800c4ab4: entity token -> word carrying the category */
static int entity_word(zf1_ana *a, int off, int len, const zf_char *cat, uint16_t pos)
{
    int src, node;
    zf_word *w;
    map_src(a, off, &src, &node);
    w = new_word(a, 0, node, a->sbuf + off, len, src, len);
    if (!w) return -1;
    if (zf_strcmp(cat, SP_COMPOUND) != 0) zf_setstr(&w->ne_type, cat);
    w->pos = pos;   /* vt+0xe8(pos, postable): +0x138 = pos, +0x13a = converted (set by the POS module) */
    zf_setstr(&w->s1b0, NONE_STR);
    return 0;
}

/* FUN_180062900 (analyzer vt2[0]): plain token -> word, quotes normalised in Text */
static int plain_word(zf1_ana *a, int off, int len)
{
    int src, node, k;
    zf_word *w;
    map_src(a, off, &src, &node);
    w = new_word(a, 0, node, a->sbuf + off, len, src, len);
    if (!w) return -1;
    for (k = 0; w->text && w->text[k]; k++) {
        zf_char c = w->text[k];
        if (c == 0x201c || c == 0x201d || c == 0x2ba || c == 0x2ee || c == 0x2033 || c == 0xff02) w->text[k] = 0x22;
        else if (c == 0x2b9 || c == 0x2bc || c == 0x2018 || c == 0x2019 || c == 0x2032 || c == 0xff07) w->text[k] = 0x27;
    }
    return 0;
}

/* FUN_180071b70: CTTSWordSink tokens -> words */
static int sink_to_words(zf1_ana *a, const zf1_toklist *tl)
{
    int i, r = 0;
    for (i = 0; i < tl->n && r >= 0; i++) {
        const zf1_tok *t = &tl->t[i];
        if (!t->entity) {
            /* POS = the POS table's default (vt+0x38): 50 "unknown" */
            if (has_digit(a->sbuf + t->off, t->len)) r = entity_word(a, t->off, t->len, SP_DEFAULT, 50);
            else r = plain_word(a, t->off, t->len);
        } else {
            r = entity_word(a, t->off, t->len, t->cat, t->pos);
        }
    }
    return r;
}

typedef struct { zf1_toklist *out; int base; } brk_ctx;
static int brk_emit(void *c, int off, int len)
{
    brk_ctx *b = (brk_ctx *)c;
    return zf1_tok_add(b->out, b->base + off, len, 0, NULL, 0xFFFF);
}

int zf1_ana_break(zf1_ana *a, int off, int len, zf1_toklist *out)
{
    brk_ctx b;
    b.out = out;
    b.base = off;
    return zf1_wb_break(a->wb, a->sbuf + off, len, 1, brk_emit, &b);
}

/* FUN_1800c4f10: plain text fragment piece */
static int text_piece(zf1_ana *a, int node, int off, int len)
{
    zf1_toklist tl;
    int r;
    if (len == 0) return 0;
    memset(&tl, 0, sizeof tl);
    if (a->hk && a->hk->text_tokens)
        r = a->hk->text_tokens(a->hk->ctx, a, &a->fr->nd[node], a->sbuf, off, len, &tl);
    else
        r = zf1_ana_break(a, off, len, &tl);
    if (r >= 0) r = sink_to_words(a, &tl);
    free(tl.t);
    return r;
}

/* ---------------- CCharTable ---------------- */
int zf1_ctab_init(zf1_ctab *ct, const zf1_dat *d)
{
    size_t n;
    const uint8_t *r = zf1_dat_get(d, 0xf6e4f50au, 0x80b9a5a3u, &n);
    memset(ct, 0, sizeof *ct);
    if (!r || n < 4) return -1;
    ct->n = (int)zf_rd32(r);
    ct->rec = r + 4;
    ct->pool = r + 4 + 16 * (size_t)ct->n;
    return 0;
}

int zf1_ctab_find(const zf1_ctab *ct, uint32_t cp, const uint8_t **name, const uint8_t **pron, uint32_t *flags)
{
    int lo = 0, hi = ct->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        const uint8_t *r = ct->rec + 16 * (size_t)mid;
        uint32_t v = zf_rd32(r);
        if (v == cp) {
            if (name) *name = ct->pool + zf_rd32(r + 8);
            if (pron) *pron = ct->pool + zf_rd32(r + 12);
            if (flags) *flags = zf_rd32(r + 4);
            return 1;
        }
        if (v < cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int u16len(const uint8_t *p) { int n = 0; while (p && zf_rd16(p + 2 * n)) n++; return n; }

static void set_pron_ids(zf1_ana *a, zf_word *w, const uint16_t *ph, int n)
{
    int k;
    w->nprons = 1;
    w->prons = (zf_char **)malloc(sizeof(zf_char *));
    w->prons[0] = (zf_char *)malloc(sizeof(zf_char) * ((size_t)n + 1));
    for (k = 0; k < n; k++) w->prons[0][k] = ph[k];
    w->prons[0][n] = 0;
    w->cur_pron = 0;
    free(w->pron);
    w->pron = zf1_phones_to_str(a->ps, ph, n);
}

static int is_hi16(zf_char c) { return (zf_char)(c + 0x2800) < 0x400; }
static int is_lo16(zf_char c) { return (zf_char)(c + 0x2400) < 0x400; }

/* FUN_1800c7cf0 -> FUN_1800c7914: spell out buffer text [off, off+len) letter by letter */
static int spell_piece(zf1_ana *a, int off, int len)
{
    int src0, node, i = 0, r = 0;
    if (len <= 0) return 0;
    map_src(a, off, &src0, &node);
    while (i < len && a->sbuf[off + i] && r >= 0) {
        const zf_char *p = a->sbuf + off + i;
        int rem = len - i, clen = 1, found = 0;
        uint32_t cp;
        const uint8_t *name = NULL, *pron = NULL;
        uint16_t ph[ZF1_LEX_MAXPH];
        int nph = 0;
        if (rem > 1 && is_hi16(p[0]) && is_lo16(p[1])) { clen = 2; cp = ((uint32_t)p[0] << 16) | p[1]; }
        else cp = p[0];
        if (a->ct && zf1_ctab_find(a->ct, cp, &name, &pron, NULL)) {
            found = 1;
            nph = u16len(pron);
            for (int k = 0; k < nph && k < ZF1_LEX_MAXPH; k++) ph[k] = zf_rd16(pron + 2 * k);
        } else if (a->lex) {
            /* main lexicon entry of the character itself (FUN_1800c8cd0: first pronunciation) */
            zf_char key[3];
            zf1_lexent ent;
            if (clen == 2) { key[0] = p[0]; key[1] = p[1]; } else key[0] = p[0];
            if (zf1_lex_lookup(a->lex, key, clen, &ent) && ent.n > 0 && ent.p[0].nph > 0) {
                found = 2;
                nph = ent.p[0].nph;
                memcpy(ph, ent.p[0].ph, sizeof(uint16_t) * (size_t)nph);
            }
        }
        if (found && nph > 0) {
            zf_word *w = new_word(a, 4, node, p, clen, src0 + i, clen);
            if (!w) return -1;
            set_pron_ids(a, w, ph, nph);
            w->i278 = 8;    /* pron source "spelled" set at creation */
        } else if (found == 1 && name && zf_rd16(name)) {
            /* the reading, one word per blank-separated part (text = the part) */
            int nl = u16len(name), s = 0;
            while (s < nl) {
                int e2;
                while (s < nl && zf_rd16(name + 2 * s) == ' ') s++;
                e2 = s;
                while (e2 < nl && zf_rd16(name + 2 * e2) != ' ') e2++;
                {
                    zf_word *w = new_word(a, 0, node, p, clen, src0 + i, clen);
                    int k;
                    if (!w) return -1;
                    free(w->text);
                    free(w->ci);
                    w->text = (zf_char *)malloc(sizeof(zf_char) * ((size_t)(e2 - s) + 1));
                    w->ci = (zf_char *)malloc(sizeof(zf_char) * ((size_t)(e2 - s) + 1));
                    for (k = 0; k < e2 - s; k++) {
                        w->text[k] = zf_rd16(name + 2 * (s + k));
                        w->ci[k] = c_towupper(w->text[k]);
                    }
                    w->text[e2 - s] = 0;
                    w->ci[e2 - s] = 0;
                }
                if (e2 >= nl) break;
                s = e2 + 1;
            }
        } else {
            zf_char c = p[0];
            int sp = (c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
                      (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
                      c == 0x3000);
            int ct = (c < 0x20 || (c >= 0x7f && c < 0xa0));
            if (!sp && !ct) {
                zf_word *w = new_word(a, 0, node, p, clen, src0 + i, clen);
                if (!w) return -1;
            }
        }
        i += clen;
    }
    return r;
}

/* FUN_1800c7838: <silence> */
static int silence_node(zf1_ana *a, int node)
{
    const zf1_node *nd = &a->fr->nd[node];
    zf_word *w;
    if (nd->silence_ms <= 0) return 0;
    w = new_word(a, 3, node, nd->text, nd->len, nd->src_off, nd->len);
    if (!w) return -1;
    w->silence_ms = nd->silence_ms;
    { static const uint16_t sil[1] = {3}; set_pron_ids(a, w, sil, 1); }   /* "-SIL-" */
    return 0;
}

/* FUN_1800c6d3c: bookmark (first = no non-bookmark fragment processed yet in this sentence) */
static int bookmark_node(zf1_ana *a, int node, int first)
{
    const zf1_node *nd = &a->fr->nd[node];
    a->bm_pending = 1;
    free(a->bm_name);
    a->bm_name = (nd->text && nd->len) ? zf_strndup(nd->text, (size_t)nd->len) : NULL;
    if (node + 1 >= a->fr->nnd || a->fr->nd[node + 1].group != nd->group) {   /* last fragment of the call */
        zf_word *w = new_word(a, 3, node, nd->text, nd->len, nd->src_off, nd->len);
        if (!w) return -1;
        { static const uint16_t sil[1] = {3}; set_pron_ids(a, w, sil, 1); }
        if (!first) {
            a->bm_pending = 0;
            free(a->bm_name);
            a->bm_name = NULL;
        }
    }
    return 0;
}

/* FUN_1800c6f58: <pron sym> fragment.  A template word (type 0, the fragment text, or " " placed at the end of the
 * previous word when the element is empty) is cloned once per pronunciation segment; a "," punctuation word
 * (type 1, i180 1) is created between segments split at SAPI punctuation. */
static int pron_node(zf1_ana *a, int node)
{
    const zf1_node *nd = &a->fr->nd[node];
    static ZF_TLS uint16_t ph[16][0x182]; /* ClassicVoices patch: was shared by every engine */
    int comma[16], n, k;
    zf_word base;
    static const zf_char SP[] = {' ', 0};
    if (!nd->sapi_phones || !nd->sapi_phones[0] || !a->hk || !a->hk->sapi_pron) return 0;
    zf_word_init(&base);
    fill_fs(&base.fs, nd);
    if (nd->text && nd->len) {
        base.text = zf_strndup(nd->text, (size_t)nd->len);
        base.src_off = nd->src_off;
        base.src_len = nd->len;
    } else {
        base.text = zf_strdup(SP);
        if (a->wl.n) base.src_off = a->wl.w[a->wl.n - 1].src_off + a->wl.w[a->wl.n - 1].src_len;
        base.src_len = 0;
    }
    base.ci = zf_strdup(base.text);
    for (k = 0; base.ci[k]; k++) base.ci[k] = c_towupper(base.ci[k]);
    n = a->hk->sapi_pron(a->hk->ctx, nd->sapi_phones, ph, comma, 16);
    for (k = 0; k < n; k++) {
        zf_word *w = zf_wl_push(&a->wl);
        if (!w) break;
        zf_word_free(w);
        zf_word_copy(w, &base);
        if (a->hk->setpron) a->hk->setpron(w, ph[k]);
        if (comma[k]) {
            static const zf_char CM[] = {',', 0};
            zf_word *c = zf_wl_push(&a->wl);
            if (!c) break;
            fill_fs(&c->fs, nd);
            c->type = 1;
            c->i180 = 1;
            c->text = zf_strdup(CM);
            c->ci = zf_strdup(CM);
            c->src_off = base.src_off;
        }
    }
    zf_word_free(&base);
    return 0;
}

/* FUN_1800c51cc: walk the fragments of the sentence */
static int walk(zf1_ana *a)
{
    const zf1_frags *fr = a->fr;
    int i = a->sp.n0, cur = 0, first = 1, done = 0, r = 0;
    while (i >= 0 && i < fr->nnd && !done && r >= 0) {
        const zf1_node *nd = &fr->nd[i];
        int from = (i == a->sp.n0) ? a->sp.p0 : 0, to, piece = 0, tlen = 0;
        if (i == a->sp.n1) {
            done = 1;
            if (nd->action > 7 || !((0xd1u >> nd->action) & 1)) return r;
            to = a->sp.p1;
        } else {
            to = nd->len;
        }
        if (is_text_action(nd->action)) {
            piece = to - from;
            if (nd->action == 0 || nd->action == 9) {
                tlen = piece;
                if (nd->text && piece) {
                    int buflen = (int)zf_strlen(a->sbuf), ok = 1, k;
                    if (piece != buflen) {
                        /* the engine compares the fragment's own text with the tail of the (normalised) buffer */
                        if (buflen - piece < 0) ok = 0;
                        for (k = 0; ok && k < piece; k++)
                            if (nd->text[from + k] != a->sbuf[buflen - piece + k]) ok = 0;
                    }
                    if (ok) {
                        int e = from + piece - 1, p = e;
                        while (p >= from && (nd->text[p] == ' ' || (nd->text[p] >= 9 && nd->text[p] <= 13))) p--;
                        tlen = (p < e) ? (p - from) + 1 : piece;
                    }
                }
            }
        }
        switch (nd->action) {
        case 6: case 7:
            break;      /* SSML sentence/paragraph markers (not reachable from SAPI) */
        case 3:
            r = bookmark_node(a, i, first);
            break;
        case 0: case 9:
            first = 0;
            r = text_piece(a, i, cur, tlen);
            break;
        case 1:
            first = 0;
            r = silence_node(a, i);
            break;
        case 2:
            first = 0;
            r = pron_node(a, i);
            break;
        case 4:
            first = 0;
            r = spell_piece(a, cur, piece);
            break;
        case 8:
            first = 0;
            break;
        default:
            first = 0;
            break;
        }
        if (is_text_action(nd->action)) cur += piece;
        i++;
        if (i >= fr->nnd || fr->nd[i].group != fr->nd[a->sp.n0].group) break;
    }
    return r;
}

/* ENU locale TA vt+0x50 (loc!FUN_180028730): '*' emphasis markers.  A run of adjacent "*" tokens (adjacent =
 * previous src_off+src_len == next src_off) directly attached to the following word opens an emphasis that a
 * later "*" run attached to its preceding word closes: the star tokens are deleted and the words in between get
 * emphasis 1 (vt+0x1f8, unless locked).  Without a closing run, an opening run of two or more stars is deleted;
 * a single star stays. */
static int is_star(const zf_word *w) { return w->text && w->text[0] == '*' && w->text[1] == 0; }
static int adjacent(const zf_word *a, const zf_word *b) { return a->src_off + a->src_len == b->src_off; }

static void lta_stars(zf1_ana *a)
{
    zf_wordlist *l = &a->wl;
    int i = 0;
    while (i < l->n) {
        int s0, s1, k, close0 = -1, close1 = -1;
        if (!is_star(&l->w[i])) { i++; continue; }
        s0 = i;
        s1 = i;             /* FUN_180028500: extend over adjacent stars */
        while (s1 + 1 < l->n && adjacent(&l->w[s1], &l->w[s1 + 1]) && is_star(&l->w[s1 + 1])) s1++;
        /* FUN_1800288f8: is the run attached to the next word? */
        if (s1 + 1 < l->n && adjacent(&l->w[s1], &l->w[s1 + 1])) {
            /* FUN_1800283f8: next star (run) attached to its preceding word */
            int prev = -1;
            for (k = s1 + 1; k < l->n; k++) {
                if (is_star(&l->w[k])) {
                    if (prev >= 0 && adjacent(&l->w[prev], &l->w[k])) {
                        close0 = k;
                        close1 = k;
                        while (close1 + 1 < l->n && adjacent(&l->w[close1], &l->w[close1 + 1]) && is_star(&l->w[close1 + 1]))
                            close1++;
                    }
                    break;
                }
                prev = k;
            }
        }
        if (close0 < 0) {
            if (s1 == s0) { i = s1 + 1; continue; }
            for (k = s1; k >= s0; k--) zf_wl_remove(l, k);   /* FUN_1800289bc */
            i = s0;
            continue;
        }
        /* FUN_180028620 */
        {
            int end = close1, j = s0;
            while (j <= end) {
                if (is_star(&l->w[j])) { zf_wl_remove(l, j); end--; }
                else {
                    if (!l->w[j].emph_lock) l->w[j].emphasis = 1;
                    j++;
                }
            }
            i = end + 1;
        }
    }
}

/* FUN_1800bd64c: commit the analyzer word list into the sentence */
static int commit(zf1_ana *a, zf_sentence *s)
{
    int i, first = 1, off0 = 0, offl = 0, lenl = 0;
    s->nwords = a->wl.n;
    s->words = a->wl.w;
    for (i = 0; i < a->wl.n; i++) {
        const zf_word *w = &a->wl.w[i];
        if (w->type == 0 || w->type == 1) {
            if (first) { first = 0; off0 = w->src_off; }
            offl = w->src_off;
            lenl = w->src_len;
        }
    }
    s->src_off = off0;
    s->src_len = (lenl - off0) + offl;
    a->wl.w = NULL;
    a->wl.n = a->wl.cap = 0;
    return 0;
}

int zf1_analyze(zf1_ana *a, zf1_frags *fr, const zf1_span *sp, zf_sentence *s)
{
    int r;
    memset(s, 0, sizeof *s);
    s->emotion = 1;
    a->fr = fr;
    a->sp = *sp;
    zf_wl_free(&a->wl);
    if (build_sbuf(a) < 0) return -1;
    r = walk(a);
    if (r >= 0) lta_stars(a);   /* step 3: ENU locale hook vt+0x50 */
    if (r >= 0 && a->hk && a->hk->pos_tag) r = a->hk->pos_tag(a->hk->ctx, &a->wl);
    if (r >= 0 && a->hk && a->hk->acronyms) r = a->hk->acronyms(a->hk->ctx, &a->wl);
    if (r >= 0 && a->hk && a->hk->tn_expand) r = a->hk->tn_expand(a->hk->ctx, &a->wl);
    if (r >= 0 && a->hk && a->hk->rules) r = a->hk->rules(a->hk->ctx, &a->wl);
    if (r >= 0 && a->hk && a->hk->pronounce) r = a->hk->pronounce(a->hk->ctx, &a->wl);
    if (r < 0) return r;
    commit(a, s);
    return r;
}
