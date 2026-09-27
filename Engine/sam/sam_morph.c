/* Word lookup chain of the engine's normalizer (FUN_5ed4b1f9): lexicon, then morphology
 * (FUN_5ed4ee14 suffix stripping, see tools/morph.py), then whole-word letter-to-sound.
 */
#include "sam_lex.h"

#include <ctype.h>
#include <string.h>

#define MAXE 24

typedef struct {
    uint16_t ph[SAM_MAX_PRON];
    int n;
    uint32_t pos;
} ent;

typedef struct {
    const char *rev;
    int rec;
} suffix;

static const suffix SUFFIXES[] = {
    {"RE", 5},   {"TSE", 6},  {"GNI", 2},     {"ELBA", 14}, {"ELBI", 14}, {"YLDE", 12}, {"YLBA", 24}, {"YLBI", 24},
    {"YLLACI", 26}, {"YLI", 28}, {"YL", 13},  {"Y", 11},    {"TNEM", 8},  {"RO", 7},    {"SSEN", 15}, {"SSEL", 10},
    {"EZICI", 30}, {"EZI", 17}, {"ZI", 18},   {"MSICI", 29}, {"MSI", 16}, {"DE", 1},    {"S'", 3},    {"S", 0},
    {"'", 4},    {"EGA", 9},  {"DOOH", 19},   {"LUF", 20},  {"EKIL", 21}, {"ESIW", 22}, {"HSI", 23},  {"PIHS", 25},
    {"EMOS", 27}};

/* SAPI ids: s 39, z 48, ax 15, t 41, d 19, l 31, iy 28 */
enum { P_S = 39, P_Z = 48, P_AX = 15, P_T = 41, P_D = 19, P_L = 31, P_IY = 28 };

typedef struct {
    uint16_t ph[8];
    int nph;
    uint32_t map[4][2];
    int nmap;
    int flags;
} record;

/* phones as SAPI ids (see tools/morph.py for the names) */
static const record RECORDS[31] = {
    {{39}, 1, {{0x2000, 0x2000}, {0x1000, 0x1000}}, 2, 0x0},
    {{19}, 1, {{0x2000, 0x2000}, {0x2000, 0x3001}}, 2, 0x7},
    {{27, 34}, 2, {{0x2000, 0x2000}, {0x2000, 0x3001}, {0x2000, 0x1000}}, 3, 0x5},
    {{39}, 1, {{0x1000, 0x1000}}, 1, 0x0},
    {{39}, 1, {{0x1000, 0x1000}}, 1, 0x0},
    {{22}, 1, {{0x2000, 0x1000}, {0x3001, 0x3001}, {0x3002, 0x3002}, {0x3001, 0x3002}}, 4, 0x7},
    {{15, 39, 41}, 3, {{0x3001, 0x3001}, {0x3002, 0x3002}, {0x3001, 0x3002}}, 3, 0x7},
    {{22}, 1, {{0x2000, 0x1000}}, 1, 0x5},
    {{32, 15, 33, 41}, 4, {{0x2000, 0x1000}}, 1, 0x2},
    {{27, 29}, 2, {{0x2000, 0x1000}}, 1, 0x5},
    {{31, 27, 39}, 3, {{0x1000, 0x3001}}, 1, 0x2},
    {{28}, 1, {{0x1000, 0x3001}, {0x3001, 0x3002}}, 2, 0x5},
    {{15, 19, 31, 28}, 4, {{0x2000, 0x3001}, {0x2000, 0x3002}}, 2, 0x7},
    {{31, 28}, 2, {{0x1000, 0x3001}, {0x3001, 0x3002}}, 2, 0x10},
    {{15, 1, 17, 15, 31}, 5, {{0x2000, 0x3001}, {0x1000, 0x3001}}, 2, 0x7},
    {{33, 27, 39}, 3, {{0x3001, 0x1000}}, 1, 0x2},
    {{27, 48, 15, 32}, 4, {{0x3001, 0x1000}, {0x1000, 0x1000}}, 2, 0x1},
    {{16, 48}, 2, {{0x1000, 0x2000}, {0x3001, 0x2000}}, 2, 0x1},
    {{16, 48}, 2, {{0x1000, 0x2000}, {0x3001, 0x2000}}, 2, 0x1},
    {{26, 43, 19}, 3, {{0x1000, 0x1000}}, 1, 0x0},
    {{24, 15, 31}, 3, {{0x1000, 0x3001}, {0x2000, 0x3001}}, 2, 0x0},
    {{31, 16, 30}, 3, {{0x1000, 0x3001}}, 1, 0x0},
    {{46, 16, 48}, 3, {{0x1000, 0x3001}}, 1, 0x2},
    {{27, 40}, 2, {{0x1000, 0x3001}}, 1, 0x5},
    {{15, 1, 17, 31, 28}, 5, {{0x2000, 0x3002}, {0x1000, 0x3002}}, 2, 0x7},
    {{40, 27, 9, 37}, 4, {{0x1000, 0x1000}}, 1, 0x0},
    {{31, 28}, 2, {{0x3001, 0x3002}}, 1, 0x0},
    {{39, 15, 32}, 3, {{0x1000, 0x3001}}, 1, 0x2},
    {{15, 31, 28}, 3, {{0x1000, 0x3002}}, 1, 0xC},
    {{27, 48, 15, 32}, 4, {{0x3001, 0x1000}, {0x1000, 0x1000}}, 2, 0x1},
    {{16, 48}, 2, {{0x1000, 0x2000}, {0x3001, 0x2000}}, 2, 0x1},
};

/* SAPI phone class flags @5ed36428 */
static const uint8_t PHONE_CLASS[50] = {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 5, 3, 3, 2, 2, 2, 1,
                                        3, 1, 2, 2, 7, 1, 3, 3, 3, 3, 2, 2, 1, 3, 1, 5, 1, 1, 2, 2, 3, 3, 3, 3, 7};

static int match_suffix(const char *w, int n, int *stem)
{
    size_t s;
    for (s = 0; s < sizeof SUFFIXES / sizeof SUFFIXES[0]; s++) {
        const char *rev = SUFFIXES[s].rev;
        int len = (int)strlen(rev), i = n - 1, k = 0, found = -1;
        if (i < 0 || w[i] != rev[0]) continue;
        for (;;) {
            if (i < 2) break;
            if (found != -1) {
                *stem = i + 1;
                return found;
            }
            i--;
            k++;
            if (k == len) found = SUFFIXES[s].rec;
            if (found == -1 && w[i] != rev[k]) break;
        }
        if (found != -1) {
            *stem = i + 1;
            return found;
        }
    }
    return -1;
}

static int lex_lookup(const sam_lexicon *lex, const char *w, int n, ent *out)
{
    char buf[128];
    sam_pron pr[MAXE];
    int k, c;
    if (n <= 0 || n >= (int)sizeof buf) return 0;
    memcpy(buf, w, (size_t)n);
    buf[n] = 0;
    c = sam_lexicon_lookup(lex, buf, pr, MAXE);
    for (k = 0; k < c; k++) {
        memcpy(out[k].ph, pr[k].ph, sizeof pr[k].ph);
        out[k].n = pr[k].n;
        out[k].pos = pr[k].pos;
    }
    return c;
}

static void append(ent *e, const uint16_t *ph, int n)
{
    int i;
    for (i = 0; i < n && e->n < SAM_MAX_PRON - 1; i++) e->ph[e->n++] = ph[i];
    e->ph[e->n] = 0;
}

static void add_plural(ent *e)
{
    int last = e->n ? e->ph[e->n - 1] : 0, cls = last < 50 ? PHONE_CLASS[last] : 0;
    static const uint16_t axz[2] = {P_AX, P_Z}, z = P_Z, s = P_S;
    if ((cls & 4) || last == P_S || last == P_Z) append(e, axz, 2);
    else if ((cls & 1) == 0 || (cls & 2)) append(e, &z, 1);
    else append(e, &s, 1);
}

static void add_past(ent *e)
{
    int last = e->n ? e->ph[e->n - 1] : 0;
    static const uint16_t axd[2] = {P_AX, P_D}, t = P_T, d = P_D;
    if (last == P_T || last == P_D) append(e, axd, 2);
    else if (last >= 50 || (PHONE_CLASS[last] & 2) == 0) append(e, &t, 1);
    else append(e, &d, 1);
}

static void apply_record(ent *e, int k)
{
    const record *r = &RECORDS[k];
    if (r->nph == 1 && r->ph[0] == P_S) {
        add_plural(e);
        return;
    }
    if (r->nph == 1 && r->ph[0] == P_D) {
        add_past(e);
        return;
    }
    if ((k == 29 || k == 30) && e->n) e->ph[e->n - 1] = P_S;
    append(e, r->ph, r->nph);
}

static int contains(const uint32_t *list, int n, uint32_t v)
{
    int i;
    for (i = 0; i < n; i++)
        if (list[i] == v) return 1;
    return 0;
}

/* FUN_5ed4e449 (+ FUN_5ed4e07b fallback) */
static int combine(const int *stack, int ns, ent *prons, int np, uint32_t lextype, sam_lookup_entry *out, int max)
{
    int count = 0, i, s;
    uint32_t seen[16];
    int nseen = 0;
    for (i = 0; i < np; i++) {
        ent e = prons[i];
        uint32_t cur[8];
        int ncur = 1, ok_all = 1;
        cur[0] = prons[i].pos;
        for (s = 0; s < ns; s++) {
            const record *r = &RECORDS[stack[s]];
            uint32_t nxt[8];
            int nn = 0, c, m, applied = 0;
            ent ne = e;
            for (c = 0; c < ncur; c++)
                for (m = 0; m < r->nmap; m++)
                    if (r->map[m][0] == cur[c] && !contains(nxt, nn, r->map[m][1]) && nn < 8) {
                        nxt[nn++] = r->map[m][1];
                        if (nn == 1 && !applied) {
                            apply_record(&ne, stack[s]);
                            applied = 1;
                        }
                    }
            if (!nn) {
                ok_all = 0;
                break;
            }
            e = ne;
            memcpy(cur, nxt, sizeof(uint32_t) * (size_t)nn);
            ncur = nn;
        }
        if (ok_all)
            for (s = 0; s < ncur; s++)
                if (!contains(seen, nseen, cur[s]) && nseen < 16 && count < max) {
                    seen[nseen++] = cur[s];
                    memcpy(out[count].ph, e.ph, sizeof e.ph);
                    out[count].n = e.n;
                    out[count].pos = cur[s];
                    out[count].lextype = lextype | 0x4000;
                    count++;
                }
    }
    if (!count && np) {
        ent e = prons[0];
        const record *last = &RECORDS[stack[ns - 1]];
        for (s = 0; s < ns; s++) append(&e, RECORDS[stack[s]].ph, RECORDS[stack[s]].nph);
        for (s = 0; s < last->nmap && count < max; s++) {
            memcpy(out[count].ph, e.ph, sizeof e.ph);
            out[count].n = e.n;
            out[count].pos = last->map[s][1];
            out[count].lextype = lextype | 0x4000;
            count++;
        }
    }
    return count;
}

static int lts_first(const sam_lts *lts, const char *w, int n, ent *out)
{
    char buf[128];
    sam_pron pr[4];
    if (n <= 0 || n >= (int)sizeof buf) return 0;
    memcpy(buf, w, (size_t)n);
    buf[n] = 0;
    if (sam_lts_pronounce(lts, buf, pr, 4) < 1) return 0;
    memcpy(out->ph, pr[0].ph, sizeof pr[0].ph);
    out->n = pr[0].n;
    out->pos = 0xFFFFFFFFu;
    return 1;
}

/* FUN_5ed4ee14 */
static int morph(const sam_lexicon *lex, const sam_lts *lts, const char *word, sam_lookup_entry *out, int max)
{
    char W[130];
    int n = (int)strlen(word), cut, stack[16], ns = 0, np = 0, lts_used = 0, found = 0, i;
    ent prons[MAXE];
    if (n <= 0 || n > 127) return 0;
    for (i = 0; i < n; i++) W[i] = (char)toupper((unsigned char)word[i]);
    W[n] = 0;
    cut = n;
    for (;;) {
        int sl, k = match_suffix(W, cut, &sl);
        const record *rec;
        char last;
        if (k == -1) {
            if (ns) {
                np = lts_first(lts, word, cut, prons);
                if (np) lts_used = found = 1;
            }
            break;
        }
        if (ns == 16) break;
        memmove(stack + 1, stack, sizeof(int) * (size_t)ns);
        stack[0] = k;
        ns++;
        rec = &RECORDS[k];
        if (k == 0) {
            if (W[sl - 1] == 'S') {
                memmove(stack, stack + 1, sizeof(int) * (size_t)(--ns));
                if (ns) {
                    np = lts_first(lts, word, sl + 1, prons);
                    if (np) lts_used = found = 1;
                }
                break;
            }
            if ((np = lex_lookup(lex, W, sl, prons)) > 0) {
                found = 1;
                break;
            }
            if (W[sl - 1] != 'E') {
                cut = sl;
                continue;
            }
            if (sl >= 2 && W[sl - 2] == 'I') {
                char save = W[sl - 2];
                W[sl - 2] = 'Y';
                if ((np = lex_lookup(lex, W, sl - 1, prons)) > 0) {
                    found = 1;
                    break;
                }
                W[sl - 2] = save;
            }
            if ((np = lex_lookup(lex, W, sl - 1, prons)) > 0) {
                found = 1;
                break;
            }
            cut = sl;
            continue;
        }
        if (k == 0x18 && sl + 4 <= n) {
            char save = W[sl + 3];
            W[sl + 3] = 'E';
            if ((np = lex_lookup(lex, W, sl + 4, prons)) > 0) {
                for (i = 0; i < np; i++)
                    if (prons[i].n > 2 && prons[i].ph[prons[i].n - 2] == P_AX && prons[i].ph[prons[i].n - 1] == P_L) {
                        prons[i].ph[prons[i].n - 2] = P_L;
                        prons[i].n--;
                    }
                stack[0] = 11;
                found = 1;
                break;
            }
            W[sl + 3] = save;
        } else if (k == 0x1A) {
            if ((np = lex_lookup(lex, W, sl + 2, prons)) > 0) {
                found = 1;
                break;
            }
            cut = sl + 2;
            continue;
        } else if (k == 0x1C) {
            char save = W[sl];
            W[sl] = 'Y';
            if ((np = lex_lookup(lex, W, sl + 1, prons)) > 0) {
                for (i = 0; i < np; i++)
                    if (prons[i].n && prons[i].ph[prons[i].n - 1] == P_IY) prons[i].n--;
                found = 1;
                break;
            }
            W[sl] = save;
            cut = sl;
            continue;
        } else if (k == 0x1D || k == 0x1E) {
            if ((np = lex_lookup(lex, W, sl + 2, prons)) > 0) {
                for (i = 0; i < np; i++)
                    if (prons[i].n) prons[i].ph[prons[i].n - 1] = P_S;
                found = 1;
                break;
            }
            cut = sl + 2;
            continue;
        }
        last = W[sl - 1];
        if ((rec->flags & 1) && last != 'O' && last != 'W' && last != 'Y' && (last != 'E' || k == 1)) {
            char save = W[sl];
            W[sl] = 'E';
            if ((np = lex_lookup(lex, W, sl + 1, prons)) > 0) {
                if (sl > 0 && W[sl - 1] == 'L')
                    for (i = 0; i < np; i++)
                        if (prons[i].n > 1 && prons[i].ph[prons[i].n - 2] == P_AX && prons[i].ph[prons[i].n - 1] == P_L) {
                            prons[i].ph[prons[i].n - 2] = P_L;
                            prons[i].n--;
                        }
                found = 1;
                break;
            }
            W[sl] = save;
        }
        if ((np = lex_lookup(lex, W, sl, prons)) > 0) {
            found = 1;
            break;
        }
        if ((rec->flags & 2) && W[sl - 1] == 'I') {
            W[sl - 1] = 'Y';
            if ((np = lex_lookup(lex, W, sl, prons)) > 0) {
                found = 1;
                break;
            }
            W[sl - 1] = 'I';
        }
        if ((rec->flags & 4) && !strchr("AEFHIKOSUWYZ", W[sl - 1]) && sl >= 2 && W[sl - 1] == W[sl - 2]) {
            if ((np = lex_lookup(lex, W, sl - 1, prons)) > 0) {
                found = 1;
                break;
            }
        }
        if (rec->flags & 0x10) {
            char save = W[sl];
            W[sl] = 'L';
            if ((np = lex_lookup(lex, W, sl + 1, prons)) > 0) {
                for (i = 0; i < np; i++)
                    if (prons[i].n && prons[i].ph[prons[i].n - 1] == P_L) prons[i].n--;
                found = 1;
                break;
            }
            W[sl] = save;
        }
        cut = sl;
    }
    if (!found || !ns || !np) return 0;
    if (lts_used) { /* FUN_5ed4eba0 */
        ent e = prons[0];
        ent es[4];
        int ne = 0, m;
        const record *r = &RECORDS[stack[0]];
        apply_record(&e, stack[0]);
        for (m = 0; m < r->nmap && ne < 4; m++) {
            int dup = 0, j;
            for (j = 0; j < ne; j++)
                if (es[j].pos == r->map[m][1]) dup = 1;
            if (!dup) {
                es[ne] = e;
                es[ne].pos = r->map[m][1];
                ne++;
            }
        }
        if (ns > 1) return combine(stack + 1, ns - 1, es, ne, 0x2000, out, max);
        for (m = 0; m < ne && m < max; m++) {
            memcpy(out[m].ph, es[m].ph, sizeof es[m].ph);
            out[m].n = es[m].n;
            out[m].pos = es[m].pos;
            out[m].lextype = 0x6000;
        }
        return ne < max ? ne : max;
    }
    return combine(stack, ns, prons, np, 0x1000, out, max);
}

/* first pronunciation, its POS list, and the first different pronunciation (FUN_5ed4b1f9) */
static void summarize(const sam_lookup_entry *e, int n, sam_lookup *out)
{
    int i;
    memcpy(out->pron, e[0].ph, sizeof e[0].ph);
    out->n = e[0].n;
    out->lextype = e[0].lextype;
    out->posa[out->nposa++] = e[0].pos;
    for (i = 1; i < n; i++) {
        if (e[i].n == e[0].n && !memcmp(e[i].ph, e[0].ph, sizeof(uint16_t) * (size_t)e[0].n)) {
            if (out->nposa < 4) out->posa[out->nposa++] = e[i].pos;
        } else if (out->nposb < 4) {
            out->posb[out->nposb++] = e[i].pos;
            if (!out->nalt) {
                memcpy(out->alt, e[i].ph, sizeof e[i].ph);
                out->nalt = e[i].n;
            }
        }
    }
}

static int vendor(const sam_lexicon *lex, const char *word, sam_lookup_entry *e)
{
    sam_pron pr[MAXE];
    int c = sam_lexicon_lookup(lex, word, pr, MAXE), i;
    for (i = 0; i < c; i++) {
        memcpy(e[i].ph, pr[i].ph, sizeof pr[i].ph);
        e[i].n = pr[i].n;
        e[i].pos = pr[i].pos;
        e[i].lextype = 0x1000;
    }
    return c;
}

static int has_pos(const sam_lookup_entry *e, int n, uint32_t pos)
{
    int i;
    for (i = 0; i < n; i++)
        if (e[i].pos == pos) return 1;
    return 0;
}

int sam_word_lookup_pos(const sam_lexicon *lex, const sam_lts *lts, const char *word, uint32_t pos, sam_lookup *out)
{
    sam_lookup_entry e[MAXE];
    int n = 0;
    memset(out, 0, sizeof *out);
    if (pos) { /* a forced part of speech first looks for a list that has it */
        n = vendor(lex, word, e);
        if (!has_pos(e, n, pos)) {
            n = morph(lex, lts, word, e, MAXE);
            if (!has_pos(e, n, pos)) n = 0;
        }
    }
    if (!n) n = vendor(lex, word, e);
    if (!n) n = morph(lex, lts, word, e, MAXE);
    if (!n) {
        sam_pron pr[4];
        if (sam_lts_pronounce(lts, word, pr, 4) < 1) return 0;
        memcpy(e[0].ph, pr[0].ph, sizeof pr[0].ph);
        e[0].n = pr[0].n;
        e[0].pos = 0x1000;
        e[0].lextype = 0x2000;
        n = 1;
    }
    summarize(e, n, out);
    return 1;
}

int sam_word_lookup(const sam_lexicon *lex, const sam_lts *lts, const char *word, sam_lookup *out)
{
    return sam_word_lookup_pos(lex, lts, word, 0, out);
}
