/* ClassicVoices SAPI 4 bridge - see cv4_bridge.h. */
#include "cv4_bridge.h"
#include "cv_resample.h"
#include "sapi4_tts.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE 22050

/* 8000 / 11025 -> 22050 Hz: the shared streaming resampler (cv_resample.h). */

/* ================================================================== voice */

struct cv4_voice {
    s4_engine *s;
    s4_limits lim;
    int native_rate;
    cv_resampler rs;    /* used when native_rate != 22050 */
    /* L&H TruVoice sits on a DC offset (about -150..-300) with a faint noise floor, from the first sound
     * to the end of the call: the trimmer would keep all of it as "sound". A one-pole DC blocker (15 Hz)
     * removes the offset - inaudible, even a 50 Hz fundamental loses 0.4 dB - so the trim works. */
    int dc_block;
    double dc_x1, dc_y1, dc_r;
    int16_t *dc_buf;
    size_t dc_cap;
};

cv4_voice *cv4_voice_open(const char *dir, const char *mode, char *err, size_t errlen)
{
    cv4_voice *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->s = s4_open(dir, mode, err, errlen);
    if (!v->s) {
        free(v);
        return NULL;
    }
    s4_get_limits(v->s, &v->lim);
    v->native_rate = s4_sample_rate(v->s);
    v->dc_block = s4_engine_kind(v->s) == S4_ENGINE_TRUVOICE;
    v->dc_r = 1.0 - 2.0 * M_PI * 15.0 / (v->native_rate > 0 ? v->native_rate : OUT_RATE);
    if (v->native_rate != OUT_RATE) {
        if ((v->native_rate != 8000 && v->native_rate != 11025) || cv_rs_init(&v->rs, v->native_rate)) {
            if (err && errlen) snprintf(err, errlen, "unsupported engine sample rate %d", v->native_rate);
            s4_close(v->s);
            free(v);
            return NULL;
        }
    }
    return v;
}

void cv4_voice_close(cv4_voice *v)
{
    if (!v) return;
    s4_close(v->s);
    cv_rs_free(&v->rs);
    free(v->dc_buf);
    free(v);
}

int cv4_voice_sample_rate(const cv4_voice *v)
{
    (void)v;
    return OUT_RATE;
}

static unsigned clampu(double x, unsigned lo, unsigned hi)
{
    if (!(x == x)) return lo;
    if (x < lo) return lo;
    if (x > hi) return hi;
    return (unsigned)lrint(x);
}

unsigned cv4_voice_wpm_for_rate(const cv4_voice *v, double sapi_rate)
{
    if (!v) return 0;
    if (sapi_rate < -10) sapi_rate = -10;
    if (sapi_rate > 18) sapi_rate = 18;
    return clampu(v->lim.speed_default * pow(3.0, sapi_rate / 10.0), v->lim.speed_min, v->lim.speed_max);
}

unsigned cv4_voice_pitch_for_semitones(const cv4_voice *v, double semitones)
{
    if (!v) return 0;
    if (semitones < -12) semitones = -12;
    if (semitones > 12) semitones = 12;
    return clampu(v->lim.pitch_default * pow(2.0, semitones / 12.0), v->lim.pitch_min, v->lim.pitch_max);
}

typedef struct {
    cv4_voice *v;
    cv_trimmer *t;
    int failed;
} speak_ctx;

/* native PCM from the engine -> (DC blocker) -> (resampler) -> trimmer -> caller */
static int on_engine_pcm(const int16_t *pcm, size_t n, void *user)
{
    speak_ctx *x = user;
    cv4_voice *v = x->v;
    if (v->dc_block) {
        size_t i;
        if (n > v->dc_cap) {
            int16_t *nb = realloc(v->dc_buf, n * sizeof *nb);
            if (!nb) {
                x->failed = 1;
                return 1;
            }
            v->dc_buf = nb;
            v->dc_cap = n;
        }
        for (i = 0; i < n; i++) {
            double y = (double)pcm[i] - v->dc_x1 + v->dc_r * v->dc_y1;
            long q = lrint(y);
            v->dc_x1 = pcm[i];
            v->dc_y1 = y;
            v->dc_buf[i] = (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
        }
        pcm = v->dc_buf;
    }
    if (v->native_rate == OUT_RATE) return cv_trimmer_feed(x->t, pcm, n);
    if (cv_rs_feed(&v->rs, pcm, n)) {
        x->failed = 1;
        return 1;
    }
    {
        size_t no = cv_rs_run(&v->rs, 0);
        return no ? cv_trimmer_feed(x->t, v->rs.out, no) : 0;
    }
}

int cv4_voice_speak(cv4_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                    void *user)
{
    char *clean, *text;
    speak_ctx x;
    int rc;
    if (!v || !utf8 || !fn) return -1;
    clean = cv_sanitize_alloc(utf8);
    if (!clean) return -1;
    if (!*clean) { /* nothing speakable: no audio, and never any stand-in text */
        free(clean);
        return 0;
    }
    text = cv4_to_cp1252_alloc(clean);
    free(clean);
    if (!text) return -1;
    if (s4_set_speed(v->s, cv4_voice_wpm_for_rate(v, sapi_rate)) ||
        s4_set_pitch(v->s, cv4_voice_pitch_for_semitones(v, semitones))) {
        free(text);
        return -1;
    }
    memset(&x, 0, sizeof x);
    x.v = v;
    x.t = cv_trimmer_new(trim, fn, user);
    if (!x.t) {
        free(text);
        return -1;
    }
    if (v->native_rate != OUT_RATE) cv_rs_reset(&v->rs);
    v->dc_x1 = v->dc_y1 = 0;   /* every call starts from the engine's exact-zero lead-in */
    rc = s4_speak(v->s, text, 0, on_engine_pcm, &x);   /* plain text: never interpret \tags\ */
    if (rc == 0 && v->native_rate != OUT_RATE) {
        size_t no = cv_rs_run(&v->rs, 1);
        if (no && cv_trimmer_feed(x.t, v->rs.out, no)) rc = 1;
    }
    if (rc == 0 && cv_trimmer_finish(x.t)) rc = 1;
    if (rc == 1 && x.failed) rc = -1;
    cv_trimmer_free(x.t);
    free(text);
    return rc;
}
