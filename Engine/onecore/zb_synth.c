/* zb_synth.c - per-utterance driver: model lookup (FUN_1800a2bd4 / FUN_18000246c), predictors in engine order
 * (Duration, UV, F0, LSF, Gain: FUN_1800653c4), export (FUN_1800a25f4), vocoder. Float path (David). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "zb_internal.h"

void zb_lsf_repair(float *lsf, int rows, int p);
void zb_apply_sapi_volume(const ZbUtt *u, const int *dur, int nstate, int shift, int sitevol, short *pcm);
int zb_synth_int(ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, zb_write_fn write, void *user, ZbTrace *trace);

ZbVoice *zb_voice_load(const char *voicepath, char *err, int errlen)
{
    ZbVoice *v = (ZbVoice *)calloc(1, sizeof *v);
    char path[1024];
    if (!v) return NULL;
    snprintf(path, sizeof path, "%s.APM", voicepath);
    if (zb_apm_load(v, path, err, errlen)) { zb_voice_free(v); return NULL; }
    snprintf(path, sizeof path, "%s.INI", voicepath);
    if (zb_ini_load(v, path)) { snprintf(err, errlen, "cannot read %s", path); zb_voice_free(v); return NULL; }
    zb_vocoder_init(v);
    zb_srand(v, 0x406);
    return v;
}

void zb_voice_free(ZbVoice *v)
{
    int i;
    if (!v) return;
    for (i = 0; i < v->nq; i++) free(v->q[i].val);
    free(v->q);
    for (i = 0; i < v->nmodel; i++) { free(v->model[i].treeid); free(v->model[i].suboff); }
    free(v->apm);
    zb_wave_free(v);
    free(v);
}

int zb_voice_is_int(const ZbVoice *v) { return v->isint; }
/* ClassicVoices addition: 1 if the APM has the LSF and LF0 models (0 = Eva's duration-only APM) */
int zb_voice_has_acoustics(const ZbVoice *v) { return v->m_lsf && v->m_lf0; }

void zb_trace_free(ZbTrace *t)
{
    if (!t) return;
    free(t->dur); free(t->vuv); free(t->lf0); free(t->lsf); free(t->gain); free(t->f0); free(t->lsf_fin);
    free(t->ilf0); free(t->ilsf); free(t->igain); free(t->if0);
    memset(t, 0, sizeof *t);
}

/* UV decision, FrameLevel=yes (FUN_18004d258). w[k] per global state; quirk: state 0 reads as 0. */
static void uv_frame_level(const float *w, const int *dur, int nst, float thr, int *vuv)
{
    int k, j, t = 0;
    float prev = 0.0f, cur = 0.0f;
    for (k = 0; k < nst; k++) {
        float next = (k + 1 < nst) ? w[k + 1] : 0.0f;
        int d = dur[k];
        if (thr < cur || (thr < prev && thr < next)) {
            for (j = 0; j < d; j++) vuv[t++] = 1;
        } else {
            unsigned nn = (unsigned)(long long)((float)(long long)d * cur + 0.5f);
            for (j = 0; j < d; j++) vuv[t++] = ((thr < prev && (unsigned)j < nn) || (thr < next && (unsigned)(d - j) <= nn)) ? 1 : 0;
        }
        prev = cur;
        cur = next;
    }
}
static void uv_state_level(const float *w, const int *dur, int nst, float thr, int *vuv)
{
    int k, j, t = 0;
    for (k = 0; k < nst; k++)
        for (j = 0; j < dur[k]; j++) vuv[t++] = thr < w[k];
}

/* Frame list for MLPG (FUN_180007f00): frames of included (masked) global frames; window slices zeroed where a
 * tap with coef != 0 hits an unvoiced frame (mask) or any tap leaves [0,T). Returns number of frames. */
static int build_frames(const ZbModel *m, const ZbGauss *st, const int *dur, int nst, int T, const int *mask, int D, ZbGauss **fr,
                        ZbGauss *pool)
{
    int k, j, t = 0, n = 0;
    for (k = 0; k < nst; k++) {
        for (j = 0; j < dur[k]; j++, t++) {
            const ZbGauss *g = &st[k];
            ZbGauss *c = NULL;
            int w;
            if (mask && !mask[t]) continue;
            for (w = 0; w < m->nwin; w++) {
                int half = m->winw[w] / 2, tap;
                for (tap = -half; tap <= m->winw[w] - 1 - half; tap++) {
                    int tt = t + tap;
                    if (tt < 0 || tt >= T || (m->win[w][half + tap] != 0.0f && mask && !mask[tt])) {
                        if (!c) { c = &pool[n]; *c = *g; }
                        memset(c->mp + w * D, 0, sizeof(float) * D);
                        memset(c->p + w * D, 0, sizeof(float) * D);
                        break;
                    }
                }
            }
            fr[n] = c ? c : (ZbGauss *)g;
            n++;
        }
    }
    return n;
}

/* FUN_180016010 on float data: windowed average within voiced runs */
static void f0_fir(float *f0, int T, const float *w, int N)
{
    float *x = (float *)malloc(sizeof(float) * (size_t)(T ? T : 1)), all = 0.0f;
    int t, k, h = N / 2;
    memcpy(x, f0, sizeof(float) * T);
    for (k = 0; k < N; k++) all = all + w[k];
    for (t = 0; t < T; t++) {
        int lo, hi;
        float acc = 0.0f, used = 0.0f;
        if (x[t] == 0.0f) continue;
        for (lo = 0; lo - 1 >= -h && t + lo - 1 >= 0 && x[t + lo - 1] != 0.0f; lo--);
        for (hi = 0; hi + 1 <= N - h - 1 && t + hi + 1 < T && x[t + hi + 1] != 0.0f; hi++);
        for (k = lo; k <= hi; k++) {
            acc = w[h + k] * x[t + k] + acc;
            used = used + w[h + k];
        }
        f0[t] = used == 0.0f ? 0.0f : (all / used) * acc;
    }
    free(x);
}

typedef struct {
    int nph, nst, T;
    ZbGauss *gdur, *gpdur, *glf0, *glsf, *ggain;
    int *dur;
} ZbUttModels;

static int lookup(ZbVoice *v, const ZbUtt *u, ZbUttModels *um)
{
    int p, s, k;
    const int ns = v->m_lf0->nstate;
    um->nph = u->nphone;
    um->nst = ns;
    um->gdur = (ZbGauss *)calloc((size_t)u->nphone, sizeof(ZbGauss));
    um->gpdur = v->m_pdur ? (ZbGauss *)calloc((size_t)u->nphone, sizeof(ZbGauss)) : NULL;
    um->glf0 = (ZbGauss *)calloc((size_t)u->nphone * ns, sizeof(ZbGauss));
    um->glsf = (ZbGauss *)calloc((size_t)u->nphone * ns, sizeof(ZbGauss));
    um->ggain = (ZbGauss *)calloc((size_t)u->nphone * ns, sizeof(ZbGauss));
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
            ZbGauss full, *gl = &um->glsf[p * ns + s], *gg = &um->ggain[p * ns + s];
            int P = v->m_lsf->streamdim[0] / 3, w;
            leaf = zb_tree_leaf(v, v->m_lf0, id, s, f);
            if (!leaf || zb_state_gauss(v, v->m_lf0, leaf, &um->glf0[p * ns + s])) return -1;
            leaf = zb_tree_leaf(v, v->m_lsf, id, s, f);
            if (!leaf || zb_state_gauss(v, v->m_lsf, leaf, &full)) return -1;
            /* split stream 1 into LSF (type 1, dims 0..P-2 of each window) and gain (type 5, dim P-1): FUN_180006e9c */
            gl->w = gg->w = full.w;
            gl->wmax = gg->wmax = full.wmax;
            gl->dim = 3 * (P - 1);
            gg->dim = 3;
            for (w = 0; w < 3; w++) {
                for (k = 0; k < P - 1; k++) {
                    gl->mp[w * (P - 1) + k] = full.mp[w * P + k];
                    gl->p[w * (P - 1) + k] = full.p[w * P + k];
                }
                gg->mp[w] = full.mp[w * P + P - 1];
                gg->p[w] = full.p[w * P + P - 1];
            }
        }
    }
    return 0;
}

/* FUN_18001b100 (pitch) + FUN_1800a2810 (range); the SAPI controls plus the emotion recipe (ZbStyle, zb.h) */
/* ClassicVoices patch: exported (was static) for the Eva back end, eva_voice.c; unchanged */
void zb_apply_pitch(const ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, const int *dur, int nst, float *f0, int T)
{
    const ZbConfig *cfg = &v->cfg;
    int pa_adj = zb_style_pct(cfg->pitch_adj, sapi ? sapi->style.pitch : 0);
    int pr_adj = zb_style_pct(cfg->pitch_range_scale, sapi ? sapi->style.range : 0);
    int p, s, t = 0;
    unsigned R;
    float r;
    for (p = 0; p < u->nphone; p++) {
        int n = 0;
        for (s = 0; s < nst; s++) n += dur[p * nst + s];
        if (u->ph[p].c.pitch != 0 || pa_adj != 100) {
            int pa = u->ph[p].c.pitch;
            float k = 1.0f * zb_pow2_24th(pa < -10 ? -10 : (pa > 10 ? 10 : pa));
            unsigned P = (unsigned)pa_adj;
            k = k * (float)1.0;
            if (P != 100) k = ((float)P * k) / 100.0f;
            for (s = t; s < t + n && s < T; s++)
                if (3.0f < f0[s]) {
                    float y = f0[s] * k, z = 3.0f;
                    if (3.0f <= y) z = y;
                    if (8000.0f <= z) z = 8000.0f;
                    f0[s] = z;
                }
        }
        t += n;
    }
    R = (unsigned)pr_adj;
    r = 1.0f;
    if (u->nphone && u->ph[0].c.pitch_range != 0.0f) r = u->ph[0].c.pitch_range;
    if (r != 1.0f || R != 100) {
        float k = ((float)R / 100.0f) * r, sum = 0.0f, mean;
        unsigned cnt = 0;
        for (t = 0; t < T; t++)
            if (3.0f < f0[t]) { sum = sum + f0[t]; cnt++; }
        if (cnt) {
            for (t = 0; t < T; t++)
                if (3.0f < f0[t]) {
                    float y = (f0[t] - sum / (float)cnt) * k + sum / (float)cnt, z = 3.0f;
                    if (3.0f <= y) z = y;
                    if (8000.0f <= z) z = 8000.0f;
                    f0[t] = z;
                }
        }
        (void)mean;
    }
}

int zb_synth(ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, zb_write_fn write, void *user, ZbTrace *trace)
{
    ZbUttModels um;
    int ns, nst, T = 0, k, t, nv, rc = -1;
    int *vuv = NULL;
    float *w = NULL, *lf0 = NULL, *lsf = NULL, *gain = NULL, *f0c = NULL;
    ZbGauss **fr = NULL, *pool = NULL;
    short *pcm = NULL, *wav = NULL;
    int *stretch = NULL;
    long nwav = 0;
    const int D = 24;
    if (!v->m_lsf || !v->m_lf0) return -1;   /* duration-only APM (Eva) */
    if (v->isint) return zb_synth_int(v, u, sapi, write, user, trace);
    memset(&um, 0, sizeof um);
    if (lookup(v, u, &um)) goto done;
    ns = um.nst;
    nst = u->nphone * ns;
    um.dur = (int *)calloc((size_t)nst, sizeof(int));
    stretch = (int *)calloc((size_t)u->nphone + 1, sizeof(int));
    zb_durations_ex(v, u, sapi, um.gdur, um.gpdur, ns, um.dur, stretch);
    for (k = 0; k < nst; k++) T += um.dur[k];
    /* UV */
    w = (float *)malloc(sizeof(float) * (size_t)nst);
    for (k = 0; k < nst; k++) w[k] = um.glf0[k].wmax;
    vuv = (int *)calloc((size_t)T + 1, sizeof(int));
    if (v->cfg.uv_frame_level) uv_frame_level(w, um.dur, nst, v->cfg.voiced_thr, vuv);
    else uv_state_level(w, um.dur, nst, v->cfg.voiced_thr, vuv);
    /* F0 */
    fr = (ZbGauss **)malloc(sizeof(ZbGauss *) * (size_t)(T + 1));
    pool = (ZbGauss *)malloc(sizeof(ZbGauss) * (size_t)(T + 1));
    lf0 = (float *)calloc((size_t)T + 1, sizeof(float));
    f0c = (float *)calloc((size_t)T + 1, sizeof(float));
    nv = build_frames(v->m_lf0, um.glf0, um.dur, nst, T, vuv, 1, fr, pool);
    zb_mlpg_float(v->m_lf0, fr, nv, 1, f0c);
    for (t = 0, k = 0; t < T; t++) lf0[t] = vuv[t] ? f0c[k++] : 0.0f;
    if (v->m_lf0->has_gen) {
        f0_fir(lf0, T, v->m_lf0->genw, v->m_lf0->genlen);
        for (t = 0; t < T; t++)
            if (lf0[t] != 0.0f) lf0[t] = (lf0[t] - v->m_lf0->gen_mean) * v->m_lf0->gen_enhance + v->m_lf0->gen_mean;
    }
    /* LSF, gain */
    lsf = (float *)calloc((size_t)T * D + 1, sizeof(float));
    gain = (float *)calloc((size_t)T + 1, sizeof(float));
    k = build_frames(v->m_lsf, um.glsf, um.dur, nst, T, NULL, D, fr, pool);
    zb_mlpg_float(v->m_lsf, fr, k, D, lsf);
    k = build_frames(v->m_lsf, um.ggain, um.dur, nst, T, NULL, 1, fr, pool);
    zb_mlpg_float(v->m_lsf, fr, k, 1, gain);
    if (trace) {
        trace->nphone = u->nphone; trace->nstate = ns; trace->nframe = T; trace->order = D;
        trace->dur = (int *)malloc(sizeof(int) * nst); memcpy(trace->dur, um.dur, sizeof(int) * nst);
        trace->vuv = (int *)malloc(sizeof(int) * (T + 1)); memcpy(trace->vuv, vuv, sizeof(int) * T);
        trace->lf0 = (float *)malloc(sizeof(float) * (T + 1)); memcpy(trace->lf0, lf0, sizeof(float) * T);
        trace->lsf = (float *)malloc(sizeof(float) * (T * D + 1)); memcpy(trace->lsf, lsf, sizeof(float) * T * D);
        trace->gain = (float *)malloc(sizeof(float) * (T + 1)); memcpy(trace->gain, gain, sizeof(float) * T);
    }
    /* export FUN_1800a25f4 */
    for (t = 0; t < T; t++)
        if (lf0[t] != 0.0f) lf0[t] = (float)exp((double)lf0[t]);
    zb_lsf_repair(lsf, T, D);
    zb_apply_pitch(v, u, sapi, um.dur, ns, lf0, T);
    if (trace) {
        trace->f0 = (float *)malloc(sizeof(float) * (T + 1)); memcpy(trace->f0, lf0, sizeof(float) * T);
        trace->lsf_fin = (float *)malloc(sizeof(float) * (T * D + 1)); memcpy(trace->lsf_fin, lsf, sizeof(float) * T * D);
    }
    /* vocoder */
    pcm = (short *)calloc((size_t)T * v->shift + 1, sizeof(short));
    zb_vocoder_float(v, u, um.dur, ns, T, lf0, lsf, gain, stretch, pcm);
    nwav = zb_wave_sps(v, u, sapi, um.dur, ns, stretch, NULL, 0, pcm, &wav);   /* rate stage, voice + SAPI volume */
    zb_srand(v, 0x406);   /* end-of-utterance vocoder reset (FUN_18011024c) */
    rc = 0;
    if (write && nwav) rc = write(user, wav, (int)nwav);
done:
    free(um.gdur); free(um.gpdur); free(um.glf0); free(um.glsf); free(um.ggain); free(um.dur);
    free(w); free(vuv); free(lf0); free(f0c); free(lsf); free(gain); free(fr); free(pool); free(pcm); free(wav); free(stretch);
    return rc;
}
