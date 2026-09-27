/* zb_apm.c - voice data: APM (HTS acoustic models + question set), INI voice settings.
 * Formats: notes/backend.md section 1 (loader FUN_1800b3650 .. FUN_1800b34d8). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "zb_internal.h"

const char *const zb_feature_path[ZB_NFEAT] = {
    "Phone.BwPosInSyllable", "Phone.FwPosInSyllable", "Phone.NextPhone.NextPhone.PhoneIdentity",
    "Phone.NextPhone.PhoneIdentity", "Phone.PrevPhone.PhoneIdentity", "Phone.PrevPhone.PrevPhone.PhoneIdentity",
    "Phone.Syllable.BreakIndex", "Phone.Syllable.BwPosInPhrase", "Phone.Syllable.BwPosInWord",
    "Phone.Syllable.FwPosInPhrase", "Phone.Syllable.FwPosInWord", "Phone.Syllable.NextSyllable.Accent",
    "Phone.Syllable.NextSyllable.BreakIndex", "Phone.Syllable.NextSyllable.PhoneNumber",
    "Phone.Syllable.NextSyllable.Stress", "Phone.Syllable.PhoneNumber", "Phone.Syllable.PrevSyllable.Accent",
    "Phone.Syllable.PrevSyllable.BreakIndex", "Phone.Syllable.PrevSyllable.PhoneNumber",
    "Phone.Syllable.PrevSyllable.Stress", "Phone.Syllable.Stress", "Phone.Syllable.Word.BwPosInPhrase",
    "Phone.Syllable.Word.BwPosInYesNoQuestion", "Phone.Syllable.Word.FwPosInPhrase",
    "Phone.Syllable.Word.NextWord.POS", "Phone.Syllable.Word.NextWord.SyllableNumber",
    "Phone.Syllable.Word.Phrase.BwPosInSentence", "Phone.Syllable.Word.Phrase.FwPosInSentence",
    "Phone.Syllable.Word.Phrase.IntonationPhrase.Sentence.PhraseNumber",
    "Phone.Syllable.Word.Phrase.IntonationPhrase.Sentence.SentenceType",
    "Phone.Syllable.Word.Phrase.IntonationPhrase.Sentence.SyllableNumber",
    "Phone.Syllable.Word.Phrase.IntonationPhrase.Sentence.WordNumber", "Phone.Syllable.Word.Phrase.SyllableNumber",
    "Phone.Syllable.Word.Phrase.WordNumber", "Phone.Syllable.Word.POS", "Phone.Syllable.Word.PrevWord.POS",
    "Phone.Syllable.Word.PrevWord.SyllableNumber", "Phone.Syllable.Word.SyllableNumber", "Phone.PhoneIdentity",
    "Phone.Syllable.Word.FwPosInYesNoQuestion", "Phone.Syllable.Word.Phrase.NextPhrase.SyllableNumber",
    "Phone.Syllable.Word.Phrase.NextPhrase.WordNumber", "Phone.Syllable.Word.Phrase.PrevPhrase.SyllableNumber",
    "Phone.Syllable.Word.Phrase.PrevPhrase.WordNumber", "Phone.Syllable.Word.Phrase.ToBIFinalBoundaryTone",
    "Phone.Syllable.ToBIAccent", "Phone.Syllable.PrevSyllable.ToBIAccent", "Phone.Syllable.NextSyllable.ToBIAccent"};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

static uint8_t *readfile(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1);
    if (d && fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); d = NULL; }
    fclose(f);
    *n = (size_t)sz;
    return d;
}

/* string table: UTF-16LE, nonzero units XORed with key[j], j advancing mod 8 only on nonzero units (FUN_180039f60).
 * Decodes the string at byte offset off into ASCII. */
static void apm_string(const uint8_t *tab, uint32_t size, uint32_t off, char *out, int outlen)
{
    static const uint16_t key[8] = {0x3412, 0x7856, 0xbc9a, 0xf0de, 0x5634, 0xdebc, 0x9a78, 0x12f0};
    uint32_t i, j = 0;
    int n = 0;
    /* the key index counts all nonzero units from the start of the table */
    for (i = 0; i + 1 < size && i < off; i += 2)
        if (rd16(tab + i)) j = (j + 1) & 7;
    for (i = off; i + 1 < size; i += 2) {
        uint16_t c = rd16(tab + i);
        if (!c) break;
        c ^= key[j];
        j = (j + 1) & 7;
        if (n < outlen - 1) out[n++] = (char)(c < 128 ? c : '?');
    }
    out[n] = 0;
}

int zb_apm_load(ZbVoice *v, const char *path, char *err, int errlen)
{
    const uint8_t *d, *h, *p, *strtab;
    uint32_t qoff, moff, soff, ssize, nf, i, k, nmod;
    char name[256];
    v->apm = readfile(path, &v->apmsize);
    if (!v->apm || v->apmsize < 0x58 || memcmp(v->apm, "APM ", 4)) { snprintf(err, errlen, "cannot read APM %s", path); return -1; }
    d = v->apm;
    h = d + 0x18;
    if (rd32(h) != 3) { snprintf(err, errlen, "APM version %u", rd32(h)); return -1; }
    v->isint = rd32(h + 0x0c) != 0;
    v->rate = (int)rd32(h + 0x10);
    v->bits = (int)rd32(h + 0x14);
    v->shift = (int)rd32(h + 0x18);
    qoff = rd32(h + 0x1c);
    moff = rd32(h + 0x24);
    soff = rd32(h + 0x2c);
    ssize = rd32(h + 0x30);
    if (rd32(h + 0x38)) { snprintf(err, errlen, "Huffman-compressed APM not supported"); return -1; }
    strtab = d + soff;
    /* question set */
    p = d + qoff;
    {
        uint32_t named = rd32(p);
        nf = rd32(p + 4);
        p += 8;
        if (nf > 64) { snprintf(err, errlen, "too many features"); return -1; }
        v->nfeat = (int)nf;
        for (i = 0; i < nf; i++) {
            const char *s = name;
            apm_string(strtab, ssize, rd32(p + 4 * i), name, sizeof name);
            if (*s == '$') s++;
            v->featcol[i] = -1;
            for (k = 0; k < ZB_NFEAT; k++)
                if (!strcmp(s, zb_feature_path[k])) v->featcol[i] = (int)k;
            if (v->featcol[i] < 0) { snprintf(err, errlen, "unknown APM feature %s", s); return -1; }
        }
        p += 4 * nf;
        v->nq = (int)rd32(p);
        p += 4;
        v->q = (ZbQuestion *)calloc((size_t)v->nq, sizeof *v->q);
        for (i = 0; i < (uint32_t)v->nq; i++) {
            ZbQuestion *q = &v->q[i];
            if (named) p += 4;
            q->feat = v->featcol[rd32(p)];
            q->op = (int)rd32(p + 4);
            q->nval = (int)rd32(p + 8);
            p += 12;
            q->val = (int32_t *)malloc(sizeof(int32_t) * (q->nval ? q->nval : 1));
            for (k = 0; k < (uint32_t)q->nval; k++) q->val[k] = (int32_t)rd32(p + 4 * k);
            p += 4 * q->nval;
        }
    }
    /* models */
    p = d + moff;
    nmod = rd32(p);
    if (nmod > 8) { snprintf(err, errlen, "too many models"); return -1; }
    v->nmodel = (int)nmod;
    for (i = 0; i < nmod; i++) {
        ZbModel *m = &v->model[i];
        const uint8_t *mb = d + moff + rd32(p + 4 + 8 * i), *q = mb, *t;
        uint32_t n, w, j, treeoff, pooloff, blob;
        m->type = (int)rd32(q);
        m->msd = (int)rd32(q + 12);
        m->nstate = (int)rd32(q + 16);
        n = rd32(q + 20);
        q += 24;
        m->nstream = (int)n;
        m->totdim = 0;
        for (j = 0; j < n && j < 4; j++) { m->streamdim[j] = (int)rd32(q + 4 * j); m->totdim += m->streamdim[j]; }
        q += 4 * n;
        for (j = 0; j < n && j < 4; j++) m->streamid[j] = (int)rd32(q + 4 * j);
        q += 4 * n;
        treeoff = rd32(q);
        pooloff = rd32(q + 8);
        m->poolsize = rd32(q + 12);
        q += 16;
        m->nwin = (int)rd32(q);
        q += 4;
        for (j = 0; j < (uint32_t)m->nwin; j++) {
            uint32_t c;
            w = rd32(q);
            q += 4;
            if (j < ZB_MAXWIN) {
                m->winw[j] = (int)w;
                for (c = 0; c < w && c < 5; c++) m->win[j][c] = rdf(q + 4 * c);
            }
            q += 4 * w;
        }
        for (j = 0; j < 10; j++) m->tail[j] = (int)rd32(q + 4 * j);
        q += 40;
        blob = rd32(q);
        q += 4;
        if (blob >= 8 && rd32(q) == 1) {
            uint32_t len = rd32(q + 4);
            if (len <= 64 && 8 + 4 * len + 8 <= blob) {
                m->has_gen = 1;
                m->genlen = (int)len;
                for (j = 0; j < len; j++) m->genw[j] = rdf(q + 8 + 4 * j);
                m->gen_enhance = rdf(q + 8 + 4 * len);
                m->gen_mean = rdf(q + 12 + 4 * len);
            }
        }
        m->tree = mb + treeoff;
        m->pool = mb + pooloff;
        t = m->tree;
        m->ntree = (int)rd32(t);
        if ((int)rd32(t + 4) != m->nstate) { snprintf(err, errlen, "tree/state mismatch"); return -1; }
        m->treeid = (int32_t *)malloc(sizeof(int32_t) * m->ntree);
        m->suboff = (uint32_t *)malloc(sizeof(uint32_t) * m->ntree * m->nstate);
        t += 8;
        for (j = 0; j < (uint32_t)m->ntree; j++) {
            int s;
            m->treeid[j] = (int32_t)rd32(t);
            for (s = 0; s < m->nstate; s++) m->suboff[j * m->nstate + s] = rd32(t + 4 + 8 * s);
            t += 4 + 8 * m->nstate;
        }
        switch (m->type) {
        case ZB_M_LSF: v->m_lsf = m; break;
        case ZB_M_LF0: v->m_lf0 = m; break;
        case ZB_M_DUR: v->m_dur = m; break;
        case ZB_M_PDUR: v->m_pdur = m; break;
        }
    }
    if (!v->m_dur) { snprintf(err, errlen, "APM lacks a duration model"); return -1; }   /* Eva: Dur + PDur only (the rest is the DNN) */
    return 0;
}

/* question evaluation (FUN_180013ab0 / FUN_180014290 / FUN_1800139d0); f = feature value, q = question value */
static int q_eval(const ZbQuestion *q, int f)
{
    int k;
    /* null cell: kind -1 -> evaluated with the feature's declared type and value -1 for '==' and 'in';
       any ordering operator on a null is false */
    if (f == ZB_NULL && q->op >= 2) return 0;
    if (q->op == 1) {
        for (k = 0; k < q->nval; k++)
            if (q->val[k] == f) return 1;
        return 0;
    }
    if (q->nval != 1) return 0;
    switch (q->op) {
    case 0: return f == q->val[0];
    case 2: return q->val[0] < f;
    case 3: return q->val[0] <= f;
    case 4: return f < q->val[0];
    case 5: return f <= q->val[0];
    }
    return 0;
}

/* Walk the decision tree of model m for (centre phone id, state); returns the leaf node (its pool offsets at +4).
 * Nodes (file format): internal {u8 0, u8, u16 question, u32 noOff, u32 yesOff}; leaf {u8 1, 3 pad, u32 off[k]}. */
const uint8_t *zb_tree_leaf(const ZbVoice *v, const ZbModel *m, int phoneid, int state, const int *f)
{
    int t = 0, guard = 0;
    const uint8_t *blob;
    uint32_t o = 4;
    if (m->ntree > 1 || m->treeid[0] != 0x7fffffff) {
        for (t = 0; t < m->ntree && m->treeid[t] != phoneid; t++);
        if (t == m->ntree) return NULL;
    }
    blob = m->tree + m->suboff[t * m->nstate + state];
    while (blob[o] == 0 && guard++ < 100000) {
        const ZbQuestion *q = &v->q[rd16(blob + o + 2)];
        int val = q->feat >= 0 ? f[q->feat] : ZB_NULL;
        o = q_eval(q, val) ? rd32(blob + o + 8) : rd32(blob + o + 4);
    }
    return blob[o] == 1 ? blob + o : NULL;
}

/* Build the state Gaussian from a leaf (float pools, FUN_180003e94): per stream s at pool+off[s]:
 * f32 weight, f32 meanprec[dim_s], f32 prec[dim_s]; streams concatenated (FUN_180008790). */
int zb_state_gauss(const ZbVoice *v, const ZbModel *m, const uint8_t *leaf, ZbGauss *g)
{
    int s, k, n = 0;
    (void)v;
    g->wmax = 0.0f;
    for (s = 0; s < m->nstream; s++) {
        uint32_t off = rd32(leaf + 4 + 4 * s);
        const uint8_t *e = m->pool + off;
        int dim = m->streamdim[s];
        float w = rdf(e);
        if (off + 4 + 8 * (uint32_t)dim > m->poolsize || n + dim > ZB_MAXDIM) return -1;
        if (s == 0) g->w = w;
        if (w > g->wmax) g->wmax = w;
        for (k = 0; k < dim; k++) {
            g->mp[n + k] = rdf(e + 4 + 4 * k);
            g->p[n + k] = rdf(e + 4 + 4 * dim + 4 * k);
        }
        n += dim;
    }
    g->dim = n;
    return 0;
}

/* ---------------------------------------------------------------- INI */
static void trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s)) memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
}
static int yes(const char *s) { return !strcmp(s, "yes") || !strcmp(s, "Yes") || !strcmp(s, "true") || !strcmp(s, "True") || !strcmp(s, "1"); }

int zb_ini_load(ZbVoice *v, const char *path)
{
    ZbConfig *c = &v->cfg;
    FILE *f;
    char line[512], sec[128] = "";
    /* ctor defaults (FUN_180017a48 / FUN_180056c00 / built-in table 0x18016d790) */
    c->volume_adj = 100; c->sps_volume_adj = 100; c->pitch_adj = 100; c->rate_adj = 100; c->pitch_range_scale = 100;
    c->normal_factor = 100; c->mix_rate_scale = 0; c->spectrum_adj = 100;
    c->uv_frame_level = 0; c->voiced_thr = 0.5f;
    c->pdur_weight = 0.9f; c->max_frames = 40;
    c->sh_freqdep = 0; c->sh_multi = 0; c->sh_intervals = 2; c->sh_iters = 1; c->sh_step = -1.0f; c->sh_top = 0.7f;
    c->sh_low = 0.075f; c->sh_high = 0.4f;
    c->qi_enabled = 0; c->qi_rate = 0; c->qi_max = 0;
    c->delay_mode = 0; c->silence_noise_suppress = 0; c->sentence_boundary_ms = 0;
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *eq, key[128], val[256];
        trim(line);
        if (line[0] == '[') {
            char *e = strchr(line, ']');
            if (e) *e = 0;
            snprintf(sec, sizeof sec, "%.127s", line + 1);
            continue;
        }
        eq = strchr(line, '=');
        if (!eq || line[0] == ';') continue;
        *eq = 0;
        snprintf(key, sizeof key, "%.127s", line);
        snprintf(val, sizeof val, "%.255s", eq + 1);
        trim(key);
        trim(val);
#define IS(s, k) (!strcmp(sec, s) && !strcmp(key, k))
        if (IS("VoiceSetting", "VolumeAdjustment")) c->volume_adj = atoi(val);
        else if (IS("VoiceSetting", "SpsVolumeAdjustment")) c->sps_volume_adj = atoi(val);
        else if (IS("VoiceSetting", "PitchAdjustment")) c->pitch_adj = atoi(val);
        else if (IS("VoiceSetting", "RateAdjustment")) c->rate_adj = atoi(val);
        else if (IS("VoiceSetting", "PitchRangeScale")) c->pitch_range_scale = atoi(val);
        else if (IS("VoiceSetting", "MSTTS_SpeakRate_NormalFactor")) c->normal_factor = atoi(val);
        else if (IS("VoiceSetting", "MixRateScale")) c->mix_rate_scale = atoi(val);
        else if (IS("VoiceSetting", "WaveScale")) c->wavescale_sonic = !zb_stricmp(val, "Sonic");
        else if (IS("VoiceSetting", "SpectrumAdjustment")) c->spectrum_adj = atoi(val);
        else if (IS("UVDecision", "FrameLevel")) c->uv_frame_level = yes(val);
        else if (IS("UVDecision", "VoicedWeightThreshold")) c->voiced_thr = (float)atof(val);
        else if (IS("PhoneDuration", "Weight")) c->pdur_weight = (float)atoi(val) / 100.0f;
        else if (IS("PhoneDuration", "MaxFramesPerPhone")) c->max_frames = atoi(val);
        else if (IS("LsfSharpen", "FrequencyDependent")) c->sh_freqdep = yes(val);
        else if (IS("LsfSharpen", "MultipleInterval")) c->sh_multi = yes(val);
        else if (IS("LsfSharpen", "Intervals")) c->sh_intervals = atoi(val);
        else if (IS("LsfSharpen", "Iterations")) c->sh_iters = atoi(val);
        else if (IS("LsfSharpen", "Step")) c->sh_step = (float)atof(val);
        else if (IS("LsfSharpen", "TopStep")) c->sh_top = (float)atof(val);
        else if (IS("LsfSharpen", "LowFrequency")) c->sh_low = (float)atof(val);
        else if (IS("LsfSharpen", "HighFrequency")) c->sh_high = (float)atof(val);
        else if (IS("QuestionIntonation", "LogF0RisingRate")) { c->qi_enabled = 1; c->qi_rate = (float)atof(val); }
        else if (IS("QuestionIntonation", "LogF0MaxValue")) { c->qi_enabled = 1; c->qi_max = (float)atof(val); }
        else if (IS("LSPDecomposing", "DelayMode")) c->delay_mode = atoi(val);
        else if (IS("SilenceNoiseSuppress", "Enable")) c->silence_noise_suppress = yes(val);
        else if (IS("SilenceLength", "SentenceBoundary")) c->sentence_boundary_ms = atoi(val);
        else if (!strcmp(sec, "Domain") && !strncmp(key, "Domain", 6) && isdigit((unsigned char)key[6])) {
            int i = atoi(key + 6);
            if (i >= 0 && i < 8) {
                snprintf(c->domain[i], sizeof c->domain[i], "%.31s", val);
                if (c->domain_rate[i] == 0) c->domain_rate[i] = 100;
                if (c->ndomain < i + 1) c->ndomain = i + 1;
            }
        } else if (!strcmp(sec, "Domain") && !strncmp(key, "RateAdjustment", 14) && isdigit((unsigned char)key[14])) {
            int i = atoi(key + 14);
            if (i >= 0 && i < 8) c->domain_rate[i] = atoi(val);
        } else if (!strcmp(sec, "EmotionRecipe")) {   /* FUN_180016da0 */
            static const struct { const char *k; size_t n; int f; } EK[5] = {
                {"Emotion", 7, -1}, {"RateAdjustment", 14, 0}, {"PitchAdjustment", 15, 1},
                {"PitchRangeScale", 15, 2}, {"VolumeAdjustment", 16, 3}};
            int j;
            for (j = 0; j < 5; j++) {
                int i;
                if (strncmp(key, EK[j].k, EK[j].n) || !isdigit((unsigned char)key[EK[j].n])) continue;
                i = atoi(key + EK[j].n);
                if (i < 0 || i >= 8) break;
                if (c->nemotion < i + 1) c->nemotion = i + 1;
                switch (EK[j].f) {
                case -1: snprintf(c->emotion[i], sizeof c->emotion[i], "%.31s", val); break;
                case 0: c->emo[i].rate = atoi(val); break;
                case 1: c->emo[i].pitch = atoi(val); break;
                case 2: c->emo[i].range = atoi(val); break;
                default: c->emo[i].volume = atoi(val); break;
                }
                break;
            }
        }
#undef IS
    }
    fclose(f);
    if (c->sh_step < 0) c->sh_step = c->sh_multi ? 0.3f : 0.4f;
    if (c->pdur_weight < 0) c->pdur_weight = 0;
    if (c->pdur_weight > 1) c->pdur_weight = 1;
    return 0;
}

/* ---- emotion recipes (zb.h) ---- */
const char *zb_emotion_name(const ZbVoice *v, int i)
{
    if (!v || i < 0 || i >= v->cfg.nemotion || !v->cfg.emotion[i][0]) return NULL;
    return v->cfg.emotion[i];
}

int zb_emotion(const ZbVoice *v, const char *name, ZbStyle *out)
{
    int i;
    memset(out, 0, sizeof *out);
    if (!name || !name[0]) return 0;
    if (!v) return -1;
    for (i = 0; i < v->cfg.nemotion; i++)
        if (v->cfg.emotion[i][0] && !zb_stricmp(v->cfg.emotion[i], name)) {
            *out = v->cfg.emo[i];
            return 0;
        }
    return -1;
}
