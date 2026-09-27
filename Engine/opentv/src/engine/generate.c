/*
 * The formant synthesizer's sample loop.
 *
 * Synth_Step hands this one frame of filter coefficients (the 40 int16s of
 * Engine.filt_coef, already interpolated for this frame) and it produces the
 * frame's worth of samples.  The voicing source is a table-interpolated
 * glottal pulse whose phase advances by o_207e each sample; the source runs
 * through a cascade of second-order resonators (the formants), a parallel
 * branch for the fricatives, and a final de-emphasis, and every stage
 * saturates to 16 bits the way the original does.
 *
 * All the arithmetic is the original's: 16-bit state, 32-bit products, and
 * Q15 shifts.  The helpers below name the truncations so the transliteration
 * stays readable.
 */
#include "engine.h"

/* Glottal pulse shape, indexed 0..255 (also read reflected around 0x80). */
/* @0x100e2778 */ extern const int16_t g_synth_pulse[];
/* Per-source-kind gain, indexed by the low five bits of coef[0]. */
/* @0x100e2980 */ extern const int16_t g_synth_srcgain[];
/* Voice-dependent breathiness scale, indexed by voice. */
/* @0x100b52c0 */ extern const int32_t g_synth_breath[];

/* cmp/jl/jg pair the original uses after every filter stage. */
static int32_t sat16(int32_t v)
{
    if (v >= 0x7fff)
        return 0x7fff;
    if (v <= -0x7fff)
        return 0x8000; /* stored as a word: -32768 */
    return v;
}

/* One step of the glottal pulse table with linear interpolation. */
static int32_t pulse(int32_t phase)
{
    int32_t idx = (phase >> 8) & 0xff;
    int32_t frac = phase & 0xff;
    int32_t v = g_synth_pulse[idx];
    int32_t d = 0x80 - idx;

    if (d < 0)
        d = -d;
    d = (int32_t)(int16_t)d;
    return v - (int32_t)(int16_t)((g_synth_pulse[d] * frac) >> 15);
}

/* @0x10025cb0 */
void TV_THISCALL Synth_Generate(Engine *self, int16_t sample_rate, int16_t *coef)
{
    int16_t a[28], b[8];
    int16_t n_left, t4, n_samp, t8, ta, tc, te, t10, t12, t14, t16, t18;
    int16_t t1a, t1e, t22, t26, t2a, t2e, t32, t36;
    int32_t d1c, d20, d24, d28, d2c, d30, d34, d38;
    int32_t d90, d94, d98, d9c, da0, da4, da8, dac;
    int32_t si, di, ax, bx, dx, i;
    int16_t w;

    /* ---- unpack the frame ------------------------------------------- */
    dx = coef[0];
    t4 = coef[1];
    for (i = 0; i < 28; i++)
        a[i] = coef[2 + i];
    n_left = coef[30];
    te = coef[31];
    for (i = 0; i < 8; i++)
        b[i] = coef[32 + i];

    t8 = (int16_t)(-a[3]);
    ta = a[2];
    t14 = a[19];
    t1e = a[4];
    w = (int16_t)(-a[5]);          /* si in the original */
    t2e = a[21];
    a[7] = (int16_t)(-a[7]);
    a[9] = (int16_t)(-a[9]);
    d98 = (int16_t)(-a[11]);
    da4 = (int16_t)(-b[7]);
    a[13] = (int16_t)(-a[13]);
    t22 = a[10];
    t32 = b[6];
    t36 = b[5];
    t12 = a[18];
    t2a = a[20];
    t26 = a[22];
    t1a = a[24];
    tc = a[17];
    self->o_2092[10] = b[1];

    t10 = (int16_t)(dx & 0x80);
    dx &= 0x1f;
    t18 = (int16_t)(((dx & 0xff) >= 0xf) ? 0x4000 : 0);
    t16 = g_synth_srcgain[(int32_t)(int16_t)dx];
    t4 = (int16_t)(((int32_t)t4 * 20861) >> 15);
    n_samp = self->o_rate_div100;
    dac = t36;
    da8 = t32;
    da0 = t2e;
    d9c = t22;
    d24 = w;
    d20 = t1e;
    d94 = t2a;
    d90 = t26;
    d38 = t1a;

    do {
        /* ---- pitch period bookkeeping ------------------------------- */
        dx = n_left;
        if (dx == 0 && self->o_2076 < 0) {
            self->o_2082 = 0;
            self->o_2078 = 0;
            self->o_2076 = 0;
        } else {
            self->o_2076 = (int16_t)(self->o_2076 - 1);
            if (self->o_2076 < 0) {
                self->o_2082 = -1;
                dx = (int16_t)((int16_t)dx >> 1);
                self->o_2092[0] = a[12];
                self->o_2092[1] = a[13];
                self->o_2092[2] = b[4];
                self->o_2092[3] = a[8];
                self->o_2092[4] = a[9];
                self->o_2092[5] = a[25];
                self->o_2092[6] = a[6];
                self->o_2092[7] = a[7];
                self->o_2092[8] = a[23];
                self->o_2092[9] = b[0];
                self->o_20a8 = a[27];
                self->o_2076 = n_left;
                w = n_left;
                n_left = te;
                te = w;
                self->o_2078 = (int16_t)dx;
            } else {
                self->o_2082 = 1;
                self->o_2078 = (int16_t)(self->o_2078 - 1);
            }
        }

        /* ---- voicing source ----------------------------------------- */
        if (self->o_2082 < 0) {
            self->o_20e0 = 0;
            self->o_207a = 0;
            self->o_2080 = t18;
            self->o_20e4 = (int32_t)b[2];
            w = b[3];
            b[3] = b[2];
            b[2] = w;
            self->o_207e = t4;
            self->o_20dc = (int32_t)t16;
            dx = 0;
        } else if (self->o_2082 == 0) {
            dx = 0;
        } else if (self->o_207a & 1) {
            dx = 0;
        } else {
            int32_t phase = (int32_t)self->o_207e + self->o_20e0;
            int32_t half = (self->o_207a & 2) != 0;

            self->o_20e0 = phase;
            if (phase > 0xffff) {
                phase &= 0xffff;
                self->o_20e0 = phase;
                if (half) {
                    self->o_207a = 3;
                    dx = 0;
                    goto have_source;
                }
                self->o_207a = 2;
                half = 1; /* take the shaped branch below */
            } else if (!half) {
                /* the quiet half of the period: half amplitude, inverted */
                di = pulse(phase);
                dx = self->o_20e4 >> 1;
                di = (di * dx) >> 15;
                dx = (int16_t)((int16_t)dx - (int16_t)di);
                goto have_source;
            }

            di = pulse(phase);
            ax = self->o_20dc;
            di = di * ax;
            ax <<= 15;
            si = di - ax;
            si >>= 15;
            if (self->o_2080 & 0x4000) {
                si <<= 16;
                di &= 0x7fff;
                ax = si + di * 2 + 0x7fffff;
                if (ax < 0) {
                    self->o_207a = 3;
                    dx = 0;
                    goto have_source;
                }
                ax &= 0xffff00;
                dx = self->o_20e4;
                ax = (int32_t)((uint32_t)ax >> 8);
                dx = (dx * ax) >> 15;
            } else {
                si += 0x7fff;
                si <<= 16;
                if (si < 0) {
                    self->o_207a = 3;
                    dx = 0;
                    goto have_source;
                }
                si >>= 16;
                dx = self->o_20e4;
                dx = (dx * si) >> 15;
            }
        }

have_source:
        /* ---- pre-emphasis and the first cascade resonator ------------ */
        ax = (int32_t)(int16_t)dx - (int32_t)self->o_208c;
        self->o_208c = (int16_t)dx;
        ax <<= 3;
        si = ((int32_t)a[15] * ax) >> 15;
        si = sat16(si);

        /* aspiration noise: a 16-bit LFSR */
        ax = self->o_2088;
        self->o_2088 = 0xaaaa;
        dx = (int32_t)(int16_t)(self->o_208a ^ (int16_t)ax);
        self->o_208e = (int16_t)si;
        self->o_208a = (int16_t)ax;
        ax = (int32_t)((((uint32_t)dx & 0xfc00u) >> 7) | ((uint32_t)dx << 6)) & 0xffff;
        if ((int16_t)ax != 0)
            self->o_2088 = (uint16_t)ax;

        dx = (int16_t)self->o_2088;
        ax = ((int32_t)a[14] * (int32_t)(int16_t)dx) >> 15;
        di = self->o_20ae[10];
        ax = (int16_t)((int16_t)ax + (int16_t)si);
        ax = (int32_t)(int16_t)ax * dac;
        si = (int32_t)(int16_t)di * da8;
        ax += si;
        si = (int32_t)self->o_20ae[11] * da4;
        self->o_20ae[11] = (int16_t)di;
        ax = (si + ax * 2) >> 15;
        ax = sat16(ax);
        self->o_20ae[10] = (int16_t)ax;

        if (sample_rate == 0x1f40) {
            si = (int32_t)tc * (int32_t)(int16_t)ax;
            di = self->o_20ae[0];
            si += (int32_t)(int16_t)di * d20;
            ax = (int32_t)self->o_20ae[1] * d24;
            self->o_20ae[1] = (int16_t)di;
            si = (ax + si * 2) >> 15;
            si = sat16(si);
            self->o_20ae[0] = (int16_t)si;
        } else {
            di = self->o_20aa;
            si = (int32_t)(int16_t)di * (int32_t)ta;
            bx = (int32_t)tc * (int32_t)(int16_t)ax;
            ax = (int32_t)self->o_20ac;
            self->o_20ac = (int16_t)di;
            si += bx;
            ax = ax * (int32_t)t8;
            si = (ax + si * 2) >> 15;
            si = sat16(si);
            self->o_20aa = (int16_t)si;

            di = (int32_t)t14 * (int32_t)(int16_t)si;
            bx = self->o_20ae[0];
            di += (int32_t)(int16_t)bx * d20;
            ax = (int32_t)self->o_20ae[1] * d24;
            self->o_20ae[1] = (int16_t)bx;
            di = (ax + di * 2) >> 15;
            di = sat16(di);
            self->o_20ae[0] = (int16_t)di;
        }

        /* ---- the cascade: four more resonators ---------------------- */
        d34 = self->o_2092[6];
        d30 = self->o_2092[7];
        si = (int32_t)self->o_20ae[0] * da0;
        di = self->o_20ae[2];
        si += (int32_t)(int16_t)di * d34;
        ax = (int32_t)self->o_20ae[3] * d30;
        self->o_20ae[3] = (int16_t)di;
        si = (ax + si * 2) >> 15;
        si = sat16(si);

        d2c = self->o_2092[3];
        d28 = self->o_2092[4];
        bx = self->o_20ae[4];
        self->o_20ae[2] = (int16_t)si;
        di = (int32_t)self->o_2092[8] * (int32_t)(int16_t)si;
        di += (int32_t)(int16_t)bx * d2c;
        ax = (int32_t)self->o_20ae[5] * d28;
        self->o_20ae[5] = (int16_t)bx;
        di = (ax + di * 2) >> 15;
        di = sat16(di);

        si = self->o_20ae[6];
        self->o_20ae[4] = (int16_t)di;
        bx = (int32_t)self->o_2092[5] * (int32_t)(int16_t)di;
        ax = self->o_20ae[7];
        bx += si * d9c;
        bx = (ax * d98 + bx * 2) >> 15;
        di = self->o_20ae[6];
        self->o_20ae[7] = (int16_t)di;
        bx = sat16(bx);

        di = ((int32_t)b[1] * ax) >> 15;
        ax = ((int32_t)self->o_2092[9] * si) >> 14;
        ax = (int32_t)(int16_t)((int16_t)ax & (int16_t)0xfffe);
        si = self->o_20ae[8];
        self->o_20ae[6] = (int16_t)bx;
        bx = (int16_t)((int16_t)bx + (int16_t)di);
        bx = (int16_t)((int16_t)bx + (int16_t)ax);
        di = (int32_t)self->o_20a8 * (int32_t)(int16_t)bx;
        ax = (int32_t)self->o_2092[0] * (int32_t)(int16_t)si;
        di += ax;
        ax = (int32_t)self->o_20ae[9] * (int32_t)self->o_2092[1];
        self->o_20ae[9] = (int16_t)si;
        di = (ax + di * 2) >> 15;
        di = sat16(di);

        si = (int32_t)self->o_2092[2] * (int32_t)(int16_t)di;
        self->o_20ae[8] = (int16_t)di;
        si &= ~0x4000;
        si >>= 14;
        si = sat16(si);

        /* ---- the parallel (fricative) branch ------------------------ */
        if (self->o_2076 != 0 && self->o_2078 < 0)
            dx = (int16_t)((int16_t)dx >> 1);

        bx = 0;
        if (sample_rate != 0x1f40) {
            ax = self->o_20ae[14];
            t1a = (int16_t)ax;
            bx = (int32_t)(int16_t)ax * (int32_t)ta;
            ax = (int32_t)self->o_20ae[15] * (int32_t)t8;
            bx = ax + bx * 2;
            ax = (int32_t)t12 * (int32_t)(int16_t)dx;
            bx += ax;
            bx >>= 15;
            self->o_20ae[14] = (int16_t)bx;
            self->o_20ae[15] = t1a;
            bx = sat16(bx);
        }

        d1c = (int32_t)(int16_t)dx;
        di = self->o_20ae[16];
        dx = (int32_t)(int16_t)di * d20;
        ax = (int32_t)self->o_20ae[17] * d24;
        self->o_20ae[17] = (int16_t)di;
        dx = ax + dx * 2;
        dx += d94 * d1c;
        dx >>= 15;
        self->o_20ae[16] = (int16_t)dx;
        dx += (int32_t)(int16_t)bx;
        dx = sat16(dx);

        bx = (int32_t)self->o_20ae[19] * d30;
        di = self->o_20ae[18];
        dx = (int32_t)(int16_t)dx;
        ax = (int32_t)(int16_t)di * d34;
        self->o_20ae[19] = (int16_t)di;
        ax = bx + ax * 2;
        ax += d1c * d90;
        ax >>= 15;
        self->o_20ae[18] = (int16_t)ax;
        dx += ax;
        dx = sat16(dx);

        bx = (int32_t)self->o_20ae[21] * d28;
        di = self->o_20ae[20];
        dx = (int32_t)(int16_t)dx;
        ax = (int32_t)(int16_t)di * d2c;
        self->o_20ae[21] = (int16_t)di;
        ax = bx + ax * 2;
        ax += d1c * d38;
        ax >>= 15;
        self->o_20ae[20] = (int16_t)ax;
        dx += ax;
        dx = sat16(dx);

        /* ---- output: de-emphasis, breathiness, volume --------------- */
        ax = ((int32_t)a[26] * d1c) >> 15;
        ax = (int16_t)((int16_t)ax + (int16_t)dx);
        ax = (int32_t)(int16_t)ax;
        ax = (ax * 6221) >> 15;
        di = (int16_t)((int16_t)ax - self->o_2086);
        self->o_2086 = (int16_t)ax;
        ax = ((int32_t)self->o_207c * 7537) >> 15;
        di = (int16_t)((int16_t)di - (int16_t)ax);
        di = (int16_t)((int16_t)di + (int16_t)si);
        self->o_207c = (int16_t)di;
        if (t10 != 0)
            di = 0;

        si = (int32_t)(int16_t)di;
        ax = g_synth_breath[self->cur_voice] * self->o_20e8;
        ax = ax / -100;
        ax += si;
        ax = ax * (int32_t)self->fmt_2104;
        di = sat16(ax * 8);

        n_samp = (int16_t)(n_samp - 1);
        self->o_20e8 = si;
        *(int16_t *)(self->out_buf + (self->out_count & ~1u)) = (int16_t)di;
        self->out_count += 2;
    } while (n_samp > 0);
}
