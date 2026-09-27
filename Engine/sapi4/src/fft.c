// Inverse real split-radix FFT of msttssyn.dll (@0x63684b60), decompiled from the x87 code: every value
// the original parks in a float stack slot is rounded to float here too (the f32 temporaries).
#include "fft.h"

void fft_inverse_real(float *x, int n, int m, const float *sine) {
    const double r2 = (double)0.70710677f;
    const int q = n / 2;
    int n2 = 2 * n, step = 1;
    for (int k = m - 1; k > 0; k--) {
        int id = n2;
        n2 /= 2;
        int n4 = n2 / 4, n8 = n4 / 2;
        step *= 2;
        int is = 0;
        if (n - 1 > 0) {
            do {
                for (int i = is; i < n; i += id) {
                    float *p1 = x + i, *p2 = x + i + n4, *p3 = x + i + 2 * n4, *p4 = x + i + 3 * n4;
                    double a = (double)*p3 + *p1;
                    double t = (double)*p1 - *p3;
                    *p1 = (float)a;
                    *p2 = (float)((double)*p2 + *p2);
                    *p3 = (float)(t - ((double)*p4 + *p4));
                    *p4 = (float)(t - (double)*p4 * -2.0);
                    if (n4 > 1) {
                        float *b0 = x + i + n8, *b1 = b0 + n4, *b2 = b0 + 2 * n4, *b3 = b0 + 3 * n4;
                        double s76 = (double)*b3 + *b2;
                        double d54 = (double)*b1 - *b0;
                        double B = s76 * r2, A = d54 * r2;
                        double c45 = (double)*b0 + *b1;
                        float Bf = (float)B;
                        double amb = A - Bf;
                        double bpa = B + A;
                        *b0 = (float)c45;
                        double d76 = (double)*b3 - *b2;
                        *b1 = (float)d76;
                        *b2 = (float)(bpa * -2.0);
                        *b3 = (float)(amb + amb);
                    }
                }
                is = 2 * id - n2;
                id *= 4;
            } while (is < n - 1);
        }
        for (int j = 1; j < n8; j++) {
            const float cc1 = sine[step * j + q], ss1 = sine[step * j];
            const float cc3 = sine[3 * step * j + q], ss3 = sine[3 * step * j];
            is = 0;
            id = 2 * n2;
            if (n - 1 <= 0) continue;
            do {
                for (int i = is; i < n; i += id) {
                    float *i1 = x + i + j, *i2 = i1 + n4, *i3 = i2 + n4, *i4 = i3 + n4;
                    float *i5 = x + i + n4 - j, *i6 = i5 + n4, *i7 = i6 + n4, *i8 = i7 + n4;
                    double x1 = *i1, x6 = *i6;
                    double s16 = x1 + x6;
                    double d16 = (double)*i1 - *i6;
                    *i1 = (float)s16;
                    double x2 = *i2;
                    double s25 = x2 + *i5;
                    double d52 = (double)*i5 - *i2;
                    *i5 = (float)s25;
                    double d83 = (double)*i8 - *i3;
                    double s38 = (double)*i3 + *i8;
                    float t14 = (float)d16;
                    *i6 = (float)d83;
                    double s47 = (double)*i4 + *i7;
                    float t78 = (float)d52;
                    float t18 = (float)s47;
                    double d47 = (double)*i4 - *i7;
                    *i2 = (float)d47;
                    float u1 = (float)((double)t14 - t18);
                    float u2 = (float)((double)t18 + t14);
                    float u3 = (float)(d52 - s38);
                    float u4 = (float)(s38 + t78);
                    *i3 = (float)((double)ss1 * u3 + (double)cc1 * u1);
                    *i7 = (float)((double)ss1 * u1 - (double)cc1 * u3);
                    *i4 = (float)((double)cc3 * u2 - (double)ss3 * u4);
                    *i8 = (float)((double)ss3 * u2 + (double)cc3 * u4);
                }
                is = 2 * id - n2;
                id *= 4;
            } while (is < n - 1);
        }
    }
    // length-two butterflies
    if (n - 1 > 0) {
        int is = 0, id = 4;
        do {
            for (int i = is; i < n; i += id) {
                double a = x[i], b = x[i + 1];
                x[i] = (float)(b + a);
                x[i + 1] = (float)(a - x[i + 1]);
                (void)b;
            }
            is = 2 * id - 2;
            id *= 4;
        } while (is < n - 1);
    }
    // bit reversal
    for (int i = 0, j = 0; i < n - 1; i++) {
        if (i < j) {
            float t = x[j];
            x[j] = x[i];
            x[i] = t;
        }
        int kk = q;
        while (kk <= j) {
            j -= kk;
            kk /= 2;
        }
        j += kk;
    }
    if (n > 0) {
        const double dn = (double)n;
        for (int i = 0; i < n; i++) x[i] = (float)((double)x[i] / dn);
    }
}
