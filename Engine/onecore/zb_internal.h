/* zb_internal.h - internal structures of the OneCore acoustic backend port (see zb.h). */
#ifndef ZB_INTERNAL_H
#define ZB_INTERNAL_H
#include <stdint.h>
#include <stddef.h>
#include "zb.h"

#define ZB_MAXWIN 3
#define ZB_MAXDIM 75
#define ZB_NSTATE 5

/* model types (CHTSAcousticModelHeader +0) */
enum { ZB_M_LSF = 1, ZB_M_LF0 = 2, ZB_M_DUR = 3, ZB_M_PDUR = 6, ZB_M_MBE = 7 };

typedef struct {
    int feat;        /* ZB_F_* column (or -1 if the APM feature is unknown) */
    int op;          /* 0 ==, 1 in, 2 q<f, 3 q<=f, 4 f<q, 5 f<=q */
    int nval;
    int32_t *val;
} ZbQuestion;

typedef struct {
    int type, msd, nstate, nstream;
    int streamdim[4], streamid[4], totdim;
    int nwin, winw[ZB_MAXWIN];
    float win[ZB_MAXWIN][5];
    int tail[10];                  /* nMix, staticDim, +0x54.., quantFlag(+0x64 = tail[6]), meanBits, precBits */
    const uint8_t *tree;           /* tree region */
    const uint8_t *pool;           /* pool region (starts with u32 nEntries) */
    uint32_t poolsize;
    int ntree;
    int32_t *treeid;               /* [ntree] */
    uint32_t *suboff;              /* [ntree*nstate] offsets of the sub-tree blobs from the tree region */
    /* "customized generation setting" blob (logF0 only) */
    int has_gen, genlen;
    float genw[64], gen_enhance, gen_mean;
} ZbModel;

typedef struct {
    /* [VoiceSetting] */
    int volume_adj, sps_volume_adj, pitch_adj, rate_adj, pitch_range_scale;
    int normal_factor, mix_rate_scale;
    int wavescale_sonic;           /* VoiceSetting.WaveScale == Sonic: CRateChangerSonic, else CRateChanger */
    int spectrum_adj;
    /* [UVDecision] */
    int uv_frame_level;
    float voiced_thr;
    /* [PhoneDuration] */
    float pdur_weight;
    int max_frames;
    /* [LsfSharpen] */
    int sh_freqdep, sh_multi, sh_intervals, sh_iters;
    float sh_step, sh_top, sh_low, sh_high;
    /* [QuestionIntonation] */
    int qi_enabled;
    float qi_rate, qi_max;
    int delay_mode;
    int silence_noise_suppress;
    int sentence_boundary_ms;
    /* [Domain] Number / Domain<i> / RateAdjustment<i> (NUS domains; only the rate adjustment matters for SPS text) */
    int ndomain;
    char domain[8][32];
    int domain_rate[8];
    /* [EmotionRecipe] Number / Emotion<i> / RateAdjustment<i> / PitchAdjustment<i> / PitchRangeScale<i> /
     * VolumeAdjustment<i> (CTTSEmotionSetting, FUN_180016da0); see ZbStyle in zb.h */
    int nemotion;
    char emotion[8][32];
    ZbStyle emo[8];
} ZbConfig;

/* a base percentage combined with an emotion percentage: X + emotion.X - 100 (emotion 0 or 100 = neutral) */
static inline int zb_style_pct(int base, int emo) { return (emo && emo != 100) ? base + emo - 100 : base; }

struct ZbVoice {
    uint8_t *apm;
    size_t apmsize;
    int isint;                     /* APM header flag B (file 0x24) */
    int rate, bits, shift;
    int nfeat;                     /* APM features */
    int featcol[64];               /* APM feature index -> ZB_F_* */
    int nq;
    ZbQuestion *q;
    int nmodel;
    ZbModel model[8];
    ZbModel *m_lsf, *m_lf0, *m_dur, *m_pdur;
    ZbConfig cfg;
    uint32_t rnd;                  /* CRT rand() state (per thread in the engine; per voice here) */
    /* vocoder tables */
    float hv[41], hu[41];
    float noise[1024], noise_uv[1024];
    float g40, g80;
    /* fixed-point vocoder (zb_vocoder_int.c): Q13/Q14 tables and state carried across LSF chunks of an utterance */
    short hv_i[41], noise_i[1024], noise_uv_i[1024], g40_i, g80_i;
    int iexc_last, iexc_prevf0, ihist_valid;
    unsigned char ihist[41];
    short isyn_hist[64];
    struct ZbWave *wave;           /* waveform rate stage state (zb_wave.c): Sonic stream, previous block rate */
};

/* per-state Gaussian in canonical form, concatenated over streams */
typedef struct {
    float w;                       /* MSD / mixture weight of the first stream */
    float wmax;                    /* max over the state's streams (UV decision) */
    int dim;
    float mp[ZB_MAXDIM];           /* mean * precision */
    float p[ZB_MAXDIM];            /* precision */
} ZbGauss;

/* CRT rand (UCRT): s = s*214013+2531011; return (s>>16)&0x7fff */
static inline void zb_srand(ZbVoice *v, uint32_t s) { v->rnd = s; }
static inline int zb_rand(ZbVoice *v) { v->rnd = v->rnd * 214013u + 2531011u; return (int)((v->rnd >> 16) & 0x7fff); }

/* zb_apm.c */
int zb_apm_load(ZbVoice *v, const char *path, char *err, int errlen);
int zb_ini_load(ZbVoice *v, const char *path);
const uint8_t *zb_tree_leaf(const ZbVoice *v, const ZbModel *m, int phoneid, int state, const int *f);
int zb_state_gauss(const ZbVoice *v, const ZbModel *m, const uint8_t *leaf, ZbGauss *g);

/* zb_dur.c */
float zb_pow3_tenth(int k);
float zb_pow2_24th(int k);
void zb_dur_rescale(unsigned *sum, unsigned target, int *dur, unsigned n);
void zb_durations_ex(const ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, const ZbGauss *gd, const ZbGauss *gp, int nstate,
                     int *dur, int *stretch);
/* ClassicVoices patch (Microsoft Sarah): the same with the neural voices' duration HEQ; heq == NULL = zb_durations_ex */
void zb_durations_heq(const ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, const ZbGauss *gd, const ZbGauss *gp, int nstate,
                      int *dur, int *stretch, float (*heq)(const void *ctx, float x), const void *heq_ctx, float heq_adj);
void zb_durations(const ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, const ZbGauss *gd, const ZbGauss *gp, int nstate,
                  int *dur);

/* zb_mlpg.c */
/* frames: per compact frame the Gaussian (already boundary-zeroed copies), D static dims, nwin 3 */
void zb_mlpg_float(const ZbModel *m, ZbGauss *const *fr, int T, int D, float *out /*[T*D]*/);
/* ClassicVoices patch (Eva): same, on flat arrays prec/mp [T][3][D] */
void zb_mlpg_float_arr(const ZbModel *m, const float *prec, const float *mp, int T, int D, float *out /*[T*D]*/);

/* zb_wave.c: post-vocoder SPS wave stage (Sonic rate change + smoother, voice volume, per-phone SAPI volume).
 * pcm: nframes*shift samples of the utterance (after silence zeroing); returns a malloc'ed buffer in *out. */
long zb_wave_sps(ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, const int *dur, int nstate, const int *stretch,
                 const int *blk, int nblk, const short *pcm, short **out);   /* blk: synthesis blocks (NULL: computed) */
void zb_wave_free(ZbVoice *v);
float zb_sapi_vol_gain(float x);
void zb_scale_pcm(short *s, long n, float g);

/* zb_vocoder.c */
/* zb_synth.c (ClassicVoices patch: exported for the Eva back end) */
void zb_apply_pitch(const ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, const int *dur, int nst, float *f0, int T);
void zb_lsf_repair(float *lsf, int rows, int p);
void zb_vocoder_init(ZbVoice *v);
/* same as zb_vocoder_float with an explicit LSF order p (24 for the HMM voices, 40 for Eva) */
void zb_vocoder_float_p(ZbVoice *v, const ZbUtt *u, const int *dur, int nstate, int T, const float *f0, float *lsf,
                        const float *gain, const int *stretch, short *pcm, int p);
/* ClassicVoices patch (Microsoft Sarah): with the five-band excitation, mbe = [T][nb] strengths (NULL = zb_vocoder_float_p) */
#define ZB_MBE_MAXBANDS 5
void zb_vocoder_float_mbe(ZbVoice *v, const ZbUtt *u, const int *dur, int nstate, int T, const float *f0, float *lsf,
                          const float *gain, const int *stretch, short *pcm, int p, const float *mbe, int nb);
void zb_vocoder_float(ZbVoice *v, const ZbUtt *u, const int *dur, int nstate, int T, const float *f0, float *lsf, const float *gain,
                      const int *stretch, short *pcm);
int zb_split_blocks(const ZbUtt *u, const int *phfr, int ph0, int nph, const int *stretch, int cap, int *blk);

/* zb_vocoder_int.c / zb_synth_int.c */
void zb_vocoder_int_init(ZbVoice *v);
void zb_vocoder_int_reset(ZbVoice *v);
/* phfr: frames of the chunk's phones; blk: phone counts of the chunk's synthesis blocks */
void zb_vocoder_int_chunk(ZbVoice *v, const ZbUtt *u, const int *phfr, const int *blk, int nblk, int first, int last,
                          const int32_t *f0, int32_t *lsf, int32_t *gain, int T0, int nfr, short *pcm);
void zb_ilsf_repair(int32_t *lsf, int rows, int p);
void zb_silence_zero(const ZbVoice *v, const ZbUtt *u, const int *dur, int nstate, short *pcm);

static inline float zb_f32(double x) { return (float)x; }
static inline int zb_stricmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
}

#endif
