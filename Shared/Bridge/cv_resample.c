/* ClassicVoices streaming resampler - see cv_resample.h. (Moved unchanged out of cv4_bridge.c, 2026-09-22, when
 * Microsoft Anna's 16 kHz joined the 8 kHz and 11025 Hz engines; the SAPI 4 bridge output is byte-identical.) */
#include "cv_resample.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NTAPS 16
#define HALF (NTAPS / 2)

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    int k;
    for (k = 1; k < 40; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

int cv_rs_init(cv_resampler *r, int in_rate)
{
    /* Upsampling (every engine up to 2026-09-27): cutoff 0.47 of the input rate, exactly as before. Downsampling (the
     * neural voices' 24 kHz): the cutoff moves to 0.47 of the OUTPUT rate, in input-sample units, so nothing above
     * 22050 / 2 folds back. */
    const double fc = in_rate > CV_RS_OUT_RATE ? 0.47 * CV_RS_OUT_RATE / in_rate : 0.47, beta = 7.0;
    int p, j, a = CV_RS_OUT_RATE, b = in_rate;
    memset(r, 0, sizeof *r);
    if (in_rate <= 0 || in_rate == CV_RS_OUT_RATE || in_rate > 4 * CV_RS_OUT_RATE) return -1;
    while (b) { int t = a % b; a = b; b = t; }      /* gcd */
    r->L = CV_RS_OUT_RATE / a;
    r->M = in_rate / a;
    r->taps = malloc(sizeof(float) * (size_t)r->L * NTAPS);
    if (!r->taps) return -1;
    for (p = 0; p < r->L; p++) {
        double frac = (double)p / r->L, h[NTAPS], sum = 0;
        for (j = 0; j < NTAPS; j++) {
            double x = (double)(j - HALF + 1) - frac; /* tap j sits at input n + j - HALF + 1 */
            double sinc = x == 0 ? 1.0 : sin(M_PI * 2 * fc * x) / (M_PI * 2 * fc * x);
            double w = x / HALF, win = fabs(w) >= 1.0 ? 0.0 : bessel_i0(beta * sqrt(1.0 - w * w)) / bessel_i0(beta);
            h[j] = sinc * win;
            sum += h[j];
        }
        for (j = 0; j < NTAPS; j++) r->taps[p * NTAPS + j] = (float)(h[j] / sum);
    }
    return 0;
}

void cv_rs_free(cv_resampler *r)
{
    free(r->taps);
    free(r->in);
    free(r->out);
    memset(r, 0, sizeof *r);
}

void cv_rs_reset(cv_resampler *r)
{
    r->nin = 0;
    r->base = 0;
    r->total_in = 0;
    r->next_out = 0;
}

static int rs_grow(void **b, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) return 0;
    size_t c = *cap ? *cap : 4096;
    while (c < need) c *= 2;
    void *nb = realloc(*b, c * elem);
    if (!nb) return -1;
    *b = nb;
    *cap = c;
    return 0;
}

/* in[k] for absolute index k (zero outside what has been received: start padding and the flush) */
static inline float rs_at(const cv_resampler *r, int64_t k)
{
    if (k < (int64_t)r->base || k >= (int64_t)(r->base + r->nin)) return 0.0f;
    return r->in[k - (int64_t)r->base];
}

/* produce every output sample whose taps are available (all of them when flushing); returns count */
size_t cv_rs_run(cv_resampler *r, int flush)
{
    size_t no = 0;
    uint64_t limit_out = flush ? (r->total_in * (uint64_t)r->L + (uint64_t)r->M - 1) / (uint64_t)r->M : UINT64_MAX;
    for (;;) {
        uint64_t k = r->next_out, pos = k * (uint64_t)r->M;
        int64_t n = (int64_t)(pos / (uint64_t)r->L);
        int p = (int)(pos % (uint64_t)r->L);
        if (k >= limit_out) break;
        if (!flush && (uint64_t)(n + HALF) >= r->total_in) break;
        if (rs_grow((void **)&r->out, &r->ocap, no + 1, sizeof *r->out)) break;
        const float *h = r->taps + p * NTAPS;
        float acc = 0;
        int j;
        for (j = 0; j < NTAPS; j++) acc += h[j] * rs_at(r, n + j - HALF + 1);
        long v = lrintf(acc);
        r->out[no++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        r->next_out++;
    }
    /* drop input no longer needed */
    {
        int64_t need = (int64_t)((r->next_out * (uint64_t)r->M) / (uint64_t)r->L) - HALF;
        if (need > (int64_t)r->base) {
            size_t drop = (size_t)(need - (int64_t)r->base);
            if (drop > r->nin) drop = r->nin;
            memmove(r->in, r->in + drop, (r->nin - drop) * sizeof *r->in);
            r->nin -= drop;
            r->base += drop;
        }
    }
    return no;
}

int cv_rs_feed(cv_resampler *r, const int16_t *pcm, size_t n)
{
    size_t i;
    if (rs_grow((void **)&r->in, &r->cap, r->nin + n, sizeof *r->in)) return -1;
    for (i = 0; i < n; i++) r->in[r->nin + i] = (float)pcm[i];
    r->nin += n;
    r->total_in += n;
    return 0;
}
