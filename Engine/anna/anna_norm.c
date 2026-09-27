/* Anna text normalizer: text -> sentences -> normalized word records (anna_token), ported from
 * MSTTSFrontendENU.dll (Microsoft Anna, TTS20). Addresses are VAs in that DLL (image base 0x0D5D0000).
 *
 *   0x0D5F5285  sentence builder: walks the SAPI text fragments, at most 50 tokens per sentence, adds a
 *               period when the sentence has no end mark
 *   0x0D5F469F  tokenizer / sentence breaker: whitespace token, user-lexicon test, cp1252 folding, leading
 *               and trailing punctuation, initials 0x0D5DE85A, abbreviations (table 0x0D602A30, sentence
 *               handlers 0x0D5DE1DB/DE355/DE6CB/DE502)
 *   0x0D5EB0A1  core token -> 0x0D5EA96F dispatcher (SAPI <context>, dates, states, currency, times, phone
 *               numbers, numbers, ranges, decades, durations, hyphens) -> 0x0D5EAD92 words of the type
 *
 * The engine works on the UTF-16 text in place (it folds each token and patches '-' while parsing ranges);
 * so does this port. The engine's enumerator fields are kept: p = this+0x44 (token start), fe = this+0x48
 * (fragment end), te = this+0x4c (token end), e = this+0x50 (core end), fi = this+0x40 (fragment).
 * Handler return codes: 0 ok, DECLINE (E_INVALIDARG: "not mine, try the next handler").
 * The code descends from the Microsoft Sam normalizer port (tts-random/src/sam_norm.c); every function was
 * re-checked against Anna's DLL, which differs in many details (see notes/norm.md). */
#include "anna_front.h"
#include "anna_norm_types.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define DECLINE (-1)
#define NOMEM (-2)

/* SPVSTATE of a fragment (the engine hands word records a pointer to one) */
typedef struct anna_vstate {
    int action; /* SPVACTIONS: 0 speak, 1 silence, 2 pronounce, 3 bookmark, 4 spell, 5 section, 6 unknown tag */
    int emph, rate, vol, pitch, range, sil;
    const wc *phones; /* pPhoneIds */
    int pos;          /* ePartOfSpeech */
    const wc *ctx;    /* Context.pCategory (NUL-terminated) */
} anna_vstate;

typedef struct {
    anna_vstate st;
    wc *t;  /* pTextStart (into nm->buf) */
    int len, ofs;
    int group; /* SAPI calls the engine once per group: <voice> and <lang> tags split the text */
} afrag;

/* item of the sentence list (0x1c: text, len, ofs, words, count, POS, type record) */
typedef struct aitem {
    struct aitem *next, *prev;
    const wc *t;
    const char *s; /* static text (the added period) */
    int len, ofs;
    anna_wlist words;
    int pos;
    int *ti;
} aitem;

typedef struct {
    aitem *head, *tail;
} ilist;

typedef struct ablk {
    struct ablk *next;
    size_t used, cap;
    double data[1];
} ablk;

struct anna_norm {
    wc *buf;
    int n;
    afrag *fr;
    int nfr;
    int flags;
    int fi;                  /* this+0x40: current fragment, -1 = none */
    wc *p, *fe, *te, *e;     /* this+0x44, 0x48, 0x4c, 0x50 */
    ablk *arena;
    ilist items;
    anna_token *tok;
    int ntok, tokcap;
    int (*userlex)(void *, const uint16_t *, int);
    void *userlex_ctx;
    void *xmlmem; /* strings made by the XML parser (context ids, PhoneIDs) */
    int group_next; /* XML parser: the next fragment starts a new engine Speak() call */
    int gnext;      /* first fragment of the next group (-1 = none) */
};

static const anna_vstate DEFAULT_STATE = {0, 0, 0, 100, 0, 0, 0, NULL, 0, NULL}; /* @0x0D5D9678 */

/* ============================================================================================== */
/* character classes (msvcrt "C" locale, Latin-1 range)                                           */

static int is_ws(wc c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0x200b; } /* 0x0D5F3EE8 */
static int w_isalpha(wc c)
{
    if (c < 0x80) return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return c >= 0xc0 && c <= 0xff && c != 0xd7 && c != 0xf7;
}
static int w_isupper(wc c) { return (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7); }
static int w_islower(wc c) { return (c >= 'a' && c <= 'z') || (c >= 0xdf && c <= 0xff && c != 0xf7); }
static int w_isdigit(wc c) { return (c >= '0' && c <= '9') || c == 0xb2 || c == 0xb3 || c == 0xb9; } /* iswdigit */
static int a_isdigit(wc c) { return c >= '0' && c <= '9'; }                                        /* (c-'0') < 10 */
static int a_toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int a_tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int wlen(const wc *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* _wcsnicmp against an ASCII string */
static int wnicmp_a(const wc *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int x = a_tolower(a[i]), y = a_tolower((unsigned char)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}

/* wcsncmp against an ASCII string */
static int wncmp_a(const wc *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int x = a[i], y = (unsigned char)b[i];
        if (x != y || !x) return x - y;
    }
    return 0;
}

static int wnicmp_w(const wc *a, const wc *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int x = a_tolower(a[i]), y = a_tolower(b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}

/* _wcsicmp of NUL-terminated wide string against ASCII */
static int wicmp_a(const wc *a, const char *b)
{
    int i;
    for (i = 0;; i++) {
        int x = a_tolower(a[i]), y = a_tolower((unsigned char)b[i]);
        if (x != y || !x) return x - y;
    }
}

static const wc *wchr(const wc *s, wc c)
{
    for (; *s; s++)
        if (*s == c) return s;
    return c ? NULL : s;
}

/* the engine's punctuation classes; 0x1001 = none */
static int open_type(wc c) { return c == '(' ? 1 : c == '[' ? 2 : c == '{' ? 3 : 0x1001; }  /* 0x0D5EB455 */
static int close_type(wc c) { return c == ')' ? 4 : c == ']' ? 5 : c == '}' ? 6 : 0x1001; } /* 0x0D5E0414 */
static int quote_type(wc c) { return c == '\'' ? 7 : c == '"' ? 8 : 0x1001; }               /* 0x0D5E044C */
static int sent_type(wc c) { return c == '.' ? 9 : c == '!' ? 10 : c == '?' ? 11 : 0x1001; } /* 0x0D5E0479 */
static int clause_type(wc c) /* 0x0D5E04B1 */
{
    return c == ',' ? 12 : c == ';' ? 13 : c == ':' ? 14 : c == '-' ? 15 : 0x1001;
}
static int is_punct_any(wc c)
{
    return clause_type(c) != 0x1001 || close_type(c) != 0x1001 || quote_type(c) != 0x1001 || sent_type(c) != 0x1001;
}

static int punct_pos(int t) /* 0x0D5F3E7B */
{
    switch (t) {
    case 1: case 2: case 3: return 0x400c;
    case 4: case 5: case 6: return 0x400d;
    case 7: case 8: return 0x4010;
    case 9: case 10: case 11: return 0x400e;
    case 12: case 13: case 14: case 15: case 16: return 0x400f;
    default: return 0;
    }
}

/* ============================================================================================== */
/* tables                                                                                         */

#include "anna_norm_tab.h"

static const char *const ONES[10] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
static const char *const TENS[10] = {"zero", "ten", "twenty", "thirty", "forty", "fifty", "sixty", "seventy",
                                     "eighty", "ninety"};
static const char *const TEENS[10] = {"ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
                                      "seventeen", "eighteen", "nineteen"};
static const char *const ORD_ONES[10] = {"zeroth", "first", "second", "third", "fourth", "fifth", "sixth",
                                         "seventh", "eighth", "ninth"};
static const char *const ORD_TENS[10] = {"", "tenth", "twentieth", "thirtieth", "fortieth", "fiftieth",
                                         "sixtieth", "seventieth", "eightieth", "ninetieth"};
static const char *const ORD_TEENS[10] = {"tenth", "eleventh", "twelfth", "thirteenth", "fourteenth", "fifteenth",
                                          "sixteenth", "seventeenth", "eighteenth", "nineteenth"};
static const char *const SCALE[6] = {"hundred", "thousand", "million", "billion", "trillion", "quadrillion"};
static const char *const ORD_SCALE[6] = {"hundredth", "thousandth", "millionth", "billionth", "trillionth",
                                         "quadrillionth"};
static const char *const DENOM[10] = {"", "", "halves", "thirds", "fourths", "fifths", "sixths", "sevenths",
                                      "eighths", "ninths"};
static const char *const MONTHS[13] = {"", "January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December"};
static const char *const WEEKDAYS[8] = {"", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
                                        "Sunday"};

const anna_abbrev *anna_norm_abbrev(int index)
{
    return index >= 0 && index < (int)(sizeof ABBREVS / sizeof *ABBREVS) ? &ABBREVS[index] : NULL;
}

/* ============================================================================================== */
/* per-sentence arena, word lists, items                                                          */

static void *amalloc(anna_norm *nm, size_t size)
{
    ablk *b = nm->arena;
    void *p;
    size = (size + 7) & ~(size_t)7;
    if (!b || b->used + size > b->cap) {
        size_t cap = size > 65536 ? size : 65536;
        ablk *nb = malloc(sizeof(ablk) + cap);
        if (!nb) return NULL;
        nb->next = b;
        nb->used = 0;
        nb->cap = cap;
        nm->arena = nb;
        b = nb;
    }
    p = (unsigned char *)b->data + b->used;
    b->used += size;
    memset(p, 0, size);
    return p;
}

static void arena_reset(anna_norm *nm)
{
    while (nm->arena) {
        ablk *n = nm->arena->next;
        free(nm->arena);
        nm->arena = n;
    }
}

static const anna_vstate *cur_st(const anna_norm *nm) { return nm->fi >= 0 ? &nm->fr[nm->fi].st : &DEFAULT_STATE; }
static const anna_vstate *frag_st(const anna_norm *nm, int fi) { return fi >= 0 ? &nm->fr[fi].st : &DEFAULT_STATE; }

static anna_nword *wl_new(anna_norm *nm, anna_wlist *l)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        anna_nword *w = amalloc(nm, sizeof(anna_nword) * (size_t)cap);
        if (!w) return NULL;
        if (l->n) memcpy(w, l->w, sizeof(anna_nword) * (size_t)l->n);
        l->w = w;
        l->cap = cap;
    }
    memset(&l->w[l->n], 0, sizeof(anna_nword));
    return &l->w[l->n++];
}

static void wl_span_st(anna_norm *nm, anna_wlist *l, const wc *t, int len, const anna_vstate *st)
{
    anna_nword *w = wl_new(nm, l);
    if (w) {
        w->t = t;
        w->len = len;
        w->st = st;
    }
}
static void wl_span(anna_norm *nm, anna_wlist *l, const wc *t, int len) { wl_span_st(nm, l, t, len, cur_st(nm)); }

static void wl_str_st(anna_norm *nm, anna_wlist *l, const char *s, const anna_vstate *st)
{
    anna_nword *w = wl_new(nm, l);
    if (w) {
        w->s = s;
        w->len = (int)strlen(s);
        w->st = st;
    }
}
static void wl_str(anna_norm *nm, anna_wlist *l, const char *s) { wl_str_st(nm, l, s, cur_st(nm)); }

/* a table string of several words ("number sign" -> number, sign) */
static void wl_words_st(anna_norm *nm, anna_wlist *l, const char *s, const anna_vstate *st)
{
    while (s && *s) {
        const char *sp = strchr(s, ' ');
        size_t n = sp ? (size_t)(sp - s) : strlen(s);
        char *c = amalloc(nm, n + 1);
        if (!c) return;
        memcpy(c, s, n);
        wl_str_st(nm, l, c, st);
        if (sp && l->n) l->w[l->n - 1].midword = 1;
        s = sp ? sp + 1 : NULL;
    }
}
static void wl_words(anna_norm *nm, anna_wlist *l, const char *s) { wl_words_st(nm, l, s, cur_st(nm)); }

static void wl_cat(anna_norm *nm, anna_wlist *dst, const anna_wlist *src) /* 0x0D5F0271 */
{
    int i;
    for (i = 0; i < src->n; i++) {
        anna_nword *w = wl_new(nm, dst);
        if (w) *w = src->w[i];
    }
}

/* 0x0D5F02CD: move a number's words out (they can be used only once) */
static void wl_take(anna_norm *nm, anna_wlist *dst, anna_wlist *src)
{
    wl_cat(nm, dst, src);
    src->n = 0;
    src->w = NULL;
    src->cap = 0;
}

static void wl_setst(anna_wlist *l, int from, const anna_vstate *st)
{
    int i;
    for (i = from; i < l->n; i++) l->w[i].st = st;
}

/* special items met while looking ahead (bookmarks, silences between the tokens): their word records are
 * appended to the handler's word list */
static void wl_items(anna_norm *nm, anna_wlist *l, ilist *il)
{
    aitem *it;
    for (it = il->head; it; it = it->next)
        if (it->words.n) {
            anna_nword *w = wl_new(nm, l);
            if (w) *w = it->words.w[0];
        }
    il->head = il->tail = NULL;
}

static aitem *item_new(anna_norm *nm)
{
    return amalloc(nm, sizeof(aitem));
}

static void il_append(ilist *l, aitem *it)
{
    it->next = NULL;
    it->prev = l->tail;
    if (l->tail) l->tail->next = it;
    else l->head = it;
    l->tail = it;
}

static void il_insert_before(ilist *l, aitem *at, aitem *it)
{
    it->next = at;
    it->prev = at->prev;
    if (at->prev) at->prev->next = it;
    else l->head = it;
    at->prev = it;
}

static void il_insert_after(ilist *l, aitem *at, aitem *it)
{
    it->prev = at;
    it->next = at->next;
    if (at->next) at->next->prev = it;
    else l->tail = it;
    at->next = it;
}

static void il_remove(ilist *l, aitem *it)
{
    if (it->prev) it->prev->next = it->next;
    else l->head = it->next;
    if (it->next) it->next->prev = it->prev;
    else l->tail = it->prev;
    it->next = it->prev = NULL;
}

static int *type_rec(anna_norm *nm, int type)
{
    int *t = amalloc(nm, sizeof(int) * 2);
    if (t) *t = type;
    return t;
}

static int src_ofs_fi(const anna_norm *nm, int fi, const wc *p)
{
    const afrag *f = fi >= 0 ? &nm->fr[fi] : NULL;
    return f ? (int)(p - f->t) + f->ofs : (int)(p - nm->buf);
}

/* source offset of a text pointer in the current fragment */
static int src_ofs(const anna_norm *nm, const wc *p)
{
    const afrag *f = nm->fi >= 0 ? &nm->fr[nm->fi] : NULL;
    return f ? (int)(p - f->t) + f->ofs : (int)(p - nm->buf);
}

/* item for a fragment that is not spoken text (silence, bookmark, pronounce...) */
static aitem *special_item(anna_norm *nm, int fi)
{
    aitem *it = item_new(nm);
    if (!it) return NULL;
    it->t = nm->fr[fi].t;
    it->len = nm->fr[fi].len;
    it->ofs = nm->fr[fi].ofs;
    wl_span_st(nm, &it->words, NULL, 0, &nm->fr[fi].st);
    it->ti = type_rec(nm, 0x1000);
    if (!it->ti) return NULL;
    return it;
}

/* the next fragment of the same engine call, or -1 */
static int next_frag(const anna_norm *nm, int fi)
{
    return fi >= 0 && fi + 1 < nm->nfr && nm->fr[fi + 1].group == nm->fr[fi].group ? fi + 1 : -1;
}

/* 0x0D5F4531: skip whitespace, moving to the next fragment at the end of one; fragments that are not
 * text to speak (action other than 0 and 4) become special items appended to emit (if not NULL).
 * Sets *pp = NULL at the end of the text. */
static int adv(anna_norm *nm, wc **pp, wc **pfe, int *pfi, ilist *emit)
{
    for (;;) {
        if (!*pp || (!is_ws(**pp) && *pp != *pfe)) return 0;
        while (*pp < *pfe && is_ws(**pp)) (*pp)++;
        if (*pp == *pfe) {
            int fi = next_frag(nm, *pfi);
            *pfi = fi;
            while (fi >= 0) {
                afrag *f = &nm->fr[fi];
                if (f->st.action == 0 || f->st.action == 4) break;
                *pp = f->t;
                *pfe = f->t + f->len;
                if (emit) {
                    aitem *it = special_item(nm, fi);
                    if (!it) return NOMEM;
                    il_append(emit, it);
                }
                fi = next_frag(nm, fi);
                *pfi = fi;
            }
            if (fi >= 0) {
                *pp = nm->fr[fi].t;
                *pfe = nm->fr[fi].t + nm->fr[fi].len;
            } else {
                *pp = NULL;
                *pfe = NULL;
            }
        }
    }
}

/* 0x0D5F3FA9: end of the whitespace token (at most 127 characters) */
static wc *token_end(wc *p, const wc *fe)
{
    int k = 1;
    for (;;) {
        if (!p || p >= fe) return p;
        if (is_ws(*p)) return p;
        if (k > 0x7f) return p;
        p++;
        k++;
    }
}

/* the usual trailing-punctuation strip of a lookahead token (clause, then close/quote/sentence marks) */
static wc *strip_trailing(wc *end)
{
    wc *q = end;
    for (;;) {
        do {
            end = q;
            q--;
        } while (clause_type(*q) != 0x1001);
        if (close_type(*q) == 0x1001 && quote_type(*q) == 0x1001 && sent_type(*q) == 0x1001) break;
    }
    return end;
}

/* strip of all punctuation that reports whether anything went (currency) */
static wc *strip_flag(wc *end, int *stripped)
{
    wc *q = end;
    for (;;) {
        wc *e = q;
        q--;
        if (!is_punct_any(*q)) return e;
        if (stripped) *stripped = 1;
    }
}

/* ============================================================================================== */
/* numbers (0x0D5F0F10 and helpers)                                                               */

static int parse_number(anna_norm *nm, int **out, const char *mode, int lookahead);

static int w_digit(anna_norm *nm, wc c, int *f, anna_wlist *l) /* 0x0D5EFD6F */
{
    if (!a_isdigit(c)) return DECLINE;
    wl_str(nm, l, ONES[c - '0']);
    f[0] = 1;
    return 0;
}

static int w_two(anna_norm *nm, const wc *s, int *f, anna_wlist *l) /* 0x0D5EFDF8 */
{
    int d1 = s[0] - '0', d2 = s[1] - '0';
    if (!a_isdigit(s[0]) || !a_isdigit(s[1])) return DECLINE;
    if (d1 == 1) {
        wl_str(nm, l, TEENS[d2]);
        f[0] = 1;
        return 0;
    }
    if (d1 != 0) {
        wl_str(nm, l, TENS[d1]);
        f[1] = 1;
    }
    if (d2 != 0 && w_digit(nm, s[1], f, l) >= 0) f[0] = 1;
    return 0;
}

static int w_three(anna_norm *nm, const wc *s, int *f, anna_wlist *l) /* 0x0D5EFEFC */
{
    if (s[0] != '0') {
        if (w_digit(nm, s[0], f, l) < 0) return DECLINE;
        wl_str(nm, l, SCALE[0]);
        f[0] = 0;
        f[2] = 1;
    }
    return w_two(nm, s + 1, f, l);
}

static int w_ord1(anna_norm *nm, wc c, int *f, anna_wlist *l) /* 0x0D5EFF9F */
{
    if (!a_isdigit(c)) return DECLINE;
    wl_str(nm, l, ORD_ONES[c - '0']);
    f[0] = 1;
    return 0;
}

static int w_ord2(anna_norm *nm, const wc *s, int *f, anna_wlist *l) /* 0x0D5F0028 */
{
    int d1 = s[0] - '0', d2 = s[1] - '0';
    if (!a_isdigit(s[0]) || !a_isdigit(s[1])) return DECLINE;
    if (d1 == 1) {
        wl_str(nm, l, ORD_TEENS[d2]);
        f[0] = 1;
    } else if (d1 == 0) {
        return w_ord1(nm, s[1], f, l);
    } else if (d2 == 0) {
        wl_str(nm, l, ORD_TENS[d1]);
    } else {
        wl_str(nm, l, TENS[d1]);
        f[1] = 1;
        w_ord1(nm, s[1], f, l);
        f[0] = 1;
    }
    return 0;
}

static int rest_zero(const wc *s) /* 0x0D5EF869: only zeros (and commas) up to the first other char / NUL */
{
    int i, n = wlen(s);
    for (i = 0; i < n; i++) {
        if (s[i] != '0' && a_isdigit(s[i])) return 0;
        if (!a_isdigit(s[i]) && s[i] != ',') return 1;
    }
    return 1;
}

static int group_zero(const wc *s) /* 0x0D5EF8DD */
{
    int i = 0;
    while (s[i] == '0' || !a_isdigit(s[i])) {
        i++;
        if (i > 2) return 1;
    }
    return 0;
}

static int w_ord3(anna_norm *nm, const wc *s, int *f, anna_wlist *l) /* 0x0D5F016B */
{
    if (s[0] == '0') return w_ord2(nm, s + 1, f, l);
    if (w_digit(nm, s[0], f, l) < 0) return DECLINE;
    if (!rest_zero(s + 1)) {
        wl_str(nm, l, SCALE[0]);
        w_ord2(nm, s + 1, f, l);
        f[2] = 1;
    } else {
        wl_str(nm, l, ORD_SCALE[0]);
        f[0] = 0;
        f[2] = 1;
    }
    return 0;
}

static int mode_is(const char *mode, const char *what)
{
    size_t i;
    if (!mode) return 0;
    for (i = 0;; i++) {
        int x = a_toupper((unsigned char)mode[i]), y = a_toupper((unsigned char)what[i]);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static int mode_prefix(const char *mode, const char *what) /* _wcsnicmp(mode, what, strlen(what)) == 0 */
{
    size_t i;
    for (i = 0; what[i]; i++)
        if (a_toupper((unsigned char)mode[i]) != a_toupper((unsigned char)what[i])) return 0;
    return 1;
}

/* 0x0D5F05A8: words of an integer part */
static void int_words(anna_norm *nm, anna_intpart *ip, const char *mode, anna_wlist *l)
{
    const wc *s = ip->start;
    int len = (int)(ip->end - s), keep = ip->v[0] + 1, done = 0, off = 0, i;
    int *v = ip->v;
    if (s[0] == '0' || mode_is(mode, "NUMBER_DIGIT") || v[0] > 5) {
        v[0x1a] = 1;
        v[0x1b] = 0;
        for (i = 0; i < len; i++) {
            if (a_isdigit(s[i])) {
                w_digit(nm, s[i], v + 1, l);
                v[0x1b]++;
            }
        }
        v[0] = keep;
        return;
    }
    if (v[0] == 0) {
        int f = v[0x1c];
        if (!v[0x19]) {
            if (f == 0) w_three(nm, s, v + 1, l);
            else if (f == 1) w_digit(nm, s[0], v + 1, l);
            else if (f == 2) w_two(nm, s, v + 1, l);
        } else {
            if (f == 0) w_ord3(nm, s, v + 1, l);
            else if (f == 1) w_ord1(nm, s[0], v + 1, l);
            else if (f == 2) w_ord2(nm, s, v + 1, l);
        }
        v[0] = keep;
        return;
    }
    {
        int g = v[0], f = v[0x1c];
        if (f == 0) {
            w_three(nm, s, v + g * 4 + 1, l);
            off = 3;
        } else if (f == 1) {
            w_digit(nm, s[0], v + g * 4 + 1, l);
            off = 1;
        } else if (f == 2) {
            w_two(nm, s, v + g * 4 + 1, l);
            off = 2;
        }
        v[(g + 1) * 4] = 1;
        if (!v[0x19] || !rest_zero(s + off)) {
            wl_str(nm, l, SCALE[g]);
        } else {
            wl_str(nm, l, ORD_SCALE[g]);
            done = 1;
        }
        v[0] = g - 1;
        while (v[0] > 0 && !done) {
            g = v[0];
            if (v[0x1d]) off++;
            w_three(nm, s + off, v + g * 4 + 1, l);
            off += 3;
            if (!v[0x19] || !rest_zero(s + off)) {
                if (!group_zero(s + off - 3)) {
                    v[(g + 1) * 4] = 1;
                    wl_str(nm, l, SCALE[g]);
                    v[0] = g - 1;
                } else {
                    v[0] = g - 1;
                }
            } else {
                v[(g + 1) * 4] = 1;
                wl_str(nm, l, ORD_SCALE[g]);
                v[0] = g - 1;
                done = 1;
            }
        }
        if (v[0x1d] && !done) off++;
        if (!done) {
            if (!v[0x19]) w_three(nm, s + off, v + v[0] * 4 + 1, l);
            else w_ord3(nm, s + off, v + v[0] * 4 + 1, l);
        }
    }
    v[0] = keep;
}

static int int_stop(wc c)
{
    int u = a_toupper(c);
    return c == '.' || c == '%' || c == 0xb0 || c == 0xb2 || c == 0xb3 || c == '-' || c == 0xbc || c == 0xbd ||
           c == 0xbe || u == 'S' || u == 'N' || u == 'R' || u == 'T';
}

/* 0x0D5EF3D0: integer part (thousands separator ',', decimal '.': this+0x70 == 2) */
static int int_parse(anna_norm *nm, const wc *s, anna_intpart **out)
{
    int n = (int)(nm->e - s), comma = 0, stop = 0, count, i;
    anna_intpart *ip;
    if (!a_isdigit(s[0])) return DECLINE;
    count = 1;
    i = 1;
    while (i < 4 && i < n) {
        wc c = s[i];
        if (c == ',') {
            comma = 1;
            break;
        }
        if (!a_isdigit(c) && int_stop(c)) {
            stop = 1;
            break;
        }
        if (!a_isdigit(c)) return DECLINE;
        count++;
        i++;
    }
    if (!stop && i < n) {
        if (!comma) {
            while (a_isdigit(s[i]) && i < n) {
                count++;
                i++;
            }
        } else {
            int u = i;
            for (;;) {
                int k;
                i = u;
                if (s[u] != ',' || n <= u + 3) break;
                for (k = u + 1; k < u + 4; k++) {
                    if (!a_isdigit(s[k])) return DECLINE;
                    count++;
                }
                u += 4;
            }
        }
        if (i != n && !int_stop(s[i])) return DECLINE;
    }
    ip = amalloc(nm, sizeof *ip);
    if (!ip) return NOMEM;
    ip->v[0x1d] = comma;
    ip->v[0x1c] = count % 3;
    ip->v[0] = (count - 1) / 3;
    ip->start = s;
    ip->end = s + i;
    *out = ip;
    return 0;
}

static int dec_parse(anna_norm *nm, const wc *s, anna_decpart **out) /* 0x0D5EF6B9 */
{
    const wc *q = s;
    int c = 0;
    while (q < nm->e && a_isdigit(*q)) {
        c++;
        q++;
    }
    if (!c) return DECLINE;
    *out = amalloc(nm, sizeof(anna_decpart));
    if (!*out) return NOMEM;
    (*out)->s = s;
    (*out)->n = c;
    return 0;
}

static int is_frac_reject(int t) { return t == 0x100f || t == 0x100e || t == 0x1007; }

static int frac_parse(anna_norm *nm, wc *s, anna_fracpart **out) /* 0x0D5EF9C5 */
{
    wc *e0 = nm->e, *save = nm->p, *slash;
    anna_numinfo *num = NULL, *den = NULL;
    int r;
    if (e0 - s == 0) return DECLINE;
    if (*s == 0xbc || *s == 0xbd || *s == 0xbe) {
        anna_fracpart *f = amalloc(nm, sizeof *f);
        if (!f) return NOMEM;
        f->vulgar = s;
        f->num = amalloc(nm, sizeof(anna_numinfo));
        f->den = amalloc(nm, sizeof(anna_numinfo));
        if (!f->num || !f->den) return NOMEM;
        f->num->ip = amalloc(nm, sizeof(anna_intpart));
        f->den->ip = amalloc(nm, sizeof(anna_intpart));
        if (!f->num->ip || !f->den->ip) return NOMEM;
        f->num->ip->v[0x1c] = 1;
        f->num->ip->v[0] = 1;
        f->num->ip->v[1] = 1;
        f->den->ip->v[0x1c] = 1;
        f->den->ip->v[0] = 1;
        f->den->ip->v[1] = 1;
        *out = f;
        return 0;
    }
    nm->p = s;
    slash = (wc *)wchr(s, '/');
    nm->e = slash;
    if (!slash || slash >= e0) {
        r = DECLINE;
    } else {
        r = parse_number(nm, (int **)&num, "NUMBER", 0);
        if (r >= 0) {
            if (is_frac_reject(num->type)) {
                r = DECLINE;
            } else {
                if (num->ip) nm->p += num->ip->end - num->ip->start;
                if (num->dp) nm->p += num->dp->n + 1;
            }
        }
    }
    nm->e = e0;
    if (r >= 0) {
        if (*nm->p == '/') {
            nm->p++;
            r = parse_number(nm, (int **)&den, "NUMBER", 0);
            if (r >= 0) {
                if (is_frac_reject(den->type)) {
                    r = DECLINE;
                } else {
                    anna_fracpart *f = amalloc(nm, sizeof *f);
                    if (!f) return NOMEM;
                    f->num = num;
                    f->den = den;
                    *out = f;
                }
            }
        } else {
            r = DECLINE;
        }
    }
    nm->p = save;
    return r;
}

static int fr_len(const anna_fracpart *f) { return f->vulgar ? 1 : (int)(f->den->end - f->num->start); }

/* 0x0D5F0A1A: words of a fraction */
static void frac_words(anna_norm *nm, anna_fracpart *f, anna_wlist *l)
{
    anna_numinfo *num, *den;
    int over, dl, nl, num_one;
    const char *word = NULL;
    if (f->vulgar) {
        if (*f->vulgar == 0xbc) {
            wl_str(nm, l, "one");
            wl_str(nm, l, "fourth");
        } else if (*f->vulgar == 0xbd) {
            wl_str(nm, l, "one");
            wl_str(nm, l, "half");
        } else {
            wl_str(nm, l, "three");
            wl_str(nm, l, "fourths");
        }
        return;
    }
    wl_take(nm, l, &f->num->words);
    num = f->num;
    den = f->den;
    dl = (int)(den->end - den->start);
    nl = (int)(num->end - num->start);
    num_one = nl == 1 && num->start[0] == '1';
    over = 1;
    if (den->dp || num->dp || den->neg) {
        over = 1;
    } else if (dl == 1 && den->start[0] != '1' && den->start[0] != '0') {
        over = 0;
        if (num_one) {
            if (den->start[0] != '2') {
                w_ord1(nm, den->start[0], den->ip->v + 1, l);
                goto done;
            }
            word = "half";
        } else {
            word = DENOM[den->start[0] - '0'];
        }
    } else if (dl == 2 && !wncmp_a(den->start, "10", 2)) {
        over = 0;
        word = "tenths";
        if (num_one) {
            w_ord2(nm, den->start, den->ip->v + 1, l);
            goto done;
        }
    } else if (dl == 2 && !wncmp_a(den->start, "16", 2)) {
        over = 0;
        word = "sixteenths";
        if (num_one) {
            w_ord2(nm, den->start, den->ip->v + 1, l);
            goto done;
        }
    } else if (dl == 3 && !wncmp_a(den->start, "100", 3)) {
        over = 0;
        word = "hundredths";
        if (num_one) {
            w_ord3(nm, den->start, den->ip->v + 1, l);
            goto done;
        }
    }
    if (!over) wl_str(nm, l, word);
done:
    f->over = over;
    if (over) {
        wl_str(nm, l, "over");
        wl_take(nm, l, &den->words);
    }
}

/* 0x0D5F0F10 */
static int parse_number(anna_norm *nm, int **out, const char *mode, int lookahead)
{
    wc *s = nm->p, *save_e = nm->e, *save_fe = nm->fe;
    int save_fi = nm->fi, n = (int)(nm->e - s), off, neg, r = 0;
    anna_intpart *ip = NULL;
    anna_decpart *dp = NULL;
    anna_fracpart *fr = NULL;
    ilist sp = {NULL, NULL};
    const anna_vstate *st = cur_st(nm);
    if (n == 0) return DECLINE;
    neg = s[0] == '-';
    off = neg;
    r = int_parse(nm, s + off, &ip);
    if (r < 0) {
        if (r != DECLINE) return r;
        r = 0;
        ip = NULL;
    } else {
        off += (int)(ip->end - ip->start);
    }
    if (off < n && s[off] == '.') {
        r = dec_parse(nm, s + off + 1, &dp);
        if (r < 0) {
            if (r == DECLINE) {
                r = 0;
                dp = NULL;
            }
            goto check;
        }
        off += 1 + dp->n;
        if (off < n) {
            if (s[off] != '/') goto check;
            ip = NULL;
            dp = NULL;
            off = neg;
            goto fraction;
        }
        goto check;
    }
    if (ip == NULL) {
        if (off < n) {
            if (s[off] == '-') off++;
            goto fraction;
        }
        goto look;
    }
    if (off >= n) goto look;
    if (!w_isalpha(s[off])) {
        if (s[off] == '-') off++;
        goto fraction;
    }
    {
        int u = a_toupper(s[off]), ord = 0;
        if (u == 'N') {
            if (off + 2 == n && a_toupper(s[off + 1]) == 'D' && s[off - 1] == '2') ord = off == 1 || s[off - 2] != '1';
        } else if (u == 'R') {
            if (off + 2 == n && a_toupper(s[off + 1]) == 'D' && s[off - 1] == '3') ord = off == 1 || s[off - 2] != '1';
        } else if (u == 'S') {
            if (a_toupper(s[off + 1]) == 'T' && s[off - 1] == '1' && off + 2 == n) ord = off == 1 || s[off - 2] != '1';
        } else if (u == 'T') {
            if (off + 2 == n && a_toupper(s[off + 1]) == 'H') {
                wc d = s[off - 1];
                ord = (d < 0x3a && d > 0x33) || d == '0' || off == 1 || s[off - 2] == '1';
            }
        }
        if (ord) {
            off += 2;
            ip->v[0x19] = 1;
        }
    }
    goto check;
fraction:
    r = frac_parse(nm, s + off, &fr);
    if (r >= 0) off += fr_len(fr);
    else if (r == DECLINE) r = 0;
    goto check;
look:
    if (lookahead) { /* a fraction in the next token: "1 1/2" */
        int rr;
        nm->p = nm->e;
        rr = adv(nm, &nm->p, &nm->fe, &nm->fi, &sp);
        if (rr < 0) return rr;
        if (!nm->p) {
            nm->p = s;
            nm->fe = save_fe;
            nm->fi = save_fi;
        } else {
            nm->e = strip_trailing(token_end(nm->p, nm->fe));
            r = frac_parse(nm, nm->p, &fr);
            if (r < 0) {
                nm->p = s;
                nm->fe = save_fe;
                nm->e = save_e;
                nm->fi = save_fi;
                if (r == DECLINE) r = 0;
                fr = NULL;
            } else {
                n = (int)(nm->e - nm->p);
                off = fr_len(fr);
            }
        }
    }
check:
    if (r >= 0 && off != n) {
        const wc *b = nm->p;
        if (!(n == off + 1 && (b[off] == '%' || b[off] == 0xb0 || b[off] == 0xb2 || b[off] == 0xb3))) {
            nm->p = s;
            nm->e = save_e;
            nm->fe = save_fe;
            nm->fi = save_fi;
            r = DECLINE;
        }
    }
    if (r < 0 || (!ip && !dp && !fr)) return r < 0 && r != DECLINE ? r : DECLINE;
    nm->p = s;
    if (!(ip == NULL || ip->end - ip->start != 4 || ip->start[0] == '0' || ip->v[0x1d] || ip->v[0x19] || dp || fr ||
          neg || off != n || (mode && mode_prefix(mode, "NUMBER")))) {
        anna_yearinfo *y = amalloc(nm, sizeof *y);
        if (!y) return NOMEM;
        y->type = 0x1014;
        y->s = s;
        y->len = 4;
        *out = (int *)y;
        return 0;
    }
    {
        anna_numinfo *ni = amalloc(nm, sizeof *ni);
        if (!ni) return NOMEM;
        if (dp) {
            ni->type = 0x1008;
            ni->end = ip ? ip->end + 1 + dp->n : s + 1 + dp->n + (neg ? 1 : 0);
        } else if (fr) {
            ni->end = fr->vulgar ? fr->vulgar + 1 : fr->den->end;
            ni->type = ip ? 0x100f : 0x100e;
        } else {
            ni->type = ip->v[0x19] ? 0x1007 : 0x1006;
            ni->end = ip->end + (ip->v[0x19] ? 2 : 0);
        }
        ni->neg = neg;
        ni->ip = ip;
        ni->dp = dp;
        ni->fr = fr;
        ni->start = s;
        if (neg) wl_str_st(nm, &ni->words, "negative", st);
        if (ip) int_words(nm, ip, mode, &ni->words);
        if (dp) {
            int k;
            wl_str(nm, &ni->words, "point");
            for (k = 0; k < dp->n; k++) {
                int dummy[4] = {0};
                w_digit(nm, dp->s[k], dummy, &ni->words);
            }
        }
        if (fr) {
            wl_items(nm, &ni->words, &sp);
            if (ip) wl_str(nm, &ni->words, "and");
            frac_words(nm, fr, &ni->words);
        }
        *out = (int *)ni;
    }
    return 0;
}

/* 0x0D5E17D4: a number read as a year ("nineteen eighty four") */
static void year_words(anna_norm *nm, const wc *s, int len, anna_wlist *l)
{
    int f[4] = {0};
    if (len == 2) {
        if (s[0] != '0') {
            w_two(nm, s, f, l);
            return;
        }
        if (s[1] == '0') {
            wl_str(nm, l, "two");
            wl_str(nm, l, "thousand");
            return;
        }
        wl_str(nm, l, "o");
        w_digit(nm, s[1], f, l);
        return;
    }
    if (len == 3) {
        w_three(nm, s, f, l);
        return;
    }
    if (len != 4) return;
    if (s[1] == '0' && s[2] == '0' && s[0] != '0') {
        w_digit(nm, s[0], f, l);
        wl_str(nm, l, "thousand");
        if (s[3] == '0') return;
        w_digit(nm, s[3], f, l);
        return;
    }
    w_two(nm, s, f, l);
    if (s[2] != '0') {
        w_two(nm, s + 2, f, l);
        return;
    }
    if (s[3] == '0') {
        wl_str(nm, l, "hundred");
        return;
    }
    if (s[0] != '0' || s[1] != '0') wl_str(nm, l, "o");
    w_digit(nm, s[3], f, l);
}

/* ============================================================================================== */
/* abbreviations, initials and sentence ends                                                      */

/* 0x0D5EB3EE: bsearch compare of a NUL-terminated key against {word, len} (case-insensitive, equal only with
 * equal length) */
static int starter_cmp(const wc *key, int klen, const char *w)
{
    int wl, c;
    if (!w) w = ""; /* the last state entry is {NULL, 0} */
    wl = (int)strlen(w);
    if (klen < wl) {
        c = wnicmp_a(key, w, klen);
        return c ? c : -1;
    }
    if (wl < klen) {
        c = wnicmp_a(key, w, wl);
        return c ? c : 1;
    }
    return wnicmp_a(key, w, klen);
}

static int is_starter(const wc *key, int klen)
{
    int lo = 0, hi = (int)(sizeof STARTERS / sizeof *STARTERS) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = starter_cmp(key, klen, STARTERS[mid]);
        if (!c) return 1;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return 0;
}

/* bsearch with _wcsicmp over the abbreviation table (key = span) */
static int abbrev_find(const wc *s, int len)
{
    int lo = 0, hi = (int)(sizeof ABBREVS / sizeof *ABBREVS) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c, i;
        const char *w = ABBREVS[mid].word;
        for (i = 0;; i++) {
            int x = i < len ? a_tolower(s[i]) : 0, y = a_tolower((unsigned char)w[i]);
            c = x - y;
            if (c || !x) break;
        }
        if (!c) return mid;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return -1;
}

/* the next whitespace token starts a sentence: end of the text, or a capitalized sentence-starter word.
 * Returns 1 = yes. cut_apos: the handler cuts the next token at its first apostrophe (0x0D5DE1DB). */
static int next_starts_sentence(anna_norm *nm, int cut_apos, int *no_rule)
{
    wc *q = nm->te, *qfe = nm->fe, *qe;
    int qfi = nm->fi;
    if (adv(nm, &q, &qfe, &qfi, NULL) < 0) return 0;
    if (!q) return 1;
    if (!(*q >= 'A' && *q <= 'Z')) return 0;
    qe = token_end(q, qfe);
    if (cut_apos) {
        wc *a;
        for (a = q; a < qe && *a != '\''; a++) {
        }
        qe = a;
    }
    if (is_starter(q, (int)(qe - q))) return 1;
    if (no_rule && !wncmp_a(nm->p, "No", 2) && !a_isdigit(*q)) return 1; /* 0x0D5DE355 */
    return 0;
}

static int abbrev_is_date(int a) /* 0x0D5DDCF0 */
{
    static const char *const D[] = {"jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept", "oct", "nov",
                                    "dec", "mon", "tue", "tues", "wed", "thu", "thur", "thurs", "fri", "sat", "sun"};
    size_t k;
    for (k = 0; k < sizeof D / sizeof *D; k++)
        if (!strcmp(ABBREVS[a].word, D[k])) return 1;
    return 0;
}

/* the core item becomes an abbreviation (its text keeps the period) */
static int make_abbrev(anna_norm *nm, aitem *core, int a)
{
    anna_abbrevinfo *ti = amalloc(nm, sizeof *ti);
    if (!ti) return NOMEM;
    ti->type = abbrev_is_date(a) ? 0x1004 : 0x1003;
    ti->index = a;
    core->t = nm->p;
    core->len = (int)(nm->e - nm->p);
    core->ofs = src_ofs(nm, nm->p);
    core->words.n = 0;
    wl_span(nm, &core->words, nm->p, core->len);
    core->pos = 0;
    core->ti = (int *)ti;
    return 0;
}

/* sentence-end handlers of the abbreviation table (0x0D60C48C) */
static int abbrev_sentence(anna_norm *nm, int h, int a, aitem *core, int *sent_end)
{
    switch (h) {
    case 0: /* 0x0D5DE1DB: always an abbreviation; the next token is cut at its apostrophe */
        if (!*sent_end && next_starts_sentence(nm, 1, NULL)) *sent_end = 1;
        return make_abbrev(nm, core, a);
    case 1: /* 0x0D5DE355: an abbreviation only when the sentence goes on ("No." before a capital ends it) */
        if (*sent_end) return DECLINE;
        if (next_starts_sentence(nm, 0, &h)) *sent_end = 1;
        if (*sent_end) return DECLINE;
        return make_abbrev(nm, core, a);
    case 2: /* 0x0D5DE6CB: at a sentence end only when capitalized */
        if (!*sent_end) {
            if (next_starts_sentence(nm, 0, NULL)) *sent_end = 1;
            if (!*sent_end) return make_abbrev(nm, core, a);
        }
        if (!w_isupper(*nm->p)) return DECLINE;
        return make_abbrev(nm, core, a);
    case 3: { /* 0x0D5DE502: not at a sentence end, and only as "Xxx." (capital then lower case) */
        int n, i;
        if (*sent_end) return DECLINE;
        if (next_starts_sentence(nm, 0, NULL)) *sent_end = 1;
        if (*sent_end) return DECLINE;
        n = (int)(nm->e - nm->p);
        i = 0;
        if (n != 1 && n - 1 >= 0) {
            while (i < n - 1) {
                if (i == 0 ? !w_isupper(nm->p[0]) : !w_islower(nm->p[i])) break;
                i++;
            }
        }
        if (i != n - 1) return DECLINE;
        return make_abbrev(nm, core, a);
    }
    default:
        return DECLINE;
    }
}

/* 0x0D5DE85A: initials "U.S.A." (letters get a SPVSTATE copy with ePartOfSpeech = noun) */
static int h_initials(anna_norm *nm, aitem *core, int *sent_end)
{
    const wc *q = nm->p;
    int count = 0, k;
    anna_vstate *st;
    if ((nm->e - nm->p) < 4) return DECLINE;
    for (;;) {
        if (nm->e - 2 < q) break;
        if (!w_isalpha(*q) || q[1] != '.') return DECLINE;
        q += 2;
        count++;
    }
    if (!*sent_end && next_starts_sentence(nm, 0, NULL)) *sent_end = 1;
    st = amalloc(nm, sizeof *st);
    if (!st) return NOMEM;
    *st = *cur_st(nm);
    st->pos = 0x1000;
    core->t = nm->p;
    core->len = (int)(nm->e - nm->p);
    core->ofs = src_ofs(nm, nm->p);
    core->words.n = 0;
    for (k = 0; k < count; k++) wl_span_st(nm, &core->words, nm->p + 2 * k, 1, st);
    core->pos = 0;
    core->ti = type_rec(nm, 0x1005);
    return core->ti ? 0 : NOMEM;
}

/* ============================================================================================== */
/* token classes                                                                                  */

static int h_word(const wc *s, const wc *e) /* 0x0D5DEA58 */
{
    int apos = 0;
    for (; s && s < e; s++) {
        if (w_isalpha(*s)) continue;
        if (*s == '\'' && !apos) {
            apos = 1;
            continue;
        }
        return DECLINE;
    }
    return 0;
}

static int h_num(anna_norm *nm, int **out, const char *mode) /* 0x0D5F2A85 */
{
    wc *fe = nm->fe, *p = nm->p, *e = nm->e;
    int fi = nm->fi, r;
    anna_numinfo *ni;
    int *res = NULL;
    r = parse_number(nm, &res, mode, 1);
    if (r < 0) return r;
    ni = (anna_numinfo *)res;
    if (ni->type != 0x1014) {
        if (ni->end == nm->e - 1) {
            switch (*ni->end) {
            case '%': ni->type = 0x1009; *out = res; return r;
            case 0xb0: ni->type = 0x100a; *out = res; return r;
            case 0xb2: ni->type = 0x100b; *out = res; return r;
            case 0xb3: ni->type = 0x100c; *out = res; return r;
            default: ni->words.n = 0; return DECLINE;
            }
        }
        if (ni->end != nm->e) {
            ni->words.n = 0;
            nm->fe = fe;
            nm->e = e;
            nm->p = p;
            nm->fi = fi;
            return DECLINE;
        }
    }
    *out = res; /* only on success (0x0D5F2A85 keeps the result in a local) */
    return r;
}

static int h_hyphen(anna_norm *nm, wc *s, wc *e, int **out) /* 0x0D5EBFB7 */
{
    wc *save_p = nm->p, *save_e = nm->e, *h;
    int *left = NULL, *right = NULL, r;
    for (h = s; h < e; h++)
        if (*h == '-') break;
    if (!(*h == '-' && s < h && h < e - 1)) return DECLINE;
    r = h_word(s, h);
    if (r == 0) {
        left = type_rec(nm, 0x1002);
        if (!left) return NOMEM;
    } else {
        nm->p = s;
        nm->e = h;
        r = h_num(nm, &left, "NUMBER");
    }
    if (r >= 0) {
        r = h_word(h + 1, e);
        if (r == 0) {
            right = type_rec(nm, 0x1002);
            if (!right) return NOMEM;
        } else {
            nm->e = e;
            nm->p = h + 1;
            r = h_num(nm, &right, "NUMBER");
            if (r == DECLINE) {
                r = h_hyphen(nm, h + 1, e, &right);
                if (r == DECLINE && *left != 0x1002) ((anna_numinfo *)left)->words.n = 0;
            }
        }
    }
    nm->p = save_p;
    if (!right || (*right != 0x100f && *right != 0x101b)) nm->e = save_e;
    if (r < 0) return r;
    {
        anna_hypheninfo *hi = amalloc(nm, sizeof *hi);
        if (!hi) return NOMEM;
        hi->type = 0x101b;
        hi->left = left;
        hi->right = right;
        hi->ls = s;
        hi->rs = h + 1;
        *out = (int *)hi;
    }
    return 0;
}

static int h_dashword(anna_norm *nm, const wc *s, const wc *e, int **out) /* 0x0D5EB48E */
{
    const wc *q;
    anna_dashinfo *d;
    if (*s != '-') return DECLINE;
    for (q = s + 1; q < e && w_isalpha(*q); q++) {
    }
    if (q != e || q == s + 1) return DECLINE;
    d = amalloc(nm, sizeof *d);
    if (!d) return NOMEM;
    d->type = 0x1029;
    d->s = s + 1;
    d->len = (int)(e - s) - 1;
    *out = (int *)d;
    return 0;
}

/* 0x0D5E03DD: wcstoul of a leading number (0 and end = s when it does not start with a digit) */
static unsigned long wnum(const wc *s, wc **end)
{
    unsigned long v = 0;
    int over = 0;
    if (!a_isdigit(*s)) {
        if (end) *end = (wc *)s;
        return 0;
    }
    while (*s >= '0' && *s <= '9') {
        unsigned long d = (unsigned long)(*s - '0');
        if (v > (0xffffffffUL - d) / 10) over = 1;
        else v = v * 10 + d;
        s++;
    }
    if (end) *end = (wc *)s;
    return over ? 0xffffffffUL : v;
}

/* ---- currency (0x0D5F2BA9) ---- */

static int cur_symlen(int k) { return wlen(CURRENCY[k].sym); }

/* 0x0D5EF274: *kind = 0 the whole token is a symbol, 1 symbol prefix, 2 symbol suffix; -1 none */
static int cur_match(wc **p, wc **e, int *kind)
{
    int k;
    for (k = 0; k < 21; k++) {
        int l = cur_symlen(k);
        if (*e - *p == l && !wnicmp_w(*p, CURRENCY[k].sym, l)) {
            *p += l;
            *kind = 0;
            return k;
        }
    }
    for (k = 0; k < 21; k++) {
        int l = cur_symlen(k);
        if (l <= *e - *p && !wnicmp_w(*p, CURRENCY[k].sym, l)) {
            *p += l;
            *kind = 1;
            return k;
        }
    }
    for (k = 0; k < 21; k++) {
        int l = cur_symlen(k);
        if (l <= *e - *p && !wnicmp_w(*e - l, CURRENCY[k].sym, l)) {
            *e -= l;
            *kind = 2;
            return k;
        }
    }
    return -1;
}

static int scale_match(wc **p, const wc *e) /* 0x0D5EF217 */
{
    int k;
    for (k = 0; k < 6; k++) {
        int l = (int)strlen(SCALE[k]);
        if (l <= e - *p && !wnicmp_a(*p, SCALE[k], l)) {
            *p += l;
            return k;
        }
    }
    return -1;
}

static int cur_num_ok(int t) { return t == 0x1006 || t == 0x1008 || t == 0x100e || t == 0x100f; }

static int h_currency(anna_norm *nm, int **out, anna_wlist *l)
{
    wc *s0 = nm->p, *e0 = nm->e, *fe0 = nm->fe;
    int fi0 = nm->fi, neg, cur, kind = 3, stripped = 0, scale = -1, r = 0;
    anna_numinfo *num = NULL;
    ilist before = {NULL, NULL}, after = {NULL, NULL};
    const anna_vstate *st_num = NULL, *st_cur = NULL, *st_scale = NULL;
    neg = *nm->p == '-';
    if (neg) nm->p++;
    cur = cur_match(&nm->p, &nm->e, &kind);
    if (cur < 0 || (kind != 0 && kind != 1)) {
        /* number first, then a currency token: "5 USD" */
        r = h_num(nm, (int **)&num, "NUMBER");
        if (r < 0) goto fail;
        if (!cur_num_ok(num->type)) {
            r = DECLINE;
            goto fail;
        }
        st_num = cur_st(nm);
        nm->p = nm->e;
        r = adv(nm, &nm->p, &nm->fe, &nm->fi, &before);
        if (r < 0) goto fail;
        if (!nm->p) {
            r = DECLINE;
            goto fail;
        }
        nm->e = strip_flag(token_end(nm->p, nm->fe), &stripped);
        cur = cur_match(&nm->p, &nm->e, &kind);
        if (cur >= 0 && kind == 0) st_cur = cur_st(nm);
        if (!stripped) {
            if (cur >= 0 && kind == 0) {
                wc *sp = nm->p, *sfe = nm->fe, *se = nm->e;
                int sfi = nm->fi;
                r = adv(nm, &nm->p, &nm->fe, &nm->fi, &after);
                if (r < 0) goto fail;
                if (nm->p) {
                    nm->e = strip_trailing(token_end(nm->p, nm->fe));
                    scale = scale_match(&nm->p, nm->e);
                    if (scale >= 0) {
                        st_scale = cur_st(nm);
                        goto build;
                    }
                }
                nm->p = sp;
                nm->fe = sfe;
                nm->e = se;
                nm->fi = sfi; /* the special items met stay in the list (the engine does not clear it) */
                goto build;
            }
        } else if (cur >= 0 && kind == 0) {
            goto build;
        }
        r = DECLINE;
        goto fail;
    } else {
        /* currency symbol first: "$5", "$ 5", "$2 billion" */
        st_cur = cur_st(nm);
        r = adv(nm, &nm->p, &nm->fe, &nm->fi, &after);
        if (r < 0) goto fail;
        if (!nm->p) {
            r = DECLINE;
            goto fail;
        }
        nm->e = strip_flag(token_end(nm->p, nm->fe), &stripped);
        r = h_num(nm, (int **)&num, "NUMBER");
        if (r >= 0) {
            if (cur_num_ok(num->type)) st_num = cur_st(nm);
            else r = DECLINE;
        }
        if (!stripped) {
            wc *sp, *se, *sfe;
            int sfi;
            if (r < 0) goto fail;
            sp = nm->p;
            sfi = nm->fi;
            sfe = nm->fe;
            se = nm->e;
            nm->p = se;
            r = adv(nm, &nm->p, &nm->fe, &nm->fi, &before);
            if (r < 0) goto fail;
            if (nm->p) {
                nm->e = strip_trailing(token_end(nm->p, nm->fe));
                scale = scale_match(&nm->p, nm->e);
                if (scale >= 0) {
                    st_scale = cur_st(nm);
                    goto build;
                }
            }
            nm->p = sp;
            nm->fe = sfe;
            nm->e = se;
            nm->fi = sfi; /* the special items met stay in the list */
        }
        if (r < 0) goto fail;
    }
build : {
    anna_currinfo *ci = amalloc(nm, sizeof *ci);
    const wc *dot;
    int k;
    if (!ci) return NOMEM;
    ci->type = 0x100d;
    ci->scale = scale >= 0;
    ci->num = num;
    {
        aitem *it;
        for (it = before.head; it; it = it->next) ci->n_before++;
        for (it = after.head; it; it = it->next) ci->n_after++;
    }
    if (num->type == 0x1008 && scale < 0 && CURRENCY[cur].cents[0]) {
        for (dot = num->start; *dot && *dot != '.'; dot++) {
        }
        if (*dot == '.' && num->end - dot == 3) {
            wc *sp = nm->p, *se = nm->e, *numend = (wc *)num->end;
            anna_numinfo *dollars = NULL, *cents = NULL;
            num->words.n = 0;
            nm->p = (wc *)num->start;
            nm->e = (wc *)dot;
            if (nm->p == nm->e || (*nm->p == '-' && nm->p == nm->e - 1)) {
                dollars = amalloc(nm, sizeof *dollars);
                if (!dollars) return NOMEM;
                dollars->neg = *nm->p == '-';
                dollars->ip = amalloc(nm, sizeof(anna_intpart));
                if (!dollars->ip) return NOMEM;
                dollars->ip->v[0x1a] = 1;
                dollars->ip->v[0x1b] = 1;
                if (dollars->neg) wl_str_st(nm, &dollars->words, "negative", st_num);
                wl_str(nm, &dollars->words, "zero");
                r = 0;
            } else {
                r = parse_number(nm, (int **)&dollars, "NUMBER", 0);
            }
            if (r >= 0) {
                ci->num = dollars;
                nm->p = (wc *)dot + 1;
                nm->e = numend;
                if (*nm->p == '0') {
                    if (nm->p[1] == '0') goto nocents;
                    nm->p++;
                }
                r = parse_number(nm, (int **)&cents, "NUMBER", 0);
                if (r >= 0) ci->cents = cents;
            }
        nocents:
            nm->p = sp;
            nm->e = se;
        }
    }
    if (r < 0) return r;
    if (neg) {
        ci->num->neg = 1;
        wl_str_st(nm, l, "negative", st_num);
    }
    wl_take(nm, l, &ci->num->words);
    wl_setst(l, 0, st_num); /* every word of the list, like the engine */
    wl_items(nm, l, &before);
    {
        const char *unit;
        if (scale < 0) {
            const anna_numinfo *n = ci->num;
            int nl = n->start ? (int)(n->end - n->start) : 0;
            if (!ci->cents && !n->ip && n->fr && n->fr->over == 0) {
                wl_str_st(nm, l, "of", st_num);
                wl_str_st(nm, l, "a", st_num);
                unit = CURRENCY[cur].sing;
            } else if ((nl == 1 && n->start[0] == '1') || (nl == 2 && n->start[0] == '-' && n->start[1] == '1')) {
                unit = CURRENCY[cur].sing;
            } else {
                unit = CURRENCY[cur].plural;
            }
        } else {
            wl_str_st(nm, l, SCALE[scale], st_scale);
            unit = CURRENCY[cur].plural;
        }
        wl_words_st(nm, l, unit, st_cur);
    }
    wl_items(nm, l, &after);
    if (ci->cents) {
        anna_numinfo *c = ci->cents;
        wl_str_st(nm, l, "and", st_num);
        k = l->n;
        wl_take(nm, l, &c->words);
        wl_setst(l, k, st_num);
        if (c->end - c->start == 1 && c->start[0] == '1') wl_str_st(nm, l, CURRENCY[cur].cent_sing, st_cur);
        else wl_str_st(nm, l, CURRENCY[cur].cents, st_cur);
    }
    nm->p = s0;
    *out = (int *)ci;
    return 0;
}
fail:
    if (num) num->words.n = 0;
    nm->p = s0;
    nm->fe = fe0;
    nm->e = e0;
    nm->fi = fi0;
    return r;
}

/* ---- clock times (0x0D5F569D) ---- */

static int ampm_word(const wc *s, const wc *e) /* 0 am, 1 pm, -1 none */
{
    if ((!wnicmp_a(s, "am", 2) && s + 2 == e) || (!wnicmp_a(s, "a.m.", 4) && s + 4 == e)) return 0;
    if ((!wnicmp_a(s, "pm", 2) && s + 2 == e) || (!wnicmp_a(s, "p.m.", 4) && s + 4 == e)) return 1;
    return -1;
}

/* end of a lookahead token for am/pm: clause marks, closing marks and quotes go; ! and ? go; '.' stays
 * (mode 0) or goes only in "am." / "pm." (mode 1) */
static wc *ampm_end(wc *q, wc *e, int mode)
{
    for (;;) {
        wc c = e[-1];
        int t;
        if (clause_type(c) != 0x1001) {
            e--;
            continue;
        }
        if (close_type(c) != 0x1001 || quote_type(c) != 0x1001) {
            e--;
            continue;
        }
        t = sent_type(c);
        if (t == 0x1001) break;
        if (t == 9) {
            if (mode == 0) break;
            if (!(!wnicmp_a(q, "am.", 3) && q + 3 == e) && !(!wnicmp_a(q, "pm.", 3) && q + 3 == e)) break;
        }
        e--;
    }
    return e;
}

static int h_clock(anna_norm *nm, int **out, anna_wlist *l, int lookahead)
{
    wc *p = nm->p, *e = nm->e, *q, *m = NULL, *mend, *ne = NULL, *nq, *nfe = nm->fe;
    unsigned long hour, min = 0;
    int ampm = 2, consumed = 0, nfi = nm->fi;
    anna_timeinfo *ti;
    ilist sp = {NULL, NULL};
    const anna_vstate *st0 = cur_st(nm), *st_ampm = NULL;
    int f[4] = {0};
    if (e - p > 9) return DECLINE;
    hour = wnum(p, &q);
    if (q == p || q - p > 2) return DECLINE;
    if (*q != ':') {
        if (q < e) {
            ampm = ampm_word(q, e);
            if (ampm < 0) return DECLINE;
            st_ampm = st0;
        } else {
            if (!lookahead) return DECLINE;
            nq = e;
            if (adv(nm, &nq, &nfe, &nfi, &sp) < 0) return NOMEM;
            if (!nq) return DECLINE;
            ne = ampm_end(nq, token_end(nq, nfe), 0);
            ampm = ampm_word(nq, ne);
            if (ampm < 0) return DECLINE;
            st_ampm = frag_st(nm, nfi);
            consumed = 1;
        }
    } else {
        m = q + 1;
        min = wnum(m, &mend);
        if (m == mend || mend - m != 2) return DECLINE;
        if (mend != e) {
            ampm = ampm_word(mend, e);
            if (ampm < 0) return DECLINE;
            if (hour - 1 >= 23 || min >= 60) return DECLINE;
            st_ampm = st0;
        } else {
            if (hour - 1 > 22 || min > 59) return DECLINE;
            if (lookahead) {
                nq = e;
                if (adv(nm, &nq, &nfe, &nfi, &sp) < 0) return NOMEM;
                if (nq) {
                    int a;
                    ne = ampm_end(nq, token_end(nq, nfe), 1);
                    a = ampm_word(nq, ne);
                    if (a >= 0) {
                        ampm = a;
                        consumed = 1;
                        st_ampm = frag_st(nm, nfi);
                    }
                }
            }
        }
    }
    ti = amalloc(nm, sizeof *ti);
    if (!ti) return NOMEM;
    ti->type = 0x1018;
    ti->minutes = m != NULL;
    ti->ampm = ampm != 2;
    if (a_isdigit(p[1])) w_two(nm, p, f, l);
    else w_digit(nm, p[0], f, l);
    if (m) {
        if (!wncmp_a(m, "00", 2)) {
            if (wnum(p, NULL) > 12) {
                ti->hundred = 1;
                wl_str(nm, l, "hundred");
                wl_str(nm, l, "hours");
            } else {
                wl_str(nm, l, "o'clock");
            }
        } else if (*m == '0') {
            wl_str(nm, l, "o");
            w_digit(nm, m[1], f, l);
        } else {
            w_two(nm, m, f, l);
        }
    }
    wl_setst(l, 0, st0); /* the engine sets the state of every word in the list (0x0D5F5D5A) */
    wl_items(nm, l, &sp);
    if (ampm == 0) {
        anna_vstate *st = amalloc(nm, sizeof *st);
        if (!st) return NOMEM;
        *st = *st_ampm;
        st->pos = 0x1000;
        wl_str_st(nm, l, "a", st);
        wl_str_st(nm, l, "m", st);
    } else if (ampm == 1) {
        wl_str_st(nm, l, "p", st_ampm);
        wl_str_st(nm, l, "m", st_ampm);
    }
    if (consumed) {
        nm->fi = nfi;
        nm->fe = nfe;
        nm->e = ne;
    }
    *out = (int *)ti;
    return 0;
}

/* ---- durations h:mm:ss / m:ss (0x0D5F617E, words 0x0D5F5E8B) ---- */

static int ndigits(unsigned long v)
{
    int n = 1;
    while (v >= 10) {
        v /= 10;
        n++;
    }
    return n;
}

static int h_duration(anna_norm *nm, int **out, const wc *ctx)
{
    wc *s0 = nm->p, *e0 = nm->e, *q = nm->p, *colon, *mstart, *mend, *sec, *send;
    anna_numinfo *first = NULL;
    unsigned long mval;
    int neg = 0, r;
    anna_durinfo *d;
    if (*q == '-') {
        neg = 1;
        q++;
    }
    while (*q == '0') q++;
    if (*q == ':') q--;
    colon = (wc *)wchr(q, ':');
    if (!colon || !*colon || colon <= q || colon >= nm->e - 1) return DECLINE;
    nm->p = q;
    nm->e = colon;
    r = h_num(nm, (int **)&first, "NUMBER");
    nm->p = s0;
    nm->e = e0;
    if (r < 0 || (first->type != 0x1008 && first->type != 0x1006)) {
        if (r >= 0) first->words.n = 0;
        return DECLINE;
    }
    if (neg) {
        anna_wlist w = {NULL, 0, 0};
        first->neg = 1;
        wl_str(nm, &w, "negative");
        wl_cat(nm, &w, &first->words);
        first->words = w;
    }
    mstart = colon + 1;
    mval = wnum(mstart, &mend);
    if (mstart == mend || mend - mstart != 2) goto no;
    if (mend != nm->e) {
        unsigned long sval;
        anna_numinfo *mins = NULL;
        wc *mp;
        if (*mend != ':') goto no;
        sec = mend + 1;
        sval = wnum(sec, &send);
        if (sec == send || send - sec != 2 || send != nm->e || mval >= 60 || sval >= 60) goto no;
        d = amalloc(nm, sizeof *d);
        if (!d) return NOMEM;
        d->type = 0x1019;
        d->hours = first;
        if (mval == 0) mp = sec - 2;
        else mp = mstart + ((int)(sec - 2 - mstart) - (ndigits(mval) - 1));
        nm->e = sec - 1;
        nm->p = mp;
        r = parse_number(nm, (int **)&mins, "NUMBER", 1);
        nm->p = s0;
        nm->e = e0;
        if (r < 0) {
            first->words.n = 0;
            return r;
        }
        d->minutes = mins;
        d->seconds = *sec == '0' ? sec + 1 : sec;
    } else {
        if (mval > 0x3b) goto no;
        if (!ctx || !wicmp_a(ctx, "TIME_MS")) {
            d = amalloc(nm, sizeof *d);
            if (!d) return NOMEM;
            d->type = 0x1019;
            d->minutes = first;
            d->seconds = *mstart == '0' ? colon + 2 : mstart;
        } else if (!wicmp_a(ctx, "TIME_HM")) {
            anna_numinfo *mins = NULL;
            d = amalloc(nm, sizeof *d);
            if (!d) return NOMEM;
            d->type = 0x1019;
            d->hours = first;
            nm->e = mend;
            nm->p = *mstart == '0' ? colon + 2 : mstart;
            r = parse_number(nm, (int **)&mins, "NUMBER", 1);
            nm->p = s0;
            nm->e = e0;
            if (r < 0) {
                first->words.n = 0;
                return r;
            }
            d->minutes = mins;
        } else {
            goto no;
        }
    }
    *out = (int *)d;
    return 0;
no:
    first->words.n = 0;
    return DECLINE;
}

static void duration_words(anna_norm *nm, anna_durinfo *d, anna_wlist *l)
{
    int f[4] = {0};
    const afrag *fr = nm->fi >= 0 ? &nm->fr[nm->fi] : NULL;
    const wc *fend = fr ? fr->t + fr->len : NULL;
    if (d->seconds && fend) { /* "0:30 GMT" -> "thirty" "three zero" */
        const wc *g = d->seconds, *x;
        for (x = g; *x; x++)
            if (x[0] == 'G' && x[1] == 'M' && x[2] == 'T') break;
        if (*x && x - g < 4) {
            x += 3;
            if (x <= fend && (x == fend || !w_isalpha(*x)) && !d->hours && d->minutes && d->minutes->ip &&
                d->minutes->ip->v[0x19] == 0) {
                wl_take(nm, l, &d->minutes->words);
                if (!a_isdigit(g[1])) w_digit(nm, g[0], f, l);
                else w_two(nm, g, f, l);
                return;
            }
        }
    }
    if (d->hours) {
        anna_numinfo *h = d->hours;
        wl_take(nm, l, &h->words);
        wl_str(nm, l, h->end - h->start == 1 && h->start[0] == '1' ? "hour" : "hours");
        if (d->minutes->start && !d->seconds) wl_str(nm, l, "and");
    }
    if (d->minutes) {
        anna_numinfo *m = d->minutes;
        wl_take(nm, l, &m->words);
        wl_str(nm, l, m->end - m->start == 1 && m->start && m->start[0] == '1' ? "minute" : "minutes");
        if (d->seconds) wl_str(nm, l, "and");
    }
    if (d->seconds) {
        const wc *s = d->seconds;
        if (a_isdigit(s[1])) w_two(nm, s, f, l);
        else w_digit(nm, s[0], f, l);
        wl_str(nm, l, s[0] == '1' && !a_isdigit(s[1]) ? "second" : "seconds");
    }
}

/* ---- ranges "10-20" (0x0D5F3778, words 0x0D5F0E3A) ---- */

static int h_range(anna_norm *nm, int **out)
{
    wc *s = nm->p, *e = nm->e, *h;
    int *left = NULL, *right = NULL, r;
    for (h = s; h < e; h++)
        if (*h == '-') break;
    if (!(*h == '-' && s < h && h < e - 1)) return DECLINE;
    nm->e = h;
    r = parse_number(nm, &left, NULL, 1);
    if (r >= 0) {
        nm->p = h + 1;
        nm->e = e;
        r = h_num(nm, &right, NULL);
        if (r < 0) {
            if (*left != 0x1014) ((anna_numinfo *)left)->words.n = 0;
        } else {
            anna_rangeinfo *ri = amalloc(nm, sizeof *ri);
            if (!ri) return NOMEM;
            ri->type = 0x101e;
            ri->left = left;
            ri->right = right;
            *out = (int *)ri;
        }
    }
    nm->p = s;
    if (right && *right == 0x100f) return r;
    nm->e = e;
    return r;
}

/* ---- decades "1990s", "'90s" (0x0D5E1529, words 0x0D5E19FE) ---- */

static int h_decade(anna_norm *nm, int **out)
{
    const wc *p = nm->p;
    int n = (int)(nm->e - p), ok;
    anna_decadeinfo *d;
    const wc *cent = NULL;
    int dec;
    if (n < 3 || n > 6) return DECLINE;
    if (n == 3) {
        ok = p[2] == 's';
    } else if (n == 4) {
        if (p[3] != 's') return DECLINE;
        if (p[2] == '0' && a_isdigit(p[1]) && p[0] == '\'') {
            dec = p[1] - '0';
            goto make;
        }
        ok = p[2] == '\'';
    } else {
        if (n == 5) {
            ok = p[4] == 's';
        } else {
            if (p[5] != 's') return DECLINE;
            ok = p[4] == '\'';
        }
        if (!ok || p[3] != '0' || !a_isdigit(p[2]) || !a_isdigit(p[1]) || !a_isdigit(p[0])) return DECLINE;
        cent = p;
        dec = p[2] - '0';
        goto make;
    }
    if (!ok || p[1] != '0' || !a_isdigit(p[0])) return DECLINE;
    dec = p[0] - '0';
make:
    d = amalloc(nm, sizeof *d);
    if (!d) return NOMEM;
    d->type = 0x1017;
    d->century = cent;
    d->decade = dec;
    *out = (int *)d;
    return 0;
}

static void decade_words(anna_norm *nm, const anna_decadeinfo *d, anna_wlist *l)
{
    static const char *const DEC[10] = {"thousands", "tens", "twenties", "thirties", "forties", "fifties",
                                        "sixties", "seventies", "eighties", "nineties"};
    const wc *c = d->century;
    int f[4] = {0};
    if (!c) {
        if (d->decade == 0) wl_str(nm, l, "two");
    } else if (c[0] == '0') {
        if (c[1] == '0') {
            if (d->decade == 0) {
                wl_str(nm, l, "zeroes");
                return;
            }
        } else {
            wl_str(nm, l, ONES[c[1] - '0']);
            if (d->decade == 0) {
                wl_str(nm, l, "hundreds");
                return;
            }
            wl_str(nm, l, "hundred");
        }
    } else if (d->decade == 0) {
        if (c[1] != '0') {
            w_two(nm, c, f, l);
            wl_str(nm, l, "hundreds");
            return;
        }
        wl_str(nm, l, ONES[c[0] - '0']);
    } else {
        w_two(nm, c, f, l);
    }
    wl_str(nm, l, DEC[d->decade]);
}

/* ---- phone numbers (0x0D5F1BD8) ---- */

static int is_psep(wc c) { return c == ' ' || c == '-' || c == '.'; } /* 0x0D5EF3A8 */

static int h_phone(anna_norm *nm, int **out, anna_wlist *l, const wc *mode)
{
    wc *c = nm->p, *e = nm->e, *fe = nm->fe, *sep = NULL, *country = NULL, *area = NULL, *saved_fe = NULL;
    wc *gp[4] = {0};
    int fi = nm->fi, saved_fi = 0, have_saved = 0, clen = 0, one = 0, alen = 0, k, G = 0, i;
    int gl[4] = {0};
    ilist sp_country = {NULL, NULL}, sp_one = {NULL, NULL}, sp_area = {NULL, NULL}, sp_g[4];
    const anna_vstate *st_country = NULL, *st_one = NULL, *st_area = NULL, *st_g[4] = {0};
    memset(sp_g, 0, sizeof sp_g);
    if (*c == '+') {
        wc *q;
        c++;
        k = 0;
        while (c + k < e && a_isdigit(c[k]) && k < 3) k++;
        country = c;
        clen = k;
        st_country = frag_st(nm, fi);
        if (k == 0) return DECLINE;
        q = c + k;
        if (q < e && is_psep(*q)) {
            sep = q;
            c = q + 1;
        } else {
            if (e != q) return DECLINE;
            c = q;
            if (adv(nm, &c, &fe, &fi, &sp_country) < 0) return NOMEM;
            if (!c) return DECLINE;
            e = token_end(c, fe);
        }
    } else if (*c == '1' && !a_isdigit(c[1])) {
        wc *c1 = c;
        st_one = frag_st(nm, fi);
        one = 1;
        c++;
        if (c < e && is_psep(*c)) {
            if (!sep) sep = c;
            else if (*sep != *c) return DECLINE;
            c = c1 + 2;
        } else if (!sep && e == c) {
            if (adv(nm, &c, &fe, &fi, &sp_one) < 0) return NOMEM;
            if (!c) return DECLINE;
            e = token_end(c, fe);
        } else {
            return DECLINE;
        }
    }
    if (c < e) {
        int area_ok = 1;
        if (!country && !one) {
            if (c <= nm->fr[fi].t || c[-1] != '(') area_ok = 0;
        } else {
            if (*c != '(') area_ok = 0;
            else c++;
        }
        if (area_ok) {
            wc *rp, *q4;
            k = 0;
            while (c + k < e && a_isdigit(c[k]) && k < 3) k++;
            st_area = frag_st(nm, fi);
            area = c;
            alen = k;
            if (k < 2 || c[k] != ')') return DECLINE;
            rp = c + k;
            q4 = (country || one) ? rp + 1 : rp;
            if (q4 < e) {
                int idx = k + 1;
                wc ch = c[idx];
                if (is_psep(ch)) {
                    if (!sep) sep = &c[idx];
                    else if (*sep != ch) return DECLINE;
                    idx = k + 2;
                }
                c = c + idx;
            } else if (!sep) {
                c = rp + 1;
                if (adv(nm, &c, &fe, &fi, &sp_area) < 0) return NOMEM;
                if (!c) return DECLINE;
                e = token_end(c, fe);
            } else {
                return DECLINE;
            }
        }
    }
    if (c < e) {
        for (i = 0;; i++) {
            int n = 0;
            wc *q;
            if (i > 3) {
                G = i;
                break;
            }
            if (c < e) {
                while (a_isdigit(c[n]) && n <= 3) {
                    n++;
                    if (c + n >= e) break;
                }
            }
            if (n < 2) {
                if (sep) return DECLINE;
                if (have_saved) {
                    fe = saved_fe;
                    fi = saved_fi;
                }
                G = i;
                break;
            }
            gp[i] = c;
            gl[i] = n;
            st_g[i] = frag_st(nm, fi);
            c += n;
            q = c + 1;
            if (q < e && is_psep(*c)) {
                if (!sep) {
                    if (i != 0) {
                        fe = saved_fe;
                        fi = saved_fi;
                        G = i;
                        break;
                    }
                    sep = c;
                } else if (*sep != *c) {
                    return DECLINE;
                }
                c = q;
                continue;
            }
            if (!sep && e == c) {
                wc *sfe = fe;
                int sfi = fi;
                saved_fe = fe;
                saved_fi = fi;
                have_saved = 1;
                if (adv(nm, &c, &fe, &fi, &sp_g[i]) < 0) return NOMEM;
                if (!c) {
                    fe = sfe;
                    fi = sfi;
                    G = i + 1;
                    break;
                }
                e = token_end(c, fe);
                continue;
            }
            if (e == q) {
                if (!is_punct_any(*c)) return DECLINE;
                G = i + 1;
                break;
            }
            if (e == c) {
                G = i + 1;
                break;
            }
            { /* characters after the last group: acceptable only if all punctuation */
                wc *x = e;
                for (;;) {
                    if (!is_punct_any(*x)) break;
                    x--;
                    if (x == c) break;
                }
                if (x != c) return DECLINE;
                e = x;
                G = i + 1;
                break;
            }
        }
    }
    /* pattern checks */
#define L(x) gl[x]
#define SHIFT()                                                                                           \
    do {                                                                                                  \
        area = gp[0];                                                                                     \
        alen = gl[0];                                                                                     \
        st_area = st_g[0];                                                                                \
        sp_area = sp_g[0];                                                                                \
        for (k = 0; k < 3; k++) {                                                                         \
            gp[k] = gp[k + 1];                                                                            \
            gl[k] = gl[k + 1];                                                                            \
            st_g[k] = st_g[k + 1];                                                                        \
            sp_g[k] = sp_g[k + 1];                                                                        \
        }                                                                                                 \
    } while (0)
    if (country) {
        if (one) return DECLINE;
        if (!area && G == 4 && (L(0) == 2 || L(0) == 3) && (L(1) == 2 || L(1) == 3) && L(2) >= 2 && L(3) >= 2) {
            SHIFT();
            G = 3;
        } else if (!area) {
            if (G == 3 && (L(0) == 2 || L(0) == 3) && (L(1) == 2 || L(1) == 3) && L(2) >= 2) {
                SHIFT();
                sp_g[1].head = sp_g[1].tail = NULL; /* the engine forgets to move the third group's list */
                G = 2;
            } else {
                return DECLINE;
            }
        } else if (G == 3 && (L(0) == 2 || L(0) == 3) && L(1) >= 2 && L(2) >= 2) {
        } else if (G == 2 && (L(0) == 2 || L(0) == 3) && L(1) >= 2) {
        } else {
            return DECLINE;
        }
    } else if (G == 2 && L(0) == 3 && L(1) > 2) {
        if (!one || area) {
            if ((!mode || wicmp_a(mode, "phone_number")) && !area && !one && sep && *sep == '.') return DECLINE;
        } else {
            return DECLINE;
        }
    } else if (!area && G == 3 && (L(0) == 2 || L(0) == 3) && L(1) == 3 && L(2) >= 3) {
        SHIFT();
        sp_g[1].head = sp_g[1].tail = NULL; /* the engine forgets to move the third group's list */
        G = 2;
    } else if (!one && area && G == 3 && (L(0) == 2 || L(0) == 3) && L(1) == 2 && L(2) > 1) {
    } else {
        return DECLINE;
    }
#undef SHIFT
#undef L
    {
        anna_phoneinfo *pi = amalloc(nm, sizeof *pi);
        int f[4] = {0};
        if (!pi) return NOMEM;
        pi->type = 0x1027;
        pi->one = one;
        nm->e = gp[G - 1] + gl[G - 1];
        nm->fe = fe;
        nm->fi = fi;
        if (country) {
            wc *sp = nm->p, *se = nm->e;
            int r;
            nm->p = country;
            nm->e = country + clen;
            r = parse_number(nm, (int **)&pi->country, "NUMBER", 0);
            nm->p = sp;
            nm->e = se;
            if (r < 0) return r;
        }
        if (area) {
            pi->area = amalloc(nm, sizeof(anna_wspan));
            if (!pi->area) return NOMEM;
            pi->area->s = area;
            pi->area->n = alen;
        }
        pi->ngroups = G;
        pi->groups = amalloc(nm, sizeof(anna_wspan) * (size_t)(G ? G : 1));
        if (!pi->groups) return NOMEM;
        for (i = 0; i < G; i++) {
            pi->groups[i].s = gp[i];
            pi->groups[i].n = gl[i];
        }
        if (country) {
            wl_str_st(nm, l, "country", st_country);
            wl_str_st(nm, l, "code", st_country);
            k = l->n;
            wl_take(nm, l, &pi->country->words);
            wl_setst(l, k, st_country);
            wl_items(nm, l, &sp_country);
        }
        if (one) {
            wl_str_st(nm, l, "one", st_one);
            wl_items(nm, l, &sp_one);
        }
        if (area) {
            k = l->n;
            if ((area[0] == '8' || area[0] == '9') && area[1] == '0' && area[2] == '0') {
                pi->is800 = 1;
                w_three(nm, area, f, l);
            } else {
                wl_str(nm, l, "area");
                wl_str(nm, l, "code");
                for (i = 0; i < alen; i++) w_digit(nm, area[i], f, l);
            }
            wl_setst(l, k, st_area);
            wl_items(nm, l, &sp_area);
        }
        for (i = 0; i < G; i++) {
            k = l->n;
            for (int j = 0; j < gl[i]; j++) w_digit(nm, gp[i][j], f, l);
            wl_setst(l, k, st_g[i]);
            wl_items(nm, l, &sp_g[i]);
        }
        *out = (int *)pi;
    }
    return 0;
}

/* ---- dates ---- */

static int date_sep(wc **p) /* 0x0D5E02F2: '/', '-', '.' */
{
    if (**p == '/' || **p == '-' || **p == '.') {
        (*p)++;
        return 1;
    }
    return 0;
}

static int month_parse(wc **p, int avail) /* 0x0D5E014F */
{
    static const char *const AB[13] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sept", "sep",
                                       "oct", "nov", "dec"};
    int k;
    for (k = 1; k <= 12; k++) {
        int l = (int)strlen(MONTHS[k]);
        if (avail >= l && !wnicmp_a(*p, MONTHS[k], l)) {
            *p += l;
            return k;
        }
    }
    for (k = 0; k < 13; k++) {
        int l = (int)strlen(AB[k]);
        if (avail >= l && !wnicmp_a(*p, AB[k], l)) {
            int m = k < 9 ? k + 1 : k;
            *p += l;
            if (avail > l && **p == '.') (*p)++;
            return m;
        }
    }
    return 0;
}

static int weekday_parse(wc **p, int avail) /* 0x0D5E020B */
{
    static const char *const AB[10] = {"Mon", "Tues", "Tue", "Wed", "Thurs", "Thur", "Thu", "Fri", "Sat", "Sun"};
    static const int IDX[10] = {1, 2, 2, 3, 4, 4, 4, 5, 6, 7};
    int k;
    for (k = 1; k <= 7; k++) {
        int l = (int)strlen(WEEKDAYS[k]);
        if (avail >= l && !wnicmp_a(*p, WEEKDAYS[k], l)) {
            *p += l;
            return k;
        }
    }
    for (k = 0; k < 10; k++) {
        int l = (int)strlen(AB[k]);
        if (avail >= l && !wncmp_a(*p, AB[k], l)) {
            *p += l;
            if (avail > l && **p == '.') (*p)++;
            return IDX[k];
        }
    }
    return 0;
}

static anna_dateinfo *new_date(anna_norm *nm, int month)
{
    anna_dateinfo *d = amalloc(nm, sizeof *d);
    if (!d) return NULL;
    d->type = 0x1015;
    d->month = month;
    return d;
}

static void set_day(anna_dateinfo *d, const wc *s, int n) /* two digits with a leading zero read as one */
{
    if (n == 2 && *s == '0') {
        d->day = s + 1;
        d->daylen = 1;
    } else {
        d->day = s;
        d->daylen = n == 2 ? 2 : 1;
    }
}

/* 0x0D5E04F8: numeric dates "12/25/2004"; order by <context> Date_MDY/DMY/YMD (default month first, this+0x74
 * == 1); two-part dates only with Date_MD/DM/MY/YM */
static int h_date_num(anna_norm *nm, int **out, const wc *ctx)
{
    wc *p = nm->p, *a_end, *b, *b_end, *c, *c_end;
    unsigned long A, B, C;
    int n = (int)(nm->e - p), ad, bd, cd, mdy = 0, dmy = 0;
    anna_dateinfo *d;
    if (n > 10) return DECLINE;
    if (ctx && !wicmp_a(ctx, "Date_MDY")) mdy = 1;
    else if (ctx && !wicmp_a(ctx, "Date_DMY")) dmy = 1;
    else if (ctx && !wicmp_a(ctx, "Date_YMD")) {
    } else mdy = 1;
    A = wnum(p, &a_end);
    if (p == a_end || a_end - p > 4) return DECLINE;
    b = a_end;
    if (!date_sep(&b)) return DECLINE;
    B = wnum(b, &b_end);
    if (b == b_end || b_end - b > 4) goto two_part_fail;
    c = b_end;
    if (*b_end == *a_end && date_sep(&c)) {
        C = wnum(c, &c_end);
        if (c == c_end || c_end != p + n) return DECLINE;
        cd = (int)(c_end - c);
        if (cd > 4) return DECLINE;
        ad = (int)(b - p) - 1;
        bd = (int)(c - b) - 1;
#define MDY_OK (A != 0 && A < 13 && ad < 4 && B - 1 < 31 && bd < 4 && C < 10000 && cd > 1)
#define DMY_OK (A != 0 && A < 32 && ad < 4 && B - 1 < 12 && bd < 4 && C < 10000 && cd > 1)
#define YMD_OK (A <= 9999 && ad >= 2 && B - 1 < 12 && bd <= 3 && C - 1 < 31 && cd <= 3)
        if (mdy) {
            if (MDY_OK) {
            } else if (DMY_OK) {
                mdy = 0;
                dmy = 1;
            } else if (YMD_OK) {
                mdy = 0;
            } else {
                return DECLINE;
            }
        } else if (dmy) {
            if (DMY_OK) {
            } else if (MDY_OK) {
                dmy = 0;
                mdy = 1;
            } else if (YMD_OK) {
                dmy = 0;
            } else {
                return DECLINE;
            }
        } else {
            if (YMD_OK) {
            } else if (A != 0 && A < 13 && ad < 4 && B - 1 < 31 && bd < 4 && C < 10000 && cd > 1) {
                mdy = 1;
            } else if (A != 0 && A < 32 && ad < 4 && B - 1 < 12 && bd < 4 && C < 10000 && cd > 1) {
                dmy = 1;
            } else {
                return DECLINE;
            }
        }
#undef MDY_OK
#undef DMY_OK
#undef YMD_OK
        if (mdy) {
            d = new_date(nm, (int)A);
            if (!d) return NOMEM;
            set_day(d, b, bd);
            d->year = c;
            d->yearlen = cd;
        } else if (dmy) {
            d = new_date(nm, (int)B);
            if (!d) return NOMEM;
            set_day(d, p, ad);
            d->year = c;
            d->yearlen = cd;
        } else {
            d = new_date(nm, (int)B);
            if (!d) return NOMEM;
            set_day(d, c, cd);
            d->year = p;
            d->yearlen = ad;
        }
        *out = (int *)d;
        return 0;
    }
    if (b_end != nm->e) return DECLINE;
    if (ctx) { /* two-part dates */
        int an = (int)(b - p) - 1, bn = (int)(b_end - b);
        if (!wicmp_a(ctx, "Date_MD")) {
            if (A - 1 < 12 && an < 3 && B - 1 < 31 && bn < 3) {
                d = new_date(nm, (int)A);
                if (!d) return NOMEM;
                if (*b == '0') {
                    d->day = b + 1;
                    bn--;
                } else {
                    d->day = b;
                }
                d->daylen = bn;
                *out = (int *)d;
                return 0;
            }
        } else if (!wicmp_a(ctx, "Date_DM")) {
            if (A - 1 < 31 && an < 3 && B - 1 < 12 && bn < 3) {
                d = new_date(nm, (int)B);
                if (!d) return NOMEM;
                if (*p == '0') {
                    d->day = p + 1;
                    an--;
                } else {
                    d->day = p;
                }
                d->daylen = an;
                *out = (int *)d;
                return 0;
            }
        } else if (!wicmp_a(ctx, "Date_MY")) {
            if (A - 1 < 12 && an < 3 && B < 10000 && bn > 1) {
                d = new_date(nm, (int)A);
                if (!d) return NOMEM;
                d->year = b;
                d->yearlen = bn;
                *out = (int *)d;
                return 0;
            }
        } else if (!wicmp_a(ctx, "Date_YM")) {
            if (A < 10000 && an > 1 && B - 1 < 12 && bn < 3) {
                d = new_date(nm, (int)B);
                if (!d) return NOMEM;
                d->year = p;
                d->yearlen = an;
                *out = (int *)d;
                return 0;
            }
        }
    }
    return DECLINE;
two_part_fail:
    return DECLINE;
}

/* 0x0D5E0E20: dates with a month name inside the token: "25-Dec-2004", "Dec-25-2004", "Dec-2004" */
static int h_date_name(anna_norm *nm, int **out, const wc *ctx)
{
    wc *p = nm->p, *q, *c, *cend;
    int n = (int)(nm->e - p), mdy = 0, dmy = 0;
    anna_dateinfo *d;
    if (n > 17) return DECLINE;
    if (ctx && !wicmp_a(ctx, "Date_MDY")) mdy = 1;
    else if (ctx && !wicmp_a(ctx, "Date_DMY")) dmy = 1;
    else if (ctx && !wicmp_a(ctx, "Date_YMD")) {
    } else mdy = 1;
    if (w_isalpha(p[0])) {
        unsigned long B, C;
        int month, blen, clen;
        q = p;
        month = month_parse(&q, n);
        if (!month || !date_sep(&q)) return DECLINE;
        B = wnum(q, &c);
        if (q == c) return DECLINE;
        blen = (int)(c - q);
        d = new_date(nm, month);
        if (!d) return NOMEM;
        if (blen > 2) {
            if (blen > 4 || B > 9999) return DECLINE;
            d->year = q; /* month-year */
            d->yearlen = blen;
        } else if (date_sep(&c)) {
            C = wnum(c, &cend);
            if (c == cend) return DECLINE;
            clen = (int)(cend - c);
            if (clen > 4 || B - 1 > 30 || blen > 2 || C > 9999 || clen < 2) return DECLINE;
            set_day(d, q, blen);
            d->year = c;
            d->yearlen = clen;
        } else if (!ctx || !wicmp_a(ctx, "Date_MD")) {
            if (B - 1 < 31 && blen < 3) {
                set_day(d, q, blen);
            } else {
                d->year = q;
                d->yearlen = blen;
            }
        } else if (!wicmp_a(ctx, "Date_MY")) {
            if (B > 9999 || blen > 4) return DECLINE;
            d->year = q;
            d->yearlen = blen;
        } else {
            return DECLINE;
        }
    } else {
        unsigned long A, C;
        int alen, month, clen;
        if (!a_isdigit(p[0])) return DECLINE;
        A = wnum(p, &q);
        if (p == q) return DECLINE;
        alen = (int)(q - p);
        if (alen > 4 || !date_sep(&q)) return DECLINE;
        c = q;
        month = month_parse(&c, n - alen);
        if (!month) return DECLINE;
        if (date_sep(&c) || (*c && a_isdigit(*c))) {
            int ymd = 0;
            C = wnum(c, &cend);
            if (c == cend) return DECLINE;
            clen = (int)(cend - c);
            if (clen > 4) return DECLINE;
            if (!mdy && !dmy) {
                if (A < 10000 && alen > 1 && C - 1 < 31 && clen < 3) ymd = 1;
                else if (A - 1 > 30 || alen > 2 || C > 9999 || clen < 2) return DECLINE;
            } else if (A - 1 > 30 || alen > 2 || C > 9999 || clen < 2) {
                if (A > 9999 || alen < 2 || C - 1 > 30 || clen > 2) return DECLINE;
                ymd = 1;
            }
            d = new_date(nm, month);
            if (!d) return NOMEM;
            if (ymd) {
                set_day(d, c, clen);
                d->year = p;
                d->yearlen = alen;
            } else {
                set_day(d, p, alen);
                d->year = c;
                d->yearlen = clen;
            }
        } else if (!ctx || !wicmp_a(ctx, "Date_DM")) {
            d = new_date(nm, month);
            if (!d) return NOMEM;
            if (A - 1 < 31 && alen < 3) {
                set_day(d, p, alen);
            } else {
                if (A > 9999 || alen > 4) return DECLINE;
                d->year = p;
                d->yearlen = alen;
            }
        } else if (!wicmp_a(ctx, "Date_YM")) {
            d = new_date(nm, month);
            if (!d) return NOMEM;
            if (A < 10000 && alen < 5) {
                d->year = p;
                d->yearlen = alen;
            } else {
                if (A - 1 > 30 || alen > 2) return DECLINE;
                set_day(d, p, alen);
            }
        } else {
            return DECLINE;
        }
    }
    *out = (int *)d;
    return 0;
}

/* end of a lookahead date token: punctuation stripped; *stop = something other than ',' went */
static wc *date_tok_end(wc *q, wc *fe, int *stop)
{
    wc *e = token_end(q, fe);
    for (;;) {
        wc ch = e[-1];
        if (!is_punct_any(ch)) break;
        e--;
        if (ch != ',') *stop = 1;
    }
    return e;
}

static void date_words(anna_norm *nm, const anna_dateinfo *d, anna_wlist *l) /* 0x0D5E271F */
{
    int f[4] = {0};
    if (d->weekday > 0 && d->weekday < 8) wl_str(nm, l, WEEKDAYS[d->weekday]);
    wl_str(nm, l, MONTHS[d->month]);
    if (d->day) {
        if (d->daylen == 1) w_ord1(nm, d->day[0], f, l);
        else if (d->daylen == 2) w_ord2(nm, d->day, f, l);
    }
    if (d->year) year_words(nm, d->year, d->yearlen, l);
}

/* 0x0D5E1B95: "[Weekday,] Month Day[,] [Year]" */
static int h_date_mdy_words(anna_norm *nm, int **out, anna_wlist *l)
{
    wc *p = nm->p, *e = nm->e, *fe = nm->fe, *next, *q, *day = NULL, *dend, *year = NULL, *tend;
    wc *sfe = NULL, *stend = NULL;
    int fi = nm->fi, sfi = 0, weekday, month, stop = 0, daylen = 0, yearlen = 0, k, f[4] = {0};
    unsigned long v;
    anna_dateinfo *d;
    ilist sp1 = {NULL, NULL}, sp2 = {NULL, NULL}, sp3 = {NULL, NULL};
    const anna_vstate *st_wd = NULL, *st_m, *st_d = NULL, *st_y = NULL;
    q = p;
    next = p;
    tend = e;
    weekday = weekday_parse(&q, (int)(e - p));
    if (weekday) {
        if (!(q == e || (q == e - 1 && *e == ','))) return DECLINE;
        st_wd = frag_st(nm, fi);
        next = *e == ',' ? e + 1 : e;
        if (adv(nm, &next, &fe, &fi, &sp1) < 0) return NOMEM;
        if (!next) return DECLINE;
        tend = token_end(next, fe);
    }
    q = next;
    month = month_parse(&q, (int)(tend - next));
    if (!month || (q != tend && (q != tend - 1 || *q != ','))) return DECLINE;
    st_m = frag_st(nm, fi);
    day = tend;
    if (adv(nm, &day, &fe, &fi, &sp2) < 0) return NOMEM;
    if (!day) return DECLINE;
    tend = date_tok_end(day, fe, &stop);
    v = wnum(day, &dend);
    if (v - 1 < 31 && dend - day < 3) {
        if (dend == tend) daylen = (int)(tend - day);
        else if (dend == tend - 1 && *dend == ',') daylen = (int)(tend - day) - 1;
        else goto yearpos;
        st_d = frag_st(nm, fi);
        if (!stop) {
            wc *yq, *yend;
            sfe = fe;
            sfi = fi;
            stend = tend;
            yq = *tend == ',' ? tend + 1 : tend;
            if (adv(nm, &yq, &fe, &fi, &sp3) < 0) return NOMEM;
            if (!yq) {
                stop = 1;
            } else {
                tend = strip_trailing(token_end(yq, fe));
                v = wnum(yq, &yend);
                if (v < 10000 && yend - yq < 5 && yend == tend) {
                    st_y = frag_st(nm, fi);
                    yearlen = (int)(tend - yq);
                    year = yq;
                } else {
                    fe = sfe;
                    fi = sfi;
                    tend = stend; /* special items met stay in sp3, like the engine */
                }
            }
        }
    } else {
    yearpos:
        if (v < 10000 && dend - day < 5 && dend == tend) {
            st_y = frag_st(nm, fi);
            yearlen = (int)(tend - day);
            year = day;
            day = NULL;
            stop = 1;
        } else {
            return DECLINE;
        }
    }
    d = amalloc(nm, sizeof *d);
    if (!d) return NOMEM;
    d->type = 0x1016;
    if (weekday) wl_str_st(nm, l, WEEKDAYS[weekday], st_wd);
    wl_items(nm, l, &sp1);
    wl_str_st(nm, l, MONTHS[month], st_m);
    wl_items(nm, l, &sp2);
    k = l->n;
    if (day && daylen == 1) w_ord1(nm, day[0], f, l);
    else if (day && daylen == 2) w_ord2(nm, day, f, l);
    wl_setst(l, k, st_d);
    wl_items(nm, l, &sp3);
    if (year) {
        k = l->n;
        year_words(nm, year, yearlen, l);
        wl_setst(l, k, st_y);
    }
    nm->fi = fi;
    nm->fe = fe;
    nm->e = tend;
    *out = (int *)d;
    return 0;
}

/* 0x0D5E2169: "[Weekday,] Day[,] Month[,] [Year]" */
static int h_date_dmy_words(anna_norm *nm, int **out, anna_wlist *l)
{
    wc *p = nm->p, *e = nm->e, *fe = nm->fe, *next, *q, *dend, *year = NULL, *tend, *mt, *sfe = NULL, *smt = NULL;
    int fi = nm->fi, sfi = 0, weekday, month, stop = 0, daylen, yearlen = 0, k, f[4] = {0};
    unsigned long v;
    anna_dateinfo *d;
    ilist sp1 = {NULL, NULL}, sp2 = {NULL, NULL}, sp3 = {NULL, NULL};
    const anna_vstate *st_wd = NULL, *st_m, *st_d, *st_y = NULL;
    q = p;
    next = p;
    tend = e;
    weekday = weekday_parse(&q, (int)(e - p));
    if (weekday) {
        if (!(q == e || (q == e - 1 && *e == ','))) return DECLINE;
        st_wd = frag_st(nm, fi);
        next = *e == ',' ? e + 1 : e;
        if (adv(nm, &next, &fe, &fi, &sp1) < 0) return NOMEM;
        if (!next) return DECLINE;
        tend = token_end(next, fe);
    }
    v = wnum(next, &dend);
    if (!(v - 1 < 31 && dend - next < 3)) return DECLINE;
    if (dend == tend) daylen = (int)(tend - next);
    else if (dend == tend - 1 && *dend == ',') daylen = (int)(tend - next) - 1;
    else return DECLINE;
    st_d = frag_st(nm, fi);
    mt = *tend == ',' ? tend + 1 : tend;
    if (adv(nm, &mt, &fe, &fi, &sp2) < 0) return NOMEM;
    if (!mt) return DECLINE;
    {
        wc *ms = mt;
        mt = date_tok_end(ms, fe, &stop);
        q = ms;
        month = month_parse(&q, (int)(mt - ms));
        if (!month || (q != mt && (q != mt - 1 || *q != ','))) return DECLINE;
    }
    st_m = frag_st(nm, fi);
    tend = mt;
    if (!stop) {
        wc *yq, *yend;
        sfe = fe;
        sfi = fi;
        smt = mt;
        yq = *mt == ',' ? mt + 1 : mt;
        if (adv(nm, &yq, &fe, &fi, &sp3) < 0) return NOMEM;
        if (yq) {
            tend = strip_trailing(token_end(yq, fe));
            v = wnum(yq, &yend);
            if (v < 10000 && yend - yq < 5 && yend == tend) {
                st_y = frag_st(nm, fi);
                year = yq;
                yearlen = (int)(tend - yq);
            } else {
                fe = sfe;
                fi = sfi;
                tend = smt; /* special items met stay in sp3, like the engine */
            }
        }
    }
    d = amalloc(nm, sizeof *d);
    if (!d) return NOMEM;
    d->type = 0x1016;
    if (weekday) wl_str_st(nm, l, WEEKDAYS[weekday], st_wd);
    wl_items(nm, l, &sp1);
    wl_str_st(nm, l, MONTHS[month], st_m);
    wl_items(nm, l, &sp2);
    k = l->n;
    if (daylen == 1) w_ord1(nm, next[0], f, l);
    else if (daylen == 2) w_ord2(nm, next, f, l);
    wl_setst(l, k, st_d);
    wl_items(nm, l, &sp3);
    if (year) {
        k = l->n;
        year_words(nm, year, yearlen, l);
        wl_setst(l, k, st_y);
    }
    nm->fi = fi;
    nm->fe = fe;
    nm->e = tend;
    *out = (int *)d;
    return 0;
}

/* ---- US state + ZIP code (0x0D5EB510, ZIP 0x0D5EF739, words 0x0D5F0DB7) ---- */

static int h_zip(anna_norm *nm, int **out) /* 0x0D5EF739 (also the ADDRESS context) */
{
    const wc *p = nm->p;
    int n = (int)(nm->e - p), k, plus4 = 0;
    anna_zipinfo *z;
    if (n != 5 && n != 10) return DECLINE;
    for (k = 0; k < 5; k++)
        if (!a_isdigit(p[k])) return DECLINE;
    if (k < n) {
        if (p[k] != '-') return DECLINE;
        for (k = 0; k < 4; k++) /* the engine checks p[0..3] here */
            if (!a_isdigit(p[k])) return DECLINE;
        plus4 = 1;
    }
    z = amalloc(nm, sizeof *z);
    if (!z) return NOMEM;
    z->type = 0x1013;
    z->zip = p;
    z->plus4 = plus4 ? p + 6 : NULL;
    *out = (int *)z;
    return 0;
}

static void zip_words(anna_norm *nm, const anna_zipinfo *z, anna_wlist *l)
{
    int f[4] = {0}, k;
    for (k = 0; k < 5; k++) w_digit(nm, z->zip[k], f, l);
    if (z->plus4) {
        wl_str(nm, l, "dash");
        for (k = 0; k < 4; k++) w_digit(nm, z->plus4[k], f, l);
    }
}

static int h_state_zip(anna_norm *nm, int **out, anna_wlist *l)
{
    wc *p0 = nm->p, *fe0 = nm->fe, *e0 = nm->e;
    int fi0 = nm->fi, lo = 0, hi = 62, found = -1, r, k0;
    anna_zipinfo *z = NULL;
    ilist sp = {NULL, NULL};
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = starter_cmp(p0, (int)(e0 - p0), STATES[mid][0]);
        if (!c) {
            found = mid;
            break;
        }
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    if (found < 0) return DECLINE;
    nm->p = e0;
    if (*e0 == ',' || *e0 == ';') nm->p = e0 + 1;
    r = adv(nm, &nm->p, &nm->fe, &nm->fi, &sp);
    if (r < 0) return r;
    if (!nm->p) {
        nm->p = p0;
        return DECLINE;
    }
    nm->e = strip_trailing(token_end(nm->p, nm->fe));
    r = h_zip(nm, (int **)&z);
    if (r < 0) {
        nm->p = p0;
        nm->fe = fe0;
        nm->e = e0;
        nm->fi = fi0;
        return DECLINE;
    }
    {
        anna_addrinfo *a = amalloc(nm, sizeof *a);
        if (!a) return NOMEM;
        a->type = 0x101c;
        a->zip = z;
        k0 = l->n;
        wl_words(nm, l, STATES[found][1]);
        wl_setst(l, k0, frag_st(nm, fi0));
        wl_items(nm, l, &sp);
        zip_words(nm, z, l);
        *out = (int *)a;
    }
    nm->p = p0;
    return 0;
}

/* ---- time ranges "9am-5pm" (0x0D5F652B) ---- */

static int h_time_range(anna_norm *nm, int **out, anna_wlist *l)
{
    wc *p0 = nm->p, *fe0 = nm->fe, *e0 = nm->e, *dash, *pd = NULL;
    int fi0 = nm->fi, patched = 0, r;
    int *t1 = NULL, *t2 = NULL;
    anna_wlist w = {NULL, 0, 0};
    for (dash = p0; dash < e0 && *dash != '-'; dash++) {
    }
    if (dash == e0) {
        wc *nq = dash;
        wc *d2 = NULL;
        ilist sp = {NULL, NULL};
        r = adv(nm, &nq, &nm->fe, &nm->fi, &sp);
        if (r < 0) return r;
        if (nq) {
            if (!wnicmp_a(nq, "am", 2) && nq[2] == '-') d2 = nq + 2;
            else if (!wnicmp_a(nq, "pm", 2) && nq[2] == '-') d2 = nq + 2;
            else if (!wnicmp_a(nq, "a.m.", 4) && nq[4] == '-') d2 = nq + 4;
            else if (!wnicmp_a(nq, "p.m.", 4) && nq[4] == '-') d2 = nq + 4;
        }
        if (!d2) {
            r = DECLINE;
            goto fail;
        }
        pd = d2;
        *pd = ' ';
        patched = 1;
        dash = d2;
    }
    if (nm->p < dash && dash < nm->e) nm->e = dash;
    r = h_clock(nm, &t1, &w, patched);
    if (r == DECLINE && dash <= nm->p + 2) {
        wc *q = NULL;
        unsigned long v = wnum(nm->p, &q);
        if (q == dash && v - 1 < 23) {
            int f[4] = {0};
            if (q - nm->p == 1) w_digit(nm, *nm->p, f, &w);
            else w_two(nm, nm->p, f, &w);
            r = 0;
            t1 = NULL;
        }
    }
    if (r >= 0) {
        wl_str(nm, &w, "to");
        nm->p = dash + 1;
        nm->e = strip_trailing(token_end(dash + 1, nm->fe));
        r = h_clock(nm, &t2, &w, 1);
        if (r >= 0) {
            anna_rangeinfo *ti = amalloc(nm, sizeof *ti);
            if (!ti) return NOMEM;
            nm->p = p0;
            nm->fe = fe0;
            ti->type = 0x101d;
            ti->left = t1;
            ti->right = t2;
            wl_cat(nm, l, &w);
            *out = (int *)ti;
            return 0;
        }
    }
fail:
    nm->p = p0;
    nm->fe = fe0;
    nm->e = e0;
    nm->fi = fi0;
    if (patched) *pd = '-';
    return r < 0 ? r : DECLINE;
}

/* ---- currency ranges "$5-$10" (0x0D5F387C) ---- */

/* the engine's scratch copy "text + symbol" (on the sentence heap) */
static wc *cat_text_sym(anna_norm *nm, const wc *a, int an, int cur)
{
    int bn = cur_symlen(cur);
    wc *s = amalloc(nm, sizeof(wc) * (size_t)(an + bn + 1));
    if (!s) return NULL;
    memcpy(s, a, sizeof(wc) * (size_t)an);
    memcpy(s + an, CURRENCY[cur].sym, sizeof(wc) * (size_t)bn);
    s[an + bn] = 0;
    return s;
}

static int h_currency_range(anna_norm *nm, int **out, anna_wlist *l)
{
    wc *p0 = nm->p, *e0 = nm->e, *fe0 = nm->fe, *dash, *ebefore, *scratch;
    int kind = 3, cur, kind2 = 0, cur2, r = DECLINE;
    int *left = NULL, *right = NULL;
    anna_wlist w = {NULL, 0, 0};
    cur = cur_match(&nm->p, &nm->e, &kind);
    if (cur < 0) goto done;
    for (dash = nm->p; dash < nm->e && *dash != '-'; dash++) {
    }
    if (!(*dash == '-' && nm->p < dash && dash < nm->e - 1)) {
        r = DECLINE;
        goto done;
    }
    *dash = ' ';
    nm->p = p0;
    nm->e = dash;
    cur2 = cur_match(&nm->p, &nm->e, &kind2);
    if (cur2 >= 0 && cur2 != cur) {
        r = DECLINE;
        goto undo;
    }
    r = parse_number(nm, &left, "NUMBER", 0);
    if (r < 0) goto undo;
    nm->p = dash + 1;
    nm->e = e0;
    cur2 = cur_match(&nm->p, &nm->e, &kind2);
    r = parse_number(nm, &right, "NUMBER", 0);
    if (r < 0) goto undo;
    if (*left == 0x1006 && *right == 0x1006) {
        wl_take(nm, &w, &((anna_numinfo *)left)->words);
    } else {
        nm->p = p0;
        nm->e = dash;
        if (kind == 2) {
            if (cur2 < 0) {
                wc *b = cat_text_sym(nm, nm->p, (int)(nm->e - nm->p), cur);
                if (!b) return NOMEM;
                nm->p = b;
                nm->e = b + wlen(b);
                nm->fe = nm->e;
            } else if (cur2 != cur) {
                r = DECLINE;
                goto undo;
            }
        }
        r = h_currency(nm, &left, &w);
        nm->fe = fe0;
        if (r < 0) goto undo;
    }
    wl_str(nm, &w, "to");
    nm->p = dash + 1;
    nm->e = e0;
    if (kind == 1) {
        int k3 = 3, k = cur_match(&nm->p, &nm->e, &k3);
        if (k < 0) {
            wc *b = cat_text_sym(nm, nm->p, (int)(nm->e - nm->p), cur);
            if (!b) return NOMEM;
            nm->p = b;
            nm->e = b + wlen(b);
            nm->fe = nm->e;
        } else if (k == cur) {
            nm->p = dash + 1;
            nm->e = e0;
        } else {
            r = DECLINE;
            goto undo;
        }
    }
    ebefore = nm->e;
    scratch = nm->p != dash + 1 ? nm->p : NULL;
    r = h_currency(nm, &right, &w);
    if (r >= 0) {
        anna_rangeinfo *ci;
        if (scratch && (nm->e < scratch || nm->e > ebefore)) {
            /* the scale word came from the next fragment while the right side was a scratch copy: the
             * engine adds the difference of two unrelated pointers (garbage length); we end the item at
             * the fragment end, which gives the same words and the same continuation */
            e0 = fe0;
        } else if (ebefore != nm->e) {
            e0 += nm->e - ebefore;
        }
        ci = amalloc(nm, sizeof *ci);
        if (!ci) return NOMEM;
        ci->type = 0x1028;
        ci->left = left;
        ci->right = right;
        wl_cat(nm, l, &w);
        *out = (int *)ci;
    }
undo:
    *dash = '-';
done:
    nm->p = p0;
    nm->e = e0;
    nm->fe = fe0;
    return r;
}

/* ---- <context id="number_*">: roman numerals (0x0D5F181D) ---- */

static int h_roman(anna_norm *nm, int **out, const wc *ctx)
{
    const wc *s = nm->p;
    int n = (int)(nm->e - nm->p), i = 0, j, cnt = 0, val = 0, r;
#define U(k) a_toupper(s[k])
    if (n != 0) {
        while (i < n && U(i) == 'M') {
            if (cnt >= 3) break;
            val += 1000;
            cnt++;
            i++;
        }
        if (cnt > 3) return DECLINE;
        j = i;
        if (i < n) {
            if (U(i) == 'C') {
                val += 100;
                j = i + 1;
                cnt = 1;
                if (j < n) {
                    if (U(j) == 'M') {
                        val += 800;
                        j = i + 2;
                    } else if (U(j) == 'D') {
                        val += 300;
                        j = i + 2;
                    } else {
                        while (j < n && U(j) == 'C' && cnt < 3) {
                            val += 100;
                            cnt++;
                            j++;
                        }
                    }
                }
            } else if (U(i) == 'D') {
                val += 500;
                cnt = 0;
                j = i + 1;
                while (j < n && U(j) == 'C' && cnt < 3) {
                    val += 100;
                    cnt++;
                    j++;
                }
            }
        }
        i = j;
    }
    /* tens */
    cnt = 0;
    j = i;
    if (i < n) {
        if (U(i) == 'X') {
            val += 10;
            j = i + 1;
            cnt = 1;
            if (j < n) {
                if (U(j) == 'C') {
                    val += 80;
                    j = i + 2;
                } else if (U(j) == 'L') {
                    val += 30;
                    j = i + 2;
                } else {
                    while (j < n && U(j) == 'X' && cnt < 3) {
                        val += 10;
                        cnt++;
                        j++;
                    }
                }
            }
        } else if (U(i) == 'L') {
            val += 50;
            j = i + 1;
            while (j < n && U(j) == 'X' && cnt < 3) {
                val += 10;
                cnt++;
                j++;
            }
        }
    }
    i = j;
    /* units */
    cnt = 0;
    if (i < n) {
        if (U(i) == 'I') {
            val += 1;
            j = i + 1;
            cnt = 1;
            if (j < n) {
                if (U(j) == 'X') {
                    val += 8;
                    j = i + 2;
                } else if (U(j) == 'V') {
                    val += 3;
                    j = i + 2;
                } else {
                    while (j < n && U(j) == 'I' && cnt < 3) {
                        val += 1;
                        cnt++;
                        j++;
                    }
                }
            }
            i = j;
        } else if (U(i) == 'V') {
            val += 5;
            j = i + 1;
            while (j < n && U(j) == 'I' && cnt < 3) {
                val += 1;
                cnt++;
                j++;
            }
            i = j;
        }
    }
#undef U
    if (i != n) return DECLINE;
    {
        wc *dst = amalloc(nm, sizeof(wc) * 8), *sp = nm->p, *se = nm->e;
        char tmp[16];
        int k, len;
        anna_ctxinfo *ci;
        int *inner = NULL;
        char mode[64];
        if (!dst) return NOMEM;
        len = 0;
        k = val;
        do {
            tmp[len++] = (char)('0' + k % 10);
            k /= 10;
        } while (k);
        for (k = 0; k < len; k++) dst[k] = (wc)tmp[len - 1 - k];
        dst[len] = 0;
        for (k = 0; ctx[k] && k < 63; k++) mode[k] = (char)ctx[k];
        mode[k] = 0;
        nm->p = dst;
        nm->e = dst + len;
        r = parse_number(nm, &inner, mode, 0);
        nm->p = sp;
        nm->e = se;
        if (r < 0) return r;
        ci = amalloc(nm, sizeof *ci);
        if (!ci) return NOMEM;
        ci->type = 0x1010;
        ci->inner = inner;
        *out = (int *)ci;
    }
    return 0;
}

/* ============================================================================================== */
/* words of a token                                                                               */

/* 0x0D5EB9D1: a mixed token read piece by piece (letters as words, digit runs as numbers, symbols by name) */
static int spell_words(anna_norm *nm, anna_wlist *l)
{
    wc *s = nm->p, *end = nm->e, *q, *save_p = nm->p, *save_e = nm->e;
    int n = (int)(end - s), rep = 0, r = 0;
    wc last = 0;
    if (!wnicmp_a(s, "AT&T", n)) {
        wl_span(nm, l, s, 1);
        wl_span(nm, l, s + 1, 1);
        wl_str(nm, l, "and");
        wl_span(nm, l, s + 3, 1);
        return 0;
    }
    if (n == 3 && !wncmp_a(s, "SR", 2) && s[2] > '0' && s[2] <= '9') {
        wl_span(nm, l, s, 1);
        wl_span(nm, l, s + 1, 1);
        wl_str(nm, l, ONES[s[2] - '0']);
        return 0;
    }
    if (n == 6 && (!wncmp_a(s, "VS.Net", 6) || !wncmp_a(s, "VS.NET", 6))) {
        wl_span(nm, l, s, 1);
        wl_span(nm, l, s + 1, 1);
        wl_str(nm, l, "dot");
        wl_str(nm, l, "net");
        return 0;
    }
    if (n == 2 && (!wncmp_a(s, "C#", 2) || !wncmp_a(s, "J#", 2))) {
        wl_span(nm, l, s, 1);
        wl_str(nm, l, "sharp");
        return 0;
    }
    while (s < end && r >= 0) {
        wc c = *s;
        if (w_isalpha(c)) {
            rep = 0;
            q = s;
            do q++;
            while (q < end && w_isalpha(*q));
            wl_span(nm, l, s, (int)(q - s));
            s = q;
        } else if (a_isdigit(c)) {
            int *ti = NULL;
            rep = 0;
            q = s;
            do q++;
            while (q < end && a_isdigit(*q));
            nm->p = s;
            nm->e = q;
            r = parse_number(nm, &ti, "NUMBER", 0);
            if (r >= 0) wl_take(nm, l, &((anna_numinfo *)ti)->words);
            nm->p = save_p;
            nm->e = save_e;
            s = q;
        } else {
            if (c < 0x101 && SYMNAME[c]) {
                if (rep == 0 || last != c) {
                    rep = 1;
                    last = c;
                } else {
                    rep++;
                }
                if ((open_type(c) == 0x1001 && close_type(c) == 0x1001 && quote_type(c) == 0x1001 &&
                     clause_type(c) == 0x1001) ||
                    clause_type(c) == 14 || (nm->flags & ANNA_NORM_SPEAK_PUNC) || cur_st(nm)->action == 4) {
                    const char *name = SYMNAME[c];
                    if (c == '#' && s + 1 < end && w_isdigit(s[1])) wl_str(nm, l, "number"); /* "#1": number one */
                    else wl_words(nm, l, name);
                }
            }
            s++;
        }
    }
    return r == DECLINE ? 0 : r;
}

/* 0x0D5EB8F1: a punctuation mark read aloud (SPF_NLP_SPEAK_PUNC or <spell>) */
static void punct_words(anna_norm *nm, wc c, anna_wlist *l)
{
    if (c == '.') wl_str(nm, l, "period");
    else if (c < 0x100 && SYMNAME[c]) wl_words(nm, l, SYMNAME[c]);
}

static void hyphen_words(anna_norm *nm, anna_hypheninfo *h, anna_wlist *l) /* 0x0D5EB79D */
{
    if (*h->left == 0x1002) wl_span(nm, l, h->ls, (int)(h->rs - h->ls) - 1);
    else wl_take(nm, l, &((anna_numinfo *)h->left)->words);
    if (*h->right == 0x1002) wl_span(nm, l, h->rs, (int)(nm->e - h->rs));
    else if (*h->right == 0x101b) hyphen_words(nm, (anna_hypheninfo *)h->right, l);
    else wl_take(nm, l, &((anna_numinfo *)h->right)->words);
}

static void degree_words(anna_norm *nm, anna_numinfo *ni, anna_wlist *l) /* 0x0D5F0384 */
{
    const char *w = "degree";
    wl_take(nm, l, &ni->words);
    if (ni->dp || ni->fr || !ni->ip || ni->ip->end - ni->ip->start != 1 || ni->ip->start[0] != '1') {
        w = "degrees";
        if (!ni->ip && ni->fr && ni->fr->over == 0) {
            wl_str(nm, l, "of");
            wl_str(nm, l, "a");
            w = "degree";
        }
    }
    wl_str(nm, l, w);
}

static void num_type_words(anna_norm *nm, int *ti, anna_wlist *l)
{
    anna_numinfo *ni = (anna_numinfo *)ti;
    switch (*ti) {
    case 0x1009: wl_take(nm, l, &ni->words); wl_str(nm, l, "percent"); break;
    case 0x100a: degree_words(nm, ni, l); break;
    case 0x100b: wl_take(nm, l, &ni->words); wl_str(nm, l, "squared"); break;
    case 0x100c: wl_take(nm, l, &ni->words); wl_str(nm, l, "cubed"); break;
    case 0x1014: {
        const anna_yearinfo *y = (const anna_yearinfo *)ti;
        year_words(nm, y->s, y->len, l);
        break;
    }
    default: wl_take(nm, l, &ni->words); break;
    }
}

/* 0x0D5EAD92: words of a classified token */
static int type_words(anna_norm *nm, int *ti, anna_wlist *l)
{
    switch (*ti) {
    case 0x1006: case 0x1007: case 0x1008: case 0x100e: case 0x100f:
    case 0x1009: case 0x100a: case 0x100b: case 0x100c: case 0x1014:
        num_type_words(nm, ti, l);
        return 0;
    case 0x1010: {
        int *in = (int *)((anna_ctxinfo *)ti)->inner;
        num_type_words(nm, in, l);
        return 0;
    }
    case 0x1013: zip_words(nm, (const anna_zipinfo *)ti, l); return 0;
    case 0x1015: date_words(nm, (const anna_dateinfo *)ti, l); return 0;
    case 0x1017: decade_words(nm, (const anna_decadeinfo *)ti, l); return 0;
    case 0x1019: duration_words(nm, (anna_durinfo *)ti, l); return 0;
    case 0x101a: { /* 0x0D5EBEDC: <spell> text: every character with a name */
        wc *s;
        for (s = nm->p; s < nm->e; s++)
            if (*s < 0x101 && SYMNAME[*s]) wl_words(nm, l, SYMNAME[*s]);
        return 0;
    }
    case 0x101b: hyphen_words(nm, (anna_hypheninfo *)ti, l); return 0;
    case 0x101e: { /* 0x0D5F0E3A */
        anna_rangeinfo *ri = (anna_rangeinfo *)ti;
        num_type_words(nm, (int *)ri->left, l);
        wl_str(nm, l, "to");
        num_type_words(nm, (int *)ri->right, l);
        return 0;
    }
    case 0x1029: { /* 0x0D5EB875 */
        const anna_dashinfo *d = (const anna_dashinfo *)ti;
        int k;
        for (k = 0; k < d->len; k++)
            if (SYMNAME[d->s[k]]) wl_str(nm, l, SYMNAME[d->s[k]]);
        return 0;
    }
    default:
        return spell_words(nm, l);
    }
}

/* ============================================================================================== */
/* dispatcher (0x0D5EA96F)                                                                        */

static int dispatch(anna_norm *nm, int **ti, anna_wlist *direct)
{
    const wc *ctx = nm->fi >= 0 ? nm->fr[nm->fi].st.ctx : NULL;
    int r;
    if (!nm->p) return DECLINE;
    if (ctx) {
        char mode[64];
        int k;
        for (k = 0; ctx[k] && k < 63; k++) mode[k] = (char)(ctx[k] < 0x80 ? ctx[k] : '?');
        mode[k] = 0;
        r = DECLINE;
        if (!wicmp_a(ctx, "ADDRESS")) {
            r = h_zip(nm, ti);
        } else if (!wnicmp_a(ctx, "DATE", 4)) {
            r = h_date_num(nm, ti, ctx);
            if (r == DECLINE) r = h_date_name(nm, ti, ctx);
        } else if (!wnicmp_a(ctx, "TIME", 4)) {
            r = h_duration(nm, ti, ctx);
        } else if (!wnicmp_a(ctx, "NUM", 3)) {
            r = h_num(nm, ti, mode);
            if (r == DECLINE) r = h_roman(nm, ti, ctx);
        } else if (!wicmp_a(ctx, "PHONE_NUMBER")) {
            r = h_phone(nm, ti, direct, ctx);
        }
        if (r != DECLINE) return r;
    }
    if (h_word(nm->p, nm->e) == 0) {
        int *t = type_rec(nm, 0x1002);
        if (!t) return NOMEM;
        *ti = t;
        r = h_date_mdy_words(nm, ti, direct);
        if (r != DECLINE) return r;
        r = h_date_dmy_words(nm, ti, direct);
        if (r != DECLINE) return r;
        r = h_state_zip(nm, ti, direct);
        if (r != DECLINE) return r;
        r = h_currency(nm, ti, direct);
        if (r != DECLINE) return r;
        *ti = t;
        return 0;
    }
    r = h_date_mdy_words(nm, ti, direct);
    if (r != DECLINE) return r;
    r = h_date_dmy_words(nm, ti, direct);
    if (r != DECLINE) return r;
    r = h_currency(nm, ti, direct);
    if (r != DECLINE) return r;
    r = h_time_range(nm, ti, direct);
    if (r != DECLINE) return r;
    r = h_clock(nm, ti, direct, 1);
    if (r != DECLINE) return r;
    r = h_phone(nm, ti, direct, NULL);
    if (r != DECLINE) return r;
    r = h_num(nm, ti, NULL);
    if (r != DECLINE) return r;
    r = h_range(nm, ti);
    if (r != DECLINE) return r;
    r = h_currency_range(nm, ti, direct);
    if (r != DECLINE) return r;
    r = h_date_num(nm, ti, NULL);
    if (r != DECLINE) return r;
    r = h_date_name(nm, ti, NULL);
    if (r != DECLINE) return r;
    r = h_decade(nm, ti);
    if (r != DECLINE) return r;
    r = h_duration(nm, ti, NULL);
    if (r != DECLINE) return r;
    r = h_hyphen(nm, nm->p, nm->e, ti);
    if (r != DECLINE) return r;
    r = h_dashword(nm, nm->p, nm->e, ti);
    if (r != DECLINE) return r;
    if (!*ti) {
        int *t = type_rec(nm, 0x1001);
        if (!t) return NOMEM;
        *ti = t;
    }
    return 0;
}

/* 0x0D5EB0A1: classify the core token and build its words */
static int classify(anna_norm *nm, aitem *core)
{
    int *ti = core->ti, r = 0, fi0 = nm->fi;
    anna_wlist direct = {NULL, 0, 0};
    /* the engine takes the fragment (state, offset base) before dispatching; a failed lookahead at the end
     * of the text can leave this+0x40 empty */
    if (cur_st(nm)->action != 0) { /* <spell> text */
        ti = type_rec(nm, 0x101a);
        if (!ti) return NOMEM;
    } else if (!ti || (*ti != 0x1003 && *ti != 0x1005)) {
        r = dispatch(nm, &ti, &direct);
        if (r < 0) return r;
    }
    switch (*ti) {
    case 0x1003: case 0x1004: case 0x1005:
        return 0;
    case 0x1002:
        core->t = nm->p;
        core->len = (int)(nm->e - nm->p);
        core->ofs = src_ofs_fi(nm, fi0, nm->p);
        core->words.n = 0;
        wl_span_st(nm, &core->words, nm->p, core->len, frag_st(nm, fi0));
        core->pos = 0;
        core->ti = ti;
        return 0;
    case 0x1027:
        if (core->prev && *core->prev->ti == 1 && ((anna_phoneinfo *)ti)->area) {
            /* "(425) 555-0100": the parenthesis belongs to the number */
            il_remove(&nm->items, core->prev);
            nm->p--;
        }
        /* fall through */
    case 0x100d: case 0x1016: case 0x1018: case 0x101c: case 0x101d: case 0x1028:
        core->t = nm->p;
        core->len = (int)(nm->e - nm->p);
        core->ofs = src_ofs_fi(nm, fi0, nm->p);
        core->words = direct;
        core->pos = 0;
        core->ti = ti;
        return 0;
    default: {
        anna_wlist w = {NULL, 0, 0};
        core->t = nm->p;
        core->len = (int)(nm->e - nm->p);
        core->ofs = src_ofs(nm, nm->p);
        r = type_words(nm, ti, &w);
        if (r < 0) return r;
        core->words = w;
        core->pos = 0;
        core->ti = ti;
        return 0;
    }
    }
}

/* ============================================================================================== */
/* tokenizer (0x0D5F469F)                                                                         */

/* 0x0D5EAF9F: fold a token through Windows-1252 and the table @0x0D5D8100 */
static void fold(wc *p, wc *e)
{
    for (; p < e; p++) {
        wc c = *p;
        int b;
        if (c < 0x80 || (c >= 0xa0 && c <= 0xff) || c == 0x81 || c == 0x8d || c == 0x8f || c == 0x90 || c == 0x9d) {
            b = c;
        } else {
            static const struct {
                wc u;
                unsigned char b;
            } M[] = {{0x20ac, 0x80}, {0x201a, 0x82}, {0x0192, 0x83}, {0x201e, 0x84}, {0x2026, 0x85}, {0x2020, 0x86},
                     {0x2021, 0x87}, {0x02c6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8a}, {0x2039, 0x8b}, {0x0152, 0x8c},
                     {0x017d, 0x8e}, {0x2018, 0x91}, {0x2019, 0x92}, {0x201c, 0x93}, {0x201d, 0x94}, {0x2022, 0x95},
                     {0x2013, 0x96}, {0x2014, 0x97}, {0x02dc, 0x98}, {0x2122, 0x99}, {0x0161, 0x9a}, {0x203a, 0x9b},
                     {0x0153, 0x9c}, {0x017e, 0x9e}, {0x0178, 0x9f}};
            size_t k;
            b = 0;
            for (k = 0; k < sizeof M / sizeof *M; k++)
                if (M[k].u == c) {
                    b = M[k].b;
                    break;
                }
            if (!b && c >= 0x100) {
                int lo = 0, hi = (int)(sizeof BESTFIT / sizeof *BESTFIT) - 1;
                while (lo <= hi) {
                    int mid = (lo + hi) / 2;
                    if (BESTFIT[mid][0] == c) {
                        b = BESTFIT[mid][1];
                        break;
                    }
                    if (BESTFIT[mid][0] < c) lo = mid + 1;
                    else hi = mid - 1;
                }
            }
        }
        *p = FOLD[b & 0xff];
    }
}

static aitem *punct_item(anna_norm *nm, int t, wc *at, int len, int speak)
{
    aitem *it = item_new(nm);
    anna_nword *w;
    if (!it) return NULL;
    it->t = at;
    it->len = len;
    it->ofs = src_ofs(nm, at);
    it->pos = punct_pos(t);
    it->ti = type_rec(nm, t);
    if (!it->ti) return NULL;
    w = wl_new(nm, &it->words);
    if (!w) return NULL;
    w->st = cur_st(nm);
    w->pos = it->pos;
    if (speak) {
        it->words.n = 0;
        punct_words(nm, at[len - 1 < 0 ? 0 : 0], &it->words);
        *it->ti = 0x1001;
    }
    return it;
}

static int next_token(anna_norm *nm, int *sent_end)
{
    wc *te;
    aitem *core;
    int clause = 0, ntrail = 0, more = 1, brk = 0, r = 0, speak;
    *sent_end = 0;
    r = adv(nm, &nm->p, &nm->fe, &nm->fi, &nm->items);
    if (r < 0) return r;
    if (!nm->p) return 0;
    te = token_end(nm->p, nm->fe);
    nm->te = te;
    core = item_new(nm);
    if (!core) return NOMEM;
    il_append(&nm->items, core);
    if (nm->userlex && nm->userlex(nm->userlex_ctx, nm->p, (int)(te - nm->p))) {
        core->t = nm->p;
        core->len = (int)(te - nm->p);
        core->ofs = src_ofs(nm, nm->p);
        wl_span(nm, &core->words, nm->p, core->len);
        core->ti = type_rec(nm, 0x1002);
        if (!core->ti) return NOMEM;
        nm->p = te;
        return 0;
    }
    fold(nm->p, te);
    te = token_end(nm->p, nm->fe);
    nm->te = te;
    speak = (nm->flags & ANNA_NORM_SPEAK_PUNC) || cur_st(nm)->action == 4;
    while (nm->p < te) {
        int t = open_type(*nm->p);
        aitem *it;
        if (t == 0x1001) t = quote_type(*nm->p);
        if (t == 0x1001) break;
        it = punct_item(nm, t, nm->p, 1, 0);
        if (!it) return NOMEM;
        if (speak) {
            it->words.n = 0;
            punct_words(nm, *nm->p, &it->words);
            *it->ti = 0x1001;
        }
        il_insert_before(&nm->items, core, it);
        nm->p++;
    }
    nm->e = te;
    while (r >= 0) {
        wc c;
        int t;
        if (nm->e - 1 < nm->p || !more) break;
        more = 0;
        brk = 0;
        c = nm->e[-1];
        t = close_type(c);
        if (t == 0x1001) t = quote_type(c);
        if (t == 0x1001) t = clause_type(c);
        if (t != 0x1001) {
            more = 1;
            if (t == 12 || t == 14 || t == 13) clause = 1;
            goto emit;
        }
        t = sent_type(c);
        if (t == 0x1001) continue;
        if (t == 9) {
            if (nm->p <= nm->e - 2 && w_isalpha(nm->e[-2])) {
                r = h_initials(nm, core, sent_end);
                if (r < 0) {
                    int a;
                    if (r != DECLINE) return r;
                    a = abbrev_find(nm->p, (int)(nm->e - 1 - nm->p));
                    if (a < 0) {
                        clause = 0;
                    } else if (ABBREVS[a].shandler < 0) {
                        *sent_end = 0;
                        r = make_abbrev(nm, core, a);
                    } else {
                        r = abbrev_sentence(nm, ABBREVS[a].shandler, a, core, sent_end);
                        if (r >= 0 && *sent_end) {
                            if (!clause) {
                                more = 1;
                                brk = 1;
                            } else {
                                *sent_end = 0;
                            }
                        }
                    }
                    if (r == DECLINE) {
                        wc *x;
                        for (x = nm->p; x < nm->e - 1; x++)
                            if (*x == '.') {
                                *sent_end = 0;
                                break;
                            }
                        if (x == nm->e - 1 && !clause) {
                            r = 0;
                            more = 1;
                            *sent_end = 1;
                            t = 9;
                            goto emit;
                        }
                        r = 0;
                    }
                    if (r < 0) return r;
                    if (more) {
                        t = 9;
                        goto emit;
                    }
                } else if (*sent_end) {
                    if (!clause) {
                        more = 1;
                        brk = 1;
                        t = 9;
                        goto emit;
                    }
                    *sent_end = 0;
                }
                continue;
            }
            if (nm->e == nm->te && nm->p <= nm->e - 2 && sent_type(nm->e[-2]) == 9 && nm->e - 3 == nm->p &&
                sent_type(nm->e[-3]) == 9) {
                more = 1;
                t = 16;
                goto emit;
            }
            t = 9;
        }
        more = 1;
        *sent_end = 1;
    emit:
        ntrail++;
        {
            aitem *it = punct_item(nm, t, t == 16 ? nm->e - 3 : nm->e - 1, t == 16 ? 3 : 1, 0);
            if (!it) return NOMEM;
            if ((nm->flags & ANNA_NORM_SPEAK_PUNC) || (cur_st(nm)->action == 4 && !brk)) {
                it->words.n = 0;
                punct_words(nm, nm->e[-1], &it->words);
                *it->ti = 0x1001;
            }
            il_insert_after(&nm->items, core, it);
        }
        if (brk) break;
        if (t == 16) {
            nm->e -= 3;
            ntrail = 3;
            continue;
        }
        nm->e -= 1;
    }
    if (r < 0) return r;
    if (nm->p == nm->e) {
        il_remove(&nm->items, core);
    } else {
        r = classify(nm, core);
        if (r < 0) return r;
    }
    if (!brk && nm->e + ntrail != nm->te) {
        aitem *it = core->next;
        nm->p = nm->e;
        while (it) {
            aitem *nx = it->next;
            il_remove(&nm->items, it);
            it = nx;
        }
        return 0;
    }
    nm->p = nm->te;
    return 0;
}

/* ============================================================================================== */
/* sentences (0x0D5F5285)                                                                         */

static void put_utf8(char *out, int cap, const wc *s, const char *a, int len)
{
    int i, k = 0;
    for (i = 0; i < len; i++) {
        unsigned c = s ? s[i] : (unsigned char)a[i];
        if (c < 0x80) {
            if (k + 1 >= cap) break;
            out[k++] = (char)c;
        } else if (c < 0x800) {
            if (k + 2 >= cap) break;
            out[k++] = (char)(0xc0 | (c >> 6));
            out[k++] = (char)(0x80 | (c & 0x3f));
        } else {
            if (k + 3 >= cap) break;
            out[k++] = (char)(0xe0 | (c >> 12));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[k++] = (char)(0x80 | (c & 0x3f));
        }
    }
    out[k] = 0;
}

static void put_w(uint16_t *out, const wc *s, const char *a, int len)
{
    int i;
    for (i = 0; i < len && i < 127; i++) out[i] = s ? s[i] : (uint16_t)(unsigned char)a[i];
    out[i] = 0;
}

static int emit_sentence(anna_norm *nm)
{
    aitem *it;
    int idx = 0;
    nm->ntok = 0;
    for (it = nm->items.head; it; it = it->next, idx++) {
        int k;
        for (k = 0; k < it->words.n; k++) {
            const anna_nword *w = &it->words.w[k];
            const anna_vstate *st = w->st ? w->st : &DEFAULT_STATE;
            anna_token *t;
            int type = it->ti ? *it->ti : -1;
            if (nm->ntok == nm->tokcap) {
                int cap = nm->tokcap ? nm->tokcap * 2 : 64;
                anna_token *x = realloc(nm->tok, sizeof(anna_token) * (size_t)cap);
                if (!x) return -1;
                nm->tok = x;
                nm->tokcap = cap;
            }
            t = &nm->tok[nm->ntok++];
            memset(t, 0, sizeof *t);
            if (w->t || w->s) {
                t->wlen = w->len;
                put_w(t->wtext, w->t, w->s, w->len);
                put_utf8(t->text, (int)sizeof t->text, w->t, w->s, w->len > 127 ? 127 : w->len);
            } else {
                t->wlen = -1;
            }
            t->tok_type = type;
            t->punct = type >= 1 && type <= 16 ? type : 0;
            if (type == 0x1000) {
                switch (st->action) {
                case 1: t->kind = 2; break;
                case 2: t->kind = 4; break;
                case 3: t->kind = 3; break;
                default: t->kind = 5; break;
                }
            } else {
                t->kind = t->punct ? 1 : 0;
            }
            t->action = st->action;
            t->pos_hint = st->pos ? st->pos : -1;
            t->pron = NULL;
            t->phone_ids = st->phones;
            t->silence_ms = st->sil;
            t->st.volume = st->vol;
            t->st.rate = st->rate;
            t->st.pitch = st->pitch;
            t->st.emph = st->emph;
            t->pitch_range = st->range;
            t->word_pos = w->pos;
            t->item = idx;
            t->item_nwords = it->words.n;
            t->item_pos = it->pos;
            t->item_len = it->t || it->s ? it->len : -1;
            put_w(t->item_text, it->t, it->s, it->t || it->s ? it->len : 0);
            t->src_pos = it->ofs;
            t->src_len = it->len;
            t->flags = k == 0 ? ANNA_TOK_FIRST : 0;
            /* the engine text is followed by a NUL: static strings (except inner pieces of a multi-word
             * name) and spans that end at a NUL */
            if ((w->s && !w->midword) || (w->t && w->t[w->len] == 0)) t->flags |= ANNA_TOK_NUL_AFTER;
            t->abbrev = (type == 0x1003 || type == 0x1004) ? ((const anna_abbrevinfo *)it->ti)->index : -1;
            t->ti = it->ti;
        }
    }
    return 0;
}

int anna_norm_sentence(anna_norm *nm, const anna_token **toks, int *ntok)
{
    int count = 0, sent_end = 0, r, last;
    if (toks) *toks = NULL;
    if (ntok) *ntok = 0;
    if (nm->fi < 0) { /* the next SAPI Speak() call of the text (after <voice>/<lang>) */
        int g;
        if (nm->gnext < 0 || nm->gnext >= nm->nfr) return 0;
        nm->fi = nm->gnext;
        g = nm->fr[nm->fi].group;
        nm->gnext = nm->fi;
        while (nm->gnext < nm->nfr && nm->fr[nm->gnext].group == g) nm->gnext++;
        nm->p = nm->fr[nm->fi].t;
        nm->fe = nm->p ? nm->p + nm->fr[nm->fi].len : NULL;
    }
    arena_reset(nm);
    nm->items.head = nm->items.tail = NULL;
    last = nm->fi;
    while (nm->fi >= 0 && !sent_end && count < 50) {
        int advance = 0;
        count++;
        if (nm->fr[nm->fi].st.action == 0 || nm->fr[nm->fi].st.action == 4) {
            r = next_token(nm, &sent_end);
            if (r < 0) return -1;
            if (nm->p && nm->fe && nm->fe <= nm->p) advance = 1;
        } else {
            aitem *it = special_item(nm, nm->fi);
            if (!it) return -1;
            il_append(&nm->items, it);
            advance = 1;
        }
        if (advance && nm->fi >= 0) {
            last = nm->fi;
            nm->fi = next_frag(nm, nm->fi);
            if (nm->fi >= 0) {
                nm->p = nm->fr[nm->fi].t;
                nm->fe = nm->p + nm->fr[nm->fi].len;
            } else {
                nm->p = nm->fe = NULL;
            }
        }
    }
    if (!sent_end) { /* no end mark: a period is added */
        aitem *it = item_new(nm);
        anna_nword *w;
        if (!it) return -1;
        it->s = ".";
        it->len = 1;
        it->ofs = last >= 0 ? nm->fr[last].ofs + nm->fr[last].len : nm->n;
        it->pos = 0x400e;
        it->ti = type_rec(nm, 9);
        w = wl_new(nm, &it->words);
        if (!w || !it->ti) return -1;
        w->st = &DEFAULT_STATE;
        w->pos = 0x400e;
        il_append(&nm->items, it);
    }
    if (emit_sentence(nm)) return -1;
    if (toks) *toks = nm->tok;
    if (ntok) *ntok = nm->ntok;
    return 1;
}

/* ============================================================================================== */
/* input: plain text or SAPI XML -> SPVTEXTFRAG list                                              */

static int add_frag(anna_norm *nm, const anna_vstate *st, int start, int len, int ofs)
{
    afrag *f;
    afrag *x = realloc(nm->fr, sizeof(afrag) * (size_t)(nm->nfr + 1));
    if (!x) return -1;
    nm->fr = x;
    f = &nm->fr[nm->nfr++];
    f->st = *st;
    f->t = nm->buf + start;
    f->len = len;
    f->ofs = ofs;
    f->group = nm->nfr > 1 ? nm->fr[nm->nfr - 2].group + (nm->group_next ? 1 : 0) : 0;
    nm->group_next = 0;
    return 0;
}

#include "anna_sapixml.h"

/* text buffer with a 0 in front: the duration handler (like the engine) may look one character before
 * the token ("::"), and 0-terminated with a spare 0 behind */
static wc *walloc_guarded(int len)
{
    wc *b = malloc(sizeof(wc) * ((size_t)len + 3));
    if (!b) return NULL;
    b[0] = 0;
    return b + 1;
}

static anna_norm *norm_new(wc *buf, int n, int flags)
{
    anna_norm *nm = calloc(1, sizeof *nm);
    if (!nm) {
        free(buf - 1);
        return NULL;
    }
    nm->buf = buf;
    nm->n = n;
    nm->flags = flags;
    if (flags & ANNA_NORM_XML) {
        if (sapi_xml_parse(nm) < 0) {
            anna_norm_free(nm);
            return NULL;
        }
    } else if (n > 0) {
        if (add_frag(nm, &DEFAULT_STATE, 0, n, 0) < 0) {
            anna_norm_free(nm);
            return NULL;
        }
    }
    nm->fi = nm->nfr > 0 ? 0 : -1;
    nm->gnext = 0;
    while (nm->gnext < nm->nfr && nm->fr[nm->gnext].group == 0) nm->gnext++;
    if (nm->fi >= 0) {
        nm->p = nm->fr[0].t;
        nm->fe = nm->p + nm->fr[0].len;
        if (nm->fr[0].st.action != 0 && nm->fr[0].st.action != 4) nm->p = nm->fr[0].t;
    }
    return nm;
}

anna_norm *anna_norm_new_w(const uint16_t *text, int len, int flags)
{
    wc *buf;
    if (len < 0) {
        len = 0;
        while (text[len]) len++;
    }
    buf = walloc_guarded(len);
    if (!buf) return NULL;
    memcpy(buf, text, sizeof(wc) * (size_t)len);
    buf[len] = 0;
    buf[len + 1] = 0;
    return norm_new(buf, len, flags);
}

anna_norm *anna_norm_new(const char *utf8, int flags)
{
    size_t i = 0, len = strlen(utf8);
    int n = 0;
    wc *buf = walloc_guarded((int)len);
    if (!buf) return NULL;
    while (i < len) { /* UTF-8 -> UTF-16 (invalid bytes read as Windows-1252) */
        unsigned c = (unsigned char)utf8[i], cp = c;
        int extra = 0;
        if (c >= 0xf0) extra = 3, cp = c & 7;
        else if (c >= 0xe0) extra = 2, cp = c & 15;
        else if (c >= 0xc0) extra = 1, cp = c & 31;
        if (extra && i + (size_t)extra < len + 1) {
            int k, ok = 1;
            for (k = 1; k <= extra; k++) {
                unsigned d = i + (size_t)k < len ? (unsigned char)utf8[i + (size_t)k] : 0;
                if ((d & 0xc0) != 0x80) ok = 0;
                else cp = (cp << 6) | (d & 0x3f);
            }
            if (ok) {
                i += (size_t)extra + 1;
                if (cp > 0xffff) { /* surrogate pair */
                    cp -= 0x10000;
                    buf[n++] = (wc)(0xd800 + (cp >> 10));
                    buf[n++] = (wc)(0xdc00 + (cp & 0x3ff));
                } else {
                    buf[n++] = (wc)cp;
                }
                continue;
            }
        }
        {
            static const wc C1[32] = {0x20ac, 0x81, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030,
                                      0x160, 0x2039, 0x152, 0x8d, 0x17d, 0x8f, 0x90, 0x2018, 0x2019, 0x201c,
                                      0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0x9d,
                                      0x17e, 0x178};
            buf[n++] = c >= 0x80 && c < 0xa0 ? C1[c - 0x80] : (wc)c;
        }
        i++;
    }
    buf[n] = 0;
    buf[n + 1] = 0;
    return norm_new(buf, n, flags);
}

void anna_norm_set_userlex(anna_norm *nm, int (*in_user_lex)(void *ctx, const uint16_t *w, int len), void *ctx)
{
    nm->userlex = in_user_lex;
    nm->userlex_ctx = ctx;
}

void anna_norm_free(anna_norm *nm)
{
    if (!nm) return;
    arena_reset(nm);
    sapi_xml_free(nm);
    free(nm->fr);
    free(nm->tok);
    if (nm->buf) free(nm->buf - 1);
    free(nm);
}
