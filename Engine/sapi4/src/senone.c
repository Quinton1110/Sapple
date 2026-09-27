#include "senone.h"
#include "vcrt.h"
#include <string.h>

LAYOUT(SenoneTree, qlists, 0x60);
LAYOUT(SenoneTree, roots, 0x4c);
LAYOUT(PhoneSet, nzero, 0x28);
LAYOUT(SenoneHeader, qs, 0x2c);
LAYOUT(SenoneTree, nodes, 0x5c);
LAYOUT(SenoneHeader, bit_offset, 0x20);

static int is_sil(const char *name) { return name[0] == '+' || vc_strnicmp(name, "SIL", 3) == 0; }

int senone_lookup(const SenoneTree *t, int cur, int left, int right, int pos, int state) {
    const PhoneSet *ps = GP(const PhoneSet, t->phones);
    int n = ps->nzero;
    if (cur < 0 || cur >= n || left < 0 || left >= n || right < 0 || right >= n) return -2;
    int m = GP(const int8_t, t->model)[cur];
    if (state < 0 || state >= GP(const int8_t, t->nstates)[m]) return -2;
    const TreeRoot *root = GP(const TreeRoot, GP(GPTR(const TreeRoot), t->roots)[m]) + state;
    if (root->present == 0) return -1;
    uint32_t p;
    switch (pos) {
    case 'b': case 'B': p = 1; break;
    case 'e': case 'E': p = 2; break;
    case 's': case 'S': p = 4; break;
    case 0: p = 8; break;
    default: return -2;
    }
    if (t->ignore_position) p = 8;
    GPTR(const char) *names = (GPTR(const char) *)GP(GPTR(void), ps->items);
    if (is_sil(GP(const char, names[left]))) left = t->sil;
    const char *rn = GP(const char, names[right]);
    if (is_sil(rn)) right = t->sil;
    else if (t->word_end_right >= 0 && (p == 2 || p == 4)) right = t->word_end_right;
    const SenoneHeader *h = GP(const SenoneHeader, t->hdr);
    int off = h->bit_offset, base = h->right_base;
    int L = left + off + 4, R = right + off + 4;
    int lw = L / 32;
    uint32_t lbit = 1u << (L % 32);
    uint32_t rbit = 1u << (R % 32);
    int rw = R / 32 + base;
    const uint32_t *qbits = GP(const uint32_t, t->qbits);
    const uint16_t *qlists = GP(const uint16_t, t->qlists);
    const int stride = t->qstride;
    const TreeNode *nodes = GP(const TreeNode, root->nodes), *node = nodes;
    if (off) p |= 1u << (state + 4);
    if (t->flags & 2) {
        int C = cur + off + 4;
        uint32_t cbit = 1u << (C % 32);
        int cw = C / 32 + base * 2;
        for (;;) {
            if (node->yes < 0) return node->no;
            int next = node->no;
            for (const uint16_t *q = qlists + node->qlist; *q != 0xffff; q++) {
                const uint32_t *row = qbits + stride * *q;
                if ((p & row[0]) == p && (row[lw] & lbit) && (row[rw] & rbit) && (row[cw] & cbit)) {
                    next = node->yes;
                    break;
                }
            }
            node = nodes + next;
        }
    }
    for (;;) {
        if (node->yes < 0) return node->no;
        int next = node->no;
        for (const uint16_t *q = qlists + node->qlist; *q != 0xffff; q++) {
            const uint32_t *row = qbits + stride * *q;
            if ((p & row[0]) == p && (row[lw] & lbit) && (row[rw] & rbit)) {
                next = node->yes;
                break;
            }
        }
        node = nodes + next;
    }
}

void phone_fixups(char *ph, int n) {
    static const char *const from[5] = { "DX", "AW0", "AY0", "EY0", "OY0" };
    static const char *const to[5] = { "T", "AW1", "AY1", "EY1", "OY1" };
    for (; n > 0; n--, ph += 100) {
        for (int k = 0; k < 5; k++) {
            if (vc_strcmp(ph, from[k]) == 0) {
                strcpy(ph, to[k]);
                break;
            }
        }
    }
}

// ------------------------------------------------------------------ loading (the TreeImage stream)

#include "crt_vc.h"
#include <stdlib.h>

static uint32_t bswap32(uint32_t v) { return v >> 24 | (v >> 8 & 0xff00u) | (v << 8 & 0xff0000u) | v << 24; }
static uint16_t bswap16(uint16_t v) { return (uint16_t)(v >> 8 | v << 8); }
static void rd32be(void *stm, void *dst) {
    vc_stm_read(stm, dst, 4, NULL);
    uint32_t v;
    memcpy(&v, dst, 4);
    v = bswap32(v);
    memcpy(dst, &v, 4);
}
static void rd16be(void *stm, uint16_t *dst) {
    vc_stm_read(stm, dst, 2, NULL);
    *dst = bswap16(*dst);
}
// one NUL-terminated string, a byte at a time: its length with the NUL, or -1 if the stream ended
static int32_t rd_str(void *stm, char *dst) {
    uint32_t got;
    int32_t n = 0;
    for (;;) {
        vc_stm_read(stm, dst, 1, &got);
        if (got != 1) return -1;
        n++;
        if (!*dst++) return n;
    }
}

// the header block holds pointers; a portable build's are wider than the file's layout assumes
#ifdef DECOMP_HOOK
#define HDR_EXTRA(count) 0
#else
#define HDR_EXTRA(count) ((size_t)(count) * (sizeof(SenoneQ) - 8) + offsetof(SenoneHeader, qs) - 0x2c)
#endif

int32_t senone_header_load(void *stm, SenoneTree *t, GPTR(SenoneHeader) *out) {
    int32_t v;
    char name[0x100];
    rd32be(stm, &v);
    if (v == 0) {
        *out = 0;
        return 0;
    }
    if (v < 0) {
        t->flags = (uint32_t)-v;
        rd32be(stm, &v);
    } else t->flags = 0;
    v += 8;
    SenoneHeader *h = calloc_n((uint32_t)v + (uint32_t)HDR_EXTRA(4096), 1);
    GPSET(*out, h);
    if (!h) return (int32_t)0x8007000eu;
    rd32be(stm, &h->right_base);
    int32_t len = rd_str(stm, name);
    if (len < 0) {
        vc_free(h);
        return (int32_t)0x80004005u;
    }
    rd32be(stm, &h->count);
    for (int k = 0; k < 5; k++) rd32be(stm, &h->v08[k]);
    for (int k = 0; k < 5; k++) h->v08[k] += 400;
    GPSET(h->q, h->qs);
    uint8_t *p = (uint8_t *)&h->qs[h->count];
    for (int32_t i = 0; i < h->count; i++) {
        GPSET(h->qs[i].row, (uint32_t *)(void *)p);
        p += h->right_base * 4;
    }
    GPSET(h->name, (char *)p);
#ifdef DECOMP_HOOK
    h->state_q = (uint32_t)v;       // (the block's size, overwritten before anyone reads it)
#endif
    strcpy((char *)p, name);
    p += len;
    int32_t nstate = 0;
    for (int32_t i = 0; i < h->count; i++) {
        int32_t l = rd_str(stm, (char *)p);
        if (l < 0) {
            vc_free(h);
            return (int32_t)0x80004005u;
        }
        if (vc_strnicmp((char *)p, DLLVAR(const char, 0x6371c018), 7) == 0) nstate++;     // "STATEID"
        GPSET(h->qs[i].name, (char *)p);
        p += l;
        uint32_t *row = GP(uint32_t, h->qs[i].row);
        for (int32_t k = 0; k < h->right_base; k++) rd32be(stm, &row[k]);
    }
    h->bit_offset = nstate;
    return 0;
}

#ifdef DECOMP_HOOK
#define TREE_PTR_EXTRA(n) 0
#else
#define TREE_PTR_EXTRA(n) ((size_t)(n) * (sizeof(GPTR(char)) - 4))
#endif

int32_t senone_tree_load(void *stg, const char *name, const PhoneSet *ps, GPTR(SenoneTree) *out) {
    size_t n = strlen(name) + 1;
    uint16_t *w = vc_malloc(2 * n);
    if (!w) return (int32_t)0x8007000eu;
    vc_MultiByteToWideChar_cp1252((const uint8_t *)name, (int)n, w);
    void *stm;
    int32_t hr = vc_stg_open_stream(stg, w, &stm);
    vc_free(w);
    if (hr < 0) return hr;
    SenoneTree *t = calloc_n(1, sizeof(SenoneTree));
    GPSET(*out, t);
    if (!t) {
        vc_com_release(stm);
        return (int32_t)0x8007000eu;
    }
    GPSET(t->phones, ps);
    GPTR(SenoneHeader) hdr;
    hr = senone_header_load(stm, t, &hdr);
    t->hdr = hdr;
    if (hr < 0) {
        vc_com_release(stm);
        return hr;
    }
    static char names[0x100][0x100];     // (the original's 64 KB frame)
    int32_t total = 0;
    if (t->flags & 2) {
        rd32be(stm, &t->nmodels);
        for (int32_t i = 0; i < t->nmodels; i++) {
            char *d = names[i];
            int32_t k = 0;
            do {
                vc_stm_read(stm, d, 1, NULL);
                k++;
            } while (*d++);
            total += k;
        }
    } else {
        t->nmodels = ps->nzero;
        for (int32_t i = 0; i < ps->nzero; i++) {
            const char *nm = GP(const char, GP(GPTR(void), ps->items)[i]);
            total += (int32_t)strlen(nm) + 1;
            strcpy(names[i], nm);
        }
    }
    int32_t m = t->nmodels;
    uint8_t *blk = calloc_n((uint32_t)(total + ps->nzero + m * 10 + 4) + (uint32_t)TREE_PTR_EXTRA(m), 1);
    GPSET(t->phone_units, (uint16_t *)(void *)blk);
    GPTR(char) *mnames = (GPTR(char) *)(void *)(blk + 4 * m + 4);
    GPSET(t->model_names, mnames);
    int8_t *nst = (int8_t *)(mnames + m);
    GPSET(t->nstates, nst);
    GPSET(t->tree_states, nst + m);
    int8_t *model = nst + 2 * m;
    GPSET(t->model, model);
    char *sp = (char *)(model + ps->nzero);
    for (int32_t i = 0; i < m; i++) {
        GPSET(mnames[i], sp);
        strcpy(sp, names[i]);
        sp += strlen(names[i]) + 1;
    }
    if (t->flags & 2) vc_stm_read(stm, model, (uint32_t)ps->nzero, NULL);
    else
        for (int32_t i = 0; i < ps->nzero; i++) model[i] = (int8_t)i;
    SenoneHeader *h = GP(SenoneHeader, t->hdr);
    if (h) {
        t->qstride = h->right_base * 2;
        if (t->flags & 2) t->qstride += h->right_base;
        if (t->flags & 1) t->qstride += h->right_base * 2;
        char a[0x100], b[0x60];
        int32_t la = rd_str(stm, a), lb = la < 0 ? -1 : rd_str(stm, b);
        if (la < 0 || lb < 0) goto bad;
        char *s = calloc_n((uint32_t)(la + lb), 1);
        GPSET(t->str_a, s);
        strcpy(s, a);
        GPSET(t->str_b, s + strlen(a) + 1);
        strcpy(GP(char, t->str_b), b);
    }
    int32_t chk;
    rd32be(stm, &chk);
    if (chk != ps->nzero) goto bad;
    vc_stm_read(stm, nst, (uint32_t)m, NULL);
    for (int32_t i = 0; i < m; i++) t->total_states += nst[i];
    if (!h) GPSET(t->tree_states, (int8_t *)GP(const int8_t, t->nstates));
    else {
        uint32_t *units = (uint32_t *)(void *)blk;
        for (int32_t i = 0; i < m + 1; i++) rd32be(stm, &units[i]);
        int8_t *ts = GP(int8_t, t->tree_states);
        if (h->bit_offset == 0) {
            h->state_q = 0;
            for (int32_t i = 0; i < m; i++) ts[i] = nst[i];
        } else {
            // STATEID questions: the name strings move up to make room for an int per state
            SenoneQ *q = GP(SenoneQ, h->q);
            h->bit_offset = 0;
            for (int32_t i = 0; i < m; i++)
                if (nst[i] > h->bit_offset) h->bit_offset = nst[i];
            GPSET(h->state_q, (int32_t *)(void *)GP(char, h->name));
            int32_t shift = h->bit_offset * 4;
            for (int32_t k = h->count - 1; k >= 0; k--) {
                char *nm = GP(char, q[k].name);
                for (int32_t c = (int32_t)strlen(nm); c >= 0; c--) nm[c + shift] = nm[c];
                GPSET(q[k].name, nm + shift);
            }
            char *hn = GP(char, h->name);
            for (int32_t c = (int32_t)strlen(hn); c >= 0; c--) hn[c + shift] = hn[c];
            GPSET(h->name, hn + shift);
            int32_t *sq = GP(int32_t, h->state_q);
            for (int32_t i = 0; i < h->bit_offset; i++) sq[i] = -1;
            for (int32_t k = 0; k < h->count; k++) {
                const char *nm = GP(const char, q[k].name);
                if (vc_strnicmp(nm, DLLVAR(const char, 0x6371c018), 7) == 0) sq[vc_atoi(nm + 7)] = k + 400;
            }
            for (int32_t i = 0; i < m; i++) ts[i] = 1;
        }
    }
    rd32be(stm, &t->max_senone);
    rd32be(stm, &t->f0c);
    rd32be(stm, &t->ignore_position);
    rd32be(stm, &t->sil);
    rd32be(stm, &t->word_end_right);
    if (named_index(ps, DLLVAR(const char, 0x6371c010)) != t->sil) goto bad;     // "SIL"
    if (named_index(ps, DLLVAR(const char, 0x6371c07c)) != t->word_end_right) goto bad;    // "#"
    {
        int32_t sum = 0;
        for (int32_t i = 0; i < m; i++) sum += nst[i];
        uint8_t *rb = calloc_n((uint32_t)m * sizeof(GPTR(TreeRoot)) + (uint32_t)sum * sizeof(TreeRoot), 1);
        GPTR(const TreeRoot) *roots = (GPTR(const TreeRoot) *)(void *)rb;
        GPSET(t->roots, roots);
        TreeRoot *r = (TreeRoot *)(void *)(rb + (size_t)m * sizeof(GPTR(TreeRoot)));
        for (int32_t i = 0; i < m; i++) {
            GPSET(roots[i], r);
            r += nst[i];
        }
    }
    if (h) {
        rd32be(stm, &t->nrows);
        uint32_t *qb = calloc_n((uint32_t)(t->nrows * t->qstride), 4);
        GPSET(t->qbits, qb);
        for (int32_t i = 0; i < t->nrows * t->qstride; i++) rd32be(stm, &qb[i]);
        int32_t nnodes, nq;
        rd32be(stm, &nnodes);
        rd32be(stm, &nq);
        TreeNode *nodes = calloc_n((uint32_t)nnodes, 8);
        GPSET(t->nodes, nodes);
        uint16_t *ql = calloc_n((uint32_t)nq, 2), *qc = ql;
        GPSET(t->qlists, ql);
        int8_t *ts = GP(int8_t, t->tree_states);
        for (int32_t i = 0; i < m; i++)
            for (int32_t j = 0; j < ts[i]; j++) {
                TreeRoot *root = GP(TreeRoot, GP(GPTR(const TreeRoot), t->roots)[i]) + j;
                rd16be(stm, &root->present);
                rd16be(stm, &root->_2);
                if (root->present == 0) continue;
                GPSET(root->nodes, nodes);
                for (int32_t k = 0; k < (int16_t)root->present; k++, nodes++) {
                    rd16be(stm, (uint16_t *)&nodes->yes);
                    rd16be(stm, (uint16_t *)&nodes->no);
                    rd16be(stm, (uint16_t *)&nodes->_6);
                    if (nodes->yes < 0) continue;
                    nodes->qlist = (uint16_t)((qc - ql) >> 0);
                    uint16_t c;
                    do {
                        rd16be(stm, qc);
                        c = *qc++;
                    } while (c != 0xffff);
                }
            }
    }
    vc_com_release(stm);
    if (h && h->bit_offset) {
        const int8_t *nstates = GP(const int8_t, t->nstates);
        for (int32_t i = 0; i < m; i++)
            for (int32_t j = 1; j < nstates[i]; j++) {
                TreeRoot *r0 = GP(TreeRoot, GP(GPTR(const TreeRoot), t->roots)[i]);
                r0[j] = r0[0];
            }
    }
    return 0;
bad:
    return (int32_t)0x80004005u;
}
