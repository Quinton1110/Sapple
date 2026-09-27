/* zb_dur.c - CDurationPredictor (FUN_1800692d0), rate (FUN_18006cda8) and the duration rescalers
 * (FUN_1800a47ac, FUN_1800a4710). All float ops in the engine's order (SSE single precision). */
#include <math.h>
#include <string.h>
#include "zb_internal.h"

/* powf(3.0f, (float)(k/10.0)) for k = -20..20 and powf(2.0f, (float)(k/24.0)) for k = -10..10, as returned by the
 * UCRT (these are the correctly rounded values). */
static const uint32_t pow3_tab[41] = {
    0x3de38e39, 0x3dfdfae4, 0x3e0dbc8c, 0x3e1e3205, 0x3e3090c7, 0x3e4511a3, 0x3e5bf402, 0x3e757ead, 0x3e890050,
    0x3e98e905, 0x3eaaaaab, 0x3ebe7c2b, 0x3ed49ad1, 0x3eed4b08, 0x3f046c95, 0x3f13cd3a, 0x3f24f702, 0x3f381f01,
    0x3f4d8079, 0x3f655d89, 0x3f800000, 0x3f8edd20, 0x3f9f741d, 0x3fb1f846, 0x3fc6a2e0, 0x3fddb3d7, 0x3ff77283,
    0x400a1741, 0x401a205b, 0x402c0626, 0x40400000, 0x40564bb0, 0x406f2e2c, 0x40857a34, 0x4094fa28, 0x40a646e1,
    0x40b995e2, 0x40cf22e2, 0x40e73087, 0x4101049d, 0x41100000};
static const uint32_t pow2_tab[21] = {
    0x3f3fc887, 0x3f45672a, 0x3f4b2ff5, 0x3f512424, 0x3f5744fd, 0x3f5d93ce, 0x3f6411f0, 0x3f6ac0c7, 0x3f71a1bf,
    0x3f78b651, 0x3f800000, 0x3f83c02d, 0x3f879c7d, 0x3f8b95c2, 0x3f8facd6, 0x3f93e299, 0x3f9837f0, 0x3f9cadc9,
    0x3fa14518, 0x3fa5fed7, 0x3faadc08};
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
float zb_pow3_tenth(int k) { if (k < -20) k = -20; if (k > 20) k = 20; return u2f(pow3_tab[k + 20]); }
float zb_pow2_24th(int k) { if (k < -10) k = -10; if (k > 10) k = 10; return u2f(pow2_tab[k + 10]); }

/* FUN_1800a47ac: rescale n state durations to sum max(n, target) */
void zb_dur_rescale(unsigned *sum, unsigned target, int *dur, unsigned n)
{
    unsigned tgt, old, i, s;
    float carry = 0.0f;
    if (!dur || *sum < n) return;
    old = *sum;
    tgt = n <= target ? target : n;
    *sum = 0;
    s = 0;
    for (i = 0; i < n; i++) {
        float y = (float)(unsigned)dur[i] * ((float)tgt / (float)old);
        double v = (double)(y + carry) + 0.5;
        unsigned d = 1;
        if (1.0 < v) d = (unsigned)(long long)v;
        dur[i] = (int)d;
        s = *sum + d;
        *sum = s;
        carry = carry + (y - (float)d);
    }
    while (s != tgt) {
        unsigned k = 0, j, dk, nd;
        for (j = 1; j < n; j++)
            if (!((unsigned)dur[j] <= (unsigned)dur[k])) k = j;
        dk = (unsigned)dur[k];
        nd = 1;
        if (*sum + 1 < tgt + dk) nd = (tgt - *sum) + dk;
        dur[k] = (int)nd;
        s = nd + (*sum - dk);
        *sum = s;
    }
}

/* FUN_1800a4710: silence phones: put the difference on the middle state */
static void dur_rescale_sil(unsigned *sum, unsigned target, int *dur, unsigned n)
{
    unsigned mid, s = *sum, old, nd;
    if (!dur || !n || !s || !(n & 1)) { *sum = 0; return; }
    mid = n >> 1;
    if (s < target) { dur[mid] += (int)(target - s); *sum = target; return; }
    if (s <= target) return;
    old = (unsigned)dur[mid];
    nd = old < (s - target) + 1 ? 1 : old - (s - target);
    dur[mid] = (int)nd;
    *sum += nd - old;
    if (*sum == target) return;
    zb_dur_rescale(sum, target, dur, n);
}

/* FUN_1800a48f8 */
static void dur_set(const ZbPhoneCtl *c, unsigned *sum, unsigned target, int *dur, unsigned n)
{
    if (!n) return;
    if (c->is_sil) dur_rescale_sil(sum, target, dur, n);
    else zb_dur_rescale(sum, target, dur, n);
}

static unsigned ms_to_frames(const ZbVoice *v, int ms)
{
    if (ms <= 0) return 0;
    return (unsigned)(((unsigned long long)((unsigned)v->rate / 1000u) * (unsigned)ms) / (unsigned)v->shift);
}

/* FUN_18006cda8 (no domain / NUS / absolute-rate paths) */
/* returns 0 when MixRateScale rescaled the durations (the engine then clears the segment's waveform time-stretch
 * flag, segment+0x4c), 1 otherwise (the flag stays set; see zb_wave.c) */
static int dur_rate(const ZbVoice *v, const ZbPhoneCtl *c, const ZbSapi *sapi, unsigned *sum, int *dur, unsigned n)
{
    const ZbConfig *cfg = &v->cfg;
    int site = sapi ? sapi->rate : 0;
    float sr = 1.0f, f;
    unsigned R;
    if (site < -10) site = -10;
    if (site > 10) site = 10;
    if (n != 0 && !(c->is_pause && c->word_type == 3) && c->phone_ms != 0) {
        unsigned fr = ms_to_frames(v, c->phone_ms);
        if (fr) dur_set(c, sum, fr, dur, n);
    }
    /* MSTTS.SpeakRate property (0 unless set by the app): sr = ((g(x)-1)*100)/100 + 1 = 1 */
    /* R = Config.RateAdjustment, plus the emotion recipe's RateAdjustment (see ZbStyle in zb.h; neutral = no
     * change, so this stays bit-identical to the engine for plain speech) */
    R = (unsigned)zb_style_pct(cfg->rate_adj, sapi ? sapi->style.rate : 0);
    if (site == 0 && c->rate == 0 && R == 100 && !(sapi && sapi->rate_milli)) return 1;
    f = zb_pow3_tenth(c->rate) * zb_pow3_tenth(site);
    /* ClassicVoices patch: fractional rate (0 = untouched) */
    if (sapi && sapi->rate_milli) f = f * (float)pow(3.0, (double)sapi->rate_milli / 10000.0);
    f = f * sr;
    if (R != 100 && !c->is_sil) f = ((float)R * f) / 100.0f;
    if (cfg->mix_rate_scale) {
        unsigned nw = (unsigned)(long long)((float)*sum / f);
        if (n < nw) { dur_set(c, sum, nw, dur, n); return 0; }
    }
    return 1;
}

/* CDurationPredictor main. gd[p]: state-duration Gaussian (dim nstate), gp[p]: phone-duration Gaussian (or NULL). */
void zb_durations(const ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, const ZbGauss *gd, const ZbGauss *gp, int nstate,
                  int *dur)
{
    zb_durations_ex(v, u, sapi, gd, gp, nstate, dur, NULL);
}

/* as zb_durations; stretch[p] (optional) receives the segment's waveform time-stretch flag (segment+0x4c) */
void zb_durations_ex(const ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, const ZbGauss *gd, const ZbGauss *gp, int nstate,
                     int *dur, int *stretch)
{
    zb_durations_heq(v, u, sapi, gd, gp, nstate, dur, stretch, NULL, NULL, 0.0f);
}

/* ClassicVoices patch (Microsoft Sarah): the same, with the neural voices' duration histogram equalisation (engine duration predictor,
 * Win11 build ~0x1800235xx; HEQ table stream 6): for a non-silence phone T = heq(T)*a + (1-a)*T, a =
 * DurationHeqAdjustment/100, and with a table present and a != 0 the per-phone frame limit is skipped.  heq == NULL is
 * exactly zb_durations_ex. */
void zb_durations_heq(const ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, const ZbGauss *gd, const ZbGauss *gp, int nstate,
                      int *dur, int *stretch, float (*heq)(const void *ctx, float x), const void *heq_ctx, float heq_adj)
{
    const ZbConfig *cfg = &v->cfg;
    int p, i;
    float carry = 0.0f;
    for (p = 0; p < u->nphone; p++) {
        const ZbGauss *g = &gd[p];
        const ZbPhoneCtl *c = &u->ph[p].c;
        int *d = dur + p * nstate;
        float S = 0.0f, V = 0.0f, T, rho = 0.0f;
        unsigned sum = 0, limit;
        for (i = 0; i < g->dim; i++) {
            S = g->mp[i] / g->p[i] + S;
            V = 1.0f / g->p[i] + V;
        }
        T = S;
        if (gp) {
            float P = gp[p].mp[0] / gp[p].p[0];
            if (P <= (float)nstate) P = (float)nstate;
            T = (P - S) * cfg->pdur_weight + S;
        }
        if (heq && !c->is_sil && !c->is_pause) {   /* ClassicVoices patch (Sarah): duration HEQ */
            float h = heq(heq_ctx, T) * heq_adj, r = (1.0f - heq_adj) * T;
            T = h + r;
        }
        if (0.0f < V) rho = (T - S) / V;
#ifdef ZB_DUR_CARRY_RESET
        carry = 0.0f;
#endif
        for (i = 0; i < nstate; i++) {
            float m = g->mp[i] / g->p[i], x;
            int di;
            x = (m == 0.0f) ? 0.0f : rho / g->p[i];
            di = (int)(long long)(((x + m) + carry) + 0.5f);
            if (di < 2) di = 1;
            carry = carry + ((x + m) - (float)di);
            d[i] = di;
            sum += (unsigned)di;
        }
        if (1.0 < fabs((double)carry)) zb_dur_rescale(&sum, (unsigned)(int)(T + 0.5f), d, (unsigned)nstate);
        limit = c->is_pause ? 80u : (unsigned)cfg->max_frames;
        if (!(heq && heq_adj != 0.0f) && limit < sum) zb_dur_rescale(&sum, limit, d, (unsigned)nstate);   /* (Sarah: no limit with a HEQ) */
        i = dur_rate(v, c, sapi, &sum, d, (unsigned)nstate);
        if (stretch) stretch[p] = i;
    }
}
