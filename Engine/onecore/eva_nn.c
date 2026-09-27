/* eva_nn.c - Eva's acoustic network (M1033Eva.TDAT), reconstructed from MSTTSEngine_OneCore.dll 10.3.21207.
 * Function addresses are VAs in that DLL (image base 0x180000000).  See notes/eva_dnn.md.
 * Keep -ffp-contract=off: every float statement below mirrors one SSE scalar instruction sequence. */
#include "eva_nn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(__APPLE__) || defined(__unix__)
#define EVA_MMAP 1   /* ClassicVoices: like zf1_dat.c (ZF1_DAT_MMAP), map the data read-only */
#endif
#ifdef EVA_MMAP
#include <sys/mman.h>
#include <unistd.h>
#endif

uint8_t *eva_map_file(const char *path, long *size, size_t *maplen) {
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;
    *maplen = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
#ifdef EVA_MMAP
    {
        void *m = mmap(NULL, (size_t)n, PROT_READ, MAP_PRIVATE, fileno(f), 0);
        if (m != MAP_FAILED) { fclose(f); *size = n; *maplen = (size_t)n; return (uint8_t *)m; }
    }
#endif
    p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *size = n;
    return p;
}

void eva_unmap_file(uint8_t *p, size_t maplen) {
    if (!p) return;
#ifdef EVA_MMAP
    if (maplen) { munmap(p, maplen); return; }
#endif
    (void)maplen;
    free(p);
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static float rdf(const uint8_t *p) { float f; uint32_t u = rd32(p); memcpy(&f, &u, 4); return f; }
static int pad64(int n) { return (n + 63) & ~63; }
static void *zalloc(size_t n) { void *p = calloc(1, n ? n : 1); return p; }

/* ---- MLPLinearTransform ---- */

/* SSE3Int8Impl init FUN_180127050 (base FUN_180126d14) */
static int lin_init(EvaLin *L, int type, const uint8_t *W, int stride, const uint8_t *bias, int in, int out,
                    const uint8_t *rowscale) {
    memset(L, 0, sizeof *L);
    L->type = type; L->in = in; L->out = out; L->inpad = pad64(in); L->stride = stride;
    if (type == EVA_LIN_INT16) {     /* Sarah addition: CSSE3Int16Impl init FUN_180126ddc; weight scale set by the caller (wscale) */
        L->W16 = (const int16_t *)W;
        L->bias = (W != bias) ? (const float *)bias : NULL;   /* +0x42: bias only when its offset differs */
        L->q16 = zalloc(sizeof(int16_t) * (size_t)L->inpad);
        return L->q16 ? 0 : -1;
    }
    if (type == EVA_LIN_INT8) {
        L->W = (const int8_t *)W; L->rowscale = (const float *)rowscale; L->bias = (const float *)bias;
        L->q = zalloc((size_t)L->inpad);
        return L->q ? 0 : -1;
    }
    if (type == EVA_LIN_SCALING) {   /* CScaling init FUN_180127000: W = per-dim scale, "bias" = offset */
        L->scale = (const float *)W; L->bias = (const float *)bias;
        return 0;
    }
    return -1;                       /* float / int16 layers do not occur in Eva */
}

/* x86 scalar semantics used by the engine */
static float sse_max(float a, float b) { return a > b ? a : b; }   /* maxss a,b */
static float sse_min(float a, float b) { return a < b ? a : b; }   /* minss a,b */

/* SSE3Int8Impl forward FUN_180052810 (batch 1).  Dynamic symmetric int8 quantisation of the input:
 *   FUN_18005295c: m = max(max x, |min x|) (maxss/minss chain, then comiss), range = 4*m
 *   q[i] = (int8)((int)(x[i] * (127.5/range) + 128.5) + 0x80)          (cvttss2si, byte add)
 *   FUN_1800529cc: s_j = sum_i q[i]*W[j][i] over inpad columns, exact int32 (punpcklbw/psraw/pmaddwd), cvtdq2ps
 *   y_j = rowscale_j * s_j; y_j = (range/127.5) * y_j; y_j = bias_j + y_j */
static void lin8_fwd(const EvaLin *L, const float *x, float *y) {
    float mx = x[0], mn = x[0];
    for (int i = 1; i < L->in; i++) { mx = sse_max(mx, x[i]); mn = sse_min(mn, x[i]); }
    float am = fabsf(mn);
    float m = (mx > am) ? mx : am;
    float range = m * 4.0f;
    float inv = 127.5f / range;
    float deq = range / 127.5f;
    for (int i = 0; i < L->in; i++) {
        float v = inv * x[i];
        v = v + 128.5f;
        int t = (int)v;                              /* cvttss2si */
        L->q[i] = (int8_t)(uint8_t)((unsigned)t + 0x80u);
    }
    for (int j = 0; j < L->out; j++) {
        const int8_t *w = L->W + (size_t)j * L->stride;
        int32_t s = 0;
        for (int i = 0; i < L->inpad; i++) s += (int32_t)L->q[i] * (int32_t)w[i];
        y[j] = (float)s;
    }
    for (int j = 0; j < L->out; j++) y[j] = L->rowscale[2 * j] * y[j];
    for (int j = 0; j < L->out; j++) y[j] = deq * y[j];
    for (int j = 0; j < L->out; j++) y[j] = L->bias[j] + y[j];
}

/* Sarah addition: CSSE3Int16Impl forward FUN_180126200 (batch rows are quantised one by one, so batching changes nothing) [S]; the dot
 * product FUN_1801273a4 [V disasm]: pmaddwd + paddd over pad64(in) columns of the zero-padded input, four int32 lanes
 * summed at the end - an exact int32 sum with wrap-around (FUN_180125580 / FUN_180127340 is the NEON class, a stub on x64):
 *   FUN_180135884: m = max(max x, |min x|), range = 8*m;  FUN_180124c30: inv = 32767.5/range, deq = range/32767.5
 *   FUN_1801358f4: q[i] = (int16)((int)(x[i]*inv + 32768.5) - 0x8000)
 *   y_j = (float)sum_i q[i]*W[j][i];  y_j = wdeq*y_j (wdeq = w*(1/32767.5), w = header +0x24);  y_j = deq*y_j;
 *   y_j = bias_j + y_j when the layer has a bias (+0x42) */
static void lin16_fwd(const EvaLin *L, const float *x, float *y) {
    float mx = x[0], mn = x[0];
    for (int i = 1; i < L->in; i++) { if (mx <= x[i]) mx = x[i]; if (x[i] <= mn) mn = x[i]; }
    float am = fabsf(mn);
    if (mx <= am) mx = am;
    float range = mx * 8.0f;
    float inv = 32767.5f / range, deq = range / 32767.5f;
    for (int i = 0; i < L->in; i++) {
        float v = x[i] * inv;
        v = v + 32768.5f;
        L->q16[i] = (int16_t)(uint16_t)((unsigned)(int)v - 0x8000u);
    }
    for (int j = 0; j < L->out; j++) {
        const int16_t *w = (const int16_t *)((const uint8_t *)L->W16 + (size_t)j * L->stride);
        uint32_t s = 0;                                   /* paddd wraps */
        for (int i = 0; i < L->in; i++) s += (uint32_t)((int32_t)L->q16[i] * (int32_t)w[i]);
        y[j] = (float)(int32_t)s;
    }
    for (int j = 0; j < L->out; j++) y[j] = L->wdeq * y[j];
    for (int j = 0; j < L->out; j++) y[j] = deq * y[j];
    if (L->bias) for (int j = 0; j < L->out; j++) y[j] = L->bias[j] + y[j];
}

/* CScaling forward FUN_1800718d0: y = x*scale + bias */
static void lin_scale_fwd(const EvaLin *L, const float *x, float *y) {
    for (int j = 0; j < L->out; j++) { float t = x[j] * L->scale[j]; y[j] = t + L->bias[j]; }
}

static void lin_fwd(const EvaLin *L, const float *x, float *y) {
    if (L->type == EVA_LIN_INT8) lin8_fwd(L, x, y);
    else if (L->type == EVA_LIN_INT16) lin16_fwd(L, x, y);   /* Sarah */
    else lin_scale_fwd(L, x, y);
}

/* ---- MLPNonLinearTransform: CSigmoidImpl FUN_18006fcc0 / FUN_180135d70, CHyperbolicTangentImpl FUN_180135bd0 /
 * FUN_180135b54.  Both call the CRT (expf, tanhf): 1/(expf(-x)+1).  [bit-exact caveat: UCRT expf/tanhf] ---- */
static float sigm(float x) { float e = expf(-x); return 1.0f / (e + 1.0f); }
static void act_fwd(int act, float *y, int n) {
    if (act == EVA_ACT_SIGMOID) for (int i = 0; i < n; i++) y[i] = sigm(y[i]);
    else if (act == EVA_ACT_TANH) for (int i = 0; i < n; i++) y[i] = tanhf(y[i]);
}

/* ---- layers ---- */

/* CMLPDNNLayer init FUN_1801239a0.  Header (0x28 bytes, data offsets relative to header+0x28):
 *   +0 in, +4 out, +8 activation, +0xc linear type, +0x10 weight row stride, +0x14 weights, +0x18 bias,
 *   +0x1c alignment, +0x20 row scales (8-byte records), +0x24 float (unused by the int8 path) */
static int dnn_init(EvaLayer *l, const uint8_t *h) {
    const uint8_t *d = h + 0x28;
    l->kind = 0; l->in = (int)rd32(h); l->out = (int)rd32(h + 4); l->act = (int)rd32(h + 8);
    int lt = (int)rd32(h + 0xc), stride = (int)rd16(h + 0x10);
    if (l->act != EVA_ACT_NOOP && l->act != EVA_ACT_SIGMOID && l->act != EVA_ACT_TANH) return -1;
    if (lin_init(&l->lin, lt, d + rd32(h + 0x14), stride, d + rd32(h + 0x18), l->in, l->out, d + rd32(h + 0x20)))
        return -1;
    if (lt == EVA_LIN_INT16) {   /* FUN_180126ddc: pf[1] = w * 3.0518044e-5f (0x38000080 = 1/32767.5) */
        float w = rdf(h + 0x24);
        l->lin.wdeq = w * 3.05180438e-5f;
    }
    l->y = zalloc(sizeof(float) * (size_t)pad64(l->out));
    return l->y ? 0 : -1;
}

/* CMLPLSTMSVDLayer init FUN_180123e80 -> FUN_180123ae0, weights FUN_1801240c0 (U, V) + FUN_1801241e0 (projection).
 * Header u32 fields (offsets relative to the header): +0 in, +4 out, +8 0x101, +0xc u16 align, +0xe u16 SVD rank,
 * +0x10 u16 cell, +0x12 u16 projection linear type, +0x14 u16 gate linear type, +0x18 f32 c0, +0x1c f32 h0,
 * +0x28 proj rowscales, +0x30 V rowscales, +0x38 U rowscales, +0x40 gate bias, +0x4c V weights, +0x50 U weights,
 * +0x54 u16 U stride, +0x56 u16 V stride, +0x58 proj weights, +0x5c u16 proj stride (0 = no projection),
 * +0x60 peephole i (nonzero = 4 gates; 0 = coupled input/forget), +0x64 peephole f, +0x68 peephole o */
static int lstm_init(EvaLayer *l, const uint8_t *h) {
    l->kind = 1; l->in = (int)rd32(h); l->out = (int)rd32(h + 4);
    l->rank = rd16(h + 0xe); l->cell = rd16(h + 0x10);
    int ptype = rd16(h + 0x12), gtype = rd16(h + 0x14);
    l->c0 = rdf(h + 0x18); l->h0 = rdf(h + 0x1c);
    if (rd32(h + 0x60) == 0 || rd16(h + 0x5c) == 0) return -1;   /* only the 4-gate projected variant is ported */
    l->catpad = pad64(l->in + l->out); l->rankpad = pad64(l->rank);
    int ng = 4 * l->cell;
    float *zeros = zalloc(sizeof(float) * (size_t)ng);          /* param_1[5]: zero bias for U and P */
    if (!zeros) return -1;
    if (lin_init(&l->U, gtype, h + rd32(h + 0x50), rd16(h + 0x54), (const uint8_t *)zeros, l->catpad, l->rank,
                 h + rd32(h + 0x38))) return -1;
    if (lin_init(&l->V, gtype, h + rd32(h + 0x4c), rd16(h + 0x56), h + rd32(h + 0x40), l->rankpad, ng,
                 h + rd32(h + 0x30))) return -1;
    if (lin_init(&l->P, ptype, h + rd32(h + 0x58), rd16(h + 0x5c), (const uint8_t *)zeros, l->cell, l->out,
                 h + rd32(h + 0x28))) return -1;
    l->peep_i = (const float *)(h + rd32(h + 0x60));
    l->peep_f = (const float *)(h + rd32(h + 0x64));
    l->peep_o = (const float *)(h + rd32(h + 0x68));
    size_t C = (size_t)l->cell;
    l->c = zalloc(4 * C); l->h = zalloc(4 * (size_t)l->out); l->cat = zalloc(4 * (size_t)l->catpad);
    l->urank = zalloc(4 * (size_t)l->rankpad); l->gates = zalloc(4 * (size_t)ng); l->tmp = zalloc(4 * C);
    l->ig = zalloc(4 * C); l->fg = zalloc(4 * C); l->og = zalloc(4 * C); l->cnew = zalloc(4 * C); l->m = zalloc(4 * C);
    l->y = zalloc(4 * (size_t)pad64(l->out));
    return (l->c && l->h && l->cat && l->urank && l->gates && l->tmp && l->ig && l->fg && l->og && l->cnew && l->m && l->y)
        ? 0 : -1;
}

/* LSTM slot 2 FUN_1801243c0 */
static void lstm_reset(EvaLayer *l) {
    for (int j = 0; j < l->cell; j++) l->c[j] = l->c0;
    for (int j = 0; j < l->out; j++) l->h[j] = l->h0;
}

/* LSTM step FUN_180123760: slot 14 FUN_180124300 (SVD gates), FUN_180124550 (i, f), FUN_18012442c (c),
 * FUN_180083814 (o, m, projection), then c_prev = c_new, h_prev = y.
 * Gate blocks in the V output: [0,C) input (peephole +0x60), [C,2C) forget (+0x64), [2C,3C) cell, [3C,4C) output (+0x68). */
static void lstm_fwd(EvaLayer *l, const float *x) {
    int C = l->cell;
    memcpy(l->cat, x, sizeof(float) * (size_t)l->in);
    memcpy(l->cat + l->in, l->h, sizeof(float) * (size_t)l->out);
    lin_fwd(&l->U, l->cat, l->urank);
    lin_fwd(&l->V, l->urank, l->gates);
    /* FUN_180124550 (4-gate branch, flag +0xd3 set) */
    for (int j = 0; j < C; j++) l->tmp[j] = l->c[j] * l->peep_f[j];
    for (int j = 0; j < C; j++) l->tmp[j] = l->gates[C + j] + l->tmp[j];
    for (int j = 0; j < C; j++) l->fg[j] = sigm(l->tmp[j]);                 /* -> +0x78 */
    for (int j = 0; j < C; j++) l->tmp[j] = l->c[j] * l->peep_i[j];
    for (int j = 0; j < C; j++) l->tmp[j] = l->gates[j] + l->tmp[j];
    for (int j = 0; j < C; j++) l->ig[j] = sigm(l->tmp[j]);                 /* -> +0x70 */
    /* FUN_18012442c */
    for (int j = 0; j < C; j++) l->tmp[j] = l->fg[j] * l->c[j];
    for (int j = 0; j < C; j++) l->gates[2 * C + j] = tanhf(l->gates[2 * C + j]);
    for (int j = 0; j < C; j++) l->gates[2 * C + j] = l->ig[j] * l->gates[2 * C + j];
    for (int j = 0; j < C; j++) l->cnew[j] = l->tmp[j] + l->gates[2 * C + j];
    /* FUN_180083814 */
    for (int j = 0; j < C; j++) l->tmp[j] = l->cnew[j] * l->peep_o[j];
    for (int j = 0; j < C; j++) l->tmp[j] = l->gates[3 * C + j] + l->tmp[j];
    for (int j = 0; j < C; j++) l->og[j] = sigm(l->tmp[j]);                 /* -> +0x88 */
    for (int j = 0; j < C; j++) l->tmp[j] = tanhf(l->cnew[j]);
    for (int j = 0; j < C; j++) l->m[j] = l->og[j] * l->tmp[j];             /* -> +0xa0 */
    lin_fwd(&l->P, l->m, l->y);
    memcpy(l->c, l->cnew, sizeof(float) * (size_t)C);
    memcpy(l->h, l->y, sizeof(float) * (size_t)l->out);
}

/* ---- file ---- */

/* TDAT (FUN_180124b18, FUN_1801249e4 / FUN_180124854 version 3):
 *   +0x00 GUID(16), +0x10 u32 version (3), +0x14 u32 offset of {u32 in, u32 out}, +0x18 u32 offset of the layer
 *   table {u32 n; u32 off[n]} (layer offsets relative to the table), +0x1c optional (0) */
EvaNet *eva_net_load(const char *path, char *err, int errlen) {
    EvaNet *n = zalloc(sizeof *n);
    if (!n) return NULL;
    n->file = eva_map_file(path, &n->size, &n->maplen);
    if (!n->file) { snprintf(err, errlen, "cannot read %s", path); free(n); return NULL; }
    const uint8_t *d = n->file;
    if (n->size < 0x30 || rd32(d + 0x10) != 3) { eva_net_free(n); snprintf(err, errlen, "not a version-3 TDAT"); return NULL; }
    n->in = (int)rd32(d + rd32(d + 0x14)); n->out = (int)rd32(d + rd32(d + 0x14) + 4);
    uint32_t t = rd32(d + 0x18);
    n->nlayers = (int)rd32(d + t);
    n->L = zalloc(sizeof(EvaLayer) * (size_t)n->nlayers);
    for (int i = 0; i < n->nlayers; i++) {
        uint32_t o = t + rd32(d + t + 4 + 4 * i);
        if (o + 0x70 > (uint32_t)n->size) { snprintf(err, errlen, "layer %d out of range", i); eva_net_free(n); return NULL; }
        uint32_t type = rd32(d + o + 8);
        int r = type < 0xff ? dnn_init(&n->L[i], d + o) : type == 0x101 ? lstm_init(&n->L[i], d + o) : -1;
        if (r) { snprintf(err, errlen, "layer %d (type %#x) unsupported", i, type); eva_net_free(n); return NULL; }
    }
    eva_net_reset(n);
    return n;
}

void eva_net_reset(EvaNet *n) {
    for (int i = 0; i < n->nlayers; i++) if (n->L[i].kind == 1) lstm_reset(&n->L[i]);
}

const float *eva_net_forward(EvaNet *n, const float *x) {
    const float *cur = x;
    for (int i = 0; i < n->nlayers; i++) {
        EvaLayer *l = &n->L[i];
        if (l->kind == 0) { lin_fwd(&l->lin, cur, l->y); act_fwd(l->act, l->y, l->out); }
        else lstm_fwd(l, cur);
        cur = l->y;
    }
    return cur;
}

static void lin_free(EvaLin *L) { free(L->q); free(L->q16); }
void eva_net_free(EvaNet *n) {
    if (!n) return;
    for (int i = 0; n->L && i < n->nlayers; i++) {
        EvaLayer *l = &n->L[i];
        lin_free(&l->lin); lin_free(&l->U); lin_free(&l->V); lin_free(&l->P);
        if (l->kind == 1) free((void *)l->U.bias);
        free(l->c); free(l->h); free(l->cat); free(l->urank); free(l->gates); free(l->tmp); free(l->ig); free(l->fg);
        free(l->og); free(l->cnew); free(l->m); free(l->y);
    }
    free(n->L); eva_unmap_file(n->file, n->maplen); free(n);
}
