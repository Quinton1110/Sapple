/* zf1_engine.c - zf1 public API (zf.h): text -> sentences of words with pronunciations.
 * Drives CTextProcessor::Process (FUN_1800b5110 -> FUN_18005c794) for one Speak call.  Portable C99. */
#include "zf1_eng.h"
#include <stdlib.h>
#include <string.h>

zf1_engine *zf1_open(const char *dat_path) { return zf1_open_lcid(dat_path, 1033); }

/* ClassicVoices patch: open by locale (see zf.h) */
const uint8_t *zf1_resource(const zf1_engine *e, uint32_t id1, uint32_t id2, size_t *size)
{
    return e ? zf1_dat_get(&e->dat, id1, id2, size) : NULL;
}

zf1_engine *zf1_open_lcid(const char *dat_path, int lcid)
{
    zf1_engine *e = (zf1_engine *)calloc(1, sizeof *e);
    size_t n;
    const uint8_t *r;
    if (!e) return NULL;
    e->lcid = lcid;
    if (dat_path && strlen(dat_path) < sizeof e->datdir) {
        size_t k = strlen(dat_path);
        while (k > 0 && dat_path[k - 1] != '\\' && dat_path[k - 1] != '/') k--;
        memcpy(e->datdir, dat_path, k);
        e->datdir[k] = 0;
    }
    if (zf1_dat_load(&e->dat, dat_path) < 0) { free(e); return NULL; }
    r = zf1_dat_get(&e->dat, 0x7bd71f46u, 0xf9a99c02u, &n);
    if (zf1_lex_init(&e->lex, r, n) < 0) goto fail;
    r = zf1_dat_get(&e->dat, 0x29a5584bu, 0x153f1b64u, &n);
    if (zf1_phoneset_init(&e->ps, r, n) < 0) goto fail;
    if (zf1_wb_init(&e->wb, &e->dat, 0x409) < 0) goto fail;
    if (zf1_ctab_init(&e->ct, &e->dat) < 0) goto fail;
    if (zf1_modules_init(e) < 0) goto fail;
    e->ana.wb = &e->wb;
    e->ana.lex = &e->lex;
    e->ana.ps = &e->ps;
    e->ana.ct = &e->ct;
    e->ana.hk = &e->hooks;
    return e;
fail:
    zf1_close(e);
    return NULL;
}

void zf1_close(zf1_engine *e)
{
    if (!e) return;
    zf1_modules_free(e);
    zf1_frags_free(&e->fr);
    zf_wl_free(&e->ana.wl);
    free(e->ana.bm_name);
    zf1_lex_free(&e->lex);
    zf1_dat_free(&e->dat);
    free(e);
}

int zf1_speak(zf1_engine *e, const zf_char *text, int len, int xml)
{
    zf1_frags_free(&e->fr);
    if (zf1_frags_build(&e->fr, text, len, xml) < 0) return -1;
    e->group = 0;
    e->ngroups = e->fr.nnd ? e->fr.nd[e->fr.nnd - 1].group + 1 : 0;
    zf1_enum_start(&e->en, &e->fr, 0);
    e->en.sentsep = e->sentsep;
    e->en.ssctx = e->ssctx;
    e->ana.bm_pending = 0;
    free(e->ana.bm_name);
    e->ana.bm_name = NULL;
    return 0;
}

int zf1_speak_utf8(zf1_engine *e, const char *text, int xml)
{
    int n, r;
    zf_char *w = zf_from_utf8(text, -1, &n);
    if (!w) return -1;
    r = zf1_speak(e, w, n, xml);
    free(w);
    return r;
}

/* first/last word flags (FUN_180063378): +0x2bc on the first word with a pronunciation of the Speak call (if the
 * sentence starts the text), +0x2b8 on the last one (if the sentence ends the text) */
static int span_starts_text(const zf1_engine *e, const zf1_span *sp)
{
    int i, first = -1;
    for (i = 0; i < e->fr.nnd; i++) {
        const zf1_node *nd = &e->fr.nd[i];
        if (nd->group != e->group) continue;
        if (nd->action < 10 && ((0x215u >> nd->action) & 1)) { first = i; break; }
    }
    for (i = 0; i < e->fr.nnd; i++)
        if (e->fr.nd[i].group == e->group) break;   /* fragment list head */
    if (first < 0) return 0;
    if (sp->n0 == first) return sp->p0 == 0;
    return sp->n0 == i;
}

int zf1_ss_ws(zf_char c);            /* zf1_ss.c: FUN_180072d30 with the engine's char tables */
static int zf1_ws_class(zf_char c) { return zf1_ss_ws(c); }

static int span_ends_text(const zf1_engine *e, const zf1_span *sp)
{
    int i, last = -1, tl;
    if (sp->n1 < 0) return 1;
    for (i = 0; i < e->fr.nnd; i++) {
        const zf1_node *nd = &e->fr.nd[i];
        if (nd->group != e->group) continue;
        if (nd->action < 10 && ((0x215u >> nd->action) & 1)) last = i;
    }
    if (last < 0) return 0;
    tl = 0;
    if (e->fr.nd[last].action != 2 && e->fr.nd[last].action != 9) {
        tl = e->fr.nd[last].len;
        while (tl > 0 && zf1_ws_class(e->fr.nd[last].text[tl - 1])) tl--;
    }
    return sp->n1 == last && sp->p1 >= tl;
}

int zf1_next_sentence(zf1_engine *e, zf_sentence *out)
{
    for (;;) {
        zf1_span sp;
        int i;
        if (e->group >= e->ngroups) return 0;
        if (!zf1_enum_next(&e->en, &e->fr, &sp)) {
            e->group++;
            if (e->group >= e->ngroups) return 0;
            zf1_enum_start(&e->en, &e->fr, e->group);
            e->en.sentsep = e->sentsep;
            e->en.ssctx = e->ssctx;
            continue;
        }
        if (zf1_analyze(&e->ana, &e->fr, &sp, out) < 0) return -1;
        if (out->nwords == 0) {
            zf_sentence_free(out);
            continue;
        }
        if (span_ends_text(e, &sp))
            for (i = out->nwords - 1; i >= 0; i--)
                if (out->words[i].type != 3 && out->words[i].pron && out->words[i].pron[0]) { out->words[i].i2b8 = 1; break; }
        if (span_starts_text(e, &sp))
            for (i = 0; i < out->nwords; i++)
                if (out->words[i].type != 3 && out->words[i].pron && out->words[i].pron[0]) { out->words[i].i2bc = 1; break; }
        zf1_sentence_finish(e, out);
        zf1_sentence_post(e, out);
        /* output convention (as in the engine's CTTSString): an empty string is a NULL pointer */
        for (i = 0; i < out->nwords; i++) {
            zf_word *w = &out->words[i];
            int k;
            for (k = 0; k < w->nprons; k++)
                if (w->prons[k] && !w->prons[k][0]) { free(w->prons[k]); w->prons[k] = NULL; }
            if (w->pron && !w->pron[0]) { free(w->pron); w->pron = NULL; }
        }
        return 1;
    }
}

const zf1_frags *zf1_debug_frags(const zf1_engine *e) { return &e->fr; }

void zf_sentence_free(zf_sentence *s)
{
    int i;
    for (i = 0; i < s->nwords; i++) zf_word_free(&s->words[i]);
    free(s->words);
    free(s->quotes);
    for (i = 0; i < s->npar; i++) free(s->par[i].el);
    free(s->par);
    free(s->segs);
    memset(s, 0, sizeof *s);
}
