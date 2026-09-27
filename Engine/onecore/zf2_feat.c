/* zf2_feat.c - CTTSFeatureExtractionEngine (vtbl 0x1801714b8, slot 3 = 0x18005bfc0 -> 0x180005248):
 * evaluation of the APM feature paths ("Phone.Syllable.Word.NextWord.POS") on every phone of the sentence.
 *
 * Path grammar (FUN_180037e34): Location(.Location)*.Property.  Locations (table 0x18016fee0, {name, level, dir,
 * skip}): a step to the same level moves dir nodes along the sentence-level list of that level, skipping
 * "empty" nodes when skip is set (Prev/Next of Syllable, Word, ProsodicWord, Phrase, IntonationPhrase); a step
 * to a higher level goes to the ancestor; a step to a lower level takes the first (dir -1) / last (+1) child.
 * A failed step or a property getter that declines (silence node) yields a null cell (kind -1).
 * Getters: per-class tables 0x180170240 (phone), 0x180170400 (syllable), 0x1801706d0 (word),
 * 0x180178ca0 (phrase), 0x180178e20 (intonation phrase), 0x180178bb0 (sentence). */
#include <stdlib.h>
#include <string.h>
#include "zf2_int.h"

enum { L_PHONE = 1, L_SYL = 4, L_WORD = 8, L_PW = 0x10, L_PHRASE = 0x40, L_IP = 0x80, L_SENT = 0x200 };

static const struct { const char *name; int level, dir, skip; } LOC[] = {
    {"Phone", 1, 0, 0}, {"PrevPhone", 1, -1, 0}, {"NextPhone", 1, 1, 0}, {"FirstPhone", 1, -1, 0}, {"LastPhone", 1, 1, 0},
    {"Syllable", 4, 0, 0}, {"PrevSyllable", 4, -1, 1}, {"NextSyllable", 4, 1, 1}, {"FirstSyllable", 4, -1, 0},
    {"LastSyllable", 4, 1, 0}, {"Word", 8, 0, 0}, {"PrevWord", 8, -1, 1}, {"NextWord", 8, 1, 1}, {"FirstWord", 8, -1, 0},
    {"LastWord", 8, 1, 0}, {"Token", 8, 0, 0}, {"PrevToken", 8, -1, 0}, {"NextToken", 8, 1, 0}, {"FirstToken", 8, -1, 0},
    {"LastToken", 8, 1, 0}, {"ProsodicWord", 0x10, 0, 0}, {"PrevProsodicWord", 0x10, -1, 1},
    {"NextProsodicWord", 0x10, 1, 1}, {"FirstProsodicWord", 0x10, -1, 0}, {"LastProsodicWord", 0x10, 1, 0},
    {"Phrase", 0x40, 0, 0}, {"PrevPhrase", 0x40, -1, 1}, {"NextPhrase", 0x40, 1, 1}, {"FirstPhrase", 0x40, -1, 0},
    {"LastPhrase", 0x40, 1, 0}, {"IntonationPhrase", 0x80, 0, 0}, {"PrevIntonationPhrase", 0x80, -1, 1},
    {"NextIntonationPhrase", 0x80, 1, 1}, {"FirstIntonationPhrase", 0x80, -1, 0}, {"LastIntonationPhrase", 0x80, 1, 0},
    {"Sentence", 0x200, 0, 0}};

/* properties (table 0x18016f280) that are implemented */
enum {
    P_PhoneIdentity = 0x00, P_PhoneNumber = 0x01, P_SyllableNumber = 0x02, P_WordNumber = 0x03, P_PWNumber = 0x04,
    P_PhraseNumber = 0x05, P_IPNumber = 0x06, P_FwPosInSyllable = 0x07, P_BwPosInSyllable = 0x08, P_FwPosInWord = 0x09,
    P_BwPosInWord = 0x0a, P_FwPosInPhrase = 0x0d, P_BwPosInPhrase = 0x0e, P_FwPosInSentence = 0x11,
    P_BwPosInSentence = 0x12, P_Stress = 0x15, P_Accent = 0x16, P_POS = 0x17, P_POSTaggerPOS = 0x18,
    P_ToBIFinalBoundaryTone = 0x19, P_BreakIndex = 0x2a, P_SentenceType = 0x2e, P_FwPosInYesNoQuestion = 0x2f,
    P_BwPosInYesNoQuestion = 0x30, P_Emphasis = 0x4c, P_ToBIAccent = 0x4f, P_SentenceEmotion = 0x54, P_Tone = 0x29
};
static const struct { const char *name; int id; } PROP[] = {
    {"PhoneIdentity", P_PhoneIdentity}, {"PhoneNumber", P_PhoneNumber}, {"SyllableNumber", P_SyllableNumber},
    {"WordNumber", P_WordNumber}, {"ProsodicWordNumber", P_PWNumber}, {"PhraseNumber", P_PhraseNumber},
    {"IntonationPhraseNumber", P_IPNumber}, {"FwPosInSyllable", P_FwPosInSyllable},
    {"BwPosInSyllable", P_BwPosInSyllable}, {"FwPosInWord", P_FwPosInWord}, {"BwPosInWord", P_BwPosInWord},
    {"FwPosInPhrase", P_FwPosInPhrase}, {"BwPosInPhrase", P_BwPosInPhrase}, {"FwPosInSentence", P_FwPosInSentence},
    {"BwPosInSentence", P_BwPosInSentence}, {"Stress", P_Stress}, {"Accent", P_Accent}, {"POS", P_POS},
    {"POSTaggerPOS", P_POSTaggerPOS}, {"ToBIFinalBoundaryTone", P_ToBIFinalBoundaryTone},
    {"BreakIndex", P_BreakIndex}, {"SentenceType", P_SentenceType}, {"FwPosInYesNoQuestion", P_FwPosInYesNoQuestion},
    {"BwPosInYesNoQuestion", P_BwPosInYesNoQuestion}, {"Emphasis", P_Emphasis}, {"ToBIAccent", P_ToBIAccent},
    {"SentenceEmotion", P_SentenceEmotion}, {"Tone", P_Tone}};

typedef struct { int level, i; } Node; /* i = -1: none */

static int count_of(const Z2Sent *s, int level)
{
    switch (level) {
    case L_PHONE: return s->np;
    case L_SYL: return s->ns;
    case L_WORD: return s->nw;
    case L_PW: return s->npw;
    case L_PHRASE: return s->nph;
    case L_IP: return s->nip;
    case L_SENT: return 1;
    }
    return 0;
}
/* second-vtable slot 1 ("is empty" / skip) of each node class */
static int node_skip(const Z2Sent *s, int level, int i)
{
    switch (level) {
    case L_PHONE: return z2_phone_is_sil(s, s->p[i].id);        /* thunk FUN_18001d540 */
    case L_SYL: return z2_syl_is_sil(s, i);                        /* thunk FUN_18001a40c */
    case L_WORD: return z2_word_skip(&s->w[i]);                    /* FUN_18001a070 */
    case L_PW: {                                                   /* FUN_18014c650: one word, a break token */
        const Z2PW *p = &s->pw[i];
        return p->w0 == p->w1 && z2_is_break(&s->w[p->w0]);
    }
    case L_PHRASE: return z2_phrase_is_break(s, i);                /* FUN_1800520d4 */
    case L_IP: return z2_ip_is_break(s, i);                        /* FUN_18014c4b0 */
    }
    return 0;
}
static int parent_of(const Z2Sent *s, int level, int i, int *plevel)
{
    switch (level) {
    case L_PHONE: *plevel = L_SYL; return s->p[i].syl;
    case L_SYL: *plevel = L_WORD; return s->s[i].word;
    case L_WORD: *plevel = L_PHRASE; return s->w[i].phrase;
    case L_PHRASE: *plevel = L_IP; return s->ph[i].ip;
    case L_IP: *plevel = L_SENT; return 0;
    case L_PW: *plevel = L_PHRASE; return s->pw[i].ph;
    }
    *plevel = 0;
    return -1;
}
static int move(const Z2Sent *s, int level, int i, int dir, int skip)
{
    int n = count_of(s, level);
    while (dir != 0 && i >= 0) {
        int step = dir > 0 ? 1 : -1;
        i += step;
        while (i >= 0 && i < n && skip && node_skip(s, level, i)) i += step;
        if (i < 0 || i >= n) return -1;
        dir -= step;
    }
    return i;
}
static Node step(const Z2Sent *s, Node x, int L, int dir, int skip)
{
    Node r = {L, -1};
    if (x.i < 0) return r;
    if (L == x.level) { r.i = move(s, L, x.i, dir, skip); return r; }
    if (L > x.level) {
        int lv = x.level, i = x.i;
        if (L == L_PW) { /* word -> prosodic word (word+0x2c0) */
            if (lv == L_PHONE) { i = s->p[i].syl; lv = L_SYL; }
            if (lv == L_SYL) { i = s->s[i].word; lv = L_WORD; }
            if (lv != L_WORD || i < 0) return r;
            r.i = move(s, L_PW, s->w[i].pw, dir, skip);
            return r;
        }
        while (lv < L && i >= 0) {
            int pl;
            i = parent_of(s, lv, i, &pl);
            lv = pl;
            if (lv == L_PW) { i = s->pw[i].ph; lv = L_PHRASE; }
        }
        if (lv != L || i < 0) return r;
        r.i = move(s, L, i, dir, skip);
        return r;
    }
    /* down: first (dir -1) / last (+1) child */
    switch (x.level) {
    case L_SYL:
        if (L == L_PHONE) r.i = dir < 0 ? s->s[x.i].p0 : s->s[x.i].p0 + s->s[x.i].np - 1;
        break;
    case L_WORD:
        if (L == L_SYL && s->w[x.i].ns) r.i = dir < 0 ? s->w[x.i].s0 : s->w[x.i].s0 + s->w[x.i].ns - 1;
        break;
    case L_PHRASE:
        if (L == L_WORD) r.i = dir < 0 ? s->ph[x.i].w0 : s->ph[x.i].w1;
        else if (L == L_PW) r.i = dir < 0 ? s->ph[x.i].pw0 : s->ph[x.i].pw1;
        break;
    case L_IP:
        if (L == L_PHRASE) r.i = dir < 0 ? s->ip[x.i].ph0 : s->ip[x.i].ph1;
        break;
    case L_SENT:
        if (L == L_IP) r.i = dir < 0 ? s->first_ip : s->last_ip;
        break;
    }
    return r;
}

/* ---- getters ---- */
#define NUL(v) ((v)->kind = -1, (v)->value = -1, 0)
#define INT(v, x) ((v)->kind = 0, (v)->value = (x), 0)
#define ENU(v, x) ((v)->kind = 1, (v)->value = (x), 0)

static int syl_stress(const Z2Sent *s, int si)
{
    const Z2S *y = &s->s[si];
    int k, m = 0;
    for (k = 0; k < y->np; k++) {
        int st = s->p[y->p0 + k].st, v = st ? st - s->v->ps.st1 + 1 : 0;
        if (v > m) m = v;
    }
    return m;
}
static int word_first_syl(const Z2Sent *s, int wi) { return s->w[wi].ns ? s->w[wi].s0 : -1; }
static int word_last_syl(const Z2Sent *s, int wi) { return s->w[wi].ns ? s->w[wi].s0 + s->w[wi].ns - 1 : -1; }
static int nonbreak_phrases_between(const Z2Sent *s, int a, int b)
{
    int k, n = 0;
    for (k = a; k <= b; k++) if (!z2_phrase_is_break(s, k)) n++;
    return n;
}
static int rw_between(const Z2Sent *s, int a, int b)
{   /* FUN_180048a10 with predicate FUN_18001a180 */
    int k, n = 0;
    for (k = a; k <= b; k++) if (z2_is_rw(&s->w[k])) n++;
    return n;
}

static int get_phone(const Z2Sent *s, int i, int prop, zf2_value *v)
{
    const Z2P *p = &s->p[i];
    const Z2S *y = &s->s[p->syl];
    int wi = y->word;
    switch (prop) {
    case P_PhoneIdentity: return INT(v, p->id);
    case P_FwPosInSyllable: if (z2_phone_is_sil(s, p->id)) return NUL(v); return INT(v, i - y->p0 + 1);
    case P_BwPosInSyllable: if (z2_phone_is_sil(s, p->id)) return NUL(v); return INT(v, y->p0 + y->np - i);
    case P_FwPosInWord: {
        int fs = word_first_syl(s, wi);
        if (z2_phone_is_sil(s, p->id)) return NUL(v);
        return INT(v, i - s->s[fs].p0 + 1);
    }
    case P_BwPosInWord: {
        int ls = word_last_syl(s, wi);
        if (z2_phone_is_sil(s, p->id)) return NUL(v);
        return INT(v, s->s[ls].p0 + s->s[ls].np - i);
    }
    }
    return -1;
}
static int get_syl(const Z2Sent *s, int i, int prop, zf2_value *v)
{
    const Z2S *y = &s->s[i];
    const Z2W *w = &s->w[y->word];
    if (z2_syl_is_sil(s, i)) {
        switch (prop) {
        case P_Stress: case P_Accent: case P_BreakIndex: case P_PhoneNumber: case P_FwPosInWord: case P_BwPosInWord:
        case P_FwPosInPhrase: case P_BwPosInPhrase: case P_ToBIAccent: return NUL(v);
        }
        return -1;
    }
    switch (prop) {
    case P_Stress: return ENU(v, syl_stress(s, i));
    case P_Accent: return ENU(v, syl_stress(s, i) != 0 && w->emph == 1 ? 1 : 0);
    case P_BreakIndex: return INT(v, i == word_last_syl(s, y->word) ? w->bi : 1);
    case P_PhoneNumber: return INT(v, y->np);
    case P_FwPosInWord: return INT(v, i - w->s0 + 1);
    case P_BwPosInWord: return INT(v, w->s0 + w->ns - i);
    case P_ToBIAccent: return ENU(v, y->tobi);
    case P_FwPosInPhrase: case P_BwPosInPhrase: {
        const Z2PH *ph;
        int k, a = -1, b = -1;
        if (w->phrase < 0) return -1;
        ph = &s->ph[w->phrase];
        if (prop == P_FwPosInPhrase) { /* FUN_180019754: first syllable of the phrase's first word with syllables */
            for (k = ph->w0; k <= ph->w1; k++) if (z2_has_syl_pron(&s->w[k])) { a = s->w[k].s0; break; }
            if (a < 0 || a > i) return -1;
            return INT(v, i - a + 1);
        }
        for (k = ph->w1; k >= ph->w0; k--) if (z2_has_syl_pron(&s->w[k])) { b = word_last_syl(s, k); break; }
        if (b < 0 || b < i) return -1;
        return INT(v, b - i + 1);
    }
    }
    return -1;
}
static int get_word(const Z2Sent *s, int i, int prop, zf2_value *v)
{
    const Z2W *w = &s->w[i];
    if (z2_is_break(w)) {
        switch (prop) {
        case P_POS: case P_SyllableNumber: case P_FwPosInPhrase: case P_BwPosInPhrase: case P_FwPosInYesNoQuestion:
        case P_BwPosInYesNoQuestion: case P_BreakIndex: case P_POSTaggerPOS: case P_Emphasis: case P_ToBIFinalBoundaryTone:
            return NUL(v);
        }
        return -1;
    }
    switch (prop) {
    case P_POS: return INT(v, w->pos);
    case P_POSTaggerPOS: return INT(v, w->tpos);
    case P_SyllableNumber: return INT(v, w->ns);
    case P_BreakIndex: return INT(v, w->bi);
    case P_Emphasis: return INT(v, w->emph);
    case P_ToBIFinalBoundaryTone: return ENU(v, w->tone);
    case P_FwPosInPhrase:
        if (w->phrase < 0) return -1;
        return INT(v, rw_between(s, s->ph[w->phrase].w0, i));
    case P_BwPosInPhrase:
        if (w->phrase < 0) return -1;
        return INT(v, rw_between(s, i, s->ph[w->phrase].w1));
    case P_FwPosInYesNoQuestion: case P_BwPosInYesNoQuestion: {
        int ph = w->phrase, pos;
        v->kind = -1; v->value = -1;
        if (ph < 0 || s->ph[ph].ip < 0) return -1;
        if (s->type != 1) return 0;
        if (z2_phrase_is_break(s, ph)) return 0;
        if (prop == P_FwPosInYesNoQuestion) {
            if (s->first_ip < 0 || nonbreak_phrases_between(s, s->ip[s->first_ip].ph0, ph) != 1) return 0;
            pos = rw_between(s, s->ph[ph].w0, i);
        } else {
            if (s->last_ip < 0 || nonbreak_phrases_between(s, ph, s->ip[s->last_ip].ph1) != 1) return 0;
            pos = rw_between(s, i, s->ph[ph].w1);
        }
        if ((unsigned)pos > 3) return 0;
        return INT(v, pos);
    }
    }
    return -1;
}
static int phrase_syls(const Z2Sent *s, int phi)
{
    int k, n = 0;
    for (k = s->ph[phi].w0; k <= s->ph[phi].w1; k++) if (z2_is_rw(&s->w[k])) n += s->w[k].ns;
    return n;
}
static int get_phrase(const Z2Sent *s, int i, int prop, zf2_value *v)
{
    const Z2PH *p = &s->ph[i];
    if (z2_phrase_is_break(s, i)) {
        switch (prop) {
        case P_SyllableNumber: case P_WordNumber: case P_FwPosInSentence: case P_BwPosInSentence:
        case P_ToBIFinalBoundaryTone: return NUL(v);
        }
        return -1;
    }
    switch (prop) {
    case P_SyllableNumber: return INT(v, phrase_syls(s, i));
    case P_WordNumber: return INT(v, rw_between(s, p->w0, p->w1));
    case P_FwPosInSentence:
        if (s->first_ip < 0) return -1;
        return INT(v, nonbreak_phrases_between(s, s->ip[s->first_ip].ph0, i));
    case P_BwPosInSentence:
        if (s->last_ip < 0) return -1;
        return INT(v, nonbreak_phrases_between(s, i, s->ip[s->last_ip].ph1));
    case P_ToBIFinalBoundaryTone: return ENU(v, z2_is_rw(&s->w[p->w1]) ? s->w[p->w1].tone : 0);
    }
    return -1;
}
static int get_sent(const Z2Sent *s, int prop, zf2_value *v)
{
    int k, n = 0;
    switch (prop) {
    case P_SyllableNumber:
        for (k = 0; k < s->ns; k++) if (!z2_syl_is_sil(s, k)) n++;
        return INT(v, n);
    case P_WordNumber:
        for (k = 0; k < s->nw; k++) if (z2_is_rw(&s->w[k])) n++;
        return INT(v, n);
    case P_PhraseNumber:
        for (k = 0; k < s->nph; k++) if (!z2_phrase_is_break(s, k)) n++;
        return INT(v, n);
    case P_SentenceType: return ENU(v, s->type);
    case P_SentenceEmotion: return ENU(v, s->emotion);
    }
    return -1;
}
static int get_value(const Z2Sent *s, Node x, int prop, zf2_value *v)
{
    switch (x.level) {
    case L_PHONE: return get_phone(s, x.i, prop, v);
    case L_SYL: return get_syl(s, x.i, prop, v);
    case L_WORD: return get_word(s, x.i, prop, v);
    case L_PHRASE: return get_phrase(s, x.i, prop, v);
    case L_SENT: return get_sent(s, prop, v);
    }
    return -1;
}

/* the engine matches path tokens case-insensitively (Eva's NNM spells them in lower case) */
static int z2_icmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a, y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (x != y) return x - y;
    }
    return *a - *b;
}

typedef struct { int nstep; int lv[16], dir[16], skip[16]; int prop; } Path;
static int parse_path(const char *path, Path *p)
{
    char tok[64];
    const char *a = path;
    int k;
    p->nstep = 0;
    p->prop = -1;
    for (;;) {
        const char *e = strchr(a, '.');
        size_t n = e ? (size_t)(e - a) : strlen(a);
        if (n >= sizeof tok) return -1;
        memcpy(tok, a, n);
        tok[n] = 0;
        if (!e) {
            for (k = 0; k < (int)(sizeof PROP / sizeof PROP[0]); k++)
                if (!z2_icmp(PROP[k].name, tok)) { p->prop = PROP[k].id; return 0; }
            return -1;
        }
        for (k = 0; k < (int)(sizeof LOC / sizeof LOC[0]); k++)
            if (!z2_icmp(LOC[k].name, tok)) break;
        if (k == (int)(sizeof LOC / sizeof LOC[0]) || p->nstep >= 16) return -1;
        p->lv[p->nstep] = LOC[k].level;
        p->dir[p->nstep] = LOC[k].dir;
        p->skip[p->nstep] = LOC[k].skip;
        p->nstep++;
        a = e + 1;
    }
}

int zf2_nphones(const zf2_sent *s) { return s->np; }

int zf2_eval_path(const zf2_sent *s, const char *path, zf2_value *vals)
{
    Path p;
    int i, k;
    if (parse_path(path, &p) || p.nstep < 1 || p.lv[0] != L_PHONE || p.dir[0] != 0) return -1;
    for (i = 0; i < s->np; i++) {
        Node x = {L_PHONE, i};
        vals[i].kind = -1;
        vals[i].value = -1;
        for (k = 1; k < p.nstep && x.i >= 0; k++) x = step(s, x, p.lv[k], p.dir[k], p.skip[k]);
        if (x.i < 0) continue;
        if (get_value(s, x, p.prop, &vals[i]) < 0) return -1;
    }
    return 0;
}

int zf2_features(const zf2_sent *s, ZbUtt *u)
{
    int f, i;
    zf2_value *vals = (zf2_value *)malloc(sizeof(zf2_value) * (size_t)(s->np ? s->np : 1));
    u->nphone = s->np;
    u->ph = (ZbPhone *)calloc((size_t)(s->np ? s->np : 1), sizeof(ZbPhone));
    for (f = 0; f < ZB_NFEAT; f++) {
        if (zf2_eval_path(s, zb_feature_path[f], vals)) {
            for (i = 0; i < s->np; i++) u->ph[i].f[f] = ZB_NULL;
            continue;
        }
        for (i = 0; i < s->np; i++) u->ph[i].f[f] = vals[i].kind < 0 ? ZB_NULL : vals[i].value;
    }
    free(vals);
    {
        int lastw = -1, nwseen = -1;
        for (i = 0; i < s->np; i++) {
            ZbPhoneCtl *c = &u->ph[i].c;
            const Z2S *y = &s->s[s->p[i].syl];
            const Z2W *w = &s->w[y->word];
            if (y->word != lastw) { lastw = y->word; nwseen++; }
            c->volume = 100;
            c->is_sil = z2_phone_is_sil(s, s->p[i].id);
            c->is_pause = s->v->short_pause >= 0 && s->p[i].id == s->v->short_pause; /* FUN_18004d844 */
            c->word = nwseen;         /* owning word, numbered in order of appearance (as harness/zbtap) */
            c->word_type = w->bi;     /* word vt+0x1b0 = BreakIndex (zb.h calls it word_type) */
            c->domain[0] = 0;         /* segment domain = the word's entity domain, "none" = no domain */
            if (w->domain) {
                int k;
                for (k = 0; w->domain[k] && k < 31; k++) c->domain[k] = (char)w->domain[k];
                c->domain[k] = 0;
                if (!strcmp(c->domain, "none")) c->domain[0] = 0;
            }
        }
    }
    /* sentence type for Zira's question intonation: +0x228 of the utterance's last word node, which is the
       final punctuation word ("?": 3 = yes/no question, 5 also yes/no, 4 = wh-question), read at the last phone */
    if (s->np > 0 && s->nw > 0) u->ph[s->np - 1].c.sent_type = s->w[s->nw - 1].i228;
    z2_fill_ctl(s, u);
    return 0;
}

void zf2_utt_free(ZbUtt *u)
{
    free(u->ph);
    u->ph = NULL;
    u->nphone = 0;
}
