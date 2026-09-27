/* Vendor lexicon reader (M1033DSK.TTS) and UTF-16 helpers for the Anna frontend.
 *
 * The .TTS file is the standard SAPI 5 compressed vendor lexicon (sapi.dll SpCompressedLexicon); layout in
 * notes/frontend.md section 2 and tools/tts_lex.py (verified 400/400 against SAPI):
 *   0x20 LangID, 0x24 nWords, 0x2C maxEntryBytes, 0x30 hashSize, 0x34 hashBits, 0x3C..0x44 codec sizes,
 *   0x48 word/pron/POS Huffman codecs, hash table (hashSize*hashBits bits, MSB first), bit stream (LSB first).
 */
#include "anna_lex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------ */
/* UTF-16 helpers                                                                                   */

size_t anna_wlen(const uint16_t *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* CharLowerW on the Latin-1 range (the frontend's text never leaves it after normalization) */
uint16_t anna_wlower(uint16_t c)
{
    if (c >= 'A' && c <= 'Z') return (uint16_t)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (uint16_t)(c + 32);
    if (c >= 0x100 && c < 0x138 && !(c & 1)) return (uint16_t)(c + 1); /* Latin Extended-A pairs */
    return c;
}

/* msvcrt towupper in the "C" locale: ASCII only */
uint16_t anna_wupper(uint16_t c) { return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c; }

/* msvcrt iswupper / iswlower: Latin-1 classification table for c < 256 */
int anna_wisupper(uint16_t c) { return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7); }
int anna_wislower(uint16_t c)
{
    return (c >= 'a' && c <= 'z') || c == 0xAA || c == 0xB5 || c == 0xBA || (c >= 0xDF && c <= 0xFF && c != 0xF7);
}

/* msvcrt _wcsicmp in the "C" locale: folds 'A'..'Z' to lower case */
static int fold(uint16_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int anna_wicmp(const uint16_t *a, const uint16_t *b)
{
    for (;; a++, b++) {
        int x = fold(*a), y = fold(*b);
        if (x != y || !x) return x - y;
    }
}
int anna_wicmp_a(const uint16_t *a, const char *b)
{
    for (;; a++, b++) {
        int x = fold(*a), y = fold((uint16_t)(unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

int anna_utf8_to_w(const char *s, uint16_t *out, int max)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < max - 1) {
        unsigned c = *p++;
        if (c >= 0xC0 && c < 0xE0 && (*p & 0xC0) == 0x80) {
            c = (c & 0x1F) << 6 | (*p++ & 0x3F);
        } else if (c >= 0xE0 && c < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
            c = (c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F);
            p += 2;
        } else if (c >= 0x80) {
            /* invalid or 4-byte sequence: keep the byte as a Latin-1 character */
        }
        out[n++] = (uint16_t)c;
    }
    out[n] = 0;
    return n;
}

void anna_w_to_utf8(const uint16_t *s, char *out, size_t max)
{
    size_t n = 0;
    if (!max) return;
    for (; *s; s++) {
        unsigned c = *s;
        if (c < 0x80) {
            if (n + 1 >= max) break;
            out[n++] = (char)c;
        } else if (c < 0x800) {
            if (n + 2 >= max) break;
            out[n++] = (char)(0xC0 | c >> 6);
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (n + 3 >= max) break;
            out[n++] = (char)(0xE0 | c >> 12);
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* phone strings                                                                                    */

static const char *const PHONE_NAMES[50] = {
    "", "-", "!", "&", ",", ".", "?", "_", "1", "2", "aa", "ae", "ah", "ao", "aw", "ax", "ay", "b", "ch", "d",
    "dh", "eh", "er", "ey", "f", "g", "h", "ih", "iy", "jh", "k", "l", "m", "n", "ng", "ow", "oy", "p", "r",
    "s", "sh", "t", "th", "uh", "uw", "v", "w", "y", "z", "zh"};

int anna_phones_from_string(const char *s, uint16_t *out, int max)
{
    int n = 0;
    for (;;) {
        char tok[16];
        int k = 0, i, found = 0;
        while (*s == ' ') s++;
        if (!*s) break;
        while (*s && *s != ' ') {
            if (k < 15) tok[k++] = *s;
            s++;
        }
        tok[k] = 0;
        for (i = 1; i < 50; i++) {
            const char *a = PHONE_NAMES[i], *b = tok;
            while (*a && (*a == *b || (*b >= 'A' && *b <= 'Z' && *a == *b + 32))) a++, b++;
            if (!*a && !*b) {
                found = i;
                break;
            }
        }
        if (!found) return -1;
        if (n < max) out[n] = (uint16_t)found;
        n++;
    }
    return n < max ? n : max;
}

/* ------------------------------------------------------------------------------------------------ */
/* lexicon                                                                                          */

typedef struct {
    uint32_t nsym, nnodes, root;
    const uint8_t *sym;   /* u16[nsym] */
    const uint8_t *nodes; /* {u16 c0, u16 c1}[nnodes] */
} codec;

struct anna_lexicon {
    uint8_t *data;
    size_t size;
    uint32_t hsize, hbits, empty;
    const uint8_t *hash;
    const uint8_t *bits;
    size_t nbits;
    codec wordc, pronc, posc;
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int codec_init(codec *c, const uint8_t *p, size_t avail)
{
    if (avail < 12) return 0;
    c->nsym = rd32(p);
    c->nnodes = rd32(p + 4);
    c->root = rd32(p + 8);
    if (12 + 2 * (size_t)c->nsym + 4 * (size_t)c->nnodes > avail || c->root >= c->nnodes) return 0;
    c->sym = p + 12;
    c->nodes = p + 12 + 2 * c->nsym;
    return 1;
}

anna_lexicon *anna_lexicon_load_mem(const void *data, size_t size)
{
    anna_lexicon *x;
    uint32_t cb1, cb2, cb3;
    size_t o, hlen;
    if (size < 0x48) return NULL;
    x = (anna_lexicon *)calloc(1, sizeof *x);
    if (!x) return NULL;
    x->data = (uint8_t *)malloc(size);
    if (!x->data) {
        free(x);
        return NULL;
    }
    memcpy(x->data, data, size);
    x->size = size;
    x->hsize = rd32(x->data + 0x30);
    x->hbits = rd32(x->data + 0x34);
    cb1 = rd32(x->data + 0x3c);
    cb2 = rd32(x->data + 0x40);
    cb3 = rd32(x->data + 0x44);
    o = 0x48;
    if (x->hbits == 0 || x->hbits > 32 || x->hsize == 0 || !codec_init(&x->wordc, x->data + o, size - o) ||
        o + cb1 > size || !codec_init(&x->pronc, x->data + o + cb1, size - o - cb1) || o + cb1 + cb2 > size ||
        !codec_init(&x->posc, x->data + o + cb1 + cb2, size - o - cb1 - cb2)) {
        anna_lexicon_free(x);
        return NULL;
    }
    o += (size_t)cb1 + cb2 + cb3;
    hlen = ((size_t)x->hsize * x->hbits + 7) >> 3;
    if (o + hlen > size) {
        anna_lexicon_free(x);
        return NULL;
    }
    x->hash = x->data + o;
    x->bits = x->data + o + hlen;
    x->nbits = ((size - o - hlen) >> 2) * 32;
    x->empty = x->hbits == 32 ? 0xFFFFFFFFu : (1u << x->hbits) - 1;
    return x;
}

anna_lexicon *anna_lexicon_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *buf;
    anna_lexicon *x;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = n > 0 ? malloc((size_t)n) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    x = anna_lexicon_load_mem(buf, (size_t)n);
    free(buf);
    return x;
}

void anna_lexicon_free(anna_lexicon *x)
{
    if (!x) return;
    free(x->data);
    free(x);
}

static uint32_t slot(const anna_lexicon *x, uint32_t i)
{
    uint32_t v = 0, k;
    size_t p = (size_t)i * x->hbits;
    for (k = 0; k < x->hbits; k++, p++) v = v << 1 | ((x->hash[p >> 3] >> (7 - (p & 7))) & 1);
    return v;
}

static int bit(const anna_lexicon *x, size_t pos) { return pos < x->nbits ? (x->bits[pos >> 3] >> (pos & 7)) & 1 : 0; }

static int sym(const anna_lexicon *x, const codec *c, size_t *pos)
{
    uint32_t node = c->root;
    int guard = 0;
    while (rd16(c->nodes + 4 * node) != 0xFFFF) {
        node = bit(x, *pos) ? rd16(c->nodes + 4 * node + 2) : rd16(c->nodes + 4 * node);
        (*pos)++;
        if (node >= c->nnodes || ++guard > 64) return -1;
    }
    return node < c->nsym ? rd16(c->sym + 2 * node) : -1;
}

/* 0-terminated symbol string; returns its length or -1 */
static int string(const anna_lexicon *x, const codec *c, size_t *pos, uint16_t *out, int max)
{
    int n = 0;
    for (;;) {
        int s = sym(x, c, pos);
        if (s < 0) return -1;
        if (s == 0) break;
        if (n >= max - 1) return -1;
        out[n++] = (uint16_t)s;
    }
    out[n] = 0;
    return n;
}

static uint32_t lex_hash(const uint16_t *w, uint32_t size)
{
    uint32_t h = w[0], prev = w[0];
    size_t i;
    for (i = 1; w[i]; i++) {
        uint32_t c = w[i];
        h += (prev << (c & 31)) + (c << (prev & 31));
        prev = c;
    }
    return (uint32_t)(h * 0xFFFFu) % size;
}

static int same_word(const uint16_t *a, const uint16_t *b)
{
    for (; *a && *b; a++, b++)
        if (anna_wlower(*a) != anna_wlower(*b)) return 0;
    return *a == *b;
}

int anna_lexicon_lookup(const anna_lexicon *x, const uint16_t *word, anna_pron *out, int max)
{
    uint16_t w[ANNA_WORD_MAX + 1], stored[ANNA_WORD_MAX + 1];
    size_t n = anna_wlen(word), i;
    uint32_t h, guard;
    if (!x || n == 0 || n > ANNA_WORD_MAX) return 0;
    for (i = 0; i < n; i++) w[i] = anna_wlower(word[i]);
    w[n] = 0;
    h = lex_hash(w, x->hsize);
    for (guard = 0; guard < x->hsize; guard++, h = (h + 1) % x->hsize) {
        uint32_t v = slot(x, h);
        size_t pos;
        int cnt = 0, pending = 0;
        anna_pron cur;
        if (v == x->empty) return 0;
        pos = v;
        if (string(x, &x->wordc, &pos, stored, ANNA_WORD_MAX + 1) < 0) return 0;
        if (!same_word(stored, w)) continue;
        cur.n = 0;
        for (;;) {
            int tag = 0, k;
            for (k = 0; k < 4; k++) tag |= bit(x, pos + (size_t)k) << k;
            pos += 4;
            if ((tag & 7) == 1) {
                if (pending && cnt < max) {
                    out[cnt] = cur;
                    out[cnt++].pos = 0xFFFFFFFFu;
                }
                cur.n = string(x, &x->pronc, &pos, cur.ph, ANNA_PRON_MAX);
                if (cur.n < 0) return cnt;
                pending = 1;
            } else if ((tag & 7) == 2) {
                int p = sym(x, &x->posc, &pos);
                if (p < 0) return cnt;
                if (cnt < max) {
                    out[cnt] = cur;
                    out[cnt++].pos = (uint32_t)p;
                }
                pending = 0;
            } else {
                return cnt;
            }
            if (tag & 8) break;
        }
        if (pending && cnt < max) {
            out[cnt] = cur;
            out[cnt++].pos = 0xFFFFFFFFu;
        }
        return cnt;
    }
    return 0;
}
