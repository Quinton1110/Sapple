/* zb_sonic.c - the time-scale modification library embedded in MSTTSEngine_OneCore.dll (CRateChangerSonic,
 * FUN_180085590 .. FUN_180086440). It is the open-source "Sonic" algorithm (pitch-synchronous overlap-add); this
 * file transcribes the compiled engine behaviour (16-bit samples, mono, pitch = rate = volume = 1, quality 0,
 * no chord pitch), including its 32-bit unsigned AMDF arithmetic and integer overlap-add. */
#include <stdlib.h>
#include <string.h>
#include "zb_sonic.h"

struct ZbSonic {
    short *in, *out, *down;
    int incap, outcap;
    int nin, nout;
    float speed;
    int minp, maxp, maxreq, remain, prevperiod, prevmindiff, rate;
};

ZbSonic *zb_sonic_create(int rate)
{
    ZbSonic *s = (ZbSonic *)calloc(1, sizeof *s);
    s->rate = rate;
    s->minp = rate / 400;
    s->maxp = rate / 65;
    s->maxreq = 2 * s->maxp;
    s->incap = s->outcap = s->maxreq;
    s->in = (short *)calloc((size_t)s->incap, 2);
    s->out = (short *)calloc((size_t)s->outcap, 2);
    s->down = (short *)calloc((size_t)s->maxreq, 2);
    s->speed = 1.0f;
    return s;
}

void zb_sonic_free(ZbSonic *s)
{
    if (!s) return;
    free(s->in); free(s->out); free(s->down); free(s);
}

void zb_sonic_set_speed(ZbSonic *s, float speed) { s->speed = speed; }

static void grow_out(ZbSonic *s, int need)
{
    if (s->outcap < s->nout + need) {
        s->outcap = (s->outcap >> 1) + s->outcap + need;
        s->out = (short *)realloc(s->out, (size_t)s->outcap * 2);
    }
}

/* FUN_180085cd0: AMDF over [minp, maxp], unsigned 32-bit products as in the engine */
static int period_in_range(const short *x, int minp, int maxp, unsigned *mindiff_o, unsigned *maxdiff_o)
{
    unsigned best = 0, mind = 1, worst = 255, maxd = 0;
    int p, i;
    for (p = minp; p <= maxp; p++) {
        unsigned diff = 0;
        for (i = 0; i < p; i++) {
            short a = x[i], b = x[i + p];
            unsigned short d = a < b ? (unsigned short)(b - a) : (unsigned short)(a - b);
            diff += d;
        }
        if (!((unsigned)p * mind <= diff * best)) { mind = diff; best = (unsigned)p; }
        if (!(diff * worst <= (unsigned)p * maxd)) { maxd = diff; worst = (unsigned)p; }
    }
    *mindiff_o = mind / best;
    *maxdiff_o = maxd / worst;
    return (int)best;
}

/* FUN_180085b70 with preferNewPeriod = 1 */
static int find_period(ZbSonic *s, const short *x)
{
    int minp = s->minp, maxp = s->maxp, skip = 1, period, ret, i, k;
    unsigned mind, maxd;
    if (4000 < s->rate) skip = s->rate / 4000;
    if (skip == 1) period = period_in_range(x, minp, maxp, &mind, &maxd);
    else {
        int n = s->maxreq / skip;
        for (i = 0; i < n; i++) {
            int acc = 0;
            for (k = 0; k < skip; k++) acc += x[i * skip + k];
            s->down[i] = (short)(acc / skip);
        }
        period = period_in_range(s->down, minp / skip, maxp / skip, &mind, &maxd);
        minp = period * skip - 4 * skip;
        maxp = period * skip + 4 * skip;
        if (minp < s->minp) minp = s->minp;
        if (maxp > s->maxp) maxp = s->maxp;
        period = period_in_range(x, minp, maxp, &mind, &maxd);
    }
    ret = period;
    if (mind != 0 && s->prevperiod != 0) {
        if (!((int)(mind * 3) < (int)maxd || (int)(mind * 2) <= s->prevmindiff * 3)) ret = s->prevperiod;
    }
    s->prevmindiff = (int)mind;
    s->prevperiod = period;
    return ret;
}

/* FUN_180085eb0 */
static void overlap_add(int n, short *out, const short *down, const short *up)
{
    int t;
    for (t = 0; t < n; t++) out[t] = (short)((down[t] * (n - t) + up[t] * t) / n);
}

static int copy_input(ZbSonic *s, const short *x, int n)
{
    grow_out(s, n);
    memcpy(s->out + s->nout, x, (size_t)n * 2);
    s->nout += n;
    return 1;
}

/* FUN_1800856e0 */
static void change_speed(ZbSonic *s, float speed)
{
    int numin = s->nin, pos = 0, ns;
    if (numin < s->maxreq) return;
    do {
        const short *x = s->in + pos;
        if (s->remain < 1) {
            int period = find_period(s, x);
            float fp = (float)period;
            if (speed <= 1.0f) {
                ns = period;
                if (0.5f <= speed) s->remain = (int)((((speed + speed) - 1.0f) * fp) / (1.0f - speed));
                else ns = (int)((fp * speed) / (1.0f - speed));
                grow_out(s, ns + period);
                memcpy(s->out + s->nout, x, (size_t)period * 2);
                overlap_add(ns, s->out + s->nout + period, x + period, x);
                s->nout += ns + period;
                pos += ns;
            } else {
                ns = period;
                if (speed < 2.0f) s->remain = (int)(((2.0f - speed) * fp) / (speed - 1.0f));
                else ns = (int)(fp / (speed - 1.0f));
                grow_out(s, ns);
                overlap_add(ns, s->out + s->nout, x, x + period);
                s->nout += ns;
                pos += period + ns;
            }
        } else {
            ns = s->remain < s->maxreq ? s->remain : s->maxreq;
            copy_input(s, x, ns);
            s->remain -= ns;
            pos += ns;
        }
        if (ns == 0) return;
    } while (pos + s->maxreq <= numin);
    ns = s->nin - pos;
    if (ns > 0) memmove(s->in, s->in + pos, (size_t)ns * 2);
    s->nin = ns;
}

/* FUN_180085f60 (pitch = rate = volume = 1) */
static void process(ZbSonic *s)
{
    float sp = s->speed / 1.0f;
    if (1.00001 < (double)sp || (double)sp < 0.99999) change_speed(s, sp);
    else { copy_input(s, s->in, s->nin); s->nin = 0; }
}

void zb_sonic_write(ZbSonic *s, const short *x, int n)
{
    if (n) {
        if (s->incap < s->nin + n) {
            s->incap = (s->incap >> 1) + s->incap + n;
            s->in = (short *)realloc(s->in, (size_t)s->incap * 2);
        }
        memcpy(s->in + s->nin, x, (size_t)n * 2);
        s->nin += n;
    }
    process(s);
}

/* FUN_180086200 */
void zb_sonic_flush(ZbSonic *s)
{
    int remaining = s->nin, pad = s->maxreq * 2;
    int expected = (int)(((float)remaining / (s->speed / 1.0f) + (float)0) / (1.0f * 1.0f) + 0.5f) + s->nout;
    if (s->incap < remaining + pad + remaining) {
        s->incap = (s->incap >> 1) + s->incap + pad + remaining;
        s->in = (short *)realloc(s->in, (size_t)s->incap * 2);
    }
    memset(s->in + remaining, 0, (size_t)pad * 2);
    s->nin += pad;
    process(s);
    if (expected < s->nout) s->nout = expected;
    s->nin = 0;
    s->remain = 0;
}

int zb_sonic_available(const ZbSonic *s) { return s->nout; }

int zb_sonic_read(ZbSonic *s, short *dst, int max)
{
    int n = s->nout < max ? s->nout : max;
    if (!n) return 0;
    memcpy(dst, s->out, (size_t)n * 2);
    if (s->nout - n > 0) memmove(s->out, s->out + n, (size_t)(s->nout - n) * 2);
    s->nout -= n;
    return n;
}
