/* Pronunciation sources for libsam: the SAPI compressed lexicon (LTTS1033.LXA) and the
 * letter-to-sound model (r1033tts.LXA). Both readers work directly on the original files.
 * See tools/lxa.py and tools/lts.py for the format notes; both were verified against SAPI.
 */
#include "sam_lex.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc((size_t)n + 1);
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    if (d) {
        d[n] = 0;
        *size = (size_t)n;
    }
    return d;
}

/* ============================================================================================ */
/* compressed lexicon                                                                          */

typedef struct {
    uint32_t nsym, nnodes, root;
    const uint8_t *sym;   /* u16[nsym] */
    const uint8_t *nodes; /* (u16, u16)[nnodes] */
} huff;

struct sam_lexicon {
    uint8_t *d;
    size_t size;
    uint32_t lcid, slots, slot_bits;
    huff letters, phones, pos;
    const uint8_t *hash;
    const uint8_t *data;
    size_t data_words;
    uint32_t empty;
};

static void huff_init(huff *h, const uint8_t *p)
{
    h->nsym = rd32(p);
    h->nnodes = rd32(p + 4);
    h->root = rd32(p + 8);
    h->sym = p + 12;
    h->nodes = p + 12 + 2 * h->nsym;
}

static int lex_bit(const sam_lexicon *x, uint32_t pos)
{
    if ((pos >> 5) >= x->data_words) return 0;
    return (rd32(x->data + 4 * (pos >> 5)) >> (pos & 31)) & 1;
}

static int huff_decode(const sam_lexicon *x, const huff *h, uint32_t *pos, uint16_t *out)
{
    uint32_t node = h->root, guard = 0;
    while (node < h->nnodes && rd16(h->nodes + 4 * node) != 0xFFFF) {
        node = lex_bit(x, *pos) ? rd16(h->nodes + 4 * node + 2) : rd16(h->nodes + 4 * node);
        (*pos)++;
        if (++guard > 64) return -1;
    }
    if (node >= h->nsym) return -1;
    *out = rd16(h->sym + 2 * node);
    return 0;
}

static int huff_string(const sam_lexicon *x, const huff *h, uint32_t *pos, uint16_t *out, int max)
{
    int n = 0;
    for (;;) {
        uint16_t s;
        if (huff_decode(x, h, pos, &s) != 0) return -1;
        if (s == 0) break;
        if (n < max - 1) out[n++] = s;
    }
    out[n] = 0;
    return n;
}

static uint8_t *dupmem(const void *p, size_t n)
{
    uint8_t *d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, p, n);
    d[n] = 0;
    return d;
}

/* takes ownership of d */
static sam_lexicon *lexicon_from(uint8_t *d, size_t size)
{
    sam_lexicon *x = calloc(1, sizeof *x);
    uint32_t s1, s2, s3;
    size_t off;
    if (!x) {
        free(d);
        return NULL;
    }
    x->d = d;
    x->size = size;
    if (!x->d || x->size < 0x48) goto fail;
    x->lcid = rd32(x->d + 0x20);
    x->slots = rd32(x->d + 0x30);
    x->slot_bits = rd32(x->d + 0x34);
    s1 = rd32(x->d + 0x3C);
    s2 = rd32(x->d + 0x40);
    s3 = rd32(x->d + 0x44);
    if (x->slot_bits == 0 || x->slot_bits > 31 || x->slots == 0) goto fail;
    huff_init(&x->letters, x->d + 0x48);
    huff_init(&x->phones, x->d + 0x48 + s1);
    huff_init(&x->pos, x->d + 0x48 + s1 + s2);
    off = 0x48 + (size_t)s1 + s2 + s3;
    x->hash = x->d + off;
    off += ((size_t)x->slots * x->slot_bits + 7) >> 3;
    if (off > x->size) goto fail;
    x->data = x->d + off;
    x->data_words = (x->size - off) >> 2;
    x->empty = (1u << x->slot_bits) - 1;
    return x;
fail:
    sam_lexicon_free(x);
    return NULL;
}

void sam_lexicon_free(sam_lexicon *x)
{
    if (!x) return;
    free(x->d);
    free(x);
}

static uint32_t lex_slot(const sam_lexicon *x, uint32_t k)
{
    uint32_t v = 0, b = x->slot_bits * k, i;
    for (i = 0; i < x->slot_bits; i++, b++) v = (v << 1) | ((x->hash[b >> 3] >> (7 - (b & 7))) & 1);
    return v;
}

static int lower_ascii(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int sam_lexicon_lookup(const sam_lexicon *x, const char *word, sam_pron *out, int max)
{
    uint16_t w[128], letters[128];
    int n = 0, i, count = 0;
    uint32_t h, prev, k, probes;
    for (i = 0; word[i] && n < 127; i++) w[n++] = (uint16_t)lower_ascii((unsigned char)word[i]);
    w[n] = 0;
    if (!n) return 0;
    h = prev = w[0];
    for (i = 1; i < n; i++) {
        h += (prev << (w[i] & 31)) + ((uint32_t)w[i] << (prev & 31));
        prev = w[i];
    }
    k = (uint32_t)((h * 0xFFFFu) % x->slots);
    for (probes = 0; probes < x->slots; probes++, k = (k + 1) % x->slots) {
        uint32_t pos = lex_slot(x, k);
        int ln;
        if (pos == x->empty) return 0;
        ln = huff_string(x, &x->letters, &pos, letters, 128);
        if (ln < 0) return 0;
        if (ln == n) {
            for (i = 0; i < n && lower_ascii(letters[i]) == w[i]; i++) {
            }
            if (i == n) {
                /* entries: 4-bit control, 1 = phones, 2 = POS (closes the pronunciation), 8 = last */
                uint16_t cur[SAM_MAX_PRON];
                int have = 0, prevlen = 0, last = 0;
                cur[0] = 0;
                while (!last && count < max) {
                    int ctl = 0, b;
                    for (b = 0; b < 4; b++) ctl |= lex_bit(x, pos + b) << b;
                    pos += 4;
                    last = ctl & 8;
                    if ((ctl & 7) == 1) {
                        if (have) {
                            memcpy(out[count].ph, cur, sizeof cur);
                            out[count].n = prevlen;
                            out[count].pos = 0xFFFFFFFFu;
                            count++;
                        }
                        prevlen = huff_string(x, &x->phones, &pos, cur, SAM_MAX_PRON);
                        if (prevlen < 0) return count;
                        have = 1;
                    } else if ((ctl & 7) == 2) {
                        uint16_t p;
                        if (huff_decode(x, &x->pos, &pos, &p) != 0) return count;
                        memcpy(out[count].ph, cur, sizeof cur);
                        out[count].n = prevlen;
                        out[count].pos = p;
                        count++;
                        have = 0;
                    } else {
                        return count;
                    }
                }
                if (have && count < max) {
                    memcpy(out[count].ph, cur, sizeof cur);
                    out[count].n = prevlen;
                    out[count].pos = 0xFFFFFFFFu;
                    count++;
                }
                return count;
            }
        }
    }
    return 0;
}

/* ============================================================================================ */
/* letter to sound                                                                             */

typedef struct {
    const uint8_t *nodes, *dist, *expr;
} lts_tree;

typedef struct {
    char name[8];
    uint16_t ids[4];
    int n;
} lts_phone;

struct sam_lts {
    uint8_t *d;
    size_t size;
    int n_letters, n_outputs;
    char letter_char[64];         /* lowercase letter for each letter symbol */
    const char **out_names;       /* [n_outputs] */
    int nq[2], qwords[2];
    const uint8_t *q[2];          /* bitsets */
    lts_tree *trees;              /* [n_letters] */
    lts_phone *phones;
    int n_phones;
};

static const char *LETTER_NAMES[52] = {
    "EY", "B IY", "S IY", "D IY", "IY", "EH F", "JH IY", "EY CH", "AY", "JH EY", "K EY", "EH L", "EH M", "EH N",
    "OW", "P IY", "K Y UW", "AA R", "EH S", "T IY", "Y UW", "V IY", "D AH B AX L Y UW", "EH K S", "W AY", "Z IY",
    "EY Z", "B IY Z", "S IY Z", "D IY Z", "IY Z", "EH F S", "JH IY Z", "EY CH AX Z", "AY Z", "JH EY Z", "K EY Z",
    "EH L Z", "EH M Z", "EH N Z", "OW Z", "P IY Z", "K Y UW Z", "AA R Z", "EH S AX Z", "T IY Z", "Y UW Z", "V IY Z",
    "D AH B AX L Y UW Z", "EH K S AX Z", "W AY Z", "Z IY Z"};


static sam_lts *lts_from(uint8_t *d, size_t size)
{
    sam_lts *m = calloc(1, sizeof *m);
    const uint8_t *p, *end, *t;
    int k;
    if (!m) {
        free(d);
        return NULL;
    }
    m->d = d;
    m->size = size;
    if (!m->d || m->size < 0x28) goto fail;
    /* text phone table */
    p = m->d + 0x24;
    end = (const uint8_t *)memchr(p, 0, m->size - 0x24);
    if (!end) goto fail;
    for (t = p; t < end; t++) if (*t == '\n') m->n_phones++;
    m->phones = calloc((size_t)m->n_phones + 1, sizeof *m->phones);
    if (!m->phones) goto fail;
    m->n_phones = 0;
    while (p < end) {
        const uint8_t *nl = p, *sp;
        lts_phone *ph = &m->phones[m->n_phones];
        while (nl < end && *nl != '\n') nl++;
        sp = p;
        while (sp < nl && *sp != ' ') sp++;
        if (sp < nl && sp - p < 8) {
            const uint8_t *h = sp + 1;
            memcpy(ph->name, p, (size_t)(sp - p));
            ph->name[sp - p] = 0;
            while (h + 4 <= nl && ph->n < 4) {
                char hex[5];
                memcpy(hex, h, 4);
                hex[4] = 0;
                ph->ids[ph->n++] = (uint16_t)strtol(hex, NULL, 16);
                h += 4;
            }
            m->n_phones++;
        }
        p = nl + 1;
    }
    p = end + 1;
#define NEED(n) \
    if ((size_t)(p - m->d) + (n) > m->size) goto fail
    /* letter symbols */
    NEED(4);
    m->n_letters = (int)rd32(p);
    if (m->n_letters <= 0 || m->n_letters > 63) goto fail;
    NEED(8 + 4 * (size_t)m->n_letters);
    {
        const uint8_t *offs = p + 4, *base = p + 8 + 4 * m->n_letters;
        uint32_t tsize = rd32(p + 4 + 4 * m->n_letters);
        for (k = 0; k < m->n_letters; k++) {
            int c = base[rd32(offs + 4 * k)];
            m->letter_char[k] = (char)lower_ascii(c);
        }
        p = base + tsize;
    }
    /* output symbols */
    NEED(4);
    m->n_outputs = (int)rd32(p);
    if (m->n_outputs <= 0 || m->n_outputs > 255) goto fail;
    m->out_names = calloc((size_t)m->n_outputs, sizeof(char *));
    if (!m->out_names) goto fail;
    {
        const uint8_t *offs = p + 4, *base = p + 8 + 4 * m->n_outputs;
        uint32_t tsize = rd32(p + 4 + 4 * m->n_outputs);
        for (k = 0; k < m->n_outputs; k++) m->out_names[k] = (const char *)base + rd32(offs + 4 * k);
        p = base + tsize;
    }
    for (k = 0; k < 2; k++) {
        NEED(8);
        m->nq[k] = (int)rd32(p);
        m->qwords[k] = (int)rd32(p + 4);
        m->q[k] = p + 8;
        p += 8 + 4 * (size_t)m->nq[k] * m->qwords[k];
    }
    m->trees = calloc((size_t)m->n_letters, sizeof *m->trees);
    if (!m->trees) goto fail;
    for (k = 1; k < m->n_letters; k++) {
        uint32_t n, ds, es;
        NEED(4);
        n = rd32(p);
        m->trees[k].nodes = p + 4;
        p += 4 + 8 * (size_t)n;
        NEED(4);
        ds = rd32(p);
        m->trees[k].dist = p + 4;
        p += 4 + ds;
        NEED(4);
        es = rd32(p);
        p += 4;
        m->trees[k].expr = (int32_t)es > 0 ? p : NULL;
        if ((int32_t)es > 0) p += es;
    }
#undef NEED
    return m;
fail:
    sam_lts_free(m);
    return NULL;
}

void sam_lts_free(sam_lts *m)
{
    if (!m) return;
    free(m->d);
    free(m->phones);
    free(m->out_names);
    free(m->trees);
    free(m);
}

typedef struct {
    const uint8_t *buf;
    int pos;
} lts_ctx;

static int lts_term(const sam_lts *m, int code, const lts_ctx *cl, const lts_ctx *co)
{
    int t = 0, kind = 2, off, q, i, pos, v;
    const lts_ctx *c;
    const uint8_t *bits;
    if (code > 20000) {
        t = 1;
        code -= 20000;
    }
    if (code > 10000) {
        kind = 3;
        code -= 10000;
    }
    off = code / 1000;
    q = code % 1000;
    c = t ? co : cl;
    pos = c->pos;
    for (i = 0; i < off; i++) {
        if (c->buf[pos] == 0) break;
        pos += kind == 2 ? -1 : 1;
    }
    v = c->buf[pos];
    if (q >= m->nq[t]) return 0;
    bits = m->q[t] + 4 * (size_t)q * m->qwords[t];
    if ((v >> 5) >= m->qwords[t]) return 0;
    return (int)((rd32(bits + 4 * (v >> 5)) >> (v & 31)) & 1);
}

/* FUN_5bd463a6: OR of AND-terms; returns value, *next = next clause or NULL */
static int lts_expr(const sam_lts *m, const uint8_t *p, const lts_ctx *cl, const lts_ctx *co, const uint8_t **next)
{
    for (;;) {
        int code = rd16(p), neg = (code & 0x8000) ? 1 : 0;
        code &= 0x7FFF;
        if (lts_term(m, code, cl, co) ^ neg) {
            int w;
            p += 2;
            w = rd16(p);
            if (w == 0xFFFF) {
                *next = NULL;
                return 1;
            }
            if (w == 0xFFFE) {
                *next = p + 2;
                return 1;
            }
            continue;
        }
        for (;;) {
            int w = rd16(p);
            if (w == 0xFFFE) {
                *next = p + 2;
                return 0;
            }
            if (w == 0xFFFF) {
                *next = NULL;
                return 0;
            }
            p += 2;
        }
    }
}

static const uint8_t *lts_dist(const sam_lts *m, int letter, const lts_ctx *cl, const lts_ctx *co)
{
    const lts_tree *t = &m->trees[letter];
    const uint8_t *node = t->nodes;
    for (;;) {
        int skip = rd16(node), val = 0;
        const uint8_t *e;
        if (skip == 0) break;
        e = t->expr ? t->expr + rd32(node + 4) : NULL;
        while (e) {
            val = lts_expr(m, e, cl, co, &e);
            if (val) break;
        }
        node = val ? node + skip * 8 : node + skip * 8 + 8;
    }
    return t->dist + rd32(node + 4);
}

typedef struct {
    float p;
    uint8_t s[SAM_MAX_PRON];
    int n;
} lts_res;

typedef struct {
    lts_res *r;
    int n, cap;
} lts_list;

static int list_push(lts_list *l, const lts_res *x)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        lts_res *r = realloc(l->r, sizeof(lts_res) * (size_t)cap);
        if (!r) return -1;
        l->r = r;
        l->cap = cap;
    }
    l->r[l->n++] = *x;
    return 0;
}

/* FUN_5bd46675 */
static int lts_expand(const sam_lts *m, const uint8_t *letters, int nl, int i, uint8_t *outs, int no, lts_list *res)
{
    lts_ctx cl, co;
    const uint8_t *dist;
    int n, k, total = 0, last = i == nl;
    double ftot;
    outs[no] = 1;
    outs[no + 1] = 0;
    cl.buf = letters;
    cl.pos = i;
    co.buf = outs;
    co.pos = no;
    dist = lts_dist(m, letters[i], &cl, &co);
    n = (int)rd32(dist);
    for (k = 0; k < n; k++) total += (int16_t)rd16(dist + 4 + 4 * k + 2);
    ftot = (double)(float)total;
    for (k = 0; k < n; k++) {
        int sym = (int16_t)rd16(dist + 4 + 4 * k) & 0xFF, cnt = (int16_t)rd16(dist + 4 + 4 * k + 2);
        if (!(ftot * (double)0.1f < (double)cnt)) continue;
        if (last) {
            lts_res r;
            r.p = (float)((double)cnt * (1.0 / ftot));
            r.s[0] = (uint8_t)sym;
            r.n = 1;
            if (list_push(res, &r)) return -1;
        } else {
            lts_list child = {0};
            double inv = (double)(float)(1.0 / ftot);
            int c;
            if (no + 2 >= SAM_MAX_PRON) continue;
            outs[no] = (uint8_t)sym;
            if (lts_expand(m, letters, nl, i + 1, outs, no + 1, &child)) {
                free(child.r);
                return -1;
            }
            outs[no] = 1;
            for (c = 0; c < child.n; c++) {
                lts_res r;
                if (child.r[c].n + 1 >= SAM_MAX_PRON) continue;
                r.p = (float)((double)cnt * (double)child.r[c].p * inv);
                r.s[0] = (uint8_t)sym;
                memcpy(r.s + 1, child.r[c].s, (size_t)child.r[c].n);
                r.n = child.r[c].n + 1;
                if (list_push(res, &r)) {
                    free(child.r);
                    return -1;
                }
            }
            free(child.r);
        }
    }
    return 0;
}

/* MSVC CRT qsort (reproduces its order for equal keys) */
static int res_cmp(const lts_res *a, const lts_res *b) { return a->p > b->p ? -1 : (a->p < b->p ? 1 : 0); }
static void res_swap(lts_res *a, lts_res *b)
{
    lts_res t;
    if (a == b) return;
    t = *a;
    *a = *b;
    *b = t;
}
static void msvc_qsort(lts_res *a, int n)
{
    int stack_lo[40], stack_hi[40], sp = 0, lo, hi;
    if (n < 2) return;
    lo = 0;
    hi = n - 1;
    for (;;) {
        int size = hi - lo + 1;
        if (size <= 8) {
            int h = hi;
            while (h > lo) {
                int mx = lo, p;
                for (p = lo + 1; p <= h; p++)
                    if (res_cmp(&a[p], &a[mx]) > 0) mx = p;
                res_swap(&a[mx], &a[h]);
                h--;
            }
        } else {
            int mid = lo + size / 2, loguy = lo, higuy = hi + 1;
            res_swap(&a[mid], &a[lo]);
            for (;;) {
                do loguy++;
                while (loguy <= hi && res_cmp(&a[loguy], &a[lo]) <= 0);
                do higuy--;
                while (higuy > lo && res_cmp(&a[higuy], &a[lo]) >= 0);
                if (higuy < loguy) break;
                res_swap(&a[loguy], &a[higuy]);
            }
            res_swap(&a[lo], &a[higuy]);
            if (higuy - 1 - lo >= hi - loguy) {
                if (lo + 1 < higuy && sp < 40) {
                    stack_lo[sp] = lo;
                    stack_hi[sp++] = higuy - 1;
                }
                if (loguy < hi) {
                    lo = loguy;
                    continue;
                }
            } else {
                if (loguy < hi && sp < 40) {
                    stack_lo[sp] = loguy;
                    stack_hi[sp++] = hi;
                }
                if (lo + 1 < higuy) {
                    hi = higuy - 1;
                    continue;
                }
            }
        }
        if (sp == 0) return;
        sp--;
        lo = stack_lo[sp];
        hi = stack_hi[sp];
    }
}

/* phone name string ("HH EH L OW1") -> SAPI ids appended to out */
static int names_to_sapi(const sam_lts *m, const char *names, uint16_t *out, int max)
{
    int n = 0;
    const char *p = names;
    while (*p) {
        char tok[16];
        int k = 0, j;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && k < 15) tok[k++] = *p++;
        tok[k] = 0;
        if (!k) break;
        for (j = 0; j < m->n_phones; j++) {
            if (!strcmp(m->phones[j].name, tok)) {
                int q;
                for (q = 0; q < m->phones[j].n && n < max - 1; q++) out[n++] = m->phones[j].ids[q];
                break;
            }
        }
    }
    out[n] = 0;
    return n;
}

static void spell(const char *w, char *out, size_t cap)
{
    size_t len = 0, i = 0, n = strlen(w);
    out[0] = 0;
    if (n && w[0] == '\'') i = 1;
    while (i < n) {
        int c = (unsigned char)w[i];
        size_t nxt = i + 1;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            int k = lower_ascii(c) - 'a';
            const char *name;
            if (nxt < n && w[nxt] == '\'') {
                int after = nxt + 1 < n ? (unsigned char)w[nxt + 1] : 0;
                if ((after == 0 && lower_ascii(c) == 's') || after == 's' || after == 'S') {
                    k += 26;
                    if (after) nxt++;
                }
            }
            name = LETTER_NAMES[k];
            if (len + strlen(name) + 2 < cap) {
                if (len) out[len++] = ' ';
                strcpy(out + len, name);
                len += strlen(name);
            }
        }
        i = nxt;
    }
}

int sam_lts_pronounce(const sam_lts *m, const char *word, sam_pron *out, int max)
{
    char w[128], buf[512];
    uint8_t letters[132], outs[SAM_MAX_PRON + 2];
    int n = 0, i, has_vowel = 0, all_upper = 1, count = 0;
    size_t wl = 0;
    while (*word == ' ' || *word == '\t' || *word == '\n') word++;
    while (word[wl] && word[wl] != ' ' && word[wl] != '\t' && word[wl] != '\n' && wl < 127) {
        w[wl] = word[wl];
        wl++;
    }
    w[wl] = 0;
    letters[0] = 0;
    for (i = 0; w[i]; i++) {
        int c = (unsigned char)w[i], lc = lower_ascii(c), k;
        if (!has_vowel && (lc == 'a' || lc == 'e' || lc == 'i' || lc == 'o' || lc == 'u' || lc == 'y')) has_vowel = 1;
        if (all_upper && c == lc) all_upper = 0;
        for (k = 1; k < m->n_letters; k++) {
            if (m->letter_char[k] == lc) {
                if (n < 128) letters[++n] = (uint8_t)k;
                break;
            }
        }
    }
    letters[n + 1] = 0;
    if (n > 0 && n < 0x80) {
        if (n == 1) {
            int c = m->letter_char[letters[1]];
            if (c >= 'a' && c <= 'z' && count < max) {
                out[count].n = names_to_sapi(m, LETTER_NAMES[c - 'a'], out[count].ph, SAM_MAX_PRON);
                out[count].pos = 0xFFFFFFFFu;
                count++;
            }
        } else if (has_vowel) {
            lts_list res = {0};
            if (all_upper && count < max) {
                spell(w, buf, sizeof buf);
                out[count].n = names_to_sapi(m, buf, out[count].ph, SAM_MAX_PRON);
                out[count].pos = 0xFFFFFFFFu;
                count++;
            }
            outs[0] = 0;
            if (lts_expand(m, letters, n, 1, outs, 1, &res) == 0 && res.n) {
                double tot = 0.0;
                int keep = 10 - count, r;
                for (r = 0; r < res.n; r++) tot += (double)res.r[r].p;
                for (r = 0; r < res.n; r++) res.r[r].p = (float)((1.0 / tot) * (double)res.r[r].p);
                msvc_qsort(res.r, res.n);
                if (keep > res.n) keep = res.n;
                for (r = 0; r < keep && count < max; r++) {
                    size_t len = 0;
                    int s, j;
                    char clean[512];
                    size_t cl = 0;
                    if (res.r[r].p < 0.01f) continue;
                    buf[0] = 0;
                    for (s = 0; s < res.r[r].n; s++) {
                        int sym = res.r[r].s[s];
                        const char *name;
                        if (sym >= m->n_outputs) continue;
                        name = m->out_names[sym];
                        if (len + strlen(name) + 2 < sizeof buf) {
                            strcpy(buf + len, name);
                            len += strlen(name);
                            buf[len++] = ' ';
                            buf[len] = 0;
                        }
                    }
                    for (j = 0; buf[j]; j++) {
                        if (buf[j] == '#') {
                            if (buf[j + 1]) j++;
                            continue;
                        }
                        clean[cl++] = buf[j] == '_' ? ' ' : buf[j];
                    }
                    clean[cl] = 0;
                    out[count].n = names_to_sapi(m, clean, out[count].ph, SAM_MAX_PRON);
                    if (out[count].n > 0) {
                        out[count].pos = 0xFFFFFFFFu;
                        count++;
                    }
                }
            }
            free(res.r);
        } else if (count < max) {
            spell(w, buf, sizeof buf);
            out[count].n = names_to_sapi(m, buf, out[count].ph, SAM_MAX_PRON);
            out[count].pos = 0xFFFFFFFFu;
            count++;
        }
    }
    /* With nothing left the model answers "bogus pronunciation" (BOGUS, uppercase), which SAPI's
     * phone converter rejects: the engine's lookup then fails with SPERR_NOT_IN_LEX. */
    return count;
}

sam_lexicon *sam_lexicon_load(const char *path)
{
    size_t n = 0;
    uint8_t *d = slurp(path, &n);
    return d ? lexicon_from(d, n) : NULL;
}

sam_lexicon *sam_lexicon_load_mem(const void *data, size_t size)
{
    uint8_t *d = dupmem(data, size);
    return d ? lexicon_from(d, size) : NULL;
}

sam_lts *sam_lts_load(const char *path)
{
    size_t n = 0;
    uint8_t *d = slurp(path, &n);
    return d ? lts_from(d, n) : NULL;
}

sam_lts *sam_lts_load_mem(const void *data, size_t size)
{
    uint8_t *d = dupmem(data, size);
    return d ? lts_from(d, size) : NULL;
}
