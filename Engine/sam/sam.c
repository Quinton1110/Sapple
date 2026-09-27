/* libsam back end - see sam.h.
 *
 * Function comments name the original routine in spttseng.dll (image base 0x5ed30000) that each
 * piece reproduces. Arithmetic keeps the original's precision pattern: intermediate expressions in
 * double (the original used the x87 FPU), results stored as float.
 */
#include "sam.h"
#include "sam_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CV_NO_DEBUG_ENV /* ClassicVoices patch: the shipped screen-reader voice never prints what it speaks */
#define getenv(name) ((char *)0)
#endif

#define SPD_MAGIC 0x61746144u /* "Data" */
#define MAX_CB 16
#define CHUNK_FLUSH 4999      /* Synth_RenderChunk flushes once the chunk exceeds this */
#define CHUNK_GUARD 6000      /* ...and clamps an epoch that would overflow its 6000-sample buffer */
#define GUARD_PERIOD 999

typedef struct {
    int dim;
    const uint8_t *data; /* dim floats per entry */
} codebook;

struct sam_voice {
    uint8_t *file;
    size_t size;
    int rate, order, fft_n, fft_log2, n_units;
    int n_lsf, n_exc, n_dexc;
    codebook lsf[MAX_CB], exc[MAX_CB], dexc[MAX_CB];
    const uint8_t *unit_tab;  /* int32 offsets relative to the synth section */
    const uint8_t *synth;     /* synth section base */
    size_t synth_size;
    float *sine;              /* [2 * fft_n] */
    float *window;            /* [fft_n], 1 -> 0 */
    float *noise;             /* [rate] */
};

struct sam_synth {
    const sam_voice *v;
    sam_params p;
    float *mem;          /* LPC filter state [order+1] */
    int noise_pos;       /* voice +0x78 */
    int chunk_pos;       /* position inside the engine's output chunk */
    double svib_phase;   /* singing vibrato phase */
    /* vowel hold (singing): output samples -> source samples, piecewise linear */
    int w_on;
    double w_o0, w_o1, w_s0, w_s1, w_nat, w_hold;
    unsigned vib_state;  /* synth +0x80 */
    float *buf;          /* scratch output */
    size_t buf_cap;
    /* ClassicVoices patch (singing mode): the speech settings saved while a sing call runs */
    int sing_on, sv_smooth;
    float sv_vib, sv_vib_rate;
};

/* ------------------------------------------------------------------------------------------ */
/* little-endian readers (portable to big-endian hosts)                                         */

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int32_t rd_i32(const uint8_t *p) { return (int32_t)rd_u32(p); }

static float rd_f32(const uint8_t *p)
{
    uint32_t u = rd_u32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static float *copy_floats(const uint8_t *p, int n)
{
    float *out = malloc(sizeof(float) * (size_t)n);
    int i;
    if (!out) return NULL;
    for (i = 0; i < n; i++) out[i] = rd_f32(p + 4 * i);
    return out;
}

static float cb_get(const codebook *cb, int idx, int k)
{
    return rd_f32(cb->data + ((size_t)idx * cb->dim + k) * 4);
}

/* ------------------------------------------------------------------------------------------ */
/* voice loading (FUN_5ed58d7a)                                                                 */

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) snprintf(err, errlen, "%s", msg);
    return 0;
}

static int load_codebooks(sam_voice *v, int count_off, int table_off, codebook *out, int *n,
                          char *err, size_t errlen)
{
    int i, count = rd_i32(v->synth + count_off);
    if (count < 0 || count > MAX_CB) return fail(err, errlen, "bad codebook count");
    for (i = 0; i < count; i++) {
        const uint8_t *e = v->synth + table_off + i * 12;
        int dim = rd_i32(e), off = rd_i32(e + 4);
        if (dim <= 0 || off < 0 || (size_t)off >= v->synth_size) return fail(err, errlen, "bad codebook");
        out[i].dim = dim;
        out[i].data = v->synth + off;
    }
    *n = count;
    return 1;
}

static sam_voice *voice_from(uint8_t *file, size_t size, char *err, size_t errlen);

sam_voice *sam_voice_load(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long size;
    if (!f) { fail(err, errlen, "cannot open voice file"); return NULL; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = size > 0 ? malloc((size_t)size) : NULL;
    if (!d || fread(d, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(d);
        fail(err, errlen, "cannot read voice file");
        return NULL;
    }
    fclose(f);
    return voice_from(d, (size_t)size, err, errlen);
}

sam_voice *sam_voice_load_mem(const void *data, size_t size, char *err, size_t errlen)
{
    uint8_t *d = malloc(size ? size : 1);
    if (!d) { fail(err, errlen, "out of memory"); return NULL; }
    memcpy(d, data, size);
    return voice_from(d, size, err, errlen);
}

/* takes ownership of file */
static sam_voice *voice_from(uint8_t *file, size_t size, char *err, size_t errlen)
{
    sam_voice *v = calloc(1, sizeof *v);
    uint32_t off3, size3;
    if (!v) { free(file); return NULL; }
    v->file = file;
    v->size = size;
    if (v->size < 0x40 || rd_u32(v->file) != SPD_MAGIC || rd_u32(v->file + 4) != 0x10000) {
        fail(err, errlen, "not a v1 .spd voice file");
        sam_voice_free(v);
        return NULL;
    }
    off3 = rd_u32(v->file + 0x18 + 3 * 8);
    size3 = rd_u32(v->file + 0x1C + 3 * 8);
    if ((size_t)off3 + size3 > v->size || size3 < 0x4B0) {
        fail(err, errlen, "bad synth section");
        sam_voice_free(v);
        return NULL;
    }
    v->synth = v->file + off3;
    v->synth_size = size3;
    v->rate = rd_i32(v->synth + 0);
    v->order = rd_i32(v->synth + 0x498);
    v->fft_n = rd_i32(v->synth + 0x49C);
    v->fft_log2 = rd_i32(v->synth + 0x4A0);
    v->n_units = rd_i32(v->synth + 0x490);
    if (v->order <= 0 || v->order > 30 || v->fft_n <= 0 || v->fft_n > 512 || (1 << v->fft_log2) != v->fft_n) {
        fail(err, errlen, "unsupported voice parameters");
        sam_voice_free(v);
        return NULL;
    }
    if (!load_codebooks(v, 4, 0x14, v->lsf, &v->n_lsf, err, errlen) ||
        !load_codebooks(v, 8, 0x194, v->exc, &v->n_exc, err, errlen) ||
        !load_codebooks(v, 0xC, 0x314, v->dexc, &v->n_dexc, err, errlen)) {
        sam_voice_free(v);
        return NULL;
    }
    v->unit_tab = v->synth + rd_i32(v->synth + 0x494);
    v->sine = copy_floats(v->synth + rd_i32(v->synth + 0x4A4), 2 * v->fft_n);
    v->window = copy_floats(v->synth + rd_i32(v->synth + 0x4A8), v->fft_n);
    v->noise = copy_floats(v->synth + rd_i32(v->synth + 0x4AC), v->rate);
    if (!v->sine || !v->window || !v->noise) {
        fail(err, errlen, "out of memory");
        sam_voice_free(v);
        return NULL;
    }
    return v;
}

void sam_voice_free(sam_voice *v)
{
    if (!v) return;
    free(v->file);
    free(v->sine);
    free(v->window);
    free(v->noise);
    free(v);
}

int sam_voice_rate(const sam_voice *v) { return v->rate; }

const uint8_t *sam_voice_section(const sam_voice *v, int index, uint32_t *size)
{
    uint32_t off, sz;
    if (index < 0 || index > 4) return NULL;
    off = rd_u32(v->file + 0x18 + index * 8);
    sz = rd_u32(v->file + 0x1C + index * 8);
    if ((size_t)off + sz > v->size) return NULL;
    if (size) *size = sz;
    return v->file + off;
}
int sam_voice_units(const sam_voice *v) { return v->n_units; }

/* ------------------------------------------------------------------------------------------ */
/* unit decoding                                                                                */

/* FUN_5ed57ea2: signed bytes; 0x7F / 0x80 continue into the following byte. */
static const uint8_t *read_escaped(const uint8_t *p, int n, float *out)
{
    int k;
    for (k = 0; k < n; k++) {
        int8_t c = (int8_t)*p;
        if (c == 127 || c == -128) {
            float acc = (float)c;
            while ((int8_t)*p == c) {
                p++;
                acc = (float)((double)(int8_t)*p + acc);
            }
            out[k] = acc;
        } else {
            out[k] = (float)c;
        }
        p++;
    }
    return p;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* FUN_5ed57f9e: line spectral frequencies (normalised, 0..0.5) -> LPC a[0..order], a[0] = 1. */
static void lsf_to_lpc(float *lsf, int order, float *a)
{
    const double two_pi = (double)6.2831855f; /* DAT_5ede9a44 = 0x40c90fdb */
    double pc[16], qc[16], s1[16], s2[16], t1[16], t2[16], p[17], q[17];
    int half = order / 2, i, j, n;
    double prev = 0.0;
    for (i = 1; i < order; i++) {
        if (!(lsf[i] > lsf[i - 1])) {
            qsort(lsf, (size_t)order, sizeof(float), cmp_float); /* FUN_5ed57f56 */
            break;
        }
    }
    for (i = 0; i < half; i++) {
        pc[i + 1] = cos(two_pi * (double)lsf[2 * i]) * -2.0;
        qc[i + 1] = cos(two_pi * (double)lsf[2 * i + 1]) * -2.0;
        s1[i] = s2[i] = t1[i] = t2[i] = 0.0;
    }
    for (n = 0; n <= order; n++) {
        double imp = n == 0 ? 1.0 : 0.0;
        p[0] = prev + imp;
        q[0] = imp - prev;
        prev = imp;
        for (j = 0; j < half; j++) {
            p[j + 1] = pc[j + 1] * s2[j] + s1[j] + p[j];
            q[j + 1] = qc[j + 1] * t2[j] + t1[j] + q[j];
            s1[j] = s2[j];
            s2[j] = p[j];
            t1[j] = t2[j];
            t2[j] = q[j];
        }
        if (n) a[n] = (float)((q[half] + p[half]) * -0.5);
    }
    /* original stores c[n-1] then shifts with negation: a[k] = -c[k-1] */
    for (n = 1; n <= order; n++) a[n] = -a[n];
    a[0] = 1.0f;
}

/* FUN_5ed58259: split-radix inverse real FFT (Sorensen packing: re[0..n/2], im[k] at x[n-k]),
 * scaled by 1/n. `sine` holds sin(2*pi*k/(2n)) for k in [0, 2n). */
static void ifft_split_radix(float *x, int n, int m, const float *sine)
{
    const double r2 = (double)0.70710677f;
    int q = n / 2, n2 = n * 2, step = 1, k, i, j, is, id;
    for (k = 1; k < m; k++) {
        int n4, n8;
        n2 /= 2;
        n4 = n2 / 4;
        n8 = n4 / 2;
        is = 0;
        id = n2 * 2;
        do {
            for (i = is; i < n; i += id) {
                double f1 = x[i], f2 = x[i + 2 * n4], t = f1 - f2;
                x[i] = (float)(f1 + f2);
                x[i + n4] = (float)((double)x[i + n4] * 2.0);
                x[i + 2 * n4] = (float)(t - (double)x[i + 3 * n4] * 2.0);
                x[i + 3 * n4] = (float)((double)x[i + 3 * n4] * 2.0 + t);
                if (n4 > 1) {
                    int b0 = i + n8;
                    double a = ((double)x[b0 + n4] - x[b0]) * r2;
                    double b = ((double)x[b0 + 2 * n4] + x[b0 + 3 * n4]) * r2;
                    x[b0] = (float)((double)x[b0] + x[b0 + n4]);
                    x[b0 + n4] = (float)((double)x[b0 + 3 * n4] - x[b0 + 2 * n4]);
                    x[b0 + 2 * n4] = (float)((b + a) * -2.0);
                    x[b0 + 3 * n4] = (float)((a - b) * 2.0);
                }
            }
            is = id * 2 - n2;
            id *= 4;
        } while (is < n - 1);
        for (j = 1; j < n8; j++) {
            int e = j * step;
            double cc1 = sine[2 * e + q], ss1 = sine[2 * e];
            double cc3 = sine[6 * e + q], ss3 = sine[6 * e];
            is = 0;
            id = n2 * 2;
            do {
                for (i = is; i < n; i += id) {
                    int i1 = i + j, i2 = i1 + n4, i3 = i2 + n4, i4 = i3 + n4;
                    int i5 = i + n4 - j, i6 = i5 + n4, i7 = i6 + n4, i8 = i7 + n4;
                    double x1 = x[i1], x2 = x[i2], x3 = x[i3], x4 = x[i4];
                    double x5 = x[i5], x6 = x[i6], x7 = x[i7], x8 = x[i8];
                    double u1, u2, u3, u4;
                    x[i1] = (float)(x1 + x6);
                    x[i5] = (float)(x5 + x2);
                    x[i6] = (float)(x8 - x3);
                    x[i2] = (float)(x4 - x7);
                    u1 = (x1 - x6) - (x7 + x4);
                    u2 = x7 + x4 + (x1 - x6);
                    u3 = (x5 - x2) - (x8 + x3);
                    u4 = x8 + x3 + (x5 - x2);
                    x[i3] = (float)(u1 * cc1 + u3 * ss1);
                    x[i7] = (float)(u1 * ss1 - u3 * cc1);
                    x[i4] = (float)(cc3 * u2 - ss3 * u4);
                    x[i8] = (float)(ss3 * u2 + cc3 * u4);
                }
                is = id * 2 - n2;
                id *= 4;
            } while (is < n - 1);
        }
        step *= 2;
    }
    /* length-two butterflies */
    is = 0;
    id = 4;
    do {
        for (i = is; i < n; i += id) {
            double a = x[i], b = x[i + 1];
            x[i] = (float)(a + b);
            x[i + 1] = (float)(a - b);
        }
        is = id * 2 - 2;
        id *= 4;
    } while (is < n - 1);
    /* bit reversal */
    for (i = 0, j = 0; i < n - 1; i++) {
        int kk = n / 2;
        if (i < j) {
            float t = x[i];
            x[i] = x[j];
            x[j] = t;
        }
        while (kk <= j) {
            j -= kk;
            kk /= 2;
        }
        j += kk;
    }
    {
        float scale = (float)(1.0 / (double)(float)n);
        for (i = 0; i < n; i++) x[i] = (float)((double)scale * x[i]);
    }
}

void sam_unit_free(sam_unit *u)
{
    if (!u) return;
    free(u->periods);
    free(u->lpc);
    free(u->gains);
    free(u->exc);
    free(u->lsf);
    memset(u, 0, sizeof *u);
}

/* FUN_5ed58ee3 (Voice_DecodeUnit). */
static int unit_decode(const sam_voice *v, int index, int *noise_pos, sam_unit *u, int whisper);

int sam_unit_decode(const sam_voice *v, int index, int *noise_pos, sam_unit *u)
{
    return unit_decode(v, index, noise_pos, u, 0);
}

/* whisper (SAPI 4 msttssyn): every frame takes the unvoiced (noise) path and is marked unvoiced */
static int unit_decode(const sam_voice *v, int index, int *noise_pos, sam_unit *u, int whisper)
{
    const int N = v->fft_n, order = v->order;
    const uint8_t *p;
    float spec[512], buf[512], lsf[32];
    int f, total = 0, pos = 0;
    memset(u, 0, sizeof *u);
    if (index < 0 || index >= v->n_units) return -1;
    p = v->synth + rd_i32(v->unit_tab + 4 * index);
    u->nframes = rd_i32(p);
    u->order = order;
    p += 4;
    if (u->nframes <= 0 || u->nframes > 4096) return -1;
    u->periods = malloc(sizeof(float) * u->nframes);
    u->lpc = malloc(sizeof(float) * u->nframes * (order + 1));
    u->gains = malloc(sizeof(float) * u->nframes);
    u->lsf = malloc(sizeof(float) * u->nframes * order);
    if (!u->periods || !u->lpc || !u->gains || !u->lsf) {
        sam_unit_free(u);
        return -1;
    }
    p = read_escaped(p, u->nframes, u->periods);
    for (f = 0; f < u->nframes; f++) {
        int i, k, n = 0;
        for (i = 0; i < v->n_lsf; i++) {
            int idx = *p++;
            for (k = 0; k < v->lsf[i].dim; k++) lsf[n++] = cb_get(&v->lsf[i], idx, k);
        }
        memcpy(u->lsf + f * order, lsf, sizeof(float) * order);
        lsf_to_lpc(lsf, order, u->lpc + f * (order + 1));
    }
    for (f = 0; f < u->nframes; f++) {
        u->gains[f] = rd_f32(p + 4 * f);
        total += (int)fabsf(u->periods[f]);
    }
    p += 4 * u->nframes;
    u->total = total;
    u->exc = calloc((size_t)total + 1, sizeof(float));
    if (!u->exc) {
        sam_unit_free(u);
        return -1;
    }
    memset(spec, 0, sizeof spec);
    if (whisper)
        for (f = 0; f < u->nframes; f++) u->periods[f] = -fabsf(u->periods[f]);
    for (f = 0; f < u->nframes; f++) {
        int n = (int)fabsf(u->periods[f]);
        float *out = u->exc + pos;
        if (u->periods[f] <= 0.0f) {
            /* unvoiced: running noise buffer scaled by gain * 0.02 */
            double g = (double)u->gains[f] * (double)0.02f;
            int i;
            if (*noise_pos + n >= v->rate) *noise_pos = 0;
            for (i = 0; i < n; i++) out[i] = (float)(g * v->noise[*noise_pos + i]);
            *noise_pos += n;
        } else {
            int delta = v->n_dexc && f > 0 && u->periods[f - 1] >= 0.0f;
            const codebook *books = delta ? v->dexc : v->exc;
            int nb = delta ? v->n_dexc : v->n_exc, b, bin = 1, m, i;
            if (!delta) memset(spec, 0, sizeof(float) * N);
            for (b = 0; b < nb; b++) {
                int idx = *p++, half = books[b].dim / 2;
                for (i = 0; i < half; i++) {
                    float re = cb_get(&books[b], idx, i), im = cb_get(&books[b], idx, half + i);
                    int lo = bin + i, hi = N - bin - half + 1 + i;
                    if (delta) {
                        spec[lo] = (float)((double)re + spec[lo]);
                        spec[hi] = (float)((double)im + spec[hi]);
                    } else {
                        spec[lo] = re;
                        spec[hi] = im;
                    }
                }
                bin += half;
            }
            memcpy(buf, spec, sizeof(float) * N);
            ifft_split_radix(buf, N, v->fft_log2, v->sine);
            for (i = 0; i < N; i++) buf[i] = (float)((double)u->gains[f] * buf[i]); /* FUN_5ed586f2 */
            /* FUN_5ed5867c: wrap the centred pulse into one period */
            m = n < N / 2 ? n : N / 2;
            for (i = 0; i < m; i++) out[i] = buf[i];
            for (i = m; i < n; i++) out[i] = 0.0f;
            for (i = n - m; i < n; i++) out[i] = (float)((double)buf[i - n + N] + out[i]);
        }
        pos += n;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* synthesis                                                                                    */

void sam_params_default(sam_params *p)
{
    p->gain = 1.0f;
    p->pitch_scale = 1.0f;
    p->speed = 1.0f;
    p->vibrato = 0.0f;
    p->vibrato_rate = 5.0f;
    p->reverse_units = 0;
    p->out_rate = 0;
    p->smooth = 0;
    p->sing_vibrato = 0.0f;
    p->sing_vibrato_rate = 5.5f;
    p->transpose = 0.0f;
    p->base_pitch = 100.0f;
    p->whisper = 0;
    p->monotone = 0;
    p->no_reverse = 0;
}

sam_synth *sam_synth_new(const sam_voice *v, const sam_params *p)
{
    sam_synth *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->v = v;
    if (p) s->p = *p;
    else sam_params_default(&s->p);
    s->mem = calloc((size_t)v->order + 1, sizeof(float));
    if (!s->mem) {
        free(s);
        return NULL;
    }
    return s;
}

/* ClassicVoices patch (singing mode): on = 1 saves the synth's smooth / singer's-vibrato settings and sets
 * smooth frames (as sam_say --sing does) and the given vibrato, from phase 0; on = 0 restores them exactly. */
void sam_synth_set_singing(sam_synth *s, int on, float vib_cents, float vib_rate)
{
    if (!s) return;
    if (on) {
        if (!s->sing_on) {
            s->sv_smooth = s->p.smooth;
            s->sv_vib = s->p.sing_vibrato;
            s->sv_vib_rate = s->p.sing_vibrato_rate;
            s->sing_on = 1;
        }
        s->p.smooth = 1;
        s->p.sing_vibrato = vib_cents;
        s->p.sing_vibrato_rate = vib_rate > 0.0f ? vib_rate : 5.5f;
        s->svib_phase = 0.0;
    } else if (s->sing_on) {
        s->p.smooth = s->sv_smooth;
        s->p.sing_vibrato = s->sv_vib;
        s->p.sing_vibrato_rate = s->sv_vib_rate;
        s->sing_on = 0;
    }
}

void sam_synth_free(sam_synth *s)
{
    if (!s) return;
    free(s->mem);
    free(s->buf);
    free(s);
}

static float *scratch(sam_synth *s, size_t n)
{
    if (n > s->buf_cap) {
        float *b = realloc(s->buf, n * sizeof(float));
        if (!b) return NULL;
        s->buf = b;
        s->buf_cap = n;
    }
    return s->buf;
}

/* knot interpolation used by both the pitch and the amplitude contours */
static double knot_interp(const sam_segment *g, const float *vals, double x)
{
    int i = 1;
    while (i < g->n_knots - 1 && !(x <= (double)g->t[i])) i++;
    return ((double)vals[i] - vals[i - 1]) * (x - (double)g->t[i - 1]) / ((double)g->t[i] - g->t[i - 1]) +
           vals[i - 1];
}

/* FUN_5ed42afd: fit a pulse of src_n samples into dst_n samples, tapering with the window */
static void fit_period(const float *src, int src_n, float *dst, int dst_n, const float *win, int win_n)
{
    int m = src_n < dst_n ? src_n : dst_n, i;
    double step = (double)win_n / m, acc = 0.5;
    memset(dst, 0, sizeof(float) * dst_n);
    dst[0] = src[0];
    for (i = 1; i < m; i++) {
        double w;
        acc += step;
        w = win[(int)acc];
        dst[i] = (float)(w * src[i] + dst[i]);
        dst[dst_n - i] = (float)(w * src[src_n - i] + dst[dst_n - i]);
    }
}

/* FUN_5ed42dd7: all-pole filter, state in s->mem (mem[i] = y[n-i]) */
static void lpc_filter(sam_synth *s, const float *a, float *x, int n, float gain)
{
    int order = s->v->order, k, i;
    float *mem = s->mem;
    for (k = 0; k < n; k++) {
        float acc = (float)((double)a[0] * x[k]);
        for (i = order; i > 0; i--) {
            acc = (float)((double)acc - (double)mem[i] * a[i]);
            mem[i] = i > 1 ? mem[i - 1] : acc;
        }
        mem[0] = acc;
        x[k] = (float)((double)gain * acc);
    }
}

/* FUN_5ed42a8f: clip to [-32768, 32767], then float -> int16 by truncation */
static int16_t to_pcm(float x)
{
    if (x > 32767.0f) x = 32767.0f;
    else if (x < -32768.0f) x = -32768.0f;
    return (int16_t)(long)x;
}

static void emit(const float *x, int n, sam_pcm_cb cb, void *user)
{
    int16_t tmp[1024];
    int i = 0;
    while (i < n) {
        int k = 0;
        while (k < 1024 && i < n) tmp[k++] = to_pcm(x[i++]);
        cb(tmp, (size_t)k, user);
    }
}

/* PitchMarks_Place (FUN_5ed42bda): choose a source frame and an output period for every epoch. */

/* source position (unit samples) at output sample o, for a held vowel */
static double warp(const sam_synth *s, double o)
{
    if (o < s->w_o0) return o * s->w_nat;
    if (o < s->w_o1) return s->w_s0 + (o - s->w_o0) * s->w_hold;
    return s->w_s1 + (o - s->w_o1) * s->w_nat;
}

static int place_marks(sam_synth *s, const sam_segment *g, const sam_unit *u, float rate,
                       int *frame, int *len, unsigned char *rev, int max_epochs)
{
    const double sr = (double)(float)(s->p.out_rate > 0 ? s->p.out_rate : s->v->rate);
    double T = 0.0, inv = 1.0 / (double)rate;
    float src_time = 0.0f;
    int f = 0, rep = 0, tsamp = 0, e = 0;
    while (f < u->nframes && e < max_epochs) {
        float per = u->periods[f];
        int src_n, dst_n, unvoiced, place;
        float half, src_end;
        if (per < 0.0f) {
            src_n = dst_n = (int)(-(double)per + 0.5);
            unvoiced = 1;
        } else {
            double F = knot_interp(g, g->f0, (double)tsamp);
            if (s->p.vibrato != 0.0f) {
                /* engine: signed byte from a 256-entry sine table indexed by the phase accumulator */
                double ph = (double)((s->vib_state >> 16) & 0xFF) * (2.0 * 3.14159265358979 / 256.0);
                F += floor(127.0 * sin(ph) + 0.5) * s->p.vibrato;
            }
            if (s->p.sing_vibrato != 0.0f) {
                double tsec = (double)tsamp / sr, fade = (tsec - 0.15) / 0.25;
                if (fade < 0.0) fade = 0.0;
                if (fade > 1.0) fade = 1.0;
                F *= pow(2.0, (double)s->p.sing_vibrato * fade * sin(s->svib_phase) / 1200.0);
            }
            if (F < 10.0) F = 10.0;
            src_n = (int)((double)per + 0.5);
            dst_n = (int)(sr / F);
            unvoiced = 0;
            if (s->p.sing_vibrato != 0.0f)
                s->svib_phase = fmod(s->svib_phase + 2.0 * 3.14159265358979 * (double)s->p.sing_vibrato_rate * dst_n / sr,
                                     2.0 * 3.14159265358979);
            if (s->p.vibrato != 0.0f) {
                double step = 256.0 * 65536.0 * dst_n * (double)s->p.vibrato_rate / sr;
                s->vib_state = (s->vib_state + (unsigned)(long long)step) & 0xFFFFFFu;
            }
        }
        if (s->w_on) {
            T = warp(s, (double)tsamp);
            half = (float)(0.5 * (warp(s, (double)(tsamp + dst_n)) - T));
        } else {
            half = (float)(inv * dst_n * 0.5);
        }
        src_end = (float)((double)src_time + (float)src_n);
        place = T + half <= src_end;
        if (place) {
            rev[e] = (unsigned char)(unvoiced && (rep & 1) && !s->p.no_reverse);
            frame[e] = f;
            len[e] = dst_n;
            e++;
            tsamp += dst_n;
            T = s->w_on ? warp(s, (double)tsamp) : T + inv * dst_n;
            rep++;
        }
        if (T + half > src_end || !place) {
            f++;
            rep = 0;
            src_time = src_end;
        }
    }
    return e;
}

/* samples rendered since the engine's last 5000-sample output flush (Synth_RenderChunk) */
int sam_synth_chunk_pos(const sam_synth *s) { return s->chunk_pos; }
void sam_synth_chunk_reset(sam_synth *s) { s->chunk_pos = 0; }

/* Speak_NextSentence + Synth_RenderChunk (FUN_5ed42f7f / FUN_5ed43499). */
int sam_synth_segment(sam_synth *s, const sam_segment *g_in, sam_pcm_cb cb, void *user)
{
    const sam_voice *v = s->v;
    const double sr = (double)(float)(s->p.out_rate > 0 ? s->p.out_rate : v->rate);
    sam_segment g = *g_in;
    sam_unit u;
    int *offs, *frame, *len, max_e, n_e, e, tsamp = 0, i;
    unsigned char *rev;
    float rate, *out;
    size_t n_out = 0, cap;

    if (s->p.speed > 0.0f && s->p.speed != 1.0f) {
        g.dur = (float)((double)g.dur / s->p.speed);
        for (i = 0; i < g.n_knots; i++) g.t[i] = (float)((double)g.t[i] / s->p.speed);
    }
    if (s->p.out_rate > 0 && s->p.out_rate != v->rate) /* knot times arrive in voice-rate samples */
        for (i = 0; i < g.n_knots; i++) g.t[i] = (float)((double)g.t[i] * sr / (double)v->rate);
    if (s->p.pitch_scale != 1.0f)
        for (i = 0; i < g.n_knots; i++) g.f0[i] = (float)((double)g.f0[i] * s->p.pitch_scale);

    if (g.unit == 0) { /* silence: zero samples, reset the filter */
        int n = (int)(sr * (double)g.dur);
        memset(s->mem, 0, sizeof(float) * (v->order + 1));
        out = scratch(s, (size_t)n + 1);
        if (!out) return -1;
        for (i = 0; i < n; i++) out[i] = 0.0f;
        s->chunk_pos = (s->chunk_pos + n) % (CHUNK_FLUSH + 1);
        emit(out, n, cb, user);
        return 0;
    }
    if (g.n_knots < 2 || g.n_knots > SAM_MAX_KNOTS) return -1;
    if (unit_decode(v, g.unit, &s->noise_pos, &u, s->p.whisper) != 0) return -1;

    rate = (float)((double)g.dur * sr / (double)u.total);
    /* the original sizes its epoch arrays from duration and peak pitch; a frame can repeat at most
     * about (period / shortest output period) times, so this bound is generous */
    max_e = (int)((double)g.dur * 2205.0) + 2 * u.nframes + 16;
    offs = malloc(sizeof(int) * (u.nframes + 1));
    frame = malloc(sizeof(int) * max_e);
    len = malloc(sizeof(int) * max_e);
    rev = malloc((size_t)max_e);
    if (!offs || !frame || !len || !rev) goto oom;
    offs[0] = 0;
    for (i = 0; i < u.nframes; i++) offs[i + 1] = offs[i] + (int)fabsf(u.periods[i]);

    s->w_on = 0;
    if (g.hold && u.nframes >= 5) {
        /* steadiest voiced frame in the middle of the unit: smallest LSF change to its neighbours */
        double out_len = (double)g.dur * sr, nat = (double)v->rate / sr, best = 1e30;
        int f, fc = u.nframes / 2, lo = u.nframes / 4, hi = (3 * u.nframes) / 4;
        if (g.hold == 2) { /* a held ending (the r of "are"): hold its last frames */
            lo = hi = fc = u.nframes - 2;
        }
        if (lo < 1) lo = 1;
        if (hi > u.nframes - 2) hi = u.nframes - 2;
        for (f = lo; f <= hi; f++) {
            double d = 0.0;
            int k;
            if (u.periods[f - 1] <= 0 || u.periods[f] <= 0 || u.periods[f + 1] <= 0) continue;
            for (k = 0; k < u.order; k++)
                d += fabs(u.lsf[f * u.order + k] - u.lsf[(f - 1) * u.order + k]) +
                     fabs(u.lsf[(f + 1) * u.order + k] - u.lsf[f * u.order + k]);
            if (d < best) {
                best = d;
                fc = f;
            }
        }
        {
            double s0 = offs[fc - 1 >= 0 ? fc - 1 : 0], s1 = offs[fc + 2 <= u.nframes ? fc + 2 : u.nframes];
            double hold_out = out_len - (s0 + ((double)u.total - s1)) / nat;
            if (getenv("SAM_DEBUG")) fprintf(stderr, "HOLD unit %d frames %d hold %d-%d of %d\n", g.unit, u.nframes, fc - 1, fc + 1, u.nframes);
            if (hold_out > (s1 - s0) / nat) { /* only when the vowel must get longer */
                s->w_on = 1;
                s->w_nat = nat;
                s->w_s0 = s0;
                s->w_s1 = s1;
                s->w_o0 = s0 / nat;
                s->w_o1 = s->w_o0 + hold_out;
                s->w_hold = (s1 - s0) / hold_out;
            }
        }
    }
    n_e = place_marks(s, &g, &u, rate, frame, len, rev, max_e);
    cap = 0;
    for (e = 0; e < n_e; e++) cap += (size_t)(len[e] > GUARD_PERIOD ? len[e] : GUARD_PERIOD);
    out = scratch(s, cap + 1);
    if (!out) goto oom;

    for (e = 0; e < n_e; e++) {
        int f = frame[e], src_n = offs[f + 1] - offs[f], n = len[e];
        const float *src = u.exc + offs[f];
        float *dst = out + n_out, amp, *a;
        float ai[33]; /* ClassicVoices patch: the smoothed filter, used after the block below (was scoped inside it) */
        if (s->chunk_pos + n > CHUNK_GUARD) n = GUARD_PERIOD;
        amp = (float)knot_interp(&g, g.amp, (double)tsamp);
        /* Excitation_FetchPeriod (FUN_5ed42e40) */
        if (rev[e]) {
            for (i = 0; i < n; i++) dst[i] = src[n - 1 - i < src_n ? n - 1 - i : src_n - 1];
        } else if (src_n == n) {
            memcpy(dst, src, sizeof(float) * n);
        } else {
            fit_period(src, src_n, dst, n, v->window, v->fft_n);
        }
        if (amp != 1.0f)
            for (i = 0; i < n; i++) dst[i] = (float)((double)amp * dst[i]);
        a = u.lpc + f * (u.order + 1);
        a[0] = 1.0f;
        if (s->p.smooth && u.nframes > 1) {
            /* blend with the neighbouring frame at this point of the unit (not in the original) */
            float lsf[32], *tmp;
            double src_pos = s->w_on ? warp(s, (double)tsamp + 0.5 * n) : ((double)tsamp + 0.5 * n) / (double)rate, c0, c1, w;
            int f0 = 0, j2;
            while (f0 + 1 < u.nframes && 0.5 * (offs[f0 + 1] + offs[f0 + 2]) <= src_pos) f0++;
            c0 = 0.5 * (offs[f0] + offs[f0 + 1]);
            c1 = f0 + 1 < u.nframes ? 0.5 * (offs[f0 + 1] + offs[f0 + 2]) : c0;
            w = c1 > c0 ? (src_pos - c0) / (c1 - c0) : 0.0;
            if (w < 0.0) w = 0.0;
            if (w > 1.0) w = 1.0;
            if (f0 + 1 < u.nframes && w > 0.0) {
                const float *la = u.lsf + f0 * u.order, *lb = u.lsf + (f0 + 1) * u.order;
                for (j2 = 0; j2 < u.order; j2++) lsf[j2] = (float)((1.0 - w) * la[j2] + w * lb[j2]);
                lsf_to_lpc(lsf, u.order, ai);
                ai[0] = 1.0f;
                a = ai;
                /* crossfade the pulse shapes when both frames are voiced */
                if (u.periods[f0] > 0 && u.periods[f0 + 1] > 0 && !rev[e]) {
                    int na = offs[f0 + 1] - offs[f0], nb = offs[f0 + 2] - offs[f0 + 1];
                    tmp = malloc(sizeof(float) * 2 * (size_t)(n > 0 ? n : 1));
                    if (tmp) {
                        float *pa = tmp, *pb = tmp + n;
                        if (na == n) memcpy(pa, u.exc + offs[f0], sizeof(float) * n);
                        else fit_period(u.exc + offs[f0], na, pa, n, v->window, v->fft_n);
                        if (nb == n) memcpy(pb, u.exc + offs[f0 + 1], sizeof(float) * n);
                        else fit_period(u.exc + offs[f0 + 1], nb, pb, n, v->window, v->fft_n);
                        for (j2 = 0; j2 < n; j2++) dst[j2] = (float)(amp * ((1.0 - w) * pa[j2] + w * pb[j2]));
                        free(tmp);
                    }
                }
            }
        }
        lpc_filter(s, a, dst, n, s->p.gain);
        n_out += (size_t)n;
        tsamp += n;
        s->chunk_pos += n;
        if (s->chunk_pos > CHUNK_FLUSH) s->chunk_pos = 0;
    }
    if (s->p.reverse_units)
        for (i = 0; i < (int)n_out / 2; i++) {
            float tmp = out[i];
            out[i] = out[n_out - 1 - i];
            out[n_out - 1 - i] = tmp;
        }
    emit(out, (int)n_out, cb, user);
    free(offs);
    free(frame);
    free(len);
    free(rev);
    sam_unit_free(&u);
    return 0;
oom:
    free(offs);
    free(frame);
    free(len);
    free(rev);
    sam_unit_free(&u);
    return -1;
}
