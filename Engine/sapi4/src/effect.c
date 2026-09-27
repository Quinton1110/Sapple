// Voice-mode effects of msttssyn.dll, decompiled (see effect.h).
#include "effect.h"
#include "crt_vc.h"
#include "x87.h"
#include <math.h>
#include <string.h>

LAYOUT(Allpass, fir, 0x1c);
LAYOUT(EffectPreset, warble, 0x14);
LAYOUT(Effect, preset, 0x28);
LAYOUT(Effect, ncoef, 0x38);

double effect_db_to_gain(Effect *self, float db) {
    (void)self;
    if (!((double)db >= -110.0)) return 0.0;   // jb: below, or unordered
    return pow(10.0, (double)db * 0.05);
}

void effect_gain_copy(Effect *self, float *dst, const float *src, uint32_t n, float g) {
    (void)self;
    if (!((double)g > 0.0)) memset(dst, 0, (size_t)n * 4);
    else if ((double)g == 1.0) memcpy(dst, src, (size_t)n * 4);   // 
    else
        for (; n; n--) *dst++ = (float)((double)g * *src++);
}

static inline double clip16(double v) {
    if (!(v >= -32768.0)) return -32768.0;
    if (v > 32767.0) return 32767.0;
    return v;
}

void effect_mix_clip(Effect *self, const float *x, float *y, uint32_t n, float g) {
    (void)self;
    if (!((double)g > 0.0)) return;
    if ((double)g == 1.0) {
        for (; n; n--, x++, y++) *y = (float)clip16((double)*y + *x);
    } else {
        for (; n; n--, x++, y++) *y = (float)clip16((double)g * *y + *x);
    }
}

void effect_allpass(Effect *self, Allpass *ap, uint32_t n, const float *in, float *out) {
    float *end = GP(float, ap->buf) + x87_ftol((double)ap->len * self->shrink);
    for (; n; n--, in++, out++) {
        float d = *GP(float, ap->rd);
        double v = (double)d * ap->gain + *in;
        float vf = (float)v;
        if (v > 0.0) {
            if (!((double)vf >= 0.001)) vf = 0.0f;
        } else {
            if ((double)vf > -0.001) vf = 0.0f;   // (NaN is kept: unordered takes the jbe)
        }
        if (self->ncoef) {
            float *h = GP(float, ap->fir);
            memmove(h + 1, h, (size_t)(self->ncoef * 4 - 4));
            h[0] = vf;
            float *w = GP(float, ap->wr);
            *w = 0.0f;
            const float *cf = GP(const float, self->coef);
            for (int k = 0; k < self->ncoef; k++) *w = (float)((double)h[k] * cf[k] + *w);
        } else {
            *GP(float, ap->wr) = vf;
        }
        *out = (float)((double)d - (double)vf * ap->gain);
        float *w = GP(float, ap->wr) + 1;
        GPSET(ap->wr, w >= end ? GP(float, ap->buf) : w);
        float *r = GP(float, ap->rd) + 1;
        GPSET(ap->rd, r >= end ? GP(float, ap->buf) : r);
    }
}

void effect_chain(Effect *self, float *buf, int n, GPTR(Allpass) *stages) {
    for (int16_t i = 0; i < 5; i++) {
        if (!stages[i]) break;
        effect_allpass(self, GP(Allpass, stages[i]), (uint32_t)n, buf, buf);
    }
}

void effect_process(Effect *self, float *buf, int n) {
    if (self->nstages) {
        self->shrink = (float)(1.0 - (double)GP(const EffectPreset, self->preset)->warble * self->rnd);
        while (n > 0) {
            int k = n < self->chunk ? n : self->chunk;
            effect_gain_copy(self, GP(float, self->scratch), buf, (uint32_t)k, self->send);
            effect_chain(self, GP(float, self->scratch), k, self->stage);
            effect_mix_clip(self, GP(float, self->scratch), buf, (uint32_t)k, self->dry);
            n -= k;
            buf += k;
        }
    }
    self->rnd = (float)((double)vc_rand() * 0.000244140625);
}

void effect_ap_clear(Effect *self, Allpass *ap) {
    (void)self;
    float *b = GP(float, ap->buf);
    for (int i = 0; i < ap->len; i++) b[i] = 0.0f;
}

int16_t effect_ap_init(Effect *self, Allpass *ap, float gain, int delay, int len) {
    ap->gain = gain;
    ap->delay = delay;
    ap->len = len;
    ap->fir = 0;
    float *b = vc_malloc((size_t)(uint32_t)(len * 4));
    GPSET(ap->buf, b);
    if (!b) return 1;
    GPSET(ap->rd, b);
    GPSET(ap->end, b + ap->len);
    GPSET(ap->wr, ap->len == ap->delay ? b : b + ap->delay);
    effect_ap_clear(self, ap);
    return 0;
}

int16_t effect_build_stages(Effect *self, int16_t n, GPTR(Allpass) *stages, const float *delays_ms,
                            const float *gains_db, float samples_per_ms) {
    int16_t r = 0;
    for (int i = 0; i < n; i++) {
        Allpass *ap = vc_malloc(sizeof(Allpass));
        GPSET(stages[i], ap);
        if (!ap) return 1;
        int d = x87_ftol((double)delays_ms[i] * samples_per_ms);
        if (d < 2) d = 2;
        double g = effect_db_to_gain(self, gains_db[i]);
        r = effect_ap_init(self, ap, (float)g, d, d);
        if (r) return r;
    }
    return r;
}

void effect_free(Effect *self) {
    for (int i = 0; i < 5; i++) {
        Allpass *ap = GP(Allpass, self->stage[i]);
        if (!self->stage[i]) continue;
        if (ap->buf) vc_free(GP(void, ap->buf));
        if (ap->fir) vc_free(GP(void, ap->fir));
        vc_free(ap);
        self->stage[i] = 0;
    }
    if (self->scratch) {
        vc_free(GP(void, self->scratch));
        self->scratch = 0;
    }
}

GPTR(const EffectPreset) effect_preset(Effect *self, int mode) {
    (void)self;
    static const uint32_t table[8] = { 0, 0x6371f0a8, 0x6371f0c8, 0x6371f018, 0x6371f088, 0x6371f050, 0x6371f0e8, 0x6371f108 };
    if (mode >= 1 && mode <= 7) {
#ifdef DECOMP_HOOK
        GPTR(const EffectPreset) p;
        GPSET(p, DLLVAR(const EffectPreset, table[mode]));
        return p;
#else
        // (the presets hold pointers: a portable build reads them field by field)
        static EffectPreset host[8];
        uint32_t a = table[mode];
        EffectPreset *p = &host[mode];
        p->send_db = *DLLVAR(const float, a);
        p->dry_db = *DLLVAR(const float, a + 4);
        p->nstages = *DLLVAR(const int16_t, a + 8);
        p->_pad = *DLLVAR(const int16_t, a + 10);
        p->delays = *DLLPTR(float, a + 0xc);
        p->gains_db = *DLLPTR(float, a + 0x10);
        p->warble = *DLLVAR(const float, a + 0x14);
        return p;
#endif
    }
#ifdef DECOMP_HOOK
    return (uint32_t)mode;
#else
    return (const EffectPreset *)(intptr_t)mode;
#endif
}

int16_t effect_init(Effect *self, int mode, int rate) {
    if (mode <= 0) {
        effect_free(self);
        return 2;
    }
    self->preset = effect_preset(self, mode);
    const EffectPreset *p = GP(const EffectPreset, self->preset);
    self->nstages = p->nstages;
    self->send = (float)effect_db_to_gain(self, p->send_db);
    self->dry = (float)effect_db_to_gain(self, p->dry_db);
    int16_t r = effect_build_stages(self, (int16_t)self->nstages, self->stage, GP(const float, p->delays),
                                    GP(const float, p->gains_db), (float)((double)rate * (double)0.001f));
    if (r) return r;
    if (!self->scratch) {
        float *s = vc_malloc((size_t)(uint32_t)(self->chunk * 4));
        GPSET(self->scratch, s);
        if (!s) return 1;
    }
    return r;
}

Effect *effect_ctor(Effect *self) {
    self->send = 0.0f;
    self->chunk = 0x400;
    self->dry = 0.0f;
    self->scratch = 0;
    self->nstages = 0;
    self->rnd = 0.0f;
    self->coef = 0;
    self->ncoef = 0;
    for (int i = 0; i < 5; i++) self->stage[i] = 0;
    return self;
}

void effect_set_fir(Effect *self, int32_t n, const float *coef) {
    GPSET(self->coef, (float *)coef);
    self->ncoef = n;
    for (int32_t i = 0; i < self->nstages; i++) {
        Allpass *ap = GP(Allpass, self->stage[i]);
        if (ap->fir) vc_free(GP(void, ap->fir));
        GPSET(ap->fir, (float *)vc_calloc((size_t)(uint32_t)n, 4));
    }
}
