/* zb_vocoder_int.c - CSpsVocoder fixed-point path (OneCore Zira / Mark): per LSF chunk prepare (FUN_18010c148: edge
 * copy FUN_1800a1db0, int LsfSharpen FUN_18003adb0, int LSF repair FUN_18006cc7c), CExcitationGeneratorImpl<int>
 * (FUN_18010e0d4: Q13 pulse FIR, Q13/Q14 noise), CLsfSynthesizerImpl<short,int> (FUN_18006c250: cos/exp tables, Q30
 * LSP->LPC with permuted pairs, int16 filter memory). Transcribed from tools/vocoder_model_int.py (verified bit-exact
 * against the engine) and the decompile. Excitation / filter state is continuous across chunks and reset per utterance. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "zb_internal.h"
#include "zb_voctab_int.h"

void zb_ilsf_repair(int32_t *lsf, int rows, int p);

static short f2s(float x) { return (short)(int)x; }

/* called from zb_vocoder_init (after the float tables exist) */
void zb_vocoder_int_init(ZbVoice *v)
{
    int k;
    float e = 0.0f, a;
    int i1;
    for (k = 0; k < 41; k++) v->hv_i[k] = f2s(8192.0f * v->hv[k]);
    for (k = 0; k < 1024; k++) {
        v->noise_i[k] = f2s(8192.0f * v->noise[k]);
        v->noise_uv_i[k] = f2s(16384.0f * v->noise_uv[k]);
    }
    for (k = 0; k < 1024; k++) e = e + v->noise[k] * v->noise[k];
    a = 1024.0f / e;
    i1 = (int)((a * 16384.0f) * 16384.0f);
    v->g40_i = (short)(int)sqrt((double)(i1 / 40));
    v->g80_i = (short)(int)sqrt((double)(i1 / 80));
}

void zb_vocoder_int_reset(ZbVoice *v)
{
    v->iexc_last = 0;
    v->iexc_prevf0 = 0;
    v->ihist_valid = 0;
    memset(v->ihist, 0, sizeof v->ihist);
    memset(v->isyn_hist, 0, sizeof v->isyn_hist);
    zb_srand(v, 0x406);
}

/* int LsfSharpen (FUN_18003af48 / FUN_18003b084 / FUN_18003b2c0), MultipleInterval */
static void isharpen(const ZbConfig *c, int32_t *x, int p)
{
    const int st = f2s(c->sh_step * 32768.0f), lo = f2s(c->sh_low * 32768.0f), hi = f2s(c->sh_high * 32768.0f);
    static const float gapbase[4] = {0.28f, 0.7f, 1.2f, 1.8f};
    int d = c->sh_intervals, it, i, j, k;
    int acc[64], t[64];
    for (it = 0; it < c->sh_iters; it++) {
        if (d == 0) d = 1;
        memset(acc, 0, sizeof(int) * p);
        for (i = d; i < p - d; i++) {
            int f = x[i], w, fl, fr, cc, delta;
            if (lo < f) {
                w = st;
                if (hi <= f) w = st - ((f - hi) * st) / (0x4000 - hi);
            } else
                w = (f * st) / lo;
            fl = f - x[i - d];
            fr = x[i + d] - f;
            if (fl == 0) cc = 0;
            else {
                int den = (fr * fr) / fl + fl;
                cc = den == 0 ? 0 : (w * fr) / den;
            }
            delta = ((fl - fr) * cc) / 0x8000;
            acc[i] += delta;
            for (j = 1; j < d; j++) {
                acc[i - j] += fl != 0 ? ((f - x[i - j]) * delta) / fl : 0;
                acc[i + j] += fr != 0 ? ((x[i + j] - f) * delta) / fr : 0;
            }
        }
        for (k = 1; k < p - 1; k++) {
            int den = k;
            if (p - 1 - k < den) den = p - 1 - k;
            if (2 * d - 1 < den) den = 2 * d - 1;
            t[k] = acc[k] / den + x[k];
        }
        for (k = 1; k < p - 1; k++) x[k] = t[k];
        d--;
    }
    for (d = c->sh_intervals - 1; d > 0; d--) {
        int g = (int)(gapbase[d - 1] * 16384.0f) / p;
        for (i = 0; i < p - d; i++) {
            int gap = x[i + d] - x[i];
            if (gap < g) {
                int adj = (g - gap) / 2;
                x[i + d] += adj;
                x[i] -= adj;
            }
        }
    }
}

/* FUN_18010e0d4 without MBE: pulses (int Hz), Q13 voiced FIR with 41-mark history, Q13/Q14 noise -> Q14 excitation */
static void iexcitation(ZbVoice *v, const int32_t *f0, int nfr, int n, int shift, int32_t *out)
{
    unsigned char *mark = (unsigned char *)calloc((size_t)n + 1, 1);
    int i, s = 0, m, k;
    const int g = shift == 40 ? v->g40_i : v->g80_i;
    for (i = 0; i < nfr && s < n; i++, s += shift) {
        int f = f0[i];
        if ((float)f <= 3.0f) f = 0;
        else {
            unsigned T;
            if ((float)v->iexc_prevf0 <= 3.0f) { mark[s] = 1; v->iexc_last = s; }
            T = (unsigned)v->rate / (unsigned)f;
            if (T) {
                for (m = s; m < s + shift; m++) {
                    unsigned kk = (unsigned)(m - v->iexc_last) / T, q;
                    if (kk >= 1) {
                        int step = (int)T, base = v->iexc_last;
                        for (q = 0; q < kk; q++) {
                            int pos = step + base;
                            if (pos < 0) mark[0] = 1;
                            else if (pos < n) mark[pos] = 1;
                            step += (int)T;
                            v->iexc_last = m;
                            base = m;
                        }
                    }
                }
            }
        }
        v->iexc_prevf0 = f;
    }
    v->iexc_last -= n;
    for (i = 0; i < n; i++) out[i] = 0;
    if (v->ihist_valid)
        for (i = 0; i < 41; i++)
            if (v->ihist[i])
                for (k = 0; k < 41; k++)
                    if (i + k >= 41 && i + k - 41 < n) out[i + k - 41] += v->hv_i[k];
    for (m = 0; m < n; m++)
        if (mark[m])
            for (k = 0; k < 41 && m + k < n; k++) out[m + k] += v->hv_i[k];
    if (n >= 41) memcpy(v->ihist, mark + n - 41, 41);
    else {
        unsigned char h[41];
        memcpy(h, v->ihist + n, (size_t)(41 - n));
        memcpy(h + 41 - n, mark, (size_t)n);
        memcpy(v->ihist, h, 41);
    }
    v->ihist_valid = 1;
    for (i = 0; i < nfr && (i + 1) * shift <= n; i++) {
        int r = zb_rand(v), off = shift + r % (1024 - 2 * shift);
        int32_t *o = out + i * shift;
        if ((float)f0[i] <= 3.0f)
            for (k = 0; k < shift; k++) o[k] = (g * v->noise_i[off + k]) / 0x2000;
        else
            for (k = 0; k < shift; k++) o[k] = (g * v->noise_uv_i[off + k]) / 0x4000 + o[k] * 2;
    }
    free(mark);
}

/* FUN_18003b460: Q15 cosines -> int LPC (Q30 polynomials, LSP pairs consumed in a permuted order) */
static void ilsp2lpc(const int *c, int p, int *a)
{
    int64_t P[34], Q[34], P2[34], Q2[34];
    int n = p / 2, i, j;
    P[0] = (int64_t)1 << 30; P[1] = (int64_t)c[0] * -65536;
    Q[0] = (int64_t)1 << 30; Q[1] = (int64_t)c[1] * -65536;
    for (i = 1; i < n; i++) {
        int idx = (i % 2 == 0) ? 2 * i : (n % 2 == 0 ? 2 * (n - i) : 2 * (n - i) - 2);
        int64_t d = (int64_t)c[idx] * -2, e = (int64_t)c[idx + 1] * -2;
        P2[0] = Q2[0] = (int64_t)1 << 30;
        P2[i + 1] = ((d * P[i]) >> 15) + P[i - 1] * 2;
        Q2[i + 1] = ((e * Q[i]) >> 15) + Q[i - 1] * 2;
        for (j = i; j > 1; j--) {
            P2[j] = ((d * P[j - 1]) >> 15) + P[j - 2] + P[j];
            Q2[j] = ((e * Q[j - 1]) >> 15) + Q[j - 2] + Q[j];
        }
        P2[1] = ((d * P[0]) >> 15) + P[1];
        Q2[1] = ((e * Q[0]) >> 15) + Q[1];
        for (j = 0; j <= i + 1; j++) { P[j] = P2[j]; Q[j] = Q2[j]; }
    }
    for (j = 1; j <= n; j++) {
        int64_t s = (Q[j] - Q[j - 1]) + P[j] + P[j - 1], t = (P[j] + P[j - 1]) - (Q[j] - Q[j - 1]);
        a[j - 1] = (int)(s / 32768);
        a[p - j] = (int)(t / 32768);
    }
}

static short softclip(float y)
{
    double x = fabs((double)y);
    if (x > 31000.0) {
        float r = (float)(32767.0 - exp((x - 31000.0) * -1.0 * 0.0005000000237487257) * 1767.0);
        if (!(y > 0.0f)) r = r * -1.0f;
        return (short)(int)r;
    }
    return (short)(int)y;
}

/* FUN_18006c250 */
static void isynth(ZbVoice *v, const int32_t *lsf, int p, const int32_t *gain, const int32_t *exc, int nfr, int shift, short *out)
{
    short *B = (short *)malloc(sizeof(short) * (size_t)(p + shift));
    int c[64], a[64];
    int u, n, k;
    memcpy(B, v->isyn_hist, sizeof(short) * p);
    for (u = 0; u < nfr; u++) {
        int G = gain[u], g = 0, tries = 0, gi;
        if (G != 0) {
            gi = (G + 0x80) / 0x100;
            if (gi > 1330) gi = 1330;
            g = zb_exp1331[gi];
        }
        for (k = 0; k < p; k++) {
            int ci = lsf[u * p + k] / 8;
            c[k] = zb_cos2048[ci > 2047 ? 2047 : ci];
        }
        ilsp2lpc(c, p, a);
        for (;;) {
            int bad = 0;
            tries++;
            for (n = 0; n < shift; n++) {
                int64_t acc = (int64_t)exc[u * shift + n] * g * 4;
                int y;
                for (k = 0; k < p; k++) acc += -(int64_t)B[n + p - 1 - k] * a[k];
                acc >>= 16;
                y = acc < -0x8000 ? -0x8000 : (acc > 0x7fff ? 0x7fff : (int)acc);
                B[n + p] = (short)y;
                if (tries < 5 && (y > 31000 || y < -31000)) { bad = 1; break; }
                out[u * shift + n] = softclip((float)y);
            }
            if (!bad) break;
            {
                float fac = 0.99f, mm = 0.99f;
                for (k = 0; k < p; k++) { a[k] = (int)((float)a[k] * fac); fac = fac * mm; }
            }
        }
        memmove(B, B + shift, sizeof(short) * p);
    }
    memcpy(v->isyn_hist, B, sizeof(short) * p);
    free(B);
}

/* one LSF chunk: phones [ph0, ph0+nph), frames [T0, T0+nfr) of the utterance matrices */
void zb_vocoder_int_chunk(ZbVoice *v, const ZbUtt *u, const int *phfr, const int *blk, int nblk, int first, int last,
                          const int32_t *f0, int32_t *lsf, int32_t *gain, int T0, int nfr, short *pcm)
{
    const int p = 24, shift = v->shift, head = first ? 10 : 0, tail = last ? 10 : 0;
    int32_t *L = lsf + (size_t)T0 * p, *exc;
    int r, b, ph = 0, f = 0;
    (void)u;
    if (nfr <= 0) return;
    /* FUN_1800a1db0 */
    if (head + tail < nfr) {
        for (r = 0; r < head; r++) memcpy(L + r * p, L + head * p, sizeof(int32_t) * p);
        for (r = nfr - tail; r < nfr; r++) memcpy(L + r * p, L + (nfr - tail - 1) * p, sizeof(int32_t) * p);
    } else {
        memset(L, 0, sizeof(int32_t) * (size_t)nfr * p);
        memset(gain + T0, 0, sizeof(int32_t) * (size_t)nfr);
    }
    for (r = 0; r < nfr; r++) isharpen(&v->cfg, L + r * p, p);
    zb_ilsf_repair(L, nfr, p);
    exc = (int32_t *)malloc(sizeof(int32_t) * (size_t)nfr * shift);
    for (b = 0; b < nblk; b++) {   /* synthesis blocks of the chunk (FUN_180074968) */
        int nf = 0, k;
        for (k = 0; k < blk[b]; k++) nf += phfr[ph + k];
        ph += blk[b];
        iexcitation(v, f0 + T0 + f, nf, nf * shift, shift, exc + (size_t)f * shift);
        isynth(v, L + (size_t)f * p, p, gain + T0 + f, exc + (size_t)f * shift, nf, shift, pcm + (size_t)(T0 + f) * shift);
        f += nf;
    }
    free(exc);
}
