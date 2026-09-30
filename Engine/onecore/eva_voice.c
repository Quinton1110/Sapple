/* eva_voice.c - Microsoft Eva back end (see eva_voice.h).  ClassicVoices addition, ported from ~/code/eva-onecore
 * (notes/eva_dnn.md, notes/eva_prosody.md there).  Status legend: [S] = transcribed from the decompile of the
 * Windows 11 26100.1742 DLLs, [G] = inferred.
 * Keep -ffp-contract=off like the rest of the engine. */
#include "eva_voice.h"
#include "eva_nn.h"
#include "eva_crf.h"
#include "zb_internal.h"
#include "zf2_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define EVA_MAXFEAT 64
typedef struct {
    int feature;           /* index into feat[] */
    float mean, scale;     /* input = (x - mean) * scale */
    int nval;
    const uint32_t *val;   /* one-hot value list (nval == 0: numeric input) */
} EvaQuestion;

typedef struct {
    uint8_t *file; long size; size_t maplen;
    int nfeat, nframefeat, nframeq;
    char feat[EVA_MAXFEAT][128];   /* dotted feature paths (ASCII) */
    int nq;                        /* network input size (631) */
    EvaQuestion *q;
    int nprec;                     /* per-frame output size (127) */
    const float *prec;             /* precision per output dim */
    int nstream;
    struct { int type, nseg, off[3], dim[3]; } stream[8];
    float win[3][3];               /* the [1], [-.5 0 .5], [1 -2 1] windows of stream 1 */
    /* model tail: customized F0 generation setting [G on the reader] */
    int genlen; float genw[32], gen_enhance, gen_mean;
} EvaNNM;

typedef struct { int type, n; const float *src, *dst; } EvaHeqTable;
typedef struct { uint8_t *file; size_t maplen; int ntab; EvaHeqTable tab[8]; } EvaHeq;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p) { float f; uint32_t u = rd32(p); memcpy(&f, &u, 4); return f; }

/* APM string obfuscation (FUN_180039f60): key advances only on nonzero UTF-16 units */
static const uint16_t KEY[8] = {0x3412, 0x7856, 0xbc9a, 0xf0de, 0x5634, 0xdebc, 0x9a78, 0x12f0};
static int get_string(const uint8_t *s, uint32_t size, uint32_t off, char *out, int cap) {
    /* decode from the start (the key index depends on every nonzero unit before off) */
    uint32_t i; int j = 0, n = 0;
    for (i = 0; i + 1 < size; i += 2) {
        uint16_t c = (uint16_t)(s[i] | s[i + 1] << 8);
        if (c) { c ^= KEY[j]; j = (j + 1) & 7; }
        if (i >= off) {
            if (!c) break;
            if (n + 1 < cap) out[n++] = (char)(c < 128 ? c : '?');
        }
    }
    out[n] = 0;
    return n ? 0 : -1;
}

static int eva_nnm_load(EvaNNM *m, const char *path, char *err, int errlen) {
    memset(m, 0, sizeof *m);
    m->file = eva_map_file(path, &m->size, &m->maplen);
    if (!m->file || m->size < 0x60) { snprintf(err, errlen, "cannot read %s", path); return -1; }
    const uint8_t *d = m->file;
    if (memcmp(d, "NNM ", 4)) { snprintf(err, errlen, "not an NNM file"); return -1; }
    const uint8_t *h = d + 0x18;
    uint32_t qo = rd32(h + 0x20), mo = rd32(h + 0x28), so = rd32(h + 0x30), ss = rd32(h + 0x34);
    const uint8_t *p = d + qo;
    m->nframefeat = (int)rd32(p); m->nframeq = (int)rd32(p + 4); p += 8;
    m->nfeat = (int)rd32(p); p += 4;
    if (m->nfeat > EVA_MAXFEAT) { snprintf(err, errlen, "too many features"); return -1; }
    for (int i = 0; i < m->nfeat; i++, p += 4)
        if (get_string(d + so, ss, rd32(p), m->feat[i], sizeof m->feat[i])) { snprintf(err, errlen, "bad string"); return -1; }
    m->nq = (int)rd32(p); p += 4;
    m->q = calloc((size_t)m->nq, sizeof(EvaQuestion));
    for (int i = 0; i < m->nq; i++) {
        EvaQuestion *q = &m->q[i];
        q->feature = (int)rd32(p); q->mean = rdf(p + 4); q->scale = rdf(p + 8); q->nval = (int)rd32(p + 12); p += 16;
        q->val = (const uint32_t *)p; p += 4 * q->nval;
    }
    m->nprec = (int)rd32(p); p += 4;
    m->prec = (const float *)p; p += 4 * m->nprec;
    /* output streams FUN_18012141c */
    p = d + mo;
    m->nstream = (int)rd32(p); p += 4;
    for (int i = 0; i < m->nstream && i < 8; i++) {
        m->stream[i].type = (int)rd32(p); m->stream[i].nseg = (int)rd32(p + 4); p += 8;
        for (int k = 0; k < m->stream[i].nseg; k++, p += 8)
            if (k < 3) { m->stream[i].off[k] = (int)rd32(p); m->stream[i].dim[k] = (int)rd32(p + 4); }
    }
    int nw = (int)rd32(p); p += 4;
    for (int i = 0; i < nw; i++) {
        int sid = (int)rd32(p), n = (int)rd32(p + 4); p += 8;
        for (int k = 0; k < n; k++) {
            int w = (int)rd32(p); p += 4;
            if (sid == 1 && k < 3 && w <= 3)
                for (int c = 0; c < w; c++) m->win[k][c] = rdf(p + 4 * c);
            p += 4 * w;
        }
    }
    /* model tail: {4, 0, 0, 1, 2, 15, w[15], EnhanceRate, Mean, 0} [G: read like the APM F0 blob] */
    if (p + 24 <= d + mo + rd32(h + 0x2c)) {
        int len = (int)rd32(p + 20);
        if (len > 0 && len <= 32 && p + 24 + 4 * len + 8 <= d + m->size) {
            m->genlen = len;
            for (int k = 0; k < len; k++) m->genw[k] = rdf(p + 24 + 4 * k);
            m->gen_enhance = rdf(p + 24 + 4 * len);
            m->gen_mean = rdf(p + 28 + 4 * len);
        }
    }
    return 0;
}

static void eva_nnm_free(EvaNNM *m) { free(m->q); eva_unmap_file(m->file, m->maplen); memset(m, 0, sizeof *m); }

/* ---- HEQ ---- */
static int eva_heq_load(EvaHeq *h, const char *path, char *err, int errlen) {
    long n = 0;
    memset(h, 0, sizeof *h);
    h->file = eva_map_file(path, &n, &h->maplen);
    if (!h->file) { snprintf(err, errlen, "cannot read %s", path); return -1; }
    /* container FUN_180097ab8: "HEQT", GUID, u32 version, u32 checksum, u32 payload size -> payload at 0x24 */
    if (n < 0x28 || memcmp(h->file, "HEQT", 4)) { snprintf(err, errlen, "not a HEQ file"); return -1; }
    const uint8_t *p = h->file + 0x24, *end = h->file + n;
    int nt = (int)rd32(p); p += 4;
    for (int i = 0; i < nt && i < 8; i++) {
        EvaHeqTable *t = &h->tab[i];
        t->type = (int)rd32(p); t->n = (int)rd32(p + 4); p += 8;
        if (p + 8 * (size_t)t->n > end) { snprintf(err, errlen, "HEQ table out of range"); return -1; }
        t->src = (const float *)p; p += 4 * t->n;
        t->dst = (const float *)p; p += 4 * t->n;
        h->ntab++;
    }
    return 0;
}
static const EvaHeqTable *eva_heq_find(const EvaHeq *h, int type) {
    for (int i = 0; i < h->ntab; i++) if (h->tab[i].type == type) return &h->tab[i];
    return NULL;
}
/* FUN_1800ac8c0 (checked in disassembly): bisection on the source quantiles, then linear interpolation (and
 * extrapolation past the ends) onto the target quantiles */
static float eva_heq_map(const EvaHeqTable *t, float x) {
    if (t->n < 2) return x;
    int hi = t->n - 1, lo = -1, i = 0;
    if (hi > 0) {
        do {
            i = (lo + hi) / 2;
            if (x > t->src[i + 1]) lo = i;
            else if (t->src[i] > x) hi = i;
            else break;
        } while (hi > lo + 1);
    }
    float r = 0.0f;
    if (t->src[i + 1] != t->src[i]) { float a = t->src[i + 1] - t->src[i], b = x - t->src[i]; r = b / a; }
    float d = t->dst[i + 1] - t->dst[i];
    d = d * r;
    return d + t->dst[i];
}
static void eva_heq_free(EvaHeq *h) { eva_unmap_file(h->file, h->maplen); memset(h, 0, sizeof *h); }

/* minimal INI reader for the Eva keys the ZbConfig loader does not keep */
static int ini_int(const char *path, const char *sec, const char *key, int def) {
    FILE *f = fopen(path, "r");
    char line[512], cur[128] = "";
    int v = def;
    if (!f) return def;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *q;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') { q = strchr(p, ']'); if (q) { *q = 0; snprintf(cur, sizeof cur, "%s", p + 1); } continue; }
        q = strchr(p, '=');
        if (!q || strcmp(cur, sec)) continue;
        *q = 0;
        for (char *r = q - 1; r >= p && (*r == ' ' || *r == '\t'); r--) *r = 0;
        if (!strcmp(p, key)) v = atoi(q + 1);
    }
    fclose(f);
    return v;
}

struct EvaModel {
    EvaNet *net;
    EvaNNM nnm;
    EvaHeq heq;
    float normk;           /* NN.NumericLinguisticFeatureNormFactor (6 for 0x409, table 0x18018a250) [S] */
    float heq_adj;         /* VoiceSetting.LogF0HeqAdjustment / 100 */
    /* Sarah additions (en-GB, 2057): a feed-forward int16 net, one frame per step with frame skipping, a duration
     * HEQ table and the five-band excitation stream; Eva (packed, no stream 6 / 7) is unaffected by all of them */
    int lcid;
    int sL, sG, sF, sV, sM;  /* output stream indices by type: 1 LSF, 5 gain, 2 log F0, 4 voicing, 7 MBE (-1 none) */
    int skip;              /* NN.SkipFrameCount + 1, used by non-packed nets only (Sarah 3) */
    float skipfac;         /* NN.FrameSkippingVarScaleFactor (Sarah 10) */
    float dheq_adj;        /* VoiceSetting.DurationHeqAdjustment / 100 (only with a stream-6 HEQ table) */
    int has_crf;           /* .BR2 / .TON / .ACL present (Eva) */
    float mbe_offset;      /* MultiBandExcitation.Offset (default 0.0, [0, 1]) [S FUN_18005c910] */
    ZbModel wm;            /* the NNM window set, for MLPG */
    int f_state, f_frames, f_pct;
    EvaProsody prosody;    /* .BR2 / .TON / .ACL */
    zf2_voice *vf;
};

/* NN.NumericLinguisticFeatureNormFactor default per language (table 0x18018a270 / 0x18018a250) [S] */
static int normk_default(int lcid) {
    switch (lcid) { case 0x809: return 10; case 0x40c: return 11; case 0x410: return 17; case 0x80a: return 9;
                    case 0x407: return 10; case 0xc09: return 9; default: return 6; }
}
static int nnm_stream(const EvaNNM *m, int type) {
    for (int i = 0; i < m->nstream && i < 8; i++) if (m->stream[i].type == type) return i;
    return -1;
}
static int file_exists(const char *path) { FILE *f = fopen(path, "rb"); if (f) fclose(f); return f != NULL; }
/* "<vp>.<EXT>", or "<vp>.<ext>" when only the lower-case name exists (Sarah's files as Windows ships them; iOS's file
 * system is case-sensitive) */
static void data_path(char *out, size_t cap, const char *vp, const char *EXT) {
    char low[16]; size_t i;
    snprintf(out, cap, "%s.%s", vp, EXT);
    if (file_exists(out)) return;
    for (i = 0; EXT[i] && i + 1 < sizeof low; i++) low[i] = (char)(EXT[i] >= 'A' && EXT[i] <= 'Z' ? EXT[i] + 32 : EXT[i]);
    low[i] = 0;
    snprintf(out, cap, "%s.%s", vp, low);
    if (!file_exists(out)) snprintf(out, cap, "%s.%s", vp, EXT);
}

EvaModel *eva_model_open(const char *vp, int lcid, zf2_voice *vf, char *err, int errlen) {
    char path[1200];
    EvaModel *e = calloc(1, sizeof *e);
    static const char *const ext[3] = {"BR2", "TON", "ACL"};
    if (!e) { snprintf(err, errlen, "out of memory"); return NULL; }
    e->lcid = lcid;
    data_path(path, sizeof path, vp, "NNM");
    if (eva_nnm_load(&e->nnm, path, err, errlen)) goto fail;
    data_path(path, sizeof path, vp, "HEQ");
    if (eva_heq_load(&e->heq, path, err, errlen)) goto fail;
    snprintf(path, sizeof path, "%s.INI", vp);
    e->heq_adj = (float)ini_int(path, "VoiceSetting", "LogF0HeqAdjustment", 100) / 100.0f;
    e->normk = (float)ini_int(path, "NN", "NumericLinguisticFeatureNormFactor", normk_default(lcid));
    e->dheq_adj = (float)ini_int(path, "VoiceSetting", "DurationHeqAdjustment", 100) / 100.0f;
    e->skip = ini_int(path, "NN", "SkipFrameCount", 0) + 1;
    e->skipfac = (float)ini_int(path, "NN", "FrameSkippingVarScaleFactor", 1);
    if (e->skipfac <= 0.0f) e->skipfac = 1.0f;
    e->mbe_offset = (float)ini_int(path, "MultiBandExcitation", "Offset", 0);   /* integer read: only 0 / 1 [G]; Sarah has none */
    data_path(path, sizeof path, vp, "TDAT");
    e->net = eva_net_load(path, err, errlen);
    if (!e->net) goto fail;
    e->sL = nnm_stream(&e->nnm, 1); e->sG = nnm_stream(&e->nnm, 5); e->sF = nnm_stream(&e->nnm, 2);
    e->sV = nnm_stream(&e->nnm, 4); e->sM = nnm_stream(&e->nnm, 7);
    if (e->net->in != e->nnm.nq || e->net->out % e->nnm.nprec || e->sL < 0 || e->sG < 0 || e->sF < 0 || e->sV < 0
        || (e->nnm.stream[e->sL].dim[0] != 40 && e->nnm.stream[e->sL].dim[0] != 24)   /* Eva / Sarah 40, Matilda 24 */
        || e->nnm.stream[e->sL].nseg != 3 || e->nnm.stream[e->sG].nseg != 3
        || e->nnm.stream[e->sF].nseg != 3
        || (e->sM >= 0 && (e->nnm.stream[e->sM].nseg != 3 || e->nnm.stream[e->sM].dim[0] > ZB_MBE_MAXBANDS)))
        { snprintf(err, errlen, "unexpected NNM / TDAT layout"); goto fail; }
    snprintf(path, sizeof path, "%s.%s", vp, ext[0]);
    e->has_crf = file_exists(path);
    if (e->has_crf) {   /* statistical prosody models: CLinguisticProsodyTagger model runners (loc model table: BR2, TON, ACL) */
        EvaCrf **slot[3] = {&e->prosody.br2, &e->prosody.ton, &e->prosody.acl};
        for (int k = 0; k < 3; k++) {
            snprintf(path, sizeof path, "%s.%s", vp, ext[k]);
            *slot[k] = eva_crf_load(path, err, errlen);
            if (!*slot[k]) goto fail;
        }
    }
    e->f_state = e->nnm.nfeat - 3; e->f_frames = e->nnm.nfeat - 2; e->f_pct = e->nnm.nfeat - 1;
    e->wm.nwin = 3; e->wm.winw[0] = 1; e->wm.winw[1] = 3; e->wm.winw[2] = 3;
    e->wm.win[0][0] = e->nnm.win[0][0];
    for (int k = 0; k < 3; k++) { e->wm.win[1][k] = e->nnm.win[1][k]; e->wm.win[2][k] = e->nnm.win[2][k]; }
    e->vf = vf;
    if (e->has_crf) zf2_voice_set_prosody_hook(vf, eva_prosody_hook, &e->prosody);
    return e;
fail:
    eva_model_close(e);
    return NULL;
}

void eva_model_close(EvaModel *e) {
    if (!e) return;
    if (e->vf && e->has_crf) zf2_voice_set_prosody_hook(e->vf, NULL, NULL);
    eva_net_free(e->net);
    eva_nnm_free(&e->nnm);
    eva_heq_free(&e->heq);
    eva_crf_free(e->prosody.br2); eva_crf_free(e->prosody.ton); eva_crf_free(e->prosody.acl);
    free(e);
}

void eva_model_set_debug(EvaModel *e, int models, int verbose) {
    e->prosody.verbose = verbose;
    e->prosody.off = !models;
}

static int in_list(const EvaQuestion *q, int v) {
    for (int i = 0; i < q->nval; i++) if ((int)q->val[i] == v) return 1;
    return 0;
}
/* FUN_1800a7544: clamp a numeric input to mean +- K/scale (skipped when mean == 0, scale == 1 or scale == 0) */
static float nclamp(const EvaQuestion *q, float v, float K) {
    if (K == 0.0f || q->mean == 0.0f || q->scale == 1.0f || q->scale == 0.0f) return v;
    float r = K / q->scale, hi = q->mean + r, lo = q->mean - r;
    if (v <= hi) { if (v < lo) v = lo; } else v = hi;
    return v;
}

static float heq_cb(const void *ctx, float x) { return eva_heq_map((const EvaHeqTable *)ctx, x); }

int eva_synth(EvaModel *e, ZbVoice *vb, const zf2_sent *s, const ZbUtt *u, const ZbSapi *sapi, zb_write_fn write,
              void *user, ZbTrace *trace) {
    const EvaNNM *m = &e->nnm;
    const int NS = 5, NQ = m->nq, P = m->nprec, K = e->net->out / m->nprec;   /* K = 4 frames per network step */
    const int nph = u->nphone, nst = nph * NS, D = m->stream[e->sL].dim[0];  /* D = 40 */
    /* frame skipping, non-packed nets only (Sarah: S = 3) [S FUN_1800433c0, see the network loop] */
    const int S = (K == 1 && e->skip > 1) ? e->skip : 1, G = K * S;
    const int NB = e->sM >= 0 ? m->stream[e->sM].dim[0] : 0;                 /* MBE bands (Sarah 5) */
    int rc = -1, T = 0, nf = m->nfeat - m->nframefeat;
    ZbGauss *gd = calloc((size_t)nph + 1, sizeof(ZbGauss)), *gp = vb->m_pdur ? calloc((size_t)nph + 1, sizeof(ZbGauss)) : NULL;
    int *dur = calloc((size_t)nst + 1, sizeof(int)), *stretch = calloc((size_t)nph + 1, sizeof(int));
    zf2_value *vals = NULL;
    int *fph = NULL, *fst = NULL, *fk = NULL, *vuv = NULL;
    char *lowp = NULL;
    float *x = NULL, *o = NULL, *pa = NULL, *ba = NULL, *lf0 = NULL, *lsf = NULL, *gain = NULL, *col = NULL, *mbe = NULL;
    short *pcm = NULL, *wav = NULL;
    if (!gd || (vb->m_pdur && !gp) || !dur || !stretch || nph <= 0) goto done;
    /* ---- durations: Eva's APM state and phone duration trees, the HMM voices' duration code ---- */
    for (int p = 0; p < nph; p++) {
        const int *f = u->ph[p].f; int id = f[ZB_F_PhoneIdentity];
        const uint8_t *leaf = zb_tree_leaf(vb, vb->m_dur, id, 0, f);
        if (!leaf || zb_state_gauss(vb, vb->m_dur, leaf, &gd[p])) goto done;
        if (gp) {
            leaf = zb_tree_leaf(vb, vb->m_pdur, id, 0, f);
            if (!leaf || zb_state_gauss(vb, vb->m_pdur, leaf, &gp[p])) goto done;
        }
    }
    {   /* Sarah: duration HEQ (stream-6 table) [S duration predictor]; Eva's HEQ has no such table */
        const EvaHeqTable *dh = eva_heq_find(&e->heq, 6);
        if (dh) zb_durations_heq(vb, u, sapi, gd, gp, NS, dur, stretch, heq_cb, dh, e->dheq_adj);
        else zb_durations_ex(vb, u, sapi, gd, gp, NS, dur, stretch);
    }
    for (int k = 0; k < nst; k++) T += dur[k];
    if (T <= 0) goto done;
    /* ---- phone-level feature values (the NNM's 43 paths) ---- */
    vals = calloc((size_t)nf * (size_t)nph, sizeof(zf2_value));
    fph = malloc(sizeof(int) * (size_t)T); fst = malloc(sizeof(int) * (size_t)T); fk = malloc(sizeof(int) * (size_t)T);
    x = malloc(sizeof(float) * (size_t)NQ);
    o = malloc(sizeof(float) * (size_t)P * (size_t)T);
    vuv = calloc((size_t)T, sizeof(int));
    lowp = calloc((size_t)T, 1);
    if (!vals || !fph || !fst || !fk || !x || !o || !vuv || !lowp) goto done;
    for (int f = 0; f < nf; f++)
        if (zf2_eval_path(s, m->feat[f], vals + (size_t)f * nph))
            for (int p = 0; p < nph; p++) vals[(size_t)f * nph + p].kind = -1;
    for (int p = 0, t = 0; p < nph; p++)
        for (int st = 0; st < NS; st++)
            for (int k = 0; k < dur[p * NS + st]; k++, t++) { fph[t] = p; fst[t] = st; fk[t] = k; }
    /* ---- the network: one step per K frames on the input row of the group's last frame [S FUN_1800433c0] ---- */
    /* Sarah (non-packed, S > 1): one step per S frames on the input row of the group's LAST frame (partial final
     * group: the utterance's last frame); that frame gets the NNM precision, the group's other frames the same
     * output with precision / FrameSkippingVarScaleFactor [S FUN_1800433c0; the partial group's handling is G] */
    eva_net_reset(e->net);
    for (int g = 0; g < T; g += G) {
        int t = g + G - 1 < T ? g + G - 1 : T - 1;
        int p = fph[t], st = fst[t], d = dur[p * NS + st];
        for (int j = 0; j < NQ; j++) {
            const EvaQuestion *q = &m->q[j];
            float v = 0.0f;
            if (q->feature < nf) {
                const zf2_value *fv = &vals[(size_t)q->feature * nph + p];
                if (fv->kind == -1) v = 0.0f;
                else if (q->nval) v = in_list(q, fv->value) ? 1.0f : 0.0f;
                else v = (float)fv->value;
                v = nclamp(q, v, e->normk);
            } else if (q->feature == e->f_state) v = in_list(q, st + 2) ? 1.0f : 0.0f;
            else if (q->feature == e->f_frames) v = nclamp(q, (float)d, e->normk);
            else if (q->feature == e->f_pct) v = (float)fk[t] / (float)d;
            x[j] = (v - q->mean) * q->scale;
        }
        const float *y = eva_net_forward(e->net, x);
        if (S == 1)
            for (int k = 0; k < K && g + k < T; k++) memcpy(o + (size_t)(g + k) * P, y + (size_t)k * P, sizeof(float) * (size_t)P);
        else
            for (int k = 0; k < S && g + k < T; k++) {
                memcpy(o + (size_t)(g + k) * P, y, sizeof(float) * (size_t)P);
                lowp[g + k] = (char)(g + k != t);
            }
    }
    /* voicing: raw output (Eva 126, Sarah 141) <= VoicedWeightThreshold (0.5) [S FUN_1800a9170 / FUN_1800a4380] */
    for (int t = 0; t < T; t++) vuv[t] = o[(size_t)t * P + m->stream[e->sV].off[0]] <= vb->cfg.voiced_thr;
    /* ---- per-frame Gaussians (mean x precision, precision), first / last frame all zero [S FUN_1800a988c], MLPG
     *      dimension by dimension on flat [T][3] arrays (zb_mlpg_float_arr = zb_mlpg_float's arithmetic) ---- */
    pa = malloc(sizeof(float) * 3 * (size_t)T); ba = malloc(sizeof(float) * 3 * (size_t)T);
    col = malloc(sizeof(float) * (size_t)T);
    lf0 = calloc((size_t)T, sizeof(float)); gain = calloc((size_t)T, sizeof(float));
    lsf = calloc((size_t)T * (size_t)D, sizeof(float));
    if (!pa || !ba || !col || !lf0 || !gain || !lsf) goto done;
    for (int dim = 0; dim <= D + 1; dim++) {     /* 0..39 LSF, 40 gain, 41 log F0 */
        int str = dim < D ? e->sL : dim == D ? e->sG : e->sF, k = dim < D ? dim : 0;
        for (int t = 0; t < T; t++) {
            const float *y = o + (size_t)t * P;
            int edge = t == 0 || t == T - 1;
            for (int w = 0; w < 3; w++) {
                int dd = m->stream[str].off[w] + k;
                float pr = lowp[t] ? m->prec[dd] / e->skipfac : m->prec[dd];
                pa[3 * t + w] = edge ? 0.0f : pr;
                ba[3 * t + w] = edge ? 0.0f : pr * y[dd];
            }
        }
        zb_mlpg_float_arr(&e->wm, pa, ba, T, 1, col);
        for (int t = 0; t < T; t++) {
            if (dim < D) lsf[(size_t)t * D + dim] = col[t];
            else if (dim == D) gain[t] = col[t];
            else lf0[t] = vuv[t] ? col[t] : 0.0f;     /* log F0 over all frames, unvoiced -> 0 [S] */
        }
    }
    /* Sarah: the five-band excitation strengths (stream 7) [S PostNN FUN_1800a988c / FUN_1800a9170 / FUN_1800a6788]:
     * MLPG like the other streams, except that only the utterance's FIRST frame gets the zero Gaussian (the engine's
     * last-frame test is off by a chunk for this stream); a frame whose raw voicing output > VoicedWeightThreshold gets
     * all zeros, others MLPG + MultiBandExcitation.Offset (0 for Sarah); then per phone: a vowel's strengths
     * v = ADD[b] + v clamped to [LO[b], HI[b]], any other phone's clamped to [0, 1] (tables 0x18018a2a0 / 2b8 / 2d0;
     * "vowel" = phone flag bit 0 of the language data's phone set [G]) */
    if (NB > 0) {
        static const float ADD[5] = {0.1f, 0.25f, 0.2f, 0.1f, 0.0f}, HI[5] = {1.0f, 0.9f, 0.7f, 0.6f, 0.5f},
                           LO[5] = {0.8f, 0.6f, 0.4f, 0.3f, 0.2f};
        mbe = calloc((size_t)T * (size_t)NB, sizeof(float));
        if (!mbe) goto done;
        for (int b = 0; b < NB; b++) {
            for (int t = 0; t < T; t++) {
                const float *y = o + (size_t)t * P;
                for (int w = 0; w < 3; w++) {
                    int dd = m->stream[e->sM].off[w] + b;
                    float pr = lowp[t] ? m->prec[dd] / e->skipfac : m->prec[dd];
                    pa[3 * t + w] = t == 0 ? 0.0f : pr;
                    ba[3 * t + w] = t == 0 ? 0.0f : pr * y[dd];
                }
            }
            zb_mlpg_float_arr(&e->wm, pa, ba, T, 1, col);
            for (int t = 0; t < T; t++) {
                float vv = o[(size_t)t * P + m->stream[e->sV].off[0]] > vb->cfg.voiced_thr ? 0.0f : col[t] + e->mbe_offset;
                mbe[(size_t)t * NB + b] = vv;
            }
        }
        for (int p = 0, t = 0; p < nph; p++) {
            int id = u->ph[p].f[ZB_F_PhoneIdentity], nfr = 0;
            int vowel = id > 0 && id < Z2_PS_MAX && (e->vf->ps.flags[id] & 1u);
            for (int st = 0; st < NS; st++) nfr += dur[p * NS + st];
            for (int k = 0; k < nfr && t < T; k++, t++)
                for (int b = 0; b < NB && b < 5; b++) {
                    float vv = mbe[(size_t)t * NB + b];
                    if (vowel) { vv = ADD[b] + vv; if (vv > HI[b]) vv = HI[b]; if (LO[b] > vv) vv = LO[b]; }
                    else { if (vv > 1.0f) vv = 1.0f; if (0.0f > vv) vv = 0.0f; }
                    mbe[(size_t)t * NB + b] = vv;
                }
        }
    }
    /* LogF0 HEQ [S FUN_1800a8c90 -> FUN_1800a0ab8]: lf0 > 3.0: heq(lf0)*a + (1-a)*lf0 */
    {
        const EvaHeqTable *ht = eva_heq_find(&e->heq, 2);
        float oneminus = 1.0f - e->heq_adj;
        if (ht) for (int t = 0; t < T; t++)
            if (3.0f < lf0[t]) { float a = eva_heq_map(ht, lf0[t]) * e->heq_adj, b = oneminus * lf0[t]; lf0[t] = a + b; }
    }
    /* ---- export [S FUN_1800abee0]: expf, LSF repair, the pitch controls ---- */
    for (int t = 0; t < T; t++) if (lf0[t] != 0.0f) lf0[t] = expf(lf0[t]);
    zb_lsf_repair(lsf, T, D);
    zb_apply_pitch(vb, u, sapi, dur, NS, lf0, T);
    if (trace) {
        trace->nphone = nph; trace->nstate = NS; trace->nframe = T; trace->order = D;
        trace->dur = malloc(sizeof(int) * (size_t)nst);
        if (trace->dur) memcpy(trace->dur, dur, sizeof(int) * (size_t)nst);
    }
    /* ---- the float LSF vocoder at the NNM's order (Eva / Sarah 40, Matilda 24 like the HMM voices; Eva's NNM has no
     *      Sew stream, so no ITFTE [S]) + wave stage ---- */
    free(o); o = NULL; free(pa); pa = NULL; free(ba); ba = NULL;
    pcm = calloc((size_t)T * (size_t)vb->shift + 1, sizeof(short));
    if (!pcm) goto done;
    zb_vocoder_float_mbe(vb, u, dur, NS, T, lf0, lsf, gain, stretch, pcm, D, mbe, NB);   /* mbe NULL: Eva, the plain path */
    long nwav = zb_wave_sps(vb, u, sapi, dur, NS, stretch, NULL, 0, pcm, &wav);
    zb_srand(vb, 0x406);
    rc = 0;
    if (write && nwav > 0) rc = write(user, wav, (int)nwav);
done:
    free(gd); free(gp); free(dur); free(stretch); free(vals); free(fph); free(fst); free(fk); free(vuv); free(lowp);
    free(x); free(o); free(pa); free(ba); free(col); free(lf0); free(lsf); free(gain); free(pcm); free(wav); free(mbe);
    return rc;
}
