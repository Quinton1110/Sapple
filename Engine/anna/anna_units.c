/* Anna frontend, last stage: word records -> unit records -> UnitSpecs.
 *
 * Reconstructed from MSTTSFrontendENU.dll (image base 0x0D5D0000):
 *   0x0D5EA45E  sentence: per word pause / bookmark / segmentation, then the "s+t s" -> "s t+s" fix-up
 *   0x0D5E9DF9  one word: syllables (0x0D5E4AB0), onset / nucleus / coda matching against the UDT lists
 *   0x0D5E9BB6  spelled word (flags 0x8000): letter units "_a_".."_z_"
 *   0x0D5E54DA  first-exact or longest-prefix match of a phone range against a UDT list (0x0D5E444B)
 *   0x0D5E9687 / 0x0D5E9519  unit record / pause record
 *   0x0D5E96EC (+0x0D5E9970, 0x0D5E9A0F)  unit records -> UnitSpecs, context features f0 f1 f3 f4
 *   0x0D5EA75F  GetNextUnitSpecList: leading copy of UnitSpec 0
 * Quirks of the original are kept on purpose (see notes/units.md): e.g. a spelled word repeats its first letter,
 * and the match routine leaves the stress / break side effects of the last candidate it tried in the record.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "anna_front.h"

#ifdef ANNA_UNITS_COV /* branch coverage counters for the test driver */
int anna_units_cov[16];
#define COV(i) (anna_units_cov[i]++)
#else
#define COV(i) ((void)0)
#endif

/* ---------------------------------------------------------------- UDT (0x0D5F73A3) */

typedef struct {
    int lang, idx;       /* u16 0x409, i16 unit index 1..260 */
    int front, back;     /* i8 phone classes of the first / last phone (index into the flags table) */
    int type;            /* 0, 2, 4 */
    char name[24];       /* e.g. "w+aa+n", "_a_" (ASCII from UTF-16) */
} udt_unit;

struct anna_udt {
    int n;
    udt_unit *u;
    int nl[3];
    int *list[3];        /* in file order: type 0 (voice+0x228), type 2 (+0x238), type 4 (+0x248) */
};

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

anna_udt *anna_udt_load(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    unsigned char *d = NULL;
    long sz;
    anna_udt *u = NULL;
    int n, i, k;
    const unsigned char *blob;
    unsigned bsz;
    if (!f) { snprintf(err, errlen, "cannot open %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc((size_t)sz);
    if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d); snprintf(err, errlen, "read error"); return NULL; }
    fclose(f);
    if (sz < 12 || memcmp(d, "UDT", 3) || rd32(d + 4) != 1) { free(d); snprintf(err, errlen, "not a UDT v1 file"); return NULL; }
    n = (int)rd32(d + 8);
    if (n < 0 || 12 + (long)n * 24 + 4 > sz) { free(d); snprintf(err, errlen, "bad UDT count"); return NULL; }
    bsz = rd32(d + 12 + n * 24);
    blob = d + 16 + n * 24;
    if ((long)(16 + n * 24) + (long)bsz > sz) { free(d); snprintf(err, errlen, "bad UDT blob"); return NULL; }
    u = calloc(1, sizeof *u);
    u->n = n;
    u->u = calloc((size_t)n + 1, sizeof *u->u);
    for (k = 0; k < 3; k++) u->list[k] = calloc((size_t)n + 1, sizeof(int));
    for (i = 0; i < n; i++) {
        const unsigned char *r = d + 12 + 24 * i;
        udt_unit *x = &u->u[i];
        unsigned off = rd32(r + 4), j;
        x->lang = (int)rd16(r);
        x->idx = (short)rd16(r + 2);
        x->front = (signed char)r[8];
        x->back = (signed char)r[9];
        x->type = (int)rd32(r + 12);
        if (x->lang != 0x409 || x->idx < 1 || x->front < 0 || x->back < 0 || x->type > 4 || off >= bsz) {
            snprintf(err, errlen, "bad UDT record %d", i);
            anna_udt_free(u);
            free(d);
            return NULL;
        }
        for (j = 0; j + 1 < sizeof x->name && off + 2 * j + 1 < bsz && rd16(blob + off + 2 * j); j++)
            x->name[j] = (char)rd16(blob + off + 2 * j);
        k = x->type == 0 ? 0 : x->type == 2 ? 1 : x->type == 4 ? 2 : -1;
        if (k < 0) { snprintf(err, errlen, "bad UDT type"); anna_udt_free(u); free(d); return NULL; }
        u->list[k][u->nl[k]++] = i;
    }
    free(d);
    return u;
}

void anna_udt_free(anna_udt *u)
{
    int k;
    if (!u) return;
    for (k = 0; k < 3; k++) free(u->list[k]);
    free(u->u);
    free(u);
}

int anna_udt_count(const anna_udt *u) { return u ? u->n : 0; }

/* unit type name ("w+aa+n", "_a_"): the phones one recorded unit of that type holds.  NULL out of range. */
const char *anna_udt_name(const anna_udt *u, int index)
{
    return u && index >= 0 && index < u->n ? u->u[index].name : NULL;
}

/* the type's own index (udt +0x02, 1..260), which is how the UNT groups the corpus; -1 out of range */
int anna_udt_index(const anna_udt *u, int index) { return u && index >= 0 && index < u->n ? u->u[index].idx : -1; }

/* ---------------------------------------------------------------- tables */

/* internal phone code -> name (0x0D6022E8); 17 '_', 43/44 stress, 45, 46 '-' are handled before lookup */
static const char *const ph_name[43] = {
    "iy", "ih", "eh", "ae", "aa", "ah", "ao", "uh", "ax", "er", "ey", "ay", "oy", "aw", "ow", "uw", "ix", "sil",
    "w", "y", "r", "l", "h", "m", "n", "ng", "f", "v", "th", "dh", "s", "z", "sh", "zh", "p", "b", "t", "d", "k", "g",
    "ch", "jh", "dx"};

/* phone class -> lFront/BackPhoneFlags (0x0D5D1D00) */
static const unsigned ph_flags[48] = {
    0x28003d, 0x20003d, 0x20003d, 0x20003d, 0x3d, 0x3d, 0x3d, 0x3d, 0x3d, 0x3d, 0x68003d, 0x40003d, 0x40003d,
    0x40003d, 0x40003d, 0x40003d, 0x20003d, 0x20, 0x20001b6, 0xc01b6, 0x20001b6, 0x1b6, 0x22, 0x808976, 0x802976,
    0x804976, 0x8008402, 0x8008406, 0x8010402, 0x8010406, 0x8002402, 0x8002406, 0x8020402, 0x8020406, 0x809e02,
    0x809e06, 0x803e02, 0x803e06, 0x805e02, 0x805e06, 0x1020e02, 0x1020e06, 0xc06, 0x0, 0x1b001c, 0xb0015, 0xc000a,
    0x2b000d};

/* legal onset clusters, tried in order at the end of an intervocalic cluster (0x0D6023D8, 72 x {code[4], len}) */
static const signed char onsets[72][5] = {
    {30, 38, 18, 0, 3}, {30, 36, 20, 0, 3}, {30, 38, 20, 0, 3}, {30, 34, 20, 0, 3}, {30, 38, 21, 0, 3},
    {30, 34, 21, 0, 3}, {30, 38, 19, 0, 3}, {30, 36, 19, 0, 3}, {30, 34, 19, 0, 3}, {32, 20, 0, 0, 2},
    {30, 34, 0, 0, 2}, {30, 26, 0, 0, 2}, {26, 19, 0, 0, 2}, {30, 36, 0, 0, 2}, {26, 20, 0, 0, 2},
    {32, 24, 0, 0, 2}, {27, 19, 0, 0, 2}, {32, 18, 0, 0, 2}, {28, 20, 0, 0, 2}, {34, 20, 0, 0, 2},
    {35, 20, 0, 0, 2}, {36, 20, 0, 0, 2}, {37, 20, 0, 0, 2}, {38, 20, 0, 0, 2}, {39, 20, 0, 0, 2},
    {34, 21, 0, 0, 2}, {35, 21, 0, 0, 2}, {38, 21, 0, 0, 2}, {39, 21, 0, 0, 2}, {26, 21, 0, 0, 2},
    {30, 21, 0, 0, 2}, {34, 19, 0, 0, 2}, {35, 19, 0, 0, 2}, {36, 19, 0, 0, 2}, {37, 19, 0, 0, 2},
    {38, 19, 0, 0, 2}, {39, 19, 0, 0, 2}, {36, 18, 0, 0, 2}, {37, 18, 0, 0, 2}, {38, 18, 0, 0, 2},
    {39, 18, 0, 0, 2}, {28, 18, 0, 0, 2}, {30, 18, 0, 0, 2}, {30, 19, 0, 0, 2}, {30, 38, 0, 0, 2},
    {22, 19, 0, 0, 2}, {23, 19, 0, 0, 2}, {24, 19, 0, 0, 2}, {30, 23, 0, 0, 2}, {28, 19, 0, 0, 2},
    {34, 0, 0, 0, 1}, {35, 0, 0, 0, 1}, {30, 0, 0, 0, 1}, {31, 0, 0, 0, 1}, {23, 0, 0, 0, 1},
    {24, 0, 0, 0, 1}, {36, 0, 0, 0, 1}, {37, 0, 0, 0, 1}, {26, 0, 0, 0, 1}, {27, 0, 0, 0, 1},
    {21, 0, 0, 0, 1}, {20, 0, 0, 0, 1}, {38, 0, 0, 0, 1}, {39, 0, 0, 0, 1}, {28, 0, 0, 0, 1},
    {29, 0, 0, 0, 1}, {19, 0, 0, 0, 1}, {18, 0, 0, 0, 1}, {40, 0, 0, 0, 1}, {41, 0, 0, 0, 1},
    {22, 0, 0, 0, 1}, {32, 0, 0, 0, 1}};

static const int pause_ms_tab[6] = {0, 0, 0, 0, 400, 750};        /* 0x0D5D2188 by break level */
static const int f0_tab[26] = {1, 32, 0, 0, 0, 1, 1, 2, 0, 3,     /* 0x0D602960[end + 4 * start], start/end 2..5 */
                               0, 3, 6, 7, 1, 4, 8, 9, 2, 5, 10, 10, 2, 5, 10, 10};
static const int f1_tab[4] = {1, 2, 0, 3};                        /* 0x0D602978[(end > 1) + 2 * (start > 1)] */

#define PH_PAUSE 0x11
#define PH_STRESS1 0x2b
#define PH_STRESS2 0x2c
#define PH_SYL 0x2e

#define DEF_NONE (-1)     /* NULL unit definition */
#define DEF_SIL (-2)      /* silence definition 0x0D6023A4 (lang 0, index 0) */
#define DEF_BM (-3)       /* bookmark definition 0x0D6023BC (lang 0, index -1) */

/* ---------------------------------------------------------------- unit records (0x48 bytes) */

typedef struct {
    int def;             /* [0] UDT unit, or DEF_* */
    int stress;          /* [2] */
    int emph;            /* [3] */
    int punct;           /* [4] */
    int brk;             /* [5] */
    int role;            /* [6] 0 onset (greedy), 1 onset split off the nucleus, 2 nucleus, 3 split-off coda, 4 coda,
                                else the UDT type (phone-by-phone fallback) */
    int vol, rate, voice, pitch; /* [7] [8] [9] [10] */
    int ctrl;            /* [11] */
    int src_pos, src_len, sent_pos, sent_len; /* [12..15] */
    int ms;              /* [16] pause length */
} urec;

typedef struct {
    const anna_udt *u;
    urec *r;
    int n, cap;
    int ph[ANNA_MAX_PH]; /* working copy of the current word's phones */
    int nph;
} seg;

static int push(seg *s, const urec *r)
{
    if (s->n == s->cap) {
        int c = s->cap ? s->cap * 2 : 64;
        urec *nr = realloc(s->r, (size_t)c * sizeof *nr);
        if (!nr) return -1;
        s->r = nr;
        s->cap = c;
    }
    s->r[s->n++] = *r;
    return 0;
}

/* 0x0D5E9687 */
static int add(seg *s, urec *r, int punct, int stress, int emph, int role)
{
    r->punct = punct;
    if (r->emph < emph) r->emph = emph;
    if (r->stress < stress) r->stress = stress;
    r->role = role;
    return push(s, r);
}

/* 0x0D5E9519: pause record after word w (break level brk); user silences (type 8) are stored negative */
static int add_pause(seg *s, const anna_word *w, int brk, unsigned ms)
{
    urec r;
    memset(&r, 0, sizeof r);
    r.def = DEF_SIL;
    r.rate = w->st.rate;
    r.vol = w->st.volume;
    r.brk = brk;
    if (w->type == 8) r.ms = -(int)(ms < 0xffff ? ms : 0xffff);
    else r.ms = (int)(ms < 0xffff ? ms : 0xffff);
    r.src_pos = w->src_pos;
    r.src_len = w->src_len;
    r.sent_pos = w->sent_pos;
    r.sent_len = w->sent_len;
    return push(s, &r);
}

/* 0x0D5E95E0 */
static int add_bookmark(seg *s, const anna_word *w)
{
    urec r;
    memset(&r, 0, sizeof r);
    r.def = DEF_BM;
    r.rate = w->st.rate;
    r.vol = w->st.volume;
    r.src_pos = w->src_pos;
    r.src_len = w->src_len;
    r.sent_pos = w->sent_pos;
    r.sent_len = w->sent_len;
    return push(s, &r);
}

static int is_marker(int c) { return c >= 0x2b || c == PH_PAUSE; }                   /* 0x0D5E4995 */
static int is_sonorant(int c) { return (c < 0x1a || c > 0x2a) && c != 0x16; }        /* 0x0D5E4970 (not h) */

/* 0x0D5E4A27: every '-'-delimited part has a vowel */
static int syl_ok(const int *ph, int n)
{
    int i, v = 0;
    if (n < 1) return 0;
    for (i = 0; i < n; i++) {
        if (ph[i] == PH_SYL) {
            if (!v) return 0;
            v = 0;
        } else if (ph[i] < 0x11) v = 1;
    }
    return v;
}

/* 0x0D5E42F9: length of the legal onset at the end of cluster ph[0..n) (0 if none) */
static int onset_len(const int *ph, int n)
{
    int e, k;
    for (e = 0; e < 72; e++) {
        int len = onsets[e][4];
        if (len > n) continue;
        for (k = 0; k < len && ph[n - len + k] == onsets[e][k]; k++) {}
        if (k == len) return len;
    }
    return 0;
}

/* 0x0D5E4AB0: first syllable of ph[0..n): son = start of the sonorant (vowel) part, nuc_end = its end,
 * end = syllable end, stress = strongest stress mark seen */
static void syllable(const int *ph, int n, int *son, int *nuc_end, int *end, int *stress)
{
    int i = 0, vowel = 0;
    *end = *nuc_end = *son = -1;
    *stress = 0;
    if (n < 1) return;
    if (n == 1) {
        if (ph[0] < 0x11) { *son = 0; *end = *nuc_end = 1; }
        return;
    }
    for (; i < n; i++) {
        int c = ph[i];
        if (c == PH_SYL) { i++; break; }
        if (c == PH_PAUSE) continue;
        if (c == PH_STRESS1) { if (*stress < 2) *stress = 1; }
        else if (c == PH_STRESS2) { if (*stress < 3) *stress = 2; }
        if (*son < 0) {
            if (is_sonorant(c)) { *son = i; vowel = c < 0x11; }
        } else if (*nuc_end < 0) {
            if (!is_sonorant(c)) {
                if (vowel) *nuc_end = i;
                else *son = -1;
            } else if (c < 0x11) {
                if (vowel) {
                    if (ph[i - 1] > 0x10) i--;
                    break;
                }
                vowel = 1;
            }
        } else if (c < 0x11) {
            int ol = onset_len(ph + *nuc_end, i - *nuc_end);
            if (ol == 0) COV(13);
            else if (ol > 1) COV(14);
            i -= ol;
            break;
        }
    }
    *end = i;
    if (*nuc_end < 0) *nuc_end = i;
    if (!vowel) { *son = -1; *nuc_end = *end; }
}

/* 0x0D5E444B: does the unit name spell ph[0..n)? Returns n on a full match, the number of codes consumed when the
 * name ends early (prefix), or 0. Side effects on r as in the original: brk = 1 if a pause / boundary code was seen
 * (reset per call), stress = 1/2 for stress marks (never reset). */
static int name_match(const char *name, const int *ph, int n, urec *r)
{
    const char *p = name, *e = name + strlen(name);
    int k;
    r->brk = 0;
    for (k = 0; k < n; k++) {
        int c = ph[k];
        if (c < 0x2f && c >= 0) {
            if (c == PH_PAUSE || c > 0x2c) r->brk = 1;
            else if (c == PH_STRESS1) r->stress = 1;
            else if (c == PH_STRESS2) r->stress = 2;
            else {
                const char *q = p, *pn = ph_name[c];
                size_t l = strlen(pn), j;
                while (q < e && (*q == ' ' || *q == '+')) q++;
                if (q == e) return k;
                if (q + l > e) return 0;
                for (j = 0; j < l; j++) {
                    int a = q[j], b = pn[j];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) return 0;
                }
                p = q + l;
            }
        }
    }
    while (p < e && (*p == ' ' || *p == '+')) p++;
    return p == e ? n : 0;
}

/* word-final break level (0x0D5E54DA / 0x0D5E9BB6) */
static int final_brk(const anna_word *w)
{
    if (w->bnd >= 10) return 5;
    if (w->bnd < 1) return 2;
    return 3 + (w->rule2 == 0x11);
}

/* 0x0D5E54DA: match ph[pos..pos+n) against UDT list k (0: +0x228, 1: +0x238, 2: +0x248);
 * greedy = longest prefix (first wins on ties), else the first exact match. Returns the length (0 = none). */
static int match(seg *s, const anna_word *w, int pos, int n, int k, urec *r, int greedy)
{
    const anna_udt *u = s->u;
    int i, best = -1, blen = 0;
    memset(r, 0, sizeof *r);
    r->def = DEF_NONE;
    for (i = 0; i < u->nl[k]; i++) {
        int m = name_match(u->u[u->list[k][i]].name, s->ph + pos, n, r);
        if (!greedy) {
            if (m == n) { best = i; blen = m; break; }
        } else if (m > blen) {
            best = i;
            blen = m;
        }
    }
    if (best < 0) return 0;
    r->def = u->list[k][best];
    if (blen + pos == s->nph) r->brk = final_brk(w);
    r->rate = w->st.rate;
    r->vol = w->st.volume;
    r->pitch = w->st.pitch;
    r->src_pos = w->src_pos;
    r->src_len = w->src_len;
    r->sent_pos = w->sent_pos;
    r->sent_len = w->sent_len;
    if (pos == 0) r->ctrl |= 1;
    return blen;
}

static int find_name(const anna_udt *u, int k, const char *name)
{
    int i;
    for (i = 0; i < u->nl[k]; i++)
        if (!strcmp(u->u[u->list[k][i]].name, name)) return u->list[k][i];
    return DEF_NONE;
}

/* 0x0D5E9BB6: spelled word. Returns 1 if it cannot be spelled (fall back to the phones).
 * The original reads the first character for every letter, so "ab" gives "_a_ _a_". */
static int spell(seg *s, const anna_word *w, int punct, int emph)
{
    int k, c, d;
    char nm[4];
    c = (unsigned char)w->text[0];
    if (w->len > 0) {
        if (c >= 'a' && c <= 'z') c -= 'a';
        else if (c >= 'A' && c <= 'Z') c -= 'A';
        else return 1;
        nm[0] = '_'; nm[1] = (char)('a' + c); nm[2] = '_'; nm[3] = 0;
        d = find_name(s->u, 0, nm);
        if (d < 0) return 1;
        for (k = 0; k < w->len; k++) {
            urec r;
            memset(&r, 0, sizeof r);
            r.def = d;
            r.brk = final_brk(w);
            r.rate = w->st.rate;
            r.vol = w->st.volume;
            r.pitch = w->st.pitch;
            r.src_pos = w->src_pos;
            r.src_len = w->src_len;
            r.sent_pos = w->sent_pos;
            r.sent_len = w->sent_len;
            if (k == 0) r.ctrl |= 1;
            if (add(s, &r, punct, 0, emph, 2) < 0) return -1;
        }
    }
    return 0;
}

/* 0x0D5E9DF9: one word. Returns < 0 on error, 1 if the word has no units (punctuation, '_'), else 0. */
static int seg_word(seg *s, const anna_word *w)
{
    int n0 = s->n, hr = 0, pos = 0, i;
    int punct = (w->rule2 == 0x10 || w->rule2 == 0xf) ? 1 : w->rule2 == 0xe ? 3 : 0;
    int emph = w->acc_var > 5;
    int son, nuc, end, stress, emphf, n;
    int L[ANNA_MAX_PH], R[ANNA_MAX_PH], nl, nr, lh;
    urec r;

    if (w->nph == 0) return 1;
    if (w->flags == 0x8000) {
        hr = spell(s, w, punct, emph);
        if (hr != 1) { COV(0); return hr; }
        COV(1);
    }
    s->nph = w->nph < ANNA_MAX_PH ? w->nph : ANNA_MAX_PH;
    for (i = 0; i < s->nph; i++) s->ph[i] = w->ph[i];
    if (!syl_ok(s->ph, s->nph)) { /* syllable marks that do not delimit vowels are dropped */
        int m = 0;
        for (i = 0; i < s->nph; i++)
            if (s->ph[i] != PH_SYL) s->ph[m++] = s->ph[i];
        if (m != s->nph) COV(2);
        s->nph = m;
    }
    while (hr >= 0 && pos < s->nph) {
        if (s->ph[pos] == PH_PAUSE) return 1;
        syllable(s->ph + pos, s->nph - pos, &son, &nuc, &end, &stress);
        emphf = stress > 0 ? emph : 0;
        if (son < 0) { /* no vowel: one unit per phone over the whole word, no pause */
            COV(3);
            for (i = 0; i < s->nph; i++) {
                int c = s->ph[i];
                if (is_marker(c)) continue;
                if (c < 0x11) match(s, w, i, 1, 1, &r, 0);
                else if (match(s, w, i, 1, 0, &r, 0) != 1) match(s, w, i, 1, 2, &r, 0);
                if (r.def != DEF_NONE) hr = add(s, &r, punct, stress, emphf, s->u->u[r.def].type);
                if (hr < 0) return 0;
            }
            return 0;
        }
        son += pos;
        nuc += pos;
        end += pos;
        /* onset: longest type-0 units */
        for (i = pos; i < son; i += n) {
            n = match(s, w, i, son - i, 0, &r, 1);
            if (!n) break;
            hr = add(s, &r, punct, stress, emphf, 0);
            if (hr < 0) break;
        }
        /* nucleus: exact type-2 unit; peel sonorant consonants off either side until one exists */
        n = nuc - son;
        nl = nr = lh = 0;
        for (;;) {
            int first, last, cnt, j;
            if (hr < 0) goto done;
            while (lh < nl) {
                int idx = L[lh++];
                if (match(s, w, idx, 1, 0, &r, 0) == 1) {
                    hr = add(s, &r, punct, stress, emphf, 1);
                    if (hr < 0) break;
                }
            }
            if (match(s, w, son, n, 1, &r, 0) == n) {
                hr = add(s, &r, punct, stress, emphf, 2);
                while (hr >= 0 && nr > 0) {
                    int idx = R[--nr];
                    if (match(s, w, idx, 1, 2, &r, 0) == 1) hr = add(s, &r, punct, stress, emphf, 3);
                }
                break;
            }
            first = s->ph[son];
            j = son + n - 1;
            last = s->ph[j];
            cnt = 1;
            while (is_marker(last)) { cnt++; last = s->ph[--j]; }
            if (first == 0x17 || first == 0x18 || first == 0x19) goto strip_first;
            if (last == 0x17 || last == 0x18 || last == 0x19 || last == 0x13 || last == 0x12) goto strip_last;
            if (first == 0x15 || first == 0x14) goto strip_first;
            if (last == 0x15) goto strip_last;
            if (first == 0x13 || first == 0x12) goto strip_first;
            if (last == 0x14) goto strip_last;
            COV(6);
            break; /* nucleus not found and not splittable: it is dropped (as in the original) */
        strip_first:
            COV(4);
            n--;
            L[nl++] = son++;
            continue;
        strip_last:
            COV(5);
            n -= cnt;
            R[nr++] = son + n;
        }
    done:
        if (hr < 0) break;
        /* coda: longest type-4 units */
        for (i = nuc; i < end; i += n) {
            n = match(s, w, i, end - i, 2, &r, 1);
            if (!n) break;
            hr = add(s, &r, punct, stress, emphf, 4);
            if (hr < 0) break;
        }
        if (s->n > n0 && s->r[s->n - 1].brk < 2) s->r[s->n - 1].brk = 1;
        pos = end;
    }
    if (hr >= 0 && s->n > n0) {
        urec *l = &s->r[s->n - 1];
        if (l->brk < 3) l->brk = 2;
        if (pause_ms_tab[l->brk] > 0) {
            COV(l->brk == 4 ? 9 : 10);
            hr = add_pause(s, w, l->brk, (unsigned)pause_ms_tab[l->brk]);
        }
    }
    return hr;
}

/* ---------------------------------------------------------------- UnitSpecs */

typedef struct {
    anna_unitspec s;
    anna_unitinfo i;
    int def;
} spec;

static int lang_of(const anna_udt *u, int def) { return def >= 0 ? u->u[def].lang : 0; }
static int is_special(int def) { return def == DEF_SIL || def == DEF_BM; }

/* 0x0D5E4F78: next spec at or after i that is a real unit */
static int skip_special(const spec *p, int i, int n)
{
    while (i < n && is_special(p[i].def)) i++;
    return i;
}

/* 0x0D5E4DFF: break level of the first real unit at or after i with brk >= lvl, else lvl */
static int next_brk(const spec *p, int i, int n, int lvl)
{
    for (; i < n; i++)
        if (!is_special(p[i].def) && p[i].s.brk >= lvl) return p[i].s.brk;
    return lvl;
}

/* 0x0D5E4E38: ePuncType of the first real unit at or after i with brk > 1, else 1 */
static int next_punct(const spec *p, int i, int n)
{
    for (; i < n; i++)
        if (!is_special(p[i].def) && p[i].s.brk > 1) return p[i].i.punct;
    return 1;
}

static int back_class(const anna_udt *u, int def) { return def >= 0 ? u->u[def].back : 0; }
static int front_class(const anna_udt *u, int def) { return def >= 0 ? u->u[def].front : 0; }
static int default_class(int lang) { return lang == 0x409 ? 0x11 : lang == 0x804 ? 0x1b : 0; } /* 0x0D5E49B4 */

int anna_words_to_units(const anna_udt *u, const anna_word *w, int nw, anna_unitspec *out, anna_unitinfo *info, int max)
{
    seg s;
    spec *p = NULL;
    int i, k, n = 0, hr = 0, first = 1;
    memset(&s, 0, sizeof s);
    s.u = u;

    /* 0x0D5EA45E */
    for (i = 0; i < nw && hr >= 0; i++) {
        const anna_word *x = &w[i];
        if (x->nbookmarks) { COV(11); hr = add_bookmark(&s, x); }
        if (hr < 0) break;
        if (x->nph == 1 && x->ph[0] == PH_PAUSE && x->type == 8 && x->silence_ms != 0) {
            COV(8);
            hr = add_pause(&s, x, 3, (unsigned)x->silence_ms);
        } else
            hr = seg_word(&s, x);
    }
    if (hr < 0) { free(s.r); return -1; }
    /* "s+t" + "s" -> "s" + "t+s" (also f, k, p) */
    for (k = 0; k < s.n - 1; k++) {
        urec *a = &s.r[k], *b = &s.r[k + 1];
        const char *nb, *na, *rep = NULL;
        int da, db;
        if (a->def == DEF_NONE || b->def == DEF_NONE || a->brk >= 2 || b->brk <= 1) continue;
        nb = b->def >= 0 ? u->u[b->def].name : "";
        if (strcmp(nb, "s")) continue;
        na = a->def >= 0 ? u->u[a->def].name : "";
        if (!strcmp(na, "s+t")) rep = "s";
        else if (!strcmp(na, "f+t")) rep = "f";
        else if (!strcmp(na, "k+t")) rep = "k";
        else if (!strcmp(na, "p+t")) rep = "p";
        else continue;
        da = find_name(u, 2, rep);
        db = find_name(u, 2, "t+s");
        if (da >= 0 && db >= 0) {
            COV(7);
            a->def = da;
            b->def = db;
            k++;
        }
    }

    /* 0x0D5E96EC: unit records -> specs (bookmarks produce none) */
    p = calloc((size_t)s.n + 2, sizeof *p);
    if (!p) { free(s.r); return -1; }
    for (i = 0; i < s.n; i++) {
        const urec *r = &s.r[i];
        spec *q = &p[n];
        if (r->def == DEF_BM) continue;
        memset(q, 0, sizeof *q);
        q->def = r->def;
        q->s.type = r->def >= 0 ? u->u[r->def].idx : 0;
        q->s.brk = r->brk;
        if (i < s.n - 1 && s.r[i + 1].def == DEF_SIL && q->s.brk < s.r[i + 1].brk) q->s.brk = s.r[i + 1].brk;
        q->i.punct = r->punct;
        q->s.volume = r->vol;
        q->s.rate = r->rate;
        q->s.pitch = r->pitch;
        q->i.voice = r->voice;
        if (r->def == DEF_SIL) q->s.pause_ms = r->ms;
        q->i.ctrl = r->ctrl;
        q->i.src_pos = r->src_pos;
        q->i.src_len = r->src_len;
        if (first && r->sent_len != 0) {
            q->i.sent_pos = r->sent_pos;
            q->i.ctrl |= 2;
            first = 0;
            q->i.sent_len = r->sent_len;
        }
        if (r->def >= 0 && u->u[r->def].lang == 0x409) { /* 0x0D5E4713 */
            const udt_unit *d = &u->u[r->def];
            q->s.ff = d->front < 48 ? ph_flags[d->front] : 0;
            q->s.bf = d->back < 48 ? ph_flags[d->back] : 0;
        }
        q->s.f[7] = r->stress;
        q->s.f[8] = r->emph;
        q->s.f[2] = r->role;
        n++;
    }
    if (s.n > 0 && s.r[s.n - 1].def == DEF_BM) { /* trailing bookmarks get an empty silence to carry them */
        COV(12);
        memset(&p[n], 0, sizeof p[n]);
        p[n].def = DEF_SIL;
        n++;
    }
    free(s.r);

    /* context features (0x0D5E9A0F) */
    {
        int c = skip_special(p, 0, n), prev = n, nx, st = 5, sb = 5, e, cb;
        if (c < n) {
            nx = skip_special(p, c + 1, n);
            e = next_brk(p, c, n, 2);
            cb = next_brk(p, c, n, 1);
            while (c < n) {
                int lang = lang_of(u, p[c].def), br;
                if (prev == n || lang_of(u, p[prev].def) != lang || p[prev].s.brk > 3) p[c].s.f[3] = default_class(lang);
                else p[c].s.f[3] = back_class(u, p[prev].def);
                p[c].s.f[5] = 0;
                if (nx == n || lang_of(u, p[nx].def) != lang || p[c].s.brk > 3) p[c].s.f[4] = default_class(lang);
                else p[c].s.f[4] = front_class(u, p[nx].def);
                p[c].s.f[6] = 0;
                p[c].s.f[1] = f1_tab[(cb > 1) + (sb > 1) * 2];
                br = e + st * 4;
                p[c].s.f[0] = br >= 0 && br < 26 ? f0_tab[br] : 0;
                if (next_punct(p, c, n) == 3) { COV(15); p[c].s.f[0] = 0xb; }
                prev = c;
                c = nx;
                if (c == n) break;
                nx = skip_special(p, c + 1, n);
                br = p[prev].s.brk;
                if (br > 0 && c < n) {
                    sb = cb;
                    cb = next_brk(p, c, n, 1);
                }
                if (br > 1) {
                    if (c >= n) break;
                    st = e;
                    e = next_brk(p, c, n, 2);
                }
            }
        }
    }

    /* 0x0D5EA75F: output = copy of spec 0 as a leading silence, then all specs */
    if (n == 0) { free(p); return 0; }
    for (i = 0; i <= n && i < max; i++) {
        const spec *q = &p[i ? i - 1 : 0];
        out[i] = q->s;
        if (info) info[i] = q->i;
        if (i == 0) {
            out[i].type = 0;
            out[i].pause_ms = 5;
            if (info) info[i].ctrl = 0;
        }
    }
    free(p);
    return n + 1 <= max ? n + 1 : max;
}
