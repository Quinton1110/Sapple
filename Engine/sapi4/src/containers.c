#include "containers.h"
#include "crt_vc.h"
#include "vcrt.h"
#include "x87.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LAYOUT(NamedTable, count, 0x24);
LAYOUT(HashTable, moved, 0x18);
LAYOUT(HashTable, probes, 0x10);
LAYOUT(IntVec, data, 0x0c);
LAYOUT(VecArray, items, 0x10);
LAYOUT(Blob, bytes, 0x10);

int hash_lookup(HashTable *t, const char *key, int32_t *out) {
    if (!t || !key || !out) return -1;
    uint32_t h = 0;
    int k = -1;
    const char *p = key;
    signed char ch = (signed char)*p;
    do {
        h += (uint32_t)(int32_t)ch << (k & 15);
        k--;
        ch = (signed char)*++p;
    } while (ch);
    t->lookups++;
    for (;;) {
        if (!t->size) break;
        h %= t->size;
        const HashEntry *e = GP(const HashEntry, t->entries) + h;
        if (!e->key) break;
        if (vc_strcmp(GP(const char, e->key), key) == 0) {
            *out = e->value;
            return 0;
        }
        h++;
        t->probes++;
    }
    *out = (int32_t)h;
    return -1;
}

int named_index(const NamedTable *t, const char *key) {
    int32_t v;
    if (hash_lookup(GP(HashTable, t->names), key, &v)) return -1;
    return v;
}

GPTR(void) named_item(const NamedTable *t, int i) { return GP(GPTR(void), t->items)[i]; }

int prefix_cmp(const char *a, const char *b, int32_t *n) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (lb > la) return -1;
    if (lb < la) la = lb;
    *n = la;
    return vc_memcmp(a, b, (size_t)(uint32_t)la);
}

IntVec *intvec_ctor(IntVec *v) {
    v->cap = 0;
    v->data = 0;
    v->count = 0;
    v->step = 10;
    return v;
}

void intvec_free_data(IntVec *v) {
    if (v->data) vc_free(GP(void, v->data));
}

void intvec_reserve(IntVec *v, int n, int step) {
    if (n > v->cap) {
        GPSET(v->data, (int32_t *)vc_realloc(GP(void, v->data), (size_t)(uint32_t)(n * 4)));
        v->cap = n;
    }
    v->step = step > 0 ? step : 10;
}

VecArray *vecarray_ctor(VecArray *a) {
    memset(a, 0, sizeof *a);   // (a 0.0f and four zero words)
    return a;
}

void vecarray_destroy(VecArray *a) {
    for (int i = 0; i < a->count; i++) {
        IntVec *v = GP(IntVec, GP(GPTR(IntVec), a->items)[i]);
        if (!GP(GPTR(IntVec), a->items)[i]) continue;
        intvec_free_data(v);
        vc_free(v);
    }
    if (a->items) vc_free(GP(void, a->items));
    if (a->result) vc_free(GP(void, a->result));
}

int vecarray_init(VecArray *a, int n, int cap, int step) {
    a->count = n;
    GPTR(IntVec) *items = vc_malloc((size_t)(uint32_t)n * sizeof(GPTR(IntVec)));
    GPSET(a->items, items);
    if (!items) return -1;
    for (int i = 0; i < a->count; i++) {
        IntVec *v = vc_malloc(sizeof(IntVec));
        GPSET(items[i], v ? intvec_ctor(v) : NULL);
        if (!items[i]) {
            a->count = i;
            return -1;
        }
        intvec_reserve(GP(IntVec, items[i]), cap, step);
    }
    return 0;
}

int blob_copy80(const void *src, Blob *out) {
    out->bytes = 0x50;
    void *d = vc_malloc(0x50);
    GPSET(out->data, d);
    memcpy(d, src, (size_t)out->bytes);
    out->type = 4;
    return 1;
}

int32_t intvec_append(IntVec *v, int32_t x) {
    if (v->count >= v->cap) {
        v->cap += v->step;
        GPSET(v->data, (int32_t *)vc_realloc(GP(void, v->data), (size_t)(uint32_t)(v->cap * 4)));
    }
    GP(int32_t, v->data)[v->count] = x;
    return v->count++;
}

int32_t vecarray_append(VecArray *a, int32_t i, int32_t x) {
    if (i > a->count || i < 0) return -1;
    intvec_append(GP(IntVec, GP(GPTR(IntVec), a->items)[i]), x);
    return 0;
}

double lattice_cost(int32_t a, int32_t b) { return a == b ? 0.0 : 1.0; }

int array_lacks(int16_t v, int32_t n, const int16_t *a) {
    for (int32_t i = 0; i < n; i++)
        if (a[i] == v) return 0;
    return 1;
}

static double cost(uint32_t fn, int32_t a, int32_t b) {
    if (CODEADDR(fn) == 0x63685d00) return lattice_cost(a, b);
    return lattice_cost_other(fn, a, b);
}

int32_t lattice_viterbi(VecArray *a, uint32_t fn) {
    if (fn) a->cost_fn = fn;
    int32_t ret = -1;
    GPTR(IntVec) const *items = GP(GPTR(IntVec), a->items);
#define ITEM(t) GP(IntVec, items[t])
    int32_t n0 = ITEM(0)->count, mx = n0 ? n0 : 1, total = mx;
    for (int32_t t = 1; t < a->count; t++) {
        int32_t c = ITEM(t)->count ? ITEM(t)->count : 1;
        if (c > mx) mx = c;
        total += c;
    }
    float *score = vc_calloc((size_t)(uint32_t)total, 4);
    int32_t *back = vc_calloc((size_t)(uint32_t)total, 4);
    GPTR(float) *S = vc_malloc((size_t)(uint32_t)a->count * sizeof(GPTR(float)));
    GPTR(int32_t) *B = vc_malloc((size_t)(uint32_t)a->count * sizeof(GPTR(int32_t)));
    if (!S || !score || !B || !back) goto done;
    GPSET(S[0], score);
    GPSET(B[0], back);
    int32_t n = n0, off = n0 ? n0 : 1, best = 0;
    for (int32_t t = 1; t < a->count; t++) {
        int32_t prevn = n;
        const IntVec *cur = ITEM(t);
        n = cur->count;
        GPSET(S[t], score + off);
        GPSET(B[t], back + off);
        off += n ? n : 1;
        for (int32_t j = 0; j < n; j++) {
            for (int32_t k = 0; k < prevn; k++) {
                const IntVec *prev = ITEM(t - 1);
                if (GP(int32_t, prev->data)[k] == 0) continue;
                double c = cost(a->cost_fn, GP(int32_t, prev->data)[k], GP(int32_t, ITEM(t)->data)[j]);
                double v = c + GP(float, S[t - 1])[k];
                float *s = GP(float, S[t]) + j;
                if (!x87_jae(v, *s)) {
                    *s = (float)v;
                    GP(int32_t, B[t])[j] = k;
                }
            }
            if (!x87_je(0.0, a->beam) && !x87_jbe(GP(float, S[t])[j], a->beam))
                GP(int32_t, ITEM(t)->data)[j] = 0;
        }
    }
    {
        const float *last = GP(float, S[a->count - 1]);
        double st = last[0];
        for (int32_t i = 1; i < n; i++)
            if (!x87_jbe(st, last[i])) { best = i; st = last[i]; }
    }
    int32_t *res = vc_calloc((size_t)(uint32_t)a->count, 4);
    GPSET(a->result, res);
    if (!res) goto done;
    {
        int32_t c = a->count;
        const IntVec *lastv = ITEM(c - 1);
        res[c - 1] = best >= lastv->count ? 0 : GP(int32_t, lastv->data)[best];
        int32_t pick = best;
        for (int32_t t = c - 2; t >= 0; t--) {
            if (ITEM(t + 1)->count != 0) {
                pick = GP(int32_t, B[t + 1])[pick];
                best = pick;
            } else {
                int32_t m = ITEM(t)->count;
                const float *sc = GP(float, S[t]);
                double st = sc[0];
                if (m > 1) {
                    for (int32_t i = 1; i < m; i++)
                        if (!x87_jbe(st, sc[i])) { best = i; st = sc[i]; }
                    pick = best;
                }
            }
            const IntVec *v = ITEM(t);
            res[t] = pick >= v->count ? 0 : GP(int32_t, v->data)[pick];
        }
    }
    ret = 0;
done:
    if (S) vc_free(S);
    if (score) vc_free(score);
    if (B) vc_free(B);
    if (back) vc_free(back);
    return ret;
#undef ITEM
}

// ------------------------------------------------------------------ building the tables (voice loading)

void *calloc_n(uint32_t n, uint32_t size) { return n ? vc_calloc(n, size) : NULL; }

char *str_dup0(const char *s) {
    char *p = calloc_n(1, (uint32_t)strlen(s) + 1);
    strcpy(p, s);
    return p;
}

int32_t next_prime(int32_t n) {
    if (n < 2) n = 2;
    for (;;) {
        int32_t r = x87_ftol(sqrt((double)n)), d = 2;
        if (r >= 2)
            while (n % d != 0 && ++d <= r) {}
        if (d > r) return n;
        n++;
    }
}

HashTable *hash_new(int32_t n) {
    HashTable *t = calloc_n(1, sizeof(HashTable));
    t->size = (uint32_t)next_prime(n * 2);
    t->count = 0;
    GPSET(t->entries, (HashEntry *)calloc_n(t->size, sizeof(HashEntry)));
    return t;
}

int hash_add(HashTable *t, const char *key, int32_t value) {
    if (!t || !key) return -1;
    int32_t slot;
    if (hash_lookup(t, key, &slot)) {
        GPSET(GP(HashEntry, t->entries)[slot].key, key);
        GP(HashEntry, t->entries)[slot].value = value;
        t->count++;
        return 0;
    }
    return slot == value ? 0 : -1;
}

int hash_add_grow(HashTable *t, const char *key, int32_t value) {
    if (!t || !key) return 0;
    uint32_t old = t->size;
    if ((uint32_t)(t->count * 2) >= old) {      // (a signed compare in the original; sizes are small)
        HashEntry *e = GP(HashEntry, t->entries);
        t->size = (uint32_t)next_prime(t->count * 3 + 2);
        t->count = 0;
        GPSET(t->entries, (HashEntry *)calloc_n(t->size, sizeof(HashEntry)));
        for (uint32_t i = 0; i < old; i++)
            if (e[i].key) hash_add(t, GP(const char, e[i].key), e[i].value);
        vc_free(e);
        t->regrows++;
        t->moved += t->count;
    }
    hash_add(t, key, value);
    return 0;
}

NamedTable *phoneset_new(int32_t n) {
    NamedTable *ps = calloc_n(1, sizeof(NamedTable));
    GPSET(ps->names, hash_new(n));
    GPSET(ps->items, (GPTR(void) *)calloc_n((uint32_t)n, sizeof(GPTR(void))));
    GPSET(ps->base, (uint8_t *)calloc_n((uint32_t)n, 1));
    GPSET(ps->left, (uint8_t *)calloc_n((uint32_t)n, 1));
    GPSET(ps->right, (uint8_t *)calloc_n((uint32_t)n, 1));
    GPSET(ps->ctx, (uint8_t *)calloc_n((uint32_t)n, 1));
    GPSET(ps->nparts, (uint8_t *)calloc_n((uint32_t)n, 1));
    GPSET(ps->kind, (int32_t *)calloc_n((uint32_t)n, 4));
    return ps;
}

void phoneset_set(NamedTable *ps, const char *name, int32_t idx, int32_t base, int32_t kind, int32_t nparts) {
    char *dup = str_dup0(name);
    hash_add_grow(GP(HashTable, ps->names), dup, idx);
    GPSET(GP(GPTR(void), ps->items)[idx], (void *)dup);
    GP(uint8_t, ps->base)[idx] = (uint8_t)base;
    GP(uint8_t, ps->nparts)[idx] = (uint8_t)nparts;
    GP(int32_t, ps->kind)[idx] = kind;
}

int32_t phoneset_kind(const NamedTable *ps, int32_t i) { return GP(int32_t, ps->kind)[i]; }
int32_t phoneset_nparts(const NamedTable *ps, int32_t i) { return GP(uint8_t, ps->nparts)[i]; }

int32_t phone_spec_split(const char *s, char *a, char *b, char *c, char *d) {
    *a = *b = *c = *d = 0;
    while (*s != '(' && *s) *a++ = *s++;
    *a = 0;
    if (!*s) return 1;
    s++;
    while (*s != ',' && *s) *b++ = *s++;
    *b = 0;
    if (!*s) return 2;
    s++;
    while (*s != ')' && *s) *c++ = *s++;
    *c = 0;
    if (!*s) return 3;
    s++;
    while (*s) *d++ = *s++;
    *d = 0;
    return 4;
}

// sscanf(p, "%s%d%d%d%d\n", ...) as the emulator's MSVCRT does it (the file is well formed)
static void scan_phone_line(const char *p, char *tok, int32_t *v[4]) {
    while (vc_isspace((uint8_t)*p)) p++;
    int n = 0;
    while (p[n] && !vc_isspace((uint8_t)p[n]) && n < 1023) {
        tok[n] = p[n];
        n++;
    }
    tok[n] = 0;
    p += n;
    for (int k = 0; k < 4; k++) {
        while (vc_isspace((uint8_t)*p)) p++;
        char buf[64];
        int m = 0;
        while (m < 63 && ((p[m] >= '0' && p[m] <= '9') || (m == 0 && (p[m] == '-' || p[m] == '+')))) {
            buf[m] = p[m];
            m++;
        }
        buf[m] = 0;
        if (!m) return;
        *v[k] = (int32_t)strtol(buf, NULL, 10);
        p += m;
    }
}

int32_t phoneset_load(void *stg, const char *name, GPTR(NamedTable) *out) {
    size_t n = strlen(name) + 1;
    uint16_t *w = vc_malloc(2 * n);
    if (!w) return (int32_t)0x8007000eu;
    vc_MultiByteToWideChar_cp1252((const uint8_t *)name, (int)n, w);     // (with its NUL, as -1 converts)
    void *stm;
    int32_t hr = vc_stg_open_stream(stg, w, &stm);
    vc_free(w);
    if (hr < 0) return hr;
    uint32_t size;
    hr = vc_stm_size(stm, &size);
    if (hr < 0) {
        vc_com_release(stm);
        return hr;
    }
    char *buf = vc_malloc(size + 1);
    if (!buf) {
        vc_com_release(stm);
        return (int32_t)0x8007000eu;
    }
    vc_stm_read(stm, buf, size + 1, NULL);
    buf[size] = 0;
    vc_com_release(stm);
    int32_t idx = 0, base = 0, kind = 0, parts = 0, lines = 0;
    for (const char *q = buf; *q; q++)
        if (*q == '\r') lines++;
    NamedTable *ps = phoneset_new(lines);
    GPSET(*out, ps);
    GPSET(ps->name, name);
    char tok[0xa0];
    for (char *q = buf; *q;) {
        int32_t *v[4] = { &kind, &parts, &base, &idx };
        scan_phone_line(q, tok, v);
        while (*q != '\r') q++;
        q += 2;
        if (kind == 0) ps->nzero++;
        int32_t np = 1;
        if (kind == -2) {
            ps->nsplit++;
            np = parts;
        }
        phoneset_set(ps, tok, idx, base, kind, np);
    }
    vc_free(buf);
    int32_t n2 = idx++;
    for (int32_t k = 0; k < n2; k++) {
        if (phoneset_kind(ps, k) != -2) continue;
        int32_t m = phoneset_nparts(ps, k);
        for (int32_t j = 1; j < m; j++) {
            char nm[0x100];
            sprintf(nm, DLLVAR(const char, 0x6371efe4), GP(const char, named_item(ps, k)), j);    // "%s(%d)"
            phoneset_set(ps, nm, idx, k, j + 1000, 1);
            idx++;
        }
    }
    ps->count = idx;
    for (int32_t i = 0; i < ps->count; i++) {
        if (i == GP(uint8_t, ps->base)[i]) {
            GP(uint8_t, ps->left)[i] = 0xff;
            GP(uint8_t, ps->right)[i] = 0xff;
            GP(uint8_t, ps->ctx)[i] = 0;
        } else {
            char a[0x80], b[0x20], c[0x20], d[0x20];
            phone_spec_split(GP(const char, GP(GPTR(void), ps->items)[i]), a, b, c, d);
            GP(uint8_t, ps->left)[i] = (uint8_t)named_index(ps, b);
            GP(uint8_t, ps->right)[i] = (uint8_t)named_index(ps, c);
            GP(uint8_t, ps->ctx)[i] = (uint8_t)d[0];
        }
    }
    return 0;
}
