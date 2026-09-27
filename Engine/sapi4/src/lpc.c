#include "lpc.h"
#include "dsp.h"
#include <math.h>

void lpc_from_lsf(float *lsf, float *out, int order, int unused) {
    (void)unused;
    // the original keeps these in fixed stack arrays (21 doubles each); orders are 10..16 in practice
    double w[64], cp[32], cq[32], s1[33], s2[33], t1[33], t2[33], p[33], q[33];
    int unsorted = 0;
    for (int i = 1; i < order; i++)
        if (!(lsf[i] > lsf[i - 1])) unsorted = 1;
    if (unsorted) dsp_sort_floats(lsf, order);
    int half = order / 2;
    for (int i = 0; i < order; i++) w[i] = lsf[i];
    for (int i = 0; i <= half; i++) t1[i] = t2[i] = q[i] = s1[i] = s2[i] = p[i] = 0.0;
    for (int i = 0; i < half; i++) {
        cp[i] = cos(w[2 * i] * 6.283185307179586) * -2.0;
        cq[i] = cos(w[2 * i + 1] * 6.283185307179586) * -2.0;
    }
    double prev = 0.0;
    for (int n = 0; n < order + 1; n++) {
        double imp = n == 0 ? 1.0 : 0.0;
        p[0] = prev + imp;
        q[0] = imp - prev;
        prev = imp;
        for (int j = 0; j < half; j++) {
            p[j + 1] = cp[j] * s2[j] + s1[j] + p[j];
            q[j + 1] = cq[j] * t2[j] + t1[j] + q[j];
            s1[j] = s2[j];
            s2[j] = p[j];
            t1[j] = t2[j];
            t2[j] = q[j];
        }
        if (n) out[n - 1] = (float)((p[half] + q[half]) * -0.5);
    }
    for (int k = order - 1; k >= 0; k--) out[k + 1] = (float)-(double)out[k];
    out[0] = 1.0f;
}
