#include "fe_lex.h"
#include <string.h>

uint32_t lex_pack(uint8_t bits, uint16_t count) {
    const uint8_t *codes = DLLVAR(const uint8_t, LEX_CODES);
    uint8_t *key = DLLVAR(uint8_t, LEX_KEY);
    uint32_t acc = 0;
    int16_t out = 0, i = 0;
    uint8_t used = bits;                    // (bits in the window after the next code, plus `bits`)
    do {
        used = (uint8_t)(used + bits);
        acc += codes[i++];
        uint8_t sh = bits;
        if ((int8_t)used >= 16) {
            sh = (uint8_t)(bits - used + 16);
            acc = (acc << (sh & 31)) & 0xffff;
            key[out++] = (uint8_t)(acc >> 8);
            uint8_t rest = (uint8_t)(bits - sh);
            acc = (acc << (rest & 31)) & 0xffff;
            used = (uint8_t)(rest + 8);
        } else {
            acc = (acc << (sh & 31)) & 0xffff;
        }
    } while (--count != 0);
    acc = (acc << ((uint8_t)(16 - used) & 31)) & 0xffff;
    key[out] = (uint8_t)(acc >> 8);
    key[out + 1] = (uint8_t)acc;
    return acc;
}

uint32_t lex_field(uint32_t table, uint16_t idx, int n) {
    const uint16_t *t = DLLVAR(const uint16_t, table);
    uint32_t bit = (uint32_t)idx * (uint32_t)n;
    uint32_t w = (bit >> 4) & 0xffff, b = bit & 15;
    if (b + (uint32_t)n > 16)   // (the original adds the second word in a register that held 2 * w)
        return ((t[w] & (0xffffu >> b)) << (b - (16 - n))) + (((2 * w) & 0xffff0000u) | (uint16_t)(t[w + 1] >> (32 - n - b)));
    return (uint16_t)(t[w] >> (16 - n - b)) & ((1u << n) - 1);
}

uint32_t lex_field18(uint16_t idx) {
    const uint16_t *t = DLLVAR(const uint16_t, 0x63722090);
    uint32_t bit = (uint32_t)idx * 18u;
    uint32_t w = bit >> 4, b = bit & 15;
    uint32_t v = ((uint32_t)t[w] & (0xffffu >> b)) << (b + 2);
    if (b + 18 > 32) return v + ((uint32_t)t[w + 1] << (b - 14)) + ((uint32_t)t[w + 2] >> (30 - b));
    return v + ((uint32_t)t[w + 1] >> (14 - b));
}

#include "fe_rules.h"

int16_t lex_bsearch8w(uint32_t table, uint32_t base, uint8_t lo, uint8_t hi, uint8_t width) {
    const uint8_t *t = DLLVAR(const uint8_t, table), *key = DLLVAR(const uint8_t, LEX_KEY);
    while (hi > lo) {
        uint8_t mid = (uint8_t)(lo + (((int)hi - (int)lo) >> 1));
        uint16_t pos = (uint16_t)(width * mid + base);
        uint8_t k = 0;
        if (key[0] == t[pos]) {
            while (k < (uint8_t)(width - 1)) {
                pos++;
                k++;
                if (key[k] != t[pos]) break;
            }
        }
        if (key[k] > t[pos]) lo = (uint8_t)(mid + 1);
        else hi = mid;
    }
    uint16_t at = (uint16_t)(width * lo + base);
    for (uint8_t k = 0; k < width; k++)
        if (key[k] != t[at + k]) return -1;
    return lo;
}

int16_t lex_bsearch16(uint32_t table, uint32_t base, uint16_t lo, uint16_t hi, uint8_t width) {
    const uint8_t *t = DLLVAR(const uint8_t, table), *key = DLLVAR(const uint8_t, LEX_KEY);
    while (hi > lo) {
        uint32_t mid = lo + (((int32_t)hi - (int32_t)lo) >> 1);
        uint16_t pos = (uint16_t)(mid * width + base);
        uint8_t k = 0;
        if (key[0] == t[pos]) {
            while (k < (uint8_t)(width - 1)) {
                pos++;
                k++;
                if (key[k] != t[pos]) break;
            }
        }
        if (key[k] > t[pos]) lo = (uint16_t)(mid + 1);
        else hi = (uint16_t)mid;
    }
    uint16_t at = (uint16_t)(lo * width + base);
    for (uint8_t k = 0; k < width; k++)
        if (key[k] != t[(uint16_t)at + k]) return -1;
    return (int16_t)lo;
}

int tapes_printable(int16_t lo, int16_t hi) {
    for (int16_t i = lo; i <= hi; i++) {
        uint8_t ch = (uint8_t)GP(char, *DLLPTR(char, TAPE_A))[i];
        if (ch < 0x20 || ch > 0xfe) return 0;
    }
    return 1;
}

#define D_BITS(t, n) LEX_FIELD_BITS, t, n
const LexDict lex_dicts[] = {
    // 0 @0x6369a4ea
    { 0x63719707, 0x63719480, 0x1e, 0, LEX_RANGE_SBYTE, 0x63719627, 0x63719808, LEX_SEARCH_BYTE, 0x63718da0,
      D_BITS(0x6371a748, 12), 0x63719818, 0x6371a546, 0x63739958, 0x63739988, 0x63739e68, 5, 14, 0x21, '!', 3, 0 },
    // 1 @0x6369773d
    { 0x636cb8d7, 0x636cb670, 0x1c, 0, LEX_RANGE_SBYTE, 0x636cb7f7, 0x636cb9d8, LEX_SEARCH_BYTE, 0x636cad90,
      D_BITS(0x636cd1d0, 13), 0x636cb9e8, 0x636ccfce, 0x63739788, 0x637397b8, 0x63739e6a, 5, 14, 0x21, '#', 3, 1 },
    // 2 @0x636980bf
    { 0x636ce5ff, 0x636ce710, 0x1a, 0, LEX_RANGE_SBYTE, 0x636ce51f, 0x636ce700, LEX_SEARCH_BYTE, 0x636ce848,
      D_BITS(0x636cefd0, 11), 0x636cdda8, 0x636cdba6, 0x637397f8, 0x63739828, 0, 6, 11, 0x21, '!', 3, 0 },
    // 3 @0x63698a3e
    { 0x636d09ff, 0x636d0730, 0x42, 0, LEX_RANGE_UBYTE, 0x636d091f, 0x636d0b00, LEX_SEARCH_WORD8, 0x636d01a0,
      D_BITS(0x636d1bd8, 12), 0x636d0b08, 0x636d19d6, 0x63739868, 0x63739898, 0, 6, 7, 0x21, '!', 3, 0 },
    // 4 @0x63698594
    { 0x636cf5b7, 0x636cf430, 0x1c, 0, LEX_RANGE_UBYTE, 0x636cf4d7, 0x636cf6b8, LEX_SEARCH_WORD8, 0x636cf1e0,
      D_BITS(0x636cffe0, 11), 0x636cf6c0, 0x636cfdde, 0x63739830, 0x63739860, 0x63739e69, 5, 6, 0x21, '#', 2, 0 },
    // 5 @0x63698f1a
    { 0x636d2b6f, 0x636d2c80, 0x1e, 0, LEX_RANGE_UBYTE, 0x636d2a8f, 0x636d2c70, LEX_SEARCH_WORD8, 0x636d2db0,
      D_BITS(0x636d33f8, 12), 0x636d20d0, 0x636d1ece, 0x637398a0, 0x637398d0, 0, 5, 9, 0x21, '"', 3, 0 },
    // 6 @0x636993f8
    { 0x636d3ca7, 0x636d3db0, 0x1c, 1, LEX_RANGE_SBYTE, 0x636d3bc7, 0x636d3da8, LEX_SEARCH_BYTE, 0x636d3e78,
      D_BITS(0x636d4078, 10), 0x636d3868, 0x636d3716, 0x637398d8, 0x63739908, 0, 5, 6, 0x21, 0, 1, 0 },
    // 7 @0x63697c17
    { 0x636cd858, 0x636cd968, 0x14, 1, LEX_RANGE_SBYTE, 0x636cd778, 0x636cd958, LEX_SEARCH_BYTE, 0x636cda80,
      D_BITS(0x636cdba8, 9), 0x636cd5d8, 0x636cd4b0, 0x637397c0, 0x637397f0, 0, 5, 13, 0x20, '!', 2, 0 },
    // 8 @0x636972c7
    { 0x636c24ff, 0x636c2618, 0x3a, 0, LEX_RANGE_WORD, 0x636c241f, 0x636c2600, LEX_SEARCH_WORD16, 0x636c2a30,
      LEX_FIELD_TABLE16, 0x636c8080, 16, 0x636b99a8, 0x636b97a6, 0x63739750, 0x63739780, 0x63739e6b, 5, 17, 0x21, '!', 3, 0 },
};

#define TP(addr) GP(char, *DLLPTR(char, addr))

uint8_t lex_lookup(const LexDict *d, int16_t *left, int16_t *len) {
    uint8_t ok = 1;
    uint8_t *buf = DLLVAR(uint8_t, d->buf);
    int16_t *ends = DLLVAR(int16_t, d->ends);
    (*left)--;
    (*len)--;
    int16_t n = (int16_t)(*len - *left);
    if (n < 0 || n > d->maxlen) goto fail;
    if (!(d->minchar == 0x20 ? tapes_printable(*left, *len) : tapes_graphic(*left, *len))) goto fail;
    {
        uint8_t col = DLLVAR(const uint8_t, d->first_map)[(uint8_t)TP(TAPE_A)[*left]];
        if (col == 0xff) goto fail;
        const uint8_t *row = DLLVAR(const uint8_t, d->rows) + n * d->row_stride;
        int32_t lo, hi;                 // the column's entries lo..hi (compared as the original does)
        switch (d->range) {
        case LEX_RANGE_SBYTE: lo = (int8_t)row[4 + col]; hi = (int8_t)(row[5 + col] - 1); break;
        case LEX_RANGE_UBYTE: lo = row[4 + col]; hi = (int16_t)(row[5 + col] - 1); break;
        default:
            lo = (int16_t)(row[4 + 2 * col] | row[5 + 2 * col] << 8);
            hi = (int16_t)((row[6 + 2 * col] | row[7 + 2 * col] << 8) - 1);
        }
        int32_t r;
        if (lo > hi) r = -1;
        else if (n > 0) {
            for (int16_t i = (int16_t)(*left + 1); i <= *len; i++)
                DLLVAR(uint8_t, LEX_CODES)[i - *left - 1] = DLLVAR(const uint8_t, d->code_map)[(uint8_t)TP(TAPE_A)[i]];
            lex_pack(d->codebits, (uint8_t)n);
            uint16_t base = (uint16_t)(row[2] | row[3] << 8);
            uint8_t width = DLLVAR(const uint8_t, d->widths)[n];
            switch (d->search) {
            case LEX_SEARCH_BYTE:
                r = (int8_t)rule_bsearch(DLLVAR(const uint8_t, d->keys), base, (uint8_t)lo, (uint8_t)hi, width);
                break;
            case LEX_SEARCH_WORD8: r = lex_bsearch8w(d->keys, base, (uint8_t)lo, (uint8_t)hi, width); break;
            default: r = lex_bsearch16(d->keys, base, (uint16_t)lo, (uint16_t)hi, width);
            }
        } else r = (int16_t)lo;
        if (r < 0) goto fail;
        uint16_t e = (uint16_t)(r + (d->base_byte ? row[0] : (int16_t)(row[0] | row[1] << 8)));
        uint16_t p;
        switch (d->field) {
        case LEX_FIELD_BITS: p = (uint16_t)lex_field(d->fields, e, d->nbits); break;
        case LEX_FIELD_TABLE16: p = DLLVAR(const uint16_t, d->fields)[e]; break;
        default: p = (uint16_t)lex_field18(e);
        }
        // expand the pronunciation: symbols with a pair expand in place, recursively, left to right
        const uint8_t *prons = DLLVAR(const uint8_t, d->prons), *pairs = DLLVAR(const uint8_t, d->pairs);
        int16_t cnt = 0, k = 0;
        for (uint8_t c; (c = prons[p]) != 5; p++) {
            buf[cnt++] = c;
            do {
                while (pairs[buf[k] * 2] != 0x20) {
                    for (int16_t m = (int16_t)(cnt - 1); m > k; m--) buf[m + 1] = buf[m];
                    cnt++;
                    uint8_t e0 = buf[k];
                    buf[k + 1] = pairs[e0 * 2 + 1];
                    buf[k] = pairs[e0 * 2];
                }
                k++;
            } while (k < cnt);
        }
        // the fields: all but the last end at the separator, the last takes the rest
        static const uint32_t tapes[2][3] = { { TAPE_A, TAPE_C, TAPE_D }, { TAPE_C, TAPE_A, TAPE_D } };
        static const int end_of[2][3] = { { 0, 2, 3 }, { 2, 0, 3 } };
        const uint32_t *out_tape = tapes[d->c_first];
        const int *out_end = end_of[d->c_first];
        for (int t = 0; t < 4; t++) ends[t] = *len;
        int16_t i = 0;
        for (int f = 0; f < d->nout; f++) {
            int16_t q = *left;
            if (f < d->nout - 1) {
                for (uint8_t c; (c = buf[i]) != d->sep; i++) TP(out_tape[f])[q++] = (char)c;
                i++;
            } else {
                for (; i < cnt; i++) TP(out_tape[f])[q++] = (char)buf[i];
            }
            ends[out_end[f]] = (int16_t)(q - 1);
        }
        *len = tapes_pad(ends, *left);
        goto out;
    }
fail:
    ok = 0;
out:
    if (d->flag) *DLLVAR(uint8_t, d->flag) = ok;
    (*left)++;
    (*len)++;
    return ok;
}

// ---- the main lexicon: a Huffman-coded trie

#define STREAM 0x636fdbb8
#define CUR_W (*DLLVAR(uint32_t, LEX_CUR_WORD))
#define CUR_B (*DLLVAR(uint8_t, LEX_CUR_BIT))

// walk a Huffman tree from `root`, one stream bit per step (set: first child, clear: second) until a leaf
static uint32_t huff_walk(uint32_t nodes, uint32_t root, int wide) {
    const uint16_t *s = DLLVAR(const uint16_t, STREAM);
    const uint8_t *nb = DLLVAR(const uint8_t, nodes);
    const uint16_t *nw = DLLVAR(const uint16_t, nodes);
#define LEAF(n) (wide ? nw[2 * (n)] == 0xffff : nb[2 * (n)] == 0xff)
    uint32_t n = root;
    if (LEAF(n)) return n;
    uint16_t word = s[CUR_W];
    do {
        uint8_t b = CUR_B;
        uint32_t bit = (1u << b) & word;
        if (b < 15) CUR_B = (uint8_t)(b + 1);
        else { CUR_B = 0; CUR_W++; word = s[CUR_W]; }
        n = wide ? (bit ? nw[2 * n] : nw[2 * n + 1]) : (bit ? nb[2 * n] : nb[2 * n + 1]);
    } while (!LEAF(n));
    return n;
#undef LEAF
}

uint32_t lex_huff_branches(void) { uint32_t n = huff_walk(0x636fc4d0, 0x5c, 0); return (n & ~0xffu) | DLLVAR(const uint8_t, 0x636fc590)[n]; }
uint32_t lex_huff_letter(void) { uint32_t n = huff_walk(0x636fc5c0, 0x62, 0); return (n & ~0xffu) | DLLVAR(const uint8_t, 0x636fc688)[n]; }
uint32_t lex_huff_step(void) { uint32_t n = huff_walk(0x636fc6c0, 0x48, 0); return (n & ~0xffu) | DLLVAR(const uint8_t, 0x636fc758)[n]; }
uint32_t lex_huff_size(void) { uint32_t n = huff_walk(0x636fc780, 0x1b8, 1); return (n & ~0xffffu) | DLLVAR(const uint16_t, 0x636fce68)[n]; }

// n-th 19-bit value of an array packed into 16-bit words, least significant bits first
static uint32_t bits19(const uint16_t *t, uint32_t w, uint32_t b) {
    uint32_t v = (uint32_t)t[w] >> b;
    if (b + 19 > 32) return v + ((uint32_t)t[w + 1] << (16 - b)) + ((0xffffu >> (29 - b) & t[w + 2]) << (32 - b));
    return v + ((0xffffu >> (13 - b) & t[w + 1]) << (16 - b));
}

uint32_t lex_huff_value(void) {
    uint32_t n = huff_walk(0x636fd028, 0x238, 1) & 0xffff;
    uint32_t bit = (uint32_t)(uint16_t)n * 19u;
    // (the original: b = low byte of 3 * (n & 0xff), mod 16; the same as bit & 15)
    uint32_t v = bits19(DLLVAR(const uint16_t, 0x636fd910), (bit >> 4) & 0xffff, bit & 15);
    return v > 3 ? v + v : v;
}

uint32_t lex_raw19(void) {
    uint8_t b = CUR_B;
    uint32_t v = bits19(DLLVAR(const uint16_t, STREAM), CUR_W, b);
    uint8_t nb = (uint8_t)(b + 19);
    CUR_B = nb & 15;
    CUR_W += (nb >> 4) & 3;
    return v + v;
}

// round the cursor up to an even bit
static void align2(void) {
    uint8_t b = CUR_B;
    if (b & 1) { do b++; while (b & 1); CUR_B = b; }
    if (b == 16) { CUR_B = 0; CUR_W++; }
}

static uint32_t value_or_raw(void) { uint32_t r = lex_huff_value(); return r ? r : lex_raw19(); }

void lex_skip_node(void) {
    uint8_t n = (uint8_t)lex_huff_branches();
    uint8_t c = 0;
    if (n < 0x39) {
        c = (uint8_t)lex_huff_letter();
        if (c != 0x0e) { value_or_raw(); lex_huff_size(); }
        for (uint8_t j = 1; n > 1 && j < n; j++) {
            c = (uint8_t)(c + lex_huff_step());
            if (c == 0x0e) continue;
            value_or_raw();
            if (j < (uint8_t)(n - 1)) lex_huff_size();
        }
    } else {
        // (n == 0x39 would leave c as the original's uninitialised local: 0x39 is not a code of the tree)
        for (uint32_t k = (uint8_t)(n - 0x39); k; k--) c = (uint8_t)lex_huff_letter();
        if (c != 0x0e) value_or_raw();
    }
    align2();
}

uint32_t lex_trie_step(uint32_t start, uint8_t ch, uint32_t *count, uint32_t *acc) {
    CUR_W = start >> 4;
    CUR_B = (uint8_t)(start & 15);
#define ACC16(v) (*acc = (*acc & 0xffff0000u) | (uint16_t)(v))
    ACC16(0);
    uint16_t si = 1;
    uint8_t n = (uint8_t)lex_huff_branches();
    uint32_t r = 0;
    if (n >= 0x39) {
        uint8_t k = (uint8_t)(n - 0x39), c;
        uint16_t i = 0;
        do { c = (uint8_t)lex_huff_letter(); i++; } while (i <= (uint16_t)*count);
        if (c != ch) return 0;
        ACC16(0);
        *count = (*count & 0xffff0000u) | (uint16_t)(*count + 1);
        if ((uint16_t)*count < k) return start;
        *count &= 0xffff0000u;
        if (ch == 0x0e) return 1;
        r = lex_huff_value();
        if (r == 0) return lex_raw19();
        if (r > 3) return r;
        align2();
        for (uint32_t m = (uint16_t)r; m > 1; m--) lex_skip_node();
        return (CUR_W << 4) + CUR_B;
    }
    uint8_t c = (uint8_t)lex_huff_letter();
    if (c != 0x0e) {
        r = value_or_raw();
        if (c != ch) ACC16(*acc + lex_huff_size());
    } else if (ch != 0x0e) ACC16(*acc + 1);
    uint16_t bx = n;
    if (bx > 1) {
        while (c < ch) {
            c = (uint8_t)(c + lex_huff_step());
            if (c != 0x0e) {
                r = value_or_raw();
                if (c != ch) ACC16(*acc + lex_huff_size());
            } else if (ch != 0x0e) ACC16(*acc + 1);
            if (++si >= bx) break;
        }
    }
    if (c != ch) return 0;
    if (ch == 0x0e) return 1;
    if (r > 3) return r;
    if (si < bx) {
        lex_huff_size();
        for (; si < bx; si++) {
            c = (uint8_t)(c + lex_huff_step());
            if (c == 0x0e) continue;
            value_or_raw();
            if (si < (uint16_t)(bx - 1)) lex_huff_size();
        }
    }
    align2();
    for (uint32_t m = (uint16_t)r; m > 1; m--) lex_skip_node();
    return (CUR_W << 4) + CUR_B;
#undef ACC16
}

int32_t lex_trie_lookup(uint16_t lo, uint16_t hi) {
    const uint8_t *map = DLLVAR(const uint8_t, 0x636fc3cf);
    const uint8_t *firsts = DLLVAR(const uint8_t, 0x63718cc0);
    const uint16_t *counts = DLLVAR(const uint16_t, 0x63718d60);
    const uint8_t *ta = (const uint8_t *)TP(TAPE_A);
    uint32_t count = 0, acc = 0;
    uint8_t cl = map[ta[lo]];
    uint16_t dx = 0;
    for (; dx < 0x1f; dx++) {
        if (cl <= firsts[dx]) break;
        acc += counts[dx];
    }
    if (dx == 0x1f || cl != firsts[dx]) return -1;
    uint32_t cur = DLLVAR(const uint32_t, 0x63718ce0)[dx];
    uint32_t sum = acc;
    if (cur <= 3) {
        CUR_B = 4;
        CUR_W = 0x42;
        for (uint32_t m = (uint16_t)cur; m > 1; m--) lex_skip_node();
        cur = (CUR_W << 4) + CUR_B;
    }
    uint16_t n = (uint16_t)(hi - lo + 1);
    for (uint16_t si = 1;; si++) {
        uint8_t ch = si == n ? 0x0e : map[ta[(uint32_t)lo + si]];
        uint32_t r = lex_trie_step(cur, ch, &count, &acc);
        if (r == 0) return -1;
        sum += acc;
        cur = r;
        if (ch == 0x0e) return r == 1 ? (int32_t)(uint16_t)sum : -1;
    }
}

// expand symbol buf[m] in place (and what it expands to), shifting buf[m+1..*cnt-1] up
static void expand_at(uint8_t *buf, int16_t m, int16_t *cnt, const uint8_t *pairs) {
    while (pairs[buf[m] * 2] != 0x20) {
        for (int16_t j = (int16_t)(*cnt - 1); j > m; j--) buf[j + 1] = buf[j];
        (*cnt)++;
        uint8_t e = buf[m];
        buf[m + 1] = pairs[e * 2 + 1];
        buf[m] = pairs[e * 2];
    }
}

uint8_t lex_main(int16_t *left, int16_t *len) {
    const uint8_t *T = DLLVAR(const uint8_t, 0x636d4340);      // pronunciation symbols
    const uint8_t *pairs = DLLVAR(const uint8_t, 0x636d413e);
    uint8_t *buf = DLLVAR(uint8_t, 0x63739910);
    int16_t *ends = DLLVAR(int16_t, 0x63739948);
    uint8_t ok = 1;
    (*left)--;
    (*len)--;
    int16_t n = (int16_t)(*len - *left + 1);
    int32_t e;
    if (n < 0 || n > 0x1f || !tapes_graphic(*left, *len) || (e = lex_trie_lookup((uint16_t)*left, (uint16_t)*len)) < 0) {
        ok = 0;
        goto out;
    }
    {
        uint32_t p = lex_field18((uint16_t)e);
        int16_t k = 0, m = 0;
        uint8_t s = T[p];
        // the entry's symbols, expanded, up to the end (5) or a reference (0x7b..0x8a)
        do {
            buf[k++] = s;
            p++;
            do { expand_at(buf, m, &k, pairs); m++; } while (m < k);
            s = T[p];
        } while (!(s == 5 || (s >= 0x7b && s <= 0x8a)));
        // a leading reference 0x7b + i: replace the first symbol by the next i + 1 of the stream
        // (expanded), as long as references follow
        uint8_t dl = buf[0], prev = dl;
        uint8_t tmp[64];
        while (dl >= 0x7b && dl <= 0x8a) {
            uint8_t al;
            for (;;) {
                al = T[p];
                if (al == 5) { p++; break; }
                if (al >= 0x7b && al <= 0x8a) break;
                p++;
            }
            if (!(al >= 0x7b && al <= 0x8a)) al = 0x7a;
            if (!(dl > al)) { p++; continue; }
            int16_t bx = 0, tm = 0;
            uint8_t left_n = dl;
            do {
                tmp[bx++] = T[p++];
                for (;;) {
                    expand_at(tmp, tm, &bx, pairs);
                    tm++;
                    left_n--;
                    if (!(tm < bx && left_n >= al)) break;
                }
            } while (left_n >= al);
            int16_t add = (int16_t)(prev - al);
            int16_t nk = (int16_t)(add + k);
            if (k > 1)
                for (int16_t j = (int16_t)(k - 1); j >= 1; j--) buf[j + add] = buf[j];
            if (add >= 0) memcpy(buf, tmp, (size_t)(add + 1));
            k = nk;
            prev = al;
            dl = al;
        }
        for (int t = 0; t < 4; t++) ends[t] = *len;
        int16_t i = 0, q = *left;
        for (uint8_t c = buf[0]; c != '!'; c = buf[++i]) TP(TAPE_A)[q++] = (char)c;
        i++;
        ends[0] = (int16_t)(q - 1);
        q = *left;
        for (; i < k; i++) TP(TAPE_C)[q++] = (char)buf[i];
        ends[2] = (int16_t)(q - 1);
        *len = tapes_pad(ends, *left);
    }
out:
    (*left)++;
    (*len)++;
    return ok;
}
