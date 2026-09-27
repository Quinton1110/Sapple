// Signal-processing leaves of msttssyn.dll, decompiled. Each keeps the original's order of loads,
// stores and x87 roundings (see x87.h), so the output is bit-identical and in-place/aliasing calls
// behave the same.
#include "dsp.h"
#include "x87.h"
#include "crt_vc.h"
#include <string.h>

void dsp_allpole(const float *a, int order, float *mem, float *x, int n) {
    for (; n > 0; n--, x++) {
        mem[0] = *x;
        for (int k = order; k > 0; k--) {
            double p = (double)a[k] * mem[k];
            mem[0] = (float)((double)mem[0] - p);
            mem[k] = mem[k - 1];
        }
        *x = mem[0];
    }
}

void dsp_fir(float *x, int n, const float *c, float *mem, int order) {
    for (; n > 0; n--, x++) {
        float xi = *x;
        mem[0] = xi;
        double acc = (double)xi * c[0];
        for (int k = order - 1; k > 0; k--) {
            float prev = mem[k - 1];
            double p = (double)c[k] * mem[k];
            mem[k] = prev;
            acc = acc + p;
        }
        *x = (float)acc;
    }
}

void dsp_iir_c0(float *x, int n, const float *c, float *mem, int order) {
    for (; n > 0; n--, x++) {
        double acc = (double)c[0] * *x;
        for (int k = order - 1; k > 0; k--) {
            float m = mem[k - 1];
            mem[k] = m;
            double p = (double)c[k] * m;
            acc = p + acc;
        }
        *x = (float)acc;
        mem[0] = (float)acc;
    }
}

double dsp_sum_abs(const float *x, int n) {
    double acc = 0.0;
    for (; n > 0; n--, x++) acc = acc + x87_abs_cmp0(*x);
    return acc;
}

void dsp_scale(float *x, int n, float g) {
    for (; n > 0; n--, x++) *x = (float)((double)g * *x);
}

void dsp_reverse(const float *src, int n, float *dst) {
    const float *s = src + n - 1;
    for (; n > 0; n--) *dst++ = *s--;
}

void dsp_to_pcm16(int16_t *dst, const float *src, int n) {
    for (; n > 0; n--) *dst++ = (int16_t)x87_fistp32((double)*src++);
}

void dsp_to_pcm8(uint8_t *dst, const float *src, int n) {
    for (int i = 0; i < n; i++) {
        int32_t v = x87_ftol(floor((double)src[i] - -0.5));
        dst[i] = (uint8_t)((v >> 8) ^ 0x80);
    }
}

double dsp_max_abs(const float *x, int n) {
    double m = x87_abs_cmp0(x[0]);
    for (int i = 1; i < n; i++) {
        double a = x87_abs_cmp0(x[i]);
        if (!(m >= a)) m = a;
    }
    return m;
}

void dsp_scale_b(float *x, int n, float g) {
    for (; n > 0; n--, x++) *x = (float)((double)g * *x);
}

void dsp_fold_pulse(float *dst, int n, const float *src, int len) {
    int h = len / 2;
    int m = h > n ? n : h;
    for (int i = 0; i < m; i++) dst[i] = src[i];
    for (int i = m; i < n; i++) dst[i] = 0.0f;
    for (int k = 0; k < m; k++) dst[n - m + k] = (float)((double)src[len - m + k] + dst[n - m + k]);
}

void dsp_place_halves(float *dst, const float *src, int off, int n, int len) {
    for (int i = 0; i < n; i++) dst[off + i] = src[i];
    int b = len - off - n + 1;
    for (int i = 0; i < n; i++) dst[b + i] = src[n + i];
}

void dsp_add_halves(float *dst, const float *src, int off, int n, int len) {
    for (int i = 0; i < n; i++) dst[off + i] = (float)((double)src[i] + dst[off + i]);
    int b = len - off - n + 1;
    for (int i = 0; i < n; i++) dst[b + i] = (float)((double)dst[b + i] + src[n + i]);
}

int dsp_cmp_float(const void *pa, const void *pb) {
    double a = *(const float *)pa, b = *(const float *)pb;
    if (a > b) return 1;
    if (a < b) return -1;
    return 0;
}

int dsp_sort_floats(float *x, int n) {
    int sorted = 1;
    for (int i = 1; i < n; i++)
        if (x[i - 1] > x[i]) sorted = 0;
    vc_qsort(x, (size_t)n, 4, dsp_cmp_float);
    return sorted;
}

void dsp_fit_period(const float *src, int srcn, float *dst, int dstn, const float *window, int winn) {
    memset(dst, 0, (size_t)dstn * 4);
    int h = srcn > dstn ? dstn : srcn;
    double step = (double)winn / (double)h;
    dst[0] = src[0];
    double acc = 0.5;
    for (int k = 1; k < h; k++) {
        acc = acc + step;
        double w = window[x87_ftol(acc)];
        dst[k] = (float)((double)src[k] * w + dst[k]);
        dst[dstn - k] = (float)(w * src[srcn - k] + dst[dstn - k]);
    }
}

void dsp_period(const float *src, int srcn, float *dst, int dstn, float gain, int reverse,
                const float *window, int winn) {
    if (reverse) dsp_reverse(src, dstn, dst);
    else if (srcn == dstn) memcpy(dst, src, (size_t)dstn * 4);
    else dsp_fit_period(src, srcn, dst, dstn, window, winn);
    if (!(gain == 1.0f || isnan(gain))) dsp_scale(dst, dstn, gain);
}
