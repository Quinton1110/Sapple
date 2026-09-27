/* Anna's recording access with a portable WMA Voice (format 0x000A, "WMSpeech") decoder.
 *
 * Bit-exact re-implementation of the decoder in the 32-bit Windows WMSPDMOD.DLL (the DMO that
 * MSTTSDecWrp.dll drives inside the real engine) for the configuration Anna's voice uses: 16 kHz,
 * 450-byte packets, LSP order 16 with inter-frame residual LSP coding, adaptive post-filter mode 5.
 * anna_decoder_read() reproduces MSTTSDecWrp's access pattern (see anna_dec_dmo.c), and the decoder
 * state behaves exactly like the DMO's under it (a Flush destroys the DMO's decoder; a packet whose
 * sequence number does not follow the previous one resets most, but not all, of its state).
 *
 * Every floating-point operation follows the DLL: SSE float code is written as the same sequence of
 * float operations, x87 code (the DLL runs with the default 53-bit x87 precision) as double operations
 * in the same order, and the UCRT math functions it calls (logf, expf, powf, cosf, pow) are
 * re-implemented from the same algorithms and tables (anna_wmav_tab.h).
 *
 * Build requirements: IEEE-754 float/double evaluated in their own precision (FLT_EVAL_METHOD 0; on
 * 32-bit x86 use SSE2 math, e.g. gcc/clang -msse2 -mfpmath=sse; MSVC's x86 default /arch:SSE2 is fine),
 * NO contraction of a*b+c into FMA (gcc/clang: -ffp-contract=off; MSVC: no /fp:fast or /fp:contract),
 * no -ffast-math, default rounding mode.  anna_decoder_open() runs a self-test and refuses to work
 * if the build breaks these rules.
 */
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anna.h"

#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0 && FLT_EVAL_METHOD != -1
#error "anna_dec_wmav.c needs float/double evaluated in their own precision (x86: -msse2 -mfpmath=sse)"
#endif

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off") /* gcc ignores the STDC pragma; still build with -ffp-contract=off */
#endif
#if defined(_MSC_VER)
#pragma fp_contract(off)
#endif

#include "anna_wmav_tab.h"

#ifndef WV_DBG
#define WV_DBG(...) ((void)0)
#endif

/* ================================================================================================ */
/* helpers                                                                                          */

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

/* SSE cvtss2si with the default rounding (nearest, ties to even); |x| < 2^23 here */
static int32_t rint_f(float x)
{
    float fl = floorf(x), d = x - fl;
    int32_t r = (int32_t)fl;
    if (d > 0.5f || (d == 0.5f && (r & 1))) r++;
    return r;
}
static int32_t rint_d(double x)
{
    double fl = floor(x), d = x - fl;
    int32_t r = (int32_t)fl;
    if (d > 0.5 || (d == 0.5 && (r & 1))) r++;
    return r;
}
/* arithmetic shift right of a signed value (sar) */
static int32_t asr(int32_t v, int n) { return v >= 0 ? v >> n : -1 - ((-1 - v) >> n); }
/* cvttss2si */
static int32_t trunc_f(float x) { return (int32_t)x; }

/* SSE maxss / minss: dst = dst > src ? dst : src (resp. <) */
static float maxss(float a, float b) { return a > b ? a : b; }
static float minss(float a, float b) { return a < b ? a : b; }

/* MSB-first bit reader (FUN_100756b1), at most 32 bits */
static uint32_t getbits(const uint8_t *b, uint32_t pos, int n)
{
    uint32_t v = 0;
    if (n > 32) n = 32;
    while (n-- > 0) {
        v = v << 1 | ((b[pos >> 3] >> (7 - (pos & 7))) & 1);
        pos++;
    }
    return v;
}

/* ================================================================================================ */
/* UCRT __libm_sse2_* functions, same algorithm and tables                                          */

static float wv_logf(float x)
{
    uint32_t ix = f2u(x), hi = ix >> 16, base = 0x3f3f;
    double m, r, r2, e, res;
    int i;
    if (((0x7f7fu - hi) | (hi - 0x80u)) >= 0x8000u) {
        if (ix >= 0x80000000u) return ix == 0x80000000u ? u2f(0xff800000u) : u2f(0xffc00000u);
        if (ix >= 0x7f800000u) return x + x;
        if (ix == 0) return u2f(0xff800000u);
        x = x * u2f(0x5f800000u); /* denormal: scale by 2^64 */
        ix = f2u(x);
        hi = ix >> 16;
        base = 0x5f3f;
    }
    m = u2d(((uint64_t)ix << 29 & 0x000fffffffffffffull) | 0x3ff0000000000000ull);
    i = (int)((((hi & 0x7f) + 1) & 0xfe) >> 1);
    e = (double)asr((int32_t)(hi - base), 7);
    r = m * u2d(lm_logf_rcp[i]) - 1.0;
    r2 = r * r;
    res = u2d(lm_logf_log[i]) + e * u2d(LM_LN2);
    res = res + ((u2d(LM_LOGF_P1) * r2 + r) + (r * r2) * (u2d(LM_LOGF_P3) * r + u2d(LM_LOGF_P2)));
    return (float)res;
}

static float wv_expf(float x)
{
    uint32_t ix = f2u(x), ax = ix & 0x7fffffffu;
    int32_t k;
    double t, r, c, p;
    uint64_t pb;
    if (ax - 0x31800000u >= 0x12000000u) {
        if (ax <= 0x31800000u) return 1.0f;
        if (ax < 0x7f800000u) return ix < 0x80000000u ? u2f(0x7f7fffffu) * u2f(0x7f7fffffu) : u2f(0x800000u) * u2f(0x800000u);
        if (ax == 0x7f800000u) return ix == 0x7f800000u ? u2f(0x7f800000u) : 0.0f;
        return x + x;
    }
    k = rint_f(x * u2f(0x46b8aa3bu));
    r = u2d(LM_EXPF_C0) * (double)x - (double)k;
    t = u2d(lm_expf_t16[k & 15]);
    c = u2d(LM_EXPF_C1) * t * r;
    p = u2d(lm_expf_t32a[asr(k, 4) & 31]) * u2d(lm_expf_t32b[asr(k, 9) & 31]);
    pb = d2u(p);
    pb = (pb & 0xffffffffull) | (uint64_t)(uint32_t)((uint32_t)(pb >> 32) + ((uint32_t)asr(k, 14) << 20)) << 32;
    return (float)((t + c) * u2d(pb));
}

static float wv_cosf(float x)
{
    uint32_t ix = f2u(x);
    uint16_t hx = (uint16_t)((ix >> 16) & 0x7fff);
    int32_t k;
    float t, tr;
    uint64_t s3, tb;
    double xd, a, b, z, z2, cp, sp;
    if ((uint16_t)(hx - 0x80) > 0x457f) {
        if ((int16_t)(hx - 0x80) <= 0x457f) { /* |x| < 2^-126 */
            xd = (double)x;
            return (float)(1.0 - xd * xd);
        }
        if ((ix & 0x7f800000u) == 0x7f800000u) return x - x;
        return (float)cos((double)x); /* |x| >= 8192: not reached by the decoder */
    }
    t = u2f(0x4122f983u) * x;
    k = rint_f(t);
    s3 = (uint64_t)(ix & 0x80000000u);
    s3 = (s3 << 32) ^ (s3 << 29);
    xd = (double)x;
    tr = (t + 12582912.0f) - 12582912.0f;
    k = (k + 0x1c7610) & 0x3f;
    tb = (uint64_t)f2u(tr) << 29;
    a = u2d(LM_COSF_PIHI ^ s3) * u2d(tb);
    b = u2d(LM_COSF_PILO ^ s3) * u2d(tb);
    xd = xd - a;
    b = b + xd;
    z = xd * xd;
    z2 = z * z;
    cp = (u2d(LM_COSF_C2) * z + u2d(LM_COSF_ONE)) + u2d(LM_COSF_C4) * z2;
    sp = (u2d(LM_COSF_S3) * z + u2d(LM_COSF_ONE)) + u2d(LM_COSF_S5) * z2;
    return (float)(u2d(lm_cosf_tab[2 * k]) * cp + (b * u2d(lm_cosf_tab[2 * k + 1])) * sp);
}

/* powf, main path only (x normal and positive, result in range); the decoder calls it with y = 0.2
 * and 0.1 <= x <= 1.1.  Anything else goes to the generic fallback (never reached in practice). */
static float wv_powf(float x, float y)
{
    uint32_t ix = f2u(x), hi = ix >> 16;
    double m, rc, rh, rl, e, r, w, r2, r4, lo0, hi1, y2, wr, f, sc, a0, a1, res;
    int32_t n, i;
    uint64_t wb;
    if ((((0x7f7fu - hi) | (hi - 0x80u)) & 0xffffffffu) >= 0x8000u) return (float)pow((double)x, (double)y);
    m = u2d(((uint64_t)ix << 29 & 0x000fffffffffffffull) | 0x3ff0000000000000ull);
    i = (int32_t)((((hi & 0x7f) + 1) & 0xfe) >> 1);
    rc = u2d(lm_powf_rcp[i]);
    y2 = (double)y * u2d(LM_POWF_YSCALE);
    rh = u2d(d2u(rc) >> 26 << 26);
    e = (double)asr((int32_t)(hi - 0x3f3f), 7);
    rl = rc - rh;
    r = m * rl + (rh * m - u2d(lm_powf_rcp[0]));
    r2 = r * r;
    lo0 = u2d(lm_powf_c10[0]) * r; /* lane 0 of xmm6 */
    hi1 = u2d(lm_powf_c10[1]) * r; /* lane 1 of xmm6 */
    w = ((u2d(lm_powf_log[i]) + r) + e) + u2d(lm_powf_c20[0]) * r2;
    lo0 = lo0 + u2d(lm_powf_c20[1]);
    w = w * y2;
    r4 = r2 * r2;
    wb = d2u(w);
    {
        uint32_t ex = (uint32_t)(wb >> 48) & 0x7ff0, chk = ((0x41d0u - ex) | (ex - 0x3e60u));
        if (chk >= 0x80000000u) return (float)pow((double)x, (double)y);
    }
    wr = (w + u2d(LM_POWF_SHIFT)) - u2d(LM_POWF_SHIFT);
    lo0 = lo0 * r4;
    hi1 = hi1 * r2; /* lane 1 of xmm7 still holds r^2 */
    n = rint_d(wr);
    f = w - wr;
    lo0 = lo0 + hi1;
    f = f + y2 * lo0;
    if ((uint32_t)((0xfbf - n) | (n + 0xfa0)) >= 0x80000000u) return (float)pow((double)x, (double)y);
    sc = u2d((uint64_t)(uint16_t)(0x3ff0 + (asr(n, 1) & ~0xf)) << 48);
    a0 = u2d(lm_powf_c50[0]) + u2d(lm_powf_c40[0]) * f;
    a1 = u2d(lm_powf_c50[1]) + u2d(lm_powf_c40[1]) * f;
    {
        double t = u2d(lm_powf_exp[n & 31]), f2 = f * f, ft;
        ft = (f * t) * sc;
        t = t * sc;
        res = (a1 + f2 * a0) * ft + t;
    }
    return (float)res;
}

/* ================================================================================================ */
/* decoder state                                                                                    */

#define WV_ORDER 16
#define WV_FRAME 160
#define WV_SFRAME 480
#define WV_MAXPITCH 296
#define WV_MINPITCH 40
#define WV_EXCHIST (WV_MAXPITCH + 8)
#define WV_SFBUF 136 /* superframe bit buffer (the DLL allocates 131 bytes) */
#define WV_PF_PITCH 34 /* float offset of the pitch post-filter state inside pf[] */

typedef struct {
    /* configuration (fixed for Anna's format) */
    int16_t vbm[25];      /* frame type VLC symbol -> frame type id (extradata) */
    int pf_on;            /* adaptive post-filter */
    int pf_mode;          /* post-filter tables (5 for 450-byte packets) */
    int lsp_mean;         /* which LSP mean table */

    /* packet layer */
    int fresh;            /* no packet decoded since creation/reset */
    int seq_next;
    int sf_left;          /* superframes still to start in the packets seen */
    int inprog;           /* a superframe is being collected */
    int lsp_res;          /* packet header flag: LSPs coded once per superframe */
    uint8_t sfb[WV_SFBUF + 8];
    int sfb_bits;

    /* synthesis state */
    float lsp_prev_sf[WV_ORDER];   /* last LSPs of the previous superframe (residual prediction) */
    float lsp_prev_fr[WV_ORDER];   /* LSPs of the previous frame (subframe interpolation) */
    float lsp[3][WV_ORDER];        /* LSPs of the three frames of the current superframe */
    float gain_hist[8];
    int gain_hist_i[8];            /* row of gain_hist[] in wv_gain_pow[] */
    uint16_t noise_ctr;
    float exc_hist[WV_EXCHIST];
    float syn_mem[WV_ORDER];
    float pf[0x448];
    int16_t sf_pitch[8];
    float out[WV_SFRAME];
} wv_dec;

/* pow(gain history, exponent) as computed by the DLL with the UCRT's double pow(): the gain history
 * only holds max(gain_cb[i], 0.05) clamped to 5.0, or 1.0, so the 129 x 6 results are tabulated
 * (anna_wmav_tab.h: wv_gain_pow, index 128 = 1.0) */
static int gain_hist_index(float g)
{
    int i;
    if (g == 1.0f) return 128;
    for (i = 0; i < 128; i++) {
        float v = maxss(wv_gain_cb[i], 0x1.99999ap-5f);
        if (!(5.0f > v)) v = 5.0f;
        if (v == g) return i;
    }
    return -1;
}

static void codec_reset(wv_dec *d)
{
    /* FUN_1004d17e: what a discontinuity resets (lsp_prev_fr is kept) */
    int i;
    for (i = 0; i < WV_ORDER; i++) d->lsp_prev_sf[i] = (float)(i + 1) / (float)(WV_ORDER * 2 + 2);
    d->noise_ctr = 0;
    memset(d->exc_hist, 0, sizeof d->exc_hist);
    memset(d->syn_mem, 0, sizeof d->syn_mem);
    memset(d->pf, 0, sizeof d->pf);
    for (i = 0; i < 8; i++) {
        d->gain_hist[i] = 1.0f;
        d->gain_hist_i[i] = 128;
    }
    d->fresh = 1;
    d->sf_left = 0;
    d->inprog = 0;
    d->sfb_bits = 0;
}

static void codec_init(wv_dec *d, const uint8_t *fmt)
{
    /* FUN_1004c747 + FUN_1004dd75 for Anna's format */
    const uint8_t *ext = fmt + 18;
    uint32_t flags = (uint32_t)ext[18] | (uint32_t)ext[19] << 8 | (uint32_t)ext[20] << 16 | (uint32_t)ext[21] << 24;
    int cnt[8] = {0}, n, i;
    memset(d, 0, sizeof *d);
    for (i = 0; i < 25; i++) d->vbm[i] = -1;
    for (n = 0; n < 17; n++) {
        int v = (int)getbits(ext + 22, (uint32_t)n * 3, 3);
        if (v < 7) d->vbm[v * 3 + cnt[v]++] = (int16_t)n;
        else d->vbm[21 + cnt[7]++] = (int16_t)n;
    }
    d->pf_on = (flags & 1) != 0 && (flags & 2) != 0;
    d->pf_mode = 5;
    d->lsp_mean = ((flags >> 14) & 1) ? 3 : 2;
    for (i = 0; i < WV_ORDER; i++) d->lsp_prev_fr[i] = (float)(i + 1) / (float)(WV_ORDER * 2 + 2);
    codec_reset(d);
}

/* ================================================================================================ */
/* LSP decoding                                                                                     */

static void vq_add(float *out, const uint8_t *cb, int nst, const int *sizes, const int16_t *idx, int dim, const float *scale, const float *offs)
{
    /* FUN_10076000 */
    int s, j;
    const uint8_t *base = cb;
    for (j = 0; j < dim; j++) out[j] = 0.0f;
    for (s = 0; s < nst; s++) {
        if (s > 0) base += sizes[s - 1] * dim;
        for (j = 0; j < dim; j++) out[j] = ((float)base[idx[s] * dim + j] * scale[s] + offs[s]) + out[j];
    }
}

static uint32_t read_range(const uint8_t *b, uint32_t *pos, int min, int max)
{
    /* FUN_1007562d for one value: ceil(log2(max - min + 1)) bits, plus min */
    int nb = 0, s = 1;
    uint32_t v;
    while (s < max - min + 1) {
        s *= 2;
        nb++;
    }
    v = getbits(b, *pos, nb);
    *pos += (uint32_t)nb;
    return (uint32_t)((int)v + min);
}

static void lsp_stabilize(float *lsp)
{
    /* FUN_10073bcb, order 16, 16 kHz */
    const float fs = (float)(double)16000;
    float t[WV_ORDER], rcp;
    int i, c, j;
    for (i = 0; i < WV_ORDER; i++) t[i] = fs * lsp[i];
    t[0] = maxss(t[0], fs * 0x1.89374cp-11f);
    for (i = 0; i < WV_ORDER - 1; i++) t[i + 1] = maxss(fs * 0x1.99999ap-8f + t[i], t[i + 1]);
    t[WV_ORDER - 1] = minss(t[WV_ORDER - 1], fs * 0x1.ff3b64p-2f);
    rcp = 1.0f / fs;
    for (i = 0; i < WV_ORDER; i++) lsp[i] = rcp * t[i];
    for (i = 1; i < WV_ORDER; i++)
        if (lsp[i - 1] > lsp[i]) break;
    if (i < WV_ORDER) /* insertion sort */
        for (c = 0; c + 1 < WV_ORDER; c++) {
            float x = lsp[c + 1];
            for (j = c; j >= 0 && lsp[j] > x; j--) lsp[j + 1] = lsp[j];
            lsp[j + 1] = x;
        }
}

static void decode_lsps_res(wv_dec *d, const uint8_t *b, uint32_t *pos)
{
    /* FUN_1004e50e with residual coding: frame 3 coded, frames 1 and 2 interpolated + residual */
    static const int sz1[2] = {256, 64}, sz2[2] = {128, 64}, sz3[1] = {128};
    int16_t i1[5], i2[4];
    const float *mean = wv_lsp_mean + 16 * d->lsp_mean;
    float prev[WV_ORDER], cur[WV_ORDER], ip[32], res[32];
    int j;
    for (j = 0; j < WV_ORDER; j++) prev[j] = d->lsp_prev_sf[j] - mean[j];
    i1[0] = (int16_t)read_range(b, pos, 0, 255);
    i1[1] = (int16_t)read_range(b, pos, 0, 63);
    i1[2] = (int16_t)read_range(b, pos, 0, 127);
    i1[3] = (int16_t)read_range(b, pos, 0, 63);
    i1[4] = (int16_t)read_range(b, pos, 0, 127);
    vq_add(cur, wv_lsp_cb1, 2, sz1, i1, 5, wv_lsp_cb1_scale, wv_lsp_cb1_offs);
    vq_add(cur + 5, wv_lsp_cb2, 2, sz2, i1 + 2, 5, wv_lsp_cb2_scale, wv_lsp_cb2_offs);
    vq_add(cur + 10, wv_lsp_cb3, 1, sz3, i1 + 4, 6, wv_lsp_cb3_scale, wv_lsp_cb3_offs);
    /* FUN_10075af6 */
    i2[0] = (int16_t)read_range(b, pos, 0, 31);
    i2[1] = (int16_t)read_range(b, pos, 0, 127);
    i2[2] = (int16_t)read_range(b, pos, 0, 127);
    i2[3] = (int16_t)read_range(b, pos, 0, 127);
    for (j = 0; j < WV_ORDER; j++) {
        float w1 = (float)wv_lsp_interp[i2[0] * 32 + j] * 0x1.583dfcp-15f - 0x1.85bf8p-1f;
        float w2 = (float)wv_lsp_interp[i2[0] * 32 + 16 + j] * 0x1.583dfcp-15f - 0x1.85bf8p-1f;
        ip[j] = (1.0f - w1) * cur[j] + prev[j] * w1;
        ip[16 + j] = (1.0f - w2) * cur[j] + prev[j] * w2;
    }
    vq_add(res, wv_lsp_res1, 1, sz3, i2 + 1, 10, wv_lsp_res1_scale, wv_lsp_res1_offs);
    vq_add(res + 10, wv_lsp_res2, 1, sz3, i2 + 2, 10, wv_lsp_res2_scale, wv_lsp_res2_offs);
    vq_add(res + 20, wv_lsp_res3, 1, sz3, i2 + 3, 12, wv_lsp_res3_scale, wv_lsp_res3_offs);
    for (j = 0; j < WV_ORDER; j++) {
        d->lsp[0][j] = (ip[j] + mean[j]) - res[2 * j];
        d->lsp[1][j] = (ip[16 + j] + mean[j]) - res[2 * j + 1];
        d->lsp[2][j] = cur[j] + mean[j];
    }
    lsp_stabilize(d->lsp[0]);
    lsp_stabilize(d->lsp[1]);
    lsp_stabilize(d->lsp[2]);
    memcpy(d->lsp_prev_sf, d->lsp[2], sizeof d->lsp_prev_sf);
}

/* LSP (normalized frequencies) -> LPC, FUN_10073967 / FUN_10073add */
static void lsp_poly(const float *c, float *p, int h)
{
    int i, k;
    float b;
    {
        float c1 = c[1] + c[1], c0 = c[0] * -2.0f;
        float pr = c1 * c0;
        p[0] = c0 - c1;
        p[1] = 2.0f - pr;
    }
    for (i = 2; i < h; i++) {
        float p0;
        b = c[i] * -2.0f;
        p[i] = b * p[i - 1] + (p[i - 2] + p[i - 2]);
        for (k = i - 1; k >= 2; k--) p[k] = (b * p[k - 1] + p[k - 2]) + p[k];
        p0 = p[0];
        p[0] = p0 - (c[i] + c[i]);
        p[1] = (p0 * b + 1.0f) + p[1];
    }
}

static void lsp2lpc(const float *lsp, float *a)
{
    float c1[8], c2[8], p[8], q[8], P[8], Q[8];
    int i;
    for (i = 0; i < 8; i++) {
        c1[i] = wv_cosf(lsp[2 * i] * 0x1.921fb6p+2f);
        c2[i] = wv_cosf(lsp[2 * i + 1] * 0x1.921fb6p+2f);
    }
    lsp_poly(c1, p, 8);
    lsp_poly(c2, q, 8);
    P[0] = p[0] + 1.0f;
    Q[0] = q[0] - 1.0f;
    for (i = 1; i < 8; i++) {
        P[i] = p[i - 1] + p[i];
        Q[i] = q[i] - q[i - 1];
    }
    for (i = 0; i < 8; i++) {
        a[i] = (P[i] + Q[i]) * 0.5f;
        a[15 - i] = (P[i] - Q[i]) * 0.5f;
    }
}

/* ================================================================================================ */
/* DSP kernels                                                                                      */

/* x87 dot product as the DLL's compiler generated it (5 partial sums, 53-bit precision) */
static float x87_dot(const float *a, const float *b, int n)
{
    double s0, s1, s2, s3, s4, sum;
    int c = n;
    if (c > 4) {
        s4 = (double)a[c - 1] * b[c - 1];
        s3 = (double)a[c - 2] * b[c - 2];
        s2 = (double)a[c - 3] * b[c - 3];
        s1 = (double)a[c - 4] * b[c - 4];
        s0 = (double)a[c - 5] * b[c - 5];
        c -= 5;
        while (c > 2) {
            double q1 = (double)a[c - 1] * b[c - 1], q2, q3, n2, n3, n4;
            n4 = s4 + s1;
            q2 = (double)a[c - 2] * b[c - 2];
            n3 = s3 + s0;
            q3 = (double)a[c - 3] * b[c - 3];
            n2 = s2 + q1;
            s0 = q3;
            s1 = q2;
            s2 = n2;
            s3 = n3;
            s4 = n4;
            c -= 3;
        }
        sum = ((s4 + s3) + s0) + (s2 + s1);
    } else
        sum = 0.0;
    for (; c > 0; c--) sum = sum + (double)a[c - 1] * b[c - 1];
    return (float)sum;
}

/* all-pole synthesis 1/A(z), order 16, n % 4 == 0 (FUN_10072f78): lags 4..16 in SSE float,
 * lags 1..3 in x87 with partly unrounded intermediate outputs */
static void synth16(float *y, const float *x, const float *a, float *mem, int n)
{
    int i, j, l, k;
    const double A = a[0], B = a[1], C = a[2];
    for (i = 0; i < 16; i++) y[-1 - i] = mem[i];
    for (j = 0; j < n; j += 4) {
        float s[4];
        double ym1 = y[j - 1], ym2 = y[j - 2], ym3 = y[j - 3];
        double T1, U, V, Y0, W1, W2, Y1, Y2, X;
        for (l = 0; l < 4; l++) {
            float acc = x[j + l];
            for (k = 16; k >= 4; k--) acc = acc - y[j + l - k] * a[k - 1];
            s[l] = acc;
        }
        T1 = ym2 * C + ym1 * B;
        U = ym3 * C + ym2 * B;
        V = U + ym1 * A;
        Y0 = (double)s[0] - V;
        W1 = (double)s[1] - T1;
        W2 = (double)s[2] - ym1 * C;
        y[j] = (float)Y0;
        Y1 = W1 - Y0 * A;
        y[j + 1] = (float)Y1;
        W2 = W2 - (double)y[j] * B;
        Y2 = W2 - Y1 * A;
        y[j + 2] = (float)Y2;
        X = (double)s[3] - Y2 * A;
        X = X - (double)y[j + 1] * B;
        X = X - (double)y[j] * C;
        y[j + 3] = (float)X;
    }
    for (i = 0; i < 16; i++) mem[i] = y[n - 1 - i];
}

/* FIR 1 + sum h[k] z^-(k+1), order 16, SSE float (FUN_1007340e) */
static void fir16(float *out, const float *in, const float *h, float *mem, int n)
{
    float t[16 + WV_FRAME];
    int i, j, k;
    for (i = 0; i < 16; i++) t[i] = mem[15 - i];
    memcpy(t + 16, in, sizeof(float) * (size_t)n);
    for (j = 0; j < n; j++) {
        float acc = t[16 + j];
        for (k = 15; k >= 0; k--) acc = acc + t[15 + j - k] * h[k];
        out[j] = acc;
    }
    for (i = 0; i < 16; i++) mem[i] = t[16 + n - 1 - i];
}

/* 64-point complex FFT on split arrays (FUN_1007287b) */
static void fft64(float *re, float *im)
{
    int i, j, st, t = 0;
    for (i = 0; i < 28; i++) {
        int a = wv_fft_swap[2 * i], b = wv_fft_swap[2 * i + 1];
        float x = re[b];
        re[b] = re[a];
        re[a] = x;
        x = im[b];
        im[b] = im[a];
        im[a] = x;
    }
    for (i = 0; i < 64; i += 2) {
        float r0 = re[i], r1 = re[i + 1], i0 = im[i], i1 = im[i + 1];
        re[i + 1] = r0 - r1;
        re[i] = r0 + r1;
        im[i + 1] = i0 - i1;
        im[i] = i0 + i1;
    }
    for (i = 0; i < 64; i += 4) {
        float r0 = re[i], r2 = re[i + 2], i0 = im[i], i2 = im[i + 2];
        re[i + 2] = r0 - r2;
        re[i] = r0 + r2;
        im[i + 2] = i0 - i2;
        im[i] = i0 + i2;
    }
    for (i = 1; i < 64; i += 4) {
        float r0 = re[i], i2 = im[i + 2], i0 = im[i], nr2 = -re[i + 2];
        re[i + 2] = r0 - i2;
        re[i] = r0 + i2;
        im[i + 2] = i0 - nr2;
        im[i] = i0 + nr2;
    }
    for (st = 2; st < 6; st++) {
        int h = 1 << st, span = 2 * h;
        for (i = 0; i < 64; i += span) {
            float rb = re[i + h], ra = re[i], ib = im[i + h], ia = im[i];
            re[i + h] = ra - rb;
            re[i] = rb + ra;
            im[i + h] = ia - ib;
            im[i] = ib + ia;
        }
        for (j = 1; j < h; j++) {
            float wr = wv_fft_wr[t], wi = wv_fft_wi[t];
            t++;
            for (i = j; i < 64; i += span) {
                float a = re[i + h], b = im[i + h];
                float tr = a * wr - b * wi;
                float ti = a * wi + b * wr;
                re[i + h] = re[i] - tr;
                re[i] = tr + re[i];
                im[i + h] = im[i] - ti;
                im[i] = ti + im[i];
            }
        }
    }
}

/* 128-point real FFT in place, output DC, Nyquist and bins 1..63 as re/im pairs (FUN_100724bd) */
static void rfft128(float *x)
{
    float re[64], im[64];
    int k;
    for (k = 0; k < 64; k++) {
        re[k] = x[2 * k];
        im[k] = x[2 * k + 1];
    }
    fft64(re, im);
    x[1] = 0.0f;
    x[0] = re[0] + im[0];
    x[129] = 0.0f;
    x[128] = re[0] - im[0];
    for (k = 1; k < 32; k++) {
        int j = 64 - k;
        float A = (im[k] + im[j]) * 0.5f, D = (im[k] - im[j]) * 0.5f;
        float H = (re[k] + re[j]) * 0.5f, Bv = (re[j] - re[k]) * 0.5f;
        float c = wv_rfft_cos[k], s = wv_rfft_sin[k];
        float cA = c * A, sB = s * Bv, cB = c * Bv, sA = s * A;
        x[2 * k] = (cA + H) + sB;
        x[2 * k + 1] = (cB + D) - sA;
        x[2 * j + 1] = (cB - D) - sA;
        x[2 * j] = (H - cA) - sB;
    }
    x[64] = re[32];
    x[65] = -im[32];
}

/* inverse of rfft128 scaled by 1/64 (FUN_100726a0) */
static void irfft128(float *x)
{
    float re[64], im[64];
    int k;
    im[0] = (x[0] + x[128]) * 0.5f;
    re[0] = (x[0] - x[128]) * 0.5f;
    for (k = 1; k < 32; k++) {
        int j = 64 - k;
        float D = x[2 * k] - x[2 * j], S = x[2 * j] + x[2 * k];
        float P = x[2 * j + 1] + x[2 * k + 1], M = x[2 * k + 1] - x[2 * j + 1];
        float c = wv_rfft_cos[k], s = wv_rfft_sin[k];
        float A = c * D - s * P, B = s * D + c * P;
        im[k] = (S - B) * 0.5f;
        im[j] = (B + S) * 0.5f;
        re[k] = (A + M) * 0.5f;
        re[j] = (A - M) * 0.5f;
    }
    im[32] = x[64];
    re[32] = -x[65];
    fft64(re, im);
    for (k = 0; k < 64; k++) {
        x[2 * k] = im[k] * 0.015625f;
        x[2 * k + 1] = re[k] * 0.015625f;
    }
}

/* adaptive codebook: fractional-delay interpolation of the excitation history (FUN_10073e24) */
static void acb_vector(float *src, int n, float *out, float pitch)
{
    int ip = trunc_f(pitch + 0x1.99999ap-1f), frac, i, k;
    float fi = (float)ip;
    if (fi - 0x1.333334p-1f > pitch) frac = 1;
    else if (fi - 0x1.99999ap-2f > pitch) frac = 0;
    else if (fi - 0x1.99999ap-3f > pitch) frac = 2;
    else frac = 3;
    if (frac == 3) {
        int m = ip < n ? ip : n;
        for (i = 0; i < m; i++) out[i] = src[i - ip];
        for (; i < n; i++) out[i] = out[i - ip];
        return;
    }
    {
        const float *c = wv_acb_interp + 16 * frac;
        for (i = 0; i < n; i++) {
            const float *s = src + i - 7 - ip;
            float l[4], v;
            for (k = 0; k < 4; k++) {
                float acc = 0.0f;
                acc = acc + s[k] * c[k];
                acc = acc + s[4 + k] * c[4 + k];
                acc = acc + s[8 + k] * c[8 + k];
                acc = acc + s[12 + k] * c[12 + k];
                l[k] = acc;
            }
            v = (l[0] + l[2]) + (l[1] + l[3]);
            src[i] = v;
            out[i] = v;
        }
    }
}

/* pitch value from its code (FUN_10075e5e) */
static float pitch_value(int v)
{
    static const int p0 = 40, p1 = 100, p2 = 176, p3 = 295;
    int16_t s = (int16_t)v;
    int16_t A = (int16_t)(uint16_t)((p1 - p0) * 4), B = (int16_t)(uint16_t)((p2 - p1) * 2);
    if (s < A) return (float)s * 0.25f + (float)p0;
    s = (int16_t)(s - A);
    if (s < B) return (float)s * 0.5f + (float)p1;
    s = (int16_t)(s - B);
    if (s < (int16_t)(p3 - p2 + 1)) return (float)(p2 + s);
    return (float)p3;
}

/* position of the noise excitation for silence frames (FUN_10075e04) */
static int noise_offset(uint16_t ctr, uint16_t sf, uint16_t len)
{
    uint32_t t = ((uint32_t)sf * 0x757u + ctr) % 0xffffu;
    int32_t m = (int32_t)((t % 9) * 5 + 6);
    int32_t q = (int32_t)(uint32_t)(t * 0xc34bu) / m;
    int32_t u = (int32_t)(uint16_t)q;
    return (int16_t)(u % (1000 - (int32_t)len));
}

/* fixed codebook pulses (FUN_10075ce6 / FUN_10075f17) */
static void pulse_pos(int16_t code, int n, int mode, int16_t *pos, int16_t *sign)
{
    int b = 0;
    uint16_t v = 1;
    int16_t mask = (int16_t)(n - 1), lo = (int16_t)(code & mask), hi;
    while ((int16_t)v < (int16_t)n) {
        v = (uint16_t)(v * 2);
        b++;
    }
    hi = (int16_t)(code >> b);
    if (mode == 2) {
        int16_t s;
        hi = (int16_t)(hi & mask);
        pos[1] = lo;
        pos[0] = hi;
        s = ((int16_t)(code >> (2 * b)) == 1) ? 1 : -1;
        sign[0] = s;
        sign[1] = hi < lo ? (int16_t)-s : s;
    } else {
        pos[0] = lo;
        sign[0] = hi == 1 ? 1 : -1;
    }
}

static void fcb_pulses(int npulses, int n, float *out, const int16_t *codes)
{
    int per = n / 5, np = npulses - 5, i;
    int16_t pos[2], sign[2];
    for (i = 0; i < n; i++) out[i] = 0.0f;
    for (i = 0; i < np; i++) {
        pulse_pos(codes[i], per, 2, pos, sign);
        out[pos[0] * 5 + i] = (float)sign[0] + out[pos[0] * 5 + i];
        out[pos[1] * 5 + i] = (float)sign[1] + out[pos[1] * 5 + i];
    }
    for (i = np < 0 ? 0 : np; i < 5; i++) {
        pulse_pos(codes[i], per, 1, pos, sign);
        out[pos[0] * 5 + i] = (float)sign[0] + out[pos[0] * 5 + i];
    }
}

/* ================================================================================================ */
/* post-filters                                                                                     */

/* long-term (pitch) enhancement of the excitation followed by synthesis with its own memory
 * (FUN_1007490e); st = pf + WV_PF_PITCH, the subframe's excitation is already at st[40 + 296] */
static void pitch_postfilter(float *out, const float *lpc, float *st, int pitch, int n)
{
    float *cur = st + 40 + WV_MAXPITCH;
    float ybuf[16 + WV_FRAME];
    float best = 0.0f, gain = 0.0f, norm, E;
    int lo = pitch - 3, hi = pitch + 3, lag, best_lag = pitch, i;
    if (WV_MINPITCH > lo) lo = WV_MINPITCH;
    if (WV_MAXPITCH < hi) hi = WV_MAXPITCH;
    for (lag = lo; lag <= hi; lag++) {
        float c = x87_dot(cur, cur - lag, n);
        if (c > best) {
            best = c;
            best_lag = lag;
        }
    }
    E = x87_dot(cur - best_lag, cur - best_lag, n);
    if (E * best != 0.0f) {
        if (0.0f > best) gain = 0.0f;
        else if (best > E) gain = 1.0f;
        else gain = best / E;
    }
    gain = gain * 0x1.333334p-1f;
    norm = 1.0f / (gain + 1.0f);
    for (i = 0; i < n; i++) out[i] = (gain * cur[i - best_lag] + cur[i]) * norm;
    memset(ybuf, 0, sizeof ybuf);
    synth16(ybuf + 16, out, lpc, st, n);
    memcpy(out, ybuf + 16, sizeof(float) * (size_t)n);
    memmove(cur - (WV_MAXPITCH + 8), cur + n - (WV_MAXPITCH + 8), sizeof(float) * (WV_MAXPITCH + 8));
}

/* sum|ref| / sum|out| (FUN_100748ad, x87) */
static float agc_ratio(const float *ref, const float *out, int n)
{
    double sa = 0.0, sb = 0.0;
    float fa, fb;
    int i;
    for (i = 0; i < n; i++) {
        sa = sa + fabs((double)ref[i]);
        sb = sb + fabs((double)out[i]);
    }
    fa = (float)sa;
    fb = (float)sb;
    return fb > 0.0f ? fa / fb : 0.0f;
}

/* spectral post-filter (FUN_10074cc4): FIR designed from the compressed LPC envelope, tilt
 * compensation and gain control; st = pf (0x2a0 block), out filtered in place, ref = synthesis */
static void spectral_postfilter(const float *ref, float *out, const float *lpc, float *st, int n, int mode)
{
    float B[130], L[65], mx, mn, range, scale, sum, thr, mn2, *h = st + 16;
    const float c0 = wv_pf_tilt[2 * mode], c1 = wv_pf_tilt[2 * mode + 1];
    int i;
    memset(B + 2, 0, sizeof(float) * 128);
    B[0] = c0;
    B[1] = c0 * lpc[0] + c1;
    for (i = 1; i < WV_ORDER; i++) B[1 + i] = c1 * lpc[i - 1] + c0 * lpc[i];
    B[1 + WV_ORDER] = c1 * lpc[WV_ORDER - 1];
    rfft128(B);
    for (i = 1; i < 64; i++) {
        float m2 = B[2 * i + 1] * B[2 * i + 1] + B[2 * i] * B[2 * i];
        L[i] = m2 != 0.0f ? wv_logf(m2) : -30.0f;
    }
    if (B[0] != 0.0f) {
        float v = wv_logf(fabsf(B[0]));
        L[0] = v + v;
    } else
        L[0] = -30.0f;
    if (B[128] != 0.0f) {
        float v = wv_logf(fabsf(B[128]));
        L[64] = v + v;
    } else
        L[64] = -30.0f;
    mx = -30.0f;
    mn = 30.0f;
    for (i = 0; i < 65; i++) {
        float v = -(wv_pf_logmul[mode] * L[i]);
        L[i] = v;
        mx = maxss(v, mx);
        mn = minss(v, mn);
    }
    range = mx - mn;
    scale = range * wv_pf_scale[mode];
    sum = 0.0f;
    for (i = 0; i < 65; i++) {
        float r;
        if (range != 0.0f) L[i] = (L[i] - mn) / range + 0x1.99999ap-4f;
        r = wv_powf(L[i], wv_pf_powexp[mode]) * scale;
        L[i] = r;
        sum = r + sum;
    }
    thr = sum * wv_pf_limit[mode] * 0x1.f81f82p-7f;
    mn2 = 30.0f;
    for (i = 0; i < 65; i++) {
        float v = L[i];
        if (v > thr) {
            v = thr;
            L[i] = thr;
        }
        mn2 = minss(v, mn2);
    }
    for (i = 0; i < 65; i++) {
        if (mode >= 5 && mn2 > 0.75f) L[i] = (L[i] - mn2) + 0.75f;
        B[2 * i] = wv_expf(L[i]);
        B[2 * i + 1] = 0.0f;
    }
    irfft128(B);
    for (i = 0; i < 16; i++) h[i] = B[0] != 0.0f ? B[1 + i] / B[0] : 0.0f;
    fir16(out, out, h, st, n);
    if (mode >= 5) {
        float k = range * wv_pf_detilt[mode], g = k + 1.0f;
        for (i = 0; i < n; i++) {
            float v = g * out[i] - k * st[33];
            out[i] = v;
            st[33] = v;
        }
    }
    {
        float r = agc_ratio(ref, out, n) * 0x1.47ae14p-7f;
        for (i = 0; i < n; i++) {
            float g = st[32] * 0x1.fae148p-1f + r;
            st[32] = g;
            out[i] = g * out[i];
        }
    }
}

/* ================================================================================================ */
/* frames and superframes                                                                           */

static int frame_type(const wv_dec *d, const uint8_t *b, uint32_t *pos, uint32_t limit)
{
    /* FUN_100755ad + FUN_1004e900: returns the index into wv_frame_desc, -1 on error, -2 if the
     * bits run out */
    int nb = 2, k, i, id;
    uint16_t e = 0;
    const uint16_t *base = wv_vlc_tree;
    for (k = 8; k > 0; k--) {
        uint32_t v;
        if (*pos + (uint32_t)nb > limit) return -2;
        v = getbits(b, *pos, nb);
        *pos += (uint32_t)nb;
        e = base[v];
        if (e & 0x8000) break;
        nb = (e >> 12) & 7;
        base += v + (e & 0xfff);
    }
    k = e & 0xfff;
    if (k >= 25) return -1;
    id = d->vbm[k];
    for (i = 0; i < 24; i++)
        if (wv_frame_desc[i * 8] == id) return i;
    return -1;
}

static int frame_bits(int t)
{
    /* FUN_1004e1cb */
    const int16_t *fd = wv_frame_desc + t * 8;
    int nsf = WV_FRAME / fd[1], bits = (fd[3] + fd[4]) * nsf;
    if (fd[2] == 1) bits += 8;
    else if (fd[2] == 2) bits += 6 * (nsf - 1) + 9;
    return bits;
}

static void set_gain_hist(wv_dec *d, int k8, float g, int gi)
{
    int j;
    if (k8 <= 7)
        for (j = 0; j < 8 - k8; j++) {
            d->gain_hist[7 - j] = d->gain_hist[7 - k8 - j];
            d->gain_hist_i[7 - j] = d->gain_hist_i[7 - k8 - j];
        }
    for (j = 0; j < k8; j++) {
        d->gain_hist[j] = g;
        d->gain_hist_i[j] = gi;
    }
}

static int decode_frame(wv_dec *d, const uint8_t *b, uint32_t *pos, uint32_t limit, int f)
{
    /* FUN_1004e9cf */
    float exc[WV_EXCHIST + WV_FRAME], *E = exc + WV_EXCHIST;
    float syn[16 + WV_FRAME], *Y = syn + 16;
    float *out = d->out + f * WV_FRAME, *lspc = d->lsp[f];
    const int16_t *fd;
    int t, sflen, nsf, acb, fcb, fcbp, k8, sf, pfflag, prev_idx = 0;
    float gfix = 0.0f, acb_gain = 0.0f, pitch = 0.0f;
    t = frame_type(d, b, pos, limit);
    if (t < 0) {
        WV_DBG("frame type error %d at %u\n", t, *pos);
        return -1;
    }
    fd = wv_frame_desc + t * 8;
    sflen = fd[1];
    nsf = WV_FRAME / sflen;
    acb = fd[2];
    fcb = fd[6];
    fcbp = fd[7];
    if (nsf < 1 || acb == 1 || fcb == 3 || fcb > 7) { /* not used by Anna's data */
        WV_DBG("unsupported frame type %d\n", t);
        return -1;
    }
    k8 = 8 / nsf;
    if (acb == 0)
        for (sf = 0; sf < nsf; sf++) d->sf_pitch[sf] = 0;
    if (fcb == 0) gfix = wv_silence_gain[read_range(b, pos, 0, 255)];
    pfflag = fcb == 0 ? 0 : fcb == 1 ? 1 : 2;
    memcpy(exc, d->exc_hist, sizeof d->exc_hist);
    for (sf = 0; sf < nsf; sf++) {
        int off = sf * sflen, i;
        float fcbv[WV_FRAME], acbv[WV_FRAME], lspi[WV_ORDER], lpc[WV_ORDER], hist_new = 1.0f, w;
        int hist_i = 128;
        if (acb == 2) {
            int v;
            if (sf == 0) v = (int)read_range(b, pos, 0, 511);
            else v = (int16_t)((int16_t)read_range(b, pos, -32, 31) + (int16_t)prev_idx);
            {
                int m = v <= 32 ? 32 : v;
                if (m >= 480) prev_idx = 480;
                else prev_idx = v > 32 ? (uint16_t)v : 32;
            }
            pitch = pitch_value(v);
            d->sf_pitch[sf] = (int16_t)trunc_f(pitch);
        }
        if (fcb == 0) {
            int o = noise_offset(d->noise_ctr, (uint16_t)sf, (uint16_t)sflen);
            for (i = 0; i < sflen; i++) fcbv[i] = gfix * wv_hardcoded_exc[o + i];
        } else if (fcb == 1) {
            int idx = (int16_t)read_range(b, pos, 0, 255);
            gfix = wv_hardcoded_gain[read_range(b, pos, 0, 63)];
            for (i = 0; i < sflen; i++) fcbv[i] = gfix * wv_hardcoded_exc[idx + i];
        } else {
            int16_t codes[5];
            int bl = fcb == 5 ? 0x1f : fcb == 6 ? 0xf : 7, bh = fcb == 5 ? 0x1ff : fcb == 6 ? 0x7f : 0x1f;
            if (fcbp == 5)
                for (i = 0; i < 5; i++) codes[i] = (int16_t)read_range(b, pos, 0, bl);
            else if (fcbp == 7) {
                for (i = 0; i < 2; i++) codes[i] = (int16_t)read_range(b, pos, 0, bh);
                for (i = 2; i < 5; i++) codes[i] = (int16_t)read_range(b, pos, 0, bl);
            } else if (fcbp == 10)
                for (i = 0; i < 5; i++) codes[i] = (int16_t)read_range(b, pos, 0, bh);
            fcb_pulses(fcbp, sflen, fcbv, codes);
        }
        if (fcb >= 3) {
            float en = x87_dot(fcbv, fcbv, sflen), x = (float)sflen > en ? 1.0f : en / (float)sflen, g0, gc, fg;
            int gi;
            g0 = (float)(0x1.5b0a3ep+7 / sqrt((double)x));
            for (i = 0; i < 6; i++) g0 = (float)(u2d(wv_gain_pow[d->gain_hist_i[i] * 6 + i]) * (double)g0);
            gi = (int)read_range(b, pos, 0, 127);
            gc = wv_gain_cb[gi];
            acb_gain = wv_gain_cb2[gi];
            fg = g0 * gc;
            hist_new = maxss(gc, 0x1.99999ap-5f);
            if (!(5.0f > hist_new)) hist_new = 5.0f;
            hist_i = gain_hist_index(hist_new);
            if (hist_i < 0) return -1;
            for (i = 0; i < sflen; i++) fcbv[i] = fg * fcbv[i];
        }
        set_gain_hist(d, k8, hist_new, hist_i);
        if (acb == 2) {
            acb_vector(E + off, sflen, acbv, pitch);
            for (i = 0; i < sflen; i++) acbv[i] = acb_gain * acbv[i];
            for (i = 0; i < sflen; i++) E[off + i] = fcbv[i] + acbv[i];
        } else
            memcpy(E + off, fcbv, sizeof(float) * (size_t)sflen);
        w = (float)(2 * sf + 1) / (float)(2 * nsf);
        for (i = 0; i < WV_ORDER; i++) lspi[i] = (1.0f - w) * d->lsp_prev_fr[i] + w * lspc[i];
        lsp2lpc(lspi, lpc);
        synth16(Y + off, E + off, lpc, d->syn_mem, sflen);
        if (d->pf_on) {
            if (pfflag == 0)
                memmove(out + off, Y + off, sizeof(float) * (size_t)sflen);
            else {
                float *ps = d->pf + WV_PF_PITCH;
                memmove(ps + WV_MAXPITCH + 40, E + off, sizeof(float) * (size_t)sflen);
                pitch_postfilter(out + off, lpc, ps, d->sf_pitch[sf], sflen);
                spectral_postfilter(Y + off, out + off, lpc, d->pf, sflen, d->pf_mode);
            }
        } else
            memcpy(out + off, Y + off, sizeof(float) * (size_t)sflen);
    }
    memmove(d->exc_hist, exc + WV_FRAME, sizeof d->exc_hist);
#ifdef WV_FRAME_HOOK
    WV_FRAME_HOOK(d, f);
#endif
    memcpy(d->lsp_prev_fr, lspc, sizeof d->lsp_prev_fr);
    d->noise_ctr = (uint16_t)((d->noise_ctr + 1u) % 0xffffu);
    return 0;
}

/* size of the superframe in the buffer (FUN_1004e024): returns bits, 0 = incomplete, -1 = error */
static int superframe_size(const wv_dec *d, const uint8_t *b, int nbits)
{
    uint32_t pos = 1, lim = (uint32_t)nbits;
    int f;
    if (nbits < 1) return 0;
    if (getbits(b, 0, 1)) {
        pos = 13;
        if (nbits < 13) return 0;
        if (getbits(b, 1, 12) > WV_SFRAME) return -1;
    }
    if (d->lsp_res) pos += 60;
    if (pos > lim) return 0;
    for (f = 0; f < 3; f++) {
        int t;
        if (!d->lsp_res) pos += 34;
        t = frame_type(d, b, &pos, lim);
        if (t == -2) return 0;
        if (t < 0) return -1;
        pos += (uint32_t)frame_bits(t);
        if (pos > lim) return 0;
    }
    if (pos + 1 > lim) return 0;
    if (getbits(b, pos, 1)) {
        uint32_t idx;
        if (pos + 5 > lim) return 0;
        idx = getbits(b, pos + 1, 4);
        pos += 5 + (uint32_t)wv_extra_bits[idx];
        if (pos > lim) return 0;
    } else
        pos += 1;
    return (int)pos;
}

/* decode the buffered superframe (FUN_1004e2fd); returns the number of samples, -1 on error */
static int decode_superframe(wv_dec *d)
{
    uint32_t pos = 1, lim = (uint32_t)d->sfb_bits;
    int n = WV_SFRAME, f;
    if (getbits(d->sfb, 0, 1)) {
        n = (int)getbits(d->sfb, 1, 12);
        pos = 13;
    }
    if (!d->lsp_res) return -1; /* per-frame LSPs are not used by Anna's data */
    decode_lsps_res(d, d->sfb, &pos);
    for (f = 0; f < 3; f++)
        if (decode_frame(d, d->sfb, &pos, lim, f) < 0) return -1;
    return n;
}

/* one 450-byte packet (already descrambled); returns the number of samples written to pcm (at most cap), -1 on error */
static int decode_packet(wv_dec *d, const uint8_t *p, int len, int16_t *pcm, int cap)
{
    uint32_t pos = 0, nb = (uint32_t)len * 8;
    int seq, res, nsf = 0, spill, v, total = 0;
    seq = (int)getbits(p, 0, 4);
    res = (int)getbits(p, 4, 1);
    pos = 5;
    do {
        v = (int)getbits(p, pos, 6);
        pos += 6;
        nsf += v;
    } while (v == 63);
    spill = (int)getbits(p, pos, 12);
    pos += 12;
    d->lsp_res = res;
    if (!d->fresh && seq != d->seq_next) codec_reset(d);
    if (d->fresh) {
        pos += (uint32_t)spill;
        d->sf_left = nsf;
        d->inprog = 0;
        d->sfb_bits = 0;
    } else {
        d->sf_left += nsf;
        if (spill == 0) d->inprog = 0;
    }
    d->fresh = 0;
    d->seq_next = (seq + 1) & 15;
    for (;;) {
        int take, have, sz, n, i;
        if (!d->inprog) {
            if (d->sf_left < 1 || pos >= nb) break;
            if (!getbits(p, pos, 1)) return -1; /* WMA Pro "music" superframe: not supported */
            pos++;
            d->sf_left--;
            d->inprog = 1;
        }
        have = d->sfb_bits;
        take = (int)(nb - pos);
        if (take > WV_SFBUF * 8 - have) take = WV_SFBUF * 8 - have;
        for (i = 0; i < take; i++) {
            uint32_t bp = (uint32_t)(have + i);
            int bit = (int)((p[(pos + (uint32_t)i) >> 3] >> (7 - ((pos + (uint32_t)i) & 7))) & 1);
            if (bit) d->sfb[bp >> 3] |= (uint8_t)(0x80 >> (bp & 7));
            else d->sfb[bp >> 3] &= (uint8_t)~(0x80 >> (bp & 7));
        }
        d->sfb_bits = have + take;
        sz = superframe_size(d, d->sfb, d->sfb_bits);
        if (sz < 0) {
            WV_DBG("superframe_size error (%d bits)\n", d->sfb_bits);
            return -1;
        }
        if (sz == 0) { /* continues in the next packet */
            pos = nb;
            break;
        }
        pos += (uint32_t)(sz - have);
        d->sfb_bits = sz;
        n = decode_superframe(d);
        if (n < 0) {
            WV_DBG("decode_superframe error\n");
            return -1;
        }
        if (total + n > cap) return -1;
        for (i = 0; i < n; i++) {
            float x = d->out[i];
            pcm[total + i] = x > 32767.0f ? (int16_t)0x7fff : (-32768.0f > x ? (int16_t)-32768 : (int16_t)trunc_f(x));
        }
        total += n;
        d->sfb_bits = 0;
        d->inprog = 0;
    }
    return total;
}

/* ================================================================================================ */
/* build self-test: detects FMA contraction and excess precision                                    */

static int selftest(void)
{
    /* a*a+c where fusing or excess precision changes the result: (1+2^-12)^2 rounds to 1+2^-11 in
       float, (1+2^-27)^2 to 1+2^-26 in double */
    volatile float a = 0x1.001p+0f, c = -0x1.002p+0f;
    volatile double x = 0x1.0000002p+0, z = -0x1.0000004p+0;
    float r = a * a + c;
    double s = x * x + z;
    if (r != 0.0f || s != 0.0) return 0;
    /* spot checks of the math replicas against the UCRT results */
    if (f2u(wv_logf(0x1.333334p-2f)) != 0xbf9a1bc8u) return 0;
    if (f2u(wv_expf(-0x1.b33334p+0f)) != 0x3e3b1163u) return 0;
    if (f2u(wv_cosf(1.0f)) != 0x3f0a5140u) return 0;
    if (f2u(wv_powf(0x1.666666p-1f, 0x1.99999ap-3f)) != 0x3f6e5fd7u) return 0;
    return 1;
}

/* ================================================================================================ */
/* anna_decoder API (MSTTSDecWrp access pattern)                                                    */

struct anna_decoder {
    FILE *csd;
    unsigned char key[129];
    unsigned char fmt[64];
    int block;
    long nblk;
    long long *pos; /* first sample of each block, [nblk] = total */
    wv_dec dec;
    unsigned char *in;
    int16_t *scratch;
    size_t scap;
};

anna_decoder *anna_decoder_open(const char *csd_path, const char *idx_path, const unsigned char key[129], char *err, size_t errlen)
{
    anna_decoder *d = calloc(1, sizeof *d);
    FILE *f = NULL;
    unsigned char hdr[72];
    long i, sz;
    if (!d) return NULL;
    if (!selftest()) {
        snprintf(err, errlen, "anna_dec_wmav.c was compiled with FMA contraction or excess precision");
        goto fail;
    }
    memcpy(d->key, key, 129);
    f = fopen(idx_path, "rb");
    if (!f || fread(hdr, 1, 72, f) != 72) {
        snprintf(err, errlen, "cannot read %s", idx_path);
        goto fail;
    }
    memcpy(d->fmt, hdr + 8, 64);
    /* only Anna's configuration is implemented: WMA Voice, mono 16 kHz, 450-byte packets, extradata flags
       0x1a99a7 (LSP order 16, residual LSPs, adaptive post-filter) */
    if ((d->fmt[0] | d->fmt[1] << 8) != 0x000a || (d->fmt[2] | d->fmt[3] << 8) != 1 || (d->fmt[4] | d->fmt[5] << 8) != 16000 ||
        (d->fmt[12] | d->fmt[13] << 8) != 450 || (d->fmt[16] | d->fmt[17] << 8) < 30 ||
        (d->fmt[36] | d->fmt[37] << 8 | (unsigned long)d->fmt[38] << 16 | (unsigned long)d->fmt[39] << 24) != 0x1a99a7ul) {
        snprintf(err, errlen, "unsupported voice format");
        goto fail;
    }
    d->block = 450;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    d->nblk = (sz - 72) / 2;
    fseek(f, 72, SEEK_SET);
    d->pos = malloc(sizeof *d->pos * (size_t)(d->nblk + 1));
    if (!d->pos) goto fail;
    d->pos[0] = 0;
    for (i = 0; i < d->nblk; i++) {
        unsigned char c[2];
        if (fread(c, 1, 2, f) != 2) goto fail;
        d->pos[i + 1] = d->pos[i] + ((c[0] | c[1] << 8) / 2);
    }
    fclose(f);
    f = NULL;
    d->csd = fopen(csd_path, "rb");
    if (!d->csd) {
        snprintf(err, errlen, "cannot open %s", csd_path);
        goto fail;
    }
    d->in = malloc((size_t)d->block);
    if (!d->in) goto fail;
    return d;
fail:
    if (f) fclose(f);
    anna_decoder_close(d);
    return NULL;
}

/* decode block k, append its PCM to the scratch buffer if keep; returns the sample count or -1 */
static long decode_block(anna_decoder *d, long k, int keep, size_t *have)
{
    int16_t pcm[WV_SFRAME * 64];
    int j, n;
    if (fseek(d->csd, k * d->block, SEEK_SET) != 0) return -1; /* the CSD is ~30 MB */
    if (fread(d->in, 1, (size_t)d->block, d->csd) != (size_t)d->block) return -1;
    for (j = 0; j < d->block; j++) d->in[j] ^= d->key[j % 129];
    n = decode_packet(&d->dec, d->in, d->block, pcm, (int)(sizeof pcm / sizeof pcm[0]));
    if (n < 0) return -1;
    if (keep) {
        size_t m = (size_t)n;
        if (*have + m > d->scap) {
            size_t c = d->scap ? d->scap : 1 << 16;
            int16_t *p;
            while (c < *have + m) c *= 2;
            p = realloc(d->scratch, c * sizeof *p);
            if (!p) return -1;
            d->scratch = p;
            d->scap = c;
        }
        memcpy(d->scratch + *have, pcm, m * 2);
        *have += m;
    }
    return n;
}

int anna_decoder_read(anna_decoder *d, long start, long n, int16_t *out)
{
    long lo = 0, hi = d->nblk, b, k;
    size_t have = 0;
    long long off;
    long i;
    if (n <= 0) return 0;
    while (hi - lo > 1) { /* last block with pos <= start */
        long m = (lo + hi) / 2;
        if (d->pos[m] <= start) lo = m;
        else hi = m;
    }
    b = lo;
    off = start - d->pos[b];
    codec_init(&d->dec, d->fmt); /* the DMO's Flush destroys its decoder; a new one is made */
    {   /* MSTTSDecWrp: pre-roll blocks pre..b-1 (discarded), then decode b, b+1, ... ; if any block comes
           out shorter than its IDX length, back up one more block and start over (no reset), <= 10 times */
        long pre = b, retries = 0;
        for (;;) {
            long bytes, skip0 = (long)off;
            int again = 0;
            for (k = pre > 0 ? pre : 0; k < b; k++)
                if (decode_block(d, k, 0, &have) < 0) return -1;
            have = 0;
            for (k = b; k < d->nblk && (long long)have < off + n; k++) {
                size_t before = have;
                long want = (long)(d->pos[k + 1] - d->pos[k]);
                bytes = decode_block(d, k, 1, &have);
                if (bytes < 0) return -1;
                if (bytes < want || bytes == 0 || bytes < skip0) {
                    have = before;
                    again = 1;
                    break;
                }
                skip0 = 0;
            }
            if (!again) break;
            pre--;
            if (++retries > 10) return -1;
            have = 0;
        }
    }
    for (i = 0; i < n; i++) out[i] = off + i < (long long)have ? d->scratch[off + i] : 0;
    return 0;
}

void anna_decoder_close(anna_decoder *d)
{
    if (!d) return;
    if (d->csd) fclose(d->csd);
    free(d->pos);
    free(d->in);
    free(d->scratch);
    free(d);
}
