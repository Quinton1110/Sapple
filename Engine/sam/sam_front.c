/* libsam front end: text -> words -> phone items -> segments, then the back end in sam.c.
 *
 * Stages mirror the original engine (spttseng.dll); each was verified against the real engine with
 * the tools in tools/ (see the Python models named in the comments):
 *   normalizer + tagger                                  sam_norm.c / sam_pos.c
 *   word records incl. number prosody                    FUN_5ed45c97 / FUN_5ed45ac0 (below)
 *   word passes: emphasis / phrasing / accents           word_prosody.py   FUN_5ed454df/45651/43f71
 *   items + position flags                               frontend_model.py FUN_5ed42301/5ed5623e
 *   unit selection (decision trees in the .spd)          units_model.py    FUN_5ed58bfa
 *   durations                                            frontend_model.py FUN_5ed4387b
 *   F0 contour                                           prosody_model.py  FUN_5ed541ad/53c25
 */
#include "sam.h"
#include "sam_lex.h"
#include "sam_internal.h"
#include "sam_norm.h"
#include "sam_norm_types.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CV_NO_DEBUG_ENV /* ClassicVoices patch: the shipped screen-reader voice never prints what it speaks */
#define getenv(name) ((char *)0)
#endif

#define SIL 17
#define ST1 43
#define ST2 44
#define SYL 46
#define MAX_WORD_PH 128

/* internal phone id -> SAPI phone id (@5ed31b00) */
static const int SAPI_PHONE[47] = {28, 27, 21, 11, 10, 12, 13, 43, 15, 22, 23, 16, 36, 14, 35, 44, 0, 7, 46, 47, 38, 31,
                                   26, 32, 33, 34, 24, 45, 42, 20, 39, 48, 40, 49, 37, 17, 41, 19, 30, 25, 18, 29, 0, 8,
                                   9, 0, 1};
/* phone properties (@5ed31a50): bit0 vowel, bit2 boundary-tone anchor */
static const uint32_t PHONE_PROPS[43] = {
    0x28003d, 0x20003d, 0x20003d, 0x20003d, 0x3d, 0x3d, 0x3d, 0x3d, 0x3d, 0x3d, 0x68003d, 0x40003d, 0x40003d,
    0x40003d, 0x40003d, 0x40003d, 0x20003d, 0x20, 0x20001b6, 0xc01b6, 0x20001b6, 0x1b6, 0x22, 0x808976, 0x802976,
    0x804976, 0x8008402, 0x8008406, 0x8010402, 0x8010406, 0x8002402, 0x8002406, 0x8020402, 0x8020406, 0x809e02,
    0x809e06, 0x803e02, 0x803e06, 0x805e02, 0x805e06, 0x1020e02, 0x1020e06, 0xc06};
static const float PAUSE_TABLE[21] = {0.2f, 0.2f, 0.3f, 0.3f, 0.3f, 0.3f, 0.2f, 0.2f, 0.2f, 0.2f, 0.1f,
                                      0.01f, 0.2f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f};
static const float FINAL_LENGTHEN[20] = {1.0f, 1.33f, 1.67f, 1.67f, 1.67f, 1.67f, 1.0f, 1.0f, 1.0f, 1.0f,
                                         1.0f, 1.3f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

static int is_vowel(int ph) { return ph >= 0 && ph < 43 && (PHONE_PROPS[ph] & 1); }

/* ============================================================================================ */
/* words                                                                                        */

typedef struct {
    char text[64];
    int ph[MAX_WORD_PH], nph;
    uint32_t pos;
    int cls;
    int btype, acc, prom, bound, bstr, emph, silence_ms, rule_a, rule_b, x694, vol, semi;
    float pause, rate, x680, off, rng;
    int ofs, len;
    short note[MAX_WORD_PH]; /* sing mode: 1 + note index of each phone (0 = none) */
} word;

typedef struct {
    word *w;
    int n, cap;
} wordlist;

static word *wl_insert(wordlist *l, int at)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        word *w = realloc(l->w, sizeof(word) * (size_t)cap);
        if (!w) return NULL;
        l->w = w;
        l->cap = cap;
    }
    memmove(l->w + at + 1, l->w + at, sizeof(word) * (size_t)(l->n - at));
    l->n++;
    memset(&l->w[at], 0, sizeof(word));
    l->w[at].vol = 100;
    l->w[at].x680 = 1.0f;
    l->w[at].rng = 1.0f;
    return &l->w[at];
}

/* FUN_5ed43c81: part of speech -> word class (0 other, 1 function, 2 content, 3 auxiliary) */
static int pos_class(uint32_t p)
{
    if (p == 0x4001) return 3;
    if (p == 0x1000 || p == 0x2000 || p == 0x5000 || p == 0x3001 || p == 0x3002) return 2;
    if ((p >= 0x1001 && p <= 0x1004) || p == 0x3000 || p == 0x4000 || (p >= 0x4003 && p <= 0x4007) || p == 0x4009)
        return 1;
    return 0;
}

static int sapi_to_internal(int sapi)
{
    int i;
    if (sapi == 1) return SYL;
    if (sapi == 8) return ST1;
    if (sapi == 9) return ST2;
    for (i = 0; i < 43; i++)
        if (SAPI_PHONE[i] == sapi) return i;
    return -1;
}

/* ============================================================================================ */
/* word passes (word_prosody.py)                                                                */

static uint32_t msvc_rand(sam_tts *t)
{
    t->rand_state = t->rand_state * 214013u + 2531011u;
    return (t->rand_state >> 16) & 0x7FFF;
}

static int is_silence_word(const word *w) { return w->nph > 0 && w->ph[0] == SIL; }

static void pass0(wordlist *l)
{
    int i, prev_btype = 1, prev_out = -1;
    if (l->n && l->w[l->n - 1].btype == 2) { /* FUN_5ed4471c: emphasis in '!' sentences */
        int n_content = 0, n_all = 0, k;
        word *pick = NULL, *prio[5] = {0};
        for (i = 0; i < l->n; i++) {
            word *w = &l->w[i];
            if (w->cls == 2) {
                n_content++;
                n_all++;
                if (n_content == 1) pick = w;
                k = w->pos == 0x5000 ? 0 : w->pos == 0x3002 ? 1 : w->pos == 0x2000 ? 2 : w->pos == 0x3001 ? 3 : w->pos == 0x1000 ? 4 : -1;
                if (k >= 0 && !prio[k]) prio[k] = w;
            } else if (w->cls == 1) {
                n_all++;
                if (n_all == 1) pick = w;
            }
        }
        if (!(n_content != 1 && n_all != 1)) {
            if (pick) pick->emph = 1;
        } else if (n_content >= 2) {
            for (k = 0; k < 5; k++)
                if (prio[k]) {
                    pick = prio[k];
                    break;
                }
            if (pick) pick->emph = 1;
        }
    }
    for (i = 0; i < l->n; i++) { /* FUN_5ed454df: 1 ms pause before emphasized words */
        int pb;
        if (i < l->n - 1 && l->w[i].emph > 0 && prev_btype == 0) {
            word saved = l->w[i];
            word *x = wl_insert(l, i);
            if (!x) return;
            x->silence_ms = 1;
            x->ph[0] = SIL;
            x->nph = 1;
            x->ofs = saved.ofs;
            x->len = saved.len;
            x->x694 = 6;
            x->x680 = prev_out >= 0 ? l->w[prev_out].x680 : 1.0f;
            if (prev_out >= 0) x->vol = l->w[prev_out].vol;
            i++;
            prev_out = i - 1;
        } else {
            prev_out = i;
        }
        pb = l->w[i].btype;
        prev_btype = pb;
    }
}

static void question_type(word *w, int cq)
{
    if (w->btype == 3 || w->btype == 4) {
        w->btype = cq ? 3 : 4;
        w->rule_b = cq ? 0xE : 0xF;
    }
}

static void pass1(wordlist *l)
{
    int i = 0, prev_btype = 1, since = 0, b_conj = 0, b_det = 0, b_next = 0, cq = 0;
    uint32_t prev_pos = 0;
    int prev_idx = -1;
    while (i < l->n - 1) {
        word *W = &l->w[i];
        uint32_t pos = W->pos, npos = i + 1 < l->n ? l->w[i + 1].pos : 0, n2pos = i + 2 < l->n ? l->w[i + 2].pos : 0;
        int wh = (W->text[0] == 'w' || W->text[0] == 'W') && (W->text[1] == 'h' || W->text[1] == 'H');
        int brk_type = -1, brk_rule = 0, check = 0, cur;
        if (prev_btype >= 1 && prev_btype <= 12) {
            since = 1;
            cq = 1;
            b_conj = 0;
            b_det = 0;
        } else {
            since++;
        }
        if (since == 1) {
            if (pos == 0x4005 || wh) cq = 0;
            else if (pos == 0x4009 || pos == 0x4003 || pos == 0x4004) b_conj = 1;
        } else if (since == 2 && b_conj && (pos == 0x4005 || pos == 0x1004 || wh)) {
            cq = 0;
        }
        if (b_next) {
            brk_type = 0xD;
            brk_rule = 1;
            b_next = 0;
        } else {
            if (since == 1 && pos == 0x3002 && npos == 0x4006) {
                b_next = 1;
                check = 1;
            } else {
                b_next = 0;
                if (pos == 0x4004) {
                    if (b_det || since < 4 || n2pos == 0x4003) check = 1;
                    else {
                        brk_type = 0xE;
                        brk_rule = 2;
                    }
                } else if (pos != 0x3002 || since < 5 || npos == 0x3001) {
                    check = 1;
                } else {
                    brk_type = 0xE;
                    brk_rule = 3;
                }
            }
            if (check) {
                if (prev_pos == 0x1003 && since > 2) {
                    brk_type = 0xE;
                    brk_rule = 4;
                } else if ((pos == 0x1002 || pos == 0x4007) && since > 3 && prev_pos != 0x1004 && prev_pos != 0x4003) {
                    brk_type = 0xE;
                    brk_rule = 5;
                } else if (pos == 0x4005 && since > 4) {
                    brk_type = 0xE;
                    brk_rule = 6;
                } else if (since > 2 && (prev_pos == 0x1000 || prev_pos == 0x2000) && pos == 0x4001) {
                    brk_type = 0xF;
                    brk_rule = 7;
                } else {
                    int go = 0;
                    if (prev_pos == 0x1000) {
                        if (npos == 0x1004 || npos == 0x4003 || npos == 0x4004 || since < 4 || pos != 0x2000) go = 1;
                        else {
                            brk_type = 0xF;
                            brk_rule = 9;
                        }
                    } else if (prev_pos == 0x2000 || prev_pos == 0x3001 || prev_pos == 0x3002) {
                        go = 1;
                    }
                    if (go && pos != 0x1000 && pos != 0x2000 && pos != 0x3001 && pos != 0x3002 && i + 1 < l->n &&
                        l->w[i + 1].btype == 0) {
                        brk_type = 0x12;
                        brk_rule = 0xD;
                    }
                }
            }
        }
        cur = i;
        if (brk_type >= 0 && i > 0 && prev_btype == 0 && W->btype == 0) {
            int ofs = W->ofs, len = W->len;
            word *x = wl_insert(l, i);
            if (!x) return;
            x->btype = brk_type;
            x->ph[0] = SIL;
            x->nph = 1;
            x->ofs = ofs;
            x->len = len;
            strcpy(x->text, "+");
            x->x694 = 7;
            if (prev_idx >= 0) {
                word *pw = &l->w[prev_idx];
                pw->rule_a = brk_rule;
                pw->rule_b = brk_rule;
                pw->acc = 5;
                x->x680 = pw->x680;
                x->vol = pw->vol;
            }
            cur = i;
            i++;
        }
        question_type(&l->w[cur], cq);
        prev_idx = cur;
        prev_btype = l->w[cur].btype;
        if (since > 2) b_det = 0;
        if (pos == 0x4006) b_det = 1;
        prev_pos = pos;
        i++;
    }
    if (l->n) question_type(&l->w[l->n - 1], cq);
}

static void pass2(sam_tts *t, wordlist *l)
{
    int n = l->n, i = 0, k, has_emph = 0, start, cur_cls;
    word *first;
    int q_first;
    if (!n) return;
    while (i < n - 1 && is_silence_word(&l->w[i])) i++;
    first = &l->w[i];
    q_first = first->cls == 3;
    cur_cls = l->w[0].cls;
    start = i;
    for (k = i; k <= n; k++) {
        int end_run = k == n || (l->w[k].cls != cur_cls && !is_silence_word(&l->w[k]));
        if (k < n && l->w[k].emph > 0) has_emph = 1;
        if (end_run) {
            int a = start, b = k - 1;
            if ((cur_cls == 1 || cur_cls == 3) && b != a) {
                word *w = &l->w[b];
                if (w->acc == 0) {
                    w->acc = 2;
                    w->prom = 2;
                    w->rule_a = 0xF;
                }
            } else if (cur_cls == 2 || cur_cls == 0) {
                word *w = &l->w[a];
                if (w->acc == 0) {
                    w->acc = 1;
                    w->rule_a = 0x10;
                    w->prom = (int)(msvc_rand(t) % 5);
                }
            }
            if (k < n) {
                start = k;
                cur_cls = l->w[k].cls;
            }
        }
    }
    for (k = 0; k < n - 1; k++) {
        word *w = &l->w[k], *nx = &l->w[k + 1];
        int bt = nx->btype;
        if (bt == 0) continue;
        if (bt == 1) {
            if (w->cls == 2) {
                w->acc = 5;
                w->prom = 10;
                if (!w->rule_a) w->rule_a = 0x13;
            }
            w->bound = 0x3EB;
            w->bstr = 5;
            if (!w->rule_b) w->rule_b = 0x11;
        } else if (bt == 2 || (bt > 3 && bt < 6)) {
            if (w->cls == 2) {
                w->acc = 1;
                w->prom = 4;
                if (!w->rule_a) w->rule_a = 0x12;
            }
            w->bound = 0x3EA;
            w->bstr = 10;
            if (!w->rule_b) w->rule_b = 0x10;
        } else if (bt == 3) {
            w->acc = 2;
            w->prom = 10;
            w->bound = 0x3EC;
            w->bstr = 10;
            if (!w->rule_a) w->rule_a = 0x11;
            if (!w->rule_b) w->rule_b = 0xE;
            if (q_first) {
                first->acc = 1;
                first->prom = 5;
                first->rule_a = 0xE;
            }
        } else if (bt == 0x13) {
            w->bound = 0x3EB;
            w->bstr = 5;
            if (!w->rule_b) w->rule_b = 0x12;
        } else {
            if (w->cls == 2) {
                w->acc = 5;
                w->prom = 10;
                if (!w->rule_a) w->rule_a = nx->rule_a;
            }
            w->bound = 0x3EB;
            w->bstr = 5;
            if (!w->rule_b) w->rule_b = nx->rule_b;
        }
    }
    if (has_emph) {
        word *prev = NULL;
        for (k = 0; k < n; k++) {
            word *w = &l->w[k];
            if (w->emph < 1) {
                if (w->acc && w->prom > 5) w->prom = 5;
            } else {
                w->acc = 7;
                w->prom = 10;
                w->bound = 0;
                if (prev) prev->bound = 0;
            }
            prev = w;
        }
    }
}

/* ============================================================================================ */
/* items                                                                                        */

typedef struct {
    int type, flags, accent, prom, bound, bstr, semi, emph, silence_ms, pause_ms, btype, next_btype;
    float rate, rate2, off, rng;
    float base, dur, amp;
    int unit, vphone;
    int note;               /* sing mode: 1 + note index (0 = none) */
    int coda;               /* sing mode: sustained consonant after the vowel */
    int f_start, f_end;
    int wofs, wlen, wstart; /* source span of the word this sound belongs to; wstart = its first sound */
    float top, bot, t[20], f0[20];
} item;

typedef struct {
    item *it;
    int n, cap;
} itemlist;

static item *il_push(itemlist *l)
{
    item *x;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        item *it = realloc(l->it, sizeof(item) * (size_t)cap);
        if (!it) return NULL;
        l->it = it;
        l->cap = cap;
    }
    x = &l->it[l->n++];
    memset(x, 0, sizeof *x);
    x->type = SIL;
    x->rate = 1.0f;
    x->rate2 = 1.0f;
    x->rng = 1.0f;
    return x;
}

static int build_items(const wordlist *l, itemlist *out)
{
    int wi;
    float prev_rate = 1.0f;
    item *st = il_push(out);
    if (!st) return -1;
    st->flags |= 1;
    for (wi = 0; wi < l->n; wi++) {
        const word *w = &l->w[wi];
        const word *nx = wi + 1 < l->n ? &l->w[wi + 1] : NULL;
        int accent_idx = -1, anchor_idx = -1, found = 0, k, first = 1;
        int spoken = !is_silence_word(w); /* punctuation silences are not words */
        float wrate = w->rate;
        for (k = 0; k < w->nph; k++) {
            int ph = w->ph[k];
            if (ph >= 0 && ph < 0x2B) {
                if (!found && (PHONE_PROPS[ph] & 1)) {
                    if (k < w->nph - 1 && w->ph[k + 1] == ST1) {
                        found = 1;
                        accent_idx = k;
                    } else if (accent_idx < 0) {
                        accent_idx = k;
                    }
                }
                if (PHONE_PROPS[ph] & 4) anchor_idx = k;
            }
        }
        for (k = 0; k < w->nph; k++) {
            int ph = w->ph[k];
            item *it;
            if (ph < 0 || ph >= 0x2B) continue;
            if (ph == SIL && w->btype > 12) {
                if (wrate == 0.0f) wrate = prev_rate;
                continue;
            }
            it = il_push(out);
            if (!it) return -1;
            it->type = ph;
            it->note = w->note[k];
            it->wofs = w->ofs;
            it->wlen = w->len;
            if (first) {
                it->flags |= 1;
                it->wstart = spoken;
                first = 0;
            }
            if (k < w->nph - 1 && (w->ph[k + 1] == ST1 || ph == 0xD || ph == 0xB || ph == 0xA || ph == 0xC)) it->flags |= 0x40;
            if (k == accent_idx) it->accent = w->acc;
            it->prom = w->prom;
            if (k == anchor_idx) it->bound = w->bound;
            it->bstr = w->bstr;
            it->semi = w->semi;
            if (w->emph > 0 && k == accent_idx) {
                it->flags |= 0x40;
                it->emph = w->emph;
            }
            it->silence_ms = w->silence_ms;
            if (wrate == 0.0f) wrate = prev_rate;
            it->rate = wrate;
            it->rate2 = w->x680;
            it->next_btype = nx ? nx->btype : 0;
            it->off = w->off;
            it->rng = w->rng;
            it->btype = w->btype;
            if (w->btype != 0) it->flags |= 3;
        }
        if (w->pause > 0.0f) {
            item *it = il_push(out);
            if (!it) return -1;
            it->btype = w->btype;
            it->bstr = w->bstr;
            it->prom = w->prom;
            it->semi = w->semi;
            it->emph = w->emph;
            it->wofs = w->ofs;
            it->wlen = w->len;
            it->base = w->pause;
            it->pause_ms = (int)((double)w->pause * 1000.0);
            it->rate2 = 1.0f;
            it->rate = wrate;
        }
        prev_rate = wrate == 0.0f ? prev_rate : wrate;
    }
    st = il_push(out);
    if (!st) return -1;
    st->flags |= 3;
    st->btype = 0x14;
    return 0;
}

static int legal_onset(int c1, int c2)
{
    switch (c1) {
    case 0x1A: case 0x1B: case 0x22: case 0x23: return c2 >= 0x14 && c2 <= 0x15;
    case 0x1C: case 0x24: case 0x25: return c2 == 0x12 || c2 == 0x14;
    case 0x1E:
        return c2 == 0x12 || c2 == 0x15 || c2 == 0x17 || c2 == 0x18 || c2 == 0x1A || c2 == 0x22 || c2 == 0x24 || c2 == 0x26;
    case 0x20:
        return c2 == 0x12 || (c2 >= 0x14 && c2 <= 0x15) || (c2 >= 0x17 && c2 <= 0x18) || c2 == 0x22 || c2 == 0x24;
    case 0x26: case 0x27: return c2 == 0x12 || (c2 >= 0x14 && c2 <= 0x15);
    default: return 0;
    }
}

/* FUN_5ed5623e */
static void position_flags(itemlist *l)
{
    int n = l->n, i, j;
    item *it = l->it;
    for (i = 0; i < n; i++) {
        if (is_vowel(it[i].type)) {
            int local = 0;
            for (j = i - 1; j > 0; j--) {
                if ((it[j].flags & 0xC) > 3) break;
                if (is_vowel(it[j].type)) {
                    local = 0x30;
                    break;
                }
            }
            for (j = i + 1; j < n; j++) {
                if (it[j].flags & 3) {
                    it[i].flags |= local;
                    break;
                }
                if (is_vowel(it[j].type)) {
                    if (local == 0x30) local = 0x20;
                    else if (local == 0) local = 0x10;
                }
            }
        }
        for (j = i + 1; j < n; j++) {
            if (it[j].flags & 3) it[i].flags |= ((it[j].flags & 2) ? 0xC : 0) | ((it[j].flags & 1) ? 4 : 0);
            if (is_vowel(it[j].type)) break;
        }
    }
    { /* FUN_5ed55f70 syllable onsets (flag 0x400) */
        int mark = 0;
        i = 0;
        while (i < n) {
            int u, cnt, last, jj;
            while (i < n && it[i].type == SIL) {
                mark++;
                i++;
            }
            if (i >= n) break;
            if (!is_vowel(it[i].type)) {
                i++;
                continue;
            }
            it[mark].flags |= 0x400;
            u = it[i].flags & 0x30;
            if (u == 0 || u == 0x30) {
                jj = i + 1;
                while (jj < n && !(it[jj].flags & 3)) jj++;
                mark = i = jj;
                continue;
            }
            cnt = -1;
            jj = i;
            last = i;
            for (;;) {
                last = jj;
                jj++;
                cnt++;
                if (jj >= n || is_vowel(it[jj].type)) break;
            }
            mark = i = jj;
            if (cnt != 0 && jj < n) {
                mark = i = last;
                if (cnt == 2) {
                    if (legal_onset(it[jj - 2].type, it[jj - 1].type)) mark = i = last - 1;
                } else if (cnt == 3) {
                    if (legal_onset(it[jj - 2].type, it[jj - 1].type)) mark = i = it[jj - 3].type != 0x1E ? last - 1 : last - 2;
                } else if (cnt > 3) {
                    if (!legal_onset(it[jj - cnt + 1].type, it[jj - cnt].type)) mark = i = jj - (cnt >> 1);
                    else mark = i = jj + (2 - cnt);
                }
            }
        }
    }
}

/* ============================================================================================ */
/* unit selection (units_model.py) - works on the voice file sections                           */

static int32_t r32(const uint8_t *p) { return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); }
static int16_t r16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static float rf32(const uint8_t *p)
{
    uint32_t u = (uint32_t)r32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static int voice_phone(const sam_tts *t, int type, int stressed)
{
    uint32_t size;
    const uint8_t *s4 = sam_voice_section(t->voice, 4, &size);
    int cnt = (int)(size / 8);
    if (type < 0 || type >= cnt) return -1;
    return r16(s4 + 2 * (type * 2 + (stressed ? cnt * 2 : 0)));
}

static int phone_is_sil(const sam_tts *t, int ph)
{
    uint32_t size;
    const uint8_t *s0 = sam_voice_section(t->voice, 0, &size);
    const uint8_t *name = s0 + r32(s0 + r32(s0 + 0x1C) + 4 * ph);
    return name[0] == '+' || ((name[0] == 'S' || name[0] == 's') && (name[1] == 'I' || name[1] == 'i') && (name[2] == 'L' || name[2] == 'l'));
}

static int tree_leaf(const sam_tts *t, int phone, int left, int right, int posbits)
{
    uint32_t size;
    const uint8_t *s2 = sam_voice_section(t->voice, 2, &size);
    const uint8_t *root = s2 + 0x134 + phone * 8, *nodes, *node, *qbase;
    int lb, rb, half, qwords, guard = 0;
    if (r16(root) == 0) return -1;
    if (phone_is_sil(t, left)) left = r32(s2 + 0x28);
    if (phone_is_sil(t, right)) right = r32(s2 + 0x28);
    else if (r32(s2 + 0x2C) >= 0 && (posbits == 2 || posbits == 4)) right = r32(s2 + 0x2C);
    lb = r32(s2 + 0x1C) + 4 + left;
    rb = r32(s2 + 0x1C) + 4 + right;
    half = r32(s2 + 4);
    qbase = s2 + r32(s2 + 0x338);
    qwords = r32(s2 + 0x33C);
    nodes = s2 + r32(root + 4);
    node = nodes;
    while (guard++ < 10000) {
        int yes = r16(node + 4), nxt = r16(node + 6);
        const uint8_t *ql;
        if (yes < 0) return nxt;
        ql = s2 + r32(node);
        for (;;) {
            int q = (uint16_t)r16(ql);
            const uint8_t *qb;
            if (q == 0xFFFF) break;
            qb = qbase + (size_t)q * qwords * 4;
            if (((uint32_t)r32(qb) & (uint32_t)posbits) == (uint32_t)posbits &&
                (((uint32_t)r32(qb + 4 * (lb / 32)) >> (lb % 32)) & 1) &&
                (((uint32_t)r32(qb + 4 * (half + rb / 32)) >> (rb % 32)) & 1)) {
                nxt = yes;
                break;
            }
            ql += 2;
        }
        node = nodes + nxt * 10;
    }
    return -1;
}

static void select_units(const sam_tts *t, itemlist *l)
{
    uint32_t s0size, s1size, s2size;
    const uint8_t *s0 = sam_voice_section(t->voice, 0, &s0size);
    const uint8_t *s1 = sam_voice_section(t->voice, 1, &s1size);
    const uint8_t *s2 = sam_voice_section(t->voice, 2, &s2size);
    int sil = -1, i, n = l->n, base_sub = r32(s0 + 9 * 4), nph = r32(s0 + 0x24);
    for (i = 0; i < nph && sil < 0; i++) {
        const uint8_t *name = s0 + r32(s0 + r32(s0 + 0x1C) + 4 * i);
        if (!strcmp((const char *)name, "SIL")) sil = i;
    }
    (void)s0size;
    (void)s1size;
    (void)s2size;
    for (i = 0; i < n; i++) l->it[i].vphone = voice_phone(t, l->it[i].type, (l->it[i].flags & 0x40) != 0);
    for (i = 0; i < n; i++) {
        item *it = &l->it[i];
        int ph = it->vphone, left, right, nflags, posbits, leaf;
        const uint8_t *tab, *f;
        if (ph == sil || ph < 0) {
            it->unit = 0;
            it->base = 0.01f;
            it->amp = 1.0f;
            continue;
        }
        left = i > 0 ? l->it[i - 1].vphone : sil;
        if (i < n - 1) {
            right = l->it[i + 1].vphone;
            nflags = l->it[i + 1].flags & 1;
        } else {
            right = sil;
            nflags = 0;
        }
        if (it->flags & 1) posbits = nflags ? 4 : 1;
        else posbits = nflags ? 2 : 8;
        leaf = tree_leaf(t, ph, left, right, posbits);
        if (leaf < 0) {
            it->unit = 0;
            it->base = 0.01f;
            it->amp = 1.0f;
            continue;
        }
        it->unit = r32(s2 + 0x34 + ph * 4) - base_sub + 1 + leaf;
        tab = s1 + r32(s1 + ph * 4);
        f = tab + leaf * 16;
        it->base = (float)((double)rf32(f) * (double)0.001f);
        it->amp = (float)sqrt((double)rf32(f + 12));
    }
}

/* ============================================================================================ */
/* durations (FUN_5ed4387b)                                                                     */

static void durations(itemlist *l, double sapi_rate)
{
    int k, carry = 0;
    l->it[0].base = 0.01f;
    l->it[0].dur = 0.01f;
    for (k = 1; k < l->n; k++) {
        item *it = &l->it[k];
        double base = it->base, add = 0.0;
        double div = (double)(float)((double)(float)it->rate2 * (double)(float)it->rate * sapi_rate);
        int vowel = is_vowel(it->type);
        if (it->type == SIL) {
            if (it->silence_ms) {
                base = (double)(float)(it->silence_ms * (double)0.001f);
                div = 1.0;
            } else if (it->pause_ms) {
                base = (double)(float)(it->pause_ms * (double)0.001f);
            } else if (it->btype) {
                double b = it->btype < 21 ? (double)PAUSE_TABLE[it->btype] : (double)0.001f;
                base = b > 1.0 ? 1.0 : b;
            }
        } else {
            int fl = it->flags | (it->emph > 0 ? 0x100 : 0), p;
            if ((fl & 8) && (fl & 0x1C0) && vowel) {
                base = (double)(float)((double)FINAL_LENGTHEN[it->next_btype < 20 ? it->next_btype : 0] * base);
                if (base > 5.0) base = 5.0;
                else if (base < (double)0.011f) base = (double)0.011f;
            }
            p = carry;
            if (!(fl & 0x200) && vowel) p = 0;
            if (fl & 0x100) p = 1;
            if (p) {
                if (!vowel) {
                    double a = base * 1.25 - base;
                    add += a < (double)0.02f ? (double)0.02f : a;
                } else {
                    add += (double)0.06f;
                }
            }
            carry = p;
        }
        it->dur = (float)(((double)(float)base + add) / div);
    }
}

/* ============================================================================================ */
/* F0 (prosody_model.py)                                                                        */

static void interp_linear(float *c, int n)
{
    int i0 = 0, j, last = n - 1, i;
    double v0, vl;
    while (i0 < n && c[i0] == 0.0f) i0++;
    v0 = c[i0 < n ? i0 : n - 1];
    j = last;
    while (j >= 0 && c[j] == 0.0f) j--;
    vl = j >= 0 ? c[j] : 0.0;
    i = 0;
    while (i < last) {
        while (c[i] == 0.0f) {
            int k, gap_end, L, m;
            double prev, nv, d, acc;
            if (last <= i) return;
            k = i + 1;
            while (k < n && c[k] == 0.0f) k++;
            gap_end = k - 1;
            prev = i == 0 ? v0 : (double)c[i - 1];
            nv = gap_end == last ? vl : (double)c[gap_end + 1];
            d = nv - prev;
            L = gap_end - i;
            acc = prev;
            for (m = i; m <= gap_end; m++) {
                acc = acc + d / (L + 2);
                c[m] = (float)acc;
            }
            i = k;
            if (n <= k) return;
        }
        i++;
    }
}

static void ease_fill(float *c, int start, int length, double a, double b, int flag)
{
    double d = a - b;
    int half = length / 2, i;
    for (i = start; i < start + length; i++) {
        double x = (double)(i - start) / length;
        if (flag == 0) c[i] = (float)(i < start + half ? b + 2 * d * x * x : (d + b) - 2 * d * (1 - x) * (1 - x));
        else c[i] = (float)(i >= start + half ? b + 2 * d * (1 - x) * (1 - x) : (d + b) - 2 * d * x * x);
    }
}

static void smooth(float *c, int n)
{
    int i = 0, j, last = n - 1, k;
    double v0, vl, y;
    while (i < n && c[i] == 0.0f) i++;
    v0 = c[i < n ? i : n - 1];
    j = last;
    while (j >= 0 && c[j] == 0.0f) j--;
    vl = j >= 0 ? c[j] : 0.0;
    i = 0;
    while (i < last) {
        if (c[i] == 0.0f) {
            int gap_end;
            k = i + 1;
            while (k < n && c[k] == 0.0f) k++;
            gap_end = k - 1;
            {
                double prev = i == 0 ? v0 : (double)c[i - 1];
                double nxt = gap_end == last ? vl : (double)c[gap_end + 1];
                if (nxt - prev <= 0.0) ease_fill(c, i, gap_end - i + 1, prev, nxt, 1);
                else ease_fill(c, i, gap_end - i + 1, nxt, prev, 0);
            }
            i = gap_end + 1;
        } else {
            i++;
        }
    }
    if (n > 1) {
        y = c[0];
        for (k = 0; k < n; k++) {
            y = (double)0.9f * y + (double)c[k] * (double)0.1f;
            c[k] = (float)y;
        }
        memmove(c, c + 1, sizeof(float) * (size_t)(n - 1));
        c[n - 1] = c[n - 2];
    }
}

static void pitch_range(const item *it, double base, double prange, float *top, float *bot)
{
    static const float SEMI[25] = {1.0f, 1.0293022f, 1.0594631f, 1.0905077f, 1.122462f, 1.1553525f, 1.1892071f,
                                   1.2240535f, 1.2599211f, 1.2968396f, 1.3348398f, 1.3739537f, 1.4142135f, 1.4556532f,
                                   1.4983071f, 1.5422108f, 1.587401f, 1.6339157f, 1.6817929f, 1.7310687f, 1.7817974f,
                                   1.8340081f, 1.8877486f, 1.9430604f, 2.0f};
    int s = it->semi < -24 ? -24 : it->semi > 24 ? 24 : it->semi;
    float a = (float)(s >= 0 ? base * SEMI[s] : base / SEMI[-s]);
    double lg = log2((double)a * 0.9784975170625504);
    double center_ext = lg + (double)it->off;
    float center = (float)center_ext;
    float half = (float)(prange * (double)it->rng);
    *top = (float)(exp2((double)(float)(center_ext + half)) * 1.021975);
    *bot = (float)(exp2((double)(float)((double)center - half)) * 1.021975);
}

static int build_f0(itemlist *l, double base_pitch, double prange)
{
    int n_items = l->n, k, n, idx, first, counter, prev_start = 0, prev_end = 0, fk;
    float total = 0.0f, *top, *bot, *mid, *c;
    double s = 0.0;
    item *it = l->it;
    /* frame spans */
    first = 1;
    {
        int prev = 0;
        for (k = 0; k < n_items; k++) {
            int end;
            it[k].f_start = it[k].f_end = 0;
            if (first && it[k].type == SIL) continue;
            if (k == n_items - 1 && it[k].type == SIL) break;
            {
                /* x87: the sum is stored as a float but the unrounded value is scaled (fst; fmul) */
                double sum = (double)total + (double)it[k].dur;
                total = (float)sum;
                end = (int)(sum * 100.0);
            }
            it[k].f_start = prev;
            it[k].f_end = end;
            prev = end;
            first = 0;
        }
    }
    for (k = 0; k < n_items; k++) pitch_range(&it[k], base_pitch, prange, &it[k].top, &it[k].bot);
    n = (int)((double)total * 100.0);
    if (n < 2) n = 2;
    top = calloc((size_t)n, sizeof(float));
    bot = calloc((size_t)n, sizeof(float));
    mid = calloc((size_t)n, sizeof(float));
    c = calloc((size_t)n + 1, sizeof(float));
    if (!top || !bot || !mid || !c) {
        free(top); free(bot); free(mid); free(c);
        return -1;
    }
    for (k = 0; k < n; k++) bot[k] = 1e-5f;
    top[0] = 1.0f;
    top[n - 1] = 0.7f; /* sentence type 1 (the engine always uses it) */
    interp_linear(top, n);
    mid[0] = (float)(((double)top[0] - bot[0]) * (double)0.33f + bot[0]);
    mid[n - 1] = (float)(((double)top[n - 1] - bot[n - 1]) * (double)0.33f + bot[0]);
    interp_linear(mid, n);
    c[0] = mid[0];
    c[n - 1] = 1e-4f;
#define T(f) ((double)top[f])
#define M(f) ((double)mid[f])
#define B(f) ((double)bot[f])
#define CLAMPF(f) ((f) < 0 ? 0 : (f) >= n ? n - 1 : (f))
    idx = 0;
    while (idx < n_items && it[idx].type == SIL) idx++;
    first = 1;
    counter = 1;
    while (idx < n_items - 1) {
        item *x = &it[idx];
        int start = CLAMPF(x->f_start), end = CLAMPF(x->f_end), nstart = it[idx + 1].f_start, nend = CLAMPF(it[idx + 1].f_end);
        double ln = (double)(float)(x->f_end - x->f_start), p = (double)x->prom * (double)0.1f, q;
        int f;
        switch (x->accent) {
        case 1:
            if (!first && prev_start != 0) {
                f = CLAMPF((int)(ln * (double)0.1f + start));
                c[prev_start] = (float)((T(f) - M(f)) * p * 0.25 + M(f));
            }
            f = x->bound != 0 ? start : CLAMPF((int)(ln * 0.5 + start));
            c[f] = (float)((T(f) - M(f)) * p + M(f));
            break;
        case 2:
            f = CLAMPF((int)(ln * (double)0.3f + start));
            c[f] = (float)(M(f) - (M(f) - B(f)) * p);
            break;
        case 3:
            c[start] = (float)(M(start) - (M(start) - B(start)) * p);
            if (nstart != 0) c[nend] = (float)(M(nend) - (M(nend) - B(nend)) * p);
            s = 0.0;
            break;
        case 5:
            f = CLAMPF((int)(ln * (double)0.3f + start));
            if (prev_start != 0) c[prev_start] = (float)(M(prev_start) - (M(prev_start) - B(prev_start)) * (p * (double)0.3f));
            c[f] = (float)((T(f) - M(f)) * p + M(f));
            s = p;
            break;
        case 6:
            f = start;
            if (s == 0.0) {
                c[f] = (float)((T(f) - M(f)) * p + M(f));
                s = p;
            } else {
                s = (double)(float)(s * 0.5);
                c[f] = (float)((T(f) - M(f)) * s + M(f));
            }
            break;
        case 7:
            c[start] = (float)((T(0) - M(0)) * p + M(0));
            f = CLAMPF((int)(ln * 0.75 + start));
            c[f] = (float)(M(f) - (M(f) - B(f)) * p);
            s = p;
            break;
        default:
            break;
        }
        q = (double)x->bstr * (double)0.1f;
        switch (x->bound) {
        case 1000: c[end] = (float)(M(end) - (M(end) - B(end)) * q); break;
        case 1001: c[end] = (float)((T(end) - M(end)) * q + M(end)); break;
        case 1002: c[start] = bot[end]; break;
        case 1003: {
            int mf = CLAMPF(prev_start + (prev_end - prev_start) / 2);
            c[mf] = (float)(M(mf) - (M(mf) - B(mf)) * q);
            c[end] = (float)((T(end) - M(end)) * q + M(end));
            break;
        }
        case 1004: c[end] = top[end]; break;
        case 1005:
            c[start] = (float)((T(start) - M(start)) * q + M(start));
            c[end] = bot[end];
            break;
        default: break;
        }
        if (first && (x->flags & 1)) {
            counter--;
            if (counter < 0) first = 0;
        }
        prev_start = start;
        prev_end = end;
        idx++;
    }
#undef T
#undef M
#undef B
    smooth(c, n);
    /* knots */
    first = 1;
    for (k = 0; k < n_items; k++) {
        item *x = &it[k];
        double step = (double)x->dur * (double)0.05f;
        float flat = (float)(((double)x->top - x->bot) * 0.5 + x->bot);
        if (k < n_items - 1 && !(first && x->type == SIL)) {
            double R = (double)x->top - (double)x->bot, kk = 0.0;
            for (fk = 0; fk < 20; fk++) {
                int fr = CLAMPF((int)((x->f_end - x->f_start) * kk) + x->f_start);
                x->f0[fk] = (float)(R * (double)c[fr] + x->bot);
                x->t[fk] = (float)(kk * (double)x->dur);
                kk += (double)0.05f;
            }
            first = 0;
        } else {
            for (fk = 0; fk < 20; fk++) {
                x->f0[fk] = flat;
                x->t[fk] = (float)(step * fk);
            }
        }
    }
#undef CLAMPF
    free(top);
    free(bot);
    free(mid);
    free(c);
    return 0;
}

/* ============================================================================================ */
/* word records (FUN_5ed45c97)                                                                  */

/* word record for a tagged entry (FUN_5ed45c97, action "speak") */
static int add_word(sam_tts *t, wordlist *l, const sam_tag_entry *x, int ofs, int len)
{
    word *w;
    int k, n = x->npron[x->idx];
    const uint16_t *pr = x->pron[x->idx];
    w = wl_insert(l, l->n);
    if (!w) return -1;
    /* the engine copies at most 19 characters; entries fixed by special-word rules read "*phones*" */
    snprintf(w->text, 20, "%s", x->star ? "*" : x->text);
    w->pos = x->pos;
    w->cls = pos_class(w->pos);
    w->rate = 1.0f;
    w->ofs = ofs;
    w->len = len;
    w->off = t->st_off;
    w->rng = t->st_rng;
    w->x680 = t->st_rate;
    for (k = 0; k < n && w->nph < MAX_WORD_PH; k++) {
        int ph = sapi_to_internal(pr[k]);
        if (ph >= 0) w->ph[w->nph++] = ph;
    }
    return 0;
}

/* FUN_5ed44997: explicit silence carrying the current quote / parenthesis state */
static int add_silence(sam_tts *t, wordlist *l, int ms, int ofs, int len, int x694)
{
    word *w = wl_insert(l, l->n);
    if (!w) return -1;
    if (ms > 0) w->silence_ms = ms;
    w->ph[0] = SIL;
    w->nph = 1;
    w->ofs = ofs;
    w->len = len;
    w->off = t->st_off;
    w->rng = t->st_rng;
    w->x680 = t->st_rate;
    w->x694 = x694;
    return 0;
}

/* ============================================================================================ */
/* number prosody (FUN_5ed45ac0 and helpers): positions are indices into the sentence word list, */
/* -1 standing for the engine's NULL list node                                                  */

static int nx(const wordlist *l, int i) { return i >= 0 && i + 1 < l->n ? i + 1 : -1; }

static void unstress(word *w) /* "if (acc == 0) { acc = 0; prom = 5; }" */
{
    if (w->acc == 0) {
        w->acc = 0;
        w->prom = 5;
    }
}

static void accent(sam_tts *t, word *w, int rule)
{
    w->acc = 1;
    w->prom = (int)(msvc_rand(t) % 4) + 4;
    w->rule_a = rule;
}

/* FUN_5ed44a0c (after i; at the end when i < 0) / FUN_5ed44f1c (before i): a short silence */
static int insert_sil(sam_tts *t, wordlist *l, int at)
{
    word *w = wl_insert(l, at);
    if (!w) return -1;
    w->text[0] = '+';
    w->text[1] = 0;
    w->ph[0] = SIL;
    w->nph = 1;
    w->off = t->st_off;
    w->rng = t->st_rng;
    w->x680 = t->st_rate;
    return at;
}

static int sil_after(sam_tts *t, wordlist *l, int i) { return insert_sil(t, l, i < 0 ? l->n : i + 1); }
static int sil_before(sam_tts *t, wordlist *l, int i) { return insert_sil(t, l, i < 0 ? 0 : i); }

/* FUN_5ed443e6: digits read one by one, every second one accented */
static int digits_prosody(sam_tts *t, wordlist *l, int pos, int n)
{
    for (; n > 1; n -= 2) {
        int nxt = -1;
        if (pos >= 0) {
            nxt = nx(l, pos);
            accent(t, &l->w[pos], 0x15);
        }
        pos = nxt >= 0 ? nx(l, nxt) : -1;
    }
    if (n > 0 && pos >= 0) pos = nx(l, pos);
    return pos;
}

/* FUN_5ed44ae2: groups of an integer: the first word of each group accented, a pause after scale words */
static int int_prosody(sam_tts *t, wordlist *l, int *ppos, const numinfo *ni, int cnt)
{
    const intpart *ip = ni->ip;
    int cur, nxt, k;
    if (ip->v[0x1a]) {
        *ppos = digits_prosody(t, l, *ppos, ip->v[0x1b]);
        return cnt - ip->v[0x1b];
    }
    cur = *ppos;
    if (cur < 0) return cnt;
    nxt = nx(l, cur);
    l->w[cur].acc = 0;
    l->w[cur].prom = 5;
    if (ni->neg) {
        if (nxt >= 0) {
            cur = nxt;
            nxt = nx(l, cur);
            l->w[cur].acc = 0;
            l->w[cur].prom = 5;
        }
        cnt--;
    }
#define ADVANCE()                                                                                             \
    do {                                                                                                      \
        if (nxt >= 0) {                                                                                       \
            cur = nxt;                                                                                        \
            nxt = nx(l, cur);                                                                                 \
            unstress(&l->w[cur]);                                                                             \
        }                                                                                                     \
    } while (0)
    for (k = ip->v[0] - 1; k >= 0; k--) {
        const int *f = &ip->v[4 * k + 1]; /* ones, tens, hundred, scale word */
        accent(t, &l->w[cur], 0x14);
        if (f[2]) {
            ADVANCE();
            ADVANCE();
            cnt -= 2;
        }
        if (f[1]) {
            ADVANCE();
            cnt--;
        }
        if (f[0]) {
            ADVANCE();
            cnt--;
        }
        if (f[3]) {
            int s = sil_after(t, l, cur);
            if (s < 0) break;
            l->w[s].x694 = 0x11;
            l->w[s].btype = 0x13;
            l->w[s].rule_b = 0x13;
            unstress(&l->w[cur]);
            cur = s;
            nxt = nx(l, s);
            if (nxt >= 0) {
                cur = nxt;
                nxt = nx(l, cur);
            }
            cnt--;
        }
    }
#undef ADVANCE
    *ppos = cur;
    return cnt;
}

/* FUN_5ed450e4: pause in a phone number, then on to the next word that is not a silence */
static int pause_before(sam_tts *t, wordlist *l, int pos, int rule_b, int x694)
{
    int s = sil_before(t, l, pos), cur;
    if (s < 0) return pos;
    l->w[s].x694 = x694;
    l->w[s].btype = 10;
    l->w[s].rule_b = rule_b;
    l->w[s].rule_a = 0x20;
    cur = s;
    while (l->w[cur].nph > 0 && l->w[cur].ph[0] == SIL && nx(l, cur) >= 0) cur = nx(l, cur);
    return cur;
}

/* FUN_5ed453a1: fractions */
static int frac_prosody(sam_tts *t, wordlist *l, int *ppos, const numinfo *ni, int cnt)
{
    const fracpart *fr = ni->fr;
    int s;
    if (fr->num->ip) cnt = int_prosody(t, l, ppos, fr->num, cnt);
    if (fr->num->dp) {
        *ppos = nx(l, *ppos);
        *ppos = digits_prosody(t, l, *ppos, fr->num->dp->n);
    }
    if (fr->over == 0) *ppos = *ppos < 0 ? l->n - 1 : *ppos - 1;
    s = sil_before(t, l, *ppos);
    if (s >= 0) {
        l->w[s].x694 = 0xf;
        l->w[s].btype = 0x13;
        l->w[s].rule_b = 0x15;
        *ppos = nx(l, s);
    }
    if (fr->den->ip) {
        int node = *ppos;
        if (node >= 0) {
            *ppos = nx(l, node);
            if (l->w[node].acc == 0) {
                l->w[node].acc = 0;
                l->w[node].prom = 5;
                l->w[node].pos = 0x1000;
                l->w[node].cls = 2;
            }
        }
        cnt = int_prosody(t, l, ppos, fr->den, cnt);
    }
    if (fr->den->dp) {
        *ppos = nx(l, *ppos);
        *ppos = digits_prosody(t, l, *ppos, fr->den->dp->n);
    }
    return cnt;
}

/* FUN_5ed4526a: currencies */
static void currency_prosody(sam_tts *t, wordlist *l, int pos, const currinfo *ci, int count)
{
    int cnt, p2 = pos, nxt, rec = -1, s;
    if (ci->num->type != 0x1006) return;
    cnt = int_prosody(t, l, &p2, ci->num, count);
    if (cnt <= 1) return;
    nxt = p2;
    if (ci->scale) {
        if (p2 >= 0) {
            rec = p2;
            nxt = nx(l, p2);
        }
        cnt--;
    }
    if (cnt <= 1) return;
    s = sil_after(t, l, p2);
    if (s >= 0) {
        l->w[s].x694 = 0x10;
        l->w[s].btype = 0x13;
        l->w[s].rule_b = 0x14;
        rec = s;
        nxt = nx(l, s);
        p2 = s;
    }
    if (ci->cents) {
        int q = nxt;
        if (nxt >= 0) {
            rec = nxt;
            q = nx(l, nxt);
            p2 = nxt;
        }
        if (q >= 0) {
            p2 = q;
            if (rec >= 0 && l->w[rec].acc == 0) {
                l->w[rec].acc = 0;
                l->w[rec].prom = 5;
                l->w[rec].pos = 0x1000;
                l->w[rec].cls = 2;
            }
        }
        if (p2 >= 0) int_prosody(t, l, &p2, ci->cents, cnt - 2);
    }
}

/* FUN_5ed44f8a: clock times */
static void time_prosody(sam_tts *t, wordlist *l, int pos, const timeinfo *ti)
{
    int s, nxt;
    if (ti->hundred || pos < 0) return;
    accent(t, &l->w[pos], 0x1d);
    nxt = nx(l, pos);
    s = sil_after(t, l, pos);
    if (s >= 0) {
        l->w[s].x694 = 9;
        l->w[s].btype = 0x13;
        l->w[s].rule_b = 0x1a;
        nxt = nx(l, s);
    }
    if (ti->minutes && nxt >= 0) accent(t, &l->w[nxt], 0x1e);
    if (ti->ampm) {
        int s2 = sil_before(t, l, l->n - 2);
        if (s2 >= 0) {
            l->w[s2].x694 = 10;
            l->w[s2].btype = 0xb;
            l->w[s2].rule_b = 0x1b;
        }
        accent(t, &l->w[l->n - 1], 0x1f);
    }
}

/* FUN_5ed45177: phone numbers */
static void phone_prosody(sam_tts *t, wordlist *l, int pos, const phoneinfo *pi, int count)
{
    int g, s;
    if (pi->country) {
        pos = nx(l, nx(l, pos));
        int_prosody(t, l, &pos, pi->country, count);
        pos = pause_before(t, l, pos, 0x16, 0xb);
    }
    if (pi->one) {
        pos = nx(l, pos);
        pos = pause_before(t, l, pos, 0x18, 0xd);
    }
    if (pi->area) {
        if (!pi->is800 || pos < 0) {
            pos = nx(l, nx(l, pos));
            pos = digits_prosody(t, l, pos, pi->area->n);
        } else {
            pos = nx(l, nx(l, pos));
        }
        if (pos >= 0) pos = pause_before(t, l, pos, 0x17, 0xc);
    }
    for (g = 0; g < pi->ngroups; g++) {
        pos = digits_prosody(t, l, pos, pi->groups[g].n);
        if (pos >= 0) pos = pause_before(t, l, pos, 0x19, 0xe);
    }
    s = sil_after(t, l, l->n - 1); /* FUN_5ed44a7a */
    if (s >= 0) {
        l->w[s].x694 = 0xe;
        l->w[s].btype = 10;
        l->w[s].rule_b = 0x19;
        l->w[s].rule_a = 0x20;
    }
}

/* FUN_5ed45ac0: prosody of a token after its words were added (first = its first word) */
static void node_prosody(sam_tts *t, wordlist *l, int first, const sam_node *n)
{
    const int *ti = n->ti;
    int type = ti ? *ti : n->type, pos = first;
    if (type == 0x10) { /* ellipsis */
        int s;
        if (add_silence(t, l, 0, n->ofs, n->len, 0x14)) return;
        s = l->n - 1;
        l->w[s].btype = 0xc;
        l->w[s].rule_b = 0x1c;
        return;
    }
    if (!ti || pos < 0) return;
    switch (type) {
    case 0x1006: case 0x1007: case 0x1008: case 0x100f: {
        const numinfo *ni = (const numinfo *)ti;
        if (ni->ip) int_prosody(t, l, &pos, ni, n->count);
        if (ni->dp) {
            pos = nx(l, pos);
            pos = digits_prosody(t, l, pos, ni->dp->n);
        }
        if (!ni->fr) return;
        if (pos >= 0) {
            unstress(&l->w[pos]);
            if (l->w[pos].acc == 0) {
                l->w[pos].pos = 0x1000;
                l->w[pos].cls = 2;
            }
            pos = nx(l, pos);
        }
        frac_prosody(t, l, &pos, ni, n->count);
        return;
    }
    case 0x100e:
        frac_prosody(t, l, &pos, (const numinfo *)ti, n->count);
        return;
    case 0x100d:
        currency_prosody(t, l, pos, (const currinfo *)ti, n->count);
        return;
    case 0x1018:
        time_prosody(t, l, pos, (const timeinfo *)ti);
        return;
    case 0x1027:
        phone_prosody(t, l, pos, (const phoneinfo *)ti, n->count);
        return;
    default:
        return;
    }
}

/* FUN_5ed44e69 / 44eca / 44dd8: parenthesis and quote state; returns 1 when the mark was taken */
static int paren_open(sam_tts *t, wordlist *l, const sam_node *n, int ofs, int sil)
{
    if (t->in_paren || t->in_quote) return 0;
    t->st_off = -0.2f;
    t->st_rng = 0.75f;
    t->in_paren = 1;
    t->st_rate = 1.25f;
    if (sil && add_silence(t, l, 100, ofs, (int)strlen(n->text), 4)) return -1;
    return 1;
}

static int paren_close(sam_tts *t, wordlist *l, const sam_node *n, int ofs, int sil)
{
    if (!t->in_paren) return 0;
    t->st_off = 0.0f;
    t->in_paren = 0;
    t->st_rng = 1.0f;
    t->st_rate = 1.0f;
    if (sil && add_silence(t, l, 100, ofs, (int)strlen(n->text), 4)) return -1;
    return 1;
}

static int quote_mark(sam_tts *t, wordlist *l, const sam_node *n, int ofs, int sil)
{
    if (t->in_paren) return 0;
    if (!t->in_quote) {
        t->in_quote = 1;
        t->st_off = 0.1f;
        t->st_rng = 1.25f;
        if (sil && add_silence(t, l, 100, ofs, (int)strlen(n->text), 2)) return -1;
    } else {
        t->st_off = 0.0f;
        t->in_quote = 0;
        t->st_rng = 1.0f;
        if (sil && add_silence(t, l, 100, ofs, (int)strlen(n->text), 3)) return -1;
    }
    return 1;
}

/* Speaks one sentence (a word list ending in punctuation). */
static int speak_sentence(sam_tts *t, wordlist *l, sam_pcm_cb cb, void *user);
static void hold(const int16_t *pcm, size_t n, void *user);
static void release(sam_tts *t, size_t keep);

/* FUN_5ed45c97: tagged nodes -> word records */
static int build_words(sam_tts *t, const sam_sentence *b, wordlist *l)
{
    int i, k, count = 0, last_sil = 0, prev_punct = 1, final_bt = 1;
    for (i = 0; i < b->nn; i++) {
        const sam_node *n = &b->nodes[i];
        int is_last = i == b->nn - 1 && i > 0, ty = n->type; /* local_5 is set after the node before the last */
        if (!(ty & 0x1000)) {
            if (ty >= 9 && ty <= 14) {
                int bt = ty == 9 ? 5 : ty == 10 ? 2 : ty == 11 ? 3 : 1;
                if (ty <= 11 && !is_last) {
                    final_bt = bt;
                    continue;
                }
                if (ty >= 12 && prev_punct) continue;
                if (count > 0) {
                    word *w = wl_insert(l, l->n);
                    if (!w) return -1;
                    w->btype = bt;
                    w->ph[0] = SIL;
                    w->nph = 1;
                    w->ofs = n->ofs;
                    w->len = n->len;
                    w->text[0] = n->text[0];
                    w->x694 = 1;
                    count++;
                    last_sil = 1;
                    prev_punct = 1;
                }
            } else if (ty >= 1 && ty <= 8 && ty != 7) {
                int sil, r;
                if (ty <= 3) {
                    sil = !is_last;
                    r = paren_open(t, l, n, n->ofs, sil);
                } else if (ty <= 6) {
                    sil = !is_last;
                    r = paren_close(t, l, n, n->ofs, sil);
                } else {
                    sil = !prev_punct && !is_last;
                    r = quote_mark(t, l, n, n->ofs, sil);
                }
                if (r < 0) return -1;
                if (r) {
                    if (sil) count++;
                    prev_punct = 1;
                    last_sil = 0;
                }
            } else if (ty == 0x10) {
                node_prosody(t, l, -1, n);
            }
            continue;
        }
        {
            int first = l->n;
            for (k = 0; k < n->count; k++) {
                if (add_word(t, l, &b->e[n->first + k], n->ofs, n->len)) return -1;
                count++;
                prev_punct = 0;
                last_sil = 0;
            }
            node_prosody(t, l, n->count ? first : -1, n);
        }
    }
    if (!last_sil) {
        word *w = wl_insert(l, l->n);
        if (!w) return -1;
        w->btype = final_bt;
        w->rule_b = 0x1d;
        w->x694 = 1;
        w->ph[0] = SIL;
        w->nph = 1;
        w->text[0] = '.';
        w->len = 1;
        w->ofs = b->end_ofs;
    }
    return 0;
}

/* Turns a tagged sentence into the word list and speaks it. */
static int finish_sentence(sam_tts *t, const sam_sentence *b, sam_pcm_cb cb, void *user)
{
    wordlist l = {0};
    int i, k, rc = 0;
    if (getenv("SAM_NODES")) {
        fprintf(stderr, "SENT\n");
        for (i = 0; i < b->nn; i++) {
            const sam_node *n = &b->nodes[i];
            fprintf(stderr, "N %x %s\n", (unsigned)n->type, n->text);
            for (k = 0; k < n->count; k++) {
                const sam_tag_entry *x = &b->e[n->first + k];
                int j;
                fprintf(stderr, "E %x %s\t", (unsigned)x->pos, x->text);
                for (j = 0; j < x->npron[x->idx]; j++) fprintf(stderr, "%d ", x->pron[x->idx][j]);
                fprintf(stderr, "\n");
            }
        }
    }
    { /* span of this sentence in the input text, for sam_mark */
        int lo = b->end_ofs, hi = 0;
        for (i = 0; i < b->nn; i++) {
            const sam_node *n = &b->nodes[i];
            if (n->len <= 0) continue;
            if (n->ofs < lo) lo = n->ofs;
            if (n->ofs + n->len > hi) hi = n->ofs + n->len;
        }
        t->sent_pos = hi > lo ? lo : 0;
        t->sent_len = hi > lo ? hi - lo : 0;
    }
    rc = build_words(t, b, &l);
    if (rc == 0 && l.n > 0) rc = speak_sentence(t, &l, cb, user);
    free(l.w);
    return rc;
}

/* The engine writes audio in chunks of about 5000 samples; when a later sentence fails (a word it
 * cannot pronounce) the unwritten part of the current chunk is lost. Samples are therefore held
 * back until their chunk is complete. */
static void hold(const int16_t *pcm, size_t n, void *user)
{
    sam_tts *t = user;
    if (t->npend + n > t->pcap) {
        size_t cap = (t->npend + n) * 2 + 1024;
        int16_t *p = realloc(t->pend, sizeof(int16_t) * cap);
        if (!p) return;
        t->pend = p;
        t->pcap = cap;
    }
    memcpy(t->pend + t->npend, pcm, sizeof(int16_t) * n);
    t->npend += n;
    t->ev_total += (long long)n;
}

static void release(sam_tts *t, size_t keep)
{
    size_t out = t->npend > keep ? t->npend - keep : 0;
    if (!out) return;
    t->out_cb(t->pend, out, t->out_user);
    memmove(t->pend, t->pend + out, sizeof(int16_t) * (t->npend - out));
    t->npend -= out;
}

int sam_tts_speak_pcm(sam_tts *t, const char *text, sam_pcm_cb cb, void *user)
{
    return sam_tts_speak_ex(t, text, NULL, cb, user);
}

int sam_tts_speak_ex(sam_tts *t, const char *text, const sam_speak_opts *opts, sam_pcm_cb cb, void *user)
{
    sam_norm *nm = sam_norm_new(text);
    sam_sentence s = {0};
    int rc = 0, k = 0;
    if (!nm) return -1;
    t->out_cb = cb;
    t->out_user = user;
    t->npend = 0;
    t->opts = opts;
    t->ev_total = 0;
    sam_synth_chunk_reset(t->synth);
    while (rc == 0 && (k = sam_norm_next(nm, t->lex, t->lts, &s)) > 0) {
        if (opts && opts->cancel && *opts->cancel) {
            rc = 1;
            break;
        }
        rc = finish_sentence(t, &s, cb, user);
    }
    if (k < 0) rc = -1;
    if (rc == 0) release(t, 0);
    t->npend = 0;
    t->opts = NULL;
    sam_sentence_free(&s);
    sam_norm_free(nm);
    return rc;
}

static int speak_sentence(sam_tts *t, wordlist *l, sam_pcm_cb cb, void *user)
{
    itemlist il = {0};
    const sam_speak_opts *o = t->opts;
    int k, rc = 0, sent_done = 0;
    (void)cb; /* audio goes through hold()/release() */
    (void)user;
    if (getenv("SAM_DEBUG")) {
        for (k = 0; k < l->n; k++) {
            int j;
            fprintf(stderr, "W %s\t%x\t%d\t%d\t%d\t", l->w[k].text, (unsigned)l->w[k].pos, l->w[k].cls, l->w[k].btype, l->w[k].x694);
            for (j = 0; j < l->w[k].nph; j++) fprintf(stderr, "%d ", l->w[k].ph[j]);
            fprintf(stderr, "\n");
        }
    }
    pass0(l);
    pass1(l);
    pass2(t, l);
    if (build_items(l, &il) != 0) return -1;
    position_flags(&il);
    select_units(t, &il);
    if (o && o->pitch_offset != 0.0f)
        for (k = 0; k < il.n; k++) il.it[k].off += o->pitch_offset;
    durations(&il, o && o->sapi_rate > 0.0 ? o->sapi_rate : 1.0);
    if (build_f0(&il, (double)t->base_pitch, (double)0.4f) != 0) {
        free(il.it);
        return -1;
    }
    for (k = 0; k < il.n && rc == 0; k++) {
        const item *x = &il.it[k];
        sam_segment g;
        int j;
        if (o && o->cancel && *o->cancel) {
            rc = 1;
            break;
        }
        if (o && o->mark_cb && x->wstart) { /* just before the audio of this word */
            sam_mark m;
            m.flags = sent_done ? 1 : 3;
            m.word_pos = x->wofs;
            m.word_len = x->wlen;
            m.sent_pos = t->sent_pos;
            m.sent_len = t->sent_len;
            m.audio_pos = t->ev_total;
            sent_done = 1;
            o->mark_cb(o->user, &m);
        }
        g.hold = 0;
        g.unit = x->unit;
        g.dur = x->dur;
        if (getenv("SAM_DEBUG")) {
            fprintf(stderr, "S %d %.6f", x->unit, x->dur);
            for (j = 0; j < 20; j++) fprintf(stderr, " %.9g", x->f0[j]);
            fprintf(stderr, "\n");
        }
        g.n_knots = 20;
        for (j = 0; j < 20; j++) {
            g.t[j] = (float)((double)x->t[j] * 22050.0);
            /* ClassicVoices patch: a monotone voice still follows the host's pitch shift (log2 offset) */
            g.f0[j] = t->monotone ? (float)((double)t->base_pitch *
                                            (o && o->pitch_offset != 0.0f ? pow(2.0, (double)o->pitch_offset) : 1.0))
                                  : x->f0[j];
            g.amp[j] = x->amp;
        }
        rc = sam_synth_segment(t->synth, &g, hold, t);
        if (rc == 0) release(t, (size_t)sam_synth_chunk_pos(t->synth));
    }
    free(il.it);
    return rc;
}


/* ============================================================================================ */
/* sing mode (not part of the original engine)                                                  */

#define SING_MAX_NOTES 2048

/* "C4", "F#3", "Bb5", or a MIDI number; returns Hz, or 0 */
static double note_hz(const char *s)
{
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7}; /* A B C D E F G */
    int midi;
    char c = (char)toupper((unsigned char)s[0]);
    if (isdigit((unsigned char)s[0])) {
        midi = atoi(s);
    } else if (c >= 'A' && c <= 'G') {
        int semi = base[c - 'A'], i = 1;
        if (s[i] == '#') {
            semi++;
            i++;
        } else if (s[i] == 'b') {
            semi--;
            i++;
        }
        if (!isdigit((unsigned char)s[i]) && s[i] != '-') return 0.0;
        midi = (atoi(s + i) + 1) * 12 + semi;
    } else {
        return 0.0;
    }
    return 440.0 * pow(2.0, (midi - 69) / 12.0);
}

typedef struct {
    double hz, sec; /* hz 0 = rest */
} sing_note;

/* r, l, m, n, ng: consonants a singer can hold (SAPI ids 38, 31, 32, 33, 34) */
static int is_sonorant(int ph)
{
    return ph == sapi_to_internal(38) || ph == sapi_to_internal(31) || ph == sapi_to_internal(32) ||
           ph == sapi_to_internal(33) || ph == sapi_to_internal(34);
}

/* ClassicVoices patch: the syllables of a word's pronunciation (internal phone ids, with SYL / stress marks).
 * The count is the number of vowel nuclei; when the lexicon's syllable marks agree with it they place the
 * boundaries, otherwise (a missing mark: "stylish" = s t ay 1 l ih sh, or "rofl" = r aa f l) the boundaries
 * go between the vowels: of the consonants between two vowels the last starts the next syllable and the rest
 * close the previous one (never an onset English cannot start with). A word with no vowel is one syllable.
 * syl[j] = syllable of phone j (marks follow the phone before them); stress[i] = 1 / 2 for a primary /
 * secondary stress mark in syllable i (cap entries). Returns the syllable count. */
static int syllabify(const int *ph, int n, int *syl, int *stress, int cap)
{
    int j, nv = 0, nmark = 0, real = 0, cur = 0, last_real = -1;
    for (j = 0; j < n; j++) {
        if (ph[j] == SYL) nmark++;
        else if (ph[j] >= 0 && ph[j] < 43) {
            real++;
            if (is_vowel(ph[j])) nv++;
        }
    }
    for (j = 0; j < cap; j++) stress[j] = 0;
    if (!real) {
        for (j = 0; j < n; j++) syl[j] = 0;
        return 1;
    }
    if (nv == 0) {
        for (j = 0; j < n; j++) syl[j] = 0;
    } else if (nmark + 1 == nv) {
        for (j = 0; j < n; j++) {
            if (ph[j] == SYL) cur++;
            syl[j] = cur;
        }
    } else {
        int k = 0; /* vowels seen */
        for (j = 0; j < n; j++) {
            if (ph[j] < 0 || ph[j] >= 43) { /* a mark: with the phone before it */
                syl[j] = last_real >= 0 ? syl[last_real] : 0;
                continue;
            }
            if (is_vowel(ph[j])) {
                syl[j] = k++;
            } else if (k == 0 || k == nv) {
                syl[j] = k == 0 ? 0 : nv - 1;
            } else { /* between vowel k-1 and vowel k: is another consonant still to come before it? */
                int m, more = 0;
                for (m = j + 1; m < n && !(ph[m] >= 0 && ph[m] < 43 && is_vowel(ph[m])); m++)
                    if (ph[m] >= 0 && ph[m] < 43) more = 1;
                syl[j] = more ? k - 1 : k;
            }
            last_real = j;
        }
        if (nv < 1) nv = 1;
    }
    {
        int ns = nv ? nv : 1;
        for (j = 0; j < n; j++)
            if ((ph[j] == ST1 || ph[j] == ST2) && syl[j] < cap && syl[j] < ns && !stress[syl[j]])
                stress[syl[j]] = ph[j] == ST1 ? 1 : 2;
        return ns;
    }
}

/* The shared back half of the sing calls: a word list whose phones carry note numbers -> items, notes fitted,
 * audio (the original sam_tts_sing's code, unchanged apart from the cancel check). */
static int sing_render(sam_tts *t, wordlist *l, sing_note *notes, int nnotes, volatile const int *cancel)
{
    itemlist il = {0};
    int rc = 0, k;
    if (rc == 0 && l->n > 0) {
        word *w = wl_insert(l, l->n); /* the sentence-final silence */
        if (w) {
            w->ph[0] = SIL;
            w->nph = 1;
            w->btype = 5;
            w->x694 = 1;
        }
        if (build_items(l, &il) != 0) rc = -1;
    }
    if (rc == 0 && il.n > 0) {
        position_flags(&il);
        select_units(t, &il);
        durations(&il, 1.0);
        /* fit each note: consonants keep their natural length; the vowel takes the rest, shared
         * with sustainable consonants after it (the r of "are", the l of "all", the n of "on") */
        for (k = 0; k < nnotes; k++) {
            double cons = 0.0, vow = 0.0, son = 0.0, room, vroom, sroom;
            int i, nv = 0, ns = 0, seen_vowel = 0;
            for (i = 0; i < il.n; i++) {
                item *x = &il.it[i];
                if (x->note != k + 1) continue;
                x->coda = 0;
                if (is_vowel(x->type)) {
                    vow += x->dur;
                    nv++;
                    seen_vowel = 1;
                } else if (x->type != SIL) {
                    if (seen_vowel && is_sonorant(x->type)) {
                        x->coda = 1;
                        son += x->dur;
                        ns++;
                    } else {
                        cons += x->dur;
                    }
                }
            }
            room = notes[k].sec - cons;
            if (room < 0.04 * (nv + ns ? nv + ns : 1)) room = 0.04 * (nv + ns ? nv + ns : 1);
            vroom = ns ? room * 0.55 : room;
            sroom = room - vroom;
            for (i = 0; i < il.n; i++) {
                item *x = &il.it[i];
                if (x->note != k + 1) continue;
                if (x->type == SIL) x->dur = (float)notes[k].sec;
                else if (is_vowel(x->type)) x->dur = (float)(vow > 0.0 ? vroom * x->dur / vow : vroom / nv);
                else if (x->coda) x->dur = (float)(son > 0.0 ? sroom * x->dur / son : sroom / ns);
            }
        }
        for (k = 0; k < il.n && rc == 0; k++) {
            item *x = &il.it[k];
            sam_segment g;
            if (cancel && *cancel) { /* ClassicVoices patch: cancellable between sounds */
                rc = 1;
                break;
            }
            double hz = 100.0;
            int j, i;
            /* pitch: the note of this item, else the nearest sung note (silences, rests) */
            for (i = 0; i < il.n; i++) {
                int a = k - i, b = k + i;
                if (b < il.n && il.it[b].note && notes[il.it[b].note - 1].hz > 0.0) {
                    hz = notes[il.it[b].note - 1].hz;
                    break;
                }
                if (a >= 0 && il.it[a].note && notes[il.it[a].note - 1].hz > 0.0) {
                    hz = notes[il.it[a].note - 1].hz;
                    break;
                }
            }
            g.unit = x->unit;
            g.dur = x->dur;
            g.hold = !x->note ? 0 : x->coda ? 2 : is_vowel(x->type) ? 1 : 0;
            if (getenv("SAM_DEBUG")) fprintf(stderr, "SING item %d type %d unit %d dur %.3f note %d coda %d hold %d\n", k, x->type, x->unit, x->dur, x->note, x->coda, g.hold);
            g.n_knots = 20;
            for (j = 0; j < 20; j++) {
                g.t[j] = (float)(x->dur * 22050.0 * j / 20.0);
                g.f0[j] = (float)hz;
                g.amp[j] = x->amp;
            }
            rc = sam_synth_segment(t->synth, &g, hold, t);
            if (rc == 0) release(t, (size_t)sam_synth_chunk_pos(t->synth));
        }
    }
    free(il.it);
    return rc;
}

static void sing_begin(sam_tts *t, const sam_sing_opts *o, sam_pcm_cb cb, void *user)
{
    t->out_cb = cb;
    t->out_user = user;
    t->npend = 0;
    sam_synth_chunk_reset(t->synth);
    if (o) sam_synth_set_singing(t->synth, 1, o->vibrato_cents, o->vibrato_rate);
}

static void sing_end(sam_tts *t, const sam_sing_opts *o, int rc)
{
    if (rc == 0) release(t, 0);
    t->npend = 0;
    if (o) sam_synth_set_singing(t->synth, 0, 0.0f, 0.0f);
}

/* ClassicVoices patch: a beat count from a pasted score, 0..32 (NaN and negatives 0) */
static double sane_beats(double b) { return b > 0.0 ? (b < 32.0 ? b : 32.0) : 0.0; }

int sam_tts_sing(sam_tts *t, const char *score, sam_pcm_cb cb, void *user)
{
    return sam_tts_sing_ex(t, score, NULL, cb, user);
}

int sam_tts_sing_ex(sam_tts *t, const char *score, sam_sing_opts *o, sam_pcm_cb cb, void *user)
{
    sing_note *notes = calloc(SING_MAX_NOTES, sizeof *notes);
    wordlist l = {0};
    double tempo = 100.0, shift = pow(2.0, ((double)t->transpose + (o ? o->transpose : 0.0)) / 12.0);
    const char *p = score;
    int nnotes = 0, rc = 0;
    if (!notes) return -1;
    if (o) o->mismatches = 0;
    sing_begin(t, o, cb, user);
    /* one line per word: "twin-kle C4 1 C4 1", "- 1" (rest), "tempo 120" */
    while (*p && rc == 0) {
        char line[512], *tok[64];
        int nt = 0, i;
        size_t n = strcspn(p, "\r\n");
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p += n;
        while (*p == '\r' || *p == '\n') p++;
        for (i = 0; line[i] == ' ' || line[i] == '\t'; i++) {
        }
        if (!line[i] || line[i] == '#') continue;
        {
            char *s = line + i;
            while (*s && nt < 64) {
                tok[nt++] = s;
                while (*s && *s != ' ' && *s != '\t') s++;
                if (*s) *s++ = 0;
                while (*s == ' ' || *s == '\t') s++;
            }
        }
        if (!strcmp(tok[0], "tempo") && nt > 1) {
            tempo = atof(tok[1]);
            if (!(tempo > 0.0)) tempo = 100.0;
            if (tempo < 20.0) tempo = 20.0; /* ClassicVoices patch: pasted scores are untrusted - keep them sane */
            if (tempo > 1000.0) tempo = 1000.0;
            continue;
        }
        if (!strcmp(tok[0], "-")) { /* rest */
            word *w;
            if (nnotes >= SING_MAX_NOTES) break;
            notes[nnotes].hz = 0.0;
            notes[nnotes].sec = sane_beats(nt > 1 ? atof(tok[1]) : 1.0) * 60.0 / tempo;
            w = wl_insert(&l, l.n);
            if (!w) {
                rc = -1;
                break;
            }
            w->ph[0] = SIL;
            w->nph = 1;
            w->note[0] = (short)(nnotes + 1);
            w->silence_ms = (int)(notes[nnotes].sec * 1000.0);
            nnotes++;
            continue;
        }
        {
            char text[128];
            sam_lookup r;
            word *w;
            int first = nnotes, cnt = 0, j, o2 = 0, nsyl, syl[MAX_WORD_PH], st[MAX_WORD_PH];
            for (j = 0; tok[0][j] && o2 < 127; j++)
                if (tok[0][j] != '-') text[o2++] = tok[0][j];
            text[o2] = 0;
            for (j = 1; j + 1 < nt && nnotes < SING_MAX_NOTES; j += 2) {
                double hz = note_hz(tok[j]) * shift;
                if (hz > 0.0 && hz < 30.0) hz = 30.0; /* ClassicVoices patch: an out-of-range note stays audible */
                if (hz > 2000.0) hz = 2000.0;
                notes[nnotes].hz = hz;
                notes[nnotes].sec = sane_beats(atof(tok[j + 1])) * 60.0 / tempo;
                nnotes++;
                cnt++;
            }
            if (!cnt || !sam_word_lookup(t->lex, t->lts, text, &r)) continue;
            w = wl_insert(&l, l.n);
            if (!w) {
                rc = -1;
                break;
            }
            snprintf(w->text, sizeof w->text, "%s", text);
            w->pos = r.posa[0];
            w->cls = pos_class(w->pos);
            w->rate = 1.0f;
            for (j = 0; j < r.n && w->nph < MAX_WORD_PH; j++) {
                int ph = sapi_to_internal(r.pron[j]);
                if (ph < 0) continue;
                w->ph[w->nph++] = ph;
            }
            nsyl = syllabify(w->ph, w->nph, syl, st, MAX_WORD_PH);
            for (j = 0; j < w->nph; j++) w->note[j] = (short)(first + (syl[j] < cnt ? syl[j] : cnt - 1) + 1);
            if (cnt != nsyl && o) o->mismatches++;
            /* more notes than syllables: the last syllable holds them all */
            for (j = first + nsyl; j < first + cnt; j++) {
                notes[first + nsyl - 1].sec += notes[j].sec;
                notes[j].sec = 0.0;
            }
        }
    }
    if (rc == 0 && l.n > 0) rc = sing_render(t, &l, notes, nnotes, o ? o->cancel : NULL);
    sing_end(t, o, rc);
    free(l.w);
    free(notes);
    return rc;
}

int sam_sing_is_score(const char *text)
{
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!strncmp(p, "tempo ", 6)) return 1;
    while (*p) { /* a line "word NOTE BEATS [NOTE BEATS ...]" */
        const char *e = p + strcspn(p, "\r\n"), *q = p;
        char tok[3][32];
        int nt = 0;
        while (q < e && nt < 3) {
            size_t k = 0;
            while (q < e && (*q == ' ' || *q == '\t')) q++;
            while (q < e && *q != ' ' && *q != '\t' && k + 1 < sizeof tok[0]) tok[nt][k++] = *q++;
            while (q < e && *q != ' ' && *q != '\t') q++;
            if (k) {
                tok[nt][k] = 0;
                nt++;
            }
        }
        if (nt == 3 && isalpha((unsigned char)tok[0][0]) && isupper((unsigned char)tok[1][0]) && note_hz(tok[1]) > 0.0 &&
            (isdigit((unsigned char)tok[2][0]) || tok[2][0] == '.'))
            return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

/* ---- the melody: plain text (what VoiceOver sends) -> one note per syllable ----
 *
 * Deterministic, a simple sing-song in a major key a fourth above the voice's speaking pitch (Sam C3, Mike D3,
 * Mary B3). Each sentence walks up and down the scale, 3-4-5-5-6-5-4-3 (scale steps from the key note = 1),
 * continuing across its words; a stressed syllable (the lexicon's stress mark) is lifted to the nearest note of
 * the home chord at or above it and lasts a beat, every other syllable half a beat. Phrase ends: a statement
 * resolves down to the key note (3-2-1, the last note two beats); a question rises (3-5-8, the last one and a half
 * beats); a comma or other pause inside the sentence stops on the fifth (4-5) with half a beat's rest. Sentences
 * end with a beat's rest. 132 beats a minute, times the speaking-rate factor. */
static const int SCALE[10] = {0, 2, 4, 5, 7, 9, 11, 12, 14, 16};
static const int WALK[8] = {2, 3, 4, 4, 5, 4, 3, 2};

typedef struct {
    int word, syl, stress, rest, btype; /* rest: a silence word (btype of it) */
} sing_ev;

static int sing_sentence(sam_tts *t, wordlist *l, int final_bt, double tonic, double tempo, volatile const int *cancel)
{
    sing_ev *ev = NULL;
    sing_note *notes = NULL;
    int nev = 0, cap = 0, i, j, rc = 0, walk = 0, *wfirst = NULL;
    int syl[MAX_WORD_PH], st[MAX_WORD_PH];
    const double beat = 60.0 / tempo;
    if (!l->n) return 0;
    wfirst = calloc((size_t)l->n, sizeof *wfirst);
    if (!wfirst) return -1;
    for (i = 0; i < l->n; i++) {
        word *w = &l->w[i];
        int nsyl;
        wfirst[i] = nev;
        if (is_silence_word(w) && w->nph == 1) {
            nsyl = 1;
            st[0] = 0;
        } else {
            nsyl = syllabify(w->ph, w->nph, syl, st, MAX_WORD_PH);
        }
        if (nev + nsyl > cap) {
            int nc = (nev + nsyl) * 2 + 16;
            sing_ev *ne = realloc(ev, sizeof *ev * (size_t)nc);
            if (!ne) {
                rc = -1;
                goto out;
            }
            ev = ne;
            cap = nc;
        }
        for (j = 0; j < nsyl; j++) {
            ev[nev].word = i;
            ev[nev].syl = j;
            ev[nev].stress = j < MAX_WORD_PH ? st[j] : 0;
            ev[nev].rest = is_silence_word(w) && w->nph == 1;
            ev[nev].btype = w->btype;
            nev++;
        }
        if (!(is_silence_word(w) && w->nph == 1))
            for (j = 0; j < w->nph; j++) w->note[j] = (short)(wfirst[i] + syl[j] + 1);
        else
            w->note[0] = (short)(wfirst[i] + 1);
    }
    notes = calloc((size_t)nev + 1, sizeof *notes);
    if (!notes) {
        rc = -1;
        goto out;
    }
    /* phrases: the syllables between rests */
    for (i = 0; i < nev;) {
        int a = i, b, m, last_rest, q;
        while (i < nev && !ev[i].rest) i++;
        b = i; /* syllables a..b-1, then the rest at b (if any) */
        m = b - a;
        last_rest = 1;
        for (q = b; q < nev; q++)
            if (!ev[q].rest) last_rest = 0;
        for (j = a; j < b; j++) {
            int deg = WALK[walk++ % 8], pos = j - a, fromend = m - 1 - pos;
            double beats = ev[j].stress == 1 ? 1.0 : 0.5;
            if (ev[j].stress == 1 && deg != 0 && deg != 2 && deg != 4 && deg != 7) deg = deg < 4 ? 4 : 7;
            if (last_rest) { /* the sentence's cadence */
                if (final_bt == 3 || final_bt == 4) {
                    static const int Q[3] = {7, 4, 2};
                    if (fromend < 3 && m >= fromend + 1) deg = Q[fromend];
                    if (fromend == 0) beats = 1.5;
                } else {
                    static const int D[3] = {0, 1, 2};
                    if (fromend < 3 && m >= fromend + 1) deg = D[fromend];
                    if (fromend == 0) beats = 2.0;
                    else if (fromend == 1 && beats < 1.0) beats = 1.0;
                }
            } else if (fromend < 2) { /* a pause inside the sentence: stop on the fifth */
                deg = fromend == 0 ? 4 : 3;
                if (fromend == 0) beats = 1.5;
            }
            notes[j].hz = 440.0 * pow(2.0, (tonic + SCALE[deg] - 69.0) / 12.0);
            notes[j].sec = beats * beat;
        }
        if (i < nev) { /* the rest */
            notes[i].hz = 0.0;
            notes[i].sec = (last_rest ? 1.0 : 0.5) * beat;
            i++;
        }
    }
    rc = sing_render(t, l, notes, nev, cancel);
out:
    free(wfirst);
    free(ev);
    free(notes);
    return rc;
}

int sam_tts_sing_text(sam_tts *t, const char *text, sam_sing_opts *o, sam_pcm_cb cb, void *user)
{
    sam_norm *nm;
    sam_sentence s = {0};
    int rc = 0, k = 0, in_quote = t->in_quote, in_paren = t->in_paren;
    float st_off = t->st_off, st_rng = t->st_rng, st_rate = t->st_rate;
    uint32_t rs = t->rand_state;
    double tempo = 132.0 * (o && o->tempo_scale > 0.0 ? o->tempo_scale : 1.0);
    /* the key note: a fourth above the voice's speaking pitch, as a MIDI number, plus any transposition */
    double tonic = floor(12.0 * log2((double)t->base_pitch / 440.0) + 69.0 + 0.5) + 5.0 + (double)t->transpose +
                   (o ? o->transpose : 0.0);
    if (tempo < 50.0) tempo = 50.0;
    if (tempo > 480.0) tempo = 480.0;
    nm = sam_norm_new(text);
    if (!nm) return -1;
    sing_begin(t, o, cb, user);
    while (rc == 0 && (k = sam_norm_next(nm, t->lex, t->lts, &s)) > 0) {
        wordlist l = {0};
        if (o && o->cancel && *o->cancel) {
            rc = 1;
            break;
        }
        rc = build_words(t, &s, &l);
        if (rc == 0 && l.n > 0) {
            int j, sung = 0;
            for (j = 0; j < l.n; j++)
                if (!(is_silence_word(&l.w[j]) && l.w[j].nph == 1)) sung = 1;
            if (!sung) l.n = 0; /* nothing but pauses (a trailing newline): no rest either */
        }
        if (rc == 0 && l.n > 0) {
            int final_bt = l.w[l.n - 1].btype;
            rc = sing_sentence(t, &l, final_bt, tonic, tempo, o ? o->cancel : NULL);
        }
        if (rc == 0) release(t, (size_t)sam_synth_chunk_pos(t->synth));
        free(l.w);
    }
    if (k < 0) rc = -1;
    sing_end(t, o, rc);
    /* the speech path's running state is left as it was: singing never changes how the voice speaks next */
    t->in_quote = in_quote;
    t->in_paren = in_paren;
    t->st_off = st_off;
    t->st_rng = st_rng;
    t->st_rate = st_rate;
    t->rand_state = rs;
    sam_sentence_free(&s);
    sam_norm_free(nm);
    return rc;
}

/* shared tail of the constructors; takes the loaded parts (NULL = failed) */
static sam_tts *tts_finish(sam_tts *t, const sam_params *p, char *err, size_t errlen)
{
    if (!t->voice) goto fail;
    if (!t->lex) {
        snprintf(err, errlen, "cannot load lexicon (LTTS1033.LXA)");
        goto fail;
    }
    if (!t->lts) {
        snprintf(err, errlen, "cannot load letter-to-sound model (r1033tts.LXA)");
        goto fail;
    }
    t->synth = sam_synth_new(t->voice, p);
    if (!t->synth) goto fail;
    t->transpose = p ? p->transpose : 0.0f;
    t->base_pitch = p && p->base_pitch > 0.0f ? p->base_pitch : 100.0f;
    t->monotone = p ? p->monotone : 0;
    t->rand_state = 1;
    t->st_rng = 1.0f;
    t->st_rate = 1.0f;
    return t;
fail:
    sam_tts_free(t);
    return NULL;
}

sam_tts *sam_tts_new(const char *spd, const char *lex, const char *lts, const sam_params *p, char *err, size_t errlen)
{
    sam_tts *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->voice = sam_voice_load(spd, err, errlen);
    if (t->voice) t->lex = sam_lexicon_load(lex);
    if (t->lex) t->lts = sam_lts_load(lts);
    return tts_finish(t, p, err, errlen);
}

sam_tts *sam_tts_new_mem(const void *spd, size_t spd_size, const void *lex, size_t lex_size, const void *lts,
                         size_t lts_size, const sam_params *p, char *err, size_t errlen)
{
    sam_tts *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->voice = sam_voice_load_mem(spd, spd_size, err, errlen);
    if (t->voice) t->lex = sam_lexicon_load_mem(lex, lex_size);
    if (t->lex) t->lts = sam_lts_load_mem(lts, lts_size);
    return tts_finish(t, p, err, errlen);
}

void sam_tts_free(sam_tts *t)
{
    if (!t) return;
    sam_synth_free(t->synth);
    sam_lexicon_free(t->lex);
    sam_lts_free(t->lts);
    sam_voice_free(t->voice);
    free(t->pend);
    free(t);
}
