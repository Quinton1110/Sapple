/* Word list of the Anna frontend (MSTTSFrontendENU.dll), one sentence:
 *   0x0D5E8071  word records from the items (punctuation codes, brackets/quotes 0x0D5E7713/777B/7683,
 *               silences 0x0D5E6511, SAPI state 0x0D5E4552, phone codes 0x0D5E43AB, POS class 0x0D5E45E3)
 *   0x0D5E7E2D  prosody of number/time/currency/phone items (0x0D5E6947 digit groups, 0x0D5E4BF9 digits,
 *               0x0D5E7AA7 fractions, 0x0D5E7937 currency, 0x0D5E6658 clock times, 0x0D5E77D0 phones)
 *   0x0D5E4C79  sentence span
 *   0x0D5E6BE8  focus word of '!' sentences (0x0D5E4CEF) and emphasis breaks
 *   0x0D5E6D74  phrase breaks (rules on POS and words since the last break)
 *   0x0D5E6112  accents (runs of POS classes, msvcrt rand()), boundary tones, emphasis
 * plus the public anna_mid_* API (anna_lex.h).
 */
#include "anna_lex.h"
#include "anna_norm_types.h"

#include <stdio.h>
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

#define SIL 0x11

struct anna_mid {
    anna_lexicon *lex;
    anna_lts *lts;
    uint32_t rnd;           /* msvcrt per-thread rand() state (_holdrand, 1 at thread start) */
    float f38, f44, f78, f7c; /* frontend +0x38, +0x44 rate factor, +0x78 pitch offset, +0x7c pitch range */
    int in_quote, in_paren; /* +0x74, +0x75 */
    anna_msent last;        /* the last sentence (work records for tests) */
    int have_last;
};

/* ------------------------------------------------------------------------------------------------ */
/* word list                                                                                        */

typedef struct {
    anna_word *w;
    int n, cap;
} wlist;

static anna_word *wl_insert(wlist *l, int at)
{
    anna_word *w;
    if (l->n == l->cap) {
        int nc = l->cap ? 2 * l->cap : 64;
        anna_word *p = (anna_word *)realloc(l->w, sizeof *p * (size_t)nc);
        if (!p) return NULL;
        l->w = p;
        l->cap = nc;
    }
    if (at < 0) at = 0;
    if (at > l->n) at = l->n;
    memmove(l->w + at + 1, l->w + at, sizeof *l->w * (size_t)(l->n - at));
    l->n++;
    w = &l->w[at];
    /* constructor 0x0D5DDAFC */
    memset(w, 0, sizeof *w);
    w->st.volume = 100;
    w->rate_mul = 1.0f;
    w->range = 1.0f;
    return w;
}

static int nx(const wlist *l, int i) { return i >= 0 && i + 1 < l->n ? i + 1 : -1; }
static int pv(const wlist *l, int i) { return i > 0 && i < l->n ? i - 1 : -1; }

static uint32_t msrand(anna_mid *m)
{
    m->rnd = m->rnd * 214013u + 2531011u;
    return (m->rnd >> 16) & 0x7fff;
}

/* 0x0D5E45E3 */
static int pos_class(uint32_t p)
{
    if (p == 0x1000 || p == 0x2000 || p == 0x3001 || p == 0x3002 || p == 0x5000) return 2;
    if ((p >= 0x1001 && p <= 0x1004) || p == 0x3000 || p == 0x4000 || (p >= 0x4003 && p <= 0x4007) || p == 0x4009) return 1;
    if (p == 0x4001) return 3;
    return 0;
}

/* 0x0D5E43AB: SAPI phone ids -> internal codes (unknown ids dropped, at most 0x17f) */
static int to_internal(const uint16_t *p, unsigned char *out)
{
    int n = 0, i, u;
    if (!p) return 0;
    for (i = 0; p[i]; i++) {
        for (u = 0; u < 0x2f; u++)
            if (PHONE_MAP[u] == p[i]) {
                if (n < 0x180) out[n] = (unsigned char)u;
                n++;
                break;
            }
        if (n > 0x17e) return 0x17f;
    }
    return n;
}

static void set_text(anna_word *w, const uint16_t *s, int n)
{
    uint16_t tmp[20];
    int i;
    if (n > 19) n = 19;
    for (i = 0; i < n; i++) tmp[i] = s[i];
    tmp[n] = 0;
    anna_w_to_utf8(tmp, w->text, sizeof w->text);
}

/* 0x0D5E4367 */
static float rate_factor(int r)
{
    if (r < 0) {
        if (r < -18) r = -18;
        return (float)(1.0 / RATE_TAB[-r]);
    }
    if (r > 18) r = 18;
    return RATE_TAB[r];
}

/* 0x0D5E4552: SAPI state of the fragment */
static void apply_state(const anna_mid *m, const anna_token *t, anna_word *w)
{
    float rf;
    double f;
    w->st.volume = t->st.volume;
    w->st.rate = t->st.rate;
    w->st.pitch = t->st.pitch;
    w->st.emph = t->st.emph;
    w->emph = t->st.emph;
    rf = rate_factor(t->st.rate);
    w->rate_f = rf;
    f = (double)m->f38 * (double)rf * (double)m->f44;
    if (0.33329999446868896 <= f) {
        w->pause_f = 0.0f;
    } else {
        w->pause_f = (float)(0.05000000074505806 / f);
        w->rate_f = 0.3333f;
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* number prosody 0x0D5E7E2D (positions are list indices; -1 is the engine's NULL list node)        */

static void unstress(anna_word *w)
{
    if (w->acc_type == 0) {
        w->acc_type = 0;
        w->acc_var = 5;
    }
}

static void accent(anna_mid *m, anna_word *w, int rule)
{
    w->acc_type = 1;
    w->acc_var = (int)(msrand(m) & 3) + 4;
    w->rule1 = rule;
}

/* 0x0D5E6580 (after i; at the end when i < 0) / 0x0D5E65EC (before i; at the start when i < 0) */
static int insert_break(anna_mid *m, wlist *l, int at)
{
    anna_word *w = wl_insert(l, at);
    if (!w) return -1;
    w->text[0] = '+';
    w->len = 1;
    w->nph = 1;
    w->ph[0] = SIL;
    w->pitch_off = m->f78;
    w->range = m->f7c;
    w->rate_mul = m->f44;
    w->rate_f = 0.0f;
    return at < 0 ? 0 : (at > l->n - 1 ? l->n - 1 : at);
}
static int sil_after(anna_mid *m, wlist *l, int i) { return insert_break(m, l, i < 0 ? l->n : i + 1); }
static int sil_before(anna_mid *m, wlist *l, int i) { return insert_break(m, l, i < 0 ? 0 : i); }

/* 0x0D5E4BF9: digits read one by one, every second one accented */
static int digits(anna_mid *m, wlist *l, int pos, int n)
{
    for (; n > 1; n -= 2) {
        int nxt = -1;
        if (pos >= 0) {
            nxt = nx(l, pos);
            accent(m, &l->w[pos], 0x15);
        }
        pos = nxt >= 0 ? nx(l, nxt) : -1;
    }
    if (n > 0 && pos >= 0) pos = nx(l, pos);
    return pos;
}

/* 0x0D5E6947: integer groups (the first word of every group accented, a break after scale words) */
static void int_groups(anna_mid *m, wlist *l, int *ppos, const anna_numinfo *ni, int *cnt)
{
    const anna_intpart *ip = ni->ip;
    int cur, nxt, k;
    if (ip->v[0x1a]) {
        *ppos = digits(m, l, *ppos, ip->v[0x1b]);
        *cnt -= ip->v[0x1b];
        return;
    }
    cur = *ppos;
    if (cur < 0) return; /* the engine would crash */
    nxt = nx(l, cur);
    l->w[cur].acc_type = 0;
    l->w[cur].acc_var = 5;
    if (ni->neg) {
        if (nxt >= 0) {
            cur = nxt;
            nxt = nx(l, cur);
            l->w[cur].acc_type = 0;
            l->w[cur].acc_var = 5;
        }
        (*cnt)--;
    }
#define ADVANCE()                                                                                             \
    do {                                                                                                      \
        if (nxt >= 0) {                                                                                       \
            cur = nxt;                                                                                        \
            nxt = nx(l, cur);                                                                                 \
            unstress(&l->w[cur]);                                                                             \
        }                                                                                                     \
        (*cnt)--;                                                                                             \
    } while (0)
    for (k = ip->v[0] - 1; k >= 0; k--) {
        const int *f = &ip->v[4 * k + 1]; /* ones, tens, hundred, scale word */
        int acc_word = cur;
        accent(m, &l->w[acc_word], 0x14);
        if (f[2]) {
            ADVANCE();
            ADVANCE();
        }
        if (f[1]) ADVANCE();
        if (f[0]) ADVANCE();
        if (f[3]) {
            int s = sil_after(m, l, cur);
            if (s < 0) break;
            l->w[s].type = 0x11;
            l->w[s].punct = 0x13;
            l->w[s].rule2 = 0x13;
            unstress(&l->w[cur]);
            cur = s;
            nxt = nx(l, s);
            if (nxt >= 0) {
                cur = nxt;
                nxt = nx(l, cur);
            }
            (*cnt)--;
        }
    }
#undef ADVANCE
    *ppos = cur;
}

/* 0x0D5E6813: a pause before pos, then on to the next word that is not a pause */
static int pause_before(anna_mid *m, wlist *l, int pos, int rule2, int type)
{
    int s = sil_before(m, l, pos), cur;
    if (s < 0) return pos;
    l->w[s].type = type;
    l->w[s].punct = 10;
    l->w[s].rule2 = rule2;
    l->w[s].rule1 = 0x20;
    cur = s;
    while (l->w[cur].ph[0] == SIL && nx(l, cur) >= 0) cur = nx(l, cur);
    return cur;
}

/* 0x0D5E7AA7: fractions */
static void fraction(anna_mid *m, wlist *l, int *ppos, const anna_numinfo *ni, int *cnt)
{
    const anna_fracpart *fr = ni->fr;
    int s;
    if (fr->num->ip) int_groups(m, l, ppos, fr->num, cnt);
    if (fr->num->dp) {
        if (*ppos >= 0) *ppos = nx(l, *ppos);
        *ppos = digits(m, l, *ppos, fr->num->dp->n);
    }
    if (fr->over == 0) *ppos = *ppos < 0 ? l->n - 1 : pv(l, *ppos);
    s = sil_before(m, l, *ppos);
    if (s >= 0) {
        l->w[s].type = 0xf;
        l->w[s].punct = 0x13;
        l->w[s].rule2 = 0x15;
        *ppos = nx(l, s);
    }
    if (fr->den->ip) {
        int node = *ppos;
        if (node >= 0) {
            *ppos = nx(l, node);
            if (l->w[node].acc_type == 0) {
                l->w[node].acc_type = 0;
                l->w[node].acc_var = 5;
                l->w[node].pos = 0x1000;
                l->w[node].pos_class = 2;
            }
            int_groups(m, l, ppos, fr->den, cnt);
        }
    }
    if (fr->den->dp && *ppos >= 0) {
        *ppos = nx(l, *ppos);
        *ppos = digits(m, l, *ppos, fr->den->dp->n);
    }
}

/* 0x0D5E7937: currencies */
static void currency(anna_mid *m, wlist *l, int pos, const anna_currinfo *ci, int count)
{
    int cnt = count, p = pos, rec = -1, nxt, s;
    if (!ci->num || ci->num->type != 0x1006) return;
    if (ci->num->ip) int_groups(m, l, &p, ci->num, &cnt);
    if (cnt < 2) return;
    nxt = p;
    if (ci->scale) {
        if (p >= 0) {
            rec = p;
            nxt = nx(l, p);
        }
        cnt--;
    }
    if (cnt < 2) return;
    s = sil_after(m, l, p);
    if (s < 0) return;
    l->w[s].type = 0x10;
    l->w[s].punct = 0x13;
    l->w[s].rule2 = 0x14;
    rec = s;
    nxt = nx(l, s);
    p = s;
    if (ci->cents) {
        int q = nxt;
        if (nxt >= 0) {
            rec = nxt;
            q = nx(l, nxt);
            p = nxt;
        }
        if (q >= 0) {
            p = q;
            if (l->w[rec].acc_type == 0) {
                l->w[rec].acc_type = 0;
                l->w[rec].acc_var = 5;
                l->w[rec].pos = 0x1000;
                l->w[rec].pos_class = 2;
            }
        }
        cnt -= 2;
        if (ci->cents->ip) int_groups(m, l, &p, ci->cents, &cnt);
    }
}

/* 0x0D5E6658: clock times */
static void clock_time(anna_mid *m, wlist *l, int pos, const anna_timeinfo *ti)
{
    int s, nxt;
    if (ti->hundred || pos < 0) return;
    accent(m, &l->w[pos], 0x1d);
    s = sil_after(m, l, pos);
    if (s < 0) return;
    l->w[s].type = 9;
    l->w[s].punct = 0x13;
    l->w[s].rule2 = 0x1a;
    nxt = nx(l, s);
    if (ti->minutes && nxt >= 0) accent(m, &l->w[nxt], 0x1e);
    if (ti->ampm) {
        int s2 = sil_before(m, l, l->n - 2);
        if (s2 >= 0) {
            l->w[s2].type = 10;
            l->w[s2].punct = 0xb;
            l->w[s2].rule2 = 0x1b;
        }
        accent(m, &l->w[l->n - 1], 0x1f);
    }
}

/* 0x0D5E77D0: phone numbers */
static void phone_number(anna_mid *m, wlist *l, int pos, const anna_phoneinfo *pi, int count)
{
    int g, s, cnt = count;
    if (pi->country) {
        pos = nx(l, nx(l, pos));
        if (pi->country->ip) int_groups(m, l, &pos, pi->country, &cnt);
        pos = pause_before(m, l, pos, 0x16, 0xb);
    }
    if (pi->one && pos >= 0) {
        pos = nx(l, pos);
        pos = pause_before(m, l, pos, 0x18, 0xd);
    }
    if (pi->area) {
        int skip = 0;
        if (!pi->is800) {
            if (pos < 0 || nx(l, pos) < 0) skip = 1;
            else pos = digits(m, l, nx(l, nx(l, pos)), pi->area->n);
        } else {
            if (pos < 0) skip = 1;
            else pos = nx(l, nx(l, pos));
        }
        if (!skip && pos >= 0) pos = pause_before(m, l, pos, 0x17, 0xc);
    }
    for (g = 0; g < pi->ngroups; g++) {
        pos = digits(m, l, pos, pi->groups[g].n);
        if (pos >= 0) pos = pause_before(m, l, pos, 0x19, 0xe);
    }
    s = sil_after(m, l, -1); /* 0x0D5E68BF */
    if (s >= 0) {
        l->w[s].type = 0xe;
        l->w[s].punct = 10;
        l->w[s].rule2 = 0x19;
        l->w[s].rule1 = 0x20;
    }
}

static void item_prosody(anna_mid *m, wlist *l, int first, const anna_mnode *n)
{
    const int *ti = n->ti;
    int type = ti ? *ti : n->type, pos = first, cnt = n->nw;
    if (!ti || pos < 0) return;
    switch (type) {
    case 0x1010:
    case 0x1011: {
        const anna_numinfo *ni = (const anna_numinfo *)((const anna_ctxinfo *)ti)->inner;
        if (!ni || ni->type == 0x1014) return;
        if (ni->ip) int_groups(m, l, &pos, ni, &cnt);
        if (ni->dp) digits(m, l, pos, ni->dp->n);
        return;
    }
    case 0x1006: case 0x1007: case 0x1008: case 0x100f: {
        const anna_numinfo *ni = (const anna_numinfo *)ti;
        if (ni->ip) int_groups(m, l, &pos, ni, &cnt);
        if (ni->dp && pos >= 0) pos = digits(m, l, nx(l, pos), ni->dp->n);
        if (!ni->fr || pos < 0) return;
        {
            int nxt = nx(l, pos);
            anna_word *w = &l->w[pos];
            if (w->acc_type == 0) {
                w->acc_type = 0;
                w->acc_var = 5;
                w->pos = 0x1000;
                w->pos_class = 2;
            }
            cnt = n->nw;
            pos = nxt;
        }
        fraction(m, l, &pos, ni, &cnt);
        return;
    }
    case 0x100e:
        fraction(m, l, &pos, (const anna_numinfo *)ti, &cnt);
        return;
    case 0x100d:
        currency(m, l, pos, (const anna_currinfo *)ti, cnt);
        return;
    case 0x1018:
        clock_time(m, l, pos, (const anna_timeinfo *)ti);
        return;
    case 0x1012:
    case 0x1027:
        phone_number(m, l, pos, (const anna_phoneinfo *)ti, cnt);
        return;
    default:
        return;
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* word records 0x0D5E8071                                                                          */

/* 0x0D5E6511: a silence record (brackets, quotes, SAPI <silence>) */
static anna_word *add_silence(anna_mid *m, wlist *l, const anna_mnode *n, int ms)
{
    anna_word *w = wl_insert(l, l->n);
    if (!w) return NULL;
    if (ms > 0) w->silence_ms = ms;
    w->nph = 1;
    w->ph[0] = SIL;
    w->src_pos = n->ofs;
    w->src_len = n->len;
    w->len = 0;
    w->text[0] = 0;
    w->pitch_off = m->f78;
    w->range = m->f7c;
    w->rate_mul = m->f44;
    return w;
}

static int build_words(anna_mid *m, const anna_msent *s, wlist *l)
{
    int i, k, cnt = 0, e_done = 0, b = 1, last = 0, sent_code = 1, done_nodes = 0;
    anna_word pend;  /* the engine reuses one pending record until it is added */
    int have_pend = 0;
    const anna_mnode *lastn = NULL;
    memset(&pend, 0, sizeof pend);
    for (i = 0; i < s->nn; i++) {
        const anna_mnode *n = &s->nodes[i];
        int first = -1, letter = 0, remaining = n->nw;
        lastn = n;
        if (!n->nw) continue;
        for (k = 0; k < n->nw; k++) {
            const anna_mword *mw = &s->words[n->first + k];
            const anna_token *t = mw->tok;
            anna_word *w;
            if (!have_pend) {
                memset(&pend, 0, sizeof pend);
                pend.st.volume = 100;
                pend.rate_mul = 1.0f;
                pend.range = 1.0f;
                have_pend = 1;
            }
            if (!(n->type & 0x1000)) {
                int ty = n->type;
                if (ty == 9 || ty == 10 || ty == 11 || (ty >= 12 && ty <= 15)) {
                    int code = -1;
                    if (ty == 9) {
                        if (last) code = 5;
                        else sent_code = 5;
                    } else if (ty == 10) {
                        if (last) code = 2;
                        else sent_code = 2;
                    } else if (ty == 11) {
                        if (last) code = 3;
                        else sent_code = 3;
                    } else if (!b) {
                        code = 1;
                    }
                    if (code >= 0 && cnt > 0) {
                        w = wl_insert(l, l->n);
                        if (!w) return -1;
                        *w = pend;
                        have_pend = 0;
                        w->punct = code;
                        w->nph = 1;
                        w->ph[0] = SIL;
                        w->src_pos = n->ofs;
                        w->src_len = n->len;
                        w->pause_f = 0.0f;
                        {
                            uint16_t c1[2];
                            c1[0] = n->text[0];
                            c1[1] = 0;
                            set_text(w, c1, 1);
                        }
                        w->len = 1;
                        w->type = 1;
                        if (first < 0) first = l->n - 1;
                        cnt++;
                        e_done = 1;
                        b = 1;
                    }
                } else if (ty >= 1 && ty <= 8 && ty != 7) {
                    int sil, taken = 0;
                    if (ty <= 3) { /* 0x0D5E7713 */
                        sil = !last;
                        if (!m->in_paren && !m->in_quote) {
                            m->f78 = -0.2f;
                            m->in_paren = 1;
                            m->f7c = 0.75f;
                            m->f44 = 1.25f;
                            taken = 1;
                        }
                    } else if (ty <= 6) { /* 0x0D5E777B */
                        sil = !last;
                        if (m->in_paren) {
                            m->f78 = 0.0f;
                            m->in_paren = 0;
                            m->f7c = 1.0f;
                            m->f44 = 1.0f;
                            taken = 1;
                        }
                    } else { /* 0x0D5E7683 */
                        sil = !b && !last;
                        if (!m->in_paren) {
                            if (!m->in_quote) {
                                m->in_quote = 1;
                                m->f78 = 0.1f;
                                m->f7c = 1.25f;
                            } else {
                                m->in_quote = 0;
                                m->f78 = 0.0f;
                                m->f7c = 1.0f;
                            }
                            taken = 1;
                        }
                    }
                    if (taken) {
                        if (sil) {
                            int was_open_quote = ty == 8 && m->in_quote;
                            w = add_silence(m, l, n, 100);
                            if (!w) return -1;
                            {
                                anna_word keep = *w;
                                *w = pend;
                                w->silence_ms = keep.silence_ms;
                                w->nph = 1;
                                w->ph[0] = SIL;
                                w->src_pos = keep.src_pos;
                                w->src_len = keep.src_len;
                                w->len = 0;
                                w->text[0] = 0;
                                w->pitch_off = keep.pitch_off;
                                w->range = keep.range;
                                w->rate_mul = keep.rate_mul;
                            }
                            have_pend = 0;
                            w->type = ty <= 6 ? 4 : (was_open_quote ? 2 : 3);
                            if (first < 0) first = l->n - 1;
                            cnt++;
                        }
                        b = 1;
                        e_done = 0;
                    }
                }
            } else {
                apply_state(m, t, &pend);
                switch (mw->action) {
                case 0:
                case 4: {
                    int len = mw->has_text ? t->wlen : 0;
                    w = wl_insert(l, l->n);
                    if (!w) return -1;
                    *w = pend;
                    have_pend = 0;
                    if (len > 19) len = 19;
                    set_text(w, t->wtext, len);
                    w->len = len;
                    w->nph = to_internal(mw->pron_set ? mw->pron : NULL, w->ph);
                    if (mw->action == 4 && w->len == 1) w->flags = 0x8000;
                    w->pos = (int)mw->pos;
                    w->pos_class = pos_class(mw->pos);
                    if (mw->action == 4) {
                        w->src_pos = n->ofs + letter;
                        w->src_len = 1;
                        if (t->flags & ANNA_TOK_NUL_AFTER) letter++;
                    } else {
                        w->src_pos = n->ofs;
                        w->src_len = n->len;
                    }
                    w->pitch_off = m->f78;
                    w->range = m->f7c;
                    w->rate_mul = m->f44;
                    if (first < 0) first = l->n - 1;
                    cnt++;
                    b = 0;
                    e_done = 0;
                    break;
                }
                case 1: /* SAPI silence */
                    w = wl_insert(l, l->n);
                    if (!w) return -1;
                    *w = pend;
                    have_pend = 0;
                    if (t->silence_ms > 0) w->silence_ms = t->silence_ms;
                    w->nph = 1;
                    w->ph[0] = SIL;
                    w->src_pos = n->ofs;
                    w->src_len = n->len;
                    w->len = 0;
                    w->text[0] = 0;
                    w->pitch_off = m->f78;
                    w->range = m->f7c;
                    w->rate_mul = m->f44;
                    w->type = 8;
                    if (first < 0) first = l->n - 1;
                    cnt++;
                    e_done = 0;
                    break;
                case 2: /* SAPI pronounce */
                    w = wl_insert(l, l->n);
                    if (!w) return -1;
                    *w = pend;
                    have_pend = 0;
                    w->text[0] = 0;
                    w->len = 0;
                    w->nph = to_internal(t->phone_ids, w->ph);
                    w->pos = (int)mw->pos;
                    w->pos_class = pos_class(mw->pos);
                    w->src_pos = n->ofs;
                    w->src_len = n->len;
                    w->pitch_off = m->f78;
                    w->range = m->f7c;
                    w->rate_mul = m->f44;
                    if (first < 0) first = l->n - 1;
                    cnt++;
                    b = 0;
                    e_done = 0;
                    break;
                case 3: /* SAPI bookmark: attached to the next record */
                    pend.len = mw->has_text ? t->wlen : 0;
                    pend.nbookmarks++;
                    break;
                default:
                    break;
                }
            }
            if (--remaining == 0) done_nodes++;
            if (done_nodes == s->nn - 1) last = 1;
        }
        if (first >= 0) item_prosody(m, l, first, n);
    }
    if (!e_done) {
        anna_word *w = wl_insert(l, l->n);
        if (!w) return -1;
        if (have_pend) *w = pend;
        w->punct = sent_code;
        w->rule2 = 0x1d;
        w->type = 1;
        w->nph = 1;
        w->ph[0] = SIL;
        w->src_pos = lastn ? lastn->ofs : 0;
        w->src_len = lastn ? lastn->len : 0;
        strcpy(w->text, ".");
        w->len = 1;
        cnt++;
    }
    /* 0x0D5E4C79: sentence span on the first word with source text */
    {
        int f = -1, fpos = 0, lpos = 0, llen = 0;
        for (i = 0; i < l->n; i++)
            if (l->w[i].src_len != 0) {
                if (f < 0) {
                    f = i;
                    fpos = l->w[i].src_pos;
                } else {
                    lpos = l->w[i].src_pos;
                    llen = l->w[i].src_len;
                }
            }
        if (f >= 0) {
            l->w[f].sent_pos = fpos;
            l->w[f].sent_len = llen - fpos + lpos;
        }
    }
    return cnt;
}

/* ------------------------------------------------------------------------------------------------ */
/* focus 0x0D5E4CEF + emphasis breaks 0x0D5E6BE8                                                    */

static void focus(wlist *l)
{
    int slot[5] = {-1, -1, -1, -1, -1}, c = 0, a = 0, first = -1, i;
    if (!l->n || l->w[l->n - 1].punct != 2) return;
    for (i = 0; i < l->n; i++) {
        const anna_word *w = &l->w[i];
        if (w->pos_class == 2) {
            c++;
            a++;
            if (c == 1) first = i;
            if (w->pos == 0x1000 && slot[4] < 0) slot[4] = i;
            else if (w->pos == 0x2000 && slot[2] < 0) slot[2] = i;
            else if (w->pos == 0x3001 && slot[3] < 0) slot[3] = i;
            else if (w->pos == 0x3002 && slot[1] < 0) slot[1] = i;
            else if (w->pos == 0x5000 && slot[0] < 0) slot[0] = i;
        } else if (w->pos_class == 1 && ++a == 1) {
            first = i;
        }
    }
    if (c != 1 && a != 1) {
        if (c < 2) return;
        for (i = 0; i < 5; i++)
            if (slot[i] >= 0) {
                first = slot[i];
                break;
            }
    }
    if (first >= 0) l->w[first].emph = 1;
}

static int emphasis_breaks(wlist *l)
{
    int i = 0, limit, prevpunct = 1, prev = -1;
    if (l->n <= 0) return 0;
    focus(l);
    limit = l->n - 1;
    while (i < limit) {
        int p = l->w[i].punct, cur = i;
        if (l->w[i].emph > 0 && prevpunct == 0) {
            anna_word src = l->w[i], *w;
            float r = prev >= 0 ? l->w[prev].rate_mul : 1.0f;
            int vol = prev >= 0 ? l->w[prev].st.volume : 100;
            w = wl_insert(l, i);
            if (!w) return -1;
            w->punct = 0;
            w->silence_ms = 1;
            w->nph = 1;
            w->ph[0] = SIL;
            w->src_pos = src.src_pos;
            w->src_len = src.src_len;
            w->pause_f = 0.0f;
            w->rate_f = 0.0f;
            w->len = 0;
            w->text[0] = 0;
            w->type = 6;
            w->rate_mul = r;
            w->st.volume = vol;
            limit++;
            i++;
            cur = i - 1;
        }
        i++;
        prevpunct = p;
        prev = cur;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* phrasing 0x0D5E6D74                                                                              */

/* 0x0D5E46C5: question type */
static void question(anna_word *w, int yesno)
{
    if (w->punct == 3 || w->punct == 4) {
        if (yesno) {
            w->punct = 3;
            w->rule2 = 0xe;
        } else {
            w->punct = 4;
            w->rule2 = 0xf;
        }
    }
}

static int phrasing(wlist *l)
{
    int n = l->n, i = 0, prevpunct = 1, cnt = 0, yesno = 0, f = 0, d = 0, e = 0, prevword = -1;
    uint32_t prevpos = 0;
    if (n <= 0) return 0;
    while (i < n - 1) {
        anna_word *cur = &l->w[i];
        const anna_word *w1 = i + 1 < l->n ? &l->w[i + 1] : NULL, *w2 = i + 2 < l->n ? &l->w[i + 2] : NULL;
        uint32_t pos = (uint32_t)cur->pos, p1 = w1 ? (uint32_t)w1->pos : 0, p2 = w2 ? (uint32_t)w2->pos : 0;
        int b10 = w1 ? w1->punct != 0 : 0, wh, code = 0, rule = 0, ins = 0;
        if ((unsigned)(prevpunct - 1) < 12) {
            cnt = 1;
            yesno = 1;
            f = 0;
            e = 0;
        } else {
            cnt++;
        }
        wh = (cur->text[0] == 'W' || cur->text[0] == 'w') && (cur->text[1] == 'H' || cur->text[1] == 'h');
        if (cnt == 1) {
            if (pos == 0x4005 || wh) yesno = 0;
            else if (pos == 0x4009 || pos == 0x4003 || pos == 0x4004) f = 1;
        } else if (cnt == 2 && f && (pos == 0x4005 || pos == 0x1004 || wh)) {
            yesno = 0;
        }
        if (d) {
            code = 0xd;
            d = 0;
            rule = 1;
            ins = 1;
        } else {
            int rules = 1;
            if (cnt == 1 && pos == 0x3002 && p1 == 0x4006) {
                d = 1;
            } else {
                d = 0;
                if (pos == 0x4004) {
                    if (!e && cnt > 3 && p2 != 0x4003) {
                        code = 0xe;
                        rule = 2;
                        ins = 1;
                        rules = 0;
                    }
                } else if (pos == 0x3002) {
                    if (cnt > 4 && p1 != 0x3001) {
                        code = 0xe;
                        rule = 3;
                        ins = 1;
                        rules = 0;
                    }
                }
            }
            if (rules) {
                uint32_t pp = prevpos;
                if (pp == 0x1003 && cnt > 2) {
                    code = 0xe;
                    rule = 4;
                    ins = 1;
                } else if ((pos == 0x1002 || pos == 0x4007) && cnt > 3 && pp != 0x1004 && pp != 0x4003) {
                    code = 0xe;
                    rule = 5;
                    ins = 1;
                } else if (pos == 0x4005 && cnt > 4) {
                    code = 0xe;
                    rule = 6;
                    ins = 1;
                } else if (cnt > 2 && (pp == 0x1000 || pp == 0x2000) && pos == 0x4001) {
                    code = 0xf;
                    rule = 7;
                    ins = 1;
                } else {
                    int tail = 0;
                    if (pp == 0x1000) {
                        if (!(p1 == 0x1004 || p1 == 0x4003 || p1 == 0x4004) && cnt > 3 && pos == 0x2000) {
                            code = 0xf;
                            rule = 9;
                            ins = 1;
                        } else {
                            tail = 1;
                        }
                    } else if (pp == 0x2000 || pp == 0x3001 || pp == 0x3002) {
                        tail = 1;
                    }
                    if (tail && pos != 0x1000 && pos != 0x2000 && pos != 0x3001 && pos != 0x3002 && !b10) {
                        code = 0x12;
                        rule = 0xd;
                        ins = 1;
                    }
                }
            }
        }
        if (ins && i > 0 && prevpunct == 0 && cur->punct == 0) {
            anna_word *w;
            int src = cur->src_pos, srcl = cur->src_len;
            float r = prevword >= 0 ? l->w[prevword].rate_mul : 1.0f;
            int vol = prevword >= 0 ? l->w[prevword].st.volume : 100;
            if (prevword >= 0) {
                l->w[prevword].rule1 = rule;
                l->w[prevword].rule2 = rule;
                l->w[prevword].acc_type = 5;
            }
            w = wl_insert(l, i);
            if (!w) return -1;
            w->punct = code;
            w->nph = 1;
            w->ph[0] = SIL;
            w->src_pos = src;
            w->src_len = srcl;
            w->pause_f = 0.0f;
            w->rate_f = 0.0f;
            w->text[0] = '+';
            w->text[1] = 0;
            w->len = 1;
            w->type = 7;
            w->rate_mul = r;
            if (prevword >= 0) w->st.volume = vol;
            n++;
            i++;
            cur = &l->w[i - 1]; /* the break record continues as the current record */
            question(cur, yesno);
            prevword = i - 1;
            prevpunct = cur->punct;
        } else {
            question(cur, yesno);
            prevword = i;
            prevpunct = cur->punct;
        }
        if (cnt > 2) e = 0;
        if (pos == 0x4006) e = 1;
        prevpos = pos;
        i++;
    }
    if (l->n) question(&l->w[l->n - 1], yesno);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* accents 0x0D5E6112                                                                               */

static int accents(anna_mid *m, wlist *l)
{
    int n = l->n, i = 0, cls0, nr = 0, emph = 0, yesno_first = -1, P, k;
    int *run;
    if (!n) return 0;
    run = (int *)malloc(sizeof(int) * 3 * (size_t)n + 12);
    if (!run) return -1;
    cls0 = l->w[0].pos_class;
    while (l->w[i].ph[0] == SIL && i < n - 1) i++;
    if (l->w[i].pos_class == 3) yesno_first = i;
    run[1] = i;
    for (; i < n; i++) {
        const anna_word *w = &l->w[i];
        if (w->pos_class != cls0 && w->ph[0] != SIL) {
            run[3 * nr] = cls0;
            run[3 * nr + 2] = i - 1;
            nr++;
            run[3 * nr + 1] = i;
            cls0 = w->pos_class;
        }
        if (w->emph > 0) emph = 1;
    }
    run[3 * nr] = cls0;
    run[3 * nr + 2] = n - 1;
    nr++;
    for (k = 0; k < nr; k++) {
        int c = run[3 * k];
        if ((c == 1 || c == 3) && run[3 * k + 2] != run[3 * k + 1]) {
            anna_word *w = &l->w[run[3 * k + 2]];
            if (w->acc_type == 0) {
                w->acc_type = 2;
                w->acc_var = 2;
                w->rule1 = 0xf;
            }
        } else if (c == 2 || c == 0) {
            anna_word *w = &l->w[run[3 * k + 1]];
            if (w->acc_type == 0) {
                w->acc_type = 1;
                w->rule1 = 0x10;
                w->acc_var = (int)(msrand(m) % 5);
            }
        }
    }
    free(run);
    /* boundary tones on the last word before each break */
    P = 0;
    for (i = 1; i < n; i++) {
        anna_word *w = &l->w[i], *p = &l->w[P];
        int c = w->punct;
        if (c != 0) {
            if (c == 1) {
                if (p->pos_class == 2) {
                    p->acc_type = 5;
                    p->acc_var = 10;
                    if (!p->rule1) p->rule1 = 0x13;
                }
                p->tone = 0x3eb;
                p->bnd = 5;
                if (!p->rule2) p->rule2 = 0x11;
            } else if (c == 2 || c == 4 || c == 5) {
                if (p->pos_class == 2) {
                    p->acc_type = 1;
                    p->acc_var = 4;
                    if (!p->rule1) p->rule1 = 0x12;
                }
                p->tone = 0x3ea;
                p->bnd = 10;
                if (!p->rule2) p->rule2 = 0x10;
            } else if (c == 3) {
                p->acc_type = 2;
                p->acc_var = 10;
                p->tone = 0x3ec;
                p->bnd = 10;
                if (!p->rule1) p->rule1 = 0x11;
                if (!p->rule2) p->rule2 = 0xe;
                if (yesno_first >= 0) {
                    anna_word *q = &l->w[yesno_first];
                    q->acc_type = 1;
                    q->acc_var = 5;
                    q->rule1 = 0xe;
                }
            } else if (c == 0x13) {
                p->tone = 0x3eb;
                p->bnd = 5;
                if (!p->rule2) p->rule2 = 0x12;
            } else {
                if (p->pos_class == 2) {
                    p->acc_type = 5;
                    p->acc_var = 10;
                    if (!p->rule1) p->rule1 = w->rule1;
                }
                p->tone = 0x3eb;
                p->bnd = 5;
                if (!p->rule2) p->rule2 = w->rule2;
            }
        }
        if (w->len > 0 && !(w->nph == 1 && w->ph[0] == SIL)) P = i;
    }
    if (emph) {
        int prev = -1;
        for (i = 0; i < n; i++) {
            anna_word *w = &l->w[i];
            if (w->emph > 0) {
                w->acc_type = 7;
                w->acc_var = 10;
                w->tone = 0;
                if (prev >= 0) l->w[prev].tone = 0;
            } else if (w->acc_type != 0 && w->acc_var > 5) {
                w->acc_var = 5;
            }
            prev = i;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* API                                                                                              */

anna_mid *anna_mid_new(const char *voice_base, char *err, size_t errlen)
{
    anna_mid *m = (anna_mid *)calloc(1, sizeof *m);
    char p[1024];
    if (!m) return NULL;
    snprintf(p, sizeof p, "%s.TTS", voice_base);
    m->lex = anna_lexicon_load(p);
    snprintf(p, sizeof p, "%s.LTS", voice_base);
    m->lts = anna_lts_load(p);
    if (!m->lex || !m->lts) {
        if (err && errlen) snprintf(err, errlen, "cannot load %s.TTS / .LTS", voice_base);
        anna_mid_free(m);
        return NULL;
    }
    m->rnd = 1;
    m->f38 = 3.342864e33f; /* a pointer read as a float in the engine (see anna_mid_set_f38) */
    m->f44 = 0.0f;
    anna_mid_reset(m);
    return m;
}

void anna_mid_free(anna_mid *m)
{
    if (!m) return;
    anna_lexicon_free(m->lex);
    anna_lts_free(m->lts);
    if (m->have_last) anna_msent_free(&m->last);
    free(m);
}

void anna_mid_reset(anna_mid *m)
{
    m->f78 = 0.0f;
    m->f7c = 1.0f;
    m->in_quote = 0;
    m->in_paren = 0;
}

void anna_mid_set_rand(anna_mid *m, uint32_t state) { m->rnd = state; }
uint32_t anna_mid_get_rand(const anna_mid *m) { return m->rnd; }
void anna_mid_set_rate_factor(anna_mid *m, float f44) { m->f44 = f44; }
void anna_mid_set_f38(anna_mid *m, float f38) { m->f38 = f38; }

const anna_msent *anna_mid_last(const anna_mid *m) { return m->have_last ? &m->last : NULL; }

int anna_mid_sentence(anna_mid *m, const anna_token *t, int n, anna_word *out, int max)
{
    wlist l = {NULL, 0, 0};
    int r, i;
    if (m->have_last) anna_msent_free(&m->last);
    m->have_last = 0;
    if (anna_msent_build(&m->last, t, n) < 0) return -1;
    m->have_last = 1;
    if (anna_pronounce(m->lex, m->lts, &m->last) < 0) return -1;
    r = build_words(m, &m->last, &l);
    if (r < 0 || emphasis_breaks(&l) < 0 || phrasing(&l) < 0 || accents(m, &l) < 0) {
        free(l.w);
        return -1;
    }
    for (i = 0; i < l.n && i < max; i++) out[i] = l.w[i];
    r = l.n;
    free(l.w);
    return r;
}
