/* zf1_modules.c - wiring of the zf1 component modules into the analyzer hooks, plus the first-pass word-level
 * part of sentence vt+0xa0 (FUN_18001a644).  Portable C99. */
#include "zf1_eng.h"
#include "zf1_post.h"
#include "zf1_ss.h"
#include "zf1_pos.h"
#include "zf1_pron.h"
#include "zf1_tn.h"
#include <stdlib.h>
#include <string.h>

/* ClassicVoices patch: function-level scratch is per thread (two voices may speak on two threads at once). */
#ifndef ZF_TLS
#define ZF_TLS __thread
#endif

typedef struct zf1_mods {
    zf1_post post;
    int post_ok;
    zf1_ss ss;
    int ss_ok;
    zf1_pos pos;
    int pos_ok;
    zf1_pron pron;
    int pron_ok;
    zf1_tn tn;
    int tn_ok;
    zf_char **cats;                     /* interned entity category strings (tokens point at them) */
    int ncats;
} zf1_mods;

/* ---------------- NE / TN (zf1_tn.c, fork B) ---------------- */
static const zf_char *intern_cat(zf1_mods *m, const zf_char *c)
{
    int i;
    for (i = 0; i < m->ncats; i++) if (!zf_strcmp(m->cats[i], c)) return m->cats[i];
    m->cats = (zf_char **)realloc(m->cats, sizeof(zf_char *) * (size_t)(m->ncats + 1));
    m->cats[m->ncats] = zf_strdup(c);
    return m->cats[m->ncats++];
}

/* FUN_1800c4f10 (FullContext) -> FUN_1800c6888: NE entity tokens + word-broken plain spans */
static int hk_text_tokens(void *ctx, zf1_ana *a, const zf1_node *nd, const zf_char *text, int off, int len,
                          zf1_toklist *out)
{
    zf1_mods *m = (zf1_mods *)((zf1_engine *)ctx)->mod;
    zf1_tnseg *segs = NULL;
    int ns, i, r = 0;
    ns = zf1_tn_segment(&m->tn, text + off, len, nd ? nd->sayas : NULL, &segs);
    for (i = 0; i < ns && r >= 0; i++) {
        if (segs[i].entity) r = zf1_tok_add(out, off + segs[i].off, segs[i].len, 1, intern_cat(m, segs[i].cat), segs[i].attr);
        else r = zf1_ana_break(a, off + segs[i].off, segs[i].len, out);
    }
    free(segs);
    return r;
}

static void cb_tn_postag(void *ctx, zf_wordlist *wl, int first, int count, uint16_t oldpos)
{
    zf1_mods *m = (zf1_mods *)ctx;
    if (!m->pos_ok) return;
    if (count == 1) zf1_pos_set(&m->pos, &wl->w[first], oldpos);
    zf1_pos_tag(&m->pos, wl, first, first + count);
}

/* Analyze step 7b (FUN_180018ed4 after the acronym pass); step 4 domains (FUN_18002bf08) are applied here too */
static int hk_tn_expand(void *ctx, zf_wordlist *wl)
{
    zf1_mods *m = (zf1_mods *)((zf1_engine *)ctx)->mod;
    int i;
    for (i = 0; i < wl->n; i++) zf1_tn_domain(&wl->w[i]);
    return zf1_tn_expand(&m->tn, wl, cb_tn_postag, m);
}

/* Analyze step 7a: CAcronymHandler (FUN_180079408); the step-4 domains (FUN_18002bf08) must be set first */
static int hk_acronyms(void *ctx, zf_wordlist *wl)
{
    zf1_engine *e = (zf1_engine *)ctx;
    zf1_mods *m = (zf1_mods *)e->mod;
    int i;
    if (!m->pos_ok) return 0;
    for (i = 0; i < wl->n; i++) zf1_tn_domain(&wl->w[i]);
    return zf1_acro_apply(&m->pos, wl, &e->wb);
}

static void cb_setpos(void *posobj, zf_word *w, uint16_t pos) { zf1_pos_set((const zf1_pos *)posobj, w, pos); }
static int cb_poly(void *posobj, zf_wordlist *wl) { return zf1_poly_apply((zf1_pos *)posobj, wl); }

static int hk_pos(void *ctx, zf_wordlist *wl)
{
    zf1_mods *m = (zf1_mods *)((zf1_engine *)ctx)->mod;
    return m->pos_ok ? zf1_pos_tag(&m->pos, wl, 0, wl->n) : 0;
}

static int hk_sapi_pron(void *ctx, const uint16_t *ids, uint16_t (*ph)[0x182], int *comma, int max)
{
    zf1_mods *m = (zf1_mods *)((zf1_engine *)ctx)->mod;
    static ZF_TLS zf1_sapiseg seg[16]; /* ClassicVoices patch: was shared by every engine */
    int n, k;
    if (!m->pron_ok) return 0;
    if (max > 16) max = 16;
    n = zf1_pron_sapi(&m->pron, ids, seg, max);
    for (k = 0; k < n; k++) {
        memcpy(ph[k], seg[k].ph, sizeof seg[k].ph);
        comma[k] = seg[k].comma_after;
    }
    return n;
}

static void hk_setpron(zf_word *w, const uint16_t *ph) { zf1_word_setpron(w, ph, 20, 1); }

static int hk_pron(void *ctx, zf_wordlist *wl)
{
    zf1_mods *m = (zf1_mods *)((zf1_engine *)ctx)->mod;
    return m->pron_ok ? zf1_pron_sentence(&m->pron, wl) : 0;
}

static int hk_sentsep(void *ctx, const zf_char *text, int len, int *consumed, int *hard_end)
{
    zf1_mods *m = (zf1_mods *)ctx;
    return zf1_ss_run(&m->ss, text, len, consumed, hard_end);
}

static int hk_rules(void *ctx, zf_wordlist *wl)
{
    zf1_engine *e = (zf1_engine *)ctx;
    zf1_mods *m = (zf1_mods *)e->mod;
    return m->post_ok ? zf1_post_words(&m->post, wl) : 0;
}

/* post-Analyze sentence steps (after the first FUN_18001a644 pass) */
void zf1_sentence_post(zf1_engine *e, zf_sentence *s)
{
    zf1_mods *m = (zf1_mods *)e->mod;
    zf1_postout o;
    memset(&o, 0, sizeof o);
    if (!m->post_ok) return;
    zf1_post_sentence(&m->post, s, &o);
    s->nsegs = o.nsegs;
    s->segs = o.segs;
    o.segs = NULL;
    o.nsegs = 0;
    zf1_postout_free(&o);
}

/* TEMPORARY stand-in until zf1_pron (fork A) is wired: first lexicon pronunciation */
static int tmp_pronounce(void *ctx, zf_wordlist *wl)
{
    zf1_engine *e = (zf1_engine *)ctx;
    int i;
    for (i = 0; i < wl->n; i++) {
        zf_word *w = &wl->w[i];
        zf1_lexent ent;
        if (w->type != 0 || !w->text || w->ne_type) continue;
        if (zf1_lex_lookup(&e->lex, w->text, (int)zf_strlen(w->text), &ent) && ent.n > 0) {
            free(w->pron);
            w->pron = zf1_phones_to_str(&e->ps, ent.p[0].ph, ent.p[0].nph);
        }
    }
    return 0;
}

/* fork C's zf1_pos word_sets: the lexicon entry the engine has attached to a word (word vt+0x258).  TN output
 * words keep their token's domain (s1b0); with a domain lexicon (enUS.<Domain>.dat) holding the word, the entry
 * found by the domain-aware container lookup (FUN_1800484b4 -> 0x1800c1350) is the domain one, whose attribute
 * sets replace the main-lexicon ones for POS tagging (prose line 667: "use" in the address domain is a noun). */
static int cb_word_sets(void *ctx, const zf_word *w, uint16_t *sets, int cap)
{
    zf1_mods *m = (zf1_mods *)ctx;
    zf1_lexhit *h;
    int k = 0, i, j, q, n;
    if (!w->text || !w->s1b0 || !w->s1b0[0] || !zf_strcmp_a(w->s1b0, "none") || !zf_strcmp_a(w->s1b0, "general"))
        return -1;
    n = (int)zf_strlen(w->text);
    h = (zf1_lexhit *)malloc(sizeof *h);
    if (!h) return -1;
    if (!zf1_pron_lookup_dom(&m->pron, w->text, n, w->s1b0, h) || h->n == 0 || h->p[0].source != 0x10000) {
        free(h);
        return -1;
    }
    for (i = 0; i < h->n; i++)
        for (j = 0; j < h->p[i].nset; j++) {
            for (q = 0; h->p[i].set[j][q] && k < cap - 1; q++) sets[k++] = h->p[i].set[j][q];
            if (k < cap) sets[k++] = 0;
        }
    free(h);
    return k;
}

int zf1_modules_init(zf1_engine *e)
{
    zf1_mods *m = (zf1_mods *)calloc(1, sizeof *m);
    if (!m) return -1;
    e->mod = m;
    m->post_ok = zf1_post_init(&m->post, &e->dat) == 0;
    m->ss_ok = zf1_ss_init(&m->ss, &e->dat) == 0;
    if (m->ss_ok) { e->sentsep = hk_sentsep; e->ssctx = m; }
    memset(&e->hooks, 0, sizeof e->hooks);
    e->hooks.ctx = e;
    e->hooks.rules = hk_rules;
    m->pron_ok = zf1_pron_init(&m->pron, &e->dat, &e->lex, &e->ps) == 0;
    if (m->pron_ok && m->pron.ndom == 0 && e->datdir[0]) zf1_pron_load_domains_lcid(&m->pron, e->datdir, e->lcid);   /* non-default location */
    m->pos_ok = zf1_pos_init(&m->pos, &e->dat, &e->lex) == 0;
    if (m->pos_ok) zf1_acro_init_lcid(&m->pos, getenv("ZF1_DATDIR") ? getenv("ZF1_DATDIR") : (e->datdir[0] ? e->datdir : NULL), e->lcid);   /* domain .dat files next to the main .dat */
    if (m->pos_ok && m->pron_ok) {
        zf1_pos_set_lookup(&m->pos, zf1_pron_lexlookup_cb, &m->pron);
        m->pron.posobj = &m->pos;
        m->pron.setpos = cb_setpos;
        m->pron.poly = cb_poly;
        m->pos.word_sets = cb_word_sets;
        m->pos.word_sets_ctx = m;
    }
    m->tn_ok = zf1_tn_init(&m->tn, &e->dat) == 0;
    if (m->tn_ok) {
        e->hooks.text_tokens = hk_text_tokens;
        e->hooks.tn_expand = hk_tn_expand;
    }
    e->hooks.pos_tag = hk_pos;
    e->hooks.acronyms = hk_acronyms;
    e->hooks.sapi_pron = hk_sapi_pron;
    e->hooks.setpron = hk_setpron;
    e->hooks.pronounce = m->pron_ok ? hk_pron : tmp_pronounce;
    return 0;
}

void zf1_modules_free(zf1_engine *e)
{
    zf1_mods *m = (zf1_mods *)e->mod;
    if (m && m->post_ok) zf1_post_free(&m->post);
    if (m && m->ss_ok) zf1_ss_free(&m->ss);
    if (m && m->pos_ok) zf1_pos_free(&m->pos);
    if (m && m->pron_ok) zf1_pron_free(&m->pron);
    if (m && m->tn_ok) zf1_tn_free(&m->tn);
    if (m) { int i; for (i = 0; i < m->ncats; i++) free(m->cats[i]); free(m->cats); }
    free(e->mod);
    e->mod = NULL;
}

/* ---------------- first pass of FUN_18001a644 (word level) ----------------
 * The engine builds syllables/phones here (zf2's job) and inserts SIL words:
 *  - before the first word that has a pronunciation (while the sentence has no phones yet): 100 ms (the
 *    SpeakSessionStart setting, 100 for David and Zira, when that word is the first word of the Speak call),
 *    BreakIndex 5, pause class 7 (FUN_180068c6c; a pending bookmark moves to the SIL word);
 *  - after a word whose silence (FUN_1800199a8: +0x214 if set, else SilenceLength[pause class], which is 0 at
 *    this stage) is non-zero, unless the next word (skipping pron-less type 0/4 words) is a <silence> word
 *    (FUN_18014ad10). */
static int has_pron(const zf_word *w) { return w->pron && w->pron[0]; }

static void make_sil(zf_word *s, const zf_word *tpl, int ms)
{
    static const zf_char SP[] = {' ', 0};
    static const zf_char SILN[] = {'-', 'S', 'I', 'L', '-', 0};
    static const zf_char SILID[] = {3, 0};
    zf_word_init(s);
    s->type = 3;
    s->fs = tpl->fs;
    s->lang = tpl->lang;
    s->src_off = tpl->src_off;
    s->src_len = 0;
    s->bi = tpl->bi;
    s->pause_class = tpl->pause_class;
    s->text = zf_strdup(SP);
    s->regular = zf_strdup(SP);
    s->ci = zf_strdup(SP);
    s->pron = zf_strdup(SILN);
    s->nprons = 1;
    s->prons = (zf_char **)malloc(sizeof(zf_char *));
    s->prons[0] = zf_strdup(SILID);
    s->cur_pron = 0;
    s->silence_ms = ms;
    s->attached_pause = -1;
}

static zf_word *insert_word(zf_sentence *s, int at)
{
    zf_word *nw = (zf_word *)realloc(s->words, sizeof(zf_word) * (size_t)(s->nwords + 1));
    if (!nw) return NULL;
    s->words = nw;
    memmove(nw + at + 1, nw + at, sizeof(zf_word) * (size_t)(s->nwords - at));
    s->nwords++;
    return &nw[at];
}

void zf1_sentence_finish(zf1_engine *e, zf_sentence *s)
{
    int i, have_phones = 0, k;
    (void)e;
    for (i = 0; i < s->nwords; i++) s->words[i].attached_pause = -1;
    for (i = 0; i < s->nwords; i++) {
        zf_word *w = &s->words[i];
        if (w->type != 3 && has_pron(w) && w->lang) {
            int after;
            if (!have_phones) {
                int ms = 100;   /* w->i2bc == 1: SpeakSessionStart (default 100) */
                if (ms) {
                    zf_word tmp, *sil;
                    make_sil(&tmp, w, ms);
                    tmp.bi = 5;
                    tmp.pause_class = 7;
                    if (w->i2f0) {
                        tmp.i2f0 = w->i2f0;
                        tmp.s2f8 = w->s2f8;
                        w->i2f0 = 0;
                        w->s2f8 = NULL;
                    }
                    sil = insert_word(s, i);
                    if (!sil) return;
                    *sil = tmp;
                    have_phones = 1;
                    continue;    /* re-visit the word at i+1 */
                }
            }
            have_phones = 1;
            after = w->i214;
            if (after && w->attached_pause < 0) {
                k = i + 1;
                while (k < s->nwords && (s->words[k].type & ~4) == 0 && !has_pron(&s->words[k])) k++;
                if (!(k < s->nwords && s->words[k].type == 3 && s->words[k].fs.action == 1)) {
                    zf_word tmp, *sil;
                    make_sil(&tmp, w, after);
                    sil = insert_word(s, i + 1);
                    if (!sil) return;
                    *sil = tmp;
                    s->words[i].attached_pause = i + 1;
                }
            }
        }
        /* type-3 words (<silence>, bookmarks) do not count as phones here: "<silence/>text" still gets the
           sentence-initial SIL before the first pronounced word */
    }
}
