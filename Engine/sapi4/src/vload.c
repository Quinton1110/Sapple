// Voice loading for the unit stage: the voice file's streams (VcHeader, PhoneFile, TreeImage, Senone,
// AltUnits) into the unit stage's tables.
#include "unitsel.h"
#include "voice.h"
#include "fe_input.h"
#include "crt_vc.h"
#include "vcrt.h"
#include "x87.h"
#include <math.h>
#include <string.h>

void alt_arg(int32_t flag, const char **pp, uint8_t *count, GPTR(const int16_t) *ptr) {
    if (!flag) {
        *count = 0;
        *ptr = 0;
        return;
    }
    *count = (uint8_t)**pp;
    (*pp)++;
    GPSET(*ptr, (const int16_t *)(const void *)*pp);
    *pp += *count * 2;
}

int32_t alt_parse(const char *buf, int32_t n, GPTR(AltBucket) *out) {
    int32_t off = 0;
    for (int32_t i = 0; i < n; i++) {
        AltBucket *b = vc_calloc(1, sizeof(AltBucket));
        GPSET(out[i], b);
        if (!b) goto fail;
        b->first_id = off;
        b->count = *(const int32_t *)(const void *)buf;
        off += b->count;
        buf += 4;
        uint32_t c = (uint32_t)b->count;
        if (!c) continue;
        GPSET(b->keys, (GPTR(const char) *)vc_malloc(c * sizeof(GPTR(char))));
        GPSET(b->units, (GPTR(const int16_t) *)vc_malloc(c * sizeof(GPTR(int16_t))));
        GPSET(b->tails, (GPTR(const char) *)vc_malloc(c * sizeof(GPTR(char))));
        GPSET(b->flags, (uint8_t *)vc_malloc(c));
        GPSET(b->nleft, (uint8_t *)vc_malloc(c));
        GPSET(b->nright, (uint8_t *)vc_malloc(c));
        GPSET(b->left, (GPTR(const int16_t) *)vc_malloc(c * sizeof(GPTR(int16_t))));
        GPSET(b->right, (GPTR(const int16_t) *)vc_malloc(c * sizeof(GPTR(int16_t))));
        GPSET(b->nunits, (int16_t *)vc_malloc(c * 2));
        if (!b->keys || !b->units || !b->flags || !b->nleft || !b->nright || !b->left || !b->right || !b->nunits)
            goto fail;
        for (uint32_t j = 0; j < c; j++) {
            GPSET(GP(GPTR(const char), b->keys)[j], buf);
            buf = strchr(buf, 0) + 1;
            uint8_t f = (uint8_t)*buf;
            ((uint8_t *)GP(const uint8_t, b->flags))[j] = f;
            buf++;
            alt_arg(f & 4, &buf, (uint8_t *)GP(const uint8_t, b->nleft) + j, GP(GPTR(const int16_t), b->left) + j);
            alt_arg(f & 8, &buf, (uint8_t *)GP(const uint8_t, b->nright) + j, GP(GPTR(const int16_t), b->right) + j);
            ((int16_t *)GP(const int16_t, b->nunits))[j] = *(const int16_t *)(const void *)buf;
            buf += 2;
            GPSET(GP(GPTR(const int16_t), b->units)[j], (const int16_t *)(const void *)buf);
            buf += GP(const int16_t, b->nunits)[j] * 2;
            GPSET(GP(GPTR(const char), b->tails)[j], buf);
            buf = strchr(buf, 0) + 1;
        }
    }
    return 0;
fail:
    // (the original frees the rule arrays and, once per bucket, the bucket array it was given)
    for (int32_t i = 0; i < n && out; i++) {
        AltBucket *b = GP(AltBucket, out[i]);
        if (b) {
            if (b->keys) vc_free(GP(void, b->keys));
            if (b->units) vc_free(GP(void, b->units));
            if (b->flags) vc_free((void *)GP(const uint8_t, b->flags));
            if (b->nleft) vc_free((void *)GP(const uint8_t, b->nleft));
            if (b->nright) vc_free((void *)GP(const uint8_t, b->nright));
            if (b->left) vc_free(GP(void, b->left));
            if (b->right) vc_free(GP(void, b->right));
            if (b->nunits) vc_free((void *)GP(const int16_t, b->nunits));
        }
    }
    return -1;
}

void alt_units_load(void *stg, const uint16_t *name, int32_t n, GPTR(AltRules) *out) {
    *out = 0;
    void *stm = NULL;
    vc_stg_open_stream(stg, name, &stm);
    if (!stm) return;
    uint32_t size = 0;
    vc_stm_read(stm, &size, 4, NULL);
    char *buf = vc_malloc(size);
    if (!buf) return;
    vc_stm_read(stm, buf, size, NULL);
    AltRules *r = vc_calloc((size_t)(uint32_t)n, sizeof(GPTR(AltBucket)));
    GPSET(*out, r);
    if (alt_parse(buf, n, (GPTR(AltBucket) *)(void *)r)) *out = 0;    // (the original also frees out itself)
    vc_com_release(stm);
}

void unit_voice_scan(UnitStage *u, int32_t voice, int32_t variant) {
    (void)u;
    const ModeTable *mt = GP(const ModeTable, GP(GPTR(const ModeTable), *DLLPTR(GPTR(const ModeTable), 0x63738bc0))[voice]);
    const uint8_t *d = GP(const uint8_t, GP(GPTR(const uint8_t), mt->modes)[variant]);
    uint32_t n = voice_rec_count(d);
    for (uint32_t i = 0; i < n; i++) {
        int32_t key, len;
        voice_rec(d, i, &key, &len);
    }
}

int32_t unit_init(UnitStage *u, uint8_t *qin, uint8_t *qout, void *stg, const uint8_t *voice, int32_t voice_idx,
                  int32_t variant, uint32_t hwnd, uint32_t msg) {
    GPSET(u->in, (struct Queue *)(void *)qin);
    GPSET(u->out, (struct Queue *)(void *)qout);
    uint32_t rate = *(const uint16_t *)(const void *)(voice + 0x8d6);
    u->rate = rate;
    u->default_rate = rate;
    u->fast_rate = (uint32_t)x87_ftol((double)rate * (double)0.6666666865348816f);
    unit_rate_update(u);
    u->notify_hwnd = hwnd;
    u->notify_msg = msg;
    void *stm = NULL;
    vc_stg_open_stream(stg, DLLVAR(const uint16_t, 0x6371ef30), &stm);      // L"VcHeader"
    u->amp = 0xffff;
    uint8_t *hdr = vc_malloc(0x8f8);
    uint32_t got;
    vc_stm_read(stm, hdr, 0x8f8, &got);
    u->tree_mode = 1;
    if (memcmp(hdr, DLLVAR(const uint8_t, 0x6369f168), 16) == 0) u->tree_mode = 2;
    vc_free(hdr);
    vc_com_release(stm);
    GPTR(const NamedTable) *pn = &u->phone_names;
    phoneset_load(stg, DLLVAR(const char, 0x63721b9c), (GPTR(NamedTable) *)pn);     // "PhoneFile"
    const NamedTable *ps = GP(const NamedTable, u->phone_names);
    u->senone_base = ps->nzero;
    u->sil_phone = named_index(ps, DLLVAR(const char, 0x6371c010));      // "SIL"
    GPTR(SenoneTree) *pt = (GPTR(SenoneTree) *)&u->tree;
    if (u->tree_mode == 1) senone_tree_load(stg, DLLVAR(const char, 0x63721b90), ps, pt);    // "TreeImage"
    else senone_tree_load_old(stg, DLLVAR(const char, 0x63721b90), ps, pt);
    for (int32_t i = 0; i < u->senone_base; i++) {
        const int32_t *units = (const int32_t *)(const void *)GP(const uint16_t, GP(const SenoneTree, u->tree)->phone_units);
        int32_t c = units[i + 1] - units[i];
        if (c > 0) GPSET(u->stats[i], (SenStat *)vc_malloc((size_t)(uint32_t)c << 4));
        else u->stats[i] = 0;
    }
    vc_stg_open_stream(stg, DLLVAR(const uint16_t, 0x63721b80), &stm);     // L"Senone"
    int32_t nrec = 0;
    vc_stm_read(stm, &nrec, 4, &got);
    for (int32_t k = 0; k < nrec; k++) {
        SenoneRec r;
        vc_stm_read(stm, &r, 0x1c, &got);
        int32_t p = named_index(GP(const NamedTable, u->phone_names), r.name);
        SenStat *st = (SenStat *)GP(const SenStat, u->stats[p]) + (r.idx - 1);
        st->_8 = r.c;
        memcpy(&st->scale, &r.a, 4);
        memcpy(&st->_4, &r.b, 4);
        st->var = (float)exp((double)st->_8 - (double)r.d);
    }
    vc_com_release(stm);
    GPTR(AltRules) *pl = (GPTR(AltRules) *)&u->lattice;
    alt_units_load(stg, DLLVAR(const uint16_t, 0x63721b6c), u->senone_base, pl);     // L"AltUnits"
    if (variant < 0) unit_voice_scan(u, voice_idx, -variant);
    return 1;
}

UnitStage *unit_ctor(UnitStage *u) {
    u->out = 0;
    u->in = 0;
    u->_000 = 0;
    u->lattice = 0;
    u->phone_names = 0;
    u->tree = 0;
    u->rate_mode = 0;
    memset(u->cs, 0, sizeof u->cs);
    memset(u->stats, 0, sizeof u->stats);
    vc_InitializeCriticalSection(u->cs);
    u->ev_stop = vc_CreateEventA(1, 0);
    u->ev_done = vc_CreateEventA(1, 0);
    u->cap = 10;
    GPSET(u->recs, (UnitRec *)vc_malloc(0x2f8));
    GPSET(u->ids, (int16_t *)vc_malloc((size_t)(uint32_t)u->cap * 2));
    GPSET(u->senones, (int16_t *)vc_malloc((size_t)(uint32_t)u->cap * 2));
    return u;
}
