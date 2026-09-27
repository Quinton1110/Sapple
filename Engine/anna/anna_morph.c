/* Pronunciation lookup chain of the Anna frontend (MSTTSFrontendENU.dll):
 *   0x0D5E2D08  lookup chain: [user/app lexicons: not modelled] -> vendor lexicon M1033DSK.TTS
 *               -> morphology 0x0D5EDDA2 -> letter to sound 0x0D5F41FA; fills the 0x848 work record
 *   0x0D5EDDA2  suffix stripping (33 reversed suffixes, 31 records), stem lookup with spelling repairs,
 *               0x0D5ED24B / 0x0D5ECBA5 / 0x0D5ED789 combine stem pronunciations with the suffix records
 *   0x0D5F41FA  letter to sound: spell with letter names (0x0D5F3C9D / 0x0D5F3D77) or ILTS::GetPronunciation
 * The same design as the SAPI5 Microsoft Sam frontend (see tts-random/src/sam_morph.c); tables differ.
 */
#include "anna_lex.h"

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

#if defined(__GNUC__)
#define UNUSED_TAB __attribute__((unused))
#else
#define UNUSED_TAB
#endif
#include "anna_lex_tab.h"

#define MAXE 32 /* pronunciations per lookup list */

enum { P_S = 39, P_Z = 48, P_AX = 15, P_T = 41, P_D = 19, P_L = 31, P_IY = 28 };

/* one entry of a SPWORDPRONUNCIATIONLIST */
typedef struct {
    uint32_t lextype, pos;
    uint16_t ph[ANNA_PRON_MAX];
    int n;
} lent;

typedef struct {
    lent e[MAXE];
    int n;
} llist;

static size_t wl(const uint16_t *s) { return anna_wlen(s); }

/* ------------------------------------------------------------------------------------------------ */
/* letter to sound 0x0D5F41FA                                                                       */

/* 0x0D5F3C9D: spell when the word has no lower-case letter or no vowel */
static int must_spell(const uint16_t *w)
{
    int nolower = 1, vowel = 0;
    size_t i, n = wl(w);
    for (i = 0; i < n; i++) {
        uint16_t c = anna_wlower(w[i]);
        if (nolower && anna_wislower(w[i])) nolower = 0;
        if (!vowel && (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y')) vowel = 1;
        if (!nolower && vowel) return 0;
    }
    return 1;
}

/* 0x0D5F3D77: letter names joined with " & " (buffer 0xf00 chars) */
static void spell(const uint16_t *w, char *out, int cap)
{
    int n = 0;
    for (; *w && n < cap - 1; w++) {
        int L = -1;
        const char *s;
        int sl;
        if (*w >= 'A' && *w <= 'Z') L = *w - 'A';
        else if (*w >= 'a' && *w <= 'z') L = *w - 'a';
        if (L < 0) continue;
        s = LETTER_NAMES[L];
        if (n) {
            if (n >= cap - 4) break;
            out[n++] = ' ';
            out[n++] = '&';
            out[n++] = ' ';
        }
        sl = (int)strlen(s);
        if (n >= cap - sl - 1) break;
        memcpy(out + n, s, (size_t)sl);
        n += sl;
    }
    out[n] = 0;
}

int anna_lts_word(const anna_lts *lts, const uint16_t *word, uint16_t *ph, int max)
{
    static ANNA_TLS char buf[0x2000]; /* ClassicVoices patch: per thread */
    int i, prev = ' ', toks = 0, n;
    uint16_t ids[ANNA_PRON_MAX];
    if (must_spell(word)) spell(word, buf, 0xf00);
    else if (!lts) return -1;
    else anna_lts_pronounce(lts, word, buf, sizeof buf);
    /* at most 0x175 phones */
    for (i = 0; buf[i]; i++) {
        if (prev == ' ' && buf[i] != ' ' && ++toks > 0x175) {
            buf[i] = 0;
            break;
        }
        prev = (unsigned char)buf[i];
    }
    n = anna_phones_from_string(buf, ids, ANNA_PRON_MAX - 1);
    if (n < 0) return -1;
    if (n > 0x17f) n = 0x17f;
    if (n > max - 1) n = max - 1;
    memcpy(ph, ids, sizeof(uint16_t) * (size_t)n);
    ph[n] = 0;
    return n;
}

/* ------------------------------------------------------------------------------------------------ */
/* lexicon access                                                                                   */

static int vendor(const anna_lexicon *lex, const uint16_t *w, int len, llist *out)
{
    uint16_t buf[ANNA_WORD_MAX + 1];
    static ANNA_TLS anna_pron pr[MAXE]; /* ClassicVoices patch: per thread */
    int i, c;
    out->n = 0;
    if (len <= 0) return 0;
    if (len > ANNA_WORD_MAX - 1) len = ANNA_WORD_MAX - 1; /* 0x0D5EC1C1 copies at most 0x7f chars */
    memcpy(buf, w, sizeof(uint16_t) * (size_t)len);
    buf[len] = 0;
    c = anna_lexicon_lookup(lex, buf, pr, MAXE);
    for (i = 0; i < c; i++) {
        memcpy(out->e[i].ph, pr[i].ph, sizeof pr[i].ph);
        out->e[i].n = pr[i].n;
        out->e[i].pos = pr[i].pos;
        out->e[i].lextype = 0x1000;
    }
    out->n = c;
    return c;
}

/* ------------------------------------------------------------------------------------------------ */
/* morphology 0x0D5EDDA2                                                                            */

static uint16_t rec_ph[31][10];
static int rec_nph[31];
static int rec_ready;
static pthread_once_t rec_once = PTHREAD_ONCE_INIT; /* ClassicVoices patch: filled once, by one thread */

static void init_records_once(void)
{
    int k;
    for (k = 0; k < 31; k++) {
        rec_nph[k] = anna_phones_from_string(RECORDS[k].phones, rec_ph[k], 9);
        if (rec_nph[k] < 0) rec_nph[k] = 0;
    }
    rec_ready = 1;
}

static void init_records(void) { pthread_once(&rec_once, init_records_once); }

/* 0x0D5EC14C: longest listed suffix; *cut = stem length */
static int match_suffix(const uint16_t *w, int n, int *cut)
{
    int s;
    if (n == 0) return -1;
    for (s = 0; s < 33; s++) {
        const char *rev = SUFFIXES[s].rev;
        int i = n - 1, p = 0, found = -1;
        if (w[i] != (uint16_t)(unsigned char)rev[0]) continue;
        for (;;) {
            if (i < 2) break;
            if (found != -1) break;
            i--;
            p++;
            if (rev[p] == 0) found = SUFFIXES[s].rec;
            if (w[i] != (uint16_t)(unsigned char)rev[p]) break;
        }
        if (found != -1) {
            *cut = i + 1;
            return found;
        }
    }
    return -1;
}

static void cat(lent *e, const uint16_t *ph, int n)
{
    int i;
    for (i = 0; i < n && e->n < ANNA_PRON_MAX - 1; i++) e->ph[e->n++] = ph[i];
    e->ph[e->n] = 0;
}

/* 0x0D5ECDA6: plural / third person */
static void add_plural(lent *e)
{
    int last = e->n ? e->ph[e->n - 1] : 0, cls;
    static const uint16_t axz[2] = {P_AX, P_Z}, z = P_Z, s = P_S;
    if (!e->n || last >= 50) return;
    cls = PHONE_CLASS[last];
    if ((cls & 4) || last == P_S || last == P_Z) cat(e, axz, 2);
    else if ((cls & 1) && !(cls & 2)) cat(e, &s, 1);
    else cat(e, &z, 1);
}

/* 0x0D5ECEE7: past tense */
static void add_past(lent *e)
{
    int last = e->n ? e->ph[e->n - 1] : 0;
    static const uint16_t axd[2] = {P_AX, P_D}, t = P_T, d = P_D;
    if (!e->n || last >= 50) return;
    if (last == P_T || last == P_D) cat(e, axd, 2);
    else if (!(PHONE_CLASS[last] & 2)) cat(e, &t, 1);
    else cat(e, &d, 1);
}

static int is_s(int k) { return rec_nph[k] == 1 && rec_ph[k][0] == P_S; }
static int is_d(int k) { return rec_nph[k] == 1 && rec_ph[k][0] == P_D; }

static void apply_record(lent *e, int k)
{
    if (is_s(k)) {
        add_plural(e);
        return;
    }
    if (is_d(k)) {
        add_past(e);
        return;
    }
    if ((k == 29 || k == 30) && e->n) e->ph[e->n - 1] = P_S; /* -icism / -icize */
    if (e->n + rec_nph[k] < 0x180) cat(e, rec_ph[k], rec_nph[k]);
}

static int contains(const uint32_t *list, int n, uint32_t v)
{
    int i;
    for (i = 0; i < n; i++)
        if (list[i] == v) return 1;
    return 0;
}

/* 0x0D5ECBA5: no POS path through the records: all record phones appended to the first pronunciation,
 * one entry per POS of the outermost record */
static void fallback(const int *stack, int ns, llist *l)
{
    lent e = l->e[0];
    const morph_record *last = &RECORDS[stack[ns - 1]];
    uint32_t lt = e.lextype | 0x4000;
    int s, k, cnt = last->nmap < 5 ? last->nmap : 5;
    for (s = 0; s < ns; s++)
        if (e.n + rec_nph[stack[s]] < 0x180) cat(&e, rec_ph[stack[s]], rec_nph[stack[s]]);
    for (k = 0; k < cnt; k++) {
        l->e[k] = e;
        l->e[k].pos = last->map[k][1];
        l->e[k].lextype = lt;
    }
    l->n = cnt;
}

/* 0x0D5ED24B: combine each stem pronunciation with the records (innermost first) */
static void combine(const int *stack, int ns, llist *l)
{
    static ANNA_TLS llist out; /* ClassicVoices patch: per thread */
    uint32_t seen[5];
    int nseen = 0, i, s;
    out.n = 0;
    for (i = 0; i < l->n; i++) {
        lent e = l->e[i];
        uint32_t cur[5], nxt[5];
        int ncur = 1, ok = 1;
        cur[0] = l->e[i].pos;
        for (s = 0; s < ns && ok; s++) {
            const morph_record *r = &RECORDS[stack[s]];
            int nn = 0, c, m;
            ok = 0;
            for (c = 0; c < ncur && c < 5; c++)
                for (m = 0; m < r->nmap && m < 4; m++)
                    if (r->map[m][0] == cur[c] && nn < 5 && !contains(nxt, nn, r->map[m][1])) {
                        nxt[nn++] = r->map[m][1];
                        if (nn == 1) {
                            ok = 1;
                            apply_record(&e, stack[s]);
                        }
                    }
            if (nn) memcpy(cur, nxt, sizeof(uint32_t) * (size_t)nn);
            ncur = nn;
        }
        if (ok && ncur)
            for (s = 0; s < ncur; s++)
                if (nseen < 5 && !contains(seen, nseen, cur[s])) {
                    seen[nseen++] = cur[s];
                    out.e[out.n] = e;
                    out.e[out.n].pos = cur[s];
                    out.e[out.n].lextype = l->e[i].lextype | 0x4000;
                    out.n++;
                }
    }
    if (!out.n) {
        fallback(stack, ns, l);
        return;
    }
    *l = out;
}

/* 0x0D5ED789: the stem came from letter to sound: the innermost record decides the POS list */
static void combine_lts(const int *stack, int ns, llist *l)
{
    lent e = l->e[0];
    uint32_t pos[5];
    int np = 0, m;
    const morph_record *r = &RECORDS[stack[0]];
    apply_record(&e, stack[0]);
    for (m = 0; m < r->nmap && m < 4; m++)
        if (np < 5 && !contains(pos, np, r->map[m][1])) pos[np++] = r->map[m][1];
    for (m = 0; m < np; m++) {
        l->e[m] = e;
        l->e[m].pos = pos[m];
        l->e[m].lextype = e.lextype | 0x4000;
    }
    l->n = np;
    if (ns > 1) combine(stack + 1, ns - 1, l);
}

static int lts_list(const anna_lts *lts, const uint16_t *word, int len, llist *l)
{
    uint16_t w[ANNA_WORD_MAX + 1];
    int n;
    if (len > ANNA_WORD_MAX - 1) len = ANNA_WORD_MAX - 1;
    memcpy(w, word, sizeof(uint16_t) * (size_t)len);
    w[len] = 0;
    n = anna_lts_word(lts, w, l->e[0].ph, ANNA_PRON_MAX);
    if (n < 0) return 0;
    l->e[0].n = n;
    l->e[0].pos = 0x1000;
    l->e[0].lextype = 0x2000;
    l->n = 1;
    return 1;
}

/* returns the number of entries in l (0 = not found); stem receives the upper-case stem */
static int morph(const anna_lexicon *lex, const anna_lts *lts, const uint16_t *word, llist *l, uint16_t *stem)
{
    uint16_t W[ANNA_WORD_MAX + 2];
    int n = (int)wl(word), cut, stack[64], ns = 0, found = 0, lts_used = 0, i, stemlen = 0;
    init_records();
    l->n = 0;
    stem[0] = 0;
    if (n <= 0 || n >= ANNA_WORD_MAX) return 0;
    for (i = 0; i < n; i++) W[i] = anna_wupper(word[i]);
    W[n] = 0;
    cut = n;
    for (;;) {
        int sl = cut, k = match_suffix(W, cut, &sl);
        const morph_record *rec;
        uint16_t last;
        if (k == -1) {
            if (ns && lts_list(lts, word, cut, l)) {
                lts_used = found = 1;
                stemlen = cut;
            }
            break;
        }
        if (ns == 64) break;
        memmove(stack + 1, stack, sizeof(int) * (size_t)ns);
        stack[0] = k;
        ns++;
        rec = &RECORDS[k];
        cut = sl;
        if (k == 0) {
            if (W[sl - 1] == 'S') {
                memmove(stack, stack + 1, sizeof(int) * (size_t)(--ns));
                if (ns && lts_list(lts, word, sl + 1, l)) {
                    lts_used = found = 1;
                    stemlen = sl + 1;
                }
                break;
            }
            if (vendor(lex, W, sl, l)) {
                found = 1;
                stemlen = sl;
                break;
            }
            if (W[sl - 1] != 'E') continue;
            if (sl >= 2 && W[sl - 2] == 'I') { /* -ies: y */
                W[sl - 2] = 'Y';
                if (vendor(lex, W, sl - 1, l)) {
                    found = 1;
                    stemlen = sl;
                    break;
                }
                W[sl - 2] = 'I';
            }
            if (vendor(lex, W, sl - 1, l)) {
                found = 1;
                stemlen = sl;
                break;
            }
            continue;
        }
        if (k == 0x18) { /* -ably / -ibly: -able / -ible, then -ly */
            if (sl + 3 < ANNA_WORD_MAX) {
                W[sl + 3] = 'E';
                if (vendor(lex, W, sl + 4, l)) {
                    for (i = 0; i < l->n; i++)
                        if (l->e[i].n > 2 && l->e[i].ph[l->e[i].n - 2] == P_AX && l->e[i].ph[l->e[i].n - 1] == P_L) {
                            l->e[i].ph[l->e[i].n - 2] = P_L;
                            l->e[i].ph[--l->e[i].n] = 0;
                        }
                    stack[0] = 11;
                    found = 1;
                    stemlen = sl; /* the engine keeps uVar15 = cut here */
                    break;
                }
                W[sl + 3] = 'Y';
            }
        } else if (k == 0x1A) { /* -ically: -ic + ly */
            if (vendor(lex, W, sl + 2, l)) {
                found = 1;
                stemlen = sl;
                break;
            }
            cut = sl + 2;
            continue;
        } else if (k == 0x1C) { /* -ily: -y */
            uint16_t save = W[sl];
            W[sl] = 'Y';
            if (vendor(lex, W, sl + 1, l)) {
                for (i = 0; i < l->n; i++)
                    if (l->e[i].n && l->e[i].ph[l->e[i].n - 1] == P_IY) l->e[i].ph[--l->e[i].n] = 0;
                found = 1;
                stemlen = sl + 1;
                break;
            }
            W[sl] = save;
            continue;
        } else if (k == 0x1D || k == 0x1E) { /* -icism / -icize: -ic, final k -> s */
            if (vendor(lex, W, sl + 2, l)) {
                for (i = 0; i < l->n; i++)
                    if (l->e[i].n) l->e[i].ph[l->e[i].n - 1] = P_S;
                found = 1;
                stemlen = sl;
                break;
            }
            cut = sl + 2;
            continue;
        }
        last = W[sl - 1];
        if ((rec->flags & 1) && last != 'O' && (last != 'E' || k == 1) && last != 'W' && last != 'Y') {
            if (sl < ANNA_WORD_MAX) { /* 0x0D5EC42B: restore a final e */
                uint16_t save = W[sl];
                W[sl] = 'E';
                if (vendor(lex, W, sl + 1, l)) {
                    if (sl > 0 && W[sl - 1] == 'L')
                        for (i = 0; i < l->n; i++)
                            if (l->e[i].n > 1 && l->e[i].ph[l->e[i].n - 2] == P_AX && l->e[i].ph[l->e[i].n - 1] == P_L) {
                                l->e[i].ph[l->e[i].n - 2] = P_L;
                                l->e[i].ph[--l->e[i].n] = 0;
                            }
                    found = 1;
                    stemlen = sl + 1;
                    break;
                }
                W[sl] = save;
            }
        }
        if (vendor(lex, W, sl, l)) {
            found = 1;
            stemlen = sl;
            break;
        }
        if ((rec->flags & 2) && W[sl - 1] == 'I') { /* 0x0D5EC636: i -> y */
            W[sl - 1] = 'Y';
            if (vendor(lex, W, sl, l)) {
                found = 1;
                stemlen = sl;
                break;
            }
            W[sl - 1] = 'I';
        }
        if (rec->flags & 4) { /* 0x0D5EC688: doubled consonant */
            uint16_t c = W[sl - 1];
            if (!(c == 'A' || c == 'E' || c == 'F' || c == 'H' || c == 'I' || c == 'K' || c == 'O' || c == 'S' ||
                  c == 'U' || c == 'W' || c == 'Y' || c == 'Z') &&
                sl >= 2 && c == W[sl - 2]) {
                if (vendor(lex, W, sl - 1, l)) {
                    found = 1;
                    stemlen = sl - 1;
                    break;
                }
            }
        }
        if (rec->flags & 0x10) { /* 0x0D5EC5A3: -ly after l */
            if (sl < ANNA_WORD_MAX) {
                uint16_t save = W[sl];
                W[sl] = 'L';
                if (vendor(lex, W, sl + 1, l)) {
                    for (i = 0; i < l->n; i++)
                        if (l->e[i].n && l->e[i].ph[l->e[i].n - 1] == P_L) l->e[i].ph[--l->e[i].n] = 0;
                    found = 1;
                    stemlen = sl + 1;
                    break;
                }
                W[sl] = save;
            }
        }
    }
    if (!found || !ns || !l->n) {
        l->n = 0;
        return 0;
    }
    if (stemlen + 1 < ANNA_WORD_MAX) {
        memcpy(stem, W, sizeof(uint16_t) * (size_t)stemlen);
        stem[stemlen] = 0;
    }
    if (lts_used) combine_lts(stack, ns, l);
    else combine(stack, ns, l);
    return l->n;
}

/* ------------------------------------------------------------------------------------------------ */
/* lookup chain 0x0D5E2D08                                                                          */

static int has_pos(const llist *l, uint32_t pos)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (l->e[i].pos == pos) return 1;
    return 0;
}

int anna_lookup_word(const anna_lexicon *lex, const anna_lts *lts, anna_entry *e)
{
    static ANNA_TLS llist l; /* ClassicVoices patch: per thread */
    int i, n = (int)wl(e->text);
    uint16_t stem[ANNA_WORD_MAX];
    stem[0] = 0;
    l.n = 0;
    if (e->reqpos) { /* a requested POS: the first list that has it */
        if (!vendor(lex, e->text, n, &l) || !has_pos(&l, e->reqpos)) {
            if (!morph(lex, lts, e->text, &l, stem) || !has_pos(&l, e->reqpos)) {
                l.n = 0;
                stem[0] = 0;
            }
        }
    }
    if (!l.n && !vendor(lex, e->text, n, &l) && !morph(lex, lts, e->text, &l, stem)) {
        if (!lts_list(lts, e->text, n, &l)) return -1;
    }
    if (stem[0]) memcpy(e->stem, stem, sizeof stem);
    /* first pronunciation, POS of the same pronunciation, first different pronunciation */
    e->lextype = l.e[0].lextype;
    e->npos1 = 1;
    e->n1 = l.e[0].n < 0x17f ? l.e[0].n : 0x17f;
    memcpy(e->pron1, l.e[0].ph, sizeof(uint16_t) * (size_t)e->n1);
    e->pron1[e->n1] = 0;
    e->npos2 = 0;
    e->n2 = 0;
    e->pos1[0] = l.e[0].pos;
    for (i = 1; i < l.n; i++) {
        if (l.e[i].n == l.e[0].n && !memcmp(l.e[i].ph, l.e[0].ph, sizeof(uint16_t) * (size_t)l.e[0].n)) {
            if (e->npos1 < 4) e->pos1[e->npos1++] = l.e[i].pos;
        } else if (e->npos2 < 4) {
            e->pos2[e->npos2++] = l.e[i].pos;
            if (!e->n2) {
                e->n2 = l.e[i].n < 0x17f ? l.e[i].n : 0x17f;
                memcpy(e->pron2, l.e[i].ph, sizeof(uint16_t) * (size_t)e->n2);
                e->pron2[e->n2] = 0;
                e->hasalt = 1;
            }
        }
    }
    if (e->reqpos) {
        int ok = 0;
        for (i = 0; i < e->npos1; i++)
            if (e->pos1[i] == e->reqpos) {
                e->usealt = 0;
                e->pos = e->reqpos;
                ok = 1;
            }
        if (e->hasalt)
            for (i = 0; i < e->npos2; i++)
                if (e->pos2[i] == e->reqpos) {
                    e->usealt = 1;
                    e->pos = e->reqpos;
                    ok = 1;
                }
        if (ok) return 0;
        e->reqpos = 0;
    } else {
        e->usealt = 0;
    }
    e->pos = e->pos1[0];
    return 0;
}
