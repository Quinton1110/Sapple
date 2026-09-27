/* ClassicVoices neural bridge (Jenny, Aria, Guy; Sonia, Ryan) - see cvn_bridge.h.
 *
 * The engine is Microsoft's Speech SDK (1.33) with its embedded (offline) TTS extension and ONNX runtime, as three
 * dylibs that are dlopen'ed at run time (the core one loads the other two from its own folder). Only its C API is used,
 * through function pointers; the few types and ids it needs are declared here (from the SDK's C headers:
 * speechapi_c_common.h, _property_bag.h, _audio_stream.h), so no SDK header is vendored.
 *
 * Rate: SSML <prosody rate="x">. The engine applies it as a factor on the voice's own RateAdjustment (its INI): measured
 * 2026-09-27, prosody rate 1.37 / 2.0 / 3.0 renders BYTE-IDENTICAL to the INI's RateAdjustment set to 1.37 / 2.0 / 3.0 x the
 * default (Jenny 100, Sonia 95), so nothing is time-stretched - the engine generates at that speed, pauses included.
 * Prosody stops at 1/3x and 3x. The old NeuralVoice app rewrote the INI instead (up to 3.5x), which needs a new engine
 * configuration per rate, and the SDK never gives a configuration's memory back: +3.4 MB per rebuild, measured (a voice
 * switch costs the same, which is why every voice keeps its own configuration and synthesizer here, built once).
 *
 * The voices' files are shared where they are byte-identical (checked by `make neural-data`): the en-US voices use
 * OneCoreVoice's MSTTSLocEnUS.dat and enUS.*.dat, the en-GB voices five of its EnGB/enGB domain files; the engine opens
 * every file by name in one folder, so each voice gets a folder of symbolic links in work_dir. (Without the domain files
 * the engine refuses the voice: EMBEDDED_TTS_ERROR_WRONG_DECRYPTION_KEY - measured.) */
#include "cvn_bridge.h"
#include "cv_resample.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- the SDK's C API (speechapi_c_*.h) ---- */
typedef uintptr_t SPXHR;          /* 0 = success */
typedef void *SPXH;               /* every handle */
#define SPX_INVALID ((SPXH)(intptr_t)-1)
#define PROP_SYNTH_OFFLINE_VOICE 3113 /* PropertyId SpeechServiceConnection_SynthOfflineVoice */
#define PROP_SYNTH_MODEL_KEY 3114     /* PropertyId SpeechServiceConnection_SynthModelKey */
#define FMT_RAW_24K_16BIT_MONO 16     /* SpeechSynthesisOutputFormat Raw24Khz16BitMonoPcm */
#define STREAM_CANCELED 4             /* Stream_Status StreamStatus_Canceled */
#define ENGINE_RATE 24000

typedef SPXHR (*fn_h)(SPXH *);
typedef SPXHR (*fn_hs)(SPXH, const char *);
typedef SPXHR (*fn_hh)(SPXH, SPXH *);
typedef SPXHR (*fn_rel)(SPXH);
typedef SPXHR (*fn_pb_set)(SPXH, int, const char *, const char *);
typedef SPXHR (*fn_fmt)(SPXH, int);
typedef SPXHR (*fn_synth_create)(SPXH *, SPXH, SPXH);
typedef SPXHR (*fn_start)(SPXH, const char *, uint32_t, SPXH *);
typedef SPXHR (*fn_ds_create)(SPXH *, SPXH);
typedef SPXHR (*fn_ds_read)(SPXH, uint8_t *, uint32_t, uint32_t *);
typedef SPXHR (*fn_ds_status)(SPXH, int *);

/* ---- the voices ---- */
typedef struct {
    const char *name, *locale, *ini, *engine_name;
} nv_def;

static const nv_def VOICES[] = {
    {"Jenny", "en-US", "1033.INI", "Microsoft Jenny (Natural) - English (United States)"},
    {"Aria", "en-US", "1033.INI", "Microsoft Aria (Natural) - English (United States)"},
    {"Guy", "en-US", "1033.INI", "Microsoft Guy (Natural) - English (United States)"},
    {"Sonia", "en-GB", "2057.INI", "Microsoft Sonia (Natural) - English (United Kingdom)"},
    {"Ryan", "en-GB", "2057.INI", "Microsoft Ryan (Natural) - English (United Kingdom)"},
};
#define NVOICES ((int)(sizeof VOICES / sizeof VOICES[0]))

/* name in the voice's folder -> file in OneCoreVoice (identical bytes, names differ in case) */
typedef struct {
    const char *link, *target;
} nv_link;
static const nv_link ONECORE_EN_US[] = {
    {"MSTTSLocEnUS.dat", "MSTTSLocEnUS.dat"},   {"EnUS.address.dat", "enUS.Address.dat"},
    {"EnUS.companyname.dat", "enUS.CompanyName.dat"}, {"EnUS.computer.dat", "enUS.Computer.dat"},
    {"EnUS.media.dat", "enUS.Media.dat"},       {"EnUS.message.dat", "enUS.Message.dat"},
    {"EnUS.name.dat", "enUS.Name.dat"},         {NULL, NULL}};
static const nv_link ONECORE_EN_GB[] = {
    {"EnGB.address.dat", "EnGB.Address.dat"},   {"EnGB.cityname.dat", "EnGB.CityName.dat"},
    {"EnGB.companyname.dat", "EnGB.CompanyName.dat"}, {"EnGB.computer.dat", "enGB.Computer.dat"},
    {"EnGB.message.dat", "enGB.Message.dat"},   {NULL, NULL}};

/* ---- process-wide state: one engine ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    void *dl;
    fn_h cfg_create;
    fn_hs cfg_add_path;
    fn_hh cfg_get_bag;
    fn_rel cfg_release, bag_release, synth_release, result_release, ds_release, stop;
    fn_pb_set bag_set;
    fn_fmt cfg_set_format;
    fn_synth_create synth_create;
    fn_start start_ssml;
    fn_ds_create ds_create;
    fn_ds_read ds_read;
    fn_ds_status ds_status;

    SPXH config[NVOICES], synth[NVOICES]; /* per voice, built on its first utterance; NULL = not built */
    int prepared[NVOICES];                /* work folder built in this process */
    char work[NVOICES][PATH_MAX];         /* the voice's work folder */
    char key[1024];
} G;

struct cvn_voice {
    int index;
    cv_resampler rs; /* 24000 -> 22050 */
};

static void seterr(char *err, size_t errlen, const char *fmt, const char *a)
{
    if (err && errlen) snprintf(err, errlen, fmt, a ? a : "");
}

static int voice_index(const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < NVOICES; i++)
        if (!strcmp(VOICES[i].name, name)) return i;
    return -1;
}

double cvn_speed_factor(double sapi_rate)
{
    if (!(sapi_rate == sapi_rate)) sapi_rate = 0;
    if (sapi_rate < -10) sapi_rate = -10;
    if (sapi_rate > 10) sapi_rate = 10; /* 3x: the engine's prosody limit */
    return pow(3.0, sapi_rate / 10.0);
}

/* ---- loading the SDK (under g_lock) ---- */
#define SYM(field, type, name)                                                                                           \
    do {                                                                                                                 \
        G.field = (type)dlsym(dl, name);                                                                                 \
        if (!G.field) {                                                                                                  \
            seterr(err, errlen, "the speech engine lacks %s", name);                                                     \
            return -1;                                                                                                   \
        }                                                                                                                \
    } while (0)

static int load_sdk(const char *sdk_dir, char *err, size_t errlen)
{
    char path[PATH_MAX];
    void *dl;
    if (G.dl) return 0;
    snprintf(path, sizeof path, "%s/libMicrosoft.CognitiveServices.Speech.core.dylib", sdk_dir ? sdk_dir : "");
    dl = dlopen(path, RTLD_NOW | RTLD_GLOBAL); /* the extension dylibs find the core's symbols through it */
    if (!dl) {
        seterr(err, errlen, "cannot load the speech engine: %s", dlerror());
        return -1;
    }
    SYM(cfg_create, fn_h, "embedded_speech_config_create");
    SYM(cfg_add_path, fn_hs, "embedded_speech_config_add_path");
    SYM(cfg_get_bag, fn_hh, "speech_config_get_property_bag");
    SYM(cfg_release, fn_rel, "speech_config_release");
    SYM(cfg_set_format, fn_fmt, "speech_config_set_audio_output_format");
    SYM(bag_set, fn_pb_set, "property_bag_set_string");
    SYM(bag_release, fn_rel, "property_bag_release");
    SYM(synth_create, fn_synth_create, "synthesizer_create_speech_synthesizer_from_config");
    SYM(synth_release, fn_rel, "synthesizer_handle_release");
    SYM(start_ssml, fn_start, "synthesizer_start_speaking_ssml");
    SYM(stop, fn_rel, "synthesizer_stop_speaking");
    SYM(result_release, fn_rel, "synthesizer_result_handle_release");
    SYM(ds_create, fn_ds_create, "audio_data_stream_create_from_result");
    SYM(ds_read, fn_ds_read, "audio_data_stream_read");
    SYM(ds_status, fn_ds_status, "audio_data_stream_get_status");
    SYM(ds_release, fn_rel, "audio_data_stream_release");
    G.dl = dl; /* never dlclose'd: the SDK's threads may outlive a synthesizer */
    return 0;
}

/* ---- the voice's work folder (under g_lock) ---- */
static char *read_file(const char *path, size_t max)
{
    FILE *f = fopen(path, "rb");
    char *b;
    size_t n;
    if (!f) return NULL;
    b = malloc(max + 1);
    if (!b) {
        fclose(f);
        return NULL;
    }
    n = fread(b, 1, max, f);
    fclose(f);
    b[n] = 0;
    return b;
}

static int link_to(const char *dir, const char *name, const char *target)
{
    char path[PATH_MAX];
    struct stat st;
    if (stat(target, &st) || !S_ISREG(st.st_mode)) return -1;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    unlink(path);
    return symlink(target, path);
}

/* every regular file of src_dir except `skip` */
static int link_folder(const char *dir, const char *src_dir, const char *skip)
{
    DIR *d = opendir(src_dir);
    struct dirent *e;
    char target[PATH_MAX];
    int n = 0;
    if (!d) return -1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || (skip && !strcmp(e->d_name, skip))) continue;
        snprintf(target, sizeof target, "%s/%s", src_dir, e->d_name);
        if (link_to(dir, e->d_name, target) == 0) n++;
    }
    closedir(d);
    return n;
}

static int links(const char *dir, const char *src_dir, const nv_link *l)
{
    char target[PATH_MAX];
    for (; l->link; l++) {
        snprintf(target, sizeof target, "%s/%s", src_dir, l->target);
        if (link_to(dir, l->link, target)) return -1;
    }
    return 0;
}

static void remove_folder_entries(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[PATH_MAX];
    if (!d) return;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        unlink(path);
    }
    closedir(d);
}

static int prepare(int i, const char *data_dir, const char *onecore_dir, const char *work_dir, char *err, size_t errlen)
{
    const nv_def *v = &VOICES[i];
    char src[PATH_MAX], path[PATH_MAX];
    char *key;
    if (G.prepared[i]) return 0;
    if (!data_dir || !*data_dir || !onecore_dir || !*onecore_dir || !work_dir || !*work_dir) {
        seterr(err, errlen, "no data folder%s", "");
        return -1;
    }
    if (!G.key[0]) {
        snprintf(path, sizeof path, "%s/model.key", data_dir);
        key = read_file(path, sizeof G.key - 1);
        if (!key) {
            seterr(err, errlen, "no model key in %s", data_dir);
            return -1;
        }
        key[strcspn(key, "\r\n")] = 0;
        snprintf(G.key, sizeof G.key, "%s", key);
        free(key);
        if (!G.key[0]) {
            seterr(err, errlen, "empty model key%s", "");
            return -1;
        }
    }
    snprintf(src, sizeof src, "%s/%s", data_dir, v->name);
    snprintf(path, sizeof path, "%s/%s", src, v->ini);
    if (access(path, R_OK)) {
        seterr(err, errlen, "no voice data for %s", v->name);
        return -1;
    }
    mkdir(work_dir, 0700);
    snprintf(G.work[i], sizeof G.work[i], "%s/%s", work_dir, v->name);
    if (mkdir(G.work[i], 0700) && errno != EEXIST) {
        seterr(err, errlen, "cannot create %s", G.work[i]);
        return -1;
    }
    remove_folder_entries(G.work[i]); /* links from an earlier install point into a bundle that may have moved */
    if (link_folder(G.work[i], src, NULL) < 6) {
        seterr(err, errlen, "incomplete voice data for %s", v->name);
        return -1;
    }
    if (!strcmp(v->locale, "en-US")) {
        if (links(G.work[i], onecore_dir, ONECORE_EN_US)) {
            seterr(err, errlen, "missing OneCoreVoice language data for %s", v->name);
            return -1;
        }
    } else {
        snprintf(path, sizeof path, "%s/en-GB", data_dir);
        if (link_folder(G.work[i], path, NULL) < 2 || links(G.work[i], onecore_dir, ONECORE_EN_GB)) {
            seterr(err, errlen, "missing en-GB language data for %s", v->name);
            return -1;
        }
    }
    G.prepared[i] = 1;
    return 0;
}

/* ---- the engine's configuration (under g_lock) ---- */
static void drop_voice(int i)
{
    if (G.synth[i] && G.synth[i] != SPX_INVALID) G.synth_release(G.synth[i]);
    if (G.config[i] && G.config[i] != SPX_INVALID) G.cfg_release(G.config[i]);
    G.synth[i] = G.config[i] = NULL;
}

/* the voice's synthesizer, built the first time (the model itself loads on its first utterance) */
static SPXH synth_for(int i)
{
    SPXH bag = SPX_INVALID, cfg = SPX_INVALID, syn = SPX_INVALID;
    if (G.synth[i]) return G.synth[i];
    if (G.cfg_create(&cfg)) return NULL;
    G.config[i] = cfg;
    if (G.cfg_add_path(cfg, G.work[i]) || G.cfg_set_format(cfg, FMT_RAW_24K_16BIT_MONO) || G.cfg_get_bag(cfg, &bag)) {
        drop_voice(i);
        return NULL;
    }
    G.bag_set(bag, PROP_SYNTH_OFFLINE_VOICE, NULL, VOICES[i].engine_name);
    G.bag_set(bag, PROP_SYNTH_MODEL_KEY, NULL, G.key);
    G.bag_release(bag);
    if (G.synth_create(&syn, cfg, SPX_INVALID)) { /* no audio output: the audio comes back to us */
        drop_voice(i);
        return NULL;
    }
    G.synth[i] = syn;
    return syn;
}

void cvn_release_engine(void)
{
    int i;
    pthread_mutex_lock(&g_lock);
    if (G.dl)
        for (i = 0; i < NVOICES; i++) drop_voice(i);
    pthread_mutex_unlock(&g_lock);
}

/* ---- public ---- */
cvn_voice *cvn_voice_open(const char *sdk_dir, const char *data_dir, const char *onecore_dir, const char *work_dir,
                          const char *voice, char *err, size_t errlen)
{
    int i = voice_index(voice);
    cvn_voice *v;
    if (i < 0) {
        seterr(err, errlen, "unknown neural voice%s", "");
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    if (load_sdk(sdk_dir, err, errlen) || prepare(i, data_dir, onecore_dir, work_dir, err, errlen)) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    pthread_mutex_unlock(&g_lock);
    v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->index = i;
    if (cv_rs_init(&v->rs, ENGINE_RATE)) {
        seterr(err, errlen, "cannot set up the resampler%s", "");
        free(v);
        return NULL;
    }
    return v;
}

void cvn_voice_close(cvn_voice *v)
{
    if (!v) return;
    cv_rs_free(&v->rs);
    free(v); /* the engine stays loaded for the next utterance of any neural voice */
}

int cvn_voice_sample_rate(const cvn_voice *v)
{
    (void)v;
    return CV_RS_OUT_RATE;
}

/* The request: the text XML-escaped (& < > " ') inside <prosody rate pitch>. At rate 1 and pitch 0 it renders exactly like
 * the plain text (measured). */
static char *ssml_for(const char *engine_name, const char *locale, const char *text, double speed, double semitones)
{
    size_t n = strlen(text), cap = n * 6 + 512, o = 0;
    char *s = malloc(cap);
    double pct;
    const char *p;
    if (!s) return NULL;
    if (semitones < -12) semitones = -12;
    if (semitones > 12) semitones = 12;
    pct = (pow(2.0, semitones / 12.0) - 1.0) * 100.0;
    o += (size_t)snprintf(s + o, cap - o,
                          "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='%s'>"
                          "<voice name='%s'><prosody rate='%.3f' pitch='%+.1f%%'>",
                          locale, engine_name, speed, pct);
    for (p = text; *p; p++) {
        const char *rep = NULL;
        switch (*p) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&apos;"; break;
        }
        if (rep) {
            memcpy(s + o, rep, strlen(rep));
            o += strlen(rep);
        } else {
            s[o++] = *p;
        }
    }
    o += (size_t)snprintf(s + o, cap - o, "</prosody></voice></speak>");
    return s;
}

int cvn_voice_speak(cvn_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                    void *user)
{
    char *clean, *ssml = NULL;
    SPXH syn;
    const char *p;
    cv_trimmer *t;
    SPXH res = SPX_INVALID, ds = SPX_INVALID;
    uint8_t buf[4800 + 1]; /* 100 ms at 24 kHz, plus a carried odd byte */
    size_t have = 0;
    int rc = 0, stopped = 0, status = 0;
    if (!v || !utf8 || !fn) return -1;
    clean = cv_sanitize_alloc(utf8);
    if (!clean) return -1;
    for (p = clean; *p == ' '; p++) {}
    if (!*p) { /* nothing speakable: no audio, and never any stand-in text */
        free(clean);
        return 0;
    }
    if (!(semitones == semitones)) semitones = 0;
    t = cv_trimmer_new(trim, fn, user);
    if (!t) {
        free(clean);
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    syn = G.dl && G.prepared[v->index] ? synth_for(v->index) : NULL;
    /* markup in the text is escaped: spoken, never obeyed */
    ssml = syn ? ssml_for(VOICES[v->index].engine_name, VOICES[v->index].locale, clean, cvn_speed_factor(sapi_rate),
                          semitones)
               : NULL;
    if (!ssml || G.start_ssml(syn, ssml, (uint32_t)strlen(ssml), &res) || G.ds_create(&ds, res)) {
        rc = -1;
        goto out;
    }
    cv_rs_reset(&v->rs);
    for (;;) {
        uint32_t got = 0;
        size_t samples, no;
        if (G.ds_read(ds, buf + have, (uint32_t)(sizeof buf - 1 - have), &got) || got == 0) break;
        have += got;
        samples = have / 2;
        if (samples) {
            int16_t pcm[2400];
            memcpy(pcm, buf, samples * 2);
            if (have & 1) buf[0] = buf[have - 1];
            have &= 1;
            if (cv_rs_feed(&v->rs, pcm, samples)) {
                rc = -1;
                stopped = 1;
            } else {
                no = cv_rs_run(&v->rs, 0);
                if (no && cv_trimmer_feed(t, v->rs.out, no)) {
                    rc = 1;
                    stopped = 1;
                }
            }
            if (stopped) {
                G.stop(syn); /* the engine stops within its current chunk */
                break;
            }
        }
    }
    if (!stopped) {
        size_t no;
        if (G.ds_status(ds, &status) == 0 && status == STREAM_CANCELED) {
            rc = -1; /* the engine gave up (the text, or a broken voice): whatever came is kept */
        }
        no = cv_rs_run(&v->rs, 1);
        if (no && cv_trimmer_feed(t, v->rs.out, no)) rc = 1;
        if (rc == 0 && cv_trimmer_finish(t)) rc = 1;
    }
out:
    if (ds != SPX_INVALID) G.ds_release(ds);
    if (res != SPX_INVALID) G.result_release(res);
    pthread_mutex_unlock(&g_lock);
    cv_trimmer_free(t);
    free(ssml);
    free(clean);
    return rc;
}
