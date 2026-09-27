/* zb_mlpg.c - float MLPG of CTTSAcousticSpaceImpl<float> (David): FUN_18000ca70 per dimension:
 * FUN_18000d6a0 (band WUW / WUM for the standard 3 windows, float products accumulated in double),
 * FUN_18000cf70 (LDL^T in double, pivot guard 1e-6), FUN_180012e30 (substitution; back substitution on
 * float-rounded outputs). */
#include <stdlib.h>
#include <math.h>
#include "zb_internal.h"

#define GUARD(x) do { if (fabs((x) - 0.0) <= 1e-6) (x) = 1.0; } while (0)

static void mlpg_dim(const ZbModel *m, ZbGauss *const *fr, int T, int D, int d, double (*W)[3], double *R, double *g, float *out)
{
    /* window coefficients: delta c[-1], c[+1]; delta-delta a[-1], a[0], a[+1] */
    const float dm = m->win[1][0], dp = m->win[1][2];
    const float am = m->win[2][0], a0 = m->win[2][1], ap = m->win[2][2];
    const float f17 = a0 * a0, f20 = a0 * ap, f18 = dp * dp, f21 = dm * dm, f19 = ap * ap, f22 = am * am;
    const int is = d, id = d + D, ia = d + 2 * D;
    int t;
#define P(t, i) (fr[t]->p[i])
#define B(t, i) (fr[t]->mp[i])
    /* row 0 */
    W[0][0] = (double)P(0, is);
    W[0][0] = (double)(f17 * P(0, ia)) + W[0][0];
    W[0][1] = (double)(f20 * P(0, ia));
    W[0][2] = 0.0;
    R[0] = (double)B(0, is);
    R[0] = (double)(a0 * B(0, ia)) + R[0];
    if (T > 1) {
        W[0][0] = (double)(f21 * P(1, id)) + W[0][0];
        W[0][0] = (double)(f22 * P(1, ia)) + W[0][0];
        W[0][1] = (double)(f20 * P(1, ia)) + W[0][1];
        W[0][2] = (double)(dm * dp * P(1, id)) + W[0][2];
        W[0][2] = (double)(am * ap * P(1, ia)) + W[0][2];
        R[0] = (double)(dm * B(1, id)) + R[0];
        R[0] = (double)(am * B(1, ia)) + R[0];
    }
    for (t = 1; t + 2 < T; t++) {
        double x = (double)P(t, is);
        x = (double)(f18 * P(t - 1, id)) + x;
        x = (double)(f21 * P(t + 1, id)) + x;
        x = (double)(f19 * P(t - 1, ia)) + x;
        x = (double)(f17 * P(t, ia)) + x;
        W[t][0] = (double)(f22 * P(t + 1, ia)) + x;
        W[t][1] = (double)(f20 * P(t, ia));
        W[t][1] = (double)(am * a0 * P(t + 1, ia)) + W[t][1];
        W[t][2] = (double)(dm * dp * P(t + 1, id));
        W[t][2] = (double)(am * ap * P(t + 1, ia)) + W[t][2];
        R[t] = (double)B(t, is);
        R[t] = (double)(dp * B(t - 1, id)) + R[t];
        R[t] = (double)(dm * B(t + 1, id)) + R[t];
        R[t] = (double)(ap * B(t - 1, ia)) + R[t];
        R[t] = (double)(a0 * B(t, ia)) + R[t];
        R[t] = (double)(am * B(t + 1, ia)) + R[t];
    }
    if (T >= 3) {
        double x;
        t = T - 2;
        x = (double)P(t, is);
        x = (double)(f18 * P(t - 1, id)) + x;
        x = (double)(f21 * P(t + 1, id)) + x;
        x = (double)(f19 * P(t - 1, ia)) + x;
        x = (double)(f17 * P(t, ia)) + x;
        W[t][0] = (double)(f22 * P(t + 1, ia)) + x;
        W[t][1] = (double)(f20 * P(t, ia));
        W[t][1] = (double)(am * a0 * P(t + 1, ia)) + W[t][1];
        W[t][2] = 0.0;
        R[t] = (double)B(t, is);
        R[t] = (double)(dp * B(t - 1, id)) + R[t];
        R[t] = (double)(dm * B(t + 1, id)) + R[t];
        R[t] = (double)(ap * B(t - 1, ia)) + R[t];
        R[t] = (double)(a0 * B(t, ia)) + R[t];
        R[t] = (double)(am * B(t + 1, ia)) + R[t];
    }
    if (T >= 2) {
        double x;
        t = T - 1;
        x = (double)P(t, is);
        x = (double)(f18 * P(t - 1, id)) + x;
        x = (double)(f19 * P(t - 1, ia)) + x;
        W[t][0] = (double)(f17 * P(t, ia)) + x;
        W[t][1] = 0.0;
        W[t][2] = 0.0;
        R[t] = (double)B(t, is);
        R[t] = (double)(dp * B(t - 1, id)) + R[t];
        R[t] = (double)(ap * B(t - 1, ia)) + R[t];
        R[t] = (double)(a0 * B(t, ia)) + R[t];
    }
#undef P
#undef B
    /* LDL^T (FUN_18000cf70), band width 3 */
    GUARD(W[0][0]);
    W[0][1] = W[0][1] / W[0][0];
    GUARD(W[0][0]);
    W[0][2] = W[0][2] / W[0][0];
    if (T > 1) {
        W[1][0] = W[1][0] - W[0][1] * W[0][1] * W[0][0];
        W[1][1] = W[1][1] - W[0][2] * W[0][1] * W[0][0];
        GUARD(W[1][0]);
        W[1][1] = W[1][1] / W[1][0];
        W[1][2] = W[1][2] / W[1][0];
        for (t = 2; t < T; t++) {
            W[t][0] = W[t][0] - W[t - 2][2] * W[t - 2][2] * W[t - 2][0];
            W[t][0] = W[t][0] - W[t - 1][1] * W[t - 1][1] * W[t - 1][0];
            W[t][1] = W[t][1] - W[t - 1][1] * W[t - 1][2] * W[t - 1][0];
            GUARD(W[t][0]);
            W[t][1] = W[t][1] / W[t][0];
            W[t][2] = W[t][2] / W[t][0];
        }
    }
    /* substitution (FUN_180012e30) */
    if (T < 2) {
        g[0] = R[0] / W[0][0];
        out[d] = (float)g[0];
        return;
    }
    g[0] = R[0];
    g[1] = R[1] - g[0] * W[0][1];
    for (t = 2; t < T; t++) g[t] = (R[t] - W[t - 1][1] * g[t - 1]) - W[t - 2][2] * g[t - 2];
    for (t = 0; t < T; t++) g[t] = g[t] / W[t][0];
    out[(T - 1) * D + d] = (float)g[T - 1];
    out[(T - 2) * D + d] = (float)(g[T - 2] - (double)out[(T - 1) * D + d] * W[T - 2][1]);
    for (t = T - 3; t >= 0; t--)
        out[t * D + d] = (float)((g[t] - (double)out[(t + 1) * D + d] * W[t][1]) - (double)out[(t + 2) * D + d] * W[t][2]);
}

void zb_mlpg_float(const ZbModel *m, ZbGauss *const *fr, int T, int D, float *out)
{
    double (*W)[3];
    double *R, *g;
    int d;
    if (T <= 0) return;
    W = (double (*)[3])malloc(sizeof(double) * 3 * (size_t)T);
    R = (double *)malloc(sizeof(double) * (size_t)T);
    g = (double *)malloc(sizeof(double) * (size_t)T);
    for (d = 0; d < D; d++) mlpg_dim(m, fr, T, D, d, W, R, g, out);
    free(W);
    free(R);
    free(g);
}

/* ClassicVoices patch (Eva): the same MLPG, statement for statement, on flat per-frame arrays [T][3][D]
 * (static, delta, delta-delta) instead of ZbGauss pointers - Eva has one Gaussian per FRAME, and ZbGauss frames
 * would cost ~1 KB each.  mlpg_dim_arr is mlpg_dim with only the P()/B() accessors changed. */
static void mlpg_dim_arr(const ZbModel *m, const float *pa, const float *ba, int T, int D, int d, double (*W)[3], double *R, double *g, float *out)
{
    /* window coefficients: delta c[-1], c[+1]; delta-delta a[-1], a[0], a[+1] */
    const float dm = m->win[1][0], dp = m->win[1][2];
    const float am = m->win[2][0], a0 = m->win[2][1], ap = m->win[2][2];
    const float f17 = a0 * a0, f20 = a0 * ap, f18 = dp * dp, f21 = dm * dm, f19 = ap * ap, f22 = am * am;
    const int is = d, id = d + D, ia = d + 2 * D;
    int t;
#define P(t, i) (pa[(size_t)(t) * 3 * (size_t)D + (size_t)(i)])
#define B(t, i) (ba[(size_t)(t) * 3 * (size_t)D + (size_t)(i)])
    /* row 0 */
    W[0][0] = (double)P(0, is);
    W[0][0] = (double)(f17 * P(0, ia)) + W[0][0];
    W[0][1] = (double)(f20 * P(0, ia));
    W[0][2] = 0.0;
    R[0] = (double)B(0, is);
    R[0] = (double)(a0 * B(0, ia)) + R[0];
    if (T > 1) {
        W[0][0] = (double)(f21 * P(1, id)) + W[0][0];
        W[0][0] = (double)(f22 * P(1, ia)) + W[0][0];
        W[0][1] = (double)(f20 * P(1, ia)) + W[0][1];
        W[0][2] = (double)(dm * dp * P(1, id)) + W[0][2];
        W[0][2] = (double)(am * ap * P(1, ia)) + W[0][2];
        R[0] = (double)(dm * B(1, id)) + R[0];
        R[0] = (double)(am * B(1, ia)) + R[0];
    }
    for (t = 1; t + 2 < T; t++) {
        double x = (double)P(t, is);
        x = (double)(f18 * P(t - 1, id)) + x;
        x = (double)(f21 * P(t + 1, id)) + x;
        x = (double)(f19 * P(t - 1, ia)) + x;
        x = (double)(f17 * P(t, ia)) + x;
        W[t][0] = (double)(f22 * P(t + 1, ia)) + x;
        W[t][1] = (double)(f20 * P(t, ia));
        W[t][1] = (double)(am * a0 * P(t + 1, ia)) + W[t][1];
        W[t][2] = (double)(dm * dp * P(t + 1, id));
        W[t][2] = (double)(am * ap * P(t + 1, ia)) + W[t][2];
        R[t] = (double)B(t, is);
        R[t] = (double)(dp * B(t - 1, id)) + R[t];
        R[t] = (double)(dm * B(t + 1, id)) + R[t];
        R[t] = (double)(ap * B(t - 1, ia)) + R[t];
        R[t] = (double)(a0 * B(t, ia)) + R[t];
        R[t] = (double)(am * B(t + 1, ia)) + R[t];
    }
    if (T >= 3) {
        double x;
        t = T - 2;
        x = (double)P(t, is);
        x = (double)(f18 * P(t - 1, id)) + x;
        x = (double)(f21 * P(t + 1, id)) + x;
        x = (double)(f19 * P(t - 1, ia)) + x;
        x = (double)(f17 * P(t, ia)) + x;
        W[t][0] = (double)(f22 * P(t + 1, ia)) + x;
        W[t][1] = (double)(f20 * P(t, ia));
        W[t][1] = (double)(am * a0 * P(t + 1, ia)) + W[t][1];
        W[t][2] = 0.0;
        R[t] = (double)B(t, is);
        R[t] = (double)(dp * B(t - 1, id)) + R[t];
        R[t] = (double)(dm * B(t + 1, id)) + R[t];
        R[t] = (double)(ap * B(t - 1, ia)) + R[t];
        R[t] = (double)(a0 * B(t, ia)) + R[t];
        R[t] = (double)(am * B(t + 1, ia)) + R[t];
    }
    if (T >= 2) {
        double x;
        t = T - 1;
        x = (double)P(t, is);
        x = (double)(f18 * P(t - 1, id)) + x;
        x = (double)(f19 * P(t - 1, ia)) + x;
        W[t][0] = (double)(f17 * P(t, ia)) + x;
        W[t][1] = 0.0;
        W[t][2] = 0.0;
        R[t] = (double)B(t, is);
        R[t] = (double)(dp * B(t - 1, id)) + R[t];
        R[t] = (double)(ap * B(t - 1, ia)) + R[t];
        R[t] = (double)(a0 * B(t, ia)) + R[t];
    }
#undef P
#undef B
    /* LDL^T (FUN_18000cf70), band width 3 */
    GUARD(W[0][0]);
    W[0][1] = W[0][1] / W[0][0];
    GUARD(W[0][0]);
    W[0][2] = W[0][2] / W[0][0];
    if (T > 1) {
        W[1][0] = W[1][0] - W[0][1] * W[0][1] * W[0][0];
        W[1][1] = W[1][1] - W[0][2] * W[0][1] * W[0][0];
        GUARD(W[1][0]);
        W[1][1] = W[1][1] / W[1][0];
        W[1][2] = W[1][2] / W[1][0];
        for (t = 2; t < T; t++) {
            W[t][0] = W[t][0] - W[t - 2][2] * W[t - 2][2] * W[t - 2][0];
            W[t][0] = W[t][0] - W[t - 1][1] * W[t - 1][1] * W[t - 1][0];
            W[t][1] = W[t][1] - W[t - 1][1] * W[t - 1][2] * W[t - 1][0];
            GUARD(W[t][0]);
            W[t][1] = W[t][1] / W[t][0];
            W[t][2] = W[t][2] / W[t][0];
        }
    }
    /* substitution (FUN_180012e30) */
    if (T < 2) {
        g[0] = R[0] / W[0][0];
        out[d] = (float)g[0];
        return;
    }
    g[0] = R[0];
    g[1] = R[1] - g[0] * W[0][1];
    for (t = 2; t < T; t++) g[t] = (R[t] - W[t - 1][1] * g[t - 1]) - W[t - 2][2] * g[t - 2];
    for (t = 0; t < T; t++) g[t] = g[t] / W[t][0];
    out[(T - 1) * D + d] = (float)g[T - 1];
    out[(T - 2) * D + d] = (float)(g[T - 2] - (double)out[(T - 1) * D + d] * W[T - 2][1]);
    for (t = T - 3; t >= 0; t--)
        out[t * D + d] = (float)((g[t] - (double)out[(t + 1) * D + d] * W[t][1]) - (double)out[(t + 2) * D + d] * W[t][2]);
}

void zb_mlpg_float_arr(const ZbModel *m, const float *prec, const float *mp, int T, int D, float *out)
{
    double (*W)[3];
    double *R, *g;
    int d;
    if (T <= 0) return;
    W = (double (*)[3])malloc(sizeof(double) * 3 * (size_t)T);
    R = (double *)malloc(sizeof(double) * (size_t)T);
    g = (double *)malloc(sizeof(double) * (size_t)T);
    if (W && R && g)
        for (d = 0; d < D; d++) mlpg_dim_arr(m, prec, mp, T, D, d, W, R, g, out);
    free(W);
    free(R);
    free(g);
}
