/* ClassicVoices SAPI 4 bridge, native (Engine/sapi4) - see cv4n_bridge.h. The speak path is cv4_bridge.c's for the
 * Microsoft engine, statement for statement, so both give the same PCM. */
#include "cv4n_bridge.h"
#include "cv_resample.h"
#include "sapi4tts.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE 22050

/* ================================================================== engines kept for the process */

typedef struct parked {
    s4_engine *e;
    char mode[64];
    struct parked *next;
} parked;

static pthread_mutex_t g_park_lock = PTHREAD_MUTEX_INITIALIZER;
static parked *g_parked;
static int g_opened;
static int g_init_ok;

/* the library's data: <dir>/msttssyn.dll, loaded once per process (under g_park_lock); the first folder that works
 * serves every later open (the app and the extension each use their own bundle's single SAPI4Voices) */
static int init_locked(const char *dir, char *err, size_t errlen)
{
    char path[1100];
    if (g_init_ok) return 0;
    snprintf(path, sizeof path, "%s/msttssyn.dll", dir);
    if (s4_init(path) == 0) g_init_ok = 1;
    else if (err && errlen) snprintf(err, errlen, "cannot load %s", path);
    return g_init_ok ? 0 : -1;
}

static s4_engine *engine_take(const char *dir, const char *mode, char *err, size_t errlen)
{
    parked **pp, *p;
    s4_engine *e = NULL;
    pthread_mutex_lock(&g_park_lock);
    if (init_locked(dir, err, errlen) == 0) {
        for (pp = &g_parked; (p = *pp) != NULL; pp = &p->next)
            if (strcmp(p->mode, mode) == 0) {
                *pp = p->next;
                e = p->e;
                free(p);
                break;
            }
        if (!e) {
            e = s4_open(dir, mode, err, errlen);
            if (e) g_opened++;
        }
    }
    pthread_mutex_unlock(&g_park_lock);
    return e;
}

static void engine_park(s4_engine *e, const char *mode)
{
    parked *p = calloc(1, sizeof *p);
    if (!p) {           /* cannot park: close it (its inner buffers stay allocated, see the header) */
        s4_close(e);
        return;
    }
    p->e = e;
    snprintf(p->mode, sizeof p->mode, "%s", mode);
    pthread_mutex_lock(&g_park_lock);
    p->next = g_parked;
    g_parked = p;
    pthread_mutex_unlock(&g_park_lock);
}

int cv4n_discard_parked(void)
{
    parked *p;
    int n = 0;
    pthread_mutex_lock(&g_park_lock);
    p = g_parked;
    g_parked = NULL;
    pthread_mutex_unlock(&g_park_lock);
    while (p) {
        parked *next = p->next;
        s4_close(p->e);
        free(p);
        p = next;
        n++;
    }
    return n;
}

int cv4n_engines_opened(void)
{
    int n;
    pthread_mutex_lock(&g_park_lock);
    n = g_opened;
    pthread_mutex_unlock(&g_park_lock);
    return n;
}

/* ================================================================== voice */

struct cv4n_voice {
    s4_engine *s;
    char mode[64];
    s4_limits lim;
    int native_rate;
    int dead;           /* the engine failed: drop it instead of parking it */
    cv_resampler rs;    /* used when native_rate != 22050 */
};

cv4n_voice *cv4n_voice_open(const char *dir, const char *mode, char *err, size_t errlen)
{
    cv4n_voice *v;
    if (!dir || !mode || strlen(mode) >= sizeof v->mode) {
        if (err && errlen) snprintf(err, errlen, "no such voice mode");
        return NULL;
    }
    v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->s = engine_take(dir, mode, err, errlen);
    if (!v->s) {
        free(v);
        return NULL;
    }
    snprintf(v->mode, sizeof v->mode, "%s", mode);
    s4_get_limits(v->s, &v->lim);
    v->native_rate = s4_sample_rate(v->s);
    if (v->native_rate != OUT_RATE) {
        if (v->native_rate != 8000 || cv_rs_init(&v->rs, v->native_rate)) {
            if (err && errlen) snprintf(err, errlen, "unsupported engine sample rate %d", v->native_rate);
            engine_park(v->s, v->mode);
            free(v);
            return NULL;
        }
    }
    return v;
}

void cv4n_voice_close(cv4n_voice *v)
{
    if (!v) return;
    if (v->dead) s4_close(v->s);
    else engine_park(v->s, v->mode);
    cv_rs_free(&v->rs);
    free(v);
}

int cv4n_voice_sample_rate(const cv4n_voice *v)
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

unsigned cv4n_voice_wpm_for_rate(const cv4n_voice *v, double sapi_rate)
{
    if (!v) return 0;
    if (sapi_rate < -10) sapi_rate = -10;
    if (sapi_rate > 18) sapi_rate = 18;
    return clampu(v->lim.speed_default * pow(3.0, sapi_rate / 10.0), v->lim.speed_min, v->lim.speed_max);
}

unsigned cv4n_voice_pitch_for_semitones(const cv4n_voice *v, double semitones)
{
    if (!v) return 0;
    if (semitones < -12) semitones = -12;
    if (semitones > 12) semitones = 12;
    return clampu(v->lim.pitch_default * pow(2.0, semitones / 12.0), v->lim.pitch_min, v->lim.pitch_max);
}

typedef struct {
    cv4n_voice *v;
    cv_trimmer *t;
    int failed;
} speak_ctx;

/* native PCM from the engine -> (resampler) -> trimmer -> caller. Runs under the library's lock: nothing here may call
 * back into the library. */
static int on_engine_pcm(const int16_t *pcm, size_t n, void *user)
{
    speak_ctx *x = user;
    cv4n_voice *v = x->v;
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

int cv4n_voice_speak(cv4n_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                     void *user)
{
    char *clean, *text;
    speak_ctx x;
    int rc;
    if (!v || !utf8 || !fn || v->dead) return -1;
    clean = cv_sanitize_alloc(utf8);
    if (!clean) return -1;
    if (!*clean) { /* nothing speakable: no audio, and never any stand-in text */
        free(clean);
        return 0;
    }
    text = cv4_to_cp1252_alloc(clean);
    free(clean);
    if (!text) return -1;
    if (s4_set_speed(v->s, cv4n_voice_wpm_for_rate(v, sapi_rate)) ||
        s4_set_pitch(v->s, cv4n_voice_pitch_for_semitones(v, semitones))) {
        free(text);
        v->dead = 1;
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
    rc = s4_speak(v->s, text, 0, on_engine_pcm, &x);   /* plain text: never interpret \tags\ */
    if (rc < 0) v->dead = 1;
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
