/* zf1_wb.c - CWhiteSpaceBreaker + CVocabTrie.  See zf1_wb.h and notes/fe1.md "Word breaking".  Portable C99.
 *
 * Resource 629aa5c4/9756c4e5 (CWordBreakerInfo, FUN_1800cbe0c, version 9756c4e5):
 *   u32 n0, u32 t0[n0]   characters skipped between tokens (controls, blanks)          -> info+0x20/+0x28
 *   u32 n1, u32 t1[n1]   characters that always form a token of their own (punctuation,
 *                        symbols; surrogate pairs as hi<<16|lo)                          -> info+0x30/+0x38
 *   u32 n2, u32 t2[n2]   characters trimmed off the start/end of a word (' - U+2010 ...) -> info+0x40/+0x48
 *   u32 bytes, trie0 (padded to 4)    CVocabTrie: abbreviations ("st.", "Dr." ...) and emoji sequences -> +0x50
 *   u32 bytes, trie1                  CVocabTrie: emoticons and a few fixed phrases ("I ll")            -> +0x58
 * CVocabTrie header (u16 h[]): h[0] version (<2), h[6..9] sizes of the four u16 threshold tables T1..T4
 * (each table start is aligned to 4 bytes, alignment taken from the file/mapping address), u32 at h[0xe] node
 * count ({u16 char, u16 flags}), u32 h[0x10] / h[0x12] sizes of the u32 tables T7 / T8, u32 h[0x14] = root
 * offset.  Node references are "threshold varints" (see dec()). */
#include "zf1_wb.h"
#include <string.h>

static uint16_t R16(const uint8_t *p) { return zf_rd16(p); }

/* threshold varint: v = b0; while (T[i] <= v) { acc += T[i]; v = (v - T[i]) * 256 + b[i+1]; i++; } */
static unsigned dec(const uint8_t *T, const uint8_t **pp)
{
    const uint8_t *p = *pp;
    uint16_t v = p[0], acc = 0, t = R16(T);
    int i = 0;
    while (t <= v) {
        acc = (uint16_t)(acc + t);
        i++;
        v = (uint16_t)((uint16_t)(v - t) * 256 + p[i]);
        t = R16(T + 2 * i);
    }
    *pp = p + i + 1;
    return (uint16_t)(acc + v);
}

static int trie_init(zf1_trie *t, const uint8_t *blob, const uint8_t *res0)
{
    const uint8_t *p;
    memset(t, 0, sizeof *t);
    if (R16(blob) >= 2) return -1;
    t->base = blob;
#define ALIGN4(q) ((((size_t)((q) - res0)) & 2) ? (q) + 2 : (q))
    p = blob + 0x30;
    t->T1 = p;
    p = ALIGN4(p + 2 * R16(blob + 12));
    /* T2 unused by the matcher */
    p = ALIGN4(p + 2 * R16(blob + 14));
    t->T3 = p;
    p = ALIGN4(p + 2 * R16(blob + 16));
    t->T4 = p;
    p = ALIGN4(p + 2 * R16(blob + 18));
    t->nodes = p;
    p += 4 * (size_t)zf_rd32(blob + 0x1c);
    /* T6 = p (unused) */
    t->T7 = p + 4 * (size_t)zf_rd32(blob + 0x20);
    t->T8 = t->T7 + 4 * (size_t)zf_rd32(blob + 0x24);
    t->root = blob + zf_rd32(blob + 0x28);
#undef ALIGN4
    t->ok = 1;
    return 0;
}

typedef struct tnode { uint16_t c, f; } tnode;

/* FUN_18002cc60: node reference at *pp -> char, flags; skips the optional value (flag 4) */
static tnode rd_node(const zf1_trie *t, const uint8_t **pp)
{
    tnode n;
    unsigned idx = dec(t->T1, pp);
    const uint8_t *p = *pp;
    n.c = R16(t->nodes + 4 * idx);
    n.f = R16(t->nodes + 4 * idx + 2);
    if (n.f & 4) {
        uint8_t b0 = p[0];
        if (b0 >= 0xc0) p += 3;
        else if (b0 >= 0x80) p += 2;
        else p += 1;
    }
    *pp = p;
    return n;
}

static const uint8_t *child_of(const zf1_trie *t, uint16_t f, const uint8_t *p12, const uint8_t *f0)
{
    unsigned k = f & 0x1c0;
    const uint8_t *q = p12;
    if (k == 0) return f0;
    if (k == 0x40) return p12;
    if (k == 0x80) { unsigned idx = dec(t->T3, &q); return t->root + zf_rd32(t->T7 + 4 * idx); }
    if (k == 0x100) return t->root + (((uint32_t)p12[0] << 16) | ((uint32_t)p12[1] << 8) | p12[2]);
    return NULL;
}

int zf1_trie_match(const zf1_trie *t, const zf_char *text, int n)
{
    const uint8_t *base;
    int e = 0, best = -1;
    uint16_t f = 0x10;
    if (!t->ok || !text || n <= 0) return -1;
    base = t->root;
    while (e != n && (f & 0x10)) {
        const uint8_t *p = base, *p12, *p11, *f0 = NULL, *q;
        tnode nd;
        unsigned k;
        if (!base) break;
        nd = rd_node(t, &p);
        p12 = p;
        /* FUN_18002cb20: skip the child reference of the first node */
        k = nd.f & 0x1d0;
        if (k == 0x10) { unsigned idx; q = p; idx = dec(t->T4, &q); f0 = q + zf_rd32(t->T8 + 4 * idx); p = q; }
        else if (k == 0x90) { q = p; dec(t->T3, &q); p = q; }
        else if (k == 0x110) p += 3;
        p11 = p;
        for (;;) {
            const uint8_t *p4;
            if (nd.c == text[e]) break;
            if (nd.f & 2) {
                if (!(nd.f & 0x200)) return best < 0 ? -1 : best;
                f0 = NULL;
            }
            nd = rd_node(t, &p11);
            p12 = p11;
            p4 = f0;
            k = nd.f & 0x1d0;
            if (k == 0x10) {
                unsigned idx;
                q = p12;
                idx = dec(t->T4, &q);
                if (!f0) f0 = q;
                p11 = q;
                p4 = f0 + zf_rd32(t->T8 + 4 * idx);
            } else if (k == 0x50) {
                p4 = p12;
            } else if (k == 0x90) {
                q = p12;
                dec(t->T3, &q);
                p11 = q;
            } else if (k == 0x110) {
                p11 = p12 + 3;
            }
            f0 = p4;
        }
        base = child_of(t, nd.f, p12, f0);
        f = nd.f;
        e++;
        if (f & 1) best = e;
    }
    return best;
}

/* ---------------- breaker ---------------- */
static int bs32(const uint8_t *tab, int n, uint32_t key)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = zf_rd32(tab + 4 * mid);
        if (v == key) return 1;
        if (v < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

int zf1_wb_is_skip(const zf1_wb *wb, uint32_t c) { return bs32(wb->t0, wb->n0, c); }
int zf1_wb_is_split(const zf1_wb *wb, uint32_t c) { return bs32(wb->t1, wb->n1, c); }
int zf1_wb_is_trim(const zf1_wb *wb, uint32_t c) { return bs32(wb->t2, wb->n2, c); }
static int is_break(const zf1_wb *wb, uint32_t c) { return zf1_wb_is_skip(wb, c) || zf1_wb_is_split(wb, c); }

int zf1_wb_init(zf1_wb *wb, const zf1_dat *d, uint16_t lang)
{
    size_t sz;
    const uint8_t *r = zf1_dat_get(d, 0x629aa5c4u, 0x9756c4e5u, &sz), *p, *end;
    uint32_t nb;
    int old = 0;
    memset(wb, 0, sizeof *wb);
    wb->lang = lang;
    /* ClassicVoices patch: MSTTSLocEnGB.dat carries only the older version 964f993a of this resource: the same three
       character tables, then ONE vocabulary trie with no size prefix (no emoticon / fixed-phrase trie: trie1 stays
       empty and never matches).  Found by reading the en-GB data; not checked against the engine's own reader. */
    if (!r) {
        r = zf1_dat_get(d, 0x629aa5c4u, 0x964f993au, &sz);
        old = 1;
    }
    if (!r) return -1;
    p = r;
    end = r + sz;
    wb->n0 = (int)zf_rd32(p); wb->t0 = p + 4; p += 4 + 4 * (size_t)wb->n0;
    wb->n1 = (int)zf_rd32(p); wb->t1 = p + 4; p += 4 + 4 * (size_t)wb->n1;
    wb->n2 = (int)zf_rd32(p); wb->t2 = p + 4; p += 4 + 4 * (size_t)wb->n2;
    if (old) return p + 0x30 <= end && trie_init(&wb->trie0, p, r) == 0 ? 0 : -2;
    nb = zf_rd32(p);
    if (trie_init(&wb->trie0, p + 4, r)) return -2;
    p = p + 4 + ((nb + 3) & ~3u);
    if (p + 4 > end) return -3;
    if (trie_init(&wb->trie1, p + 4, r)) return -4;
    return 0;
}

static int is_hi(zf_char c) { return (zf_char)(c + 0x2800) < 0x400; }
static int is_lo(zf_char c) { return (zf_char)(c + 0x2400) < 0x400; }

/* FUN_18004d024: code point (surrogate pair as hi<<16|lo) at index i, looking at pairs starting or ending at i */
static uint32_t char_at(const zf_char *p, int n, int i)
{
    if (i < 0 || i >= n) return 0;
    if (i + 1 < n && is_hi(p[i]) && is_lo(p[i + 1])) return ((uint32_t)p[i] << 16) | p[i + 1];
    if (i != 0 && is_hi(p[i - 1]) && is_lo(p[i])) return ((uint32_t)p[i - 1] << 16) | p[i];
    return p[i];
}

static int is_apos(zf_char c) { return c == 0x2032 || c == 0x2018 || c == 0x2019 || c == 0x27 || c == 0xff07; }

/* FUN_180043ee8: split one leading and one trailing trim char off a word chunk */
static int chunk(const zf1_wb *wb, const zf_char *s, int off, int n, zf1_wb_emit emit, void *ctx)
{
    const zf_char *p = s + off;
    uint32_t first, last;
    int flen = 1, llen = 1, lastpos, pos = 0, r;
    if ((wb->lang & 0xff) == 9 && n > 1 && !is_apos(p[0]) && is_apos(p[n - 1]) && ((p[n - 2] - 0x53) & 0xffdf) == 0)
        return n ? emit(ctx, off, n) : 0;
    if (n <= 0) return 0;
    if (1 < n && is_hi(p[0]) && is_lo(p[1])) { flen = 2; first = ((uint32_t)p[0] << 16) | p[1]; }
    else first = p[0];
    lastpos = n - 1;
    if (n - 1 != 0 && is_hi(p[n - 2]) && is_lo(p[n - 1])) { lastpos = n - 2; llen = 2; last = ((uint32_t)p[n - 2] << 16) | p[n - 1]; }
    else last = p[n - 1];
    if (pos <= lastpos && zf1_wb_is_trim(wb, first)) {
        if ((r = emit(ctx, off, flen)) < 0) return r;
        pos += flen;
    }
    if (pos <= lastpos) {
        int mid = lastpos - pos;
        if (zf1_wb_is_trim(wb, last)) {
            if (mid && (r = emit(ctx, off + pos, mid)) < 0) return r;
            return emit(ctx, off + lastpos, llen);
        }
        return emit(ctx, off + pos, mid + llen);
    }
    return 0;
}

/* FUN_180076454 (version 9756c4e5 path): 0 = token of *len chars at text start, 1 = no vocabulary token */
static int classify(const zf1_wb *wb, const zf_char *p, int n, int *len, int flag)
{
    int m = zf1_trie_match(&wb->trie0, p, n);
    if (m != -1 && (m != n || flag == 0)) {
        if (is_break(wb, char_at(p, n, m)) || is_break(wb, char_at(p, n, m - 1))) { *len = m; return 0; }
    }
    m = zf1_trie_match(&wb->trie1, p, n);
    if (m != -1 && m == n) { *len = m; return 0; }
    return 1;
}

int zf1_wb_break(const zf1_wb *wb, const zf_char *text, int n, int flag, zf1_wb_emit emit, void *ctx)
{
    int i = 0, r;
    if (!text || !emit) return -1;
    while (i < n) {
        int rem = n - i, len = 0;
        const zf_char *p = text + i;
        if (classify(wb, p, rem, &len, flag) == 0) {
            if (len <= 0) len = 1;   /* engine would loop forever; never happens with the shipped data */
            if ((r = emit(ctx, i, len)) < 0) return r;
            i += len;
        } else {
            uint32_t c;
            int clen = 1, j;
            if (1 < rem && is_hi(p[0]) && is_lo(p[1])) { clen = 2; c = ((uint32_t)p[0] << 16) | p[1]; }
            else c = p[0];
            if (zf1_wb_is_skip(wb, c)) { i += clen; continue; }
            if (zf1_wb_is_split(wb, c)) {
                if ((r = emit(ctx, i, clen)) < 0) return r;
                i += clen;
                continue;
            }
            /* FUN_180044194: next skip/split char */
            j = i;
            while (j < n) {
                uint32_t d;
                int dl = 1;
                if (n - j >= 2 && is_hi(text[j]) && is_lo(text[j + 1])) { dl = 2; d = ((uint32_t)text[j] << 16) | text[j + 1]; }
                else d = text[j];
                if (is_break(wb, d)) break;
                j += dl;
            }
            if ((r = chunk(wb, text, i, j - i, emit, ctx)) < 0) return r;
            i = j;
        }
    }
    return 0;
}
