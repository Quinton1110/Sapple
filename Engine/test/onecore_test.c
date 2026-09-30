/* Mac test driver for Microsoft David, Zira and Mark (en-US), Hazel, George and Susan (en-GB), Catherine and James (en-AU)
 * and Linda and Richard (en-CA), the Windows 10 / 11
 * OneCore voices, and the neural Eva (en-US), Sarah (en-GB) and Matilda (en-AU): the engine (Engine/onecore)
 * and its bridge (Shared/Bridge/cvo_bridge.c) exactly as the iOS targets compile them, driven the way
 * ClassicEngine.swift drives them from the extension.
 *
 *   onecore_test DATA_DIR say VOICE[:emotion] RATE SEMITONES "text" out.wav [notrim]
 *   onecore_test DATA_DIR selftest OUTDIR       every check below, all thirteen voices; exit status = failures
 *   onecore_test DATA_DIR threads [reps]        two voices on two threads (run it under TSan: make onecore-tsan)
 *   onecore_test DATA_DIR bench [seconds]       open time, first-audio latency, real-time factor, memory per voice
 *   onecore_test DATA_DIR eva OUTDIR            Eva only: her checks (self-test of the models, models vs rules, the long
 *                                               paragraph latency / memory, rules-vs-models A/B WAVs)
 */
#include "zira_tts.h"
#include "cvo_bridge.h"
#include "zf1_eng.h"   /* the en-GB checks look at the front end's pronunciations and the phone sets */
#include "zf2_int.h"

#include <mach/mach.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CV_ASAN 1
#endif
#endif
#ifndef CV_ASAN
#define CV_ASAN 0
#endif

#define NVOICES 13
static const char *const VOICES[NVOICES] = {"David", "Zira", "Mark", "Hazel", "George", "Susan", "Eva", "Sarah",
                                            "Catherine", "James", "Linda", "Richard", "Matilda"};
/* only the en-US voice files carry an [EmotionRecipe] (Eva's too) */
static int has_emotions(const char *voice) { return zira_tts_voice_lcid(voice) == 1033; }
static const char *const EMOTIONS[3] = {"happy", "sad", "angry"};

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* what jetsam counts on iOS */
static double footprint_mb(void)
{
    task_vm_info_data_t info;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &n) != KERN_SUCCESS) return -1;
    return info.phys_footprint / 1048576.0;
}

typedef struct {
    int16_t *pcm;
    size_t n, cap;
    size_t stop_after; /* return "stop" once this many samples arrived (0 = never) */
    double t_first, t_stop;
} sink;

static int on_pcm(const int16_t *pcm, size_t n, void *user)
{
    sink *s = user;
    if (!s->t_first) s->t_first = now_s();
    if (s->n + n > s->cap) {
        s->cap = (s->n + n) * 2 + 65536;
        s->pcm = realloc(s->pcm, s->cap * sizeof *s->pcm);
    }
    memcpy(s->pcm + s->n, pcm, n * sizeof *pcm);
    s->n += n;
    if (s->stop_after && s->n >= s->stop_after) {
        s->t_stop = now_s();
        return 1;
    }
    return 0;
}

static void write_wav(const char *path, const int16_t *pcm, size_t n, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    uint32_t datasz = (uint32_t)(n * 2), riff = 36 + datasz, br = rate * 2, fmtsz = 16;
    uint16_t pcmfmt = 1, ch = 1, ba = 2, bits = 16;
    if (!f) return;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtsz, 4, 1, f); fwrite(&pcmfmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&datasz, 4, 1, f); fwrite(pcm, 2, n, f);
    fclose(f);
}

static int failures;
static void check(int ok, const char *what)
{
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    fflush(stdout);
    if (!ok) failures++;
}

static int speak(cvo_voice *v, const char *text, double rate, double semi, int trim, sink *s)
{
    s->n = 0;
    s->t_first = s->t_stop = 0;
    return cvo_voice_speak(v, text, rate, semi, trim, on_pcm, s);
}

static int same(const sink *a, const sink *b) { return a->n == b->n && !memcmp(a->pcm, b->pcm, a->n * 2); }

/* RMS in dBFS of the whole buffer (non-silence check) */
static double rms_db(const sink *s)
{
    double e = 0;
    size_t i;
    if (!s->n) return -200;
    for (i = 0; i < s->n; i++) e += (double)s->pcm[i] * s->pcm[i];
    return 10 * log10(e / s->n / (32768.0 * 32768.0) + 1e-20);
}

/* the engine alone (no bridge, no sanitizer), 16 kHz */
typedef struct {
    int16_t *p;
    size_t n, cap;
} rawbuf;
static int raw_collect(const int16_t *pcm, size_t n, void *user)
{
    rawbuf *b = user;
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 65536;
        b->p = realloc(b->p, b->cap * sizeof *b->p);
    }
    memcpy(b->p + b->n, pcm, n * sizeof *pcm);
    b->n += n;
    return 0;
}
/* use_f: 0 = zira_tts_set_rate((int)rate), 1 = zira_tts_set_rate_f(rate) */
static int raw_render(const char *dir, const char *voice, const char *text, double rate, int use_f, rawbuf *b)
{
    char err[256];
    zira_tts *t = zira_tts_open(dir, voice, err, sizeof err);
    zira_callbacks cb = {raw_collect, NULL, b};
    int rc;
    b->n = 0;
    if (!t) return -2;
    if (use_f) zira_tts_set_rate_f(t, rate);
    else zira_tts_set_rate(t, (int)rate);
    rc = zira_tts_speak(t, text, 0, &cb);
    zira_tts_close(t);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ threads */

typedef struct {
    const char *dir, *voice, *emotion;
    int reps, ok;
    const char *text;
    unsigned long long hash;
} thr_arg;

static unsigned long long fnv(const int16_t *p, size_t n)
{
    unsigned long long h = 1469598103934665603ull;
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n * 2; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static void *thr_main(void *p)
{
    thr_arg *a = p;
    char err[256];
    cvo_voice *v = cvo_voice_open(a->dir, a->voice, err, sizeof err);
    sink s = {0};
    int i;
    a->ok = v != NULL;
    a->hash = 1469598103934665603ull;
    if (v && a->emotion) cvo_voice_set_emotion(v, a->emotion);
    for (i = 0; v && i < a->reps; i++) {
        if (speak(v, a->text, i % 3 == 2 ? 6.5 : 0, i % 2 ? 1.5 : 0, 1, &s) != 0) a->ok = 0;
        a->hash ^= fnv(s.pcm, s.n) + (unsigned long long)i;
    }
    cvo_voice_close(v);
    free(s.pcm);
    return NULL;
}

static int run_threads(const char *dir, int reps)
{
    const char *ta = "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.";
    const char *tb = "On March 3rd, 2021 at 10:30 am, 1,234 people paid $56.78 each. Is that right?";
    /* pairs: two different voices (float David + fixed-point Zira), the same voice twice (Mark, one happy), the two
       locales at once (float Hazel + David), and the two fixed-point en-GB voices */
    /* ...and Eva (neural back end) next to David, and Eva twice; Sarah (neural, en-GB) next to Hazel, and Sarah + Eva;
       Matilda (neural, en-AU, LSF order 24) next to Catherine (same language data) */
    thr_arg pairs[9][2] = {{{dir, "David", NULL, reps, 0, ta, 0}, {dir, "Zira", NULL, reps, 0, tb, 0}},
                           {{dir, "Mark", NULL, reps, 0, tb, 0}, {dir, "Mark", "happy", reps, 0, ta, 0}},
                           {{dir, "Hazel", NULL, reps, 0, ta, 0}, {dir, "David", NULL, reps, 0, tb, 0}},
                           {{dir, "George", NULL, reps, 0, tb, 0}, {dir, "Susan", NULL, reps, 0, ta, 0}},
                           {{dir, "Eva", NULL, reps, 0, ta, 0}, {dir, "David", NULL, reps, 0, tb, 0}},
                           {{dir, "Eva", NULL, reps, 0, tb, 0}, {dir, "Eva", "happy", reps, 0, ta, 0}},
                           {{dir, "Sarah", NULL, reps, 0, ta, 0}, {dir, "Hazel", NULL, reps, 0, tb, 0}},
                           {{dir, "Sarah", NULL, reps, 0, tb, 0}, {dir, "Eva", NULL, reps, 0, ta, 0}},
                           {{dir, "Matilda", NULL, reps, 0, ta, 0}, {dir, "Catherine", NULL, reps, 0, tb, 0}}};
    int k;
    for (k = 0; k < 9; k++) {
        thr_arg *single = pairs[k], par[2];
        pthread_t th[2];
        char what[256];
        int i;
        thr_main(&single[0]); /* reference: each sequence alone, fresh voices */
        thr_main(&single[1]);
        memcpy(par, single, sizeof par);
        for (i = 0; i < 2; i++) pthread_create(&th[i], NULL, thr_main, &par[i]);
        for (i = 0; i < 2; i++) pthread_join(th[i], NULL);
        snprintf(what, sizeof what, "%s + %s%s on two threads, %d utterances each: identical to each alone (%s / %s)",
                 single[0].voice, single[1].voice, single[1].emotion ? " (happy)" : "", reps,
                 par[0].hash == single[0].hash ? "same" : "DIFFERENT", par[1].hash == single[1].hash ? "same" : "DIFFERENT");
        check(par[0].ok && par[1].ok && par[0].hash == single[0].hash && par[1].hash == single[1].hash, what);
    }
    return failures;
}

/* ------------------------------------------------------------------------------------------------ selftest */

static void selftest_voice(const char *dir, const char *voice, const char *od)
{
    char err[256], what[512], path[1024], lower[16];
    cvo_voice *v;
    sink s = {0}, a = {0}, b = {0};
    size_t base, i;
    int rc;
    double t0 = now_s(), mem0 = footprint_mb();
    const char *tail = "This sentence must still be spoken.";

    for (i = 0; voice[i] && i < sizeof lower - 1; i++) lower[i] = (char)(voice[i] | 0x20);
    lower[i] = 0;
    printf("== %s\n", voice);
    v = cvo_voice_open(dir, voice, err, sizeof err);
    snprintf(what, sizeof what, "open %s (%.1f ms, footprint +%.1f MB)", voice, (now_s() - t0) * 1000, footprint_mb() - mem0);
    check(v != NULL, what);
    if (!v) {
        printf("open failed: %s\n", err);
        return;
    }

    /* 1. sentences, numbers, dates, punctuation: they speak, loud enough, and the WAVs go to transcription */
    {
        static const struct {
            const char *name, *text;
        } texts[] = {
            {"hello", "Hello, my name is Microsoft %s."},
            {"fox", "The quick brown fox jumps over the lazy dog, and then it runs away into the forest."},
            {"numbers", "I counted 1,234 apples, 56 pears and 7.5 kilograms of grapes; that is 3 times more than last year."},
            {"dates", "The meeting moved from March 3rd, 2021 to 12/25/2024 at 10:30 am, and it costs $56.78."},
            {"punct", "Wait... really? Yes! Call 555-1234, or email me: it's (almost) done - \"quoted\" and 'single'."},
            {"battery", "65% battery power, charging."},
            {"question", "Is this a question?"},
            {"long", "Alice was beginning to get very tired of sitting by her sister on the bank, and of having nothing to "
                     "do: once or twice she had peeped into the book her sister was reading, but it had no pictures or "
                     "conversations in it, and what is the use of a book, thought Alice, without pictures or conversations?"},
        };
        for (i = 0; i < sizeof texts / sizeof *texts; i++) {
            char text[1024];
            double t1 = now_s();
            snprintf(text, sizeof text, texts[i].text, voice);
            rc = speak(v, text, 0, 0, 1, &s);
            snprintf(path, sizeof path, "%s/%s_%s.wav", od, lower, texts[i].name);
            write_wav(path, s.pcm, s.n, 22050);
            snprintf(what, sizeof what, "%-8s rc=%d %.2f s audio, %.1f dBFS, in %.0f ms (first audio %.0f ms) -> %s",
                     texts[i].name, rc, s.n / 22050.0, rms_db(&s), (now_s() - t1) * 1000,
                     s.t_first ? (s.t_first - t1) * 1000 : -1, path);
            check(rc == 0 && s.n > 22050 / 2 && rms_db(&s) > -40, what);
        }
    }

    /* 2. emotions: each one speaks, differs from neutral, and neutral comes back exactly (en-GB: none, refused) */
    if (!has_emotions(voice)) {
        const char *t = "I can't believe we finally made it here today.";
        int k, refused = 1;
        speak(v, t, 0, 0, 1, &a);
        for (k = 0; k < 3; k++) refused &= cvo_voice_set_emotion(v, EMOTIONS[k]) == -1;
        speak(v, t, 0, 0, 1, &b);
        snprintf(what, sizeof what, "no [EmotionRecipe] in its INI: happy / sad / angry refused, speaks neutrally (bit-identical)");
        check(refused && zira_tts_voice_lcid(voice) != 1033 && same(&a, &b), what);
    } else {
        const char *t = "I can't believe we finally made it here today.";
        speak(v, t, 0, 0, 1, &a);
        for (i = 0; i < 3; i++) {
            int er = cvo_voice_set_emotion(v, EMOTIONS[i]);
            rc = speak(v, t, 0, 0, 1, &s);
            snprintf(path, sizeof path, "%s/%s_%s.wav", od, lower, EMOTIONS[i]);
            write_wav(path, s.pcm, s.n, 22050);
            snprintf(what, sizeof what, "emotion %-5s: set %d, rc=%d, %.2f s (neutral %.2f s), %.1f dBFS (neutral %.1f), differs",
                     EMOTIONS[i], er, rc, s.n / 22050.0, a.n / 22050.0, rms_db(&s), rms_db(&a));
            check(er == 0 && rc == 0 && s.n > 22050 / 2 && !same(&s, &a), what);
        }
        check(cvo_voice_set_emotion(v, "bored") == -1, "unknown emotion refused (speaks neutrally)");
        speak(v, t, 0, 0, 1, &b);
        check(same(&a, &b), "after an unknown emotion: neutral again, bit-identical");
        cvo_voice_set_emotion(v, "sad");
        cvo_voice_set_emotion(v, NULL);
        speak(v, t, 0, 0, 1, &b);
        check(same(&a, &b), "emotion NULL: neutral again, bit-identical");
    }

    /* 3. hostile text: the sentence after it must still be spoken */
    rc = speak(v, tail, 0, 0, 1, &s);
    base = s.n;
    check(rc == 0 && base > 22050, "baseline sentence speaks");
    {
        static const char *hostile[] = {
            "Party time \xF0\x9F\x98\x80\xF0\x9F\x8E\x89 yes.",    /* emoji */
            "Go \xE2\x86\x92 next.",                             /* right arrow */
            "Copyright \xC2\xA9 2024, 50\xE2\x82\xAC, 20\xC2\xB0.", /* Latin-1 / Windows-1252 symbols */
            "\xE4\xBD\xA0\xE5\xA5\xBD \xE4\xB8\x96\xE7\x95\x8C.", /* Chinese */
            "\xC3\x9F.",                                         /* lone sharp s */
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.",
            "\xCE\xB1\xCE\xB2\xCE\xB3 \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82.", /* Greek, Cyrillic */
            "\xEF\xBF\xBD\xEF\xBB\xBF\xE2\x80\x8B.",             /* U+FFFD, BOM, ZWSP */
            "\xFF\xFE\x80 bad bytes.",                           /* invalid UTF-8 */
            "<rate speed=\"10\">tags</rate> <silence msec=\"5000\"/> <bookmark mark=\"x\"/> &amp; <pitch middle=\"10\">.",
            "<lang langid=\"409\">switch</lang> <mstts:express-as style=\"cheerful\">x</mstts:express-as>.",
            "C:\\Windows\\System32 \\Pit=400\\ \\\\ back\\slashes.",
            "!!!???...,,,;;;:::---___***###@@@%%%^^^&&&((()))[[[]]]{{{}}}|||~~~```.",
            "12345678901234567890123456789012345678901234567890 0.000000001 -0 1e308 $0.00 99999999999999999999%.",
            "a.b.c.d.e.f.g.h.i.j.k.l.m.n.o.p.q.r.s.t.u.v.w.x.y.z.",
            "https://www.example.com/path?query=1&x=y#frag mail@example.com.",
            "A \t\n\r\v\f B \x01\x02\x7F C.",
        };
        for (i = 0; i < sizeof hostile / sizeof *hostile; i++) {
            char text[1024];
            snprintf(text, sizeof text, "%s %s", hostile[i], tail);
            rc = speak(v, text, 0, 0, 1, &s);
            snprintf(path, sizeof path, "%s/%s_hostile%zu.wav", od, lower, i);
            write_wav(path, s.pcm, s.n, 22050);
            snprintf(what, sizeof what, "hostile %2zu: rc=%d, %.2f s (the tail alone %.2f s)", i, rc, s.n / 22050.0, base / 22050.0);
            check(rc == 0 && s.n >= base, what);
        }
        {
            size_t L = 20000, k;
            char *big = malloc(L + 64);
            for (k = 0; k < L; k++) big[k] = (k % 97 == 96) ? ' ' : (char)('a' + (k * 7) % 26);
            strcpy(big + L, ". ");
            strcat(big, tail);
            t0 = now_s();
            rc = speak(v, big, 12, 0, 1, &s);
            snprintf(what, sizeof what, "20,000 letters in words of 96, at rate 12: rc=%d, %.1f s in %.1f s", rc, s.n / 22050.0,
                     now_s() - t0);
            check(rc == 0 && s.n > base, what);
            free(big);
        }
    }
    rc = speak(v, "", 0, 0, 1, &s);
    check(rc == 0 && s.n == 0, "empty text gives no audio and no stand-in text");
    rc = speak(v, " \xF0\x9F\x98\x80 \xE2\x80\x8B ", 0, 0, 1, &s);
    check(rc == 0 && s.n == 0, "emoji-only text gives no audio");

    /* 4. rate: the engine's own control, fractional, monotonic; whole rates exactly the original engine's */
    {
        static const double rates[] = {-10, -5, 0, 2.5, 5, 5.5, 10, 14, 18};
        size_t len[sizeof rates / sizeof *rates];
        int mono = 1;
        char list[256] = "";
        for (i = 0; i < sizeof rates / sizeof *rates; i++) {
            speak(v, tail, rates[i], 0, 1, &s);
            len[i] = s.n;
            if (i && len[i] >= len[i - 1]) mono = 0;
            snprintf(list + strlen(list), sizeof list - strlen(list), " %g:%.2fs", rates[i], s.n / 22050.0);
            if (rates[i] == -10 || rates[i] == 18) {
                snprintf(path, sizeof path, "%s/%s_rate%+g.wav", od, lower, rates[i]);
                write_wav(path, s.pcm, s.n, 22050);
            }
        }
        snprintf(what, sizeof what, "rate gets strictly faster:%s (x%.2f at 18 vs 0)", list, (double)len[2] / len[8]);
        check(mono, what);
    }
    {
        rawbuf r1 = {0}, r2 = {0};
        int ok = 1;
        double r;
        for (r = -10; r <= 20; r += 5) {
            raw_render(dir, voice, tail, r, 0, &r1);
            raw_render(dir, voice, tail, r, 1, &r2);
            if (r1.n == 0 || r1.n != r2.n || memcmp(r1.p, r2.p, r1.n * 2)) ok = 0;
        }
        check(ok, "patch: zira_tts_set_rate_f(n) renders bit-identical to the original zira_tts_set_rate(n) (-10, -5 .. 20)");
        raw_render(dir, voice, tail, 3, 1, &r1);
        raw_render(dir, voice, tail, 3.5, 1, &r2);
        {
            size_t n3 = r1.n;
            raw_render(dir, voice, tail, 4, 1, &r1);
            snprintf(what, sizeof what, "fractional rate lands between its neighbours: 3 %.3f s > 3.5 %.3f s > 4 %.3f s",
                     n3 / 16000.0, r2.n / 16000.0, r1.n / 16000.0);
            check(n3 > r2.n && r2.n > r1.n, what);
        }
        free(r1.p);
        free(r2.p);
    }

    /* 5. pitch: semitones onto the engine's half-semitone steps, +-5 semitones */
    {
        int r1, r2, r3;
        r1 = speak(v, tail, 0, 0, 1, &s);
        r2 = speak(v, tail, 0, 5, 1, &a);
        r3 = speak(v, tail, 0, -12, 1, &b);
        snprintf(path, sizeof path, "%s/%s_pitch_up5.wav", od, lower);
        write_wav(path, a.pcm, a.n, 22050);
        snprintf(path, sizeof path, "%s/%s_pitch_down12.wav", od, lower);
        write_wav(path, b.pcm, b.n, 22050);
        snprintf(what, sizeof what, "pitch 0 / +5 / -12 semitones (steps %d %d %d): rc %d %d %d, %.2f %.2f %.2f s, all different",
                 cvo_pitch_for_semitones(0), cvo_pitch_for_semitones(5), cvo_pitch_for_semitones(-12), r1, r2, r3,
                 s.n / 22050.0, a.n / 22050.0, b.n / 22050.0);
        check(r1 == 0 && r2 == 0 && r3 == 0 && !same(&s, &a) && !same(&s, &b) && !same(&a, &b) &&
                  cvo_pitch_for_semitones(-12) == -10 && cvo_pitch_for_semitones(2.6) == 5,
              what);
    }

    /* 6. the trim: a lone digit */
    {
        rawbuf r = {0};
        raw_render(dir, voice, "5", 0, 0, &r);
        speak(v, "5", 0, 0, 1, &s);
        snprintf(what, sizeof what, "digit trim: %.0f ms from the engine -> %.0f ms", r.n / 16.0, s.n / 22.05);
        /* Eva pads less than the HMM voices (her INI: SpeakSessionEnd / SentenceBoundary 250 ms, David's 750) */
        check(s.n > 0 && s.n / 22.05 < r.n / 16.0 - (strcmp(voice, "Eva") ? 300 : 200), what);
        snprintf(path, sizeof path, "%s/%s_digit.wav", od, lower);
        write_wav(path, s.pcm, s.n, 22050);
        free(r.p);
    }

    /* 7. cancel mid-utterance, then speak again on the same voice */
    {
        char alice[4000] = "";
        for (i = 0; i < 20; i++) strcat(alice, "Alice was beginning to get very tired of sitting by her sister. ");
        s.stop_after = 22050;
        t0 = now_s();
        rc = speak(v, alice, 0, 0, 1, &s);
        s.stop_after = 0;
        snprintf(what, sizeof what, "cancel after 1 s of audio: rc=%d, %.2f s delivered, returned %.1f ms after asking", rc,
                 s.n / 22050.0, (now_s() - s.t_stop) * 1000);
        check(rc == 1 && s.n < 22050 * 2, what);
        rc = speak(v, tail, 0, 0, 1, &s);
        snprintf(what, sizeof what, "the next utterance after a cancel: rc=%d, %.2f s (fresh voice %.2f s)", rc, s.n / 22050.0,
                 base / 22050.0);
        check(rc == 0 && s.n == base, what);
    }

    /* 8. state between utterances: fresh and warm voices render the same text identically (the engine re-seeds) */
    {
        cvo_voice *f1 = cvo_voice_open(dir, voice, err, sizeof err);
        const char *t = "On March 3rd, 2021 at 10:30 am, 1,234 people paid $56.78 each. Is that right?";
        int k, differ = 0;
        speak(f1, t, 0, 0, 1, &a);
        for (k = 0; k < 3; k++) {
            speak(v, t, 0, 0, 1, &b);
            if (!same(&a, &b)) differ++;
        }
        snprintf(what, sizeof what, "a warm voice renders like a fresh one (%d of 3 differ)", differ);
        check(differ == 0, what);
        cvo_voice_close(f1);
    }

    /* 9. many utterances on one voice: memory stays flat. Under AddressSanitizer the footprint mostly measures the
     *    sanitizer's quarantine of freed blocks (it rose or fell by up to 15 MB with the voice order), so the ASan
     *    build only reports it and the plain build checks it (onecore_test DATA memory, part of make onecore-test). */
    {
        double m1, m2;
        int k;
        for (k = 0; k < 10; k++) speak(v, "The quick brown fox jumps over the lazy dog.", k % 5, 0, 1, &s);
        m1 = footprint_mb();
        for (k = 0; k < 60; k++) {
            if (has_emotions(voice)) cvo_voice_set_emotion(v, k % 4 == 3 ? NULL : EMOTIONS[k % 4 % 3]);
            speak(v, "The quick brown fox jumps over the lazy dog. 12:30, $5.", k % 7 - 2, k % 3, 1, &s);
        }
        cvo_voice_set_emotion(v, NULL);
        m2 = footprint_mb();
        snprintf(what, sizeof what, "60 more utterances on one voice: footprint %.1f -> %.1f MB", m1, m2);
#if CV_ASAN
        printf("info %s (ASan quarantine; checked by the plain build's memory mode)\n", what);
#else
        check(m2 - m1 < 2.0, what);
#endif
    }

    cvo_voice_close(v);
    free(s.pcm);
    free(a.pcm);
    free(b.pcm);
}

/* the words' phones as the front end chose them ("T AX - M AA 1 - T OW"), space-separated */
static void front_phones(zf1_engine *e, const char *text, char *out, size_t cap)
{
    zf_sentence st;
    out[0] = 0;
    zf1_speak_utf8(e, text, 0);
    while (zf1_next_sentence(e, &st) == 1) {
        int i, k;
        for (i = 0; i < st.nwords; i++) {
            const zf_word *w = &st.words[i];
            if (w->nprons <= 0 || w->cur_pron < 0) continue;
            for (k = 0; w->prons[w->cur_pron][k]; k++) {
                const char *nm = zf1_phone_name(&e->ps, w->prons[w->cur_pron][k]);
                snprintf(out + strlen(out), cap - strlen(out), "%s%s", out[0] ? " " : "", nm ? nm : "?");
            }
            snprintf(out + strlen(out), cap - strlen(out), " |");
        }
    }
}

/* ClassicVoices locale patch: the phone set zf2 now reads from the language data is, for en-US, exactly the built-in
 * one it used before; en-GB gets its own (no -SP-, stress mark id 4); the en-GB front end says British words */
static void locale_checks(const char *dir)
{
    char dat[1200], vp[1200], got[1024], what[1400];
    static const struct { const char *text, *want; } gb[] = {
        {"tomato", "-SIL- | T AX - M AA 1 - T OW |"},
        {"schedule", "-SIL- | SH EH 1 - D J UW L |"},
        {"aluminium", "-SIL- | AE 2 - L J UH - M IH 1 - N IH - AX M |"},
        {"Leicester", "-SIL- | L EH 1 - S T AX |"},
        {"water car", "-SIL- | W OO 1 - T AX | K AA 1 |"},
    };
    size_t i, n;
    printf("== locales\n");
    for (i = 0; i < 2; i++) {
        int lcid = i ? 2057 : 1033;
        zf1_engine *e;
        zf2_voice *v;
        z2_phoneset builtin;
        snprintf(dat, sizeof dat, "%s/MSTTSLoc%s.dat", dir, i ? "EnGB" : "EnUS");
        snprintf(vp, sizeof vp, "%s/%s", dir, i ? "M2057Hazel" : "M1033David");
        e = zf1_open_lcid(dat, lcid);
        v = zf2_voice_load(vp);
        if (!e || !v) { check(0, "open the language data and a voice INI"); zf1_close(e); zf2_voice_free(v); continue; }
        builtin = v->ps;
        {
            const uint8_t *r = zf1_resource(e, 0x29a5584bu, 0x153f1b64u, &n);
            int rc = zf2_voice_set_phoneset(v, r, n), same_set = !memcmp(&builtin, &v->ps, sizeof builtin);
            if (!i) snprintf(what, sizeof what, "en-US: the phone set read from MSTTSLocEnUS.dat is the built-in table, byte for byte (%s)",
                             same_set ? "same" : "DIFFERENT");
            else snprintf(what, sizeof what, "en-GB: its own phone set from MSTTSLocEnGB.dat: -SIL- %d, stress '1' %d, -SP- %d, phone 49 %s",
                          v->ps.sil, v->ps.st1, v->ps.sp, v->ps.name[49]);
            check(rc == 0 && (i ? (!same_set && v->ps.sil == 3 && v->ps.st1 == 4 && v->ps.sp == -1 && !strcmp(v->ps.name[49], "ZH"))
                                : same_set), what);
        }
        if (i)
            for (n = 0; n < sizeof gb / sizeof *gb; n++) {
                front_phones(e, gb[n].text, got, sizeof got);
                snprintf(what, sizeof what, "en-GB front end: %-10s -> %s", gb[n].text, got);
                check(!strcmp(got, gb[n].want), what);
            }
        else {
            front_phones(e, "tomato water", got, sizeof got);
            snprintf(what, sizeof what, "en-US front end unchanged: tomato water -> %s", got);
            check(!strcmp(got, "-SIL- | T AX - M EY 1 - T OW | W AO 1 - T AX R |"), what);
        }
        zf1_close(e);
        zf2_voice_free(v);
    }
}

static int eva_checks(const char *dir, const char *od);

/* the flat-memory check of selftest step 9 for every voice, in a build without sanitizers: after a warm-up pass,
 * the same 60 utterances again (rates, pitches, emotions) must not grow the process footprint by 2 MB */
static int memory_check(const char *dir)
{
    int i, bad = 0;
    for (i = 0; i < NVOICES; i++) {
        char err[256], what[256];
        cvo_voice *v = cvo_voice_open(dir, VOICES[i], err, sizeof err);
        sink s;
        double m1, m2;
        int k;
        memset(&s, 0, sizeof s);
        if (!v) { printf("FAIL open %s: %s\n", VOICES[i], err); bad++; continue; }
        for (k = 0; k < 60; k++) {   /* warm-up: the same 60 utterances once (allocator pools, emotions) */
            if (has_emotions(VOICES[i])) cvo_voice_set_emotion(v, k % 4 == 3 ? NULL : EMOTIONS[k % 4 % 3]);
            speak(v, "The quick brown fox jumps over the lazy dog. 12:30, $5.", k % 7 - 2, k % 3, 1, &s);
        }
        m1 = footprint_mb();
        for (k = 0; k < 60; k++) {
            if (has_emotions(VOICES[i])) cvo_voice_set_emotion(v, k % 4 == 3 ? NULL : EMOTIONS[k % 4 % 3]);
            speak(v, "The quick brown fox jumps over the lazy dog. 12:30, $5.", k % 7 - 2, k % 3, 1, &s);
        }
        m2 = footprint_mb();
        snprintf(what, sizeof what, "%-6s 60 more utterances on one voice: footprint %.1f -> %.1f MB", VOICES[i], m1, m2);
        printf("%s %s\n", m2 - m1 < 2.0 ? "ok  " : "FAIL", what);
        if (!(m2 - m1 < 2.0)) bad++;
        cvo_voice_close(v);
        free(s.pcm);
    }
    printf(bad ? "%d FAILURES\n" : "all passed\n", bad);
    return bad != 0;
}

static int selftest(const char *dir, const char *od)
{
    char err[256], what[512];
    int i;
    {
        cvo_voice *bad = cvo_voice_open("/nonexistent", "David", err, sizeof err);
        snprintf(what, sizeof what, "missing data fails cleanly: \"%s\"", err);
        check(bad == NULL && err[0], what);
        err[0] = 0;
        bad = cvo_voice_open(dir, "Cortana", err, sizeof err);
        snprintf(what, sizeof what, "unknown voice fails cleanly: \"%s\"", err);
        check(bad == NULL && err[0], what);
        err[0] = 0;
        bad = cvo_voice_open(NULL, "David", err, sizeof err);
        snprintf(what, sizeof what, "no data folder fails cleanly (never the Windows default): \"%s\"", err);
        check(bad == NULL && err[0], what);
    }
    locale_checks(dir);
    for (i = 0; i < NVOICES; i++) selftest_voice(dir, VOICES[i], od);
    eva_checks(dir, od);
    printf("== threads\n");
    run_threads(dir, 4);
    printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
    return failures != 0;
}

/* ------------------------------------------------------------------------------------------------ Eva */

#include "eva_crf.h"
static long raw_models(const char *dir, const char *text, int models, int16_t **pcm);
static int on_raw(const int16_t *p, size_t n, void *u);
typedef struct { int16_t *p; size_t n, cap; } rbuf;
static int on_raw(const int16_t *p, size_t n, void *u)
{
    rbuf *r = u;
    if (r->n + n > r->cap) { r->cap = (r->n + n) * 2 + 16384; r->p = realloc(r->p, r->cap * 2); }
    memcpy(r->p + r->n, p, n * 2);
    r->n += n;
    return 0;
}
static long raw_models(const char *dir, const char *text, int models, int16_t **pcm)
{
    char err[256];
    rbuf r = {0};
    zira_callbacks cb;
    zira_tts *t = zira_tts_open(dir, "Eva", err, sizeof err);
    if (!t) return -1;
    zira_tts_eva_debug(t, models, 0);
    memset(&cb, 0, sizeof cb);
    cb.audio = on_raw;
    cb.user = &r;
    zira_tts_speak(t, text, 0, &cb);
    zira_tts_close(t);
    *pcm = r.p;
    return (long)r.n;
}

/* Eva's own checks: her feature dictionaries, models vs rules, the long paragraph (first audio, memory) */
static int eva_checks(const char *dir, const char *od)
{
    char what[512], path[1024];
    static const char *const ext[3] = {"BR2", "TON", "ACL"};
    int k;
    printf("== Eva\n");
    for (k = 0; k < 3; k++) {
        char p[1200], err[256];
        long count = 0, bad;
        EvaCrf *m;
        snprintf(p, sizeof p, "%s/M1033Eva.%s", dir, ext[k]);
        m = eva_crf_load(p, err, sizeof err);
        if (!m) { check(0, err); continue; }
        bad = eva_crf_selfcheck(m, &count);
        snprintf(what, sizeof what, "%s: %ld feature strings, each one's trie rank = its weight row (%ld wrong), %d labels x %d templates",
                 ext[k], count, bad, eva_crf_nlabels(m), eva_crf_ntemplates(m));
        check(bad == 0 && count == eva_crf_nfeatures(m), what);
        eva_crf_free(m);
    }
    {   /* models vs rules: the same text, prosody from the CRF models (default) or the rule code */
        static const struct { const char *name, *text; } ab[] = {
            {"first", "Hello, my name is Eva."},
            {"weather", "The weather in Seattle will be rainy tomorrow, with a high of fifty two degrees. Would you like me to set a reminder?"},
        };
        size_t i;
        for (i = 0; i < sizeof ab / sizeof *ab; i++) {
            int16_t *a = NULL, *b = NULL;
            long na = raw_models(dir, ab[i].text, 1, &a), nb = raw_models(dir, ab[i].text, 0, &b);
            snprintf(path, sizeof path, "%s/eva_prosody_%s.wav", od, ab[i].name);
            write_wav(path, a, (size_t)(na > 0 ? na : 0), 16000);
            snprintf(path, sizeof path, "%s/eva_rules_%s.wav", od, ab[i].name);
            write_wav(path, b, (size_t)(nb > 0 ? nb : 0), 16000);
            snprintf(what, sizeof what, "%s: models %.2f s, rules %.2f s, different prosody", ab[i].name, na / 16000.0, nb / 16000.0);
            check(na > 16000 && nb > 16000 && (na != nb || memcmp(a, b, (size_t)na * 2)), what);
            free(a); free(b);
        }
    }
    {   /* a long paragraph as one request: first audio comes after the first sentence; memory */
        static const char *para =
            "Good morning. Before you head out, here is what your day looks like. You have a dentist appointment at ten, "
            "lunch with Sarah at noon, and a team meeting in the afternoon. The shopping list still has milk, eggs, bread, "
            "and coffee on it. Traffic on the bridge is heavier than usual, so you may want to leave a few minutes early. "
            "Do you want me to call a car for you?";
        char *runon = malloc(20000);
        char err[256];
        double m0 = footprint_mb(), t0;
        cvo_voice *v = cvo_voice_open(dir, "Eva", err, sizeof err);
        sink s = {0};
        int rc, i;
        double m1 = footprint_mb();
        t0 = now_s();
        rc = speak(v, para, 0, 0, 1, &s);
        snprintf(path, sizeof path, "%s/eva_paragraph.wav", od);
        write_wav(path, s.pcm, s.n, 22050);
        snprintf(what, sizeof what, "paragraph (6 sentences): rc=%d, %.1f s audio, first audio %.0f ms, all in %.0f ms; footprint: open +%.1f MB, after +%.1f MB",
                 rc, s.n / 22050.0, (s.t_first - t0) * 1000, (now_s() - t0) * 1000, m1 - m0, footprint_mb() - m0);
        check(rc == 0 && s.n > 22050 * 10, what);
        /* one run-on "sentence" of ~300 words with no punctuation: the engine synthesises a sentence at a time */
        runon[0] = 0;
        for (i = 0; i < 30; i++) strcat(runon, "and then we walked along the river past the old mill towards the town ");
        strcat(runon, "at last.");
        t0 = now_s();
        rc = speak(v, runon, 0, 0, 1, &s);
        snprintf(what, sizeof what, "run-on sentence (%zu chars, no punctuation): rc=%d, %.1f s audio, first audio %.0f ms, all in %.0f ms, footprint +%.1f MB",
                 strlen(runon), rc, s.n / 22050.0, (s.t_first - t0) * 1000, (now_s() - t0) * 1000, footprint_mb() - m0);
        check(rc == 0 && s.n > 22050 * 30, what);
        cvo_voice_close(v);
        free(s.pcm);
        free(runon);
    }
    return failures;
}

/* ------------------------------------------------------------------------------------------------ bench */

static int bench(const char *dir, double secs)
{
    const char *text = "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.";
    int vi;
    for (vi = 0; vi < NVOICES; vi++) {
        char err[256];
        double m0 = footprint_mb(), t0 = now_s(), t_open, audio = 0, cpu = 0, m1, m2;
        cvo_voice *v = cvo_voice_open(dir, VOICES[vi], err, sizeof err), *v2;
        sink s = {0};
        int n = 0;
        if (!v) {
            printf("open failed: %s\n", err);
            return 1;
        }
        t_open = now_s() - t0;
        m1 = footprint_mb();
        t0 = now_s();
        speak(v, text, 0, 0, 1, &s);
        printf("%-6s open %.1f ms, footprint +%.1f MB; first utterance: first audio %.1f ms after the call, %.2f s audio in %.1f ms\n",
               VOICES[vi], t_open * 1000, m1 - m0, (s.t_first - t0) * 1000, s.n / 22050.0, (now_s() - t0) * 1000);
        t0 = now_s();
        speak(v, "5", 0, 0, 1, &s);
        printf("      a lone digit: first audio %.1f ms, %.2f s in %.1f ms\n", (s.t_first - t0) * 1000, s.n / 22050.0,
               (now_s() - t0) * 1000);
        while (cpu < secs) {
            t0 = now_s();
            speak(v, text, 0, 0, 1, &s);
            cpu += now_s() - t0;
            audio += s.n / 22050.0;
            n++;
        }
        printf("      warm: %d utterances, %.1f s of audio in %.2f s = %.1fx real time\n", n, audio, cpu, audio / cpu);
        m1 = footprint_mb();
        v2 = cvo_voice_open(dir, VOICES[vi], err, sizeof err);
        speak(v2, text, 0, 0, 1, &s);
        m2 = footprint_mb();
        printf("      a second open voice (after one utterance): footprint +%.1f MB, process %.1f MB\n", m2 - m1, m2);
        cvo_voice_close(v2);
        cvo_voice_close(v);
        free(s.pcm);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 8 && !strcmp(argv[2], "say")) {
        char err[256], voice[32], *colon;
        cvo_voice *v;
        sink s = {0};
        int rc;
        double t0 = now_s();
        snprintf(voice, sizeof voice, "%s", argv[3]);
        colon = strchr(voice, ':');
        if (colon) *colon++ = 0;
        v = cvo_voice_open(argv[1], voice, err, sizeof err);
        if (!v) {
            fprintf(stderr, "open failed: %s\n", err);
            return 2;
        }
        if (colon && cvo_voice_set_emotion(v, colon)) fprintf(stderr, "no emotion %s\n", colon);
        rc = speak(v, argv[6], atof(argv[4]), atof(argv[5]), !(argc > 8 && !strcmp(argv[8], "notrim")), &s);
        write_wav(argv[7], s.pcm, s.n, 22050);
        printf("rc=%d samples=%zu (%.2f s) in %.0f ms\n", rc, s.n, s.n / 22050.0, (now_s() - t0) * 1000);
        cvo_voice_close(v);
        return rc < 0;
    }
    if (argc >= 4 && !strcmp(argv[2], "selftest")) return selftest(argv[1], argv[3]);
    if (argc >= 3 && !strcmp(argv[2], "memory")) return memory_check(argv[1]);
    if (argc >= 3 && !strcmp(argv[2], "threads")) {
        run_threads(argv[1], argc >= 4 ? atoi(argv[3]) : 3);
        printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
        return failures != 0;
    }
    if (argc >= 3 && !strcmp(argv[2], "bench")) return bench(argv[1], argc >= 4 ? atof(argv[3]) : 4);
    if (argc >= 4 && !strcmp(argv[2], "eva")) {
        eva_checks(argv[1], argv[3]);
        printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
        return failures != 0;
    }
    fprintf(stderr, "usage: see the comment at the top of onecore_test.c\n");
    return 1;
}
