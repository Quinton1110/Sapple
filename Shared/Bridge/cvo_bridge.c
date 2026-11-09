/* ClassicVoices OneCore + ProTALKER 97 bridge - see cvo_bridge.h. */
#include "cvo_bridge.h"
#include "zira_tts.h"
#include "protalker_tts.h"
#include "cv_resample.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cvo_voice {
    zira_tts *t;
    protalker_tts *pt;
    cv_resampler rs;   /* 16000 -> 22050 */
};

cvo_voice *cvo_voice_open(const char *dir, const char *voice, char *err, size_t errlen)
{
    cvo_voice *v;
    if (!voice) {
        if (err && errlen) snprintf(err, errlen, "unknown OneCore voice");
        return NULL;
    }
    if (protalker_tts_is_voice(voice)) {
        v = calloc(1, sizeof *v);
        if (!v) return NULL;
        v->pt = protalker_tts_open(dir, voice, err, errlen);
        if (!v->pt) {
            free(v);
            return NULL;
        }
        return v;
    }
    if (!zira_tts_voice_lcid(voice)) {
        if (err && errlen) snprintf(err, errlen, "unknown OneCore voice");
        return NULL;
    }
    if (!dir || !*dir) { /* the engine would fall back to a Windows path */
        if (err && errlen) snprintf(err, errlen, "no data folder");
        return NULL;
    }
    v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->t = zira_tts_open(dir, voice, err, errlen);
    if (!v->t) {
        free(v);
        return NULL;
    }
    if (cv_rs_init(&v->rs, ZIRA_SAMPLE_RATE)) {
        if (err && errlen) snprintf(err, errlen, "cannot set up the resampler");
        zira_tts_close(v->t);
        free(v);
        return NULL;
    }
    return v;
}

void cvo_voice_close(cvo_voice *v)
{
    if (!v) return;
    if (v->pt) protalker_tts_close(v->pt);
    if (v->t) zira_tts_close(v->t);
    cv_rs_free(&v->rs);
    free(v);
}

int cvo_voice_sample_rate(const cvo_voice *v)
{
    (void)v;
    return CV_RS_OUT_RATE;
}

int cvo_voice_set_emotion(cvo_voice *v, const char *emotion)
{
    if (!v) return -1;
    if (v->pt) return (!emotion || !*emotion) ? 0 : -1;
    if (zira_tts_set_emotion(v->t, emotion && *emotion ? emotion : NULL) == 0) return 0;
    zira_tts_set_emotion(v->t, NULL);
    return -1;
}

int cvo_pitch_for_semitones(double semitones)
{
    double p;
    if (!(semitones == semitones)) return 0;
    p = semitones * 2.0; /* the engine's <pitch middle> step: +-10 = +-5 semitones */
    if (p < -10) p = -10;
    if (p > 10) p = 10;
    return (int)lrint(p);
}

typedef struct {
    cvo_voice *v;
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

/* 22050 Hz from ProTALKER 97 -> trimmer -> caller */
static int on_protalker_pcm(const int16_t *pcm, size_t n, void *user)
{
    speak_ctx *x = user;
    if (x->stopped) return 1;
    if (n && cv_trimmer_feed(x->t, pcm, n)) x->stopped = 1;
    return x->stopped;
}

int cvo_voice_speak(cvo_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                    void *user)
{
    char *clean;
    speak_ctx x;
    zira_callbacks cb;
    int rc;
    if (!v || !utf8 || !fn) return -1;
    clean = cv_sanitize_unicode_alloc(utf8);
    if (!clean) return -1;
    if (!*clean) { /* nothing speakable: no audio, and never any stand-in text */
        free(clean);
        return 0;
    }
    if (!(sapi_rate == sapi_rate)) sapi_rate = 0;
    memset(&x, 0, sizeof x);
    x.v = v;
    x.t = cv_trimmer_new(trim, fn, user);
    if (!x.t) {
        free(clean);
        return -1;
    }
    if (v->pt) {
        rc = protalker_tts_speak(v->pt, clean,
                                 sapi_rate < -10 ? -10 : sapi_rate > 18 ? 18 : sapi_rate,
                                 semitones, on_protalker_pcm, &x);
        if (rc == 0 && cv_trimmer_finish(x.t)) rc = 1;
        if (x.failed) rc = -1;
        cv_trimmer_free(x.t);
        free(clean);
        return rc;
    }
    zira_tts_set_rate_f(v->t, sapi_rate < -10 ? -10 : sapi_rate > 18 ? 18 : sapi_rate);
    zira_tts_set_pitch(v->t, cvo_pitch_for_semitones(semitones));
    cv_rs_reset(&v->rs);
    cb.audio = on_engine_pcm;
    cb.event = NULL;
    cb.user = &x;
    rc = zira_tts_speak(v->t, clean, 0, &cb); /* plain text: SAPI XML in the text is spoken, never obeyed */
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
