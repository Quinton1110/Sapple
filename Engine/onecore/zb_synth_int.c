/* zb_synth_int.c - fixed-point path of the OneCore acoustic backend (CTTSAcousticSpaceImpl<int>): OneCore Zira and Mark.
 *
 * Differences from the float path (David), all transcribed from the decompile (see notes/backend_port.md):
 *  - quantised pools: F0 state = {f32 w, i16 mean[3], u8 prec[3]} (runtime weight = (short)(w*16384+0.5)),
 *    LSF state = 156-byte record decoded by FUN_1800451a0 (u8 cumulative LSF means, i8 deltas, i16 gains).
 *  - UV decision on Q14 weights (FUN_1800a51b0 / FUN_1800a49a0).
 *  - MLPG in integers (FUN_18000c0f0 generic branch, 3 windows): band W^T P W and W^T P mu in int with Q4 window
 *    coefficients (FUN_18000c6d0), LDL^T in double (FUN_18000cf70, same as the float path), back to int (D, L*2^28),
 *    integer substitution (FUN_18000bd90). F0 x7 (Q15 log F0), last LSF dim (gain) x10.
 *  - F0: FIR smoothing and EnhanceRate in Q15 (FUN_1800a66d0 / FUN_1800a5a80), optional QuestionIntonation ramp
 *    (FUN_1800a5e14), export to integer Hz through the 800-entry table (FUN_1800ad804).
 *  - LSPDecomposing DelayMode=1: LSF+gain are generated in overlapping phone chunks (FUN_180110394 / FUN_18010f584,
 *    LsfBaseModelStep 4, LsfOverlappedStep 3, LsfOverlappedModelRange 7) and each chunk is vocoded as it is produced.
 *  - fixed-point vocoder (zb_vocoder_int.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zb_internal.h"
#include "zb_voctab_int.h"

#define IMAXDIM 75

typedef struct {
    int64_t m[IMAXDIM];
    int32_t p[IMAXDIM];
    short w;
} ZbIG;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

/* FUN_1800451a0 (LSF, stream 1: u8/i8/i16 means) and FUN_1800adea4 (F0 / MBE: i16 means): T = dims (nwin*P), P = static */
static void idecode(ZbIG *g, const uint8_t *mb, const uint8_t *pb, int stream, int T, int P, int i16means)
{
    int64_t acc = 0;
    int i;
    for (i = 0; i < T; i++) {
        uint32_t u = (uint32_t)pb[i] * pb[i], u2;
        int64_t m;
        if (i < P) {
            u2 = stream == 2 ? u >> 2 : u;
            if (u2 < 2) u2 = 1;
        } else
            u2 = u * 0x100;
        if (i16means) m = (int16_t)(mb[2 * i] | mb[2 * i + 1] << 8);
        else if ((i + 1) % P == 0) {
            const uint8_t *q = mb + i + i / P;
            m = (int16_t)(q[0] | q[1] << 8);
            if (m < 0 && i + 1 == P) m = 0;
        } else if (i < P) {
            acc += mb[i];
            m = (P < 41 ? 8 : 4) * acc;
        } else
            m = (int64_t)(int8_t)mb[i + 1] * (P < 41 ? 4 : 2);
        if (P <= i && 2 * P == T) m *= 2;
        g->m[i] = m;
        g->p[i] = (int32_t)(u2 * 2);
    }
}

/* frame list (FUN_180008a6c): copies of the state Gaussians with window slices zeroed at the edges / unvoiced taps */
static int build_iframes(const ZbModel *m, const ZbIG *st, const int *dur, int nst, int T, const int *mask, int D, ZbIG **fr,
                         ZbIG *pool)
{
    int k, j, t = 0, n = 0;
    for (k = 0; k < nst; k++) {
        for (j = 0; j < dur[k]; j++, t++) {
            const ZbIG *g = &st[k];
            ZbIG *c = NULL;
            int w;
            if (mask && !mask[t]) continue;
            for (w = 0; w < m->nwin; w++) {
                int half = m->winw[w] / 2, tap;
                for (tap = -half; tap <= m->winw[w] - 1 - half; tap++) {
                    int tt = t + tap;
                    if (tt < 0 || tt >= T || (m->win[w][half + tap] != 0.0f && mask && !mask[tt])) {
                        if (!c) { c = &pool[n]; *c = *g; }
                        memset(c->m + w * D, 0, sizeof(int64_t) * D);
                        memset(c->p + w * D, 0, sizeof(int32_t) * D);
                        break;
                    }
                }
            }
            fr[n] = c ? c : (ZbIG *)g;
            n++;
        }
    }
    return n;
}

static double fabs_d(double x) { return x < 0 ? -x : x; }
#define GUARD(x) do { if (fabs_d((x) - 0.0) <= 1e-6) (x) = 1.0; } while (0)

/* FUN_18000c0f0, generic branch, one output dimension d. out: [T][D] ints. stream: 1 LSF, 2 F0. */
static void imlpg_dim(const ZbModel *m, ZbIG *const *fr, int T, int D, int d, int stream, int32_t (*Wi)[3], double (*W)[3],
                      int64_t *R, int64_t *g, int32_t *out)
{
    int t, j, i, k, run;
    const int width = 3;
    /* FUN_18000c6d0 */
    for (t = 0; t < T; t++) {
        int64_t r = 0;
        int idx = d;
        Wi[t][0] = Wi[t][1] = Wi[t][2] = 0;
        for (j = 0; j < m->nwin; j++, idx += D) {
            int wj = m->winw[j], half = wj / 2, left = -half, right = wj - 1 - half;
            for (i = left; i <= right; i++) {
                float cf = m->win[j][half - i];
                if (cf != 0.0f && t + i >= 0 && t + i < T) {
                    int c16 = (int)(cf * 16.0f), P = fr[t + i]->p[idx], n = right + 1 + i;
                    r += (((int64_t)(P >> 1) * fr[t + i]->m[idx]) * (int64_t)c16) >> 4;
                    if (T - t < n) n = T - t;
                    if (wj < n) n = wj;
                    for (k = 0; k < n; k++)
                        Wi[t][k] += (int)(((int64_t)(int)(m->win[j][half - i + k] * 16.0f) * ((int64_t)(int32_t)(P * c16) >> 4)) >> 4);
                }
            }
        }
        R[t] = r;
    }
    /* int -> double, LDL^T (FUN_18000cf70), back to int */
    for (t = 0; t < T; t++)
        for (k = 0; k < width; k++) W[t][k] = (double)Wi[t][k];
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
    for (t = 0; t < T; t++) {
        Wi[t][0] = (int32_t)W[t][0];
        for (k = 1; k < width; k++) Wi[t][k] = (int32_t)(W[t][k] * 268435456.0);
    }
    /* FUN_18000bd90 */
    g[0] = R[0] * 2;
    for (t = 1; t < T; t++) {
        int64_t acc = 0;
        int nn = t + 1 < width ? t + 1 : width;
        for (j = 1; j < nn; j++) acc += (int64_t)Wi[t - j][j] * g[t - j];
        g[t] = R[t] * 2 - (acc >> 28);
    }
    for (t = 0; t < T; t++) g[t] = ((int64_t)(Wi[t][0] / 2) + g[t]) / (int64_t)Wi[t][0];
    out[(T - 1) * D + d] = (int32_t)g[T - 1];
    for (t = T - 2; t >= 0; t--) {
        int64_t acc = 0;
        int nn = T - t < width ? T - t : width;
        for (j = 1; j < nn; j++) acc += (int64_t)Wi[t][j] * (int64_t)out[(t + j) * D + d];
        out[t * D + d] = (int32_t)g[t] - (int32_t)(acc >> 28);
    }
    /* post steps */
    if (stream != 2) {
        run = 0;
        for (t = 0; t < T; t++) {
            if (fr[t]->p[d + D] == 0) {
                run += run < 2;
                if (run > 1) out[t * D + d] = (int32_t)fr[t]->m[d];
            } else
                run = 0;
        }
    }
    if (stream == 2)
        for (t = 0; t < T; t++) out[t * D + d] *= 7;
    else if (stream == 1 && d == D - 1)
        for (t = 0; t < T; t++) out[t * D + d] *= 10;
}

static void imlpg(const ZbModel *m, ZbIG *const *fr, int T, int D, int stream, int32_t *out)
{
    int32_t (*Wi)[3];
    double (*W)[3];
    int64_t *R, *g;
    int d;
    if (T <= 0) return;
    Wi = (int32_t (*)[3])malloc(sizeof(int32_t) * 3 * (size_t)T);
    W = (double (*)[3])malloc(sizeof(double) * 3 * (size_t)T);
    R = (int64_t *)malloc(sizeof(int64_t) * (size_t)T);
    g = (int64_t *)malloc(sizeof(int64_t) * (size_t)T);
    for (d = 0; d < D; d++) imlpg_dim(m, fr, T, D, d, stream, Wi, W, R, g, out);
    free(Wi); free(W); free(R); free(g);
}

/* FUN_1800a66d0 with isInt=1: Q15 FIR within voiced runs over frames [t0, t1) */
static void f0_fir_int(int32_t *f0, int T, const int *w, int N, int t0, int t1)
{
    int32_t *x = (int32_t *)malloc(sizeof(int32_t) * (size_t)(T ? T : 1));
    int t, k, h = N / 2, all = 0;
    memcpy(x, f0, sizeof(int32_t) * T);
    for (k = 0; k < N; k++) all += w[k];
    for (t = t0; t < t1; t++) {
        int lo, hi, acc = 0, used = 0;
        if (x[t] == 0) continue;
        for (lo = 0; lo - 1 >= -h && t + lo - 1 >= 0 && x[t + lo - 1] != 0; lo--);
        for (hi = 0; hi + 1 <= N - h - 1 && t + hi + 1 < T && x[t + hi + 1] != 0; hi++);
        for (k = lo; k <= hi; k++) {
            acc += (int)(((int64_t)x[t + k] * w[h + k]) / 0x8000);
            used += w[h + k];
        }
        f0[t] = used == 0 ? 0 : (int32_t)(((int64_t)all * acc) / used);
    }
    free(x);
}

/* first voiced frame at/after the start of the question-intonation anchor phone, or -1 (FUN_1800a5b70). The utterance's
 * last word node (the final silence) must carry sentence type 3 or 5 (yes/no question; wh-questions are 4); the anchor
 * is the first phone of the last syllable of the preceding word (runtime-verified: "Is it raining?" -> 'n'). */
static int qi_anchor(const ZbUtt *u, const int *dur, int nstate, const int32_t *f0, int T)
{
    int p, a = -1, t, s, L = u->nphone - 1;
    if (L < 1 || (u->ph[L].c.sent_type != 3 && u->ph[L].c.sent_type != 5)) return -1;
    for (p = L - 1; p >= 0; p--)
        if (!u->ph[p].c.is_sil && u->ph[p].f[ZB_F_FwPosInSyllable] == 1) { a = p; break; }
    if (a < 0) return -1;
    for (p = 0, t = 0; p < a; p++)
        for (s = 0; s < nstate; s++) t += dur[p * nstate + s];
    for (; t < T; t++)
        if (f0[t] != 0) return t;
    return -1;
}

/* int LSF repair (FUN_18006cc7c; FUN_1800a18c4 sort, FUN_1800a17c4 re-spacing, FUN_1800a1768 fill) */
static void ilinfill(int32_t *x, unsigned n, int a, int b)
{
    unsigned i;
    for (i = 0; i < n; i++) x[i] = (int32_t)((double)(i + 1) * ((double)(b - a) / (double)(n + 1)) + (double)a);
}
static void ilsf_respace(int32_t *x, unsigned n)
{
    unsigned i = 0;
    while (i < n && !((uint32_t)(x[i] - 1) < 0x3fff)) i++;
    if (i != 0) ilinfill(x, i, 0, i < n ? x[i] : 0x4000);
    for (; i < n; i++)
        if (0x3ffe < (uint32_t)(x[i] - 1)) { ilinfill(x + i, n - i, x[(int)i - 1], 0x4000); break; }
}
void zb_ilsf_repair(int32_t *lsf, int rows, int p)
{
    int r, k;
    for (r = 0; r < rows; r++) {
        int32_t *x = lsf + (size_t)r * p;
        int sorted = 1, bad = 0;
        for (k = 1; k < p; k++)
            if (x[k] < x[k - 1]) { sorted = 0; break; }
        if (!sorted) {
            int swapped;
            do {
                swapped = 0;
                for (k = 1; k < p; k++)
                    if (x[k] < x[k - 1]) { int32_t tt = x[k - 1]; x[k - 1] = x[k]; x[k] = tt; swapped = 1; }
            } while (swapped);
        }
        for (k = 0; k < p; k++)
            if (0x4000u < (uint32_t)x[k]) { bad = 1; break; }
        if (bad) {
            if (r) memcpy(x, x - p, sizeof(int32_t) * p);
            else ilsf_respace(x, (unsigned)p);
        }
    }
}

typedef struct {
    int nph, nst, T;
    ZbGauss *gdur, *gpdur;
    ZbIG *gf0, *glsf;
    int *dur, *stretch, *pstart;   /* pstart[p] = first frame of phone p (nph+1 entries) */
} ZbIUtt;

static int ilookup(ZbVoice *v, const ZbUtt *u, ZbIUtt *um)
{
    int p, s;
    const int ns = v->m_lf0->nstate, P = v->m_lsf->streamdim[0] / 3;
    um->nph = u->nphone;
    um->nst = ns;
    um->gdur = (ZbGauss *)calloc((size_t)u->nphone, sizeof(ZbGauss));
    um->gpdur = v->m_pdur ? (ZbGauss *)calloc((size_t)u->nphone, sizeof(ZbGauss)) : NULL;
    um->gf0 = (ZbIG *)calloc((size_t)u->nphone * ns, sizeof(ZbIG));
    um->glsf = (ZbIG *)calloc((size_t)u->nphone * ns, sizeof(ZbIG));
    for (p = 0; p < u->nphone; p++) {
        const int *f = u->ph[p].f;
        int id = f[ZB_F_PhoneIdentity];
        const uint8_t *leaf = zb_tree_leaf(v, v->m_dur, id, 0, f);
        if (!leaf || zb_state_gauss(v, v->m_dur, leaf, &um->gdur[p])) return -1;
        if (v->m_pdur) {
            leaf = zb_tree_leaf(v, v->m_pdur, id, 0, f);
            if (!leaf || zb_state_gauss(v, v->m_pdur, leaf, &um->gpdur[p])) return -1;
        }
        for (s = 0; s < ns; s++) {
            const uint8_t *e;
            ZbIG *g = &um->gf0[p * ns + s];
            leaf = zb_tree_leaf(v, v->m_lf0, id, s, f);
            if (!leaf) return -1;
            e = v->m_lf0->pool + rd32(leaf + 4);          /* {f32 w, i16 m[3], u8 p[3], pad} */
            g->w = (short)(int)(rdf(e) * 16384.0f + 0.5f);
            idecode(g, e + 4, e + 10, 2, 3, 1, 1);
            leaf = zb_tree_leaf(v, v->m_lsf, id, s, f);
            if (!leaf) return -1;
            e = v->m_lsf->pool + rd32(leaf + 4);          /* 3P+3 bytes of means, then precisions */
            idecode(&um->glsf[p * ns + s], e, e + (3 * P + 3), 1, 3 * P, P, 0);
        }
    }
    return 0;
}

/* FUN_1800a51b0 / FUN_1800a49a0: frame-level UV on Q14 weights */
static void iuv(const ZbIUtt *um, short thr, int *vuv)
{
    int k, j, t = 0, nst = um->nph * um->nst;
    short prev = 0, cur = 0;
    for (k = 0; k < nst; k++) {
        short next = (short)(k + 1 < nst ? um->gf0[k + 1].w : 0);
        unsigned d = (unsigned)um->dur[k];
        if (thr < cur || (thr < prev && thr < next))
            for (j = 0; j < (int)d; j++) vuv[t++] = 1;
        else {
            unsigned n = (unsigned)((int)cur * (int)d + 0x2000) >> 14;
            for (j = 0; j < (int)d; j++)
                vuv[t++] = ((thr < prev && (unsigned)j < n) || (thr < next && d - (unsigned)j <= n)) ? 1 : 0;
        }
        prev = cur;
        cur = next;
    }
}

/* LSF/gain generation for phones [start, start+count) (FUN_18010f584); returns count, updates *overlap */
static int lsf_chunk(const ZbVoice *v, const ZbIUtt *um, int start, int *overlap, int32_t *lsf, int32_t *gain)
{
    const int base = 4, ovstep = 3, range = 7, P = v->m_lsf->streamdim[0] / 3, nst = um->nst;
    int nph = um->nph, oldov = *overlap, first, k, end, lo, win, ctx, cnt, nfr, f0, t;
    unsigned best = 0, prev = 0;
    ZbIG **fr, *pool;
    int32_t *out;
    first = start != 0 ? base : 1;
    *overlap = ovstep;
    k = first + start;
    end = k;
    {
        int lim = k + range < nph ? k + range : nph;
        for (; k < lim; k++) {
            unsigned dk = 0, diff, sc;
            int s;
            for (s = 0; s < nst; s++) dk += (unsigned)um->dur[k * nst + s];
            diff = dk > prev ? dk - prev : prev - dk;
            sc = (unsigned)(((unsigned long long)((dk + prev) * (dk + prev))) / (unsigned long long)(diff + 5));
            if (best < sc) { end = k; best = sc; }
            prev = dk;
        }
    }
    if ((unsigned)(nph - end) < (unsigned)base) { *overlap = 0; end = nph; }
    if (start < oldov) { lo = 0; ctx = start; }
    else { lo = start - oldov; ctx = oldov; }
    win = end + (*overlap - lo);
    if (lo + win > nph) win = nph - lo;
    cnt = win - *overlap - ctx;
    /* MLPG over the frames of phones [lo, lo+win) */
    nfr = um->pstart[lo + win] - um->pstart[lo];
    fr = (ZbIG **)malloc(sizeof(ZbIG *) * (size_t)(nfr + 1));
    pool = (ZbIG *)malloc(sizeof(ZbIG) * (size_t)(nfr + 1));
    out = (int32_t *)calloc((size_t)nfr * P + 1, sizeof(int32_t));
    build_iframes(v->m_lsf, um->glsf + lo * nst, um->dur + lo * nst, win * nst, nfr, NULL, P, fr, pool);
    imlpg(v->m_lsf, fr, nfr, P, 1, out);
    f0 = um->pstart[lo + ctx] - um->pstart[lo];
    for (t = um->pstart[start]; t < um->pstart[start + cnt]; t++) {
        memcpy(lsf + (size_t)t * (P - 1), out + (size_t)(t - um->pstart[start] + f0) * P, sizeof(int32_t) * (P - 1));
        gain[t] = out[(size_t)(t - um->pstart[start] + f0) * P + P - 1];
    }
    free(fr); free(pool); free(out);
    return cnt;
}

int zb_synth_int(ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, zb_write_fn write, void *user, ZbTrace *trace)
{
    ZbIUtt um;
    int ns, nst, T = 0, k, t, p, nv, rc = -1, start, ov, *phfr = NULL, *blk = NULL, nblk = 0, cap = 0;
    int *vuv = NULL;
    int32_t *lf0 = NULL, *f0c = NULL, *lsf = NULL, *gain = NULL;
    ZbIG **fr = NULL, *pool = NULL;
    short *pcm = NULL, *wav = NULL;
    long nwav = 0;
    const int order = v->m_lsf->streamdim[0] / 3 - 1;
    memset(&um, 0, sizeof um);
    if (ilookup(v, u, &um)) goto done;
    ns = um.nst;
    nst = u->nphone * ns;
    um.dur = (int *)calloc((size_t)nst, sizeof(int));
    um.stretch = (int *)calloc((size_t)u->nphone + 1, sizeof(int));
    um.pstart = (int *)calloc((size_t)u->nphone + 1, sizeof(int));
    zb_durations_ex(v, u, sapi, um.gdur, um.gpdur, ns, um.dur, um.stretch);
    for (p = 0; p < u->nphone; p++) {
        um.pstart[p] = T;
        for (k = 0; k < ns; k++) T += um.dur[p * ns + k];
    }
    um.pstart[u->nphone] = T;
    /* UV */
    vuv = (int *)calloc((size_t)T + 1, sizeof(int));
    if (v->cfg.uv_frame_level) iuv(&um, (short)(int)(v->cfg.voiced_thr * 16384.0f + 0.5f), vuv);
    else
        for (k = 0, t = 0; k < nst; k++) { int j; for (j = 0; j < um.dur[k]; j++) vuv[t++] = 8192 < um.gf0[k].w; }
    /* F0 (FUN_1800a615c) */
    fr = (ZbIG **)malloc(sizeof(ZbIG *) * (size_t)(T + 1));
    pool = (ZbIG *)malloc(sizeof(ZbIG) * (size_t)(T + 1));
    lf0 = (int32_t *)calloc((size_t)T + 1, sizeof(int32_t));
    f0c = (int32_t *)calloc((size_t)T + 1, sizeof(int32_t));
    nv = build_iframes(v->m_lf0, um.gf0, um.dur, nst, T, vuv, 1, fr, pool);
    imlpg(v->m_lf0, fr, nv, 1, 2, f0c);
    for (t = 0, k = 0; t < T; t++) lf0[t] = vuv[t] ? f0c[k++] : 0;
    if (v->m_lf0->has_gen) {
        int w[64], mean = (int)(v->m_lf0->gen_mean * 32768.0f), rate = (int)(v->m_lf0->gen_enhance * 32768.0f);
        for (k = 0; k < v->m_lf0->genlen; k++) w[k] = (int)(v->m_lf0->genw[k] * 32768.0f);
        f0_fir_int(lf0, T, w, v->m_lf0->genlen, 0, T);
        for (t = 0; t < T; t++)
            if (lf0[t] != 0) lf0[t] = (int32_t)(((int64_t)(lf0[t] - mean) * rate) / 0x8000) + mean;
    }
    if (v->cfg.qi_enabled) {   /* FUN_1800a5e14 */
        int s = qi_anchor(u, um.dur, ns, lf0, T);
        if (s >= 0) {
            int e = T, step = (int)(v->cfg.qi_rate * 32768.0f), mx = (int)(v->cfg.qi_max * 32768.0f), dlt, i;
            int taps[11];
            do { e--; } while (e != 0 && lf0[e] == 0);
            if (e == 0) e = s;
            dlt = (e - s) * step;
            if (mx < lf0[s] + dlt) lf0[s] = mx - dlt;
            for (t = s, i = 0; t <= e; t++, i += step)
                if (lf0[t] != 0) lf0[t] = lf0[s] + i;
            for (i = 0; i < 11; i++) taps[i] = 0xba2;
            f0_fir_int(lf0, T, taps, 11, s < 0x15 ? 0 : s - 0x14, s + 0x14 < T ? s + 0x14 : T);
        }
    }
    if (trace) {
        trace->nphone = u->nphone; trace->nstate = ns; trace->nframe = T; trace->order = order;
        trace->dur = (int *)malloc(sizeof(int) * nst); memcpy(trace->dur, um.dur, sizeof(int) * nst);
        trace->vuv = (int *)malloc(sizeof(int) * (T + 1)); memcpy(trace->vuv, vuv, sizeof(int) * T);
        trace->ilf0 = (int *)malloc(sizeof(int) * (T + 1)); memcpy(trace->ilf0, lf0, sizeof(int) * T);
    }
    /* export (FUN_1800a2488): log F0 (Q15) -> Hz */
    for (t = 0; t < T; t++)
        lf0[t] = lf0[t] < 1 ? 0 : zb_f0exp800[(lf0[t] + 0x80) / 0x100 > 799 ? 799 : (lf0[t] + 0x80) / 0x100];
    /* pitch (FUN_1800a0988 / FUN_1800a0fac) and pitch range (FUN_1800a274c) on integer Hz */
    {
        const ZbConfig *cfg = &v->cfg;
        /* + the emotion recipe's PitchAdjustment / PitchRangeScale (ZbStyle, zb.h) */
        int pa_adj = zb_style_pct(cfg->pitch_adj, sapi ? sapi->style.pitch : 0);
        unsigned R = (unsigned)zb_style_pct(cfg->pitch_range_scale, sapi ? sapi->style.range : 0);
        float rr = 1.0f;
        for (p = 0, t = 0; p < u->nphone; p++) {
            int n = um.pstart[p + 1] - um.pstart[p], pa = u->ph[p].c.pitch, i;
            if (pa != 0 || pa_adj != 100) {
                float kf = 1.0f * zb_pow2_24th(pa < -10 ? -10 : (pa > 10 ? 10 : pa)) * 1.0f;
                unsigned P = (unsigned)pa_adj;
                if (P != 100) kf = ((float)P * kf) / 100.0f;
                for (i = t; i < t + n; i++)
                    if (3.0f < (float)lf0[i]) {
                        float y = (float)lf0[i] * kf, z = 3.0f;
                        if (3.0f <= y) z = y;
                        if (8000.0f <= z) z = 8000.0f;
                        lf0[i] = (int32_t)z;
                    }
            }
            t += n;
        }
        if (u->nphone && u->ph[0].c.pitch_range != 0.0f) rr = u->ph[0].c.pitch_range;
        if (rr != 1.0f || R != 100) {
            float s = ((float)R / 100.0f) * rr;
            unsigned cnt = 0, sum = 0;
            for (t = 0; t < T; t++)
                if (3.0f < (float)lf0[t]) { cnt++; sum += (unsigned)lf0[t]; }
            if (cnt) {
                int mean = (int)(sum / cnt);
                for (t = 0; t < T; t++)
                    if (3.0f < (float)lf0[t]) {
                        float y = (float)(lf0[t] - mean) * s + (float)mean, z = 3.0f;
                        if (3.0f <= y) z = y;
                        if (8000.0f <= z) z = 8000.0f;
                        lf0[t] = (int32_t)z;
                    }
            }
        }
    }
    if (trace) { trace->if0 = (int *)malloc(sizeof(int) * (T + 1)); memcpy(trace->if0, lf0, sizeof(int) * T); }
    /* LSF chunks + vocoder */
    lsf = (int32_t *)calloc((size_t)T * order + 1, sizeof(int32_t));
    gain = (int32_t *)calloc((size_t)T + 1, sizeof(int32_t));
    pcm = (short *)calloc((size_t)T * v->shift + 1, sizeof(short));
    if (trace) {
        trace->ilsf = (int *)calloc((size_t)T * order + 1, sizeof(int));
        trace->igain = (int *)calloc((size_t)T + 1, sizeof(int));
    }
    phfr = (int *)calloc((size_t)u->nphone + 1, sizeof(int));
    blk = (int *)calloc((size_t)u->nphone + 1, sizeof(int));
    for (p = 0; p < u->nphone; p++) {
        phfr[p] = um.pstart[p + 1] - um.pstart[p];
        if (cap < phfr[p]) cap = phfr[p];
    }
    for (start = 0, ov = 0; start < u->nphone;) {
        int cnt = lsf_chunk(v, &um, start, &ov, lsf, gain), a = um.pstart[start], b = um.pstart[start + cnt];
        if (trace) {
            memcpy(trace->ilsf + (size_t)a * order, lsf + (size_t)a * order, sizeof(int) * (size_t)(b - a) * order);
            memcpy(trace->igain + a, gain + a, sizeof(int) * (size_t)(b - a));
        }
        {
            int nb = zb_split_blocks(u, phfr, start, cnt, um.stretch, cap, blk + nblk);
            zb_vocoder_int_chunk(v, u, phfr + start, blk + nblk, nb, start == 0, start + cnt >= u->nphone, lf0, lsf, gain, a,
                                 b - a, pcm);
            nblk += nb;
        }
        start += cnt;
    }
    zb_vocoder_int_reset(v);
#ifndef CV_NO_DEBUG_ENV   /* ClassicVoices patch: no debug file dumps in the app */
    if (getenv("ZB_DUMP_ILSF")) {   /* debug: LSF/gain as vocoded (after edge copy, sharpening, repair) */
        FILE *df = fopen(getenv("ZB_DUMP_ILSF"), "ab");
        if (df) { fwrite(&T, 4, 1, df); fwrite(lsf, 4, (size_t)T * order, df); fwrite(gain, 4, (size_t)T, df); fclose(df); }
    }
#endif
    zb_silence_zero(v, u, um.dur, ns, pcm);
    nwav = zb_wave_sps(v, u, sapi, um.dur, ns, um.stretch, blk, nblk, pcm, &wav);
    rc = 0;
    if (write && nwav) rc = write(user, wav, (int)nwav);
done:
    free(um.gdur); free(um.gpdur); free(um.gf0); free(um.glsf); free(um.dur); free(um.stretch); free(um.pstart);
    free(vuv); free(lf0); free(f0c); free(lsf); free(gain); free(fr); free(pool); free(pcm); free(wav); free(phfr); free(blk);
    return rc;
}
