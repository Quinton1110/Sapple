/* ClassicVoices TruVoice bridge (OpenTV) - see cvt_bridge.h. */
#include "cvt_bridge.h"
#include "cv_resample.h"
#include "tvtts.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE 22050
#define TV_RATE 11025

/* The engine's ten voices, in its own order (OpenTV voice index = position here). The names are the SAPI 4 mode names
 * tv_enua.dll enumerated, without ", American English (TruVoice)"; Voices.swift keeps the full ones. */
static const char *const MODES[10] = {
    "Adult Male #1", "Adult Male #2", "Adult Male #3", "Adult Male #4", "Adult Male #5",
    "Adult Male #6", "Adult Male #7", "Adult Male #8", "Adult Female #1", "Adult Female #2",
};

/* tv_enua.dll's limits (read from the engine by the old path, and tetyys.com's VoiceLimitations agree): speed 50..250
 * words per minute, pitch 50..400. The defaults are the same in both builds (tvtts_voice_rate / _pitch). */
#define SPEED_MIN 50
#define SPEED_MAX 250
#define PITCH_MIN 50
#define PITCH_MAX 400

/* SAPI 4 speed -> OpenTV speed. Both builds pick one of the same 26 rate rows ((wpm - 46) >> 3, 46..253 wpm), but
 * tv_enua.dll (L&H, 6.0.0.10, 1998) maps a SAPI speed onto them differently from CGRM_EN.DLL (Centigram, 1997), which
 * OpenTV reproduces: identical from 94 to 157 wpm (VoiceOver's default rate is 150), slower than the same number
 * elsewhere, with steps of its own in between. Measured 2026-09-26 (`truvoice_ab ratefit`): for every SAPI speed
 * 50..250 the row whose total duration over four sentences is closest to tv_enua.dll's. Within 4% from 86 wpm up
 * except 230..245 (+4..6%); below 80 wpm tv_enua.dll speaks slower than any row (at 50: 35% slower) and this stays at
 * the slowest row. Pairs: from this SAPI speed on, this row. */
static const struct { unsigned char from, row; } RATE_ROWS[] = {
    {50, 0}, {86, 4}, {94, 6}, {102, 7}, {110, 8}, {118, 9}, {126, 10}, {134, 11}, {142, 12}, {150, 13},
    {158, 14}, {182, 15}, {190, 16}, {198, 17}, {214, 18}, {246, 19},
};

unsigned cvt_engine_wpm(unsigned sapi_wpm)
{
    size_t i, row = 0;
    for (i = 0; i < sizeof RATE_ROWS / sizeof *RATE_ROWS; i++)
        if (sapi_wpm >= RATE_ROWS[i].from) row = RATE_ROWS[i].row;
    return 46u + 8u * (unsigned)row;
}

/* One lock for every OpenTV call that speaks or touches its process globals, and the Stage 2 state a fresh engine
 * image starts with (each voice gets a copy, like its own DLL image in the old path). */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void *g_pristine;

static void init_once(void)
{
    /* No OpenTV extensions: the 1997 engine exactly (they only act above 253 wpm, which we never ask for). */
    tvtts_set_extensions(0);
    g_pristine = malloc(tvtts_cv_s2_size());
    if (g_pristine) tvtts_cv_s2_save(g_pristine);
}

struct cvt_voice {
    tvtts_synth *s;
    int index;
    unsigned speed_default, pitch_default;
    void *s2;           /* this voice's Stage 2 state between its utterances */
    cv_resampler rs;
    /* TruVoice sits on a DC offset (about -150..-300) with a faint noise floor from its first sound to the end of the
     * call: a one-pole DC blocker (15 Hz) removes it so the trim works - exactly as cv4_bridge did for tv_enua.dll. */
    double dc_x1, dc_y1, dc_r;
    int16_t *dc_buf;
    size_t dc_cap;
};

static int mode_index(const char *mode)
{
    int i;
    if (!mode) return -1;
    for (i = 0; i < 10; i++) {
        size_t n = strlen(MODES[i]);
        if (!strncmp(mode, MODES[i], n) && (mode[n] == 0 || !strcmp(mode + n, ", American English (TruVoice)")))
            return i;
    }
    return -1;
}

cvt_voice *cvt_voice_open(const char *mode, char *err, size_t errlen)
{
    int idx = mode_index(mode);
    cvt_voice *v;
    pthread_once(&g_once, init_once);
    if (idx < 0) {
        if (err && errlen) snprintf(err, errlen, "unknown TruVoice mode");
        return NULL;
    }
    if (!g_pristine) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
        return NULL;
    }
    v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->index = idx;
    v->s2 = malloc(tvtts_cv_s2_size());
    pthread_mutex_lock(&g_lock);
    v->s = tvtts_create(TV_RATE);
    pthread_mutex_unlock(&g_lock);
    if (!v->s2 || !v->s || cv_rs_init(&v->rs, TV_RATE)) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
        cvt_voice_close(v);
        return NULL;
    }
    memcpy(v->s2, g_pristine, tvtts_cv_s2_size());
    tvtts_set_voice(v->s, idx);
    v->speed_default = (unsigned)tvtts_voice_rate(idx);
    v->pitch_default = (unsigned)tvtts_voice_pitch(idx);
    v->dc_r = 1.0 - 2.0 * M_PI * 15.0 / TV_RATE;
    return v;
}

void cvt_voice_close(cvt_voice *v)
{
    if (!v) return;
    if (v->s) {
        pthread_mutex_lock(&g_lock);
        tvtts_destroy(v->s);
        pthread_mutex_unlock(&g_lock);
    }
    cv_rs_free(&v->rs);
    free(v->s2);
    free(v->dc_buf);
    free(v);
}

int cvt_voice_sample_rate(const cvt_voice *v)
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

unsigned cvt_voice_wpm_for_rate(const cvt_voice *v, double sapi_rate)
{
    if (!v) return 0;
    if (sapi_rate < -10) sapi_rate = -10;
    if (sapi_rate > 18) sapi_rate = 18;
    return clampu(v->speed_default * pow(3.0, sapi_rate / 10.0), SPEED_MIN, SPEED_MAX);
}

unsigned cvt_voice_pitch_for_semitones(const cvt_voice *v, double semitones)
{
    if (!v) return 0;
    if (semitones < -12) semitones = -12;
    if (semitones > 12) semitones = 12;
    return clampu(v->pitch_default * pow(2.0, semitones / 12.0), PITCH_MIN, PITCH_MAX);
}

typedef struct {
    cvt_voice *v;
    cv_trimmer *t;
    int failed, stopped;
} speak_ctx;

/* 11025 Hz PCM from the engine -> DC blocker -> resampler -> trimmer -> caller */
static int on_event(const tvtts_event *ev, void *user)
{
    speak_ctx *x = user;
    cvt_voice *v = x->v;
    const int16_t *pcm;
    size_t i, n, no;
    if (ev->type != TVTTS_AUDIO || !ev->count) return 0;
    n = ev->count;
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
        double y = (double)ev->samples[i] - v->dc_x1 + v->dc_r * v->dc_y1;
        long q = lrint(y);
        v->dc_x1 = ev->samples[i];
        v->dc_y1 = y;
        v->dc_buf[i] = (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
    }
    pcm = v->dc_buf;
    if (cv_rs_feed(&v->rs, pcm, n)) {
        x->failed = 1;
        return 1;
    }
    no = cv_rs_run(&v->rs, 0);
    if (no && cv_trimmer_feed(x->t, v->rs.out, no)) {
        x->stopped = 1;
        return 1;
    }
    return 0;
}

int cvt_voice_speak(cvt_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
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
    memset(&x, 0, sizeof x);
    x.v = v;
    x.t = cv_trimmer_new(trim, fn, user);
    if (!x.t) {
        free(text);
        return -1;
    }
    cv_rs_reset(&v->rs);
    v->dc_x1 = v->dc_y1 = 0;   /* every call starts from the engine's exact-zero lead-in */
    tvtts_set_rate(v->s, (int)cvt_engine_wpm(cvt_voice_wpm_for_rate(v, sapi_rate)));
    tvtts_set_pitch(v->s, (int)cvt_voice_pitch_for_semitones(v, semitones));
    pthread_mutex_lock(&g_lock);
    tvtts_cv_s2_load(v->s2);
    /* plain Windows-1252 bytes: the engine's ESC [ ... escapes are the only markup it reads, and the sanitizer never
     * lets an ESC (a control character) through */
    rc = tvtts_speak_bytes(v->s, text, (uint32_t)strlen(text), on_event, &x);
    tvtts_cv_s2_save(v->s2);
    pthread_mutex_unlock(&g_lock);
    if (rc < 0) rc = -1;
    else if (rc == 1) rc = 1;
    if (rc == 0) {
        size_t no = cv_rs_run(&v->rs, 1);
        if (no && cv_trimmer_feed(x.t, v->rs.out, no)) rc = 1;
    }
    if (rc == 0 && cv_trimmer_finish(x.t)) rc = 1;
    if (rc == 1 && x.failed) rc = -1;
    cv_trimmer_free(x.t);
    free(text);
    return rc;
}
