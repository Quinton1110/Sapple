// The library around the decompiled engine: an engine object set up as the original's (unit stage,
// front end, synthesizer on four queues, then what a SAPI client does with ITTSAttributes), and each
// utterance run through the stages' own item handlers and thread loops, one phrase at a time, in the
// calling thread. See sapi4tts.h.
#include "sapi4tts.h"
#include "dllimage.h"
#include "voices.h"
#include "voice.h"
#include "unitsel.h"
#include "fe_init.h"
#include "fe_input.h"
#include "fe_phones.h"
#include "queue.h"
#include "crt_vc.h"
#include "vcrt.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef S4_DLL_BAKED
extern const DllBlob dll_baked;
#endif
#ifdef S4_VOICES_BAKED
extern const VcFile voice_files[];
extern const int voice_nfiles;
#endif

struct s4_engine {
    s4_mode mode;
    s4_limits lim;
    unsigned pitch, speed;
    uint32_t rate;
    Queue *q_text, *q_phones, *q_units, *q_audio;
    FrontEnd *fe;
    UnitStage *u;
    VocEngine *v;
    // each engine thread's MSVCRT rand state (all start at 1, as a new thread's does)
    uint32_t rand_setup, rand_fe, rand_unit, rand_synth;
    atomic_int stop, speaking;
};

// the engine's shared data (DLL data sections, voice table, lexicons, rule tapes) and this port's own
// process-wide tables (events, file table): all engine work holds this lock
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_init;          // 1: from a file, 2: baked
static int g_engines;       // engines opened so far: each takes one of the engine's 128 word objects, never freed

static void err_set(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

int s4_init(const char *dll_path) {
    int r = 0;
    pthread_mutex_lock(&g_lock);
    if (!g_init) {
        if (dll_path) {
            r = dll_image_load(dll_path) ? 0 : -1;
            if (!r) g_init = 1;
        } else {
#ifdef S4_DLL_BAKED
            r = dll_image_use(&dll_baked) ? 0 : -1;
            if (!r) g_init = 2;
#else
            r = -1;
#endif
        }
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

// where voice files come from for the call in progress (lock held)
static int use_voices(const char *voice_dir) {
    if (voice_dir) {
        vc_files_use(NULL, 0);
        return 0;
    }
#ifdef S4_VOICES_BAKED
    vc_files_use(voice_files, voice_nfiles);
    return 0;
#else
    return -1;
#endif
}

static void wide_to_char(char *out, size_t cap, const uint16_t *w) {
    size_t k = 0;
    for (; w[k] && k + 1 < cap; k++) out[k] = (char)uni_to_cp1252(w[k]);
    out[k] = 0;
}

static void list_free(ModeList *l) {
    for (uint32_t i = 0; i < l->count; i++) vc_free(GP(void, GP(ListItem, l->items)[i].data));
    if (l->items) vc_free(GP(void, l->items));
    vc_free(l);
}

typedef struct Voices { ModeList *paths, *hdrs, *infos; } Voices;
static void voices_load(Voices *vs, const char *voice_dir) {
    vs->paths = list_ctor(vc_malloc(sizeof(ModeList)));
    vs->hdrs = list_ctor(vc_malloc(sizeof(ModeList)));
    vs->infos = list_ctor(vc_malloc(sizeof(ModeList)));
    voices_enum(voice_dir ? voice_dir : "baked", vs->paths, vs->hdrs, vs->infos);
}
static void voices_free(Voices *vs) {
    list_free(vs->paths);
    list_free(vs->hdrs);
    list_free(vs->infos);
}
static void mode_fill(s4_mode *m, const TtsModeInfo *info, const VoiceHdr *h) {
    memset(m, 0, sizeof *m);
    wide_to_char(m->name, sizeof m->name, info->mode_name);
    wide_to_char(m->speaker, sizeof m->speaker, info->speaker);
    m->gender = info->gender;
    m->age = info->age;
    m->features = info->features;
    uint32_t rate;
    memcpy(&rate, (const uint8_t *)h + 0x8e0 + 4, 4);      // the voice's WAVEFORMATEX
    m->sample_rate = (int)rate;
}

int s4_list_modes(const char *voice_dir, s4_mode *modes, int max) {
    pthread_mutex_lock(&g_lock);
    if (!g_init || use_voices(voice_dir) < 0) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    Voices vs;
    voices_load(&vs, voice_dir);
    int n = (int)list_count(vs.infos);
    for (int i = 0; i < n && i < max; i++)
        mode_fill(&modes[i], GP(const TtsModeInfo, list_item(vs.infos, (uint32_t)i)),
                  GP(const VoiceHdr, list_item(vs.hdrs, (uint32_t)i)));
    voices_free(&vs);
    pthread_mutex_unlock(&g_lock);
    return n;
}

// ---------------------------------------------------------------- running the stages

typedef struct Idle { jmp_buf jb; } Idle;
static _Thread_local Idle *t_idle;
static void idle(void) { longjmp(t_idle->jb, 1); }

// a stage's thread loop until it would wait for input no other thread will send (its own rand state)
static void run_stage(int32_t (*loop)(void *), void *obj, uint32_t *rand_state) {
    Idle id, *prev = t_idle;
    t_idle = &id;
    vc_idle_hook = idle;
    vc_rand_use(rand_state);
    if (!setjmp(id.jb)) loop(obj);
    vc_rand_use(NULL);
    vc_idle_hook = NULL;
    t_idle = prev;
}
static int32_t unit_loop(void *u) { return unit_thread_loop(u); }
static int32_t voc_loop(void *v) { return voc_thread_loop(v); }

static void queue_drop(Queue *q) {
    QItem it;
    while (queue_pop(q, &it))
        if (it.data) vc_free(GP(void, it.data));
}

// the PCM the synthesizer handed to the audio side, in order; 1 if the callback asked to stop
static int deliver(s4_engine *e, s4_pcm_fn fn, void *user) {
    QItem it;
    int stop = 0;
    while (queue_pop(e->q_audio, &it)) {
        if (it.code == 6 && it.data && !stop && fn && fn(GP(const int16_t, it.data), it.bytes / 2, user)) stop = 1;
        if (it.data) vc_free(GP(void, it.data));
    }
    return stop;
}

// text through the front end, then its items one at a time through the unit stage and the synthesizer
static int run_text(s4_engine *e, const char *text, unsigned flags, s4_pcm_fn fn, void *user) {
    size_t n = strlen(text);
    uint16_t *w = vc_malloc(2 * (n + 1));
    for (size_t i = 0; i <= n; i++) w[i] = cp1252_to_uni[(uint8_t)text[i]];
    FeThreadFrame f;
    memset(&f, 0, sizeof f);
    f.item.code = 1;
    f.item.a.value = (flags & S4_TAGGED) ? 1u : 0u;
    f.item.v2 = 4;
    GPSET(f.item.data, (void *)w);
    f.h[0] = e->fe->abort_event;
    f.h[1] = e->q_text->ev_nonempty;
    f.h2[0] = e->fe->abort_event;
    f.h2[1] = e->q_phones->ev_room;
    vc_rand_use(&e->rand_fe);
    fe_text_item(e->fe, &f);
    vc_rand_use(NULL);
    // hold the front end's items back and pass them on one by one (the threads of the original would
    // take them as they come; the results are the same)
    uint32_t count = e->q_phones->count, k = 0;
    QItem *items = malloc((count ? count : 1) * sizeof(QItem));
    while (k < count && queue_pop(e->q_phones, &items[k])) k++;
    count = k;
    int stopped = 0;
    for (k = 0; k < count; k++) {
        if (atomic_load(&e->stop)) {
            stopped = 1;
            break;
        }
        queue_push(e->q_phones, &items[k]);
        run_stage(unit_loop, e->u, &e->rand_unit);
        // and the unit stage's items one at a time through the synthesizer, the audio out after each
        uint32_t nu = e->q_units->count, j = 0;
        QItem *units = malloc((nu ? nu : 1) * sizeof(QItem));
        while (j < nu && queue_pop(e->q_units, &units[j])) j++;
        nu = j;
        for (j = 0; j < nu; j++) {
            if (atomic_load(&e->stop)) {
                stopped = 1;
                break;
            }
            queue_push(e->q_units, &units[j]);
            run_stage(voc_loop, e->v, &e->rand_synth);
            if (deliver(e, fn, user)) {
                stopped = 1;
                j++;
                break;
            }
        }
        for (; j < nu; j++)     // (what was not handed on is dropped)
            if (units[j].data) vc_free(GP(void, units[j].data));
        free(units);
        if (stopped) {
            k++;
            break;
        }
    }
    for (; k < count; k++)
        if (items[k].data) vc_free(GP(void, items[k].data));
    free(items);
    if (stopped) {
        queue_drop(e->q_phones);
        queue_drop(e->q_units);
        queue_drop(e->q_audio);
    }
    return stopped;
}

// ---------------------------------------------------------------- engines

s4_engine *s4_open(const char *voice_dir, const char *mode_name, char *err, size_t errlen) {
    pthread_mutex_lock(&g_lock);
    s4_engine *e = NULL;
    Voices vs = { 0 };
    int have_voices = 0;
    if (!g_init) { err_set(err, errlen, "s4_init has not succeeded"); goto out; }
    if (g_engines >= 127) { err_set(err, errlen, "no more engines in this process (127)"); goto out; }
    if (use_voices(voice_dir) < 0) { err_set(err, errlen, "no voice directory, and no voices compiled in"); goto out; }
    if (voice_dir) {    // the engine's "module" is in the voice directory: its user lexicon is there
        char mp[1100];
        snprintf(mp, sizeof mp, "%s\\msttssyn.dll", voice_dir);
        vc_set_module_path(mp);
    }
    voices_load(&vs, voice_dir);
    have_voices = 1;
    uint32_t idx = list_count(vs.infos);
    for (uint32_t i = 0; i < list_count(vs.infos) && idx == list_count(vs.infos); i++) {
        char name[300];
        wide_to_char(name, sizeof name, GP(const TtsModeInfo, list_item(vs.infos, i))->mode_name);
        if (!strcmp(name, mode_name)) idx = i;
    }
    if (idx == list_count(vs.infos)) { err_set(err, errlen, "no such voice mode"); goto out; }
    e = calloc(1, sizeof *e);
    if (!e) goto out;
    e->rand_setup = e->rand_fe = e->rand_unit = e->rand_synth = 1;
    const VoiceHdr *mode = GP(const VoiceHdr, list_item(vs.hdrs, idx));
    mode_fill(&e->mode, GP(const TtsModeInfo, list_item(vs.infos, idx)), mode);
    vc_rand_use(&e->rand_setup);    // (the thread that creates the engine: the voice's noise table)
    int32_t voice = 0, variant = 0, index = 0;
    if (mode_table_add(mode, vs.paths, vs.hdrs, &voice, &variant, &index) < 0) {
        err_set(err, errlen, "no voice for the mode");
        goto fail;
    }
    const char *vpath = GP(const char, list_item(vs.paths, (uint32_t)index));
    e->rate = (uint32_t)e->mode.sample_rate;
    e->q_text = queue_ctor(vc_malloc(sizeof(Queue)));
    e->q_phones = queue_ctor(vc_malloc(sizeof(Queue)));
    e->q_units = queue_ctor(vc_malloc(sizeof(Queue)));
    e->q_audio = queue_ctor(vc_malloc(sizeof(Queue)));
    // (the engine sets high-water marks of 1, 1 and 10 to pace its threads; stages run one after
    // another must never find a queue full. Setting one also sets the events the constructor clears.)
    queue_set_high(e->q_phones, 0x7fffffff);
    queue_set_high(e->q_units, 0x7fffffff);
    queue_set_high(e->q_audio, 0x7fffffff);
    g_engines++;
    e->fe = fe_ctor(vc_calloc(1, sizeof(FrontEnd)));
    e->u = unit_ctor(vc_calloc(1, sizeof(UnitStage)));
    e->v = voc_engine_ctor(vc_calloc(1, sizeof(VocEngine)));
    uint16_t *wpath = vc_malloc(2 * (strlen(vpath) + 1));
    vc_MultiByteToWideChar_cp1252((const uint8_t *)vpath, (int)strlen(vpath) + 1, wpath);
    void *stg = NULL;
    int32_t hr = vc_StgOpenStorage(wpath, 0x20, &stg);
    vc_free(wpath);
    if (hr < 0) { err_set(err, errlen, "cannot open the voice file"); goto fail; }
    if (unit_init(e->u, (uint8_t *)e->q_phones, (uint8_t *)e->q_units, stg, (const uint8_t *)mode, voice, variant, 0, 0x46a) < 0 ||
        fe_setup(e->fe, (uint8_t *)e->q_text, (uint8_t *)e->q_phones, (const uint8_t *)mode, voice, variant,
                 (int32_t)(uintptr_t)stg, 0, 0x46a, (int32_t)(uintptr_t)e->u) < 0 ||
        voc_engine_init(e->v, (uint8_t *)e->q_units, (uint8_t *)e->q_audio, voice, variant, stg, 1, e->rate, e->rate,
                        (int32_t)(mode->features & 0x8000), 0, 0x46e) < 0) {
        vc_com_release(stg);
        err_set(err, errlen, "engine set-up failed");
        goto fail;
    }
    vc_com_release(stg);
    vc_rand_use(NULL);
    // what a SAPI client does next: read pitch and speed, probe the limits by setting the extremes and
    // reading them back, restore the defaults (ITTSAttributes calls these directly)
    uint16_t p;
    uint32_t sp;
    fe_get_pitch(e->fe, &p);
    e->lim.pitch_default = p;
    unit_get_rate(e->u, &sp);
    e->lim.speed_default = sp;
    fe_set_pitch_level(e->fe, 0);
    fe_get_pitch(e->fe, &p);
    e->lim.pitch_min = p;
    fe_set_pitch_level(e->fe, 0xffff);
    fe_get_pitch(e->fe, &p);
    e->lim.pitch_max = p;
    fe_set_pitch_level(e->fe, (uint16_t)e->lim.pitch_default);
    unit_set_rate(e->u, 0);
    unit_get_rate(e->u, &sp);
    e->lim.speed_min = sp;
    unit_set_rate(e->u, 0xffffffffu);
    unit_get_rate(e->u, &sp);
    e->lim.speed_max = sp;
    unit_set_rate(e->u, e->lim.speed_default);
    e->pitch = e->lim.pitch_default;
    e->speed = e->lim.speed_default;
    goto out;
fail:
    vc_rand_use(NULL);
    free(e);        // (the engine objects made so far are not torn down: see s4_close)
    e = NULL;
out:
    if (have_voices) voices_free(&vs);
    pthread_mutex_unlock(&g_lock);
    return e;
}

void s4_close(s4_engine *e) {
    if (!e) return;
    pthread_mutex_lock(&g_lock);
    // The original's teardown (the engine object's destructor and the stages' own) is not part of the
    // decompiled code: what is released here is what this library made and can account for - the
    // queued items, the queues, the events. The stage objects' inner buffers, the word object's slot
    // (128 per process) and the voice's share of the voice table stay allocated.
    Queue *qs[4] = { e->q_text, e->q_phones, e->q_units, e->q_audio };
    for (int k = 0; k < 4; k++) {
        queue_drop(qs[k]);
        vc_event_close(qs[k]->ev_nonempty);
        vc_event_close(qs[k]->ev_full);
        vc_event_close(qs[k]->ev_empty);
        vc_event_close(qs[k]->ev_room);
    }
    vc_event_close(e->fe->abort_event);
    vc_event_close(e->fe->done_event);
    vc_event_close(e->u->ev_stop);
    vc_event_close(e->u->ev_done);
    vc_event_close(e->v->ev_stop);
    vc_event_close(e->v->ev_done);
    pthread_mutex_unlock(&g_lock);
    free(e);
}

const s4_mode *s4_mode_info(const s4_engine *e) { return e ? &e->mode : NULL; }
int s4_sample_rate(const s4_engine *e) { return e ? (int)e->rate : 0; }
void s4_get_limits(const s4_engine *e, s4_limits *lim) {
    if (e && lim) *lim = e->lim;
}

unsigned s4_get_pitch(const s4_engine *e) { return e ? e->pitch : 0; }
unsigned s4_get_speed(const s4_engine *e) { return e ? e->speed : 0; }

int s4_set_pitch(s4_engine *e, unsigned pitch) {
    if (!e) return -1;
    if (pitch < e->lim.pitch_min) pitch = e->lim.pitch_min;
    if (pitch > e->lim.pitch_max) pitch = e->lim.pitch_max;
    pthread_mutex_lock(&g_lock);
    int r = 0;
    if (pitch != e->pitch) {
        r = fe_set_pitch_level(e->fe, (uint16_t)pitch) < 0 ? -1 : 0;
        if (!r) e->pitch = pitch;
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

int s4_set_speed(s4_engine *e, unsigned wpm) {
    if (!e) return -1;
    if (wpm < e->lim.speed_min) wpm = e->lim.speed_min;
    if (wpm > e->lim.speed_max) wpm = e->lim.speed_max;
    pthread_mutex_lock(&g_lock);
    int r = 0;
    if (wpm != e->speed) {
        r = unit_set_rate(e->u, wpm) < 0 ? -1 : 0;
        if (!r) e->speed = wpm;
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

int s4_speak(s4_engine *e, const char *text, unsigned flags, s4_pcm_fn fn, void *user) {
    if (!e || !text) return -1;
    pthread_mutex_lock(&g_lock);
    atomic_store(&e->stop, 0);
    atomic_store(&e->speaking, 1);
    int r = run_text(e, text, flags, fn, user);
    atomic_store(&e->speaking, 0);
    atomic_store(&e->stop, 0);
    pthread_mutex_unlock(&g_lock);
    return r;
}

void s4_stop(s4_engine *e) {
    if (e && atomic_load(&e->speaking)) atomic_store(&e->stop, 1);
}

int s4_reset(s4_engine *e) {
    if (!e) return -1;
    pthread_mutex_lock(&g_lock);
    queue_drop(e->q_text);
    queue_drop(e->q_phones);
    queue_drop(e->q_units);
    queue_drop(e->q_audio);
    // the engine's own reset: the \Rst\ tag through all three stages (what it produces is discarded)
    run_text(e, "\\Rst\\", S4_TAGGED, NULL, NULL);
    fe_set_pitch_level(e->fe, (uint16_t)e->lim.pitch_default);
    unit_set_rate(e->u, e->lim.speed_default);
    e->pitch = e->lim.pitch_default;
    e->speed = e->lim.speed_default;
    pthread_mutex_unlock(&g_lock);
    return 0;
}
