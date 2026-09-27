/* ClassicVoices Anna bridge - see cva_bridge.h. */
#include "cva_bridge.h"
#include "anna_tts.h"
#include "cv_resample.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cva_voice {
    anna_tts *t;
    cv_resampler rs;   /* 16000 -> 22050 */
};

cva_voice *cva_voice_open(const char *dir, char *err, size_t errlen)
{
    cva_voice *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->t = anna_tts_open(dir, err, errlen);
    if (!v->t) {
        free(v);
        return NULL;
    }
    if (cv_rs_init(&v->rs, ANNA_SAMPLE_RATE)) {
        if (err && errlen) snprintf(err, errlen, "cannot set up the resampler");
        anna_tts_close(v->t);
        free(v);
        return NULL;
    }
    return v;
}

void cva_voice_close(cva_voice *v)
{
    if (!v) return;
    anna_tts_close(v->t);
    cv_rs_free(&v->rs);
    free(v);
}

int cva_voice_sample_rate(const cva_voice *v)
{
    (void)v;
    return CV_RS_OUT_RATE;
}

int cva_pitch_for_semitones(double semitones)
{
    double p;
    if (!(semitones == semitones)) return 0;
    p = semitones * 2.0; /* the engine's pitch step is half a semitone: 2^(step/24) */
    if (p < -10) p = -10;
    if (p > 10) p = 10;
    return (int)lrint(p);
}

typedef struct {
    cva_voice *v;
    cv_trimmer *t;
    int failed, stopped;
} speak_ctx;

/* 16 kHz from the engine -> resampler -> trimmer -> caller; nonzero stops the engine within one chunk */
static int on_engine_pcm(const int16_t *pcm, size_t n, void *user)
{
    speak_ctx *x = user;
    size_t no;
    if (x->stopped) return 1;
    if (cv_rs_feed(&x->v->rs, pcm, n)) {
        x->failed = x->stopped = 1;
        return 1;
    }
    no = cv_rs_run(&x->v->rs, 0);
    if (no && cv_trimmer_feed(x->t, x->v->rs.out, no)) x->stopped = 1;
    return x->stopped;
}

int cva_voice_speak(cva_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                    void *user)
{
    char *clean;
    speak_ctx x;
    anna_callbacks cb;
    int rc;
    if (!v || !utf8 || !fn) return -1;
    clean = cv_sanitize_alloc(utf8);
    if (!clean) return -1;
    if (!*clean) { /* nothing speakable: no audio, and never any stand-in text */
        free(clean);
        return 0;
    }
    if (!(sapi_rate == sapi_rate)) sapi_rate = 0;
    anna_tts_set_rate_f(v->t, sapi_rate < -10 ? -10 : sapi_rate > 18 ? 18 : sapi_rate);
    anna_tts_set_pitch(v->t, cva_pitch_for_semitones(semitones));
    memset(&x, 0, sizeof x);
    x.v = v;
    x.t = cv_trimmer_new(trim, fn, user);
    if (!x.t) {
        free(clean);
        return -1;
    }
    cv_rs_reset(&v->rs);
    cb.audio = on_engine_pcm;
    cb.event = NULL;
    cb.user = &x;
    rc = anna_tts_speak(v->t, clean, 0, &cb); /* plain text: SAPI XML in the text is spoken, never obeyed */
    if (rc == 0) {
        size_t no = cv_rs_run(&v->rs, 1);
        if (x.failed) rc = -1;
        else if (no && cv_trimmer_feed(x.t, v->rs.out, no)) rc = 1;
    }
    if (rc == 0 && cv_trimmer_finish(x.t)) rc = 1;
    if (x.failed) rc = -1;
    cv_trimmer_free(x.t);
    free(clean);
    return rc;
}
