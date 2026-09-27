// Voice loading for the synthesizer: the voice file's Inventory stream (codebooks, unit blobs, the
// derived FFT and noise tables), the synthesizer's set-up and the mode characters.
#include "voice.h"
#include "effect.h"
#include "fe_input.h"
#include "crt_vc.h"
#include "x87.h"
#include <math.h>
#include <string.h>

LAYOUT(VocVoice, _40, 0x40);
LAYOUT(ModeTable, inv, 0x20);
#ifdef DECOMP_HOOK
_Static_assert(sizeof(VocVoice) == 0x44, "VocVoice");
#endif

float voc_gauss(int32_t *have, float *a, float *b) {
    if (*have == 1) {
        *have = 0;
        return *b;
    }
    double u1 = (double)vc_rand() * 3.0517578125e-05;
    double u2 = (double)vc_rand() * 3.0517578125e-05;
    double r = sqrt(log(1.0 - u1) * -2.0);
    double t = u2 * 6.283185307179586;
    *a = (float)(cos(t) * r);
    *b = (float)(sin(t) * r);
    *have = 1;
    return *a;
}

int8_t *voc_noise_make(int32_t n) {
    int32_t have = 0;
    float a, b;
    int8_t *p = vc_malloc((size_t)(uint32_t)n);
    if (!p) return NULL;
    for (int32_t i = 0; i < n; i++) {
        double v = (double)voc_gauss(&have, &a, &b) * 50.0;
        v = x87_jbe(v, 0.0) ? v - 0.5 : v + 0.5;
        p[i] = (int8_t)x87_ftol(v);
    }
    return p;
}

void voc_sine_table(int32_t n, float *out) {
    double step = 3.141592653589793 / (double)n;
    int32_t half = n / 2;
    out[0] = 0.0f;
    out[n] = 0.0f;
    for (int32_t i = 1; i < half; i++) {
        double s = sin((double)i * step);
        out[i] = (float)s;
        out[n - i] = (float)s;
        out[n + i] = (float)-s;
        out[2 * n - i] = (float)-s;
    }
    out[half] = 1.0f;
    out[(3 * n) / 2] = -1.0f;
}

float *voc_fade_window(int32_t n) {
    float *w = vc_malloc((size_t)(uint32_t)n * 4);
    if (!w) return NULL;
    double step = 3.141592653589793 / (double)n;
    for (int32_t i = 0; i < n; i++) w[i] = (float)((cos((double)i * step) + 1.0) * 0.5);
    return w;
}

VocVoice *voc_header_load(void *stm) {
    VocVoice *v = vc_malloc(sizeof(VocVoice));
    if (!v) return NULL;
    memset(v, 0, sizeof(VocVoice));
    uint32_t got;
    vc_stm_read(stm, &v->noise_len, 4, &got);
    GPSET(v->noise, voc_noise_make(v->noise_len));
    v->_40 = 0;
    vc_stm_read(stm, &v->order, 4, &got);
    vc_stm_read(stm, &v->n_lsf, 4, &got);
    vc_stm_read(stm, &v->n_exc, 4, &got);
    vc_stm_read(stm, &v->n_dexc, 4, &got);
    vc_stm_read(stm, &v->fft_n, 4, &got);
    vc_stm_read(stm, &v->fft_log2, 4, &got);
    float *sine = vc_malloc((size_t)(uint32_t)v->fft_n * 8);
    GPSET(v->sine, sine);
    if (!sine) {
        vc_free(v);
        return v;       // (the original returns the freed block)
    }
    voc_sine_table(v->fft_n, sine);
    GPSET(v->window, voc_fade_window(v->fft_n));
    return v;
}

Codebook *voc_codebook_load(void *stm) {
    Codebook *cb = vc_malloc(sizeof(Codebook));
    if (!cb) return NULL;
    uint32_t got;
    vc_stm_read(stm, &cb->count, 4, &got);
    vc_stm_read(stm, &cb->dim, 4, &got);
    uint32_t bytes = (uint32_t)(cb->count * cb->dim) * 4;
    float *d = vc_malloc(bytes);
    GPSET(cb->data, d);
    if (!d) return NULL;
    vc_stm_read(stm, d, bytes, &got);
    return cb;
}

static int load_books(void *stm, int32_t n, GPTR(GPTR(const Codebook)) *out) {
    if (!n) return 1;
    GPTR(const Codebook) *a = vc_malloc((size_t)(uint32_t)n * sizeof(GPTR(const Codebook)));
    GPSET(*out, a);
    for (int32_t i = 0; i < n; i++) {
        GPSET(a[i], voc_codebook_load(stm));
        if (!a[i]) return 0;
    }
    return 1;
}

VocVoice *voc_inventory_load(void *stm) {
    VocVoice *v = voc_header_load(stm);
    if (!v) return NULL;
    v->_00 = 0;
    if (!load_books(stm, v->n_lsf, &v->lsf)) return NULL;
    if (!load_books(stm, v->n_exc, &v->exc)) return NULL;
    if (!load_books(stm, v->n_dexc, &v->dexc)) return NULL;
    uint32_t got, size = 0;
    vc_stm_read(stm, &v->_20, 4, &got);
    vc_stm_read(stm, &size, 4, &got);
    uint8_t *blob = vc_malloc(size);
    if (!blob) return NULL;
    vc_stm_read(stm, blob, size, &got);
    // count + 1 offsets, relative to the end of the offset table, become pointers
    uint8_t *base = blob + (uint32_t)v->_20 * 4 + 4;
#ifdef DECOMP_HOOK
    GPSET(v->units, (GPTR(const uint8_t) *)(void *)blob);
    for (int32_t i = 0; i <= v->_20; i++) ((uint32_t *)(void *)blob)[i] += GHOST(base);
#else
    // (a portable build keeps the offsets and makes a separate table of host pointers)
    const uint8_t **u = vc_malloc(((size_t)(uint32_t)v->_20 + 1) * sizeof(*u));
    if (!u) return NULL;
    for (int32_t i = 0; i <= v->_20; i++) {
        uint32_t off;
        memcpy(&off, blob + (size_t)i * 4, 4);
        u[i] = base + off;
    }
    v->units = u;
#endif
    return v;
}

int32_t voc_setup(VocSynthState *s, const VocVoice *v, int32_t unused) {
    GPSET(s->voice, v);
    s->rate = (float)v->noise_len;
    s->order = v->order;
    float *mem = vc_malloc((size_t)(uint32_t)s->order * 4 + 4);
    GPSET(s->mem, mem);
    float *last = vc_calloc((size_t)(uint32_t)(s->order + 1), 4);
    GPSET(s->last_lpc, last);
    if (!mem || !last) return 0;
    memset(mem, 0, (size_t)(uint32_t)s->order * 4 + 4);
    s->out = 0;
    s->cap = 0;
    s->unit = 0;
    s->flags = 0;
    s->noise_pos = 0;
    return 1;
}

static ModeTable *mode_table(int32_t mode) {
    return GP(ModeTable, GP(GPTR(ModeTable), *DLLPTR(GPTR(ModeTable), 0x63738bc0))[mode]);
}

int32_t voc_engine_init(VocEngine *v, uint8_t *qin, uint8_t *qout, int32_t mode, int32_t style, void *stg,
                        int32_t pcm16, uint32_t rate, uint32_t rate_out, int32_t no_load, uint32_t hwnd,
                        uint32_t msg) {
    GPSET(v->in, (struct Queue *)(void *)qin);
    GPSET(v->out, (struct Queue *)(void *)qout);
    v->hwnd = hwnd;
    v->pcm16 = pcm16;
    v->msg = msg;
    v->rate = rate;
    v->chunks = 0;
    v->rate_out = rate_out;
    void *stm = NULL;
    vc_stg_open_stream(stg, DLLVAR(const uint16_t, 0x6371ef30), &stm);      // L"VcHeader"
    uint8_t *hdr = vc_malloc(0x8f8);
    uint32_t got;
    vc_stm_read(stm, hdr, 0x8f8, &got);
    vc_free(hdr);
    vc_com_release(stm);
    vc_stg_open_stream(stg, DLLVAR(const uint16_t, 0x6371f594), &stm);      // L"Inventory"
    v->mode = mode;
    v->style = style;
    void *cs = DLLVAR(void, 0x63738ba8);
    vc_EnterCriticalSection(cs);
    ModeTable *mt = mode_table(mode);
    if (!mt->loaded) {
        if (no_load) return (int32_t)0x80004005;     // (the lock stays held and the stream open)
        GPSET(mode_table(mode)->inv, voc_inventory_load(stm));
        mode_table(mode)->loaded = 1;
    }
    vc_LeaveCriticalSection(cs);
    vc_com_release(stm);
    if (rate == 0x5622 && rate_out == 0x1f40) v->resample[0] = voc_resampler_new(0);
    else if (rate == 0x2b11 && rate_out == 0x1f40) v->resample[2] = voc_resampler_new(2);
    else if (rate == 0x1f40 && rate_out == 0x2b11) v->resample[3] = voc_resampler_new(3);
    else if (rate == 0x5622 && rate_out == 0x3e80) v->resample[1] = voc_resampler_new(1);
    if (!voc_setup(&v->st, GP(const VocVoice, mode_table(mode)->inv), no_load)) return (int32_t)0x80004005;
    if (style < 0) voc_init(v, -style);
    return 1;
}

void voc_init(VocEngine *v, int32_t style) {
    VocSynthState *s = &v->st;
    if (style == 0) {
        s->flags = 0;
        s->forder = 0;
        if (s->mem) {           // (sic: the filter state is freed when the LPC memory exists)
            vc_free(GP(void, s->fmem));
            s->fmem = 0;
        }
        s->fcoef = 0;
        Effect *ef = GP(Effect, s->effect);
        if (ef) ef->ncoef = 0;
        return;
    }
    const uint8_t *d = GP(const uint8_t, GP(GPTR(const uint8_t), mode_table(v->mode)->modes)[style]);
    uint32_t n = voice_rec_count(d);
    for (uint32_t i = 0; i < n; i++) {
        int32_t key, len;
        const uint8_t *rec = voice_rec(d, i, &key, &len);
#define ARG (*(const int32_t *)(const void *)rec)    // (read only by the records that have one)
        switch (key & 0xffffff) {
        case 1:
            s->flags |= 1;
            break;
        case 2:
        case 3:
            s->flags = (key & 0xffffff) == 2 ? (s->flags & ~4u) | 2 : (s->flags & ~2u) | 4;
            s->forder = ARG;
            {
                float *m = vc_realloc(GP(void, s->fmem), (size_t)(uint32_t)s->forder * 4);
                GPSET(s->fmem, m);
                GPSET(s->fcoef, (const float *)(const void *)(rec + 4));
                memset(m, 0, (size_t)(uint32_t)s->forder * 4);
            }
            break;
        case 4:
            if (!s->effect) {
                Effect *ef = vc_malloc(sizeof(Effect));
                if (ef) ef = effect_ctor(ef);
                GPSET(s->effect, ef);
                if (ef && effect_init(ef, ARG, (int)v->rate) != 0) {
                    Effect *e2 = GP(Effect, s->effect);
                    if (e2) {
                        effect_free(e2);
                        vc_free(e2);     // (the pointer is kept)
                    }
                }
            }
            s->flags |= 8;
            break;
        case 6:
            if (s->effect) effect_set_fir(GP(Effect, s->effect), ARG, (const float *)(const void *)(rec + 4));
            break;
        default:
            break;
        }
#undef ARG
    }
}

VocSynthState *voc_state_ctor(VocSynthState *s) {
    s->mem = 0;
    s->fmem = 0;
    s->fcoef = 0;
    s->voice = 0;
    s->effect = 0;
    s->last_lpc = 0;
    return s;
}

void *empty_ctor(void *self) { return self; }

LAYOUT(VocEngine, _b0, 0xb0);
LAYOUT(VocEngine, cs, 0x11c);

VocEngine *voc_engine_ctor(VocEngine *v) {
    empty_ctor(v->_1c);
    voc_state_ctor(&v->st);
    v->_b0 = -1;
    v->out = 0;
    v->in = 0;
    v->_00 = 0;
    for (int k = 0; k < 4; k++) v->resample[k] = 0;
    memset(v->cs, 0, sizeof v->cs);
    vc_InitializeCriticalSection(v->cs);
    v->ev_stop = vc_CreateEventA(1, 0);
    v->ev_done = vc_CreateEventA(1, 0);
    v->_a4 = v->_a8 = v->_ac = 0;
    v->_b4 = v->_b8 = 0;
    v->thread = vc_GetCurrentThread();
    static const int32_t prio[7][3] = {
        { -15, -1, 1 }, { -15, -1, 1 }, { -15, -1, 1 }, { -15, 0, 1 }, { -1, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 },
    };
    for (int i = 0; i < 7; i++)
        for (int k = 0; k < 3; k++) v->prio[i][k] = (uint32_t)prio[i][k];
    v->level = 3;
    return v;
}
