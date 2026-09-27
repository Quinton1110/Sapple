/* Part-of-speech tagging and homograph selection, ported from spttseng.dll:
 *   FUN_5ed4b6e0  driver: special words (table 0x5edaa180), lookup chain, tagger, post-tagging words (0x5edaaba0)
 *   FUN_5ed4a436  63 contextual transformation rules (table 0x5ed32038)
 *   FUN_5ed4a3bc  tag change, allowed only to a part of speech the word has (switches pronunciation for posb) */
#include "sam_pos.h"
#include "sam_norm_types.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t from, to;
    int type;
    uint32_t tag1, tag2;
    const char *word1, *word2;
} pos_rule;

typedef struct {
    const char *word;
    const char *pron[3];
    uint32_t pos[3];
    int handler;
} special_word;

/* tag-time handlers of abbreviations (@0x5ede9880) in terms of the special-word handlers */
static const int ABBREV_HANDLER[5] = {0, 1, 2, 6, 7};

#include "sam_pos_rules.h"
#include "sam_words_tab.h"

/* ---------------------------------------------------------------------------------------------- */
/* helpers                                                                                        */

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static int icmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

static int inicmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}

static int is_up(int c) { return c >= 'A' && c <= 'Z'; }
static int is_low(int c) { return c >= 'a' && c <= 'z'; }
static int is_dig(int c) { return c >= '0' && c <= '9'; }

static int all_upper(const char *s)
{
    for (; *s; s++)
        if (!is_up((unsigned char)*s)) return 0;
    return 1;
}

/* SAPI American English phone set: 1 '-' ... 9 '2', 10 aa ... 49 zh */
static const char *const PHONES[] = {"-", "!", "&", ",", ".", "?", "_", "1", "2", "aa", "ae", "ah", "ao", "aw",
                                     "ax", "ay", "b", "ch", "d", "dh", "eh", "er", "ey", "f", "g", "h", "ih",
                                     "iy", "jh", "k", "l", "m", "n", "ng", "ow", "oy", "p", "r", "s", "sh", "t",
                                     "th", "uh", "uw", "v", "w", "y", "z", "zh"};

static int parse_pron(const char *s, uint16_t *out)
{
    int n = 0;
    while (*s && n < SAM_MAX_PRON) {
        char tok[8];
        int k = 0, i;
        while (*s == ' ') s++;
        while (*s && *s != ' ' && k < 7) tok[k++] = *s++;
        tok[k] = 0;
        if (!k) break;
        for (i = 0; i < (int)(sizeof PHONES / sizeof *PHONES); i++) {
            if (ieq(tok, PHONES[i])) {
                out[n++] = (uint16_t)(i + 1);
                break;
            }
        }
    }
    return n;
}

static void set_pron(sam_tag_entry *e, int slot, const char *ascii)
{
    e->npron[slot] = ascii ? parse_pron(ascii, e->pron[slot]) : 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* entries                                                                                        */

void sam_pos_entry_from_lookup(sam_tag_entry *e, const sam_lookup *r)
{
    int k;
    memcpy(e->pron[0], r->pron, sizeof(uint16_t) * (size_t)r->n);
    e->npron[0] = r->n;
    memcpy(e->pron[1], r->alt, sizeof(uint16_t) * (size_t)r->nalt);
    e->npron[1] = r->nalt;
    e->nposa = r->nposa;
    e->nposb = r->nposb;
    for (k = 0; k < 4; k++) {
        e->posa[k] = r->posa[k];
        e->posb[k] = r->posb[k];
    }
    e->altok = r->nalt > 0;
    e->idx = 0;
    e->pos = r->nposa ? r->posa[0] : 0;
}

/* FUN_5ed4a3bc */
static void change_tag(sam_tag_entry *e, uint32_t to)
{
    int k;
    for (k = 0; k < e->nposa; k++) {
        if (e->posa[k] == to) {
            e->idx = 0;
            e->pos = to;
            return;
        }
    }
    if (e->altok) {
        for (k = 0; k < e->nposb; k++) {
            if (e->posb[k] == to) {
                e->idx = 1;
                e->pos = to;
                return;
            }
        }
    }
}

/* FUN_5ed4a436. Rule types 0x13/0x14 read outside the array in the engine at the edges; those
 * reads are treated as no match. */
void sam_pos_tag(sam_tag_entry *e, int n)
{
    int r, i;
#define TAG(j) (e[j].pos)
#define WORD(j, w) ((j) >= 0 && (j) < n && !icmp(e[j].text, (w)))
    for (r = 0; r < (int)(sizeof POS_RULES / sizeof *POS_RULES); r++) {
        const pos_rule *R = &POS_RULES[r];
        int lo = 0, hi = n, min_n = 1;
        switch (R->type) {
        case 0: case 0xd: case 0xe: case 0xf: case 0x15: case 0x17: case 0x1a: case 0x1c:
            lo = 1; min_n = 2; break;
        case 1: case 0x10: case 0x14: case 0x16: case 0x18: case 0x1b: case 0x1d:
            hi = n - 1; min_n = 2; break;
        case 2: case 0x11: lo = 2; min_n = 3; break;
        case 3: case 0x12: hi = n - 2; min_n = 3; break;
        case 4: lo = 1; min_n = 3; break;
        case 5: hi = n - 1; min_n = 3; break;
        case 6: lo = 1; min_n = 4; break;
        case 7: hi = n - 1; min_n = 4; break;
        case 8: lo = 1; hi = n - 1; min_n = 3; break;
        case 9: lo = 1; hi = n - 2; min_n = 4; break;
        case 10: lo = 2; hi = n - 1; min_n = 4; break;
        case 0x13: hi = n - 1; min_n = 3; break;
        default: break;
        }
        if (n < min_n) continue;
        for (i = lo; i < hi; i++) {
            int m = 0;
            if (e[i].lock != 0 || TAG(i) != R->from) continue;
            switch (R->type) {
            case 0: m = TAG(i - 1) == R->tag1; break;
            case 1: m = TAG(i + 1) == R->tag1; break;
            case 2: m = TAG(i - 2) == R->tag1; break;
            case 3: m = TAG(i + 2) == R->tag1; break;
            case 4: m = TAG(i - 1) == R->tag1 || (i > 1 && TAG(i - 2) == R->tag1); break;
            case 5: m = TAG(i + 1) == R->tag1 || (i < n - 2 && TAG(i + 2) == R->tag1); break;
            case 6:
                m = TAG(i - 1) == R->tag1 || (i > 1 && TAG(i - 2) == R->tag1) || (i > 2 && TAG(i - 3) == R->tag1);
                break;
            case 7:
                m = TAG(i + 1) == R->tag1 || (i < n - 2 && TAG(i + 2) == R->tag1) ||
                    (i < n - 3 && TAG(i + 3) == R->tag1);
                break;
            case 8: m = TAG(i - 1) == R->tag1 && TAG(i + 1) == R->tag2; break;
            case 9: m = TAG(i - 1) == R->tag1 && TAG(i + 2) == R->tag2; break;
            case 10: m = TAG(i - 2) == R->tag1 && TAG(i + 1) == R->tag2; break;
            case 0xb: m = !is_up((unsigned char)e[i].text[0]); break;
            case 0xc: m = is_up((unsigned char)e[i].text[0]); break;
            case 0xd: m = !is_up((unsigned char)e[i - 1].text[0]); break;
            case 0xe: m = is_up((unsigned char)e[i - 1].text[0]); break;
            case 0xf: m = WORD(i - 1, R->word1); break;
            case 0x10: m = WORD(i + 1, R->word1); break;
            case 0x11: m = WORD(i - 2, R->word1); break;
            case 0x12: m = WORD(i + 2, R->word1); break;
            case 0x13: m = WORD(i - 1, R->word1) || WORD(i - 2, R->word1); break;
            case 0x14: m = WORD(i + 1, R->word1) || WORD(i + 2, R->word1); break;
            case 0x15: m = WORD(i, R->word1) && WORD(i - 1, R->word2); break;
            case 0x16: m = WORD(i, R->word1) && WORD(i + 1, R->word2); break;
            case 0x17: m = WORD(i, R->word1) && TAG(i - 1) == R->tag1; break;
            case 0x18: m = WORD(i, R->word1) && TAG(i + 1) == R->tag1; break;
            case 0x19: m = WORD(i, R->word1); break;
            case 0x1a: m = TAG(i - 1) == R->tag1 && WORD(i - 1, R->word1); break;
            case 0x1b: m = TAG(i + 1) == R->tag1 && WORD(i + 1, R->word1); break;
            case 0x1c: m = WORD(i, R->word1) && TAG(i - 1) == R->tag1 && WORD(i - 1, R->word2); break;
            case 0x1d: m = WORD(i, R->word1) && TAG(i + 1) == R->tag1 && WORD(i + 1, R->word2); break;
            default: break;
            }
            if (m) change_tag(&e[i], R->to);
        }
    }
#undef TAG
#undef WORD
}

/* ---------------------------------------------------------------------------------------------- */
/* special words (handlers at 0x5ede98a8)                                                         */

static const special_word *find_word(const special_word *tab, int n, const char *w)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = icmp(w, tab[mid].word);
        if (!c) return &tab[mid];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

/* pronunciation k of the record as the only pronunciation (count left as it is) */
static void pick(sam_tag_entry *e, const special_word *s, int k)
{
    set_pron(e, 0, s->pron[k]);
    e->posa[0] = s->pos[k];
    e->pos = s->pos[k];
}

static void reset_counts(sam_tag_entry *e)
{
    e->nposa = 1;
    e->nposb = 0;
    e->npron[1] = 0;
    e->altok = 0;
    e->idx = 0;
}

static int open_type(int t) { return t == 1 || t == 2 || t == 3 || t == 7 || t == 8; }

static int is_number_word(const char *s)
{
    static const char *const W[] = {"four", "five", "nine", "three", "seven", "eight", "forty", "fifty", "sixty",
                                    "twenty", "thirty", "eighty", "ninety", "eleven", "twelve", "seventy",
                                    "fifteen", "sixteen", "thirteen", "fourteen", "eighteen", "nineteen"};
    size_t k;
    for (k = 0; k < sizeof W / sizeof *W; k++)
        if (!strcmp(s, W[k])) return 1;
    return 0;
}


/* fraction node whose words end in a denominator ("five sixths"): a following unit reads "of a" */
static int frac_no_over(const sam_node *n)
{
    const numinfo *ni = (const numinfo *)n->ti;
    return n->type == SAM_NODE_FRACTION && ni && ni->fr && ni->fr->over == 0;
}

/* pronunciation "of a <unit>" / "of an <unit>" (strings @0x5edab164 / 0x5edab168) */
static void of_a(sam_tag_entry *e, const char *unit, int check_vowel)
{
    uint16_t u[SAM_MAX_PRON];
    int n = parse_pron(unit, u), k = 0;
    const char *prefix = "ah 2 v & ax 2 &";
    if (check_vowel && n > 0) {
        static const uint16_t V[] = {0xa, 0xb, 0xc, 0xd, 0xe, 0xf, 0x10, 0x15, 0x16, 0x17, 0x1b, 0x1c, 0x23, 0x24, 0x2a, 0x2b};
        size_t j;
        for (j = 0; j < sizeof V / sizeof *V; j++)
            if (u[0] == V[j]) prefix = "ah 2 v & ax 2 n &";
    }
    e->npron[0] = parse_pron(prefix, e->pron[0]);
    for (k = 0; k < n && e->npron[0] < SAM_MAX_PRON; k++) e->pron[0][e->npron[0]++] = u[k];
}

/* FUN_5ed46cb0: units, singular after "1"/"one"/ordinals, plural after other numbers */
static void h_units(sam_tag_entry *e, const special_word *s, const sam_node *nodes, int ci)
{
    const sam_node *P, *PP;
    int single = 0;
    if (ci == 0) {
        if (s->pron[2]) pick(e, s, 2);
        else pick(e, s, 1);
        return;
    }
    reset_counts(e);
    P = &nodes[ci - 1];
    PP = ci >= 2 ? &nodes[ci - 2] : NULL;
    if (P->type == SAM_NODE_CARDINAL || P->type == 0x1014) {
        single = !strcmp(P->text, "1") || !strcmp(P->text, "-1");
    } else if (P->type == SAM_NODE_ORDINAL) {
        single = 1;
    } else if (P->type == SAM_NODE_FRACTION && frac_no_over(P)) {
        of_a(e, s->pron[0], 1);
        e->posa[0] = s->pos[0];
        e->pos = s->pos[0];
        return;
    } else if (P->type == SAM_NODE_DECIMAL || P->type == SAM_NODE_FRACTION || P->type == 0x100f) {
        single = 0;
    } else {
        size_t len = strlen(P->text);
        int unitprefix = (len == 3 && (!inicmp(P->text, "cu.", 3) || !inicmp(P->text, "sq.", 3) ||
                                       !inicmp(P->text, "fl.", 3))) ||
                         (len == 2 && (!inicmp(P->text, "cu", 2) || !inicmp(P->text, "sq", 2) ||
                                       !inicmp(P->text, "fl", 2)));
        if (len == 3 && !inicmp(P->text, "one", 3)) {
            single = 1;
        } else if (unitprefix) {
            if (!PP) return; /* the engine leaves the entry empty */
            if (PP->type == SAM_NODE_CARDINAL || PP->type == 0x1014)
                single = !strcmp(PP->text, "1") || !strcmp(PP->text, "-1");
            else if (PP->type == SAM_NODE_ORDINAL) single = 1;
            else if (PP->type == SAM_NODE_FRACTION) single = frac_no_over(PP);
            else if (PP->type == SAM_NODE_DECIMAL || PP->type == 0x100f) single = 0;
            else single = strlen(PP->text) == 3 && !inicmp(PP->text, "one", 3);
        } else if (len == 3 && (!strcmp(P->text, "two") || !strcmp(P->text, "six") || !strcmp(P->text, "ten"))) {
            single = 0;
        } else if (!is_number_word(P->text) && s->pron[2]) {
            pick(e, s, 2);
            return;
        }
    }
    pick(e, s, single ? 0 : 1);
}

/* FUN_5ed4730e: Dr / St / Gov */
static void h_title(sam_tag_entry *e, const special_word *s, const sam_node *nodes, int nn, int ci)
{
    const sam_node *N = ci + 1 < nn ? &nodes[ci + 1] : NULL;
    const sam_node *P = ci > 0 ? &nodes[ci - 1] : NULL;
    int first = 0;
    reset_counts(e);
    if (N && N->pos != 0x400e) {
        const char *t = N->text;
        size_t len = strlen(t);
        int lowercase_next = len == 0 || !is_up((unsigned char)t[0]);
        if (!lowercase_next) {
            size_t u = 1;
            while (u < len && is_low((unsigned char)t[u])) u++;
            /* the engine tests t[u+1] and t[u+2] (off by one), so "John's" never matches */
            if (len >= 2 && u == len - 2 && t[u + 1] == '\'' && t[u + 2] == 's') u += 2;
            if (u == len && strncmp(t, "North", 5) && strncmp(t, "South", 5) && strncmp(t, "West", 4) &&
                strncmp(t, "East", 4) &&
                (len != 2 || (strncmp(t, "Ne", 2) && strncmp(t, "Nw", 2) && strncmp(t, "Se", 2) && strncmp(t, "Sw", 2))) &&
                (len != 1 || (t[0] != 'N' && t[0] != 'S' && t[0] != 'E' && t[0] != 'W'))) {
                first = 1;
                if (P && P->text[0] && is_up((unsigned char)P->text[0])) {
                    size_t k = 1, pl = strlen(P->text);
                    while (k < pl && is_low((unsigned char)P->text[k])) k++;
                    if (k == pl) first = 0; /* "Main St John": street */
                }
            } else if (u == 1 && len == 2 && t[1] == '.') {
                first = 1;
            } else {
                lowercase_next = 1;
            }
        }
        if (lowercase_next) first = !P || open_type(P->type);
    }
    pick(e, s, first ? 0 : 1);
}

static void apply_special(sam_tag_entry *e, const special_word *s, const sam_node *nodes, int nn, int ci)
{
    const sam_node *C = &nodes[ci];
    const sam_node *N = ci + 1 < nn ? &nodes[ci + 1] : NULL;
    const sam_node *P = ci > 0 ? &nodes[ci - 1] : NULL;
    switch (s->handler) {
    case 0: h_units(e, s, nodes, ci); break;
    case 1: h_title(e, s, nodes, nn, ci); break;
    case 2: /* FUN_5ed47629: "fig 3", "p 12" */
        reset_counts(e);
        pick(e, s, N && is_dig((unsigned char)N->text[0]) ? 0 : 1);
        break;
    case 3: /* FUN_5ed476fa: all capitals spell the letters */
        reset_counts(e);
        pick(e, s, all_upper(C->text) ? 0 : 1);
        break;
    case 4: /* FUN_5ed477e0: Mar / Sat / Wed capitalized are dates */
        reset_counts(e);
        pick(e, s, is_up((unsigned char)C->text[0]) ? 0 : 1);
        break;
    case 5: /* FUN_5ed478b3: capitals spell, otherwise a unit */
        reset_counts(e);
        if (all_upper(C->text)) pick(e, s, 2);
        else h_units(e, s, nodes, ci);
        break;
    case 6: /* FUN_5ed4798d: C / F / K after a temperature */
        reset_counts(e);
        pick(e, s, P && P->type == 0x100a ? 0 : 1);
        break;
    case 7: /* FUN_5ed47c77: cu / fl / sq */
        if (all_upper(C->text) || !P) {
            pick(e, s, 0);
        } else {
            reset_counts(e);
            if (frac_no_over(P)) {
                of_a(e, s->pron[1], 0);
                e->posa[0] = s->pos[1];
                e->pos = s->pos[1];
            } else {
                pick(e, s, 1);
            }
        }
        break;
    case 8: /* FUN_5ed4bea5: "a" is the article unless it stands alone */
    {
        int article = 1;
        if (N) {
            if (!(C->count < 2 && C->type == SAM_NODE_WORD)) article = 0;
            else article = (N->type & 0x1000) != 0;
        }
        if (article) {
            set_pron(e, 0, s->pron[1]);
            e->posa[0] = s->pos[1];
            e->nposa = 1;
            e->pos = s->pos[1];
            set_pron(e, 1, s->pron[0]);
            e->posb[0] = s->pos[0];
            e->nposb = 1;
            e->altok = 1;
        } else {
            pick(e, s, 0);
        }
        break;
    }
    case 9: /* FUN_5ed4bfb1: Polish / polish */
        if (N && is_up((unsigned char)C->text[0])) {
            if (P && !open_type(P->type)) {
                set_pron(e, 0, s->pron[0]);
                e->posa[0] = 0x1000;
                e->pos = 0x1000;
            } else {
                pick(e, s, 1);
            }
        } else {
            set_pron(e, 0, s->pron[1]);
            e->posa[0] = s->pos[1];
            e->posa[1] = s->pos[2];
            e->nposa = 2;
            set_pron(e, 1, s->pron[0]);
            e->posb[0] = s->pos[0];
            e->nposb = 1;
            e->pos = s->pos[1];
            e->altok = 1;
        }
        break;
    default: break;
    }
    e->star = 1;
}

/* ---------------------------------------------------------------------------------------------- */
/* post-tagging words (handlers at 0x5ede98f8)                                                    */

static int is_vowel_id(int id)
{
    static const int V[] = {0xa, 0xb, 0xc, 0xd, 0xe, 0xf, 0x10, 0x15, 0x16, 0x17, 0x1b, 0x1c, 0x23, 0x24, 0x2a, 0x2b};
    size_t k;
    for (k = 0; k < sizeof V / sizeof *V; k++)
        if (V[k] == id) return 1;
    return 0;
}

static const sam_tag_entry *first_entry(const sam_node *n, const sam_tag_entry *e)
{
    return n->count ? &e[n->first] : NULL;
}

static uint32_t node_pos(const sam_node *n, const sam_tag_entry *e)
{
    return n->count ? e[n->first].pos : n->pos;
}

static const char *node_word(const sam_node *n, const sam_tag_entry *e)
{
    return n->count ? e[n->first].text : "";
}

static void set_final(sam_tag_entry *e, const special_word *s, int k)
{
    set_pron(e, 0, s->pron[k]);
    e->idx = 0;
    e->pos = s->pos[k];
    e->star = 1;
}

/* FUN_5ed4bd97: "the" before a vowel */
static void post_the(const special_word *s, sam_node *nodes, int nn, int ci, sam_tag_entry *e)
{
    const sam_tag_entry *nx;
    sam_tag_entry *cur = &e[nodes[ci].first];
    if (ci + 1 >= nn) return;
    nx = first_entry(&nodes[ci + 1], e);
    set_final(cur, s, nx && nx->npron[nx->idx] && is_vowel_id(nx->pron[nx->idx][0]) ? 0 : 1);
}

/* FUN_5ed4c114: "read" present or past */
static void post_read(const special_word *s, sam_node *nodes, int nn, int ci, sam_tag_entry *e)
{
    sam_tag_entry *cur = &e[nodes[ci].first];
    int j = ci - 1, present = 0;
    (void)nn;
    if (j < 0) {
        set_final(cur, s, 0);
        return;
    }
    while (j > 0 && node_pos(&nodes[j], e) != 0x4001 && node_pos(&nodes[j], e) != 0x4007) j--;
    if (node_pos(&nodes[j], e) == 0x4001) {
        static const char *const PAST[] = {"have", "has", "had", "am", "ain't", "are", "aren't", "be", "is", "was", "were"};
        const char *w = node_word(&nodes[j], e);
        size_t k;
        present = 1;
        for (k = 0; k < sizeof PAST / sizeof *PAST; k++)
            if (!inicmp(w, PAST[k], strlen(PAST[k]))) present = 0;
    } else if (node_pos(&nodes[j], e) == 0x4007) {
        const char *ap = strchr(node_word(&nodes[j], e), '\'');
        present = ap && !inicmp(ap, "'ll", 3);
    } else {
        const char *w = node_word(&nodes[ci - 1], e);
        present = strlen(w) == 2 && !inicmp(w, "to", 2);
    }
    set_final(cur, s, present ? 0 : 1);
}

/* ---------------------------------------------------------------------------------------------- */

int sam_pos_process(const sam_lexicon *lex, const sam_lts *lts, sam_node *nodes, int nn, sam_tag_entry *e)
{
    int i, k, k2, n = 0;
    for (i = 0; i < nn; i++) {
        if ((nodes[i].type == SAM_NODE_ABBREV || nodes[i].type == SAM_NODE_ABBREV2) && nodes[i].ti && nodes[i].count) {
            /* FUN_5ed4b6e0: abbreviation records are fixed or go through a tag-time handler */
            const sam_abbrev *a = ((const sam_abbrev_ti *)nodes[i].ti)->rec;
            sam_tag_entry *x = &e[nodes[i].first];
            if (a->thandler < 0) {
                set_pron(x, 0, a->pron[0]);
                x->nposa = 1;
                x->posa[0] = a->pos[0];
                x->pos = a->pos[0];
                x->star = 1;
            } else {
                special_word sw;
                sw.word = a->word;
                sw.pron[0] = a->pron[0];
                sw.pron[1] = a->pron[1];
                sw.pron[2] = a->pron[2];
                sw.pos[0] = a->pos[0];
                sw.pos[1] = a->pos[1];
                sw.pos[2] = a->pos[2];
                sw.handler = ABBREV_HANDLER[a->thandler];
                apply_special(x, &sw, nodes, nn, i);
            }
            n = nodes[i].first + nodes[i].count;
            continue;
        }
        for (k = 0; k < nodes[i].count; k++) {
            sam_tag_entry *x = &e[nodes[i].first + k];
            const special_word *s = find_word(SPECIAL_WORDS, (int)(sizeof SPECIAL_WORDS / sizeof *SPECIAL_WORDS), x->text);
            if (s) {
                apply_special(x, s, nodes, nn, i);
            } else {
                sam_lookup r;
                uint32_t lock = x->lock;
                if (!sam_word_lookup_pos(lex, lts, x->text, lock, &r)) {
#ifdef SAM_STRICT_NOT_IN_LEX
                    return -1; /* SPERR_NOT_IN_LEX: the original engine drops the whole Speak call */
#else
                    /* ClassicVoices patch: a screen reader must never lose the rest of the text because
                     * one token is unpronounceable. Retry with the token's ASCII letters (at most 40,
                     * well inside the letter-to-sound limit), and if that still fails the token
                     * becomes a silent word instead of an error. */
                    char alt[48];
                    int j, m = 0;
                    for (j = 0; x->text[j] && m < 40; j++) {
                        int c = (unsigned char)x->text[j];
                        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                        if (c >= 'a' && c <= 'z') alt[m++] = (char)c;
                    }
                    alt[m] = 0;
                    lock = 0;
                    x->lock = 0;
                    if (!m || !sam_word_lookup_pos(lex, lts, alt, 0, &r)) {
                        memset(&r, 0, sizeof r);
                        r.pron[0] = 7; /* SAPI "_" (silence): the word keeps its slot but makes no sound */
                        r.n = 1;
                        r.posa[0] = 0x1000;
                        r.nposa = 1;
                        r.lextype = 0x2000;
                    }
#endif
                }
                sam_pos_entry_from_lookup(x, &r);
                if (lock) { /* FUN_5ed4b1f9: a forced part of speech picks its pronunciation or is dropped */
                    int found = 0;
                    for (k2 = 0; k2 < x->nposa; k2++)
                        if (x->posa[k2] == lock) {
                            x->idx = 0;
                            found = 1;
                        }
                    if (x->altok)
                        for (k2 = 0; k2 < x->nposb; k2++)
                            if (x->posb[k2] == lock) {
                                x->idx = 1;
                                found = 1;
                            }
                    if (found) x->pos = lock;
                    else x->lock = 0;
                }
            }
        }
        if (nodes[i].count) n = nodes[i].first + nodes[i].count;
    }
    sam_pos_tag(e, n);
    for (i = 0; i < nn; i++) {
        char key[128];
        size_t len;
        const special_word *s;
        if (nodes[i].type != SAM_NODE_WORD && nodes[i].type != SAM_NODE_ABBREV && nodes[i].type != SAM_NODE_ABBREV2) continue;
        if (!nodes[i].count) continue;
        strncpy(key, nodes[i].text, sizeof key - 1);
        key[sizeof key - 1] = 0;
        len = strlen(key);
        if (len > 1 && key[len - 1] == '.') key[len - 1] = 0;
        s = find_word(POST_WORDS, (int)(sizeof POST_WORDS / sizeof *POST_WORDS), key);
        if (!s) continue;
        if (s->handler == 1) post_the(s, nodes, nn, i, e);
        else if (s->handler == 2) post_read(s, nodes, nn, i, e);
        /* handler 0 (units after a cardinal, before a noun) tests a node field that is 0 for words */
    }
    return 0;
}
