/* SAPI 4 Microsoft TTS voice effects, reconstructed from msttssyn.dll (see sam4fx.h).
 *
 * The effect (FUN_63680189): per block of up to 1024 samples
 *   wet = send * x                      (FUN_6367ff1d)
 *   wet = allpass_n(...allpass_1(wet))  (FUN_63680155 -> FUN_63680036, lines in series, in place)
 *   x   = clip(dry * x + wet)           (FUN_6367ff87, clip to -32768..32767)
 * Each allpass (Schroeder): d = delay line output, v = x + g*d (|v| < 0.001 flushed to 0), store v, y = d - g*v.
 * Gains are dB: 10^(dB/20), below -110 dB = 0 (FUN_6367fbfe). Delay samples = trunc(ms * rate * 0.001), >= 2.
 * Preset 7 shortens every line's loop to trunc(len * m), m = 1 - 0.05 * r / 4096, with r = rand() drawn at the
 * end of each call (so m is 1 for the first call).
 */
#include "sam4fx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float send_db, dry_db;
    int n;
    float ms[4], fb_db[4];
    float mod; /* random loop shortening depth (preset 7) */
} preset;

static const preset P[8] = {
    {0},
    {7.0f, 9.0f, 1, {10.0f}, {-0.5f}, 0.0f},
    {0.0f, 0.0f, 1, {10.6f}, {-10.498f}, 0.0f},
    {-11.0f, -4.0f, 4, {30.6f, 20.83f, 14.85f, 10.98f}, {-2.5f, -2.25f, -2.76f, -2.58f}, 0.0f},
    {-5.0f, -5.0f, 4, {81.2f, 55.3f, 35.7f, 21.96f}, {-2.5f, -2.25f, -2.76f, -2.58f}, 0.0f},
    {-3.0f, -5.0f, 4, {162.4f, 110.6f, 71.4f, 43.92f}, {-2.5f, -2.25f, -2.76f, -2.58f}, 0.0f},
    {-10.0f, 0.0f, 1, {400.6f}, {-10.498f}, 0.0f},
    {6.5f, 9.0f, 1, {10.0f}, {-0.5f}, 0.05f},
};

typedef struct {
    float g;
    int len;
    float *buf;
    int pos;
} line;

struct sam4fx {
    int preset;
    float send, dry, mod, m, r, in_gain;
    int nl;
    line l[4];
    float *wet;
    uint32_t seed; /* msvcrt rand() */
    float fir[16], hist[16];
    int nfir;
};

enum { BLOCK = 1024 };

static float db(float x) { return x < -110.0f ? 0.0f : (float)pow(10.0, (double)(x * 0.05f)); }

static int msrand(uint32_t *s)
{
    *s = *s * 214013u + 2531011u;
    return (int)((*s >> 16) & 0x7fff);
}

sam4fx *sam4fx_new(int preset_id, int rate)
{
    sam4fx *fx = calloc(1, sizeof *fx);
    int i;
    if (!fx) return NULL;
    fx->seed = 1;
    fx->m = 1.0f;
    fx->in_gain = 1.0f;
    fx->preset = preset_id >= 1 && preset_id <= 7 ? preset_id : 0;
    if (fx->preset) {
        const preset *p = &P[fx->preset];
        float spms = (float)rate * 0.001f;
        fx->send = db(p->send_db);
        fx->dry = db(p->dry_db);
        fx->mod = p->mod;
        fx->nl = p->n;
        for (i = 0; i < p->n; i++) {
            int len = (int)(p->ms[i] * spms);
            if (len < 2) len = 2;
            fx->l[i].g = db(p->fb_db[i]);
            fx->l[i].len = len;
            fx->l[i].buf = calloc((size_t)len, sizeof(float));
            if (!fx->l[i].buf) {
                sam4fx_free(fx);
                return NULL;
            }
        }
        fx->wet = malloc(sizeof(float) * BLOCK);
        if (!fx->wet) {
            sam4fx_free(fx);
            return NULL;
        }
    }
    return fx;
}

int sam4fx_set_fir(sam4fx *fx, const float *taps, int n)
{
    if (n < 0 || n > 16) return -1;
    memcpy(fx->fir, taps, sizeof(float) * (size_t)n);
    memset(fx->hist, 0, sizeof fx->hist);
    fx->nfir = n;
    return 0;
}

/* FUN_63671943: hist[0] = x; acc = c0 * x; then taps n-1 .. 1 (oldest first), shifting the history */
static void fir(sam4fx *fx, float *x, int n)
{
    int i, k;
    for (i = 0; i < n; i++) {
        float acc;
        fx->hist[0] = x[i];
        acc = x[i] * fx->fir[0];
        for (k = fx->nfir - 1; k > 0; k--) {
            acc = fx->fir[k] * fx->hist[k] + acc;
            fx->hist[k] = fx->hist[k - 1];
        }
        x[i] = acc;
    }
}

/* FUN_63680036 */
static void allpass(line *l, float m, float *x, int n)
{
    int end = (int)((float)l->len * m), i;
    if (end < 1) end = 1;
    if (l->pos >= end) l->pos = 0;
    for (i = 0; i < n; i++) {
        float d = l->buf[l->pos], v = d * l->g + x[i];
        if (v > -0.001f && v < 0.001f) v = 0.0f;
        l->buf[l->pos] = v;
        x[i] = d - v * l->g;
        if (++l->pos >= end) l->pos = 0;
    }
}

void sam4fx_set_input_gain(sam4fx *fx, float g) { fx->in_gain = g; }

/* not in the engine: its SAPI 4 voices were quieter than the SAPI 5 ones this is used with */
float sam4fx_default_trim_db(int preset)
{
    switch (preset) {
    case SAM4FX_ROBO_A:
    case SAM4FX_ROBO_B: return -10.0f;
    case SAM4FX_ROBO_C: return -5.0f;
    default: return 0.0f;
    }
}

/* FUN_63680189 */
void sam4fx_process(sam4fx *fx, float *x, int n)
{
    int i, k;
    if (fx->in_gain != 1.0f)
        for (i = 0; i < n; i++) x[i] *= fx->in_gain;
    if (fx->nfir) fir(fx, x, n); /* the engine's FIR stage runs before the effect */
    if (!fx->preset) return;
    fx->m = 1.0f - fx->mod * fx->r; /* r from the previous call (0 at first) */
    while (n > 0) {
        int b = n < BLOCK ? n : BLOCK;
        for (i = 0; i < b; i++) fx->wet[i] = fx->send > 0.0f ? (fx->send == 1.0f ? x[i] : fx->send * x[i]) : 0.0f;
        for (k = 0; k < fx->nl; k++) allpass(&fx->l[k], fx->m, fx->wet, b);
        if (fx->dry > 0.0f)
            for (i = 0; i < b; i++) {
                float y = (fx->dry == 1.0f ? x[i] : fx->dry * x[i]) + fx->wet[i];
                x[i] = y < -32768.0f ? -32768.0f : y > 32767.0f ? 32767.0f : y;
            }
        x += b;
        n -= b;
    }
    fx->r = (float)msrand(&fx->seed) * (1.0f / 4096.0f);
}

/* one call = one engine chunk (the RoboSoft One/Six random loop length changes per call) */
void sam4fx_process_s16(sam4fx *fx, int16_t *x, int n)
{
    float *buf = n > 0 ? malloc(sizeof(float) * (size_t)n) : NULL;
    int i;
    if (!buf) return;
    for (i = 0; i < n; i++) buf[i] = x[i];
    sam4fx_process(fx, buf, n);
    for (i = 0; i < n; i++) {
        float y = buf[i];
        x[i] = (int16_t)(y < -32768.0f ? -32768.0f : y > 32767.0f ? 32767.0f : y);
    }
    free(buf);
}

void sam4fx_free(sam4fx *fx)
{
    int i;
    if (!fx) return;
    for (i = 0; i < 4; i++) free(fx->l[i].buf);
    free(fx->wet);
    free(fx);
}

int sam4fx_lookup(const char *name, int *preset, int *monotone, int *whisper)
{
    static const struct {
        const char *name;
        int preset, mono, whisper;
    } T[] = {
        {"none", 0, 0, 0},      {"hall", 3, 0, 0},      {"stadium", 5, 0, 0},   {"space", 6, 0, 0},
        {"room", 4, 0, 0},      {"whisper", 0, 0, 1},   {"robosoft1", 7, 1, 0}, {"robosoft2", 1, 1, 0},
        {"robosoft3", 2, 0, 0}, {"robosoft4", 2, 0, 0}, {"robosoft5", 1, 1, 0}, {"robosoft6", 7, 1, 0},
        {"robot", 7, 1, 0},     {"monotone", 0, 1, 0},
    };
    size_t i;
    for (i = 0; i < sizeof T / sizeof *T; i++)
        if (!strcmp(name, T[i].name)) {
            *preset = T[i].preset;
            *monotone = T[i].mono;
            *whisper = T[i].whisper;
            return 0;
        }
    return -1;
}
