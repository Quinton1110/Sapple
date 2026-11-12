/* ClassicVoices IBM ProTALKER 97 Japanese bridge - see cvp_bridge.h. */
#include "cvp_bridge.h"
#include "protalker_tts.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK g_lock = SRWLOCK_INIT;
static void pt_lock(void) { AcquireSRWLockExclusive(&g_lock); }
static void pt_unlock(void) { ReleaseSRWLockExclusive(&g_lock); }
#else
#include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void pt_lock(void) { pthread_mutex_lock(&g_lock); }
static void pt_unlock(void) { pthread_mutex_unlock(&g_lock); }
#endif

struct cvp_voice {
    protalker_tts *pt;
};

int cvp_voice_has_data(const char *dir)
{
    return protalker_tts_has_data(dir);
}

double cvp_speed_factor(double sapi_rate)
{
    /* Measured speech rate relative to speed=6 across Tts_mgr.dll's speed steps 4..16. */
    static const double factor[13] = {
        0.80, 0.89, 1.00, 1.15, 1.19, 1.35, 1.50, 1.69, 1.91, 2.36, 2.90, 3.90, 5.95
    };
    int speed;
    if (!(sapi_rate == sapi_rate)) sapi_rate = 0.0;
    if (sapi_rate < -10.0) sapi_rate = -10.0;
    if (sapi_rate > 18.0) sapi_rate = 18.0;
    if (sapi_rate < 0.0) {
        speed = (int)lrint(6.0 + (sapi_rate / 10.0) * (6.0 - 4.0));
    } else {
        speed = (int)lrint(6.0 + (sapi_rate / 18.0) * (16.0 - 6.0));
    }
    if (speed < 4) speed = 4;
    if (speed > 16) speed = 16;
    return factor[speed - 4];
}

cvp_voice *cvp_voice_open(const char *dir, const char *voice, char *err, size_t errlen)
{
    cvp_voice *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    pt_lock();
    v->pt = protalker_tts_open(dir, voice, err, errlen);
    pt_unlock();
    if (!v->pt) {
        free(v);
        return NULL;
    }
    return v;
}

void cvp_voice_close(cvp_voice *v)
{
    if (!v) return;
    pt_lock();
    protalker_tts_close(v->pt);
    pt_unlock();
    free(v);
}

int cvp_voice_sample_rate(const cvp_voice *v)
{
    (void)v;
    return PROTALKER_SAMPLE_RATE;
}

static int on_engine_pcm(const int16_t *pcm, size_t n, void *user)
{
    cv_trimmer *t = user;
    return n ? cv_trimmer_feed(t, pcm, n) : 0;
}

int cvp_voice_speak(cvp_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                    void *user)
{
    char *clean;
    const char *p;
    cv_trimmer *t;
    int rc;
    if (!v || !v->pt || !utf8 || !fn) return -1;
    clean = cv_sanitize_unicode_alloc(utf8);
    if (!clean) return -1;
    for (p = clean; *p == ' '; p++) {}
    if (!*p) {
        free(clean);
        return 0;
    }
    t = cv_trimmer_new(trim, fn, user);
    if (!t) {
        free(clean);
        return -1;
    }
    pt_lock();
    rc = protalker_tts_speak(v->pt, clean, sapi_rate, semitones, on_engine_pcm, t);
    pt_unlock();
    if (rc == 0 && cv_trimmer_finish(t)) rc = 1;
    cv_trimmer_free(t);
    free(clean);
    return rc;
}
