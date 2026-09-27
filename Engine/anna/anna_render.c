/* Anna waveform generation: chunking and decoder reads (FUN_081a334c / FUN_081a360b / FUN_081a768e),
 * join fades (FUN_081a7422 / FUN_081a7e3b), per-chunk rate change with WSOLA (FUN_081a0825 -> FUN_081a37c8,
 * FUN_081a39c8 and the core FUN_081ac3a9 / FUN_081ac575 / FUN_081ac44a / FUN_081ac3e0) and volume. */
#include "anna_internal.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WN = 240, WHS = 160, WK = 200, WL = 80, WB = WN + WK }; /* WSOLA at 16 kHz */

typedef struct {
    int pause, marker;
    long n, start;
    int join, cp, cn; /* joinable, contiguous with previous / next record */
    uint32_t wih_off;
    int wih_n;
} rrec;

typedef struct {
    int16_t *d;
    size_t n, cap;
} pcmbuf;

static int grow(pcmbuf *b, size_t add)
{
    if (b->n + add > b->cap) {
        size_t c = b->cap ? b->cap : 1 << 16;
        int16_t *p;
        while (c < b->n + add) c *= 2;
        p = realloc(b->d, c * sizeof *p);
        if (!p) return -1;
        b->d = p;
        b->cap = c;
    }
    return 0;
}

/* FUN_081a7e3b: w[i] = (float)cos((float)(i * (float)(pi / 199) - pi / 2)), pi as the engine's 3.1415926535 */
static float g_win[100];
static int g_win_ready;
static pthread_once_t g_win_once = PTHREAD_ONCE_INIT; /* ClassicVoices patch: made once, by one thread */
static void make_window(void)
{
    float step = (float)(3.1415926535 / 199);
    int i;
    for (i = 0; i < 100; i++) {
        float arg = (float)((double)(float)i * (double)step - 1.57079632675);
        g_win[i] = (float)cos((double)arg);
    }
    g_win_ready = 1;
}

static void fade(int16_t *x, long n, int fin, int fout)
{
    int i;
    if (n <= 200) return;
    if (fin)
        for (i = 0; i < 100; i++) x[i] = (int16_t)((double)x[i] * (double)g_win[i]);
    if (fout)
        for (i = 0; i < 100; i++) x[n - 100 + i] = (int16_t)((double)x[n - 100 + i] * (double)g_win[99 - i]);
}

/* ---- WSOLA ---- */

/* FUN_081a37c8: a new factor resets the stream (the carried samples are dropped) */
static void ws_set_factor(anna_wsola *w, double f)
{
    if (w->factor == f) return;
    w->started = 0;
    w->cc = 0;
    w->factor = f;
    w->ha = (int)(WHS * f);
}

/* FUN_081a382b: copy up to WB samples of (carry ++ in) from position pos into buf; returns the count */
static int ws_fill(const anna_wsola *w, const int16_t *in, int n, int pos, double *buf)
{
    int total = w->cc + n, i;
    if (pos >= total) return total - pos;
    for (i = 0; i < WB && pos + i < total; i++) {
        int p = pos + i;
        buf[i] = p < w->cc ? w->carry[p] : in[p - w->cc];
    }
    return i;
}

/* FUN_081ac575: one synthesis step on buf (WB samples); writes WHS samples */
static void ws_step(anna_wsola *w, double *buf, pcmbuf *out)
{
    int kk = WHS - w->ha + w->k, c, i;
    if (kk < 0 || kk > WK) { /* best splice point (FUN_081ac44a) */
        double best = -1.0;
        int bi = 0;
        for (c = 0; c <= WK; c++) {
            double num = 0.0, en = 0.0, val;
            for (i = 0; i < WL; i++) num += w->tail[i] * buf[c + i];
            if (num <= 0.0) continue;
            for (i = 0; i < WL; i++) en += buf[c + i] * buf[c + i];
            if (en <= 0.0) continue;
            val = num * num / en;
            if (val > best) {
                best = val;
                bi = c;
            }
        }
        kk = bi;
    }
    for (i = 0; i < WL; i++) buf[kk + i] = ((double)i * buf[kk + i] + (double)(WL - i) * w->tail[i]) / WL;
    for (i = 0; i < WHS; i++) out->d[out->n++] = (int16_t)buf[kk + i];
    for (i = 0; i < WL; i++) w->tail[i] = buf[kk + WHS + i];
    w->k = kk;
}

/* FUN_081a39c8: flag 1 = start of sentence, 2 = end of sentence */
static int ws_run(anna_wsola *w, const int16_t *in, int n, int flag, pcmbuf *out)
{
    double buf[WB];
    int a, i, pos = 0;
    size_t start_n = out->n;
    if (w->cc + n < 1) {
        w->cc += n;
        return 0;
    }
    if (grow(out, (size_t)(w->cc + n) + WB)) return -1;
    if (flag & 1) {
        w->started = 0;
        w->cc = 0;
    }
    if (!w->started) {
        a = ws_fill(w, in, n, 0, buf);
        if (a < WB) { /* too short to start: passed through unchanged */
            memcpy(out->d + out->n, in, (size_t)n * sizeof *in);
            out->n += (size_t)n;
            return 0;
        }
        for (i = 0; i < WHS; i++) out->d[out->n++] = (int16_t)buf[i]; /* FUN_081ac3a9 */
        for (i = 0; i < WL; i++) w->tail[i] = buf[WHS + i];
        w->k = 0;
        w->started = 1;
    }
    a = ws_fill(w, in, n, 0, buf);
    while (a >= WB) {
        if (grow(out, WHS)) return -1;
        ws_step(w, buf, out);
        pos += w->ha;
        a = ws_fill(w, in, n, pos, buf);
    }
    if (out->n == start_n) flag |= 2;
    if (!(flag & 2)) { /* keep the rest for the next chunk */
        int16_t keep[WB];
        for (i = 0; i < a; i++) keep[i] = (int16_t)buf[i];
        memcpy(w->carry, keep, sizeof(int16_t) * (size_t)(a > 0 ? a : 0));
        w->cc = a;
    } else { /* FUN_081ac3e0: cross-fade the tail into the rest (from the analysis position) and emit it */
        if (a > 0) {
            if (grow(out, (size_t)a)) return -1;
            for (i = 0; i < WL && i < a; i++) buf[i] = ((double)i * buf[i] + (double)(WL - i) * w->tail[i]) / WL;
            for (i = 0; i < a; i++) out->d[out->n++] = (int16_t)buf[i];
        }
        w->started = 0;
        w->cc = 0;
    }
    return 0;
}

/* ---- pitch change: TD-PSOLA-like (FUN_081a518d -> FUN_081a489a grouping, FUN_081a4ac0 synthesis) ---- */

static int iabs(int x) { return x < 0 ? -x : x; }
static int eclass(int x) { return x > 0 ? 1 : x < 0 ? -1 : 0; } /* FUN_081a3db8 */

/* copy n samples of in[0..len) starting at pos into dst, zero outside (FUN_081a3d3b) */
static void grab(int16_t *dst, const int16_t *in, long len, long pos, long n)
{
    long i;
    for (i = 0; i < n; i++) dst[i] = pos + i >= 0 && pos + i < len ? in[pos + i] : 0;
}

/* in: chunk PCM; wih: its epoch bytes; returns 0 and appends to out, or -1 */
static int psola(const anna_voice *v, const int16_t *in, long n, const signed char *wih, long nw, int pitch, double r, pcmbuf *out) /* ClassicVoices patch: r double */
{
    int *e = malloc(sizeof(int) * (size_t)(nw + 1)), *g = malloc(sizeof(int) * (size_t)(2 * nw + 2));
    float pf, inv, gg, win[1800];
    long ne = 0, ng = 0, u, i, pos = 0;
    int rc = -1;
    int16_t s1[1800], s2[1800];
    if (!e || !g || !v->hann) goto done;
    for (i = 0; i < nw; i++) { /* decode the epoch bytes: 127 / -128 continue into the next byte */
        int x = wih[i];
        if (x == 127) {
            x = 127;
            while (++i < nw) {
                x += (unsigned char)wih[i];
                if ((unsigned char)wih[i] != 0x7f) break;
            }
        } else if (x == -128) {
            x = -128;
            while (++i < nw) {
                x += wih[i];
                if (wih[i] != -128) break;
            }
        }
        e[ne++] = x;
    }
    if (pitch > 10) pitch = 10;
    if (pitch < -10) pitch = -10;
    if (r > 20) r = 20;
    if (r < -20) r = -20;
    pf = (float)pow(2.0, pitch / 24.0);
    inv = (float)(1.0 / pow(3.0, r / 10.0));
    gg = pf * inv;
    if (!(gg > 0.0f)) goto done;
    /* FUN_081a489a: group the epochs; each group start becomes one output period */
    for (u = 0; u < ne;) {
        float s = e[u] < 0 ? inv : gg;
        long used = 0, extra = 0;
        unsigned in_t = (unsigned)iabs(e[u]), out_t = in_t, thr = (unsigned)((double)out_t / (double)s);
        g[ng] = (int)u;
        for (;;) {
            if (eclass(e[u + used]) != eclass(e[u])) break;
            while (in_t <= thr) {
                used++;
                if (u + used >= ne) break;
                if (eclass(e[u + used]) != eclass(e[u])) break;
                in_t += (unsigned)iabs(e[u + used]);
            }
            if (u + used >= ne) break;
            if (eclass(e[u + used]) != eclass(e[u])) break;
            extra++;
            g[ng + extra] = (int)(u + used);
            out_t += (unsigned)iabs(e[u + used]);
            thr = (unsigned)((double)out_t / (double)s);
        }
        ng += 1 + extra;
        u += used;
    }
    /* FUN_081a4ac0 */
    for (i = 0; i + 1 < ng; i++) {
        int T, P, W, k;
        long adv = 0, src2, len2;
        float sc;
        if (pos > n || g[i] >= ne) goto done;
        T = e[g[i]];
        sc = T < 0 ? 1.0f : pf;
        P = (int)((double)iabs(T) / (double)sc + 0.5);
        W = sc < 1.0f ? iabs(T) : P;
        if (P > 0x707 || W > 0x708) goto done;
        grab(s1, in, n, pos, P);
        for (k = g[i]; k < g[i + 1]; k++) adv += iabs(e[k]);
        if (pos - P + adv >= 0) src2 = pos + adv - P, len2 = P;
        else src2 = pos, len2 = adv;
        memset(s2, 0, sizeof(int16_t) * (size_t)P);
        grab(s2, in, n, src2, len2 < P ? len2 : P);
        for (k = 0; k < 2 * W; k++) win[k] = (float)v->hann[(unsigned)k * 4999u / (unsigned)(2 * W - 1)];
        if (grow(out, (size_t)P)) goto done;
        for (k = 0; k < P; k++) {
            int16_t a = k < W ? (int16_t)((double)s1[k] * (double)win[W - 1 - k]) : 0; /* falling half */
            int16_t b = k < P - W ? 0 : (int16_t)((double)s2[k] * (double)win[k - P + W]); /* rising half */
            out->d[out->n++] = (int16_t)(a + b);
        }
        pos += adv;
    }
    if (ng > 0 && g[ng - 1] < ne) { /* the last group: falling half only */
        int T = e[g[ng - 1]], P, W, k;
        float sc = T < 1 ? 1.0f : pf;
        P = (int)((double)iabs(T) / (double)sc + 0.5);
        W = sc < 1.0f ? iabs(T) : P;
        if (P > 0x707 || W > 0x708) goto done;
        grab(s1, in, n, pos, P);
        for (k = 0; k < 2 * W; k++) win[k] = (float)v->hann[(unsigned)k * 4999u / (unsigned)(2 * W - 1)];
        if (grow(out, (size_t)P)) goto done;
        for (k = 0; k < P; k++) out->d[out->n++] = W - 1 - k < 0 ? 0 : (int16_t)((double)s1[k] * (double)win[W - 1 - k]);
        rc = 0;
    }
done:
    free(e);
    free(g);
    return rc;
}

/* FUN_081a06c9: volume 0..1 -> gain, -25 dB at 0.1 and a linear ramp below */
static float vol_gain(float x)
{
    const float c10 = (float)pow(10.0, -1.1249999981373549); /* ClassicVoices patch: was a lazily set static */
    if ((double)x >= 0.10000000149011612) return (float)pow(10.0, (1.0 - (double)x) * -25.0 / 20.0);
    return (float)((double)x / 0.10000000149011612 * (double)c10);
}

static int wgrow(signed char **w, long *cap, long need)
{
    if (need > *cap) {
        long c = *cap ? *cap : 4096;
        signed char *p;
        while (c < need) c *= 2;
        p = realloc(*w, (size_t)c);
        if (!p) return -1;
        *w = p;
        *cap = c;
    }
    return 0;
}

/* FUN_081a0786: rescale the item sample counts of a chunk to its new length */
static void rescale(long *cnt, int st, int i, long total)
{
    long old = 0, acc = 0;
    float fct;
    int j;
    for (j = st; j <= i; j++) old += cnt[j];
    fct = old ? (float)((double)total / (double)old) : 0.0f;
    for (j = st; j <= i; j++) {
        if (j < i) cnt[j] = (long)floor((double)(float)((double)cnt[j] * (double)fct));
        else cnt[j] = total - acc;
        acc += cnt[j];
    }
}

static int is_marker(const anna_unitspec *s) { return s->type == 0 && s->pause_ms < 0; }

int anna_render_sentence(anna_voice *v, const anna_unitspec *specs, int n, int sapi_rate, anna_pcm_cb cb, void *user)
{
    anna_render_opts o;
    memset(&o, 0, sizeof o);
    o.sapi_rate = sapi_rate;
    o.sapi_volume = 100;
    return anna_render_sentence_ex(v, specs, n, &o, cb, user);
}

int anna_render_sentence_ex(anna_voice *v, const anna_unitspec *specs0, int n, const anna_render_opts *opt, anna_pcm_cb cb, void *user)
{
    int sapi_rate = opt->sapi_rate;
    anna_unitspec *specs = malloc(sizeof(anna_unitspec) * (size_t)(n + 1));
    long long sent_pos = 0;
    long *units = malloc(sizeof(long) * (size_t)(n + 1));
    rrec *rec = malloc(sizeof(rrec) * (size_t)(n + 1));
    long *cnt = malloc(sizeof(long) * (size_t)(n + 1));
    pcmbuf s = {0}, o = {0};
    int i, rc = 0, st;
    long tot, nwih = 0, wcap = 0;
    signed char *wih = NULL;
    pthread_once(&g_win_once, make_window); /* ClassicVoices patch (was: if (!g_win_ready) make_window()) */
    if (specs) { /* the caller's pitch offset (screen reader pitch setting) is added to every item */
        memcpy(specs, specs0, sizeof(anna_unitspec) * (size_t)n);
        if (opt->pitch_offset)
            for (i = 0; i < n; i++) specs[i].pitch += opt->pitch_offset;
    }
    if (!specs || !units || !rec || !cnt || anna_select(v, specs, n, units)) {
        rc = -1;
        goto done;
    }
    for (i = 0; i < n; i++) { /* render records (end of FUN_081a7ed2) */
        memset(&rec[i], 0, sizeof rec[i]);
        if (units[i] < 0) {
            long ms = specs[i].pause_ms < 0 ? -specs[i].pause_ms : specs[i].pause_ms;
            rec[i].pause = 1;
            rec[i].marker = is_marker(&specs[i]);
            rec[i].n = ANNA_RATE * (ms > 65535 ? 65535 : ms) / 1000;
        } else {
            const anna_unit *u = &v->units[units[i]];
            rec[i].n = u->len;
            rec[i].start = u->start;
            rec[i].join = u->join;
            rec[i].wih_off = u->wih_off;
            rec[i].wih_n = u->wih_n;
        }
    }
    for (i = 0; i < n; i++) {
        const rrec *p = i > 0 ? &rec[i - 1] : NULL, *q = i + 1 < n ? &rec[i + 1] : NULL;
        rec[i].cp = p && !p->pause && !rec[i].pause && rec[i].join && p->start + p->n == rec[i].start;
        rec[i].cn = q && !q->pause && !rec[i].pause && q->join && rec[i].start + rec[i].n == q->start;
    }
    st = 0;
    tot = 0;
    for (i = 0; i < n && rc == 0; i++) {
        const anna_unitspec *sp = &specs[i], *nx = i + 1 < n ? &specs[i + 1] : NULL;
        int cut, a, flag, j, first, pmode;
        double r; /* ClassicVoices patch: was int (fractional rates) */
        if (opt->cancel && *opt->cancel) {
            rc = 1;
            break;
        }
        tot += rec[i].n;
        cut = !nx || sp->rate != nx->rate || sp->pitch != nx->pitch || is_marker(nx) || is_marker(sp);
        if (!cut) cut = sp->pitch == 0 ? (st == 0 && tot >= 2000) || (tot >= 5000 && !rec[i].cn) || tot >= 10000 : sp->brk > 2;
        if (!cut) continue;
        /* render the chunk st..i: each run is one decoder read (FUN_081a768e); with a pitch change the
           runs' pitch-epoch bytes are gathered too, and pauses get unvoiced 80-sample epochs */
        s.n = 0;
        pmode = 0;
        for (j = st; j <= i; j++)
            if (specs[j].pitch != 0) pmode = 1;
        nwih = 0;
        for (a = st; a <= i && rc == 0;) {
            if (rec[a].pause) {
                if (grow(&s, (size_t)rec[a].n)) rc = -1;
                else {
                    memset(s.d + s.n, 0, (size_t)rec[a].n * sizeof *s.d);
                    s.n += (size_t)rec[a].n;
                }
                if (pmode && rc == 0) {
                    long q = rec[a].n / 80, rem = rec[a].n % 80, z;
                    if (wgrow(&wih, &wcap, nwih + q + 1)) rc = -1;
                    else {
                        for (z = 0; z < q; z++) wih[nwih++] = (signed char)-80;
                        if (rem) wih[nwih++] = (signed char)rem;
                    }
                }
                a++;
            } else {
                int b = a;
                long len = rec[a].n;
                while (b + 1 <= i && rec[b].cn) len += rec[++b].n;
                if (grow(&s, (size_t)len) || anna_decoder_read(v->dec, rec[a].start, len, s.d + s.n)) rc = -1;
                else {
                    fade(s.d + s.n, len, !rec[a].cp, !rec[b].cn);
                    s.n += (size_t)len;
                }
                if (pmode && rc == 0) {
                    long nb = 0, z;
                    for (z = a; z <= b; z++) nb += rec[z].wih_n;
                    if (!v->wih || wgrow(&wih, &wcap, nwih + nb) || fseek((FILE *)v->wih, 16 + (long)rec[a].wih_off, SEEK_SET) ||
                        fread(wih + nwih, 1, (size_t)nb, (FILE *)v->wih) != (size_t)nb)
                        rc = -1;
                    else nwih += nb;
                }
                a = b + 1;
            }
        }
        if (rc) break;
        /* FUN_081a0825: sentence start/end flags, rate of the chunk's first item, WSOLA or pass-through */
        first = st;
        flag = 0;
        for (j = 0; j < first && is_marker(&specs[j]); j++) {
        }
        if (j == first) flag = 1;
        for (j = n - 1; j > i && is_marker(&specs[j]); j--) {
        }
        if (j == i) flag = 2;
        r = specs[first].rate > 10 ? 10 : specs[first].rate < -10 ? -10 : specs[first].rate;
        /* the engine clamps the SAPI rate to -10..10; screen readers may go beyond (extension, up to +18) */
        { /* ClassicVoices patch: sapi_rate + rate_frac, clamped the same way (whole rates: exactly as before) */
            double sr = (double)sapi_rate + opt->rate_frac, hi = opt->allow_fast ? 18 : 10;
            r += (sr > hi ? hi : sr < -10 ? -10 : sr) + v->default_rate;
        }
        if (r > 20) r = 20;
        if (r < -20) r = -20;
        ws_set_factor(&v->ws, pow(3.0, r / 10.0));
        if (is_marker(&specs[first])) r = 0;
        for (j = st; j <= i; j++) cnt[j] = rec[j].n;
        o.n = 0;
        if (s.n == 0) {
        } else if (pmode) {
            if (specs[first].pitch == 0 && r == 0) {
            } else if (psola(v, s.d, (long)s.n, wih, nwih, specs[first].pitch, r, &o)) rc = -1;
            else rescale(cnt, st, i, (long)o.n);
        } else if (r == 0) {
            if (grow(&o, s.n)) rc = -1;
            else {
                memcpy(o.d, s.d, s.n * sizeof *s.d);
                o.n = s.n;
            }
        } else {
            if (ws_run(&v->ws, s.d, (int)s.n, flag, &o)) rc = -1;
            else rescale(cnt, st, i, (long)o.n);
        }
        if (rc) break;
        { /* volume per item, times the SAPI volume */
            size_t at = 0;
            int sv = opt->sapi_volume < 0 ? 0 : opt->sapi_volume > 100 ? 100 : opt->sapi_volume;
            for (j = st; j <= i; j++) {
                if (opt->item_cb) opt->item_cb(opt->user, j, sent_pos + (long long)at); /* event positions */
                if (cnt[j] > 0 && (specs[j].volume != 100 || sv != 100) && at + (size_t)cnt[j] <= o.n) {
                    float g = (float)((double)vol_gain((float)(specs[j].volume / 100.0)) * (double)vol_gain((float)(sv / 100.0)));
                    long q;
                    for (q = 0; q < cnt[j]; q++) o.d[at + q] = (int16_t)((double)o.d[at + q] * (double)g);
                }
                at += cnt[j] > 0 ? (size_t)cnt[j] : 0;
            }
        }
        if (o.n) cb(o.d, o.n, user);
        sent_pos += (long long)o.n;
        st = i + 1;
        tot = 0;
    }
done:
    free(specs);
    free(units);
    free(rec);
    free(cnt);
    free(s.d);
    free(o.d);
    free(wih);
    return rc;
}
