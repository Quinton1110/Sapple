/* zf1_lex.c - lexicon + phone set.  See zf1_lex.h.  Portable C99. */
#include "zf1_lex.h"
#include <stdlib.h>
#include <string.h>

#define H32(o) zf_rd32(lx->L + (o))

static void huff_init(zf1_huff *h, const uint8_t *t, const uint8_t *words)
{
    h->nsym = zf_rd32(t);
    h->nnodes = zf_rd32(t + 4);
    h->root = zf_rd32(t + 8);
    h->sym = t + 12;
    h->node = t + 12 + 2 * h->nsym;
    h->words = words;
}

static inline int getbit(const uint8_t *words, uint32_t p)
{
    return (int)((zf_rd32(words + 4 * (p >> 5)) >> (p & 31)) & 1);
}

static uint16_t huff_one(const zf1_huff *h, uint32_t *pp)
{
    uint32_t node = h->root, p = *pp;
    for (;;) {
        uint16_t a = zf_rd16(h->node + 4 * node), b = zf_rd16(h->node + 4 * node + 2);
        if (a == 0xFFFF) { *pp = p; return zf_rd16(h->sym + 2 * node); }
        node = getbit(h->words, p++) ? b : a;
    }
}

/* decode 0-terminated string; returns length (stores at most cap) */
static int huff_str(const zf1_huff *h, uint32_t *pp, uint16_t *out, int cap)
{
    int n = 0;
    for (;;) {
        uint16_t c = huff_one(h, pp);
        if (!c) break;
        if (n < cap) out[n] = c;
        n++;
    }
    return n;
}

int zf1_lex_init(zf1_lex *lx, const uint8_t *res, size_t size)
{
    uint32_t p, nbits;
    zf1_huff c4;
    int k;
    memset(lx, 0, sizeof *lx);
    if (!res || size < 0xa0) return -1;
    lx->L = res;
    lx->size = size;
    if (H32(0x24) != size) return -2;
    lx->hash = res + H32(0x3c);
    lx->nslots = H32(0x40);
    lx->bits = H32(0x44);
    lx->words = res + H32(0x90);
    lx->nwords = H32(0x94) / 4;
    for (k = 0; k < 4; k++) huff_init(&lx->c[k], res + H32(0x48 + 8 * k), lx->words);
    /* attribute names */
    huff_init(&c4, res + H32(0x68), res + H32(0x88));
    nbits = H32(0x98);
    lx->attrname = NULL;
    lx->nattrnames = 0;
    p = 0;
    while (p < nbits) {
        uint16_t tmp[256];
        int n = huff_str(&c4, &p, tmp, 255), i;
        char *s = (char *)malloc((size_t)n + 1);
        if (n > 255) n = 255;
        for (i = 0; i < n; i++) s[i] = (char)tmp[i];
        s[n] = 0;
        lx->attrname = (char **)realloc(lx->attrname, sizeof(char *) * (size_t)(lx->nattrnames + 1));
        lx->attrname[lx->nattrnames++] = s;
    }
    lx->setstart = res + H32(0x70);
    lx->nsets = (int)(H32(0x74) / 4);
    lx->attrdefs = res + H32(0x78);
    lx->nattrdefs = (int)(H32(0x7c) / 10);
    lx->ids = res + H32(0x80);
    return 0;
}

void zf1_lex_free(zf1_lex *lx)
{
    int i;
    for (i = 0; i < lx->nattrnames; i++) free(lx->attrname[i]);
    free(lx->attrname);
    memset(lx, 0, sizeof *lx);
}

const uint8_t *zf1_lex_attrset(const zf1_lex *lx, int k)
{
    if (k < 1 || k > lx->nsets) return NULL;
    return lx->ids + 2 * zf_rd32(lx->setstart + 4 * (k - 1));
}

const char *zf1_lex_attrname(const zf1_lex *lx, int id)
{
    return (id >= 0 && id < lx->nattrnames) ? lx->attrname[id] : NULL;
}

static uint32_t slot_value(const zf1_lex *lx, uint32_t i)
{
    uint64_t p = (uint64_t)lx->bits * i;
    uint32_t v = 0, k;
    for (k = 0; k < lx->bits; k++, p++) v = (v << 1) | ((lx->hash[p >> 3] >> (7 - (p & 7))) & 1);
    return v;
}

int zf1_lex_lookup(const zf1_lex *lx, const zf_char *word, int n, zf1_lexent *out)
{
    zf_char w[130];
    uint32_t h, prev, slot, tries, empty;
    int i;
    if (!lx->L || n <= 0) return 0;
    if (n > 128) n = 128;
    for (i = 0; i < n; i++) w[i] = zf_tolower(word[i]);
    w[n] = 0;
    /* FUN_180035180 hash */
    h = prev = w[0];
    for (i = 1; i < n; i++) {
        uint32_t c = w[i];
        h += (c << (prev & 31)) + (prev << (c & 31));
        prev = c;
    }
    slot = (uint32_t)(h * 0xFFFFu) % lx->nslots;
    empty = (lx->bits >= 32) ? 0xFFFFFFFFu : ((1u << lx->bits) - 1);
    for (tries = 0; tries < lx->nslots; tries++) {
        uint32_t v = slot_value(lx, slot), p = v;
        uint16_t key[260];
        int kn, match;
        if (v == empty) return 0;
        kn = huff_str(&lx->c[0], &p, key, 259);
        match = (kn == n);
        for (i = 0; match && i < n; i++)
            if (zf_tolower(key[i]) != w[i]) match = 0;
        if (match) {
            /* FUN_180044304 entry decode */
            out->n = 0;
            for (;;) {
                uint32_t hd = 0, t, b;
                for (b = 0; b < 4; b++) hd |= (uint32_t)getbit(lx->words, p++) << b;
                t = hd & 7;
                if (t == 1) {
                    zf1_lexpron *pr;
                    if (out->n >= ZF1_LEX_MAXPRON) { uint16_t dump[ZF1_LEX_MAXPH]; huff_str(&lx->c[1], &p, dump, ZF1_LEX_MAXPH); }
                    else {
                        pr = &out->p[out->n++];
                        memset(pr, 0, sizeof *pr);
                        pr->nph = huff_str(&lx->c[1], &p, pr->ph, ZF1_LEX_MAXPH);
                        if (pr->nph > ZF1_LEX_MAXPH) pr->nph = ZF1_LEX_MAXPH;
                    }
                } else if (t == 2) {
                    uint16_t v2 = huff_one(&lx->c[2], &p);
                    if (out->n) { out->p[out->n - 1].has_val = 1; out->p[out->n - 1].val = v2; }
                } else if (t == 3) {
                    uint16_t tmp[64];
                    int m = huff_str(&lx->c[3], &p, tmp, 64), j;
                    if (out->n) {
                        zf1_lexpron *pr = &out->p[out->n - 1];
                        for (j = 0; j < m && j < 64 && pr->nattr < ZF1_LEX_MAXATTR; j++) pr->attrset[pr->nattr++] = tmp[j];
                    }
                } else {
                    return out->n > 0;   /* corrupt */
                }
                if (hd & 8) break;
            }
            return 1;
        }
        slot = (slot + 1) % lx->nslots;
    }
    return 0;
}

/* ---------------- phone set ---------------- */
int zf1_phoneset_init(zf1_phoneset *ps, const uint8_t *res, size_t size)
{
    uint32_t rs, n, k;
    memset(ps, 0, sizeof *ps);
    for (k = 0; k < 128; k++) ps->byid[k] = -1;
    if (!res || size < 8) return -1;
    rs = zf_rd32(res);
    n = zf_rd32(res + 4);
    if (n > 64) n = 64;
    for (k = 0; k < n; k++) {
        const uint8_t *r = res + 8 + rs * k;
        int j;
        ps->id[k] = zf_rd16(r);
        for (j = 0; j < 23; j++) {
            uint16_t c = zf_rd16(r + 2 + 2 * j);
            ps->name[k][j] = (char)c;
            if (!c) break;
        }
        ps->name[k][23] = 0;
        ps->flags[k] = zf_rd32(r + 48);
        if (ps->id[k] < 128) ps->byid[ps->id[k]] = (int)k;
    }
    ps->n = (int)n;
    return 0;
}

const char *zf1_phone_name(const zf1_phoneset *ps, int id)
{
    if (id < 0 || id >= 128 || ps->byid[id] < 0) return NULL;
    return ps->name[ps->byid[id]];
}

int zf1_phone_id(const zf1_phoneset *ps, const char *name, int n)
{
    int k, i;
    for (k = 0; k < ps->n; k++) {
        const char *s = ps->name[k];
        for (i = 0; i < n; i++) {
            char a = s[i], b = name[i];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (!a || a != b) break;
        }
        if (i == n && !s[n]) return ps->id[k];
    }
    return -1;
}

zf_char *zf1_phones_to_str(const zf1_phoneset *ps, const uint16_t *ph, int n)
{
    size_t cap = (size_t)n * 8 + 1, k = 0;
    zf_char *r = (zf_char *)malloc(cap * sizeof(zf_char));
    int i;
    if (!r) return NULL;
    for (i = 0; i < n; i++) {
        const char *nm = zf1_phone_name(ps, ph[i]);
        if (!nm) continue;
        if (k) r[k++] = ' ';
        while (*nm && k + 2 < cap) r[k++] = (unsigned char)*nm++;
    }
    r[k] = 0;
    return r;
}
