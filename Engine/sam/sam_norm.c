/* Text normalizer, ported from spttseng.dll (see sam_norm.h for the entry points).
 *
 * The engine works on UTF-16 text in place; this port keeps that: the input is converted to a
 * NUL-terminated uint16_t buffer and the handlers walk it with pointers like the original. Word
 * lists hold either spans of the buffer or static table strings. Everything allocated while
 * building a sentence lives in an arena that is reset for the next sentence (the engine's
 * per-sentence heap, FUN_5ed4651d). Handler return codes: 0 ok, DECLINE (E_INVALIDARG: "not mine,
 * try the next handler"). */
#include "sam_norm.h"
#include "sam_norm_types.h"

#include <stdlib.h>
#include <string.h>


#define DECLINE (-1)

/* ============================================================================================== */
/* character classes (MSVC "C" locale)                                                            */

static int is_ws(wc c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0x200b; } /* FUN_5ed54a9b */
static int w_isalpha(wc c) /* iswctype(c, 0x103), wide table */
{
    if (c < 0x80) return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (c == 0xaa || c == 0xb5 || c == 0xba) return 1;
    return c >= 0xc0 && c <= 0xff && c != 0xd7 && c != 0xf7;
}
static int w_isupper(wc c) { return (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7); } /* FUN_5ed5a4d8 */
static int n_isdigit(wc c) { return c >= '0' && c <= '9'; }                                          /* FUN_5ed5a79a */
static int n_isalpha(wc c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }              /* FUN_5ed5a70d */

static wc n_toupper(wc c) { return c >= 'a' && c <= 'z' ? (wc)(c - 32) : c; }                      /* FUN_5ed5b0a4 */
static wc w_tolower(wc c) { return c >= 'A' && c <= 'Z' ? (wc)(c + 32) : c; }

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
        int x = w_tolower(a[i]), y = w_tolower((wc)(unsigned char)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}

/* _wcsncmp against an ASCII string */
static int wncmp_a(const wc *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int x = a[i], y = (unsigned char)b[i];
        if (x != y || !x) return x - y;
    }
    return 0;
}

/* _wcsicmp of a span (terminated at len) against an ASCII string */
static int wspan_icmp_a(const wc *a, int len, const char *b)
{
    int i;
    for (i = 0;; i++) {
        int x = i < len ? w_tolower(a[i]) : 0, y = w_tolower((wc)(unsigned char)b[i]);
        if (x != y || !x) return x - y;
    }
}

/* the engine's punctuation classes; 0x1001 = none */
static int open_type(wc c) { return c == '(' ? 1 : c == '[' ? 2 : c == '{' ? 3 : 0x1001; }        /* FUN_5ed54ad0 */
static int close_type(wc c) { return c == ')' ? 4 : c == ']' ? 5 : c == '}' ? 6 : 0x1001; }       /* FUN_5ed47ed1 */
static int quote_type(wc c) { return c == '\'' ? 7 : c == '"' ? 8 : 0x1001; }                     /* FUN_5ed47f09 */
static int clause_type(wc c) { return c == ',' ? 0xc : c == ';' ? 0xd : c == ':' ? 0xe : c == '-' ? 0xf : 0x1001; } /* 47f6e */
static int sent_type(wc c) { return c == '.' ? 9 : c == '!' ? 0xa : c == '?' ? 0xb : 0x1001; }    /* FUN_5ed47f36 */
static int is_upper_ascii(wc c) { return c > 0x40 && c < 0x5b; }                                  /* FUN_5ed4679d */

static uint32_t punct_pos(int t) /* FUN_5ed54a2f */
{
    switch (t) {
    case 1: case 2: case 3: return 0x400c;
    case 4: case 5: case 6: return 0x400d;
    case 7: case 8: return 0x4010;
    case 9: case 10: case 11: return 0x400e;
    case 0xc: case 0xd: case 0xe: case 0xf: case 0x10: return 0x400f;
    default: return 0;
    }
}

/* ============================================================================================== */
/* cp1252 folding of a token (FUN_5ed4c768 + table @0x5ed36600)                                   */

static const unsigned char FOLD[256] = {
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x20,
    0x80, 0x20, 0x27, 0x20, 0x22, 0x2c, 0x20, 0x20, 0x5e, 0x89, 0x53, 0x27, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x27, 0x27, 0x22, 0x22, 0x20, 0x2d, 0x2d, 0x7e, 0x99, 0x73, 0x27, 0x20, 0x20, 0x20, 0x59,
    0x20, 0x20, 0xa2, 0xa3, 0x20, 0xa5, 0x7c, 0x20, 0x20, 0xa9, 0x20, 0x22, 0x20, 0x2d, 0xae, 0x20,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0x20, 0x20, 0x20, 0x20, 0xb9, 0x20, 0x22, 0xbc, 0xbd, 0xbe, 0x20,
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x43, 0x45, 0x45, 0x45, 0x45, 0x49, 0x49, 0x49, 0x49,
    0x20, 0x4e, 0x4f, 0x4f, 0x4f, 0x4f, 0x4f, 0x20, 0x4f, 0x55, 0x55, 0x55, 0x55, 0x59, 0x20, 0xdf,
    0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x63, 0x65, 0x65, 0x65, 0x65, 0x69, 0x69, 0x69, 0x69,
    0x75, 0x6e, 0x6f, 0x6f, 0x6f, 0x6f, 0x6f, 0xf7, 0x6f, 0x76, 0x76, 0x76, 0x76, 0x79, 0x20, 0x79};

static int to_cp1252(wc c)
{
    static const struct {
        wc u;
        unsigned char b;
    } M[] = {{0x20ac, 0x80}, {0x201a, 0x82}, {0x0192, 0x83}, {0x201e, 0x84}, {0x2026, 0x85}, {0x2020, 0x86},
             {0x2021, 0x87}, {0x02c6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8a}, {0x2039, 0x8b}, {0x0152, 0x8c},
             {0x017d, 0x8e}, {0x2018, 0x91}, {0x2019, 0x92}, {0x201c, 0x93}, {0x201d, 0x94}, {0x2022, 0x95},
             {0x2013, 0x96}, {0x2014, 0x97}, {0x02dc, 0x98}, {0x2122, 0x99}, {0x0161, 0x9a}, {0x203a, 0x9b},
             {0x0153, 0x9c}, {0x017e, 0x9e}, {0x0178, 0x9f}};
    size_t k;
    if (c < 0x80 || (c >= 0xa0 && c <= 0xff)) return c;
    if (c == 0x81 || c == 0x8d || c == 0x8f || c == 0x90 || c == 0x9d) return c;
    for (k = 0; k < sizeof M / sizeof *M; k++)
        if (M[k].u == c) return M[k].b;
    return 0; /* no mapping: the default character "" */
}

/* ============================================================================================== */
/* tables                                                                                         */

#include "sam_abbrev_tab.h"

/* spoken names of characters (@0x5ed36e10, 0x101 entries) */
static const char *symbol_name(int c)
{
    static const char *const LOW[0x7f] = {
        [0x21] = "exclamation point", [0x22] = "double quote", [0x23] = "number sign", [0x24] = "dollars",
        [0x25] = "percent", [0x26] = "and", [0x27] = "single quote", [0x28] = "left parenthesis",
        [0x29] = "right parenthesis", [0x2a] = "asterisk", [0x2b] = "plus", [0x2c] = "comma", [0x2d] = "hyphen",
        [0x2e] = "dot", [0x2f] = "slash", [0x30] = "zero", [0x31] = "one", [0x32] = "two", [0x33] = "three",
        [0x34] = "four", [0x35] = "five", [0x36] = "six", [0x37] = "seven", [0x38] = "eight", [0x39] = "nine",
        [0x3a] = "colon", [0x3b] = "semicolon", [0x3c] = "less than", [0x3d] = "equals", [0x3e] = "greater than",
        [0x3f] = "question mark", [0x40] = "at", [0x5b] = "left square bracket", [0x5c] = "backslash",
        [0x5d] = "right square bracket", [0x5e] = "circumflex accent", [0x5f] = "underscore",
        [0x60] = "grave accent", [0x7b] = "left curly bracket", [0x7c] = "vertical line",
        [0x7d] = "right curly bracket", [0x7e] = "tilde"};
    static const char *const LETTERS[26] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
                                             "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c >= 'a' && c <= 'z') return LETTERS[c - 'a'];
    if (c < 0x7f) return LOW[c];
    switch (c) {
    case 0x80: return "euros";
    case 0x89: return "per thousand";
    case 0x99: return "trademark";
    case 0xa2: return "cents";
    case 0xa3: return "pounds";
    case 0xa5: return "yen";
    case 0xa9: return "copyright";
    case 0xae: return "registered trademark";
    case 0xb0: return "degrees";
    case 0xb1: return "plus minus";
    case 0xb2: return "superscript two";
    case 0xb3: return "superscript three";
    case 0xb4: return "prime";
    case 0xb7: return "times";
    case 0xb9: return "superscript one";
    case 0xbc: return "one fourth";
    case 0xbd: return "one half";
    case 0xbe: return "three fourths";
    case 0xdf: return "beta";
    case 0xf7: return "divided by";
    case 0x100: return "thousands";
    default: return NULL;
    }
}

static const char *const ONES[10] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
static const char *const TENS[10] = {"zero", "ten", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
static const char *const TEENS[10] = {"ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
                                      "seventeen", "eighteen", "nineteen"};
static const char *const ORD_ONES[10] = {"zeroth", "first", "second", "third", "fourth", "fifth", "sixth",
                                         "seventh", "eighth", "ninth"};
static const char *const ORD_TENS[10] = {"", "tenth", "twentieth", "thirtieth", "fortieth", "fiftieth", "sixtieth",
                                         "seventieth", "eightieth", "ninetieth"};
static const char *const ORD_TEENS[10] = {"tenth", "eleventh", "twelfth", "thirteenth", "fourteenth", "fifteenth",
                                          "sixteenth", "seventeenth", "eighteenth", "nineteenth"};
static const char *const SCALE[6] = {"hundred", "thousand", "million", "billion", "trillion", "quadrillion"};
static const char *const ORD_SCALE[6] = {"hundredth", "thousandth", "millionth", "billionth", "trillionth", "quadrillionth"};
static const char *const DENOM[10] = {"", "", "halves", "thirds", "fourths", "fifths", "sixths", "sevenths", "eighths", "ninths"};

/* ============================================================================================== */
/* sentence arena and word lists                                                                  */

typedef struct ablk {
    struct ablk *next;
    size_t used, cap;
    double data[1];
} ablk;

/* a word as the normalizer produces it: a span of the text or a static string */

/* engine node under construction (0x1c: text, len, ofs, words, count, pos, type record) */
typedef struct {
    int type;
    uint32_t pos;
    const wc *s;
    int len, ofs;
    const int *ti;
    wlist words;
} pnode;

struct sam_norm {
    wc *buf;
    int n;
    const wc *p, *fe, *te, *e; /* this+0x3c token start, 0x40 fragment end, 0x44 token end, 0x48 core end */
    int frag;                  /* this+0x38 != 0: text left */
    ablk *arena;
    pnode *pn;
    int npn, pcap;
    sam_sentence out;
};

static void *amalloc(sam_norm *nm, size_t size)
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

static void arena_reset(sam_norm *nm)
{
    while (nm->arena) {
        ablk *n = nm->arena->next;
        free(nm->arena);
        nm->arena = n;
    }
}

static nword *wl_new(sam_norm *nm, wlist *l)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        nword *w = amalloc(nm, sizeof(nword) * (size_t)cap);
        if (!w) return NULL;
        if (l->n) memcpy(w, l->w, sizeof(nword) * (size_t)l->n);
        l->w = w;
        l->cap = cap;
    }
    memset(&l->w[l->n], 0, sizeof(nword));
    return &l->w[l->n++];
}

static void wl_span(sam_norm *nm, wlist *l, const wc *t, int len)
{
    nword *w = wl_new(nm, l);
    if (w) {
        w->t = t;
        w->len = len;
    }
}

static void wl_str(sam_norm *nm, wlist *l, const char *s)
{
    nword *w = wl_new(nm, l);
    if (w) w->s = s;
}

static void wl_cat(sam_norm *nm, wlist *dst, const wlist *src) /* FUN_5ed502ea */
{
    int i;
    for (i = 0; i < src->n; i++) {
        nword *w = wl_new(nm, dst);
        if (w) *w = src->w[i];
    }
}

/* a table string made of several words ("number sign") */
static void wl_words(sam_norm *nm, wlist *l, const char *s)
{
    while (s && *s) {
        const char *sp = strchr(s, ' ');
        size_t n = sp ? (size_t)(sp - s) : strlen(s);
        char *c = amalloc(nm, n + 1);
        if (!c) return;
        memcpy(c, s, n);
        wl_str(nm, l, c);
        s = sp ? sp + 1 : NULL;
    }
}

/* ============================================================================================== */
/* numbers (FUN_5ed50e1a and helpers)                                                             */

/* FUN_5ed4f47a: integer part; v[0] groups of three after the first, v[1 + 4g ..] per group flags
 * {ones, tens, hundred, scale word}, v[0x19] ordinal, v[0x1a] read digit by digit, v[0x1b] digits,
 * v[0x1c] digits in the first group (count % 3), v[0x1d] thousands separators present */

static int parse_number(sam_norm *nm, int **out, const char *mode, int lookahead);

static void w_digit(sam_norm *nm, wc c, int *f, wlist *l) /* FUN_5ed4fef7 */
{
    wl_str(nm, l, ONES[c - '0']);
    f[0] = 1;
}

static void w_two(sam_norm *nm, const wc *s, int *f, wlist *l) /* FUN_5ed4ff5e */
{
    int d1 = s[0] - '0', d2 = s[1] - '0';
    if (d1 == 1) {
        wl_str(nm, l, TEENS[d2]);
    } else {
        if (d1 != 0) {
            wl_str(nm, l, TENS[d1]);
            f[1] = 1;
        }
        if (d2 == 0) return;
        w_digit(nm, s[1], f, l);
    }
    f[0] = 1;
}

static void w_three(sam_norm *nm, const wc *s, int *f, wlist *l) /* FUN_5ed50022 */
{
    if (s[0] != '0') {
        w_digit(nm, s[0], f, l);
        wl_str(nm, l, SCALE[0]);
        f[0] = 0;
        f[2] = 1;
    }
    w_two(nm, s + 1, f, l);
}

static void w_ord1(sam_norm *nm, wc c, int *f, wlist *l) /* FUN_5ed500ad */
{
    wl_str(nm, l, ORD_ONES[c - '0']);
    f[0] = 1;
}

static void w_ord2(sam_norm *nm, const wc *s, int *f, wlist *l) /* FUN_5ed50114 */
{
    int d1 = s[0] - '0', d2 = s[1] - '0';
    if (d1 == 1) {
        wl_str(nm, l, ORD_TEENS[d2]);
        f[0] = 1;
    } else if (d1 == 0) {
        w_ord1(nm, s[1], f, l);
    } else if (d2 == 0) {
        wl_str(nm, l, ORD_TENS[d1]);
    } else {
        wl_str(nm, l, TENS[d1]);
        f[1] = 1;
        w_ord1(nm, s[1], f, l);
        f[0] = 1;
    }
}

static int rest_zero(const wc *s) /* FUN_5ed4fa32 (reads up to the end of the buffer) */
{
    int i, n = wlen(s);
    for (i = 0; i < n; i++) {
        if (s[i] != '0' && n_isdigit(s[i])) return 0;
        if (!n_isdigit(s[i]) && s[i] != ',') return 1;
    }
    return 1;
}

static int group_zero(const wc *s) /* FUN_5ed4fa98 */
{
    int i = 0;
    while (s[i] == '0' || !n_isdigit(s[i])) {
        i++;
        if (i > 2) return 1;
    }
    return 0;
}

static void w_ord3(sam_norm *nm, const wc *s, int *f, wlist *l) /* FUN_5ed50215 */
{
    if (s[0] == '0') {
        w_ord2(nm, s + 1, f, l);
    } else {
        w_digit(nm, s[0], f, l);
        if (!rest_zero(s + 1)) {
            wl_str(nm, l, SCALE[0]);
            w_ord2(nm, s + 1, f, l);
            f[2] = 1;
        } else {
            wl_str(nm, l, ORD_SCALE[0]);
            f[0] = 0;
            f[2] = 1;
        }
    }
}

static int mode_is(const char *mode, const char *what)
{
    size_t i;
    if (!mode) return 0;
    for (i = 0;; i++) {
        int x = (unsigned char)mode[i], y = (unsigned char)what[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* FUN_5ed50598 */
static void int_words(sam_norm *nm, intpart *ip, const char *mode, wlist *l)
{
    const wc *s = ip->start;
    int len = (int)(ip->end - s), keep = ip->v[0] + 1, done = 0, off = 0, i;
    int *v = ip->v;
    if (s[0] == '0' || mode_is(mode, "NUMBER_DIGIT") || v[0] > 5) {
        v[0x1a] = 1;
        v[0x1b] = 0;
        for (i = 0; i < len; i++) {
            if (n_isdigit(s[i])) {
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
            if (f == 1) w_digit(nm, s[0], v + 1, l);
            else if (f == 2) w_two(nm, s, v + 1, l);
            else w_three(nm, s, v + 1, l);
        } else {
            if (f == 1) w_ord1(nm, s[0], v + 1, l);
            else if (f == 2) w_ord2(nm, s, v + 1, l);
            else w_ord3(nm, s, v + 1, l);
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
                }
            } else {
                v[(g + 1) * 4] = 1;
                wl_str(nm, l, ORD_SCALE[g]);
                done = 1;
            }
            v[0] = g - 1;
        }
        if (v[0x1d] && !done) off++;
        if (!done) {
            if (!v[0x19]) w_three(nm, s + off, v + v[0] * 4 + 1, l);
            else w_ord3(nm, s + off, v + 1, l);
        }
    }
    v[0] = keep;
}

/* FUN_5ed4f47a */
static int int_stop(wc c)
{
    wc u = n_toupper(c);
    return c == '.' || c == '%' || c == 0xb0 || c == 0xb2 || c == 0xb3 || c == '-' || c == 0xbc || c == 0xbd ||
           c == 0xbe || u == 'S' || u == 'N' || u == 'R' || u == 'T';
}

static int int_parse(sam_norm *nm, const wc *s, intpart **out)
{
    int n = (int)(nm->e - s), comma = 0, stop = 0, count, i;
    intpart *ip;
    if (n <= 0 || !n_isdigit(s[0])) return DECLINE;
    count = 1;
    i = 1;
    while (i < 4 && i < n) {
        wc c = s[i];
        if (c == ',') {
            comma = 1;
            break;
        }
        if (!n_isdigit(c) && int_stop(c)) {
            stop = 1;
            break;
        }
        if (!n_isdigit(c)) return DECLINE;
        count++;
        i++;
    }
    if (!stop && i < n) {
        if (!comma) {
            while (n_isdigit(s[i]) && i < n) {
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
                    if (!n_isdigit(s[k])) return DECLINE;
                    count++;
                }
                u += 4;
            }
        }
        if (i != n && !int_stop(s[i])) return DECLINE;
    }
    ip = amalloc(nm, sizeof *ip);
    if (!ip) return -2;
    ip->v[0x1d] = comma;
    ip->v[0x1c] = count % 3;
    ip->v[0] = (count - 1) / 3;
    ip->start = s;
    ip->end = s + i;
    *out = ip;
    return 0;
}

static int dec_parse(sam_norm *nm, const wc *s, decpart **out) /* FUN_5ed4f757 */
{
    const wc *q = s;
    int c = 0;
    while (q < nm->e && n_isdigit(*q)) {
        c++;
        q++;
    }
    if (!c) return DECLINE;
    *out = amalloc(nm, sizeof(decpart));
    if (!*out) return -2;
    (*out)->s = s;
    (*out)->n = c;
    return 0;
}

static int is_frac_reject(int t) { return t == 0x100f || t == 0x100e || t == 0x1007; }

static int frac_parse(sam_norm *nm, const wc *s, fracpart **out) /* FUN_5ed4fbb4 */
{
    const wc *e0 = nm->e, *save = nm->p, *slash;
    numinfo *num = NULL, *den = NULL;
    int r;
    if (e0 - s == 0) return DECLINE;
    if (*s == 0xbc || *s == 0xbd || *s == 0xbe) {
        fracpart *f = amalloc(nm, sizeof *f);
        if (!f) return -2;
        f->vulgar = s;
        f->num = amalloc(nm, sizeof(numinfo));
        f->den = amalloc(nm, sizeof(numinfo));
        if (!f->num || !f->den) return -2;
        f->num->ip = amalloc(nm, sizeof(intpart));
        f->den->ip = amalloc(nm, sizeof(intpart));
        if (!f->num->ip || !f->den->ip) return -2;
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
    slash = s;
    while (*slash && *slash != '/') slash++;
    if (!*slash) slash = NULL;
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
                    fracpart *f = amalloc(nm, sizeof *f);
                    if (!f) return -2;
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

static int fr_len(const fracpart *f) { return f->vulgar ? 1 : (int)(f->den->end - f->num->start); }

/* FUN_5ed50997 */
static void frac_words(sam_norm *nm, fracpart *f, wlist *l)
{
    numinfo *num, *den;
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
    wl_cat(nm, l, &f->num->words);
    num = f->num;
    den = f->den;
    dl = (int)(den->end - den->start);
    nl = (int)(num->end - num->start);
    num_one = nl == 1 && num->start[0] == '1';
    over = 1;
    if (den->dp || num->dp || den->neg) {
        over = 1;
    } else if (dl == 1 && den->start[0] != '1') {
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
        wl_cat(nm, l, &den->words);
    }
}

/* FUN_5ed54b1d: end of the whitespace token (at most 127 characters) */
static const wc *token_end(const wc *p, const wc *fe)
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

/* FUN_5ed54fee without fragments: skip whitespace; NULL at the end of the text */
static const wc *skip_ws(const wc *p, const wc *fe)
{
    if (!p) return NULL;
    while (p < fe && is_ws(*p)) p++;
    return p < fe ? p : NULL;
}

/* strip trailing punctuation of a lookahead token (loops in FUN_5ed50e1a / 497a0) */
static const wc *strip_trailing(const wc *end)
{
    const wc *q = end;
    for (;;) {
        do {
            end = q;
            q--;
        } while (clause_type(*q) != 0x1001);
        if (close_type(*q) == 0x1001 && quote_type(*q) == 0x1001 && sent_type(*q) == 0x1001) break;
    }
    return end;
}

/* FUN_5ed50e1a */
static int parse_number(sam_norm *nm, int **out, const char *mode, int lookahead)
{
    const wc *s = nm->p, *save_e = nm->e, *save_fe = nm->fe, *base;
    int save_frag = nm->frag, n = (int)(nm->e - s), off, neg, r = 0;
    intpart *ip = NULL;
    decpart *dp = NULL;
    fracpart *fr = NULL;
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
    if (!n_isalpha(s[off])) {
        if (s[off] == '-') off++;
        goto fraction;
    }
    {
        int u = n_toupper(s[off]), ord = 0;
        if (u == 'N') {
            if (off + 2 == n && n_toupper(s[off + 1]) == 'D' && s[off - 1] == '2') ord = off == 1 || s[off - 2] != '1';
        } else if (u == 'R') {
            if (off + 2 == n && n_toupper(s[off + 1]) == 'D' && s[off - 1] == '3') ord = off == 1 || s[off - 2] != '1';
        } else if (u == 'S') {
            if (n_toupper(s[off + 1]) == 'T' && s[off - 1] == '1' && off + 2 == n) ord = off == 1 || s[off - 2] != '1';
        } else if (u == 'T') {
            if (off + 2 == n && n_toupper(s[off + 1]) == 'H') {
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
        const wc *q = skip_ws(nm->e, nm->fe);
        if (q) {
            nm->p = q;
            nm->e = strip_trailing(token_end(q, nm->fe));
            r = frac_parse(nm, nm->p, &fr);
            if (r < 0) {
                nm->p = s;
                nm->fe = save_fe;
                nm->e = save_e;
                nm->frag = save_frag;
                if (r == DECLINE) r = 0;
                fr = NULL;
            } else {
                n = (int)(nm->e - nm->p);
                off = fr_len(fr);
            }
        }
    }
check:
    base = nm->p;
    if (off != n && !(n == off + 1 && (base[off] == '%' || base[off] == 0xb0 || base[off] == 0xb2 || base[off] == 0xb3))) {
        nm->p = s;
        nm->e = save_e;
        nm->fe = save_fe;
        nm->frag = save_frag;
        r = DECLINE;
    }
    if (r < 0 || (!ip && !dp && !fr)) return r < 0 ? r : DECLINE;
    nm->p = s;
    if (ip && ip->end - ip->start == 4 && !ip->v[0x1d] && !ip->v[0x19] && !dp && !fr && !neg && off == n &&
        !(mode && !strncmp(mode, "NUMBER", 6))) {
        yearinfo *y = amalloc(nm, sizeof *y);
        if (!y) return -2;
        y->type = 0x1014;
        y->s = s;
        y->len = 4;
        *out = (int *)y;
        return 0;
    }
    {
        numinfo *ni = amalloc(nm, sizeof *ni);
        if (!ni) return -2;
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
        if (neg) wl_str(nm, &ni->words, "negative");
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
            if (ip) wl_str(nm, &ni->words, "and");
            frac_words(nm, fr, &ni->words);
        }
        *out = (int *)ni;
    }
    return 0;
}

/* FUN_5ed49422: a number read as a year */
static void year_words(sam_norm *nm, const wc *s, int len, wlist *l)
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
/* nodes                                                                                          */

static pnode *pn_add(sam_norm *nm, int type, uint32_t pos, const wc *s, int len)
{
    pnode *n;
    if (nm->npn == nm->pcap) {
        int cap = nm->pcap ? nm->pcap * 2 : 64;
        pnode *p = realloc(nm->pn, sizeof(pnode) * (size_t)cap);
        if (!p) return NULL;
        nm->pn = p;
        nm->pcap = cap;
    }
    n = &nm->pn[nm->npn++];
    memset(n, 0, sizeof *n);
    n->type = type;
    n->pos = pos;
    n->s = s;
    n->len = len;
    n->ofs = s ? (int)(s - nm->buf) : nm->n;
    return n;
}

/* ============================================================================================== */
/* abbreviations and sentence ends                                                                */

/* bsearch with _wcsicmp over the abbreviation table; key = span */
static const sam_abbrev *abbrev_find(const wc *s, int len)
{
    int lo = 0, hi = (int)(sizeof ABBREVS / sizeof *ABBREVS) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = wspan_icmp_a(s, len, ABBREVS[mid].word);
        if (!c) return &ABBREVS[mid];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

/* FUN_5ed4ce39 over the sentence-starter table (equal only with equal length) */
static int starter_cmp(const wc *key, int klen, const char *w)
{
    int wl = (int)strlen(w), c;
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
    int lo = 0, hi = (int)(sizeof SENTENCE_STARTERS / sizeof *SENTENCE_STARTERS) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = starter_cmp(key, klen, SENTENCE_STARTERS[mid]);
        if (!c) return 1;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return 0;
}

/* next token starts a new sentence: end of text, or a capitalized sentence-starter word.
 * Returns -1 for "no next token", 1 for a starter, 0 otherwise. */
static int next_starts_sentence(sam_norm *nm)
{
    const wc *q = skip_ws(nm->te, nm->fe), *qe;
    if (!q) return -1;
    if (!is_upper_ascii(*q)) return 0;
    qe = token_end(q, nm->fe);
    return is_starter(q, (int)(qe - q));
}

static int abbrev_is_date(const sam_abbrev *a) /* FUN_5ed465a4 */
{
    static const char *const D[] = {"jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept", "oct", "nov", "dec",
                                    "mon", "tue", "tues", "wed", "thu", "thur", "thurs", "fri", "sat", "sun"};
    size_t k;
    for (k = 0; k < sizeof D / sizeof *D; k++)
        if (!strcmp(a->word, D[k])) return 1;
    return 0;
}

/* the token node becomes an abbreviation (text includes the period) */
static int make_abbrev(sam_norm *nm, pnode *core, const sam_abbrev *a)
{
    sam_abbrev_ti *ti = amalloc(nm, sizeof *ti);
    if (!ti) return -2;
    ti->type = abbrev_is_date(a) ? SAM_NODE_ABBREV2 : SAM_NODE_ABBREV;
    ti->rec = a;
    core->type = ti->type;
    core->ti = (const int *)ti;
    core->s = nm->p;
    core->len = (int)(nm->e - nm->p);
    core->ofs = (int)(nm->p - nm->buf);
    core->words.n = 0;
    wl_span(nm, &core->words, nm->p, core->len);
    return 0;
}

/* sentence-time abbreviation handlers (@0x5ede9868): FUN_5ed4683a, 469a4, 46b27 */
static int abbrev_sentence(sam_norm *nm, int h, const sam_abbrev *a, pnode *core, int *sent_end)
{
    if (!*sent_end) {
        int s = next_starts_sentence(nm);
        if (s != 0) *sent_end = 1;
        if (h == 1 && s == -1) return DECLINE;
    }
    if (h == 1 && *sent_end) return DECLINE;
    if (h == 2 && *sent_end && !w_isupper(*nm->p)) return DECLINE;
    return make_abbrev(nm, core, a);
}

/* FUN_5ed47a6e: initials "U.S.A." */
static int h_initials(sam_norm *nm, pnode *core, int *sent_end)
{
    const wc *q = nm->p;
    int count = 0, k;

    if (nm->e - nm->p < 4) return DECLINE;
    for (;;) {
        if (nm->e - 2 < q) break;
        if (!w_isalpha(*q) || q[1] != '.') return DECLINE;
        q += 2;
        count++;
    }
    if (!*sent_end && next_starts_sentence(nm) != 0) *sent_end = 1;
    {
        typeinfo *ti = amalloc(nm, sizeof *ti);
        if (!ti) return -2;
        ti->type = 0x1005;
        core->type = 0x1005;
        core->ti = (const int *)ti;
        core->s = nm->p;
        core->len = (int)(nm->e - nm->p);
        core->ofs = (int)(nm->p - nm->buf);
        core->words.n = 0;
        for (k = 0; k < count; k++) {
            wl_span(nm, &core->words, nm->p + 2 * k, 1);
            core->words.w[core->words.n - 1].fpos = 0x1000;
        }
    }
    return 0;
}

/* ============================================================================================== */
/* token classes (FUN_5ed4c45e dispatcher and its handlers)                                       */

static int h_word(const wc *s, const wc *e) /* FUN_5ed467be */
{
    int apos = 0;
    for (; s < e; s++) {
        if (w_isalpha(*s)) continue;
        if (*s == '\'' && !apos) {
            apos = 1;
            continue;
        }
        return DECLINE;
    }
    return 0;
}

static int h_num(sam_norm *nm, int **out, const char *mode) /* FUN_5ed52724 */
{
    const wc *fe = nm->fe, *p = nm->p, *e = nm->e;
    int frag = nm->frag, r;
    numinfo *ni;
    r = parse_number(nm, out, mode, 1);
    if (r < 0) return r;
    ni = (numinfo *)*out;
    if (ni->type != 0x1014) {
        if (ni->end == nm->e - 1) {
            switch (*ni->end) {
            case '%': ni->type = 0x1009; return r;
            case 0xb0: ni->type = 0x100a; return r;
            case 0xb2: ni->type = 0x100b; return r;
            case 0xb3: ni->type = 0x100c; return r;
            default: return DECLINE;
            }
        }
        if (ni->end != nm->e) {
            nm->fe = fe;
            nm->e = e;
            nm->p = p;
            nm->frag = frag;
            return DECLINE;
        }
    }
    return r;
}

static int h_hyphen(sam_norm *nm, const wc *s, const wc *e, int **out) /* FUN_5ed4d66f */
{
    const wc *save_p = nm->p, *save_e = nm->e, *h;
    int *left = NULL, *right = NULL, r;
    for (h = s; h < e; h++)
        if (*h == '-') break;
    if (!(*h == '-' && s < h && h < e - 1)) return DECLINE;
    r = h_word(s, h);
    if (r == 0) {
        typeinfo *t = amalloc(nm, sizeof *t);
        if (!t) return -2;
        t->type = 0x1002;
        left = (int *)t;
    } else {
        nm->p = s;
        nm->e = h;
        r = h_num(nm, &left, "NUMBER");
    }
    if (r >= 0) {
        r = h_word(h + 1, e);
        if (r == 0) {
            typeinfo *t = amalloc(nm, sizeof *t);
            if (!t) return -2;
            t->type = 0x1002;
            right = (int *)t;
        } else {
            nm->p = h + 1;
            nm->e = e;
            r = h_num(nm, &right, "NUMBER");
            if (r == DECLINE) r = h_hyphen(nm, h + 1, e, &right);
        }
    }
    nm->p = save_p;
    nm->e = save_e;
    if (r < 0) return r;
    {
        hypheninfo *hi = amalloc(nm, sizeof *hi);
        if (!hi) return -2;
        hi->type = 0x101b;
        hi->left = left;
        hi->right = right;
        hi->ls = s;
        hi->rs = h + 1;
        *out = (int *)hi;
    }
    return 0;
}

static int h_dashword(sam_norm *nm, const wc *s, const wc *e, int **out) /* FUN_5ed4ce91 */
{
    const wc *q;
    dashinfo *d;
    if (*s != '-') return DECLINE;
    for (q = s + 1; q < e && w_isalpha(*q); q++) {
    }
    if (q != e || q == s + 1) return DECLINE;
    d = amalloc(nm, sizeof *d);
    if (!d) return -2;
    d->type = 0x1029;
    d->s = s + 1;
    d->len = (int)(e - s) - 1;
    *out = (int *)d;
    return 0;
}


/* ============================================================================================== */
/* token handlers: currency, clock times, durations, ranges, decades, phone numbers, dates        */

static int w_isdigit(wc c) { return (c >= '0' && c <= '9') || c == 0xb2 || c == 0xb3 || c == 0xb9; } /* FUN_5ed5a508 */

/* FUN_5ed47e95: wcstol of a leading number (0 and end = s when it does not start with a digit) */
static long wnum(const wc *s, const wc **end)
{
    long v = 0;
    if (!w_isdigit(*s)) {
        if (end) *end = s;
        return 0;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        if (v > 0x7fffffffL / 10) v = 0x7fffffffL / 10;
        s++;
    }
    if (end) *end = s;
    return v;
}

static int is_punct_any(wc c)
{
    return clause_type(c) != 0x1001 || close_type(c) != 0x1001 || quote_type(c) != 0x1001 || sent_type(c) != 0x1001;
}

/* trailing-punctuation strip of a lookahead token that also reports whether anything went */
static const wc *strip_flag(const wc *end, int *stripped)
{
    const wc *q = end;
    for (;;) {
        const wc *e = q;
        q--;
        if (!is_punct_any(*q)) return e;
        if (stripped) *stripped = 1;
    }
}

/* ---- currency (FUN_5ed52836) ---- */

static const wc CUR_SYM[14][7] = {{'$'}, {0xa3}, {0xa5}, {'E', 'U', 'R'}, {'U', 'S', '$'}, {0x80}, {0x20ac}, {'D', 'M'},
                                  {0xa2}, {'U', 'S', 'D'}, {'d', 'o', 'l', '.'}, {'s', 'c', 'h', 'i', 'l', '.'},
                                  {'d', 'o', 'l'}, {'s', 'c', 'h', 'i', 'l'}};
static const char *const CUR_PLURAL[14] = {"dollars", "pounds", "yen", "euros", "dollars", "euros", "euros",
                                           "deutschemarks", "cents", "dollars", "dollars", "schillings", "dollars",
                                           "schillings"};
static const char *const CUR_CENTS[14] = {"cents", "pence", "sen", "cents", "cents", "cents", "cents", "pfennigs",
                                          "", "cents", "cents", "", "cents", ""};
static const char *const CUR_SING[14] = {"dollar", "pound", "yen", "euro", "dollar", "euro", "euro", "deutschemark",
                                         "cent", "dollar", "dollar", "schilling", "dollar", "schilling"};
static const char *const CENT_SING[14] = {"cent", "penny", "sen", "cent", "cent", "cent", "cent", "pfennig",
                                          "", "cent", "cent", "", "cent", ""};

static int wlen16(const wc *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int wnicmp16(const wc *a, const wc *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int x = w_tolower(a[i]), y = w_tolower(b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}

static int cur_match(const wc **p, const wc **e, int *suffix) /* FUN_5ed4f960 */
{
    int k;
    for (k = 0; k < 14; k++) {
        int l = wlen16(CUR_SYM[k]);
        if (l <= *e - *p && !wnicmp16(*p, CUR_SYM[k], l)) {
            *p += l;
            *suffix = 0;
            return k;
        }
    }
    for (k = 0; k < 14; k++) {
        int l = wlen16(CUR_SYM[k]);
        if (l <= *e - *p && !wnicmp16(*e - l, CUR_SYM[k], l)) {
            *e -= l;
            *suffix = 1;
            return k;
        }
    }
    return -1;
}

static int scale_match(const wc **p, const wc *e) /* FUN_5ed4f7d8 */
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

static int h_currency(sam_norm *nm, int **out, wlist *l)
{
    const wc *s0 = nm->p, *e0 = nm->e, *fe0 = nm->fe;
    int frag0 = nm->frag, neg, cur, suffix = 2, stripped = 0, scale = -1, r = 0;
    numinfo *num = NULL, *cents = NULL;
    neg = *nm->p == '-';
    if (neg) nm->p++;
    cur = cur_match(&nm->p, &nm->e, &suffix);
    if (cur < 0 || suffix != 0) {
        /* number, then a currency word in the next token: "1000 dollars" */
        const wc *q;
        r = h_num(nm, (int **)&num, "NUMBER");
        if (r < 0) goto fail;
        if (!cur_num_ok(num->type)) {
            r = DECLINE;
            goto fail;
        }
        q = skip_ws(nm->e, nm->fe);
        if (!q) {
            r = DECLINE;
            goto fail;
        }
        nm->p = q;
        nm->e = strip_flag(token_end(q, nm->fe), &stripped);
        cur = cur_match(&nm->p, &nm->e, &suffix);
        if (!stripped) {
            if (cur >= 0) {
                const wc *sp = nm->p, *sfe = nm->fe, *se = nm->e;
                int sfrag = nm->frag;
                const wc *q2 = skip_ws(nm->p, nm->fe);
                if (q2) {
                    const wc *qe;
                    nm->p = q2;
                    qe = strip_trailing(token_end(q2, nm->fe));
                    nm->e = qe;
                    scale = scale_match(&nm->p, nm->e);
                    if (scale >= 0) goto build;
                }
                nm->p = sp;
                nm->fe = sfe;
                nm->e = se;
                nm->frag = sfrag;
                goto build;
            }
        } else if (cur >= 0) {
            goto build;
        }
        r = DECLINE;
        goto fail;
    } else {
        /* currency symbol first: "$5", "$2 billion" */
        const wc *q = skip_ws(nm->p, nm->fe);
        if (!q) {
            r = DECLINE;
            goto fail;
        }
        nm->p = q;
        nm->e = strip_flag(token_end(q, nm->fe), &stripped);
        r = h_num(nm, (int **)&num, "NUMBER");
        if (r >= 0 && !cur_num_ok(num->type)) r = DECLINE;
        if (!stripped) {
            const wc *sp, *se, *sfe, *q2;
            int sfrag;
            if (r < 0) goto fail;
            sp = nm->p;
            sfrag = nm->frag;
            sfe = nm->fe;
            se = nm->e;
            q2 = skip_ws(se, nm->fe);
            if (q2) {
                nm->p = q2;
                nm->e = strip_trailing(token_end(q2, nm->fe));
                scale = scale_match(&nm->p, nm->e);
                if (scale >= 0) goto build;
            }
            nm->p = sp;
            nm->fe = sfe;
            nm->e = se;
            nm->frag = sfrag;
        }
        if (r < 0) goto fail;
    }
build : {
    currinfo *ci = amalloc(nm, sizeof *ci);
    const wc *dot;
    if (!ci) return -2;
    ci->type = 0x100d;
    ci->scale = scale >= 0;
    ci->num = num;
    if (num->type == 0x1008 && scale < 0 && CUR_CENTS[cur][0]) {
        for (dot = num->start; *dot && *dot != '.'; dot++) {
        }
        if (*dot == '.' && num->end - dot == 3) {
            const wc *sp = nm->p, *se = nm->e, *numend = num->end;
            numinfo *dollars = NULL;
            nm->p = num->start;
            nm->e = dot;
            if (nm->p == nm->e || (*nm->p == '-' && nm->p == nm->e - 1)) {
                dollars = amalloc(nm, sizeof *dollars);
                if (!dollars) return -2;
                dollars->neg = *nm->p == '-';
                dollars->ip = amalloc(nm, sizeof(intpart));
                if (!dollars->ip) return -2;
                dollars->ip->v[0x1a] = 1;
                dollars->ip->v[0x1b] = 1;
                if (dollars->neg) wl_str(nm, &dollars->words, "negative");
                wl_str(nm, &dollars->words, "zero");
                r = 0;
            } else {
                r = parse_number(nm, (int **)&dollars, "NUMBER", 0);
            }
            if (r >= 0) {
                ci->num = dollars;
                nm->p = dot + 1;
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
        wl_str(nm, l, "negative");
    }
    wl_cat(nm, l, &ci->num->words);
    {
        const char *unit;
        if (scale < 0) {
            const numinfo *n = ci->num;
            int nl = n->start ? (int)(n->end - n->start) : 0;
            if (!ci->cents && !n->dp && n->fr && n->fr->over == 0) {
                wl_str(nm, l, "of");
                wl_str(nm, l, "a");
                unit = CUR_SING[cur];
            } else if ((nl == 1 && n->start[0] == '1') || (nl == 2 && n->start[0] == '-' && n->start[1] == '1')) {
                unit = CUR_SING[cur];
            } else {
                unit = CUR_PLURAL[cur];
            }
        } else {
            wl_str(nm, l, SCALE[scale]);
            unit = CUR_PLURAL[cur];
        }
        wl_str(nm, l, unit);
    }
    if (ci->cents) {
        const numinfo *c = ci->cents;
        wl_str(nm, l, "and");
        wl_cat(nm, l, &c->words);
        if (c->end - c->start == 1 && c->start[0] == '1') wl_str(nm, l, CENT_SING[cur]);
        else wl_str(nm, l, CUR_CENTS[cur]);
    }
    nm->p = s0;
    *out = (int *)ci;
    return 0;
}
fail:
    nm->p = s0;
    nm->fe = fe0;
    nm->e = e0;
    nm->frag = frag0;
    return r;
}

/* ---- clock times (FUN_5ed562fe) ---- */


static int ampm_word(const wc *s, const wc *e) /* 0 am, 1 pm, -1 none */
{
    if ((!wnicmp_a(s, "am", 2) && s + 2 == e) || (!wnicmp_a(s, "a.m.", 4) && s + 4 == e)) return 0;
    if ((!wnicmp_a(s, "pm", 2) && s + 2 == e) || (!wnicmp_a(s, "p.m.", 4) && s + 4 == e)) return 1;
    return -1;
}

/* next token for am/pm; '.' is kept unless the token is "am." / "pm." (mode 1) or always (mode 0) */
static const wc *ampm_lookahead(sam_norm *nm, const wc *from, const wc **tend, int mode)
{
    const wc *q = skip_ws(from, nm->fe), *e;
    if (!q) return NULL;
    e = token_end(q, nm->fe);
    for (;;) {
        const wc *x = e - 1;
        int t;
        if (clause_type(*x) != 0x1001 || close_type(*x) != 0x1001 || quote_type(*x) != 0x1001) {
            e = x;
            continue;
        }
        t = sent_type(*x);
        if (t == 0x1001) break;
        if (t == 9) {
            if (mode == 0) break;
            if (!(!wnicmp_a(q, "am.", 3) && q + 3 == e) && !(!wnicmp_a(q, "pm.", 3) && q + 3 == e)) break;
        }
        e = x;
    }
    *tend = e;
    return q;
}

static int h_clock(sam_norm *nm, int **out, wlist *l, int lookahead)
{
    const wc *p = nm->p, *e = nm->e, *q, *m = NULL, *mend, *ne = NULL;
    long hour, min = 0;
    int ampm = 2, consumed = 0;
    timeinfo *ti;
    int f[4] = {0};
    if (e - p > 9) return DECLINE;
    hour = wnum(p, &q);
    if (q == p || q - p > 2) return DECLINE;
    if (*q != ':') {
        if (q < e) {
            ampm = ampm_word(q, e);
            if (ampm < 0) return DECLINE;
        } else {
            const wc *nq;
            if (!lookahead) return DECLINE;
            nq = ampm_lookahead(nm, e, &ne, 0);
            if (!nq) return DECLINE;
            ampm = ampm_word(nq, ne);
            if (ampm < 0) return DECLINE;
            consumed = 1;
        }
    } else {
        m = q + 1;
        min = wnum(m, &mend);
        if (m == mend || mend - m != 2) return DECLINE;
        if (mend == e) {
            if (hour == 0 || hour > 23 || min > 59) return DECLINE;
            if (lookahead) {
                const wc *nq = ampm_lookahead(nm, e, &ne, 1);
                if (nq) {
                    int a = ampm_word(nq, ne);
                    if (a >= 0) {
                        ampm = a;
                        consumed = 1;
                    }
                }
            }
        } else {
            ampm = ampm_word(mend, e);
            if (ampm < 0) return DECLINE;
            if (hour == 0 || hour > 23 || min > 59) return DECLINE;
        }
    }
    ti = amalloc(nm, sizeof *ti);
    if (!ti) return -2;
    ti->type = 0x1018;
    ti->minutes = m != NULL;
    ti->ampm = ampm != 2;
    if (w_isdigit(p[1])) w_two(nm, p, f, l);
    else w_digit(nm, p[0], f, l);
    if (m) {
        if (!wncmp_a(m, "00", 2)) {
            if (hour > 12) {
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
    if (ampm == 0) {
        wl_str(nm, l, "a");
        l->w[l->n - 1].fpos = 0x1000;
        wl_str(nm, l, "m");
        l->w[l->n - 1].fpos = 0x1000;
    } else if (ampm == 1) {
        wl_str(nm, l, "p");
        wl_str(nm, l, "m");
    }
    if (consumed) nm->e = ne;
    *out = (int *)ti;
    return 0;
}

/* ---- durations h:mm:ss / m:ss (FUN_5ed56cc5, words FUN_5ed56ae3) ---- */

typedef struct {
    int type;
    numinfo *hours, *minutes;
    const wc *seconds;
} durinfo;

static int ndigits(long v)
{
    int n = 1;
    while (v >= 10) {
        v /= 10;
        n++;
    }
    return n;
}

static int h_duration(sam_norm *nm, int **out)
{
    const wc *s0 = nm->p, *e0 = nm->e, *q = nm->p, *colon, *mstart, *mend, *sec, *send;
    numinfo *first = NULL;
    long mval;
    int neg = 0, r;
    durinfo *d;
    if (*q == '-') {
        neg = 1;
        q++;
    }
    while (*q == '0') q++;
    if (*q == ':') q--;
    for (colon = q; *colon && *colon != ':'; colon++) {
    }
    if (!*colon || colon <= q || colon >= nm->e - 1) return DECLINE;
    nm->p = q;
    nm->e = colon;
    r = h_num(nm, (int **)&first, "NUMBER");
    nm->p = s0;
    nm->e = e0;
    if (r < 0 || (first->type != 0x1008 && first->type != 0x1006)) return DECLINE;
    if (neg) {
        first->neg = 1;
        {
            /* "negative" goes in front (FUN_5ed562c0 inserts at the head) */
            wlist w = {0};
            wl_str(nm, &w, "negative");
            wl_cat(nm, &w, &first->words);
            first->words = w;
        }
    }
    mstart = colon + 1;
    mval = wnum(mstart, &mend);
    if (mstart == mend || mend - mstart != 2) return DECLINE;
    d = NULL;
    if (mend != nm->e) {
        long sval;
        numinfo *mins = NULL;
        const wc *mp;
        if (*mend != ':') return DECLINE;
        sec = mend + 1;
        sval = wnum(sec, &send);
        if (sec == send || send - sec != 2 || send != nm->e || mval >= 60 || sval >= 60) return DECLINE;
        d = amalloc(nm, sizeof *d);
        if (!d) return -2;
        d->type = 0x1019;
        d->hours = first;
        mp = mval ? mstart + (int)((sec - 2 - mstart) - (ndigits(mval) - 1)) - 0 : sec - 2;
        if (mval) mp = mstart + ((int)(sec - mstart) - 2) + 1 - ndigits(mval);
        nm->e = sec - 1;
        nm->p = mp;
        r = parse_number(nm, (int **)&mins, "NUMBER", 1);
        nm->p = s0;
        nm->e = e0;
        if (r < 0) return r;
        d->minutes = mins;
        d->seconds = *sec == '0' ? sec + 1 : sec;
    } else {
        if (mval > 0x3b) return DECLINE;
        d = amalloc(nm, sizeof *d);
        if (!d) return -2;
        d->type = 0x1019;
        d->minutes = first;
        d->seconds = *mstart == '0' ? colon + 2 : mstart;
    }
    *out = (int *)d;
    return 0;
}

static void duration_words(sam_norm *nm, const durinfo *d, wlist *l)
{
    int f[4] = {0};
    if (d->hours) {
        const numinfo *h = d->hours;
        wl_cat(nm, l, &h->words);
        wl_str(nm, l, h->end - h->start == 1 && h->start[0] == '1' ? "hour" : "hours");
        if (d->minutes && d->minutes->start && !d->seconds) wl_str(nm, l, "and");
    }
    if (d->minutes) {
        const numinfo *m = d->minutes;
        wl_cat(nm, l, &m->words);
        wl_str(nm, l, m->end - m->start == 1 && m->start[0] == '1' ? "minute" : "minutes");
        if (d->seconds) wl_str(nm, l, "and");
    }
    if (d->seconds) {
        const wc *s = d->seconds;
        if (!w_isdigit(s[1])) w_digit(nm, s[0], f, l);
        else w_two(nm, s, f, l);
        wl_str(nm, l, s[0] == '1' && !w_isdigit(s[1]) ? "second" : "seconds");
    }
}

/* ---- ranges "10-20" (FUN_5ed5326b, words FUN_5ed50d4f) ---- */

typedef struct {
    int type;
    int *left, *right;
} rangeinfo;

static int h_range(sam_norm *nm, int **out)
{
    const wc *s = nm->p, *e = nm->e, *h;
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
        if (r >= 0) {
            rangeinfo *ri = amalloc(nm, sizeof *ri);
            if (!ri) return -2;
            ri->type = 0x101e;
            ri->left = left;
            ri->right = right;
            *out = (int *)ri;
        }
    }
    nm->p = s;
    nm->e = e;
    return r;
}

/* ---- decades "1990s", "'90s" (FUN_5ed47fb5, words FUN_5ed4961f) ---- */

typedef struct {
    int type;
    const wc *century; /* first two digits, or NULL */
    int decade;
} decadeinfo;

static int h_decade(sam_norm *nm, int **out)
{
    const wc *p = nm->p;
    int n = (int)(nm->e - p), ok;
    decadeinfo *d;
    const wc *cent = NULL;
    int dec;
    if (n < 3 || n > 6) return DECLINE;
    if (n == 4) {
        if (p[3] == 's' && p[2] == '0' && w_isdigit(p[1]) && p[0] == '\'') {
            dec = p[1] - '0';
            goto make;
        }
        if (p[3] != 's') return DECLINE;
        ok = p[2] == '\'';
    } else if (n == 3) {
        ok = p[2] == 's';
    } else {
        if (n == 5) {
            ok = p[4] == 's';
        } else {
            if (p[5] != 's') return DECLINE;
            ok = p[4] == '\'';
        }
        if (!ok || p[3] != '0' || !w_isdigit(p[2]) || !w_isdigit(p[1]) || !w_isdigit(p[0])) return DECLINE;
        cent = p;
        dec = p[2] - '0';
        goto make;
    }
    if (!ok || p[1] != '0' || !w_isdigit(p[0])) return DECLINE;
    dec = p[0] - '0';
make:
    d = amalloc(nm, sizeof *d);
    if (!d) return -2;
    d->type = 0x1017;
    d->century = cent;
    d->decade = dec;
    *out = (int *)d;
    return 0;
}

static void decade_words(sam_norm *nm, const decadeinfo *d, wlist *l)
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

/* ---- phone numbers (FUN_5ed5196a) ---- */


static int is_psep(wc c) { return c == ' ' || c == '-' || c == '.'; } /* FUN_5ed4f452 */

static int count_digits(const wc *c, const wc *e, int max)
{
    int k = 0;
    if (c < e) {
        do {
            if (!w_isdigit(c[k]) || k > max) break;
            k++;
        } while (c + k < e);
    }
    return k;
}

static int h_phone(sam_norm *nm, int **out, wlist *l, const char *mode)
{
    const wc *c = nm->p, *e = nm->e, *fe = nm->fe, *sep = NULL, *country = NULL, *area = NULL;
    const wc *saved_fe = NULL, *gp[4] = {0};
    int frag = nm->frag, saved_frag = 0, have_saved = 0, clen = 0, one = 0, alen = 0, k, G = 0, i;
    int gl[4] = {0};
    if (*c == '+') {
        const wc *q;
        c++;
        k = count_digits(c, e, 2);
        country = c;
        clen = k;
        if (k == 0) return DECLINE;
        q = c + k;
        if (q < e && is_psep(*q)) {
            sep = q;
            c = q + 1;
        } else {
            if (e != q) return DECLINE;
            c = skip_ws(q, fe);
            if (!c) return DECLINE;
            e = token_end(c, fe);
        }
    } else if (*c == '1' && !w_isdigit(c[1])) {
        const wc *c1 = c;
        c++;
        one = 1;
        if (c < e && is_psep(*c)) {
            sep = c;
            c = c1 + 2;
        } else if (e == c) {
            c = skip_ws(c, fe);
            if (!c) return DECLINE;
            e = token_end(c, fe);
        } else {
            return DECLINE;
        }
    }
    if (c < e) {
        int area_ok = 1;
        if (!country && !one) {
            if (c <= nm->buf || c[-1] != '(') area_ok = 0;
        } else {
            if (*c != '(') area_ok = 0;
            else c++;
        }
        if (area_ok) {
            const wc *rp, *q4;
            k = count_digits(c, e, 2);
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
                c = skip_ws(rp + 1, fe);
                if (!c) return DECLINE;
                e = token_end(c, fe);
            } else {
                return DECLINE;
            }
        }
    }
    if (c < e) {
        for (i = 0;; i++) {
            int n;
            const wc *q;
            if (i > 3) {
                G = i;
                break;
            }
            n = count_digits(c, e, 3);
            if (n < 2) {
                if (sep) return DECLINE;
                if (have_saved) {
                    fe = saved_fe;
                    frag = saved_frag;
                }
                G = i;
                break;
            }
            gp[i] = c;
            gl[i] = n;
            c += n;
            q = c + 1;
            if (q < e && is_psep(*c)) {
                if (sep) {
                    if (*sep != *c) return DECLINE;
                    c = q;
                    continue;
                }
                if (i == 0) {
                    sep = c;
                    c = q;
                    continue;
                }
                if (have_saved) {
                    fe = saved_fe;
                    frag = saved_frag;
                }
                G = i;
                break;
            }
            if (sep || e != c) {
                if (e == q) {
                    if (!is_punct_any(*c)) return DECLINE;
                    G = i + 1;
                    break;
                }
                if (e == c) {
                    G = i + 1;
                    break;
                }
                return DECLINE;
            }
            saved_fe = fe;
            saved_frag = frag;
            have_saved = 1;
            {
                const wc *nq = skip_ws(c, fe);
                if (!nq) {
                    G = i + 1;
                    break;
                }
                c = nq;
                e = token_end(c, fe);
            }
        }
    }
    /* pattern checks */
#define L(x) gl[x]
#define SHIFT()                                                                                           \
    do {                                                                                                  \
        area = gp[0];                                                                                     \
        alen = gl[0];                                                                                     \
        for (k = 0; k < 3; k++) {                                                                         \
            gp[k] = gp[k + 1];                                                                            \
            gl[k] = gl[k + 1];                                                                            \
        }                                                                                                 \
    } while (0)
    if (country) {
        if (!one && !area && G == 4 && (L(0) == 2 || L(0) == 3) && (L(1) == 2 || L(1) == 3) && L(2) >= 2 && L(3) >= 2) {
            SHIFT();
            G = 3;
        } else if (one) {
            return DECLINE;
        } else if (area) {
            if (G == 3 && (L(0) == 2 || L(0) == 3) && L(1) >= 2 && L(2) >= 2) {
            } else if (G == 2 && (L(0) == 2 || L(0) == 3) && L(1) >= 2) {
            } else {
                return DECLINE;
            }
        } else if (G == 3 && (L(0) == 2 || L(0) == 3) && (L(1) == 2 || L(1) == 3) && L(2) >= 2) {
            SHIFT();
            G = 2;
        } else {
            return DECLINE;
        }
    } else if (G == 2 && L(0) == 3 && L(1) >= 3) {
        if (!one || area) {
            if (!mode_is(mode, "phone_number") && !area && !one && sep && *sep == '.') return DECLINE;
        } else {
            return DECLINE;
        }
    } else if (!area && G == 3 && (L(0) == 2 || L(0) == 3) && L(1) == 3 && L(2) >= 3) {
        SHIFT();
        G = 2;
    } else if (!one && area && G == 3 && (L(0) == 2 || L(0) == 3) && L(1) == 2 && L(2) >= 2) {
    } else {
        return DECLINE;
    }
#undef SHIFT
#undef L
    {
        phoneinfo *pi = amalloc(nm, sizeof *pi);
        if (!pi) return -2;
        pi->type = 0x1027;
        pi->one = one;
        nm->e = gp[G - 1] + gl[G - 1];
        nm->fe = fe;
        nm->frag = frag;
        if (country) {
            const wc *sp = nm->p, *se = nm->e;
            int r;
            nm->p = country;
            nm->e = country + clen;
            r = parse_number(nm, (int **)&pi->country, "NUMBER", 0);
            nm->p = sp;
            nm->e = se;
            if (r < 0) return r;
        }
        if (area) {
            pi->area = amalloc(nm, sizeof(wspan));
            if (!pi->area) return -2;
            pi->area->s = area;
            pi->area->n = alen;
        }
        pi->ngroups = G;
        pi->groups = amalloc(nm, sizeof(wspan) * (size_t)(G ? G : 1));
        if (!pi->groups) return -2;
        for (i = 0; i < G; i++) {
            pi->groups[i].s = gp[i];
            pi->groups[i].n = gl[i];
        }
        if (country) {
            wl_str(nm, l, "country");
            wl_str(nm, l, "code");
            wl_cat(nm, l, &pi->country->words);
        }
        if (one) wl_str(nm, l, "one");
        if (area) {
            int f[4] = {0};
            if ((area[0] == '8' || area[0] == '9') && area[1] == '0' && area[2] == '0') {
                pi->is800 = 1;
                w_three(nm, area, f, l);
            } else {
                wl_str(nm, l, "area");
                wl_str(nm, l, "code");
                for (k = 0; k < alen; k++) w_digit(nm, area[k], f, l);
            }
        }
        for (i = 0; i < G; i++) {
            int f[4] = {0};
            for (k = 0; k < gl[i]; k++) w_digit(nm, gp[i][k], f, l);
        }
        *out = (int *)pi;
    }
    return 0;
}

/* ---- dates ---- */

typedef struct {
    int type;
    int weekday; /* 0x1015 records use this slot as 0 */
    int month;
    const wc *day;
    int daylen;
    const wc *year;
    int yearlen;
} dateinfo;

static int date_sep(const wc **p) /* FUN_5ed48318: '/', '-', '.' */
{
    if (**p == '/' || **p == '-' || **p == '.') {
        (*p)++;
        return 1;
    }
    return 0;
}

static const char *const MONTHS[13] = {"", "January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December"};
static const char *const WEEKDAYS[8] = {"", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

static int month_parse(const wc **p, int avail) /* FUN_5ed48188 */
{
    static const char *const AB[13] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sept", "sep", "oct", "nov", "dec"};
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
            if (**p == '.') (*p)++;
            return m;
        }
    }
    return 0;
}

static int weekday_parse(const wc **p, const wc *end) /* FUN_5ed4822f */
{
    static const char *const AB[10] = {"Mon", "Tues", "Tue", "Wed", "Thurs", "Thur", "Thu", "Fri", "Sat", "Sun"};
    static const int IDX[10] = {1, 2, 2, 3, 4, 4, 4, 5, 6, 7};
    int k;
    for (k = 1; k <= 7; k++) {
        int l = (int)strlen(WEEKDAYS[k]);
        if (l <= end - *p && !wnicmp_a(*p, WEEKDAYS[k], l)) {
            *p += l;
            return k;
        }
    }
    for (k = 0; k < 10; k++) {
        int l = (int)strlen(AB[k]);
        if (l <= end - *p && !wncmp_a(*p, AB[k], l)) {
            *p += l;
            if (**p == '.') (*p)++;
            return IDX[k];
        }
    }
    return 0;
}

static dateinfo *new_date(sam_norm *nm, int month)
{
    dateinfo *d = amalloc(nm, sizeof *d);
    if (!d) return NULL;
    d->type = 0x1015;
    d->month = month;
    return d;
}

static void set_day(dateinfo *d, const wc *s, int n) /* day digits: a leading zero is skipped */
{
    if (*s == '0') {
        d->day = s + 1;
        d->daylen = 1;
    } else {
        d->day = s;
        d->daylen = n;
    }
}

/* FUN_5ed48379: numeric dates "12/25/2004" (month-day-year order, then day-month-year, year-month-day) */
static int h_date_num(sam_norm *nm, int **out)
{
    const wc *p = nm->p, *a_end, *b, *b_end, *c, *c_end;
    long A, B, C;
    int n = (int)(nm->e - p), ad, bd, cd, mdy = 1, dmy = 0;
    dateinfo *d;
    if (n > 10) return DECLINE;
    A = wnum(p, &a_end);
    if (p == a_end || a_end - p > 4) return DECLINE;
    b = a_end;
    if (!date_sep(&b)) return DECLINE;
    B = wnum(b, &b_end);
    if (b == b_end || b_end - b > 4) return DECLINE;
    c = b_end;
    if (*b_end != *a_end || !date_sep(&c)) return DECLINE;
    C = wnum(c, &c_end);
    if (c == c_end || c_end != p + n) return DECLINE;
    cd = (int)(c_end - c);
    if (cd > 4) return DECLINE;
    ad = (int)(a_end - p);
    bd = (int)(b_end - b);
    /* the default order is month-day-year */
    if (A != 0 && A < 13 && ad < 4 && B != 0 && B < 32 && bd < 4 && C < 10000 && cd > 1) {
        mdy = 1;
    } else if (A != 0 && A < 32 && ad < 4 && B != 0 && B < 13 && bd < 4 && C < 10000 && cd > 1) {
        mdy = 0;
        dmy = 1;
    } else {
        if (A > 9999 || ad < 2 || B == 0 || B > 12 || bd > 3 || C == 0 || C > 31 || cd > 3) return DECLINE;
        mdy = 0;
    }
    if (mdy) {
        d = new_date(nm, (int)A);
        if (!d) return -2;
        set_day(d, b, bd);
        d->year = c;
        d->yearlen = cd;
    } else if (dmy) {
        d = new_date(nm, (int)B);
        if (!d) return -2;
        set_day(d, p, ad);
        d->year = c;
        d->yearlen = cd;
    } else {
        d = new_date(nm, (int)B);
        if (!d) return -2;
        set_day(d, c, cd);
        d->year = p;
        d->yearlen = ad;
    }
    *out = (int *)d;
    return 0;
}

/* FUN_5ed48cdc: dates with a month name inside the token: "25-Dec-2004", "Dec-25-2004", "Dec-2004" */
static int h_date_name(sam_norm *nm, int **out)
{
    const wc *p = nm->p, *q, *c, *cend;
    int n = (int)(nm->e - p);
    dateinfo *d;
    if (n > 17) return DECLINE;
    if (!w_isalpha(p[0])) {
        long A, C;
        int alen, month, clen;
        if (!n_isdigit(p[0])) return DECLINE;
        A = wnum(p, &q);
        alen = (int)(q - p);
        if (alen > 4 || !date_sep(&q)) return DECLINE;
        c = q;
        month = month_parse(&c, n - alen);
        if (!month) return DECLINE;
        if (date_sep(&c)) {
            C = wnum(c, &cend);
            if (c == cend) return DECLINE;
            clen = (int)(cend - c);
            if (clen > 4) return DECLINE;
            if (A == 0 || A > 31 || alen > 2 || C > 9999 || clen < 2) {
                if (A > 9999 || alen < 2 || C == 0 || C > 31 || clen > 2) return DECLINE;
                d = new_date(nm, month); /* year-month-day */
                if (!d) return -2;
                if (clen == 2) set_day(d, c, 2);
                else {
                    d->day = c;
                    d->daylen = 1;
                }
                d->year = p;
                d->yearlen = alen;
            } else {
                d = new_date(nm, month); /* day-month-year */
                if (!d) return -2;
                if (alen == 2) set_day(d, p, 2);
                else {
                    d->day = p;
                    d->daylen = 1;
                }
                d->year = c;
                d->yearlen = clen;
            }
        } else {
            d = new_date(nm, month);
            if (!d) return -2;
            if (A == 0 || A > 31 || alen > 2) {
                if (A > 9999 || alen > 4) return DECLINE;
                d->year = p; /* month-year */
                d->yearlen = alen;
            } else if (alen == 2) {
                set_day(d, p, 2);
            } else {
                d->day = p;
                d->daylen = 1;
            }
        }
    } else {
        long B, C;
        int month, blen, clen;
        q = p;
        month = month_parse(&q, n);
        if (!month || !date_sep(&q)) return DECLINE;
        B = wnum(q, &c);
        if (q == c) return DECLINE;
        blen = (int)(c - q);
        d = new_date(nm, month);
        if (!d) return -2;
        if (blen > 2) {
            if (blen > 4 || B > 9999) return DECLINE;
            d->year = q;
            d->yearlen = blen;
        } else if (date_sep(&c)) {
            C = wnum(c, &cend);
            if (c == cend) return DECLINE;
            clen = (int)(cend - c);
            if (clen > 4 || B == 0 || B > 31 || blen > 2 || C > 9999 || clen < 2) return DECLINE;
            if (blen == 2) set_day(d, q, 2);
            else {
                d->day = q;
                d->daylen = 1;
            }
            d->year = c;
            d->yearlen = clen;
        } else if (B == 0 || B > 31 || blen > 2) {
            d->year = q;
            d->yearlen = blen;
        } else if (blen == 2) {
            set_day(d, q, 2);
        } else {
            d->day = q;
            d->daylen = 1;
        }
    }
    *out = (int *)d;
    return 0;
}

/* next token for a date, trailing punctuation stripped; *stop = punctuation other than ',' seen */
static const wc *date_next(sam_norm *nm, const wc *from, const wc **tend, int *stop)
{
    const wc *q = skip_ws(from, nm->fe), *e;
    if (!q) return NULL;
    e = token_end(q, nm->fe);
    for (;;) {
        wc ch = e[-1];
        if (!is_punct_any(ch)) break;
        e--;
        if (stop && ch != ',') *stop = 1;
    }
    *tend = e;
    return q;
}

/* FUN_5ed497a0: "[Weekday,] Month Day[,] [Year]" */
static int h_date_mdy_words(sam_norm *nm, int **out, wlist *l)
{
    const wc *p = nm->p, *e = nm->e, *next = p, *q, *tend = e, *day, *dend, *year = NULL;
    const wc *fe = nm->fe;
    int frag = nm->frag, weekday, month, stop = 0, daylen = 0, yearlen = 0;
    long v;
    dateinfo *d;
    q = p;
    weekday = weekday_parse(&q, e);
    if (weekday) {
        if (!(q == e || (q == e - 1 && *e == ','))) return DECLINE;
        next = skip_ws(*e == ',' ? e + 1 : e, fe);
        if (!next) return DECLINE;
        tend = token_end(next, fe);
    }
    q = next;
    month = month_parse(&q, (int)(tend - next));
    if (!month || (q != tend && (q != tend - 1 || *q != ','))) return DECLINE;
    day = date_next(nm, tend, &tend, &stop);
    if (!day) return DECLINE;
    v = wnum(day, &dend);
    if (v < 1 || v > 31 || dend - day > 2) {
        if (v < 0 || v > 9999 || dend - day > 4 || dend != tend) return DECLINE;
        year = day;
        yearlen = (int)(tend - day);
        day = NULL;
        stop = 1;
    } else {
        if (dend == tend) daylen = (int)(tend - day);
        else if (dend == tend - 1 && *dend == ',') daylen = (int)(tend - day) - 1;
        else {
            if (v < 0 || v > 9999 || dend - day > 4 || dend != tend) return DECLINE;
            year = day;
            yearlen = (int)(tend - day);
            day = NULL;
            stop = 1;
        }
        if (day && !stop) {
            const wc *yq, *yend, *ytend;
            yq = skip_ws(*tend == ',' ? tend + 1 : tend, fe);
            if (yq) {
                ytend = strip_trailing(token_end(yq, fe));
                v = wnum(yq, &yend);
                if (!(v < 0 || v > 9999 || yend - yq > 4 || yend != ytend)) {
                    year = yq;
                    yearlen = (int)(ytend - yq);
                    tend = ytend;
                }
            }
        }
    }
    d = amalloc(nm, sizeof *d);
    if (!d) return -2;
    d->type = 0x1016;
    if (weekday) wl_str(nm, l, WEEKDAYS[weekday]);
    wl_str(nm, l, MONTHS[month]);
    {
        int f[4] = {0};
        if (day && daylen == 1) w_ord1(nm, day[0], f, l);
        else if (day && daylen == 2) w_ord2(nm, day, f, l);
    }
    if (year) year_words(nm, year, yearlen, l);
    (void)frag;
    nm->e = tend;
    *out = (int *)d;
    return 0;
}

/* FUN_5ed49d49: "[Weekday,] Day[,] Month[,] [Year]" */
static int h_date_dmy_words(sam_norm *nm, int **out, wlist *l)
{
    const wc *p = nm->p, *e = nm->e, *next = p, *q, *tend = e, *dend, *year = NULL, *mstart, *mt;
    const wc *fe = nm->fe;
    int weekday, month, stop = 0, daylen, yearlen = 0;
    long v;
    dateinfo *d;
    q = p;
    weekday = weekday_parse(&q, e);
    if (weekday) {
        if (!(q == e || (q == e - 1 && *e == ','))) return DECLINE;
        next = skip_ws(*e == ',' ? e + 1 : e, fe);
        if (!next) return DECLINE;
        tend = token_end(next, fe);
    }
    v = wnum(next, &dend);
    if (v < 1 || v > 31 || dend - next > 2) return DECLINE;
    if (dend == tend) daylen = (int)(tend - next);
    else if (dend == tend - 1 && *dend == ',') daylen = (int)(tend - next) - 1;
    else return DECLINE;
    mstart = date_next(nm, *tend == ',' ? tend + 1 : tend, &mt, &stop);
    if (!mstart) return DECLINE;
    q = mstart;
    month = month_parse(&q, (int)(mt - mstart));
    if (!month || (q != mt && (q != mt - 1 || *q != ','))) return DECLINE;
    tend = mt;
    if (!stop) {
        const wc *yq = skip_ws(*mt == ',' ? mt + 1 : mt, fe), *yend, *ytend;
        if (yq) {
            ytend = strip_trailing(token_end(yq, fe));
            v = wnum(yq, &yend);
            if (!(v < 0 || v > 9999 || yend - yq > 4 || yend != ytend)) {
                year = yq;
                yearlen = (int)(ytend - yq);
                tend = ytend;
            }
        }
    }
    d = amalloc(nm, sizeof *d);
    if (!d) return -2;
    d->type = 0x1016;
    if (weekday) wl_str(nm, l, WEEKDAYS[weekday]);
    wl_str(nm, l, MONTHS[month]);
    {
        int f[4] = {0};
        if (daylen == 1) w_ord1(nm, next[0], f, l);
        else if (daylen == 2) w_ord2(nm, next, f, l);
    }
    if (year) year_words(nm, year, yearlen, l);
    nm->e = tend;
    *out = (int *)d;
    return 0;
}

static void date_words(sam_norm *nm, const dateinfo *d, wlist *l) /* FUN_5ed4a2c9 */
{
    int f[4] = {0};
    if (d->weekday) wl_str(nm, l, WEEKDAYS[d->weekday]);
    wl_str(nm, l, MONTHS[d->month]);
    if (d->day) {
        if (d->daylen == 1) w_ord1(nm, d->day[0], f, l);
        else if (d->daylen == 2) w_ord2(nm, d->day, f, l);
    }
    if (d->year) year_words(nm, d->year, d->yearlen, l);
}


/* ---- US state + ZIP code (FUN_5ed4cf14, ZIP FUN_5ed4f834, words FUN_5ed50cd9) ---- */

static const char *const STATES[63][2] = {
    {"AA", "Armed Forces"}, {"AE", "Armed Forces"}, {"AK", "Alaska"}, {"AL", "Alabama"}, {"AP", "Armed Forces"},
    {"AR", "Arkansas"}, {"AS", "American Samoa"}, {"AZ", "Arizona"}, {"CA", "California"}, {"CO", "Colorado"},
    {"CT", "Connecticut"}, {"DC", "D C"}, {"DE", "Deleware"}, {"FL", "Florida"},
    {"FM", "Federated States Of Micronesia"}, {"GA", "Georgia"}, {"GU", "Guam"}, {"HI", "Hawaii"}, {"IA", "Iowa"},
    {"ID", "Idaho"}, {"IL", "Illinois"}, {"IN", "Indiana"}, {"KS", "Kansas"}, {"KY", "Kentucky"},
    {"LA", "Louisiana"}, {"MA", "Massachusetts"}, {"MD", "Maryland"}, {"ME", "Maine"}, {"MH", "Marshall Islands"},
    {"MI", "Michigan"}, {"MN", "Minnesota"}, {"MO", "Missouri"}, {"MP", "Northern Mariana Islands"},
    {"MS", "Mississippi"}, {"MT", "Montana"}, {"NC", "North Carolina"}, {"ND", "North Dakota"}, {"NE", "Nebraska"},
    {"NH", "New Hampshire"}, {"NJ", "New Jersey"}, {"NM", "New Mexico"}, {"NV", "Nevada"}, {"NY", "New York"},
    {"OH", "Ohio"}, {"OK", "Oklahoma"}, {"OR", "Oregon"}, {"PA", "Pennsylvania"}, {"PR", "Puerto Rico"},
    {"PW", "Palau"}, {"RI", "Rhode Island"}, {"SC", "South Carolina"}, {"SD", "South Dakota"}, {"TN", "Tennessee"},
    {"TX", "Texas"}, {"UT", "Utah"}, {"VA", "Virginia"}, {"VI", "Virgin Islands"}, {"VT", "Vermont"},
    {"WA", "Washington"}, {"WI", "Wisconsin"}, {"WV", "West Virginia"}, {"WY", "Wyoming"}, {"", ""}};

typedef struct {
    int type;
    const wc *zip;
    const wc *plus4;
} zipinfo;

static int h_zip(sam_norm *nm, int **out) /* FUN_5ed4f834 */
{
    const wc *p = nm->p;
    int n = (int)(nm->e - p), k, plus4 = 0;
    zipinfo *z;
    if (n != 5 && n != 10) return DECLINE;
    for (k = 0; k < 5; k++)
        if (!w_isdigit(p[k])) return DECLINE;
    if (k < n) {
        if (p[k] != '-') return DECLINE;
        for (k = 0; k < 4; k++) /* the engine checks p[0..3] here */
            if (!w_isdigit(p[k])) return DECLINE;
        plus4 = 1;
    }
    z = amalloc(nm, sizeof *z);
    if (!z) return -2;
    z->type = 0x1013;
    z->zip = p;
    z->plus4 = plus4 ? p + 6 : NULL;
    *out = (int *)z;
    return 0;
}

typedef struct {
    int type;
    zipinfo *zip;
} addrinfo;

static int h_state_zip(sam_norm *nm, int **out, wlist *l)
{
    const wc *p0 = nm->p, *fe0 = nm->fe, *e0 = nm->e, *q;
    int frag0 = nm->frag, lo = 0, hi = 62, found = -1, r;
    zipinfo *z = NULL;
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
    q = e0;
    if (*q == ',' || *q == ';') q++;
    q = skip_ws(q, nm->fe);
    if (!q) {
        nm->p = p0;
        return DECLINE;
    }
    nm->p = q;
    nm->e = strip_trailing(token_end(q, nm->fe));
    r = h_zip(nm, (int **)&z);
    if (r < 0) {
        nm->p = p0;
        nm->fe = fe0;
        nm->e = e0;
        nm->frag = frag0;
        return DECLINE;
    }
    {
        addrinfo *a = amalloc(nm, sizeof *a);
        int f[4] = {0}, k;
        if (!a) return -2;
        a->type = 0x101c;
        a->zip = z;
        wl_words(nm, l, STATES[found][1]);
        for (k = 0; k < 5; k++) w_digit(nm, z->zip[k], f, l);
        if (z->plus4) {
            wl_str(nm, l, "dash");
            for (k = 0; k < 4; k++) w_digit(nm, z->plus4[k], f, l);
        }
        *out = (int *)a;
    }
    nm->p = p0;
    return 0;
}

/* ---- time ranges "9am-5pm" (FUN_5ed57056) ---- */

typedef struct {
    int type;
    int *from, *to;
} trangeinfo;

static int h_time_range(sam_norm *nm, int **out, wlist *l)
{
    const wc *p0 = nm->p, *fe0 = nm->fe, *e0 = nm->e, *dash;
    int frag0 = nm->frag, patched = 0, r;
    int *t1 = NULL, *t2 = NULL;
    wlist w = {0};
    wc *pd = NULL;
    for (dash = p0; dash < e0 && *dash != '-'; dash++) {
    }
    if (dash == e0) {
        const wc *nq = skip_ws(dash, nm->fe);
        const wc *d2 = NULL;
        if (nq) {
            if (!wnicmp_a(nq, "am", 2) && nq[2] == '-') d2 = nq + 2;
            else if (!wnicmp_a(nq, "pm", 2) && nq[2] == '-') d2 = nq + 2;
            else if (!wnicmp_a(nq, "a.m.", 4) && nq[4] == '-') d2 = nq + 4;
            else if (!wnicmp_a(nq, "p.m.", 4) && nq[4] == '-') d2 = nq + 4;
        }
        if (!d2) return DECLINE;
        pd = (wc *)d2;
        *pd = ' ';
        patched = 1;
        dash = d2;
    }
    if (nm->p < dash && dash < nm->e) nm->e = dash;
    r = h_clock(nm, &t1, &w, patched);
    if (r == DECLINE && dash <= nm->p + 2) {
        const wc *q;
        long v = wnum(nm->p, &q);
        if (q == dash && v > 0 && v < 24) {
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
            trangeinfo *ti = amalloc(nm, sizeof *ti);
            if (!ti) return -2;
            nm->p = p0;
            nm->fe = fe0;
            ti->type = 0x101d;
            ti->from = t1;
            ti->to = t2;
            wl_cat(nm, l, &w);
            *out = (int *)ti;
            return 0;
        }
    }
    nm->p = p0;
    nm->fe = fe0;
    nm->e = e0;
    nm->frag = frag0;
    if (patched) *pd = '-';
    return r < 0 ? r : DECLINE;
}


/* ---- currency ranges "$5-$10" (FUN_5ed5335f) ---- */

typedef struct {
    int type;
    int *left, *right;
} crangeinfo;

/* a scratch copy "text + symbol" or "symbol + text" (the engine allocates it on the sentence heap) */
static wc *cat_span(sam_norm *nm, const wc *a, int an, const wc *b, int bn)
{
    wc *s = amalloc(nm, sizeof(wc) * (size_t)(an + bn + 1));
    if (!s) return NULL;
    memcpy(s, a, sizeof(wc) * (size_t)an);
    memcpy(s + an, b, sizeof(wc) * (size_t)bn);
    s[an + bn] = 0;
    return s;
}

static int h_currency_range(sam_norm *nm, int **out, wlist *l)
{
    const wc *p0 = nm->p, *e0 = nm->e, *fe0 = nm->fe, *dash;
    int sfx = 2, cur, sfx2 = 0, cur2, r = DECLINE;
    int *left = NULL, *right = NULL;
    wlist w = {0};
    wc *pd;
    cur = cur_match(&nm->p, &nm->e, &sfx);
    if (cur < 0) goto done;
    for (dash = nm->p; dash < nm->e && *dash != '-'; dash++) {
    }
    if (!(*dash == '-' && nm->p < dash && dash < nm->e - 1)) goto done;
    pd = (wc *)dash;
    *pd = ' ';
    nm->p = p0;
    nm->e = dash;
    cur2 = cur_match(&nm->p, &nm->e, &sfx2);
    if (cur2 >= 0 && cur2 != cur) goto undo;
    r = parse_number(nm, &left, "NUMBER", 0);
    if (r < 0) goto undo;
    nm->p = dash + 1;
    nm->e = e0;
    cur2 = cur_match(&nm->p, &nm->e, &sfx2);
    r = parse_number(nm, &right, "NUMBER", 0);
    if (r < 0) goto undo;
    if (*left == 0x1006 && *right == 0x1006) {
        wl_cat(nm, &w, &((numinfo *)left)->words);
    } else {
        nm->p = p0;
        nm->e = dash;
        if (sfx == 1) {
            if (cur2 < 0) {
                int sl = wlen16(CUR_SYM[cur]);
                wc *b = cat_span(nm, nm->p, (int)(nm->e - nm->p), CUR_SYM[cur], sl);
                if (!b) return -2;
                nm->p = b;
                nm->e = b + wlen16(b);
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
    if (sfx == 0) {
        int s3 = 2, k = cur_match(&nm->p, &nm->e, &s3);
        if (k < 0) {
            int sl = wlen16(CUR_SYM[cur]);
            wc *b = cat_span(nm, CUR_SYM[cur], sl, nm->p, (int)(nm->e - nm->p));
            if (!b) return -2;
            nm->p = b;
            nm->e = b + wlen16(b);
            nm->fe = nm->e;
        } else if (k == cur) {
            nm->p = dash + 1;
            nm->e = e0;
        } else {
            r = DECLINE;
            goto undo;
        }
    }
    r = h_currency(nm, &right, &w);
    if (r >= 0) {
        crangeinfo *ci = amalloc(nm, sizeof *ci);
        if (!ci) return -2;
        ci->type = 0x1028;
        ci->left = left;
        ci->right = right;
        wl_cat(nm, l, &w);
        *out = (int *)ci;
    }
undo:
    *pd = '-';
done:
    nm->p = p0;
    nm->e = e0;
    nm->fe = fe0;
    return r;
}

/* FUN_5ed4c45e without XML context (the XML "say-as" handlers are not ported) */
static int dispatch(sam_norm *nm, int **ti, wlist *direct)
{
    int r;
    if (h_word(nm->p, nm->e) == 0) {
        typeinfo *t = amalloc(nm, sizeof *t);
        if (!t) return -2;
        t->type = 0x1002;
        *ti = (int *)t;
        r = h_date_mdy_words(nm, ti, direct);
        if (r != DECLINE) return r;
        r = h_date_dmy_words(nm, ti, direct);
        if (r != DECLINE) return r;
        r = h_state_zip(nm, ti, direct);
        if (r != DECLINE) return r;
        r = h_currency(nm, ti, direct);
        if (r != DECLINE) return r;
        *ti = (int *)t;
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
    r = h_date_num(nm, ti);
    if (r != DECLINE) return r;
    r = h_date_name(nm, ti);
    if (r != DECLINE) return r;
    r = h_decade(nm, ti);
    if (r != DECLINE) return r;
    r = h_duration(nm, ti);
    if (r != DECLINE) return r;
    r = h_hyphen(nm, nm->p, nm->e, ti);
    if (r != DECLINE) return r;
    r = h_dashword(nm, nm->p, nm->e, ti);
    if (r != DECLINE) return r;
    if (!*ti) {
        typeinfo *t = amalloc(nm, sizeof *t);
        if (!t) return -2;
        t->type = 0x1001;
        *ti = (int *)t;
    }
    return 0;
}

/* FUN_5ed4d35f: read a mixed token piece by piece */
static void spell_words(sam_norm *nm, wlist *l)
{
    const wc *s = nm->p, *end = nm->e, *q;
    int rep = 0;
    wc last = 0;
    if (!wnicmp_a(s, "AT&T", (int)(end - s))) {
        wl_span(nm, l, s, 1);
        wl_span(nm, l, s + 1, 1);
        wl_str(nm, l, "and");
        wl_span(nm, l, s + 3, 1);
        return;
    }
    while (s < end) {
        wc c = *s;
        if (!w_isalpha(c)) {
            if (!n_isdigit(c)) {
                const char *name = c < 0x101 ? symbol_name(c) : NULL;
                if (name) {
                    int skip = 0;
                    if (rep == 0) {
                        rep = 1;
                        last = c;
                    } else {
                        if (last == c) rep++;
                        else {
                            rep = 1;
                            last = c;
                        }
                        if (rep > 3) skip = 1;
                    }
                    if (!skip) wl_words(nm, l, name);
                }
                s++;
            } else {
                const wc *sp = nm->p, *se = nm->e;
                int *ti = NULL;
                rep = 0;
                q = s;
                do q++;
                while (q < end && n_isdigit(*q));
                nm->p = s;
                nm->e = q;
                if (parse_number(nm, &ti, "NUMBER", 0) >= 0) wl_cat(nm, l, &((numinfo *)ti)->words);
                nm->p = sp;
                nm->e = se;
                s = q;
            }
        } else {
            rep = 0;
            q = s;
            do q++;
            while (q < end && w_isalpha(*q));
            wl_span(nm, l, s, (int)(q - s));
            s = q;
        }
    }
}

static void hyphen_words(sam_norm *nm, const hypheninfo *h, wlist *l) /* FUN_5ed4d17c */
{
    if (*h->left == 0x1002) wl_span(nm, l, h->ls, (int)(h->rs - h->ls) - 1);
    else wl_cat(nm, l, &((const numinfo *)h->left)->words);
    if (*h->right == 0x1002) wl_span(nm, l, h->rs, (int)(nm->e - h->rs));
    else if (*h->right == 0x101b) hyphen_words(nm, (const hypheninfo *)h->right, l);
    else wl_cat(nm, l, &((const numinfo *)h->right)->words);
}

/* FUN_5ed4c8fb: words of a classified token */
static void type_words(sam_norm *nm, const int *ti, wlist *l)
{
    const numinfo *ni = (const numinfo *)ti;
    switch (*ti) {
    case 0x1006: case 0x1007: case 0x1008: case 0x100e: case 0x100f:
        wl_cat(nm, l, &ni->words);
        break;
    case 0x1009:
        wl_cat(nm, l, &ni->words);
        wl_str(nm, l, "percent");
        break;
    case 0x100a: { /* FUN_5ed503b7 */
        const char *w = "degree";
        wl_cat(nm, l, &ni->words);
        if (ni->dp || ni->fr || !ni->ip || ni->ip->end - ni->ip->start != 1 || ni->ip->start[0] != '1') {
            w = "degrees";
            if (!ni->ip && ni->fr && ni->fr->over == 0) {
                wl_str(nm, l, "of");
                wl_str(nm, l, "a");
                w = "degree";
            }
        }
        wl_str(nm, l, w);
        break;
    }
    case 0x100b:
        wl_cat(nm, l, &ni->words);
        wl_str(nm, l, "squared");
        break;
    case 0x100c:
        wl_cat(nm, l, &ni->words);
        wl_str(nm, l, "cubed");
        break;
    case 0x1014: {
        const yearinfo *y = (const yearinfo *)ti;
        year_words(nm, y->s, y->len, l);
        break;
    }
    case 0x101b:
        hyphen_words(nm, (const hypheninfo *)ti, l);
        break;
    case 0x1015:
        date_words(nm, (const dateinfo *)ti, l);
        break;
    case 0x1017:
        decade_words(nm, (const decadeinfo *)ti, l);
        break;
    case 0x1019:
        duration_words(nm, (const durinfo *)ti, l);
        break;
    case 0x101e: { /* FUN_5ed50d4f */
        const rangeinfo *ri = (const rangeinfo *)ti;
        if (*ri->left == 0x1014) type_words(nm, ri->left, l);
        else wl_cat(nm, l, &((const numinfo *)ri->left)->words);
        wl_str(nm, l, "to");
        type_words(nm, ri->right, l);
        break;
    }
    case 0x1029: { /* FUN_5ed4d243 */
        const dashinfo *d = (const dashinfo *)ti;
        int k;
        for (k = 0; k < d->len; k++) wl_str(nm, l, symbol_name(d->s[k]));
        break;
    }
    default:
        spell_words(nm, l);
        break;
    }
}

/* FUN_5ed4cb02 */
static int classify(sam_norm *nm, pnode *core)
{
    int *ti = (int *)core->ti, r = 0;
    wlist direct = {0};
    if (!ti || (*ti != SAM_NODE_ABBREV && *ti != 0x1005)) {
        r = dispatch(nm, &ti, &direct);
        if (r < 0) return r;
    }
    switch (*ti) {
    case 0x1003: case 0x1004: case 0x1005:
        if (ti != core->ti) break; /* a handler replaced it: fall through below */
        return 0;
    default:
        break;
    }
    if (*ti == 0x1027 && ((const phoneinfo *)ti)->area && nm->npn > 0 && nm->pn[nm->npn - 1].type == 1) {
        nm->npn--; /* "(425) 555-0100": the parenthesis belongs to the number */
        nm->p--;
    }
    core->type = *ti;
    core->ti = ti;
    core->s = nm->p;
    core->len = (int)(nm->e - nm->p);
    core->ofs = (int)(nm->p - nm->buf);
    core->words.n = 0;
    switch (*ti) {
    case 0x1002:
        wl_span(nm, &core->words, nm->p, core->len);
        break;
    case 0x100d: case 0x1016: case 0x1018: case 0x101c: case 0x101d: case 0x1028: case 0x1027:
        wl_cat(nm, &core->words, &direct);
        break;
    default:
        type_words(nm, ti, &core->words);
        break;
    }
    return 0;
}

/* ============================================================================================== */
/* tokenizer (FUN_5ed5521e)                                                                       */

static int next_token(sam_norm *nm, int *sent_end)
{
    const wc *p, *te;
    wc *q;
    pnode core, trail[64];
    int nt = 0, ntrail = 0, local_24 = 0, local_14 = 0, local_10 = 1, r = 0, k;
    *sent_end = 0;
    p = skip_ws(nm->p, nm->fe);
    if (!p) {
        nm->p = NULL;
        nm->frag = 0;
        return 0;
    }
    te = token_end(p, nm->fe);
    for (q = (wc *)p; q < te; q++) *q = FOLD[to_cp1252(*q) & 0xff];
    te = token_end(p, nm->fe);
    nm->te = te;
    while (p < te) {
        int t = open_type(*p);
        if (t == 0x1001) t = quote_type(*p);
        if (t == 0x1001) break;
        if (!pn_add(nm, t, punct_pos(t), p, 1)) return -2;
        p++;
    }
    nm->p = p;
    nm->e = te;
    memset(&core, 0, sizeof core);
    while (nm->p <= nm->e - 1 && local_10) {
        wc c = nm->e[-1];
        int t;
        local_10 = 0;
        local_14 = 0;
        t = close_type(c);
        if (t == 0x1001) t = quote_type(c);
        if (t == 0x1001) t = clause_type(c);
        if (t != 0x1001) {
            local_10 = 1;
            if (t == 0xc || t == 0xe || t == 0xd) local_24 = 1;
            goto emit;
        }
        t = sent_type(c);
        if (t == 0x1001) continue;
        if (t == 9) {
            if (nm->p <= nm->e - 2 && w_isalpha(nm->e[-2])) {
                r = h_initials(nm, &core, sent_end);
                if (r < 0) {
                    const sam_abbrev *a;
                    const wc *x;
                    if (r != DECLINE) return r;
                    a = abbrev_find(nm->p, (int)(nm->e - 1 - nm->p));
                    if (a) {
                        if (a->shandler < 0) {
                            *sent_end = 0;
                            r = make_abbrev(nm, &core, a);
                        } else {
                            r = abbrev_sentence(nm, a->shandler, a, &core, sent_end);
                            if (r >= 0 && *sent_end) {
                                if (!local_24) {
                                    local_10 = 1;
                                    local_14 = 1;
                                } else {
                                    *sent_end = 0;
                                }
                            }
                        }
                        if (r != DECLINE) {
                            if (r < 0) return r;
                            goto e7;
                        }
                    }
                    for (x = nm->p; x < nm->e - 1; x++) {
                        if (*x == '.') {
                            *sent_end = 0;
                            break;
                        }
                    }
                    if (x != nm->e - 1 || local_24) {
                        r = 0;
                        goto e7;
                    }
                    r = 0;
                    local_10 = 1;
                    *sent_end = 1;
                    goto emit;
                } else if (*sent_end) {
                    if (!local_24) {
                        local_10 = 1;
                        local_14 = 1;
                        goto emit;
                    }
                    *sent_end = 0;
                }
                continue;
            }
            if (nm->e == te && nm->p <= nm->e - 2 && sent_type(nm->e[-2]) == 9 && nm->e - 3 == nm->p &&
                sent_type(nm->e[-3]) == 9) {
                local_10 = 1;
                t = 0x10;
                goto emit;
            }
            t = 9;
        }
        local_10 = 1;
        *sent_end = 1;
        goto emit;
    e7:
        if (!local_10) continue;
        t = 9;
    emit:
        ntrail++;
        if (nt < 64) {
            pnode *tn = &trail[nt++];
            memset(tn, 0, sizeof *tn);
            tn->type = t;
            tn->pos = punct_pos(t);
            tn->s = t == 0x10 ? nm->e - 3 : nm->e - 1;
            tn->len = t == 0x10 ? 3 : 1;
            tn->ofs = (int)(tn->s - nm->buf);
        }
        if (local_14) break;
        if (t == 0x10) {
            nm->e -= 3;
            ntrail = 3;
            continue;
        }
        nm->e -= 1;
    }
    if (nm->p != nm->e) {
        r = classify(nm, &core);
        if (r < 0) return r;
    }
    if (core.type) {
        pnode *n = pn_add(nm, core.type, 0, core.s, core.len);
        if (!n) return -2;
        *n = core;
    }
    if (!local_14 && nm->e + ntrail != te) {
        nm->p = nm->e;
    } else {
        for (k = nt - 1; k >= 0; k--) {
            pnode *n = pn_add(nm, trail[k].type, trail[k].pos, trail[k].s, trail[k].len);
            if (!n) return -2;
        }
        nm->p = te;
    }
    if (nm->p >= nm->fe) nm->frag = 0;
    return 0;
}

/* ============================================================================================== */
/* sentences (FUN_5ed55a9a)                                                                       */

static void to_chars(const wc *s, int len, char *out, int cap)
{
    int i, k = 0;
    for (i = 0; i < len && k < cap - 1; i++) out[k++] = (char)(s[i] < 0x100 ? s[i] : '?');
    out[k] = 0;
}

static int emit_sentence(sam_norm *nm, sam_sentence *o)
{
    int i, k;
    o->nn = 0;
    o->ne = 0;
    for (i = 0; i < nm->npn; i++) {
        const pnode *p = &nm->pn[i];
        sam_node *n;
        if (o->nn == o->ncap) {
            int cap = o->ncap ? o->ncap * 2 : 64;
            sam_node *x = realloc(o->nodes, sizeof(sam_node) * (size_t)cap);
            if (!x) return -1;
            o->nodes = x;
            o->ncap = cap;
        }
        n = &o->nodes[o->nn++];
        memset(n, 0, sizeof *n);
        n->type = p->type;
        n->pos = p->pos;
        if (p->s) to_chars(p->s, p->len, n->text, (int)sizeof n->text);
        else strcpy(n->text, ".");
        n->ofs = p->ofs;
        n->len = p->len;
        n->ti = p->ti;
        n->first = o->ne;
        if (!(p->type & 0x1000)) continue;
        for (k = 0; k < p->words.n; k++) {
            const nword *w = &p->words.w[k];
            sam_tag_entry *x;
            if (o->ne == o->ecap) {
                int cap = o->ecap ? o->ecap * 2 : 64;
                sam_tag_entry *e = realloc(o->e, sizeof(sam_tag_entry) * (size_t)cap);
                if (!e) return -1;
                o->e = e;
                o->ecap = cap;
            }
            x = &o->e[o->ne++];
            memset(x, 0, sizeof *x);
            if (w->s) {
                strncpy(x->text, w->s, sizeof x->text - 1);
            } else {
                to_chars(w->t, w->len > 0x7f ? 0x7f : w->len, x->text, (int)sizeof x->text);
            }
            x->lock = w->fpos;
            x->pos = w->fpos;
            n->count++;
        }
    }
    o->end_ofs = nm->n;
    return 0;
}

int sam_norm_next(sam_norm *nm, const sam_lexicon *lex, const sam_lts *lts, sam_sentence *out)
{
    int count = 0, sent_end = 0, r;
    if (!nm->frag) return 0;
    arena_reset(nm);
    nm->npn = 0;
    while (nm->frag && !sent_end && count < 50) {
        count++;
        r = next_token(nm, &sent_end);
        if (r < 0) return -1;
    }
    if (!sent_end && !pn_add(nm, SAM_NODE_PERIOD, 0x400e, NULL, 1)) return -1;
    if (emit_sentence(nm, out)) return -1;
    if (sam_pos_process(lex, lts, out->nodes, out->nn, out->e) < 0) return -1;
    return 1;
}

sam_norm *sam_norm_new(const char *utf8)
{
    sam_norm *nm = calloc(1, sizeof *nm);
    size_t i = 0, len = strlen(utf8);
    int n = 0;
    if (!nm) return NULL;
    nm->buf = malloc(sizeof(wc) * (len + 2));
    if (!nm->buf) {
        free(nm);
        return NULL;
    }
    while (i < len) { /* UTF-8 -> UTF-16 (invalid bytes read as Latin-1) */
        unsigned c = (unsigned char)utf8[i];
        unsigned cp = c;
        int extra = 0;
        if (c >= 0xf0 && i + 3 < len + 0) extra = 3, cp = c & 7;
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
                nm->buf[n++] = (wc)(cp > 0xffff ? 0xfffd : cp);
                continue;
            }
        }
        { /* not UTF-8: read the byte as Windows-1252 */
            static const wc C1[32] = {0x20ac, 0x81, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160,
                                      0x2039, 0x152, 0x8d, 0x17d, 0x8f, 0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
                                      0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0x9d, 0x17e, 0x178};
            nm->buf[n++] = c >= 0x80 && c < 0xa0 ? C1[c - 0x80] : (wc)c;
        }
        i++;
    }
    nm->buf[n] = 0;
    nm->n = n;
    nm->p = nm->buf;
    nm->fe = nm->buf + n;
    nm->frag = n > 0;
    return nm;
}

void sam_norm_free(sam_norm *nm)
{
    if (!nm) return;
    arena_reset(nm);
    free(nm->pn);
    free(nm->buf);
    free(nm);
}

void sam_sentence_free(sam_sentence *s)
{
    free(s->nodes);
    free(s->e);
    s->nodes = NULL;
    s->e = NULL;
    s->nn = s->ne = s->ncap = s->ecap = 0;
}
