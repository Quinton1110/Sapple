/* zf2_units.c - CTTSUnitGenerator::Process (0x180034b00) outputs that are not the tree itself:
 *   - the per-phone prosody block (phone+0x78, FUN_18005f190): SAPI volume / rate / pitch, pitch range, pitch
 *     factor, group duration, pause duration; <emph> words use the [Emphasis] defaults (rate 0.8, volume 1.2,
 *     pitch 1.2, FUN_18009ceb0);
 *   - the SAPI events (CSAPIEventHandler, FUN_18009db84): PHONEME / VISEME per phone (FUN_180033f34, phone
 *     converter table below), WORD_BOUNDARY (FUN_18003358c), TTS_BOOKMARK (FUN_18014b4b4), SENTENCE_BOUNDARY
 *     (FUN_18009dbe4).  Durations and audio offsets are added by the backend. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "zf2_int.h"

/* ---- prosody block ---- */
/* FUN_180074908: round half away from zero (x <= 0 and not within 1e-6 of 0 -> x - 0.5, else x + 0.5), truncate */
static int rnd(float x)
{
    int pos = !(x <= 0.0f) || fabs((double)(x - 0.0f)) <= 9.999999974752427e-07;
    return (int)(pos ? x + 0.5f : x - 0.5f);
}
/* FUN_18009cb4c(flag, v, 0, -10, 10): relative values (flag) are 20 * (v - 1); clamp to [-10, 10] */
static float lin(int flag, float v)
{
    const float lo = -10.0f, hi = 10.0f;
    float f = lo;
    if (flag) v = (hi - lo) * (v - 1.0f) + 0.0f;
    if (lo <= v) f = v;
    if (hi <= f) f = hi;
    return f;
}
/* FUN_18009cbe0: volume (flag: fraction of 100) clamped to [0, 150] */
static int vol(int flag, float v)
{
    float f = v, g = 0.0f;
    if (flag) f = f * 100.0f;
    if (0.0f <= f) g = f;
    if (150.0f <= g) g = 150.0f;
    return rnd(g);
}

/* ctl[7] = {volume, rate, pitch, f32 range, f32 pitch factor, group ms, pause ms} of word wi */
static void word_ctl(const Z2Sent *s, int wi, int32_t ctl[7])
{
    const Z2W *w = &s->w[wi];
    const zf_fragstate *fs = w->frag >= 0 ? &s->frag[w->frag] : NULL;
    const int pitch_pct = 100; /* CTTSUnitGenerator+0x74 (VoiceSetting pitch, 100) */
    float range = 0.0f, pf = 0.0f;
    memset(ctl, 0, sizeof(int32_t) * 7);
    ctl[0] = 100;
    if (fs) {
        if (fs->has_prosody) {
            int p;
            ctl[1] = rnd(lin(0, fs->rate));
            ctl[0] = vol(0, fs->volume);
            p = rnd(lin(0, fs->pitch_middle));
            ctl[2] = p;
            range = 1.0f; /* FUN_18009cb90: SAPI never sets a range (0 -> 100%) */
            if (p < -10) p = -10;
            if (p > 10) p = 10;
            pf = powf(2.0f, (float)((double)p / 24.0));
            pf = (pf * (float)pitch_pct) / 100.0f;
            /* +0x30 of the prosody block (group duration) is 0 for SAPI input */
            if (pf == 1.0f) pf = 0.0f;
        } else if (pitch_pct != 100)
            pf = (float)pitch_pct / 100.0f;
        if (fs->emph == 1 || fs->emph == 2) { /* [Emphasis] parameter tuning (enabled by default) */
            ctl[1] = rnd(lin(1, 0.8f));
            ctl[0] = vol(1, 1.2f);
            ctl[2] = rnd(lin(1, 1.2f));
        }
    }
    memcpy(&ctl[3], &range, 4);
    memcpy(&ctl[4], &pf, 4);
    if (z2_is_break(w)) ctl[6] = w->i210;
}

void z2_fill_ctl(const Z2Sent *s, ZbUtt *u)
{
    int wi, k, i;
    for (wi = 0; wi < s->nw; wi++) {
        int32_t ctl[7];
        const Z2W *w = &s->w[wi];
        if (!w->ns) continue;
        word_ctl(s, wi, ctl);
        for (k = 0; k < w->ns; k++) {
            const Z2S *y = &s->s[w->s0 + k];
            for (i = y->p0; i < y->p0 + y->np; i++) {
                ZbPhoneCtl *c = &u->ph[i].c;
                c->volume = ctl[0];
                c->rate = ctl[1];
                c->pitch = ctl[2];
                memcpy(&c->pitch_range, &ctl[3], 4);
                c->emph = ctl[4];
                c->group_ms = ctl[5];
                c->phone_ms = ctl[6];
            }
        }
    }
}

/* ---- SAPI events ---- */
/* phone converter of the en-US event handler (unit generator +0xc0 -> +8, dumped at runtime):
 * {internal phone id, viseme bytes (the first always, then up to 7 while non-zero), SAPI phone ids (0-terminated)} */
static const struct { uint8_t id; uint8_t vis[8]; uint16_t sapi[2]; } PHCONV[] = {
    {1, {0}, {3}}, {2, {0}, {1}}, {3, {0}, {7}}, {4, {0}, {7}}, {5, {0}, {8}}, {6, {0}, {9}},
    {7, {2}, {10}}, {8, {1}, {11}}, {9, {1}, {12}}, {10, {3}, {13}}, {11, {2, 4}, {14}}, {12, {1}, {15}},
    {13, {2, 6}, {16}}, {14, {21}, {17}}, {15, {19, 16}, {18}}, {16, {19}, {19}}, {17, {17}, {20}}, {18, {4}, {21}},
    {19, {5}, {22}}, {20, {4, 6}, {23}}, {21, {18}, {24}}, {22, {20}, {25}}, {23, {12}, {26}}, {24, {6}, {27}},
    {26, {6}, {28}}, {27, {19, 16}, {29}}, {28, {20}, {30}}, {29, {14}, {31}}, {30, {21}, {32}}, {31, {19}, {33}},
    {32, {20}, {34}}, {33, {8}, {35}}, {34, {3, 6}, {36}}, {35, {21}, {37}}, {36, {13}, {38}}, {37, {15}, {39}},
    {38, {16}, {40}}, {39, {19}, {41}}, {40, {19}, {42}}, {41, {4}, {43}}, {42, {7}, {44}}, {43, {18}, {45}},
    {44, {7}, {46}}, {45, {6}, {47}}, {46, {15}, {48}}, {47, {16}, {49}}};
#define SAPI_SIL 7 /* default "next phone" for LANGID 0x409 */

typedef struct { int n, cap; zf2_event *e; } EvList;
static void evpush(EvList *l, const zf2_event *e)
{
    if (l->n >= l->cap) {
        l->cap = l->cap ? 2 * l->cap : 16;
        l->e = (zf2_event *)realloc(l->e, sizeof(zf2_event) * (size_t)l->cap);
    }
    l->e[l->n++] = *e;
}
static void evfront(EvList *l, const zf2_event *e)
{
    zf2_event t = *e;
    evpush(l, e);
    memmove(&l->e[1], &l->e[0], sizeof(zf2_event) * (size_t)(l->n - 1));
    l->e[0] = t;
}

static int word_first_phone(const Z2Sent *s, int wi)
{
    const Z2W *w = &s->w[wi];
    return w->ns ? s->s[w->s0].p0 : -1;
}
static int word_last_phone(const Z2Sent *s, int wi)
{
    const Z2W *w = &s->w[wi];
    const Z2S *y;
    if (!w->ns) return -1;
    y = &s->s[w->s0 + w->ns - 1];
    return y->p0 + y->np - 1;
}
/* _wtol */
static long wtol_z(const zf_char *p)
{
    long v = 0;
    int neg = 0;
    if (!p) return 0;
    while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return neg ? -v : v;
}

int zf2_make_events(const zf2_sent *s, zf2_events *out)
{
    EvList *ph;
    zf2_event e;
    int i, k, wi, prevph = -1, prevvis = -1, first;
    const int NCONV = (int)(sizeof PHCONV / sizeof PHCONV[0]);
    if (!s->np) return 0;
    ph = (EvList *)calloc((size_t)s->np, sizeof(EvList));
    /* FUN_180033f34: phonemes + visemes per phone */
    for (i = 0; i < s->np; i++) {
        int t;
        for (t = 0; t < NCONV && PHCONV[t].id != s->p[i].id; t++);
        if (t == NCONV) continue;
        for (k = 0; k < 2 && PHCONV[t].sapi[k]; k++) {
            memset(&e, 0, sizeof e);
            e.id = ZF2_EV_PHONEME;
            e.phone = i;
            e.wparam = SAPI_SIL;
            e.lparam = PHCONV[t].sapi[k];
            evpush(&ph[i], &e);
        }
        {
            int nv = 1;
            while (nv < 7 && PHCONV[t].vis[nv] != 0) nv++;
            for (k = 0; k < nv; k++) {
                memset(&e, 0, sizeof e);
                e.id = ZF2_EV_VISEME;
                e.phone = i;
                e.lparam = PHCONV[t].vis[k];
                e.sub = k;
                e.nsub = nv;
                evpush(&ph[i], &e);
            }
        }
    }
    if (!z2_phone_is_sil(s, s->p[0].id)) {
        memset(&e, 0, sizeof e);
        e.phone = 0;
        e.id = ZF2_EV_VISEME;
        evfront(&ph[0], &e);
        e.id = ZF2_EV_PHONEME;
        e.wparam = SAPI_SIL;
        e.lparam = SAPI_SIL;
        evfront(&ph[0], &e);
    }
    /* "next" links: wParam of each phoneme / viseme event = lParam of the following one of the same kind */
    for (i = 0; i < s->np; i++)
        for (k = 0; k < ph[i].n; k++) {
            zf2_event *x = &ph[i].e[k];
            if (x->id == ZF2_EV_PHONEME) {
                if (prevph >= 0) ph[prevph >> 8].e[prevph & 255].wparam = x->lparam;
                prevph = i << 8 | k;
            } else if (x->id == ZF2_EV_VISEME) {
                if (prevvis >= 0) ph[prevvis >> 8].e[prevvis & 255].wparam = x->lparam;
                prevvis = i << 8 | k;
            }
        }
    /* FUN_18003358c: bookmarks (FUN_18014b4b4) and word boundaries */
    for (wi = 0; wi < s->nw; wi++) {
        const Z2W *w = &s->w[wi];
        if (w->bookmark) {
            int pw, at = -1;
            memset(&e, 0, sizeof e);
            e.id = ZF2_EV_BOOKMARK;
            e.wparam = wtol_z(w->bookmark_name);
            e.str = w->bookmark_name;
            for (pw = wi - 1; pw >= 0; pw--)
                if ((at = word_last_phone(s, pw)) >= 0) break;
            if (at >= 0) { e.phone = at; evpush(&ph[at], &e); }
            else { e.phone = 0; evfront(&ph[0], &e); }
        }
        if (!w->pron_null && !z2_is_break(w) && (first = word_first_phone(s, wi)) >= 0) {
            memset(&e, 0, sizeof e);
            e.id = ZF2_EV_WORD;
            e.phone = first;
            e.wparam = w->src_len;
            e.lparam = w->src_off;
            evfront(&ph[first], &e);
        }
    }
    /* FUN_18009dbe4: sentence boundary at the first phone */
    memset(&e, 0, sizeof e);
    e.id = ZF2_EV_SENTENCE;
    e.phone = 0;
    e.wparam = s->src_len;
    e.lparam = s->src_off;
    evfront(&ph[0], &e);
    for (i = 0; i < s->np; i++) {
        for (k = 0; k < ph[i].n; k++) {
            if (out->n >= out->cap) {
                out->cap = out->cap ? 2 * out->cap : 64;
                out->ev = (zf2_event *)realloc(out->ev, sizeof(zf2_event) * (size_t)out->cap);
            }
            out->ev[out->n++] = ph[i].e[k];
        }
        free(ph[i].e);
    }
    free(ph);
    return 0;
}

void zf2_events_free(zf2_events *ev)
{
    free(ev->ev);
    ev->ev = NULL;
    ev->n = ev->cap = 0;
}
