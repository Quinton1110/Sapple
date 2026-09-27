/* zb_ratechg.c - CRateChanger (vtbl 0x180171280): SOLA time-scale modification in double precision, used by the
 * wave generator for voices without VoiceSetting.WaveScale=Sonic (OneCore Zira, Mark).
 * Parameters (FUN_180052298, scaled by sr/8000): frame N = 2*(120+100) = 440 samples at 16 kHz, synthesis hop
 * L = 160, overlap V = 80, search range K = 200, analysis hop Sa = (int)(160*speed).
 * process = FUN_180025490, fill = FUN_18002cff0, first frame = FUN_18015d81c, lag search = FUN_1800259d0 (maximises
 * c^2/E over lags 0..K with c > 0, c = <frame[lag..lag+V), overlap>, E = energy of frame[lag..lag+V)), final frame =
 * FUN_18015d860, setSpeed = FUN_180153860 (a changed speed resets the stream), flush = FUN_180153690. */
#include <stdlib.h>
#include <string.h>
#include "zb_ratechg.h"

struct ZbRateChg {
    int N, L, K, V, p1, Sa, off;
    double speed;
    double *ov;         /* overlap tail of the previous output frame (V samples) */
    double *work;       /* current analysis frame (N) */
    short *left;        /* unconsumed input (N) */
    int nleft, started;
};

ZbRateChg *zb_rc_create(int sr)
{
    ZbRateChg *r = (ZbRateChg *)calloc(1, sizeof *r);
    double f = (double)sr / 8000.0;
    int p0 = (int)(120.0 * f), p2;
    r->p1 = (int)(80.0 * f);
    p2 = (int)(80.0 * f);
    r->K = (int)(100.0 * f);
    r->N = r->K + p0;
    r->L = p2;
    r->V = p0 - p2;
    r->speed = 1.0;
    r->Sa = (int)((double)r->p1 * r->speed);
    r->ov = (double *)calloc((size_t)r->N, sizeof(double));
    r->work = (double *)calloc((size_t)r->N, sizeof(double));
    r->left = (short *)calloc((size_t)r->N, sizeof(short));
    return r;
}

void zb_rc_free(ZbRateChg *r)
{
    if (!r) return;
    free(r->ov); free(r->work); free(r->left); free(r);
}

static void rc_reset(ZbRateChg *r) { r->nleft = 0; r->started = 0; }

void zb_rc_set_speed(ZbRateChg *r, double speed)
{
    if (!(0.0 < speed) || speed == r->speed) return;
    rc_reset(r);
    r->speed = speed;
    r->Sa = (int)((double)r->p1 * speed);
}

long zb_rc_max_out(const ZbRateChg *r, int n)
{
    int total = r->nleft + n;
    if (total < 1) return 0;
    return (long)(unsigned)(long long)((1.0 / r->speed + 1.0) * (double)total);
}

/* FUN_1800259d0 */
static int rc_search(const double *ov, const double *x, int K, int V)
{
    double *e, E = 0.0, best = 0.0;
    int i, lag, idx = 0;
    if (K < 0 || V < 0) return 0;
    e = (double *)malloc(sizeof(double) * (size_t)(K + V + 1));
    for (i = 0; i <= K + V; i++) e[i] = x[i] * x[i];
    for (i = 0; i < V; i++) E = E + e[i];
    for (lag = 0; lag <= K; lag++) {
        double c = 0.0;
        for (i = 0; i < V; i++) c = c + x[lag + i] * ov[i];
        if (0.0 < c) {
            double v = (c * c) / E;
            if (best < v) { idx = lag; best = v; }
        }
        E = (E - e[lag]) + e[lag + V];
    }
    free(e);
    return idx;
}

/* FUN_18002cff0: fill the frame from the pending samples (from index start) and the input */
static int rc_fill(ZbRateChg *r, const short *in, int n, int start)
{
    int cnt = 0, k;
    if (r->nleft + n <= start) return n + (r->nleft - start);
    for (k = start; k < r->nleft; k++) r->work[cnt++] = (double)r->left[k];
    for (; cnt < r->N; cnt++) {
        if (r->nleft + n <= cnt + start) return cnt;
        r->work[cnt] = (double)in[(cnt - r->nleft) + start];
    }
    return cnt;
}

int zb_rc_process(ZbRateChg *r, const short *in, int n, short *out, int flags)
{
    const int N = r->N, L = r->L, K = r->K, V = r->V;
    int total = r->nleft + n, pos = 0, nout = 0, filled, i, fl;
    if (total < 1) { r->nleft = total; return 0; }
    if (flags & 1) rc_reset(r);
    if (!r->started) {
        int k = rc_fill(r, in, n, 0);
        if (k < N) {   /* not enough for one frame: the input passes through unchanged */
            if (n > 0) memcpy(out, in, sizeof(short) * (size_t)n);
            return n;
        }
        for (i = 0; i < V; i++) r->ov[i] = r->work[L + i];   /* FUN_18015d81c */
        r->off = 0;
        for (i = 0; i < L; i++) out[i] = (short)(int)r->work[i];
        nout = L;
        pos = r->Sa;
        r->started = 1;
    }
    for (;;) {
        int off;
        if (pos < r->nleft + n) {
            int j = 0, k;
            if (pos < r->nleft)
                for (k = pos; k < r->nleft; k++) r->work[j++] = (double)r->left[k];
            for (; j < N; j++) {
                if (r->nleft + n <= j + pos) break;
                r->work[j] = (double)in[(j - r->nleft) + pos];
            }
            filled = j;
        } else
            filled = (r->nleft - pos) + n;
        if (filled < N) break;
        off = (L - r->Sa) + r->off;
        if (off < 0 || K < off) off = rc_search(r->ov, r->work, K, V);
        r->off = off;
        if (off + V <= N)
            for (i = 0; i < V; i++) r->work[off + i] = ((double)(V - i) * r->ov[i] + (double)i * r->work[off + i]) / (double)V;
        for (i = 0; i < V; i++) r->ov[i] = r->work[L + i + r->off];
        for (i = 0; i < L; i++) out[nout + i] = (short)(int)r->work[r->off + i];
        nout += L;
        pos += r->Sa;
    }
    fl = nout != 0 ? flags : 2;
    if (!(fl & 2)) {
        for (i = 0; i < filled; i++) r->left[i] = (short)(int)r->work[i];
        r->nleft = filled;
        return nout;
    }
    if (0 < filled) {   /* FUN_18015d860: last partial frame */
        int off = (L - r->Sa) + r->off, c;
        if (off < 0 || K < off || filled <= off) {
            int kk = filled - V - 1;
            if (K < kk) kk = K;
            off = rc_search(r->ov, r->work, kk, V);
        }
        r->off = off;
        c = filled - off < V ? filled - off : V;
        for (i = 0; i < c; i++) r->work[off + i] = ((double)(c - i) * r->ov[i] + (double)i * r->work[off + i]) / (double)c;
        for (i = off; i < filled; i++) out[nout++] = (short)(int)r->work[i];
    }
    rc_reset(r);
    return nout;
}

int zb_rc_flush(ZbRateChg *r, short *out)
{
    if (r->nleft == 0) return 0;
    return zb_rc_process(r, NULL, 0, out, 2);
}
