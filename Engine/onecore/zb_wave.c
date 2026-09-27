/* zb_wave.c - CWaveGenerator post-vocoder stage for SPS units (FUN_18004e460):
 *   rate change on the waveform (FUN_18004dd00: CRateChangerSonic + CWaveSmoother), voice volume (FUN_18001fedc +
 *   FUN_18002c420), per-segment SAPI volume (FUN_18006d71c), segment length redistribution (FUN_1801050c0).
 * The vocoder output is cut into the engine's synthesis blocks (FUN_180009b14 grouping), each block goes through
 * the rate stage. Only segments whose durations could not absorb the speaking rate (see zb_durations_ex, segment
 * +0x4c) are time-stretched, by Sonic (VoiceSetting.WaveScale=Sonic: David) or by CRateChanger (zb_ratechg.c: Zira,
 * Mark); the rate changer state and the previous block rate persist across utterances. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "zb_internal.h"
#include "zb_sonic.h"
#include "zb_ratechg.h"

#define ZB_SMOOTH 240   /* CWaveSmoother length (FUN_18011287c(wavegen+0xa0, 0xf0, .., 1)) */

struct ZbWave {
    ZbSonic *sonic;     /* VoiceSetting.WaveScale=Sonic (David) */
    ZbRateChg *rc;      /* otherwise: CRateChanger (Zira, Mark) */
    int prev_rate;      /* CWaveGenerator+0x160 */
    float ramp[ZB_SMOOTH];
};

static struct ZbWave *wave_state(ZbVoice *v)
{
    if (!v->wave) {
        int k;
        v->wave = (struct ZbWave *)calloc(1, sizeof(struct ZbWave));
        if (v->cfg.wavescale_sonic) v->wave->sonic = zb_sonic_create(v->rate);
        else v->wave->rc = zb_rc_create(v->rate);
        for (k = 0; k < ZB_SMOOTH; k++) v->wave->ramp[k] = (float)(unsigned)k / (float)ZB_SMOOTH;
    }
    return v->wave;
}

void zb_wave_free(ZbVoice *v)
{
    if (v && v->wave) {
        if (v->wave->sonic) zb_sonic_free(v->wave->sonic);
        if (v->wave->rc) zb_rc_free(v->wave->rc);
        free(v->wave);
        v->wave = NULL;
    }
}

/* FUN_1800665c8 via FUN_180112904 / FUN_1801052cc; fadeout = 1: ramp down (applied to the tail of flushed Sonic
 * output), 0: ramp up (applied to the head of the next block) */
static void smooth(const struct ZbWave *w, short *s, long n, int fadeout)
{
    const unsigned N = ZB_SMOOTH;
    unsigned c, i;
    if (!n) return;
    if (fadeout && (unsigned long)n > N) { s += n - N; n = N; }
    c = (unsigned)n < N ? (unsigned)n : N;
    if (c < N) {
        float f = 1.0f;
        if (c >= 2) f = (float)(N - 1) / (float)(c - 1);
        for (i = 0; i < c; i++) {
            unsigned idx = fadeout ? c - 1 - i : i;
            s[i] = (short)(int)((float)(int)s[i] * w->ramp[(int)((float)(int)idx * f + 0.5f)]);
        }
    } else {
        for (i = 0; i < N; i++) s[i] = (short)(int)((float)(int)s[i] * w->ramp[fadeout ? N - 1 - i : i]);
    }
}

typedef struct { short *p; long n, cap; } Buf;
static void put(Buf *b, const short *x, long n)
{
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 1024; b->p = (short *)realloc(b->p, sizeof(short) * (size_t)b->cap); }
    if (n) memcpy(b->p + b->n, x, sizeof(short) * (size_t)n);
    b->n += n;
}

/* FUN_180074908 */
static int round_away(float x) { return (int)(0.0f < x ? x + 0.5f : x - 0.5f); }

/* FUN_18004e254 in fine mode (wavegen+0x20c set: rates in 1/1000 steps; identical results to the coarse mode for integer
 * rates): clamp(1000*xml + segment+0x40, -10000, 10000) + 1000*site rate, plus the percent adjustments RateAdjustment
 * and the word's domain RateAdjustment converted by FUN_180067e20: round_away(log3(pct/100) * 10 * 1000), unless the
 * word is exempt (word+0x90 == 1); clamp to [-20000, 20000]. (No emotion / utterance rate object for SAPI text.) */
static int block_rate(const ZbVoice *v, const ZbPhoneCtl *c, const ZbSapi *sapi)
{
    const ZbConfig *cfg = &v->cfg;
    int r = 1000 * c->rate + c->seg_rate, site = sapi ? sapi->rate : 0;
    if (r < -10000) r = -10000;
    if (r > 10000) r = 10000;
    if (site < -10) site = -10;
    if (site > 10) site = 10;
    r += 1000 * site;
    if (sapi) r += sapi->rate_milli;   /* ClassicVoices patch: fractional rate, fine mode (0 = untouched) */
    if (c->word90 != 1) {
        /* + emotion.RateAdjustment - 100 (decompile 0x18004e2e5; see ZbStyle in zb.h) */
        int pct = zb_style_pct(cfg->rate_adj, sapi ? sapi->style.rate : 0), dom = 100, k;
        unsigned u;
        for (k = 0; c->domain[0] && k < cfg->ndomain; k++)
            if (cfg->domain[k][0] && !zb_stricmp(cfg->domain[k], c->domain)) dom = cfg->domain_rate[k];
        u = (unsigned)(dom - 100 + pct);
        if (0.0f < (float)u / 100.0f) r += round_away(((logf((float)u / 100.0f) / logf(3.0f)) * 10.0f) * 1000.0f);
    }
    if (r < -20000) r = -20000;
    if (r > 20000) r = 20000;
    return r;
}

/* FUN_18004dd00: speed = powf(3, (float)(rate / 10000.0)) (times MSTTS.SpeakRate factor and absolute rate, both 1) */
static float rate_speed(int r)
{
    if (r % 1000 == 0) return zb_pow3_tenth(r / 1000);
    return (float)pow(3.0, (double)(float)((double)r / 10000.0));
}

/* FUN_1801050c0: distribute m output samples over the block's segments (lengths len[], in samples) */
static void redistribute(long *len, int nseg, long m)
{
    long tot = 0, done = 0;
    int k;
    float scale, acc = 0.0f;
    for (k = 0; k < nseg; k++) tot += len[k];
    scale = tot ? (float)(unsigned)m / (float)(unsigned)tot : 0.0f;
    for (k = 0; k < nseg; k++) {
        long nk;
        if (k == nseg - 1) nk = m - done;
        else {
            double fl;
            acc = (float)(unsigned)len[k] * scale + acc;
            fl = floor((double)(acc + 0.5f));
            nk = (long)(unsigned)(long long)fl;
            acc = acc - (float)(unsigned)(long long)fl;
        }
        done += nk;
        len[k] = nk;
    }
}

long zb_wave_sps(ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, const int *dur, int nstate, const int *stretch,
                 const int *blk, int nblk, const short *pcm, short **out)
{
    struct ZbWave *w = wave_state(v);
    const int shift = v->shift, mix = v->cfg.mix_rate_scale;
    int np = u->nphone, i = 0, first = 1, p, s, cap = 0, bi = 0;
    long *seglen = (long *)calloc((size_t)np + 1, sizeof(long)), off = 0, pos;
    int *fr = (int *)calloc((size_t)np + 1, sizeof(int));
    Buf o = {0, 0, 0}, fl = {0, 0, 0};
    short *tmp = NULL;
    long tmpcap = 0;
    for (p = 0; p < np; p++) {
        for (s = 0; s < nstate; s++) fr[p] += dur[p * nstate + s];
        if (cap < fr[p]) cap = fr[p];
    }
    while (i < np) {
        int start = i, count = 0, frames = 0, flag = stretch ? stretch[i] : 0, xr = u->ph[i].c.rate, rate, last;
        long nin;
        const short *in = pcm + off;
        /* FUN_180009b14 block grouping (or the caller's blocks: int path, grouped per LSF chunk) */
        if (blk) {
            int c = bi < nblk ? blk[bi++] : np - i;
            for (; c > 0 && i < np; c--) { frames += fr[i]; count++; i++; }
        } else for (;;) {
            if (frames + fr[i] > cap && count) break;
            frames += fr[i];
            count++;
            i++;
            if (i >= np) break;
            if ((stretch ? stretch[i] : 0) != flag || u->ph[i].c.rate != xr) break;
            if (first && count > 1) break;
        }
        nin = (long)frames * shift;
        last = i >= np;
        /* FUN_18004dd00 */
        rate = block_rate(v, &u->ph[start].c, sapi);
        fl.n = 0;
        if ((rate != w->prev_rate || flag == 0) && w->rc) {   /* CRateChanger slot 5 */
            long n = zb_rc_max_out(w->rc, 0);
            if (fl.cap < n + 1) { fl.cap = n + 1; fl.p = (short *)realloc(fl.p, sizeof(short) * (size_t)fl.cap); }
            fl.n = zb_rc_flush(w->rc, fl.p);
            if (mix) smooth(w, fl.p, fl.n, 1);
        } else if (rate != w->prev_rate || flag == 0) {
            zb_sonic_flush(w->sonic);
            put(&fl, NULL, 0);
            if (zb_sonic_available(w->sonic)) {
                long n = zb_sonic_available(w->sonic);
                if (fl.cap < n) { fl.cap = n; fl.p = (short *)realloc(fl.p, sizeof(short) * (size_t)n); }
                fl.n = zb_sonic_read(w->sonic, fl.p, (int)n);
                if (mix) smooth(w, fl.p, fl.n, 1);
            }
        }
        if (flag == 0) rate = 0;
        pos = o.n;
        if (rate == 0) {
            if (fl.n == 0) put(&o, in, nin);          /* passthrough: segment lengths unchanged */
            else {
                put(&o, fl.p, fl.n);
                put(&o, in, nin);
                if (mix) smooth(w, o.p + pos + fl.n, nin, 0);
            }
        } else {
            long m;
            int flags = first | (last ? 2 : 0);
            if (w->rc) {   /* CRateChanger slots 1 and 4: the block flags go to process (bit0 reset, bit1 flush) */
                zb_rc_set_speed(w->rc, (double)rate_speed(rate));
                m = zb_rc_max_out(w->rc, (int)nin);
                if (tmpcap < m + 1) { tmpcap = m + 1; tmp = (short *)realloc(tmp, sizeof(short) * (size_t)tmpcap); }
                m = zb_rc_process(w->rc, in, (int)nin, tmp, flags);
            } else {
                zb_sonic_set_speed(w->sonic, rate_speed(rate));
                if (nin > 0) zb_sonic_write(w->sonic, in, (int)nin);
                if (flags == 2) zb_sonic_flush(w->sonic);
                m = zb_sonic_available(w->sonic);
                if (tmpcap < m + 1) { tmpcap = m + 1; tmp = (short *)realloc(tmp, sizeof(short) * (size_t)tmpcap); }
                m = zb_sonic_read(w->sonic, tmp, (int)m);
            }
            if (fl.n == 0) put(&o, tmp, m);
            else {
                put(&o, fl.p, fl.n);
                if (m == 0) { put(&o, in, nin); if (mix) smooth(w, o.p + pos + fl.n, nin, 0); }
                else { put(&o, tmp, m); if (mix) smooth(w, o.p + pos + fl.n, m, 0); }
            }
        }
        for (p = start; p < i; p++) seglen[p] = (long)fr[p] * shift;
        if (!(rate == 0 && fl.n == 0)) redistribute(seglen + start, i - start, o.n - pos);
        w->prev_rate = rate;
        off += nin;
        first = 0;
    }
    /* voice volume (FUN_18001fedc: SpsVolumeAdjustment, the voices have NUS domain data;
     * + emotion.VolumeAdjustment - 100, decompile 0x18001ff8e) */
    {
        int vol = zb_style_pct(v->cfg.sps_volume_adj, sapi ? sapi->style.volume : 0);
        if (vol != 100) zb_scale_pcm(o.p, o.n, (float)vol / 100.0f);
    }
    /* SAPI volume per segment (FUN_18006d71c) */
    {
        int sitevol = sapi ? sapi->volume : 100;
        long so = 0;
        for (p = 0; p < np; p++) {
            if ((float)u->ph[p].c.volume != 100.0f || sitevol != 100) {
                float x = (((float)sitevol * (float)u->ph[p].c.volume) / 100.0f) / 100.0f;
                zb_scale_pcm(o.p + so, seglen[p], zb_sapi_vol_gain(x));
            }
            so += seglen[p];
        }
    }
    free(seglen); free(fr); free(fl.p); free(tmp);
    *out = o.p;
    return o.n;
}
