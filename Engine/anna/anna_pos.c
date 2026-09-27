/* Pronunciation stage of the Anna frontend (MSTTSFrontendENU.dll 0x0D5E3DA7):
 *   - abbreviation items (types 0x1003/0x1004): fixed pronunciation or tag handler (table 0x0D60C4AC)
 *   - ambiguous words (table 0x0D604B90, 72 entries) with context handlers (table 0x0D60C4D8)
 *   - otherwise the lookup chain 0x0D5E2D08 (anna_morph.c)
 *   - Brill-style tagger 0x0D5E2892 (63 rules @0x0D5D2BE8), tag change 0x0D5E2818 picks the pronunciation
 *   - post-lexical words (table 0x0D605910, handlers 0x0D60C530): units after numbers, "the", read, live
 * Handler semantics follow the engine code; comments name the functions.
 */
#include "anna_lex.h"
#include "anna_norm_types.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *word;
    const char *pron[3];
    uint32_t pos[3];
    int handler;
} lex_entry;
typedef struct {
    uint32_t from, to;
    int type;
    uint32_t c1, c2;
    const char *word;
} tag_rule;
typedef struct {
    const char *rev;
    int rec;
} morph_suffix;
typedef struct {
    const char *phones;
    uint32_t map[4][2];
    int nmap;
    uint32_t flags;
} morph_record;

#include "anna_lex_tab.h"

#define E_INVALIDARG_ (-1)

/* ------------------------------------------------------------------------------------------------ */
/* phone strings of the tables, converted once (the engine converts them at load time)              */

typedef struct {
    uint16_t ph[3][64];
    int n[3];
} ids3;

static ids3 ambig_ids[72], abbr_ids[178], post_ids[43];
static uint16_t of_a[16], of_an[16];
static int n_of_a, n_of_an, ids_ready;
static pthread_once_t ids_once = PTHREAD_ONCE_INIT; /* ClassicVoices patch: converted once, by one thread */

static void conv(const lex_entry *t, ids3 *d)
{
    int k;
    for (k = 0; k < 3; k++) {
        d->n[k] = t->pron[k] ? anna_phones_from_string(t->pron[k], d->ph[k], 63) : -1;
        if (d->n[k] >= 0) d->ph[k][d->n[k]] = 0;
    }
}

static void init_ids_once(void)
{
    int i;
    for (i = 0; i < 72; i++) conv(&AMBIG[i], &ambig_ids[i]);
    for (i = 0; i < 178; i++) conv(&ABBR[i], &abbr_ids[i]);
    for (i = 0; i < 43; i++) conv(&POST[i], &post_ids[i]);
    n_of_a = anna_phones_from_string("ah 2 v & ax 2 &", of_a, 15);    /* @0x0D5D32CC */
    n_of_an = anna_phones_from_string("ah 2 v & ax 2 n &", of_an, 15); /* @0x0D5D32D0 */
    of_a[n_of_a] = of_an[n_of_an] = 0;
    ids_ready = 1;
}

static void init_ids(void) { pthread_once(&ids_once, init_ids_once); }

/* ------------------------------------------------------------------------------------------------ */
/* helpers                                                                                          */

static int wncmp_a(const uint16_t *a, const char *b, int n) /* wcsncmp against ASCII */
{
    int i;
    for (i = 0; i < n; i++) {
        int x = a[i], y = (unsigned char)b[i];
        if (x != y || !x) return x - y;
    }
    return 0;
}

static int wnicmp_a(const uint16_t *a, const char *b, int n) /* _wcsnicmp ("C" locale) */
{
    int i;
    for (i = 0; i < n; i++) {
        int x = a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
    return 0;
}

static int wisdigit(uint16_t c) { return c >= '0' && c <= '9'; }

typedef struct {
    anna_msent *s;
    int cur; /* current item */
} ctx;

static const anna_mnode *N(const ctx *c, int i) { return i >= 0 && i < c->s->nn ? &c->s->nodes[i] : NULL; }
static int ttype(const anna_mnode *n) { return n->ti ? n->ti[0] : n->type; }

/* first word record of an item */
static anna_mword *W0(const ctx *c, const anna_mnode *n) { return n && n->nw ? &c->s->words[n->first] : NULL; }
static const uint16_t *wtext(const anna_mword *w) { return w && w->has_text ? w->tok->wtext : NULL; }
static int wlength(const anna_mword *w) { return w && w->has_text ? w->tok->wlen : 0; }

static int frac_over(const anna_mnode *n)
{
    const anna_numinfo *ni = (const anna_numinfo *)n->ti;
    return ni && ni->fr ? ni->fr->over : 0;
}

static void setp(uint16_t *dst, int *n, const uint16_t *src, int ns)
{
    int k = ns < ANNA_PRON_MAX - 1 ? ns : ANNA_PRON_MAX - 1;
    memcpy(dst, src, sizeof(uint16_t) * (size_t)k);
    dst[k] = 0;
    *n = k;
}

/* pronunciation k of table entry t as pron1 with its POS (the handlers' common tail) */
static void pick(anna_entry *e, const lex_entry *t, const ids3 *d, int k)
{
    setp(e->pron1, &e->n1, d->ph[k], d->n[k] > 0 ? d->n[k] : 0);
    e->pos1[0] = t->pos[k];
    e->pos = t->pos[k];
}

static void reset(anna_entry *e)
{
    e->npos1 = 1;
    e->npos2 = 0;
    e->n2 = 0;
    e->hasalt = 0;
    e->usealt = 0;
    e->lextype = 0x1000;
}

/* "of a"/"of an" + unit (fractions without "over"); the engine's length field counts the prefix twice */
static void of_a_unit(anna_entry *e, const ids3 *d, int k, int check_vowel)
{
    const uint16_t *pre = of_a;
    int npre = n_of_a, nu = d->n[k] > 0 ? d->n[k] : 0, i, tot;
    if (check_vowel && nu > 0)
        for (i = 0; i < 16; i++)
            if (VOWELS[i] == d->ph[k][0]) {
                pre = of_an;
                npre = n_of_an;
            }
    setp(e->pron1, &e->n1, pre, npre);
    tot = npre;
    for (i = 0; i < nu && tot < ANNA_PRON_MAX - 1; i++) e->pron1[tot++] = d->ph[k][i];
    e->pron1[tot] = 0;
    e->n1 = npre + tot;
}

static int is_number_word(const uint16_t *s, int len)
{
    static const char *const W[] = {"four", "five", "nine", "three", "seven", "eight", "forty", "fifty", "sixty",
                                    "twenty", "thirty", "eighty", "ninety", "eleven", "twelve", "seventy",
                                    "fifteen", "sixteen", "thirteen", "fourteen", "eighteen", "nineteen"};
    size_t k;
    for (k = 0; k < sizeof W / sizeof *W; k++)
        if ((int)strlen(W[k]) == len && !wncmp_a(s, W[k], len)) return 1;
    return 0;
}

static int open_type(int t) { return t == 1 || t == 2 || t == 3 || t == 7 || t == 8; }

/* ------------------------------------------------------------------------------------------------ */
/* handlers                                                                                          */

/* 0x0D5DEB81: units: singular after 1 / one / ordinals, "of a" after a fraction, else plural */
static int h_units(const ctx *c, anna_entry *e, const lex_entry *t, const ids3 *d)
{
    const anna_mnode *P = N(c, c->cur - 1), *PP;
    int ty;
    if (!P) { /* E_INVALIDARG path: no reset */
        if (t->pron[2]) pick(e, t, d, 2);
        else pick(e, t, d, 1);
        return 0;
    }
    reset(e);
    ty = ttype(P);
    if (ty == 0x1006 || ty == 0x1014) {
        if ((P->len == 1 && !wncmp_a(P->text, "1", 1)) || (P->len == 2 && !wncmp_a(P->text, "-1", 2))) pick(e, t, d, 0);
        else pick(e, t, d, 1);
        return 0;
    }
    if (ty == 0x1008 || ty == 0x100f) {
        pick(e, t, d, 1);
        return 0;
    }
    if (ty == 0x1007) {
        pick(e, t, d, 0);
        return 0;
    }
    if (ty == 0x100e) {
        if (frac_over(P) == 0) {
            of_a_unit(e, d, 0, 1);
            e->pos1[0] = t->pos[0];
            e->pos = t->pos[0];
        } else {
            pick(e, t, d, 1);
        }
        return 0;
    }
    if (P->len == 3) {
        if (!wnicmp_a(P->text, "one", 3)) {
            pick(e, t, d, 0);
            return 0;
        }
        if (wnicmp_a(P->text, "cu.", 3) && wnicmp_a(P->text, "sq.", 3) && wnicmp_a(P->text, "fl.", 3)) {
            if (!wncmp_a(P->text, "two", 3) || !wncmp_a(P->text, "six", 3) || !wncmp_a(P->text, "ten", 3)) pick(e, t, d, 1);
            else if (is_number_word(P->text, P->len) || !t->pron[2]) pick(e, t, d, 1);
            else pick(e, t, d, 2);
            return 0;
        }
    } else if (P->len != 2 ||
               (wnicmp_a(P->text, "cu", 2) && wnicmp_a(P->text, "sq", 2) && wnicmp_a(P->text, "fl", 2))) {
        if (is_number_word(P->text, P->len) || !t->pron[2]) pick(e, t, d, 1);
        else pick(e, t, d, 2);
        return 0;
    }
    /* "cu ft", "sq. in": look one item further back */
    PP = N(c, c->cur - 2);
    if (!PP) {
        pick(e, t, d, 0);
        return 0;
    }
    ty = ttype(PP);
    if (ty == 0x1006) {
        if ((PP->len == 1 && !wncmp_a(PP->text, "1", 1)) || (PP->len == 2 && !wncmp_a(PP->text, "-1", 2))) pick(e, t, d, 0);
        else pick(e, t, d, 1);
    } else if (ty == 0x1008 || ty == 0x100f) {
        pick(e, t, d, 1);
    } else if (ty == 0x1007) {
        pick(e, t, d, 0);
    } else if (ty == 0x100e) {
        pick(e, t, d, frac_over(PP) == 0 ? 0 : 1);
    } else {
        pick(e, t, d, PP->len == 3 && !wnicmp_a(PP->text, "one", 3) ? 0 : 1);
    }
    return 0;
}

/* 0x0D5DF4D3: Dr / St / Gov: title before a name, else the second reading */
static int h_title(const ctx *c, anna_entry *e, const lex_entry *t, const ids3 *d)
{
    const anna_mnode *C = N(c, c->cur), *X = N(c, c->cur + 1), *P;
    int first = 0;
    reset(e);
    if (!X) goto second;
    if (X->pos != 0x400e) {
        const uint16_t *s = X->text;
        int len = X->len;
        if (len == 0 || !anna_wisupper(s[0])) goto lower;
        {
            int u = 1;
            while (u < len && (anna_wislower(s[u]) || s[u] == '\'')) u++;
            if (u == len - 2 && s[u + 1] == '\'' && s[u + 2] == 's') u += 2;
            if (u == len && wncmp_a(s, "North", 5) && wncmp_a(s, "South", 5) && wncmp_a(s, "West", 4) &&
                wncmp_a(s, "East", 4) &&
                (len != 2 || (wncmp_a(s, "Ne", 2) && wncmp_a(s, "Nw", 2) && wncmp_a(s, "Se", 2) && wncmp_a(s, "Sw", 2))) &&
                (len != 1 || (wncmp_a(s, "N", 1) && wncmp_a(s, "S", 1) && wncmp_a(s, "E", 1) && wncmp_a(s, "W", 1)))) {
                P = N(c, c->cur - 1);
                if (P && P->len != 0 && anna_wisupper(P->text[0])) {
                    int k = 1;
                    while (k < P->len && P->text[k] >= 'a' && P->text[k] <= 'z') k++; /* islower((byte)c) */
                    if (k == P->len) goto second;                                    /* "Main St John" */
                }
                first = 1;
                goto done;
            }
            if (u == 1 && len == 2 && s[1] == '.') {
                first = 1;
                goto done;
            }
        }
    lower:
        P = N(c, c->cur - 1);
        if (P) {
            if (!open_type(ttype(P))) {
                if (C && ttype(C) == 0x101b) first = 1;
                goto done;
            }
        }
        first = 1;
        goto done;
    }
    /* before the sentence end */
    if (C && ttype(C) == 0x101b) goto lower;
second:
    first = 0;
done:
    pick(e, t, d, first ? 0 : 1);
    return 0;
}

/* 0x0D5DF849: fig / p: before a number the first reading */
static int h_fig(const ctx *c, anna_entry *e, const lex_entry *t, const ids3 *d)
{
    const anna_mnode *X = N(c, c->cur + 1);
    reset(e);
    pick(e, t, d, X && X->len != 0 && wisdigit(X->text[0]) ? 0 : 1);
    return 0;
}

/* 0x0D5DF964: the item in capitals (and digits): first reading (letters) */
static int h_caps_digits(const ctx *c, anna_entry *e, const lex_entry *t, const ids3 *d)
{
    const anna_mnode *C = N(c, c->cur);
    int u = 0;
    reset(e);
    while (u < C->len && (anna_wisupper(C->text[u]) || wisdigit(C->text[u]))) u++;
    pick(e, t, d, u == C->len ? 0 : 1);
    return 0;
}

/* 0x0D5DFBA0: Mar / Sat / Wed capitalized: the date word */
static int h_capital(const ctx *c, anna_entry *e, const lex_entry *t, const ids3 *d)
{
    const anna_mnode *C = N(c, c->cur);
    reset(e);
    pick(e, t, d, anna_wisupper(C->text[0]) ? 0 : 1);
    return 0;
}

/* 0x0D5DFC95: in capitals the letters (third reading), else a unit */
static int h_caps_or_unit(const ctx *c, anna_entry *e, const lex_entry *t, const ids3 *d)
{
    const anna_mnode *C = N(c, c->cur);
    int u = 0;
    reset(e);
    while (u < C->len && anna_wisupper(C->text[u])) u++;
    if (u == C->len) {
        pick(e, t, d, 2);
        return 0;
    }
    return h_units(c, e, t, d);
}

/* 0x0D5DFD7F: C / F / K after a temperature */
static int h_temp(const ctx *c, anna_entry *e, const lex_entry *t, const ids3 *d)
{
    const anna_mnode *P = N(c, c->cur - 1);
    reset(e);
    if (c->cur + 1 >= c->s->nn) return E_INVALIDARG_; /* no next item: the engine gives up (empty record) */
    pick(e, t, d, P && ttype(P) == 0x100a ? 0 : 1);
    return 0;
}

/* 0x0D5DFE81: cu / fl / sq: letters in capitals, "of a" after a fraction, else the unit word */
static int h_cufl(const ctx *c, anna_entry *e, const lex_entry *t, const ids3 *d)
{
    const anna_mnode *C = N(c, c->cur), *P = N(c, c->cur - 1);
    int u = 0, ty;
    while (u < C->len && anna_wisupper(C->text[u])) u++;
    if (u == C->len || !P) { /* no reset on these paths */
        pick(e, t, d, 0);
        return 0;
    }
    reset(e);
    ty = ttype(P);
    if (ty == 0x100e && frac_over(P) == 0) {
        of_a_unit(e, d, 1, 0);
        e->pos1[0] = t->pos[1];
        e->pos = t->pos[1];
    } else {
        pick(e, t, d, 1);
    }
    return 0;
}

/* 0x0D5E33DA: "a": the article (with the letter as alternative) inside running text */
static int h_a(const ctx *c, anna_entry *e, const lex_entry *t, const ids3 *d)
{
    const anna_mnode *C = N(c, c->cur), *X = N(c, c->cur + 1);
    int article = !X || (C->nw < 2 && ttype(C) == 0x1002 && (ttype(X) & 0x1000));
    if (article) {
        pick(e, t, d, 1);
        e->npos1 = 1;
        setp(e->pron2, &e->n2, d->ph[0], d->n[0]);
        e->pos2[0] = t->pos[0];
        e->npos2 = 1;
        e->hasalt = 1;
    } else {
        pick(e, t, d, 0);
    }
    return 0;
}

/* 0x0D5E3545: Polish / polish */
static int h_polish(const ctx *c, anna_entry *e, const lex_entry *t, const ids3 *d)
{
    const anna_mnode *C = N(c, c->cur), *X = N(c, c->cur + 1), *P = N(c, c->cur - 1);
    if (!X || !anna_wisupper(C->text[0])) {
        pick(e, t, d, 1);
        e->pos1[1] = t->pos[2];
        e->npos1 = 2;
        setp(e->pron2, &e->n2, d->ph[0], d->n[0]);
        e->pos2[0] = t->pos[0];
        e->npos2 = 1;
        e->hasalt = 1;
        return 0;
    }
    if (P && !open_type(ttype(P))) {
        setp(e->pron1, &e->n1, d->ph[0], d->n[0]);
        e->pos1[0] = 0x1000;
        e->pos = 0x1000;
        return 0;
    }
    pick(e, t, d, 1);
    return 0;
}

/* 0x0D5DFA81: "Sr" capitalized = senior */
static int h_capitalized(const ctx *c, anna_entry *e, const lex_entry *t, const ids3 *d)
{
    const anna_mnode *C = N(c, c->cur);
    int u = 0;
    reset(e);
    while (u < C->len && (u == 0 ? anna_wisupper(C->text[0]) : anna_wislower(C->text[u]))) u++;
    pick(e, t, d, u == C->len ? 0 : 1);
    return 0;
}

typedef int (*handler)(const ctx *, anna_entry *, const lex_entry *, const ids3 *);
static const handler AMBIG_H[11] = {h_units, h_title, h_fig, h_caps_digits, h_capital, h_caps_or_unit,
                                    h_temp, h_cufl, h_a, h_polish, h_capitalized};
static const handler ABBR_H[5] = {h_units, h_title, h_fig, h_temp, h_cufl};

/* ------------------------------------------------------------------------------------------------ */
/* tagger 0x0D5E2892                                                                                */

/* 0x0D5E2818 */
static void change_tag(anna_entry *e, uint32_t to)
{
    int k;
    for (k = 0; k < e->npos1; k++)
        if (e->pos1[k] == to) {
            e->usealt = 0;
            e->pos = to;
            return;
        }
    if (e->hasalt)
        for (k = 0; k < e->npos2; k++)
            if (e->pos2[k] == to) {
                e->usealt = 1;
                e->pos = to;
                return;
            }
}

static void tag(anna_entry *e, int n)
{
    int r, i;
#define OK(j) (e[j].reqpos == 0 && e[j].pos == R->from)
    for (r = 0; r < 63; r++) {
        const tag_rule *R = &RULES[r];
        switch (R->type) {
        case 0:
            for (i = 1; i < n; i++)
                if (OK(i) && e[i - 1].pos == R->c1) change_tag(&e[i], R->to);
            break;
        case 1:
            for (i = 0; i < n - 1; i++)
                if (OK(i) && e[i + 1].pos == R->c1) change_tag(&e[i], R->to);
            break;
        case 2:
            for (i = 2; i < n; i++)
                if (OK(i) && e[i - 2].pos == R->c1) change_tag(&e[i], R->to);
            break;
        case 3:
            for (i = 0; i < n - 2; i++)
                if (OK(i) && e[i + 2].pos == R->c1) change_tag(&e[i], R->to);
            break;
        case 4:
            for (i = 1; i < n - 1; i++)
                if (OK(i) && e[i - 1].pos == R->c1 && e[i + 1].pos == R->c2) change_tag(&e[i], R->to);
            break;
        case 5:
            for (i = 1; i < n - 2; i++)
                if (OK(i) && e[i - 1].pos == R->c1 && e[i + 2].pos == R->c2) change_tag(&e[i], R->to);
            break;
        case 6:
            for (i = 2; i < n - 1; i++)
                if (OK(i) && e[i - 2].pos == R->c1 && e[i + 1].pos == R->c2) change_tag(&e[i], R->to);
            break;
        case 7:
            for (i = 0; i < n; i++)
                if (OK(i) && anna_wisupper(e[i].text[0])) change_tag(&e[i], R->to);
            break;
        case 8:
            for (i = 1; i < n; i++)
                if (OK(i) && !anna_wicmp_a(e[i].text, R->word) && e[i - 1].pos == R->c1) change_tag(&e[i], R->to);
            break;
        default:
            break;
        }
    }
#undef OK
}

/* ------------------------------------------------------------------------------------------------ */
/* post-lexical words (handlers 0x0D60C530)                                                         */

/* StringCchCopyW(pron, strlen(pron) + 1, src): the new pronunciation is cut to the old length */
static void trunc_copy(anna_mword *w, const uint16_t *src, int ns)
{
    int n = ns < w->npron ? ns : w->npron;
    memcpy(w->pron, src, sizeof(uint16_t) * (size_t)n);
    w->pron[n] = 0;
    w->npron = n;
}

static void post_set(anna_mnode *C, anna_mword *w, const lex_entry *t, const ids3 *d, int k, int set_item_pos)
{
    if (!w) return;
    trunc_copy(w, d->ph[k], d->n[k] > 0 ? d->n[k] : 0);
    w->pos = t->pos[k];
    if (set_item_pos) C->pos = (int)t->pos[k];
}

static int is_vowel(uint16_t id)
{
    int i;
    for (i = 0; i < 16; i++)
        if (VOWELS[i] == id) return 1;
    return 0;
}

/* 0x0D5E325B: unit after a cardinal, before a noun (item POS): the singular */
static void post_units(ctx *c, const lex_entry *t, const ids3 *d)
{
    anna_mnode *C = &c->s->nodes[c->cur];
    const anna_mnode *P = N(c, c->cur - 1), *X = N(c, c->cur + 1), *XX;
    (void)t;
    if (!X || !P || ttype(P) != 0x1006) return;
    XX = N(c, c->cur + 2);
    if (X->pos == 0x1000 || (X->pos == 0x3001 && XX && XX->pos == 0x1000)) {
        anna_mword *w = W0(c, C);
        if (w) trunc_copy(w, d->ph[0], d->n[0] > 0 ? d->n[0] : 0);
    }
}

/* 0x0D5E331D: "the" before a vowel */
static void post_the(ctx *c, const lex_entry *t, const ids3 *d)
{
    anna_mnode *C = &c->s->nodes[c->cur];
    const anna_mnode *X = N(c, c->cur + 1);
    const anna_mword *xw;
    if (!X) return;
    xw = W0(c, X);
    post_set(C, W0(c, C), t, d, xw && xw->pron_set && is_vowel(xw->pron[0]) ? 0 : 1, 0);
}

/* 0x0D5E36F1: read / misread / proofread: present or past from the words before */
static void post_read(ctx *c, const lex_entry *t, const ids3 *d)
{
    static const char *const PAST[] = {"have", "haven't", "has", "hasn't", "had", "hadn't", "am", "ain't", "are",
                                       "aren't", "be", "is", "isn't", "was", "wasn't", "were", "weren't"};
    anna_mnode *C = &c->s->nodes[c->cur];
    const anna_mnode *P, *X;
    const anna_mword *pw, *xw;
    int present = 0, i;
    if (!N(c, c->cur + 1)) return;
    P = N(c, c->cur - 1);
    if (!P) {
        post_set(C, W0(c, C), t, d, 0, 1);
        return;
    }
    pw = W0(c, P);
    if (wlength(pw) == 2 && !wnicmp_a(wtext(pw), "to", 2)) {
        present = 1;
    } else {
        int j = c->cur - 1;
        uint32_t pos = pw ? pw->pos : 0;
        X = P;
        while (pos != 0x4001 && pos != 0x4007 && j - 1 >= 0) {
            j--;
            X = N(c, j);
            xw = W0(c, X);
            pos = xw ? xw->pos : 0;
        }
        xw = W0(c, X);
        pos = xw ? xw->pos : 0;
        if (pos == 0x4001) {
            const uint16_t *s = wtext(xw);
            int len = wlength(xw);
            present = 1;
            for (i = 0; i < (int)(sizeof PAST / sizeof *PAST); i++)
                if (s && (int)strlen(PAST[i]) == len && !wnicmp_a(s, PAST[i], len)) present = 0;
        } else if (pos == 0x4007) {
            const uint16_t *s = wtext(xw);
            present = 0;
            if (s)
                for (i = 0; s[i]; i++)
                    if (s[i] == '\'') {
                        present = !wnicmp_a(s + i, "'ll", 3);
                        break;
                    }
        } else if (pos == 0x5000 && wlength(xw) == 6 && !wnicmp_a(wtext(xw), "please", 6)) {
            present = 1;
        } else if (pos == 0x2000) {
            present = 1;
        } else {
            const anna_mword *y = pw;
            const anna_mnode *PP = N(c, c->cur - 2);
            if (wlength(y) == 3 && !wnicmp_a(wtext(y), "not", 3) && PP) y = W0(c, PP);
            present = (wlength(y) == 2 && !wnicmp_a(wtext(y), "to", 2)) ||
                      (wlength(y) == 4 && !wnicmp_a(wtext(y), "will", 2));
        }
    }
    post_set(C, W0(c, C), t, d, present ? 0 : 1, 1);
}

/* 0x0D5E3C14: live / lives: verb or adjective/noun */
static void post_live(ctx *c, const lex_entry *t, const ids3 *d)
{
    anna_mnode *C = &c->s->nodes[c->cur];
    const anna_mnode *X = N(c, c->cur + 1), *P = N(c, c->cur - 1);
    if (!X) return;
    if (P) {
        const anna_mword *pw = W0(c, P), *xw = W0(c, X), *cw = W0(c, C);
        uint32_t pp = pw ? pw->pos : 0;
        int A = (wlength(pw) == 2 && !wnicmp_a(wtext(pw), "to", 2)) || pp == 0x1002 || pp == 0x2000 || pp == 0x1000 ||
                pp == 0x3002 || (N(c, c->cur + 2) && (xw ? xw->pos : 0) != 0x2000);
        int B = pp != 0x1000 || (wtext(cw) && wnicmp_a(wtext(cw), "live", 4)) || wlength(cw) != 4;
        if (A && B) {
            post_set(C, W0(c, C), t, d, 1, 1);
            return;
        }
    }
    post_set(C, W0(c, C), t, d, 0, 1);
}

/* ------------------------------------------------------------------------------------------------ */
/* sentence structure                                                                               */

void anna_msent_free(anna_msent *s)
{
    free(s->nodes);
    free(s->words);
    free(s->entries);
    free(s->tagged);
    memset(s, 0, sizeof *s);
}

int anna_msent_build(anna_msent *s, const anna_token *t, int n)
{
    int i;
    size_t cnt = (size_t)(n > 0 ? n : 1);
    memset(s, 0, sizeof *s);
    s->nodes = (anna_mnode *)calloc(cnt, sizeof *s->nodes);
    s->words = (anna_mword *)calloc(cnt, sizeof *s->words);
    s->entries = (anna_entry *)calloc(cnt, sizeof *s->entries);
    s->tagged = (anna_entry *)calloc(cnt, sizeof *s->tagged);
    if (!s->nodes || !s->words || !s->entries || !s->tagged) {
        anna_msent_free(s);
        return -1;
    }
    for (i = 0; i < n; i++) {
        anna_mword *w = &s->words[s->nw++];
        if (s->nn == 0 || t[i].item != t[i - 1].item) {
            anna_mnode *m = &s->nodes[s->nn++];
            m->type = t[i].tok_type;
            m->pos = t[i].item_pos;
            m->text = t[i].item_text;
            m->len = t[i].item_len > 0 ? t[i].item_len : 0; /* -1 (no text) is a 0 length in the engine */
            m->ofs = t[i].src_pos;
            m->first = i;
            m->ti = t[i].ti;
            m->abbrev = t[i].abbrev;
        }
        s->nodes[s->nn - 1].nw++;
        w->tok = &t[i];
        w->action = t[i].action;
        w->has_text = t[i].wlen >= 0;
        w->entry = -1;
        w->pos = (uint32_t)t[i].word_pos;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* 0x0D5E3DA7                                                                                       */

static int lookup_action(int a) { return a == 0 || a == 4 || a == 2; }

static int cmp_entry(const void *key, const void *elem)
{
    return anna_wicmp_a((const uint16_t *)key, ((const lex_entry *)elem)->word);
}

int anna_pronounce(const anna_lexicon *lex, const anna_lts *lts, anna_msent *s)
{
    ctx c;
    int i, k, ne = 0;
    init_ids();
    c.s = s;
    for (i = 0; i < s->nn; i++) {
        anna_mnode *m = &s->nodes[i];
        c.cur = i;
        for (k = 0; k < m->nw; k++) {
            anna_mword *w = &s->words[m->first + k];
            anna_entry *e;
            const anna_token *tk = w->tok;
            int hr = 0, pos_hint = tk->pos_hint > 0 ? tk->pos_hint : 0;
            if (!w->has_text || !lookup_action(w->action)) continue;
            w->entry = ne;
            e = &s->entries[ne++];
            memset(e, 0, sizeof *e);
            {
                int l = tk->wlen < 0x7f ? tk->wlen : 0x7f;
                memcpy(e->text, tk->wtext, sizeof(uint16_t) * (size_t)l);
                e->text[l] = 0;
            }
            if (pos_hint) e->reqpos = (uint32_t)pos_hint;
            if (tk->phone_ids && pos_hint) continue;
            if (m->type == 0x1003 || m->type == 0x1004) {
                int a = m->abbrev;
                if (a < 0 || a >= 178) return -1;
                if (ABBR[a].handler < 0) {
                    e->npos1 = 1;
                    setp(e->pron1, &e->n1, abbr_ids[a].ph[0], abbr_ids[a].n[0] > 0 ? abbr_ids[a].n[0] : 0);
                    e->pos1[0] = ABBR[a].pos[0];
                    e->npos2 = 0;
                    e->n2 = 0;
                    e->hasalt = 0;
                    e->usealt = 0;
                    e->pos = ABBR[a].pos[0];
                    e->lextype = 0x1000;
                } else if (ABBR[a].handler < 5) {
                    hr = ABBR_H[ABBR[a].handler](&c, e, &ABBR[a], &abbr_ids[a]);
                }
                e->fixed = 1;
            } else {
                const lex_entry *t = (const lex_entry *)bsearch(e->text, AMBIG, 72, sizeof *AMBIG, cmp_entry);
                if (t) {
                    hr = AMBIG_H[t->handler](&c, e, t, &ambig_ids[t - AMBIG]);
                    e->fixed = 1;
                } else if (anna_lookup_word(lex, lts, e) < 0) {
                    return -1;
                }
            }
            if (hr < 0) return -1;
        }
    }
    s->ne = ne;
    memcpy(s->tagged, s->entries, sizeof(anna_entry) * (size_t)ne);
    tag(s->entries, ne);
    /* copy the choices back into the word records */
    for (i = 0; i < s->nw; i++) {
        anna_mword *w = &s->words[i];
        const anna_entry *e;
        const uint16_t *p;
        int n = 0;
        if (w->entry < 0) continue;
        e = &s->entries[w->entry];
        p = w->tok->phone_ids ? w->tok->phone_ids : (e->usealt ? e->pron2 : e->pron1);
        while (p[n] && n < ANNA_PRON_MAX - 1) n++;
        memcpy(w->pron, p, sizeof(uint16_t) * (size_t)n);
        w->pron[n] = 0;
        w->npron = n;
        w->pron_set = 1;
        w->pos = w->tok->pos_hint > 0 ? (uint32_t)w->tok->pos_hint : e->pos;
        w->lextype = e->lextype;
    }
    /* post-lexical words */
    for (i = 0; i < s->nn; i++) {
        anna_mnode *m = &s->nodes[i];
        uint16_t key[ANNA_WORD_MAX];
        int l = m->len < ANNA_WORD_MAX - 1 ? m->len : ANNA_WORD_MAX - 1;
        const lex_entry *t;
        if (m->type != 0x1002 && m->type != 0x1003 && m->type != 0x1004) continue;
        memcpy(key, m->text, sizeof(uint16_t) * (size_t)l);
        key[l] = 0;
        if (m->len > 1 && l == m->len && key[l - 1] == '.') key[l - 1] = 0;
        t = (const lex_entry *)bsearch(key, POST, 43, sizeof *POST, cmp_entry);
        if (!t) continue;
        c.cur = i;
        switch (t->handler) {
        case 0: post_units(&c, t, &post_ids[t - POST]); break;
        case 1: post_the(&c, t, &post_ids[t - POST]); break;
        case 2: post_read(&c, t, &post_ids[t - POST]); break;
        case 3: post_live(&c, t, &post_ids[t - POST]); break;
        default: break;
        }
    }
    return 0;
}
