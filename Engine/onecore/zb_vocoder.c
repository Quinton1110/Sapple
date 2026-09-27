/* zb_vocoder.c - CSpsVocoder float path (David): prepare (edge copy, LsfSharpen, LSF repair), excitation
 * (CExcitationGeneratorImpl<float>), LSF->LPC + all-pole synthesis (CLsfSynthesizerImpl<float,float>), silence
 * zeroing, volume. Transcribed from notes/backend_vocoder.md and tools/vocoder_model.py (verified bit-exact).
 * The engine processes an utterance in blocks of phones; all vocoder state (pulse position, FIR history,
 * filter memory, rand) is continuous across blocks, so one block per utterance gives identical samples. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "zb_internal.h"
#include "zb_voctab.h"

static const double ZB_PI = 3.141592653589793;

/* FUN_18000b1f0: "valid" FIR, products in float accumulated in double, first taps-1 outputs 0 */
static void fir_valid(const float *h, const float *x, float *out, int n)
{
    int i, k;
    for (i = 0; i < 40 && i < n; i++) out[i] = 0.0f;
    for (i = 40; i < n; i++) {
        double acc = 0.0;
        for (k = 0; k < 41; k++) acc = acc + (double)(x[i - k] * h[k]);
        out[i] = (float)acc;
    }
}

/* silence zeroing (FUN_180009b14 / FUN_180074968): silence phones are zeroed from state s0 to s1 inclusive; with
 * SilenceNoiseSuppress = 0 an inner pause keeps its first and last two states (s0 = s1 = 2), an edge silence is zeroed
 * up to the neighbouring silence/utterance edge */
void zb_silence_zero(const ZbVoice *v, const ZbUtt *u, const int *dur, int nstate, short *pcm)
{
    const int shift = v->shift;
    int ph, off;
    for (ph = 0, off = 0; ph < u->nphone; ph++) {
        const int *d = dur + ph * nstate;
        int s, tot = 0;
        for (s = 0; s < nstate; s++) tot += d[s];
        if (u->ph[ph].c.is_sil) {
            int s0 = nstate / 2, s1 = nstate / 2, a = 0, b = 0;
            if (v->cfg.silence_noise_suppress) { s0 = 0; s1 = nstate - 1; }
            else {
                if (ph == 0 || u->ph[ph - 1].c.is_sil) s0 = 0;
                if (ph + 1 >= u->nphone || u->ph[ph + 1].c.is_sil) s1 = nstate - 1;
            }
            for (s = 0; s < s0; s++) a += d[s];
            for (s = s0; s <= s1; s++) b += d[s];
            memset(pcm + (size_t)(off + a) * shift, 0, sizeof(short) * (size_t)b * shift);
        }
        off += tot;
    }
}

void zb_vocoder_init(ZbVoice *v)
{
    const double *w = v->rate == 16000 ? zb_band_w16k : zb_band_w8k;
    int k, b;
    float e = 0.0f, a;
    for (k = 0; k < 41; k++) {
        double dv = 0.0, du = 0.0;
        for (b = 0; b < 5; b++) {
            dv = dv + w[b] * zb_band41[b][k];
            du = du + (1.0 - w[b]) * zb_band41[b][k];
        }
        v->hv[k] = (float)dv;
        v->hu[k] = (float)du;
    }
    for (k = 0; k < 1024; k++) memcpy(&v->noise[k], &zb_noise1024[k], 4);
    for (k = 0; k < 1024; k++) e = e + v->noise[k] * v->noise[k];
    a = 1024.0f / e;
    v->g40 = (float)sqrt((double)(a / 40.0f));
    v->g80 = (float)sqrt((double)(a / 80.0f));
    fir_valid(v->hu, v->noise, v->noise_uv, 1024);
    zb_vocoder_int_init(v);
}

/* ---------------------------------------------------------------- LsfSharpen (FUN_180012408, multi-interval) */
static const float mingap_base[4] = {0.28f, 0.7f, 1.2f, 1.8f};

static void sharpen_pass(float *x, int p, int d, float step, float low, float high)
{
    float acc[64], out[64];
    int i, j, k;
    memset(acc, 0, sizeof(float) * p);
    for (i = d; i < p - d; i++) {
        float f = x[i], w, fl, fr, c = 0.0f, delta;
        if (f <= low) w = ((f - 0.0f) * step) / (low - 0.0f);
        else {
            w = step;
            if (high <= f) w = step - ((f - high) * step) / (0.5f - high);
        }
        fl = f - x[i - d];
        fr = x[i + d] - f;
        if (fl != 0.0f) {
            float den = (fr * fr) / fl + fl;
            if (den != 0.0f) c = (w * fr) / den;
        }
        delta = (fl - fr) * c;
        acc[i] = delta + acc[i];
        for (j = 1; j < d; j++) {
            float t = 0.0f;
            if (fl != 0.0f) acc[i - j] = ((f - x[i - j]) * delta) / fl + acc[i - j];
            else acc[i - j] = acc[i - j] + 0.0f;
            if (fr != 0.0f) t = ((x[i + j] - f) * delta) / fr;
            acc[i + j] = t + acc[i + j];
        }
    }
    for (k = 1; k < p - 1; k++) {
        int den = k;
        if (p - 1 - k < den) den = p - 1 - k;
        if (2 * d - 1 < den) den = 2 * d - 1;
        out[k] = acc[k] / (float)den + x[k];
    }
    for (k = 1; k < p - 1; k++) x[k] = out[k];
}

static void lsf_sharpen(const ZbConfig *c, float *x, int p)
{
    int it, d = c->sh_intervals, dd;
    for (it = 0; it < c->sh_iters; it++) {
        if (d == 0) d = 1;
        sharpen_pass(x, p, d, c->sh_step, c->sh_low, c->sh_high);
        d--;
    }
    for (dd = c->sh_intervals; dd - 1 > 0; dd--) {
        int i;
        float g;
        d = dd - 1;
        g = (mingap_base[dd - 2] * 0.5f) / (float)p;
        for (i = 0; i < p - d; i++) {
            float gap = x[i + d] - x[i];
            if (gap < g) {
                float adj = (g - gap) * 0.5f;
                x[i + d] = x[i + d] + adj;
                x[i] = x[i] - adj;
            }
        }
    }
}

/* ---------------------------------------------------------------- LSF repair (FUN_18006bfec) */
static void linfill(float *x, unsigned n, float a, float b)
{
    unsigned i;
    for (i = 0; i < n; i++) x[i] = (float)(i + 1) * ((b - a) / (float)(n + 1)) + a;
}
static void lsf_respace(float *x, unsigned n)  /* FUN_18006c18c */
{
    unsigned i = 0;
    float hi = 0.5f;
    while (i < n && !(0.0f < x[i] && x[i] < 0.5f)) i++;
    if (i != 0) linfill(x, i, 0.0f, i < n ? x[i] : 0.5f);
    if (i < n) {
        for (; i < n; i++) {
            if (x[i] <= 0.0f) break;
            if (hi <= x[i]) break;
        }
        if (i < n) linfill(x + i, n - i, x[(int)i - 1], hi);
    }
}
void zb_lsf_repair(float *lsf, int rows, int p)
{
    int r, k;
    for (r = 0; r < rows; r++) {
        float *x = lsf + (size_t)r * p;
        int sorted = 1, bad = 0;
        for (k = 1; k < p; k++)
            if (x[k] <= x[k - 1] && x[k - 1] != x[k]) { sorted = 0; break; }
        if (!sorted) {  /* FUN_18006c12c bubble sort */
            int swapped;
            do {
                swapped = 0;
                for (k = 1; k < p; k++)
                    if (x[k] < x[k - 1]) { float t = x[k - 1]; x[k - 1] = x[k]; x[k] = t; swapped = 1; }
            } while (swapped);
        }
        for (k = 0; k < p; k++)
            if (x[k] < 0.0f || 0.5f < x[k]) { bad = 1; break; }
        if (bad) {
            if (r) memcpy(x, x - p, sizeof(float) * p);
            else lsf_respace(x, (unsigned)p);
        }
    }
}

/* ---------------------------------------------------------------- excitation */
typedef struct {
    int last;
    float prevf0;
    int hist_valid;
    unsigned char hist[41];     /* pulse marks of the last 41 samples of the previous block */
} ZbExcState;

static void excitation(ZbVoice *v, ZbExcState *st, const float *f0, int nfr, int n, int shift, float *out)
{
    int *mark = (int *)calloc((size_t)n + 1, sizeof(int));
    int i, s = 0, m, k;
    float g = shift == 40 ? v->g40 : v->g80;
    /* pulse train FUN_18002ce50 */
    for (i = 0; i < nfr && s < n; i++, s += shift) {
        float f = f0[i];
        if (f <= 3.0f) f = 0.0f;
        else {
            int T;
            if (st->prevf0 <= 3.0f) { mark[s] = 1; st->last = s; }
            T = (int)((float)v->rate / f);
            if (T) {
                for (m = s; m < s + shift; m++) {
                    unsigned kk = (unsigned)(m - st->last) / (unsigned)T, q;
                    if (kk >= 1) {
                        int step = T, base = st->last;
                        for (q = 0; q < kk; q++) {
                            int pos = step + base;
                            if (pos < 0) mark[0] = 1;
                            else if (pos < n) mark[pos] = 1;
                            step += T;
                            st->last = m;
                            base = m;
                        }
                    }
                }
            }
        }
        st->prevf0 = f;
    }
    st->last -= n;
    /* voiced FIR FUN_1800090e0: tails of the previous block's last 41 marks, then this block's marks */
    for (i = 0; i < n; i++) out[i] = 0.0f;
    if (st->hist_valid)
        for (i = 0; i < 41; i++)
            if (st->hist[i])
                for (k = 0; k < 41; k++)
                    if (i + k >= 41 && i + k - 41 < n) out[i + k - 41] = v->hv[k] + out[i + k - 41];
    for (m = 0; m < n; m++)
        if (mark[m])
            for (k = 0; k < 41 && m + k < n; k++) out[m + k] = v->hv[k] + out[m + k];
    {
        unsigned char h[41];
        for (k = 0; k < 41; k++) {
            int src = n - 41 + k;
            h[k] = (unsigned char)(src >= 0 ? mark[src] != 0 : st->hist[src + 41]);
        }
        memcpy(st->hist, h, 41);
        st->hist_valid = 1;
    }
    /* noise FUN_18000adc0 */
    for (i = 0; i < nfr && (i + 1) * shift <= n; i++) {
        int r = zb_rand(v), off = shift + r % (1024 - 2 * shift);
        float *o = out + i * shift;
        if (f0[i] > 3.0f)
            for (k = 0; k < shift; k++) o[k] = v->noise_uv[off + k] * g + o[k];
        else
            for (k = 0; k < shift; k++) o[k] = g * v->noise[off + k];
    }
    free(mark);
}

/* ---------------------------------------------------------------- five-band excitation (Microsoft Sarah)
 * ClassicVoices patch: the float excitation with [MultiBandExcitation] (FUN_1800094f8, taken instead of FUN_18000aaa8
 * when the MBE matrix is present) [V disasm, via the Win11 build's FUN_18003ec70 / FUN_180105d14 / FUN_180105260]:
 * the same pulse train, noise table, gains and rand() stream; the voiced / unvoiced 41-tap filters are rebuilt EVERY
 * FRAME from the frame's five band strengths (bands = zb_band41, 0-2 / 2-3 / 3-4 / 4-6 / 6-8 kHz):
 *     hv[k] = (float)sum_b v_b * band[b][k],  hu[k] = (float)sum_b (1 - v_b) * band[b][k]   (double sums)
 * the pulse FIR runs frame by frame (FUN_180008e40: a pulse's tail in the next frame is filtered with that frame's hv),
 * a voiced frame adds g * (hu filtered over 120 fresh noise samples at rand() % (1024 - shift - 40)), an unvoiced
 * frame is g * noise[rand() % (1024 - shift) ...] (no "+ shift" here, unlike the plain path). */
static void mbe_taps(const float *vb, float *hv, float *hu)
{
    int k, b;
    for (k = 0; k < 41; k++) {
        double dv = 0.0, du = 0.0;
        for (b = 0; b < 5; b++) {
            double w = (double)vb[b];
            dv = dv + w * zb_band41[b][k];
            du = du + (1.0 - w) * zb_band41[b][k];
        }
        hv[k] = (float)dv;
        hu[k] = (float)du;
    }
}

static void excitation_mbe(ZbVoice *v, ZbExcState *st, const float *f0, const float *mbe, int nb, int nfr, int n, int shift,
                           float *out)
{
    int *mark = (int *)calloc((size_t)n + 1, sizeof(int));
    int i, s = 0, m, k;
    float g = shift == 40 ? v->g40 : shift == 80 ? v->g80 : 0.0f, hv[41], hu[41], vb[5];
    unsigned char prev[41];
    /* pulse train FUN_18002ce50 (as excitation()) */
    for (i = 0; i < nfr && s < n; i++, s += shift) {
        float f = f0[i];
        if (f <= 3.0f) f = 0.0f;
        else {
            int T;
            if (st->prevf0 <= 3.0f) { mark[s] = 1; st->last = s; }
            T = (int)((float)v->rate / f);
            if (T) {
                for (m = s; m < s + shift; m++) {
                    unsigned kk = (unsigned)(m - st->last) / (unsigned)T, q;
                    if (kk >= 1) {
                        int step = T, base = st->last;
                        for (q = 0; q < kk; q++) {
                            int pos = step + base;
                            if (pos < 0) mark[0] = 1;
                            else if (pos < n) mark[pos] = 1;
                            step += T;
                            st->last = m;
                            base = m;
                        }
                    }
                }
            }
        }
        st->prevf0 = f;
    }
    st->last -= n;
    /* per-frame pulse FIR: history = the previous frame's last 41 marks (first frame: the previous block's) */
    for (k = 0; k < 41; k++) prev[k] = st->hist_valid ? st->hist[k] : 0;
    for (i = 0; (i + 1) * shift <= n; i++) {
        float *o = out + i * shift;
        const int *mk = mark + i * shift;
        int b;
        for (b = 0; b < 5; b++) vb[b] = (i < nfr && b < nb) ? mbe[(size_t)i * nb + b] : 0.0f;
        mbe_taps(vb, hv, hu);
        for (k = 0; k < shift; k++) o[k] = 0.0f;
        for (m = 0; m < 41; m++)
            if (prev[m])
                for (k = 0; k < 41; k++)
                    if (m + k >= 41 && m + k - 41 < shift) o[m + k - 41] = hv[k] + o[m + k - 41];
        for (m = 0; m < shift; m++)
            if (mk[m])
                for (k = 0; k < 41 && m + k < shift; k++) o[m + k] = hv[k] + o[m + k];
        for (k = 0; k < 41; k++) {
            int src = shift - 41 + k;
            prev[k] = (unsigned char)(src >= 0 ? mk[src] != 0 : prev[src + 41]);
        }
        /* noise FUN_180105260 */
        {
            int r = zb_rand(v);
            if (i < nfr && f0[i] > 3.0f) {
                const float *x = v->noise + r % (1024 - (shift + 40));
                for (m = 0; m < shift; m++) {
                    double acc = 0.0;
                    for (k = 0; k < 41; k++) acc = acc + (double)(x[m + 40 - k] * hu[k]);
                    o[m] = g * (float)acc + o[m];
                }
            } else {
                int off = r % (1024 - shift);
                for (m = 0; m < shift; m++) o[m] = g * v->noise[off + m];
            }
        }
    }
    memcpy(st->hist, prev, 41);
    st->hist_valid = 1;
    free(mark);
}

/* ---------------------------------------------------------------- LSP -> LPC (FUN_180014640) */
static void lsp2lpc(const float *c, int p, float *a)
{
    double P[34], Q[34], P2[34], Q2[34];
    int n = p / 2, i, j;
    P[0] = 1.0; P[1] = (double)(c[0] * -2.0f);
    Q[0] = 1.0; Q[1] = (double)(c[1] * -2.0f);
    for (i = 1; i < n; i++) {
        double d = (double)(c[2 * i] * -2.0f), e = (double)(c[2 * i + 1] * -2.0f);
        P2[0] = 1.0; Q2[0] = 1.0;
        P2[i + 1] = d * P[i] + (P[i - 1] + P[i - 1]);
        Q2[i + 1] = e * Q[i] + (Q[i - 1] + Q[i - 1]);
        for (j = i; j > 1; j--) {
            P2[j] = d * P[j - 1] + P[j] + P[j - 2];
            Q2[j] = e * Q[j - 1] + Q[j] + Q[j - 2];
        }
        P2[1] = d * P[0] + P[1];
        Q2[1] = e * Q[0] + Q[1];
        for (j = 0; j <= i + 1; j++) { P[j] = P2[j]; Q[j] = Q2[j]; }
    }
    j = 1;
    while (j <= n - 1) {
        int jj;
        for (jj = j; jj <= j + 1; jj++) {
            double sp = P[jj] + P[jj - 1], dq = Q[jj] - Q[jj - 1];
            a[jj - 1] = (float)((dq + sp) * 0.5);
            a[p - jj] = (float)((sp - dq) * 0.5);
        }
        j += 2;
    }
    if (j <= n) {
        double dq = Q[j] - Q[j - 1];
        a[j - 1] = (float)(((dq + P[j]) + P[j - 1]) * 0.5);
        a[p - j] = (float)(((P[j] + P[j - 1]) - dq) * 0.5);
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

/* FUN_18006b620 without spectrum warp / power normalisation; hist[p] = filter memory */
static void synth(const float *lsf, int p, const float *gain, const float *exc, int nfr, int shift, float *hist, short *out)
{
    float *B = (float *)malloc(sizeof(float) * (size_t)(p + shift));
    float c[64], a[64];
    int u, n, k;
    memcpy(B, hist, sizeof(float) * p);
    for (u = 0; u < nfr; u++) {
        float gv = gain[u], g = gv != 0.0f ? (float)exp((double)gv) : 0.0f;
        int tries = 0;
        for (k = 0; k < p; k++) {
            double x = (double)lsf[u * p + k];
            c[k] = (float)cos(x * ZB_PI + x * ZB_PI);
        }
        lsp2lpc(c, p, a);
        for (;;) {
            int bad = 0;
            tries++;
            for (n = 0; n < shift; n++) {
                float acc = g * exc[u * shift + n];
                for (k = 0; k < p; k++) acc = acc - B[n + p - 1 - k] * a[k];
                B[n + p] = acc;
                if (tries < 5 && (acc > 31000.0f || acc < -31000.0f)) { bad = 1; break; }
                out[u * shift + n] = softclip(acc);
            }
            if (!bad) break;
            {
                float fac = 0.99f, mm = 0.99f;
                for (k = 0; k < p; k++) { a[k] = fac * a[k]; fac = fac * mm; }
            }
        }
        memmove(B, B + shift, sizeof(float) * p);
    }
    memcpy(hist, B, sizeof(float) * p);
    free(B);
}

/* FUN_18002c420 */
void zb_scale_pcm(short *s, long n, float g)
{
    long i;
    for (i = 0; i < n; i++) {
        float x = (float)(int)s[i] * g, y = -31000.0f;
        if (-31000.0f <= x) y = x;
        if (31000.0f <= y) y = 31000.0f;
        s[i] = (short)(int)y;
    }
}

/* FUN_1801487ec: SAPI volume curve (-25 dB range) */
float zb_sapi_vol_gain(float x)
{
    if (0.1f <= x) return (float)pow(10.0, (double)((1.0f - x) * -25.0f) / 20.0);
    return (x / 0.1f) * (float)pow(10.0, (double)((1.0f - 0.1f) * -25.0f) / 20.0);
}

/* FUN_180009b14 / FUN_180074968 block grouping inside one vocoder chunk (phones [ph0, ph0+nph)): consecutive phones
 * with the same waveform-stretch flag and XML rate, at most cap frames (the longest phone of the utterance = output
 * buffer), and the first block of a chunk holds at most 2 phones. blk[] receives the phone counts; returns #blocks. */
int zb_split_blocks(const ZbUtt *u, const int *phfr, int ph0, int nph, const int *stretch, int cap, int *blk)
{
    int i = ph0, end = ph0 + nph, nb = 0, first = 1;
    while (i < end) {
        int count = 0, frames = 0, flag = stretch ? stretch[i] : 0, xr = u->ph[i].c.rate;
        for (;;) {
            if (frames + phfr[i] > cap && count) break;
            frames += phfr[i];
            count++;
            i++;
            if (i >= end) break;
            if ((stretch ? stretch[i] : 0) != flag || u->ph[i].c.rate != xr) break;
            if (first && count > 1) break;
        }
        blk[nb++] = count;
        first = 0;
    }
    return nb;
}

void zb_vocoder_float(ZbVoice *v, const ZbUtt *u, const int *dur, int nstate, int T, const float *f0, float *lsf, const float *gain,
                      const int *stretch, short *pcm)
{
    zb_vocoder_float_p(v, u, dur, nstate, T, f0, lsf, gain, stretch, pcm, 24);
}

void zb_vocoder_float_p(ZbVoice *v, const ZbUtt *u, const int *dur, int nstate, int T, const float *f0, float *lsf,
                        const float *gain, const int *stretch, short *pcm, int p)
{
    zb_vocoder_float_mbe(v, u, dur, nstate, T, f0, lsf, gain, stretch, pcm, p, NULL, 0);
}

/* ClassicVoices patch (Microsoft Sarah): mbe = [T][nb] band strengths in 0..1 (nb <= 5), NULL = the plain excitation */
void zb_vocoder_float_mbe(ZbVoice *v, const ZbUtt *u, const int *dur, int nstate, int T, const float *f0, float *lsf,
                          const float *gain, const int *stretch, short *pcm, int p, const float *mbe, int nbands)
{
    const ZbConfig *cfg = &v->cfg;
    const int shift = v->shift, n = T * shift;
    float *exc, hist[64];
    ZbExcState st;
    int r, *phfr, *blk, nb, b, ph, cap = 0, f = 0;
    /* prepare FUN_18010ca5c: whole utterance in one chunk: head = tail = 10 */
    if (20 < T) {
        for (r = 0; r < 10; r++) memcpy(lsf + r * p, lsf + 10 * p, sizeof(float) * p);
        for (r = T - 10; r < T; r++) memcpy(lsf + r * p, lsf + (T - 11) * p, sizeof(float) * p);
    } else {
        memset(lsf, 0, sizeof(float) * (size_t)T * p);
        memset((float *)gain, 0, sizeof(float) * (size_t)T);
    }
    for (r = 0; r < T; r++) lsf_sharpen(cfg, lsf + r * p, p);
    zb_lsf_repair(lsf, T, p);
    /* excitation + synthesis */
    exc = (float *)malloc(sizeof(float) * (size_t)(n ? n : 1));
    memset(&st, 0, sizeof st);
    memset(hist, 0, sizeof hist);
    phfr = (int *)calloc((size_t)u->nphone + 1, sizeof(int));
    blk = (int *)calloc((size_t)u->nphone + 1, sizeof(int));
    for (ph = 0; ph < u->nphone; ph++) {
        for (r = 0; r < nstate; r++) phfr[ph] += dur[ph * nstate + r];
        if (cap < phfr[ph]) cap = phfr[ph];
    }
    nb = zb_split_blocks(u, phfr, 0, u->nphone, stretch, cap, blk);
    for (b = 0, ph = 0; b < nb; b++) {
        int nf = 0, k;
        for (k = 0; k < blk[b]; k++) nf += phfr[ph + k];
        ph += blk[b];
        if (mbe) excitation_mbe(v, &st, f0 + f, mbe + (size_t)f * nbands, nbands, nf, nf * shift, shift, exc + (size_t)f * shift);
        else excitation(v, &st, f0 + f, nf, nf * shift, shift, exc + (size_t)f * shift);
        synth(lsf + (size_t)f * p, p, gain + f, exc + (size_t)f * shift, nf, shift, hist, pcm + (size_t)f * shift);
        f += nf;
    }
    free(phfr);
    free(blk);
    free(exc);
    zb_silence_zero(v, u, dur, nstate, pcm);
    /* voice volume (FUN_18001fedc: SpsVolumeAdjustment when the voice has NUS domain data) */
    /* voice volume and SAPI volume: zb_wave_sps() (after the waveform rate stage) */

}

/* per-phone SAPI volume (FUN_18006d71c), applied segment by segment */
void zb_apply_sapi_volume(const ZbUtt *u, const int *dur, int nstate, int shift, int sitevol, short *pcm)
{
    int ph, off = 0, s;
    for (ph = 0; ph < u->nphone; ph++) {
        int tot = 0;
        for (s = 0; s < nstate; s++) tot += dur[ph * nstate + s];
        if ((float)u->ph[ph].c.volume != 100.0f || sitevol != 100) {
            float x = (((float)sitevol * (float)u->ph[ph].c.volume) / 100.0f) / 100.0f;
            zb_scale_pcm(pcm + off * shift, (long)tot * shift, zb_sapi_vol_gain(x));
        }
        off += tot;
    }
}
