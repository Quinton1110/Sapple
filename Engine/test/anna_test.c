/* Mac test driver for Microsoft Anna: the engine (Engine/anna) and its bridge (Shared/Bridge/cva_bridge.c) exactly
 * as the iOS targets compile them, driven the way ClassicEngine.swift drives them from the extension.
 *
 *   anna_test DATA_DIR say RATE SEMITONES "text" out.wav [notrim]
 *   anna_test DATA_DIR selftest OUTDIR          every check below; exit status = failures
 *   anna_test DATA_DIR threads                  two voices on two threads (run it under TSan: make anna-tsan)
 *   anna_test DATA_DIR bench [seconds]          open time, first-audio latency, real-time factor, memory
 */
#include "anna_tts.h"
#include "cva_bridge.h"

#include <mach/mach.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

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

static int speak(cva_voice *v, const char *text, double rate, double semi, int trim, sink *s)
{
    s->n = 0;
    s->t_first = s->t_stop = 0;
    return cva_voice_speak(v, text, rate, semi, trim, on_pcm, s);
}

/* the engine alone (no bridge, no sanitizer), 16 kHz */
typedef struct {
    size_t n;
} counter;
static int count_pcm(const int16_t *pcm, size_t n, void *user)
{
    (void)pcm;
    ((counter *)user)->n += n;
    return 0;
}
static int raw_engine(const char *dir, const char *text, int flags, size_t *samples)
{
    char err[256];
    anna_tts *t = anna_tts_open(dir, err, sizeof err);
    anna_callbacks cb;
    counter c = {0};
    int rc;
    *samples = 0;
    if (!t) return -2;
    cb.audio = count_pcm;
    cb.event = NULL;
    cb.user = &c;
    rc = anna_tts_speak(t, text, flags, &cb);
    anna_tts_close(t);
    *samples = c.n;
    return rc;
}

/* raw 16 kHz PCM of the engine alone at a rate given as int or as double (patch check) */
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
static void raw_render(const char *dir, const char *text, double rate, int use_f, rawbuf *b)
{
    char err[256];
    anna_tts *t = anna_tts_open(dir, err, sizeof err);
    anna_callbacks cb = {raw_collect, NULL, b};
    b->n = 0;
    if (!t) return;
    if (use_f) anna_tts_set_rate_f(t, rate);
    else anna_tts_set_rate(t, (int)rate);
    anna_tts_speak(t, text, 0, &cb);
    anna_tts_close(t);
}

static int same(const sink *a, const sink *b) { return a->n == b->n && !memcmp(a->pcm, b->pcm, a->n * 2); }

/* ------------------------------------------------------------------------------------------------ threads */

typedef struct {
    const char *dir;
    int reps, ok;
    const char *text;
    size_t first_n;
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
    cva_voice *v = cva_voice_open(a->dir, err, sizeof err);
    sink s = {0};
    int i;
    a->ok = v != NULL;
    a->hash = 1469598103934665603ull;
    for (i = 0; v && i < a->reps; i++) {
        if (speak(v, a->text, i % 3 == 2 ? 6.5 : 0, 0, 1, &s) != 0) a->ok = 0;
        if (i == 0) a->first_n = s.n;
        a->hash ^= fnv(s.pcm, s.n) + (unsigned long long)i;
    }
    cva_voice_close(v);
    free(s.pcm);
    return NULL;
}

static int run_threads(const char *dir, int reps)
{
    const char *ta = "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.";
    const char *tb = "On March 3rd, 2021 at 10:30 am, 1,234 people paid $56.78 each. Is that right?";
    thr_arg single[2] = {{dir, reps, 0, ta, 0, 0}, {dir, reps, 0, tb, 0, 0}}, par[2];
    pthread_t th[2];
    char what[256];
    int i;
    /* reference: each sequence alone, one after the other, fresh voices */
    thr_main(&single[0]);
    thr_main(&single[1]);
    memcpy(par, single, sizeof par);
    for (i = 0; i < 2; i++) pthread_create(&th[i], NULL, thr_main, &par[i]);
    for (i = 0; i < 2; i++) pthread_join(th[i], NULL);
    snprintf(what, sizeof what, "two voices on two threads, %d utterances each: identical to each alone (%s / %s)", reps,
             par[0].hash == single[0].hash ? "same" : "DIFFERENT", par[1].hash == single[1].hash ? "same" : "DIFFERENT");
    check(par[0].ok && par[1].ok && par[0].hash == single[0].hash && par[1].hash == single[1].hash, what);
    return failures;
}

/* ------------------------------------------------------------------------------------------------ selftest */

static int selftest(const char *dir, const char *od)
{
    char err[256], what[512], path[1024];
    cva_voice *v;
    sink s = {0}, a = {0}, b = {0};
    size_t base, i;
    int rc;
    double t0 = now_s(), mem0 = footprint_mb();
    const char *tail = "This sentence must still be spoken.";

    v = cva_voice_open(dir, err, sizeof err);
    snprintf(what, sizeof what, "open the voice (%.1f ms, footprint +%.1f MB)", (now_s() - t0) * 1000, footprint_mb() - mem0);
    check(v != NULL, what);
    if (!v) {
        printf("open failed: %s\n", err);
        return 1;
    }
    {
        cva_voice *bad = cva_voice_open("/nonexistent", err, sizeof err);
        snprintf(what, sizeof what, "missing data fails cleanly: \"%s\"", err);
        check(bad == NULL && err[0], what);
    }

    /* 1. sentences, numbers, dates, punctuation: they speak, and the WAVs go to transcription */
    {
        static const struct {
            const char *name, *text;
        } texts[] = {
            {"hello", "Hello, my name is Microsoft Anna."},
            {"fox", "The quick brown fox jumps over the lazy dog, and then it runs away into the forest."},
            {"numbers", "I counted 1,234 apples, 56 pears and 7.5 kilograms of grapes; that is 3 times more than last year."},
            {"dates", "The meeting moved from March 3rd, 2021 to 12/25/2024 at 10:30 am, and it costs $56.78."},
            {"punct", "Wait... really? Yes! Call 555-1234, or email me: it's (almost) done - \"quoted\" and 'single'."},
            {"battery", "65% battery power, charging."},
            {"abbrev", "Dr. Smith lives on Elm St. near the U.S. Post Office, e.g. at No. 5."},
            {"long", "Alice was beginning to get very tired of sitting by her sister on the bank, and of having nothing to "
                     "do: once or twice she had peeped into the book her sister was reading, but it had no pictures or "
                     "conversations in it, and what is the use of a book, thought Alice, without pictures or conversations?"},
        };
        for (i = 0; i < sizeof texts / sizeof *texts; i++) {
            double t1 = now_s();
            rc = speak(v, texts[i].text, 0, 0, 1, &s);
            snprintf(path, sizeof path, "%s/anna_%s.wav", od, texts[i].name);
            write_wav(path, s.pcm, s.n, 22050);
            snprintf(what, sizeof what, "%-8s rc=%d %.2f s audio in %.0f ms (first audio %.0f ms) -> %s", texts[i].name, rc,
                     s.n / 22050.0, (now_s() - t1) * 1000, s.t_first ? (s.t_first - t1) * 1000 : -1, path);
            check(rc == 0 && s.n > 22050 / 2, what);
        }
    }

    /* 2. hostile text: the sentence after it must still be spoken */
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
            snprintf(path, sizeof path, "%s/anna_hostile%zu.wav", od, i);
            write_wav(path, s.pcm, s.n, 22050);
            snprintf(what, sizeof what, "hostile %2zu: rc=%d, %.2f s (the tail alone %.2f s)", i, rc, s.n / 22050.0, base / 22050.0);
            check(rc == 0 && s.n >= base, what);
        }
        /* a very long text of long words in one call */
        {
            size_t L = 20000, k;
            char *big = malloc(L + 64);
            for (k = 0; k < L; k++) big[k] = (k % 97 == 96) ? ' ' : (char)('a' + (k * 7) % 26);
            strcpy(big + L, ". ");
            strcat(big, tail);
            rc = speak(v, big, 12, 0, 1, &s);
            snprintf(what, sizeof what, "20,000 letters in words of 96, at rate 12: rc=%d, %.1f s", rc, s.n / 22050.0);
            check(rc == 0 && s.n > base, what);
            free(big);
        }
    }
    /* the engine alone (no sanitizer): does anything make it stop before the end? */
    {
        size_t with = 0, without = 0;
        int r1 = raw_engine(dir, "Hello. \xC3\x9F\xC3\x9F \xE4\xBD\xA0 \xF0\x9F\x98\x80 \x01 \\\\. Must speak. Also this.", 0, &with);
        int r2 = raw_engine(dir, "Hello. Must speak. Also this.", 0, &without);
        snprintf(what, sizeof what, "engine alone with unsanitized symbols: rc %d/%d, %zu samples with, %zu without", r1, r2, with, without);
        check(r1 == 0 && r2 == 0 && with >= without, what);
    }
    rc = speak(v, "", 0, 0, 1, &s);
    check(rc == 0 && s.n == 0, "empty text gives no audio and no stand-in text");
    rc = speak(v, " \xF0\x9F\x98\x80 \xE2\x80\x8B ", 0, 0, 1, &s);
    check(rc == 0 && s.n == 0, "emoji-only text gives no audio");

    /* 3. rate: the engine's own control, fractional, monotonic; whole rates exactly the original engine's */
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
                snprintf(path, sizeof path, "%s/anna_rate%+g.wav", od, rates[i]);
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
        for (r = -10; r <= 18; r += 7) {
            raw_render(dir, tail, r, 0, &r1);
            raw_render(dir, tail, r, 1, &r2);
            if (r1.n == 0 || r1.n != r2.n || memcmp(r1.p, r2.p, r1.n * 2)) ok = 0;
        }
        check(ok, "patch: anna_tts_set_rate_f(n) renders bit-identical to the original anna_tts_set_rate(n) (-10, -3, 4, 11, 18)");
        free(r1.p);
        free(r2.p);
    }

    /* 4. pitch: semitones onto the engine's half-semitone steps, +-5 semitones */
    {
        size_t n0, nu, nd;
        int r1, r2, r3;
        r1 = speak(v, tail, 0, 0, 1, &s);
        n0 = s.n;
        r2 = speak(v, tail, 0, 5, 1, &a);
        nu = a.n;
        r3 = speak(v, tail, 0, -12, 1, &b);
        nd = b.n;
        snprintf(path, sizeof path, "%s/anna_pitch_up5.wav", od);
        write_wav(path, a.pcm, a.n, 22050);
        snprintf(path, sizeof path, "%s/anna_pitch_down12.wav", od);
        write_wav(path, b.pcm, b.n, 22050);
        snprintf(what, sizeof what, "pitch 0 / +5 / -12 semitones (steps %d %d %d): rc %d %d %d, %.2f %.2f %.2f s, all different",
                 cva_pitch_for_semitones(0), cva_pitch_for_semitones(5), cva_pitch_for_semitones(-12), r1, r2, r3,
                 n0 / 22050.0, nu / 22050.0, nd / 22050.0);
        check(r1 == 0 && r2 == 0 && r3 == 0 && !same(&s, &a) && !same(&s, &b) && !same(&a, &b) &&
                  cva_pitch_for_semitones(-12) == -10 && cva_pitch_for_semitones(2.6) == 5,
              what);
    }

    /* 5. the trim: a lone digit */
    {
        size_t raw = 0;
        raw_engine(dir, "5", 0, &raw);
        speak(v, "5", 0, 0, 1, &s);
        snprintf(what, sizeof what, "digit trim: %.0f ms from the engine -> %.0f ms", raw / 16.0, s.n / 22.05);
        check(s.n > 0 && s.n / 22.05 < raw / 16.0 - 300, what);
        snprintf(path, sizeof path, "%s/anna_digit.wav", od);
        write_wav(path, s.pcm, s.n, 22050);
    }

    /* 6. cancel mid-utterance, then speak again on the same voice */
    {
        char alice[4000] = "";
        for (i = 0; i < 20; i++) strcat(alice, "Alice was beginning to get very tired of sitting by her sister. ");
        s.stop_after = 22050;
        rc = speak(v, alice, 0, 0, 1, &s);
        s.stop_after = 0;
        snprintf(what, sizeof what, "cancel after 1 s of audio: rc=%d, %.2f s delivered, stopped %.1f ms after asking", rc,
                 s.n / 22050.0, (now_s() - s.t_stop) * 1000);
        check(rc == 1 && s.n < 22050 * 2, what);
        rc = speak(v, tail, 0, 0, 1, &s);
        snprintf(what, sizeof what, "the next utterance after a cancel: rc=%d, %.2f s (fresh voice %.2f s)", rc, s.n / 22050.0, base / 22050.0);
        check(rc == 0 && s.n > base / 2, what);
    }

    /* 7. state between utterances: a fresh voice is deterministic; a warm one is not (msvcrt rand() for the
     *    accents runs on from utterance to utterance, as in the original engine) */
    {
        cva_voice *f1 = cva_voice_open(dir, err, sizeof err), *f2 = cva_voice_open(dir, err, sizeof err);
        const char *t = "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.";
        int k, differ = 0;
        speak(f1, t, 0, 0, 1, &a);
        speak(f2, t, 0, 0, 1, &b);
        check(same(&a, &b), "two fresh voices render the same text bit-identically");
        for (k = 0; k < 5; k++) {
            speak(f1, t, 0, 0, 1, &b);
            if (!same(&a, &b)) differ++;
        }
        snprintf(what, sizeof what, "a warm voice carries state: %d of the next 5 renders of the same text differ from the first "
                 "(same length: %s)", differ, a.n == b.n ? "yes" : "no");
        printf("info %s\n", what);
        cva_voice_close(f1);
        cva_voice_close(f2);
    }

    /* 8. many utterances on one voice: memory stays flat */
    {
        double m1, m2;
        int k;
        for (k = 0; k < 10; k++) speak(v, "The quick brown fox jumps over the lazy dog.", k % 5, 0, 1, &s);
        m1 = footprint_mb();
        for (k = 0; k < 60; k++) speak(v, "The quick brown fox jumps over the lazy dog. 12:30, $5.", k % 7 - 2, k % 3, 1, &s);
        m2 = footprint_mb();
        snprintf(what, sizeof what, "60 more utterances on one voice: footprint %.1f -> %.1f MB", m1, m2);
        check(m2 - m1 < 2.0, what);
    }

    cva_voice_close(v);
    free(s.pcm);
    free(a.pcm);
    free(b.pcm);
    run_threads(dir, 4);
    printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
    return failures != 0;
}

/* ------------------------------------------------------------------------------------------------ bench */

static int bench(const char *dir, double secs)
{
    const char *text = "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.";
    char err[256];
    double m0 = footprint_mb(), t0 = now_s(), t_open, audio = 0, cpu = 0, m1, m2;
    cva_voice *v = cva_voice_open(dir, err, sizeof err), *v2;
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
    printf("open %.1f ms, footprint +%.1f MB; first utterance: first audio %.1f ms after the call, %.2f s audio in %.1f ms\n",
           t_open * 1000, m1 - m0, (s.t_first - t0) * 1000, s.n / 22050.0, (now_s() - t0) * 1000);
    t0 = now_s();
    speak(v, "5", 0, 0, 1, &s);
    printf("a lone digit: first audio %.1f ms, %.2f s in %.1f ms\n", (s.t_first - t0) * 1000, s.n / 22050.0, (now_s() - t0) * 1000);
    while (cpu < secs) {
        t0 = now_s();
        speak(v, text, 0, 0, 1, &s);
        cpu += now_s() - t0;
        audio += s.n / 22050.0;
        n++;
    }
    printf("warm: %d utterances, %.1f s of audio in %.2f s = %.1fx real time\n", n, audio, cpu, audio / cpu);
    m1 = footprint_mb();
    v2 = cva_voice_open(dir, err, sizeof err);
    speak(v2, text, 0, 0, 1, &s);
    m2 = footprint_mb();
    printf("a second open voice (after one utterance): footprint +%.1f MB, process %.1f MB\n", m2 - m1, m2);
    cva_voice_close(v2);
    cva_voice_close(v);
    free(s.pcm);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 7 && !strcmp(argv[2], "say")) {
        char err[256];
        cva_voice *v = cva_voice_open(argv[1], err, sizeof err);
        sink s = {0};
        int rc;
        double t0 = now_s();
        if (!v) {
            fprintf(stderr, "open failed: %s\n", err);
            return 2;
        }
        rc = speak(v, argv[5], atof(argv[3]), atof(argv[4]), !(argc > 7 && !strcmp(argv[7], "notrim")), &s);
        write_wav(argv[6], s.pcm, s.n, 22050);
        printf("rc=%d samples=%zu (%.2f s) in %.0f ms\n", rc, s.n, s.n / 22050.0, (now_s() - t0) * 1000);
        cva_voice_close(v);
        return rc < 0;
    }
    if (argc >= 4 && !strcmp(argv[2], "selftest")) return selftest(argv[1], argv[3]);
    if (argc >= 3 && !strcmp(argv[2], "threads")) {
        run_threads(argv[1], argc >= 4 ? atoi(argv[3]) : 3);
        printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
        return failures != 0;
    }
    if (argc >= 3 && !strcmp(argv[2], "bench")) return bench(argv[1], argc >= 4 ? atof(argv[3]) : 4);
    fprintf(stderr, "usage: see the comment at the top of anna_test.c\n");
    return 1;
}
