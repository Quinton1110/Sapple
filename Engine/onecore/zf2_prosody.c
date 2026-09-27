/* zf2_prosody.c - CLinguisticProsodyTagger::Process (0x180060010) for en-US David / Zira / Mark.
 *
 * No statistical prosody model runs for these voices: the CART predictors (loc CProsodyBreakCARTPredictor /
 * CProsodyEmphasisCARTPredictor) are attached to the model runners but inactive (+0xc == 0, verified at runtime),
 * no CRF/RNN model files ship.  What runs is rule code:
 *   Break predictor  pre  0x18009a400: SSML breaks 0x18009afd0, punctuation+emphasis 0x180059964, quotes
 *                         0x18009ad24, parallel structures 0x18009aa04, sentence end 0x18009a8b0, TN silences
 *                         0x18009b3e4, grouped words 0x18009a808
 *                   post  domain handlers vt+0x70 (all no-ops); vt+0x40 -> loc vt+0x30 = phrase build (redone later)
 *   Emphasis         pre  word check only
 *   Boundary tone    pre  0x1800508d4 (tones from punctuation, emphasis); vt+0x40 -> loc list-item tagger
 *                         (CListItemBoundaryToneTaggerEnglish, loc 0x180061530)
 *   Pitch accent     pre  0x180019e70 (ToBI accent codes of emphasised words)
 *   ENU post step loc vt+0x70 (CBoundaryPronChange: no en-US data -> no-op).
 */
#include <string.h>
#include "zf2_int.h"

static void set_bi(Z2W *w, int v) { if (!w->bi_lock) w->bi = v; }          /* vt+0x1a8 */
static void set_tone(Z2W *w, int v) { if (!w->tone_lock) w->tone = v; }    /* vt+0x158 */
static void set_emph(Z2W *w, int v) { if (!w->emph_lock) w->emph = v; }    /* vt+0x1f8 */

/* FUN_18009a554: 1 if every word after i is a break token (or there is none) */
static int rest_all_breaks(const Z2Sent *s, int i)
{
    for (i++; i < s->nw; i++) if (!z2_is_break(&s->w[i])) return 0;
    return 1;
}

/* FUN_18009a710: is the say-as category of the punctuation token in the break predictor's list?
 * The list (predictor+0x30) is empty for en-US (verified at runtime), so this is always 0. */
static int sayas_in_list(const Z2Sent *s, const Z2W *w) { (void)s; (void)w; return 0; }

/* FUN_180073c04: sentence-internal punctuation */
static void punct_mid(Z2Sent *s, Z2W *w, int prev)
{
    w->i228 = 1;
    if (prev >= 0) {
        Z2W *p = &s->w[prev];
        if (p->bi < 4 && p->i248 == 0 && !p->bi_lock) {
            set_bi(p, 4);
            p->pause = sayas_in_list(s, w) ? Z2_PC_PARALIP : Z2_PC_IP;
        }
    }
}
static void punct_final(Z2Sent *s, Z2W *w, int cls, int prev)
{
    w->i228 = cls;
    if (prev >= 0 && !s->w[prev].bi_lock) {
        set_bi(&s->w[prev], 5);
        s->w[prev].pause = Z2_PC_SENT;
    }
}

/* FUN_180059964: punctuation -> break, SAPI <emph> -> word emphasis */
static void rule_punct(Z2Sent *s)
{
    int i;
    for (i = 0; i < s->nw; i++) {
        Z2W *w = &s->w[i];
        int prev = z2_prev_rw(s, i);
        if (w->type == 1) {
            int c = w->i180, fin = rest_all_breaks(s, i);
            if (c == 1 || c == 2 || c == 3 || c == 7) {
                if (fin) punct_final(s, w, 7, prev);
                else punct_mid(s, w, prev);
            } else if (c == 4) {
                if (fin) punct_final(s, w, 7, prev);
            } else if (c == 5) {
                if (fin) punct_final(s, w, 2, prev);
                else if (prev >= 0) punct_mid(s, w, prev);
            } else if (c == 6) {
                if (fin) {
                    int st = s->type, v = st == 1 ? 3 : st == 6 ? 5 : st == 7 ? 6 : 4;
                    punct_final(s, w, v, prev);
                } else if (prev >= 0)
                    punct_mid(s, w, prev);
            }
        } else if ((w->type & ~4) == 0 && w->frag >= 0) {
            int e = s->frag[w->frag].emph;
            if (e == 0) set_emph(w, 0);
            else if (e == 1 || e == 2) { set_emph(w, 1); w->emph_lock = 1; }
            else if (e == 3 || e == 4) { set_emph(w, 0); w->emph_lock = 1; }
        }
        if (z2_is_sapi_silence(s, w) && prev >= 0) set_bi(&s->w[prev], 4);
    }
}

/* FUN_18009ad24: quoted / bracketed stretches of >1 word and >3 syllables get BI 3 before and inside the end */
static void rule_quotes(Z2Sent *s)
{
    int q, last = s->nw ? s->nw - 1 : -1;
    int pc = s->v->punc_interm_raw == 10000 ? Z2_PC_INTERM : Z2_PC_PUNCINTERM;
    if (last >= 0 && !z2_is_rw(&s->w[last])) last = z2_prev_rw(s, last);
    for (q = 0; q < s->nquotes; q++) {
        int a = s->quotes[q].open, b = s->quotes[q].close, k, nwrd = 0, nsyl = 0;
        if (a < 0 || b < 0) continue;
        for (k = a; k != b && k < s->nw; k++)
            if (z2_is_rw(&s->w[k])) { nwrd++; nsyl += s->w[k].ns; }
        if (nwrd > 1 && nsyl > 3) {
            int p;
            s->w[a].i228 = 10;
            p = z2_prev_rw(s, a);
            if (p >= 0 && s->w[p].bi < 3 && !s->w[p].bi_lock) {
                set_bi(&s->w[p], 3);
                s->w[p].pause = pc;
                s->w[p].bi_lock = 1;
            }
            s->w[b].i228 = 11;
            p = z2_prev_rw(s, b);
            if (p >= 0 && s->w[p].bi < 3 && p != last && !s->w[p].bi_lock) {
                set_bi(&s->w[p], 3);
                s->w[p].pause = pc;
                s->w[p].bi_lock = 1;
            }
        }
    }
}

/* FUN_18009aa04 (utterance domain not in engine+0x548: the parallel-structure branch) */
static void rule_parallel(Z2Sent *s)
{
    int q, e;
    for (q = 0; q < s->npar; q++) {
        struct zf_parstruct *ps = &s->par[q];
        Z2W *w;
        int p;
        if (ps->nel <= 0) continue;
        for (e = 0; e < ps->nel - 1; e++)
            if (ps->el[e].b >= 0) s->w[ps->el[e].b].pause = Z2_PC_PARALIP;
        if (ps->el[ps->nel - 1].b >= 0) {
            w = &s->w[ps->el[ps->nel - 1].b];
            if (w->bi < 3 && !w->bi_lock) { set_bi(w, 3); w->pause = Z2_PC_INTERM; }
        }
        p = ps->el[ps->nel - 1].a >= 0 ? z2_prev_rw(s, ps->el[ps->nel - 1].a) : -1;
        if (p >= 0) {
            w = &s->w[p];
            if (w->bi < 4 && !w->bi_lock) { set_bi(w, 4); w->pause = Z2_PC_PARALIP; }
        }
    }
}

/* FUN_18009a8b0: the last word that has a pronunciation gets BI 5 / pause 7 / tone 0x3ea */
static void rule_last(Z2Sent *s)
{
    int i;
    for (i = s->nw - 1; i >= 0; i--) {
        Z2W *w = &s->w[i];
        if (i > 0 && (z2_is_break(w) || w->pron_null)) continue;
        set_bi(w, 5);
        w->pause = Z2_PC_SENT;
        set_tone(w, 0x3ea);
        break;
    }
}

/* FUN_18009b3e4: explicit TN silence (+0x214) -> BI 4 / pause 4 */
static void rule_tn_silence(Z2Sent *s)
{
    int i;
    for (i = 0; i < s->nw; i++) {
        Z2W *w = &s->w[i];
        if (w->i214 && !w->bi_lock) { set_bi(w, 4); w->pause = Z2_PC_PARALIP; }
    }
}

/* FUN_18009a808: consecutive words of one group object -> BI 1 */
static void rule_groups(Z2Sent *s)
{
    int i;
    for (i = 0; i + 1 < s->nw; i++)
        if (s->w[i].group && s->w[i].group == s->w[i + 1].group) {
            set_bi(&s->w[i], 1);
            s->w[i].i248 = 1;
        }
}

/* FUN_1800508d4: boundary tones from the punctuation class +0x228 */
static void rule_tones(Z2Sent *s)
{
    int i, anyemph = 0, last = -1, f;
    for (i = 0; i < s->nw; i++) if (s->w[i].emph == 1) anyemph = 1;
    /* first word with a non-empty pronunciation (any type, the sentence-initial SIL token included) */
    for (f = 0; f < s->nw && z2_pron_len(&s->w[f]) == 0; f++);
    last = f < s->nw ? f : -1;
    for (i = f + 1; i < s->nw; i++) {
        Z2W *w = &s->w[i];
        if (last >= 0 && w->i228 != 0) {
            Z2W *l = &s->w[last];
            int c = w->i228;
            if (c == 1) { set_tone(l, 0x3ea); l->i240 = 5; }
            else if (c == 2 || c == 4 || c == 6 || c == 7) { set_tone(l, 0x3ea); l->i240 = 10; }
            else if (c == 3 || c == 5) { set_tone(l, 0x3ec); l->i240 = 10; }
            else { set_tone(l, 0x3eb); l->i240 = 5; }
        }
        if (z2_pron_len(w) != 0 && w->type != 3) last = i;
    }
    if (anyemph)
        for (i = 0; i < s->nw; i++)
            if (s->w[i].emph == 1) {
                set_tone(&s->w[i], 0);
                if (i > 0) set_tone(&s->w[i - 1], 0);
            }
}

/* loc CListItemBoundaryToneTaggerEnglish (0x180061530): list items before the last one */
static void rule_list_tones(Z2Sent *s)
{
    int q, e;
    for (q = 0; q < s->npar; q++)
        for (e = 0; e < s->par[q].nel - 1; e++) {
            int b = s->par[q].el[e].b;
            if (b < 0) continue;
            if (s->w[b].bi == 4) set_tone(&s->w[b], 0x3ed);
            else if (s->w[b].bi == 3) set_tone(&s->w[b], 0x3e9);
        }
}

/* FUN_180019e70: ToBI accent codes of <emph> words (+0x22c, syllable +0xc0) */
static void rule_accents(Z2Sent *s)
{
    int i, nphr = 0, first = 1;
    for (i = 0; i < s->nw; i++) {
        Z2W *w = &s->w[i];
        if (!z2_is_rw(w) || !z2_has_syl_pron(w)) continue;
        if (w->frag >= 0 && (s->frag[w->frag].emph == 1 || s->frag[w->frag].emph == 2)) {
            int v = 6;
            if (first) {
                v = nphr == 0 ? (s->type == 2 ? 1 : 5) : 1;
                first = 0;
            }
            if (w->i230 == 0) { /* vt+0x138 */
                int k;
                w->i22c = v;
                for (k = 0; k < w->ns; k++) s->s[w->s0 + k].tobi = 0;
                z2_set_tobi_accent(s, i);
            }
            w->i230 = 1;
        }
        if (w->bi > 3) { nphr++; first = 1; }
    }
}

void z2_prosody(Z2Sent *s)
{
    /* SSML <break> tokens (FUN_18009afd0) never occur with SAPI XML input (fragment +0x18 flag is not set) */
    rule_punct(s);
    rule_quotes(s);
    rule_parallel(s);
    rule_last(s);
    rule_tn_silence(s);
    rule_groups(s);
    if (s->v && s->v->prosody_hook) s->v->prosody_hook(s->v->prosody_ctx, s, 1);   /* break model runner (BR2) */
    s->rebuild_flag = 1;
    rule_tones(s);
    if (s->v && s->v->prosody_hook) s->v->prosody_hook(s->v->prosody_ctx, s, 2);   /* boundary-tone runner (TON) */
    rule_list_tones(s);
    rule_accents(s);
    if (s->v && s->v->prosody_hook) s->v->prosody_hook(s->v->prosody_ctx, s, 3);   /* pitch-accent runner (ACL) */
}
