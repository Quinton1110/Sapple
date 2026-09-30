/* Mac test driver for the neural voices - Jenny, Aria, Guy (en-US), Sonia, Ryan (en-GB), Neerja, Prabhat (en-IN), the Windows 11 natural voices on
 * Microsoft's embedded Speech SDK - through their bridge (Shared/Bridge/cvn_bridge.c) exactly as the iOS targets compile
 * it, driven the way ClassicEngine.swift drives it. The SDK is NeuralSDK/macos (the iOS dylibs re-tagged for macOS by
 * tools/neural_stage.py): the same arm64 code the phone runs.
 *
 *   neural_test ROOT say VOICE RATE SEMITONES "text" out.wav [notrim]
 *   neural_test ROOT selftest OUTDIR      every check below; exit status = failures
 *   neural_test ROOT threads [reps]       two voices on two threads (run it under TSan: make neural-tsan)
 *   neural_test ROOT bench                open, first audio, real-time factor, memory
 * ROOT = the repo root (NeuralSDK/macos, NeuralVoices, OneCoreVoice); the voices' work folders go to build/neural-work.
 */
#include "cvn_bridge.h"

#include <mach/mach.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define NV 7
static const char *const VOICES[NV] = {"Jenny", "Aria", "Guy", "Sonia", "Ryan", "Neerja", "Prabhat"};
static char SDK[1024], DATA[1024], ONECORE[1024], WORK[1024];
static int failures;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

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
    size_t stop_after; /* 0 = never */
    double first_at, t0;
} sink;

static int on_pcm(const int16_t *pcm, size_t n, void *user)
{
    sink *s = user;
    if (!s->n && s->t0 > 0) s->first_at = now_s() - s->t0;
    if (s->n + n > s->cap) {
        size_t c = s->cap ? s->cap * 2 : 1 << 16;
        while (c < s->n + n) c *= 2;
        s->pcm = realloc(s->pcm, c * sizeof *s->pcm);
        s->cap = c;
    }
    memcpy(s->pcm + s->n, pcm, n * sizeof *pcm);
    s->n += n;
    return s->stop_after && s->n >= s->stop_after;
}

static void write_wav(const char *path, const int16_t *pcm, size_t n, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    uint32_t u;
    uint16_t h;
    if (!f) return;
    fwrite("RIFF", 1, 4, f);
    u = 36 + (uint32_t)n * 2;
    fwrite(&u, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    u = 16;
    fwrite(&u, 4, 1, f);
    h = 1;
    fwrite(&h, 2, 1, f);
    fwrite(&h, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    u = rate * 2;
    fwrite(&u, 4, 1, f);
    h = 2;
    fwrite(&h, 2, 1, f);
    h = 16;
    fwrite(&h, 2, 1, f);
    fwrite("data", 1, 4, f);
    u = (uint32_t)n * 2;
    fwrite(&u, 4, 1, f);
    fwrite(pcm, 2, n, f);
    fclose(f);
}

static cvn_voice *open_voice(const char *name)
{
    char err[256] = "";
    cvn_voice *v = cvn_voice_open(SDK, DATA, ONECORE, WORK, name, err, sizeof err);
    if (!v) printf("     (open %s: %s)\n", name, err);
    return v;
}

static int say(cvn_voice *v, const char *text, double rate, double st, sink *s)
{
    memset(s, 0, sizeof *s);
    s->t0 = now_s();
    return cvn_voice_speak(v, text, rate, st, 1, on_pcm, s);
}

static int same(const sink *a, const sink *b)
{
    return a->n > 0 && a->n == b->n && !memcmp(a->pcm, b->pcm, a->n * 2);
}

/* median F0 of the loud 40 ms frames, autocorrelation (70..400 Hz) */
static double median_f0(const sink *s)
{
    const int fl = 882, lo = 22050 / 400, hi = 22050 / 70;
    double f[4096];
    int nf = 0, i, k, j;
    for (i = 0; i + fl + hi < (int)s->n && nf < 4096; i += fl / 2) {
        double e = 0, best = 0, mean = 0;
        int bk = 0;
        for (j = 0; j < fl; j++) mean += s->pcm[i + j];
        mean /= fl;
        for (j = 0; j < fl; j++) e += (s->pcm[i + j] - mean) * (s->pcm[i + j] - mean);
        if (sqrt(e / fl) < 500) continue;
        for (k = lo; k < hi; k++) {
            double c = 0;
            for (j = 0; j < fl; j++) c += (s->pcm[i + j] - mean) * (s->pcm[i + j + k] - mean);
            if (c > best) best = c, bk = k;
        }
        if (bk && best > 0.4 * e) f[nf++] = 22050.0 / bk;
    }
    if (!nf) return 0;
    for (i = 1; i < nf; i++)
        for (j = i; j > 0 && f[j - 1] > f[j]; j--) {
            double t = f[j];
            f[j] = f[j - 1];
            f[j - 1] = t;
        }
    return f[nf / 2];
}

static void selftest(const char *out)
{
    char what[512], path[2048];
    sink a, b, c;
    cvn_voice *v[NV];
    int i, rc;
    double t0, mem0;
    static const char *const hostile[] = {
        "", "   ", "\xF0\x9F\x98\x80\xF0\x9F\x8E\x89", "<speak><break time='5s'/>not markup</speak>", "a & b < c > d \" e ' f",
        "\x01\x02\x03 control \x1b[31m escapes", "1234567890123456789012345678901234567890",
        "5555-5555-5555-5555-5555", "aaaa1111aaaa1111aaaa1111aaaa1111", "Caf\xC3\xA9 na\xC3\xAFve \xE2\x82\xAC 5 \xC2\xB0" "C",
        "\xE4\xBD\xA0\xE5\xA5\xBD world", "\xff\xfe invalid utf8 \xc3", "...,,,;;;!!!???", NULL};

    check(fabs(cvn_speed_factor(0) - 1) < 1e-9 && fabs(cvn_speed_factor(-10) - 1 / 3.0) < 1e-9 &&
              fabs(cvn_speed_factor(10) - 3) < 1e-9 && cvn_speed_factor(18) == cvn_speed_factor(10),
          "speed: 3^(rate/10), 1/3x .. 3x");

    /* bad setups fail cleanly */
    {
        char err[256] = "";
        check(!cvn_voice_open(SDK, "/nonexistent", ONECORE, WORK, "Jenny", err, sizeof err) && err[0],
              "missing data folder: open fails with a reason");
        check(!cvn_voice_open(SDK, DATA, ONECORE, WORK, "Zira", err, sizeof err), "unknown voice: open fails");
    }

    /* every voice speaks */
    for (i = 0; i < NV; i++) {
        char text[256];
        mem0 = footprint_mb();
        t0 = now_s();
        v[i] = open_voice(VOICES[i]);
        snprintf(what, sizeof what, "open %s (%.1f ms)", VOICES[i], (now_s() - t0) * 1000);
        check(v[i] != NULL, what);
        if (!v[i]) return;
        snprintf(text, sizeof text, "Hello, my name is %s. The quick brown fox jumps over the lazy dog.", VOICES[i]);
        rc = say(v[i], text, 0, 0, &a);
        snprintf(what, sizeof what, "%s speaks: rc %d, %.2f s, first audio %.0f ms, footprint +%.1f MB", VOICES[i], rc,
                 a.n / 22050.0, a.first_at * 1000, footprint_mb() - mem0);
        check(rc == 0 && a.n > 22050 * 2 && a.n < 22050 * 8, what);
        snprintf(path, sizeof path, "%s/%s.wav", out, VOICES[i]);
        write_wav(path, a.pcm, a.n, 22050);
        free(a.pcm);
    }

    /* determinism: warm = warm, and a fresh engine (model reloaded) = warm; another voice in between changes nothing */
    say(v[0], "Is it ready? Yes, it is ready.", 0, 0, &a);
    say(v[0], "Is it ready? Yes, it is ready.", 0, 0, &b);
    check(same(&a, &b), "Jenny twice in a row: identical");
    say(v[3], "Something else entirely, from Sonia.", 0, 0, &c);
    free(c.pcm);
    free(b.pcm);
    say(v[0], "Is it ready? Yes, it is ready.", 0, 0, &b);
    check(same(&a, &b), "Jenny after Sonia spoke in between: identical");
    free(b.pcm);
    cvn_release_engine();
    say(v[0], "Is it ready? Yes, it is ready.", 0, 0, &b);
    check(same(&a, &b), "Jenny from a fresh engine: identical");
    free(a.pcm);
    free(b.pcm);

    /* rate: longer when slower, shorter when faster, clamped at 3.5x; back to 0 = the first render */
    {
        const double rates[] = {-10, -5, 0, 5, 10, 12, 18};
        size_t len[7];
        sink r0;
        say(v[0], "The rain in Spain stays mainly in the plain.", 0, 0, &r0);
        for (i = 0; i < 7; i++) {
            say(v[0], "The rain in Spain stays mainly in the plain.", rates[i], 0, &a);
            len[i] = a.n;
            snprintf(path, sizeof path, "%s/rate_%+.0f.wav", out, rates[i]);
            write_wav(path, a.pcm, a.n, 22050);
            if (rates[i] == 0) {
                check(same(&a, &r0), "rate 0 after other rates = rate 0 before");
            }
            free(a.pcm);
        }
        snprintf(what, sizeof what, "rate -10..18: %.2f %.2f %.2f %.2f %.2f %.2f %.2f s (monotonic; 10, 12 and 18 all 3x)",
                 len[0] / 22050.0, len[1] / 22050.0, len[2] / 22050.0, len[3] / 22050.0, len[4] / 22050.0,
                 len[5] / 22050.0, len[6] / 22050.0);
        check(len[0] > len[1] && len[1] > len[2] && len[2] > len[3] && len[3] > len[4] && len[4] == len[5] &&
                  len[5] == len[6] && len[2] > 2.5 * len[6] && len[0] > 2.5 * len[2],
              what);
        free(r0.pcm);
    }

    /* pitch: up and down, the text still spoken (the SSML path escapes the text) */
    {
        double f[3];
        const double st[3] = {-4, 0, 4};
        for (i = 0; i < 3; i++) {
            say(v[0], "The quick brown fox jumps over the lazy dog.", 0, st[i], &a);
            f[i] = median_f0(&a);
            snprintf(path, sizeof path, "%s/pitch_%+.0f.wav", out, st[i]);
            write_wav(path, a.pcm, a.n, 22050);
            free(a.pcm);
        }
        snprintf(what, sizeof what, "pitch -4 / 0 / +4 semitones: median F0 %.0f / %.0f / %.0f Hz", f[0], f[1], f[2]);
        check(f[0] < f[1] && f[1] < f[2] && f[0] > 0, what);
        rc = say(v[0], "Tom & Jerry <3 \"quotes\" and 'apostrophes'", 0, 2, &a);
        snprintf(what, sizeof what, "pitch path with & < > \" ' in the text: rc %d, %.2f s", rc, a.n / 22050.0);
        check(rc == 0 && a.n > 22050, what);
        free(a.pcm);
    }

    /* hostile text: never a crash, empty in -> no audio, the voice unchanged after */
    for (i = 0; hostile[i]; i++) {
        rc = say(v[1], hostile[i], 0, i % 3 == 0 ? 3 : 0, &a);
        snprintf(what, sizeof what, "hostile text %d: rc %d, %.2f s", i, rc, a.n / 22050.0);
        check(rc == 0 && (i > 2 || a.n == 0), what);
        free(a.pcm);
    }
    {
        char *big = malloc(4001);
        memset(big, 'a', 4000);
        big[4000] = 0;
        t0 = now_s();
        rc = say(v[1], big, 0, 0, &a);
        snprintf(what, sizeof what, "4,000 letters: rc %d, %.1f s of audio in %.2f s", rc, a.n / 22050.0, now_s() - t0);
        check(rc == 0, what);
        free(a.pcm);
        free(big);
    }
    say(v[0], "Is it ready? Yes, it is ready.", 0, 0, &a);
    say(v[0], "Is it ready? Yes, it is ready.", 0, 0, &b);
    check(same(&a, &b), "after the hostile texts Jenny still renders identically");
    free(b.pcm);

    /* cancel: stop after 1 s of a long text; returns promptly; the next utterance is unchanged */
    {
        sink s;
        const char *longtext = "It was the best of times, it was the worst of times, it was the age of wisdom, it was the "
                               "age of foolishness, it was the epoch of belief, it was the epoch of incredulity, it was "
                               "the season of Light, it was the season of Darkness, it was the spring of hope, it was the "
                               "winter of despair.";
        memset(&s, 0, sizeof s);
        s.stop_after = 22050;
        t0 = now_s();
        rc = cvn_voice_speak(v[0], longtext, 0, 0, 1, on_pcm, &s);
        snprintf(what, sizeof what, "cancel after 1 s: rc %d, %.2f s delivered, returned %.0f ms after the start", rc,
                 s.n / 22050.0, (now_s() - t0) * 1000);
        check(rc == 1 && s.n < 22050 * 2, what);
        free(s.pcm);
        say(v[0], "Is it ready? Yes, it is ready.", 0, 0, &b);
        check(same(&a, &b), "the utterance after a cancel: identical");
        free(b.pcm);
        rc = say(v[0], longtext, 0, 0, &b);
        snprintf(what, sizeof what, "streaming: the 14 s paragraph's first audio after %.0f ms (all of it: %.2f s)",
                 b.first_at * 1000, b.n / 22050.0);
        check(rc == 0 && b.first_at < 0.2, what);
        snprintf(path, sizeof path, "%s/paragraph.wav", out);
        write_wav(path, b.pcm, b.n, 22050);
        free(b.pcm);
    }
    free(a.pcm);

    /* memory: every voice in turn, then 100 utterances over every voice, rate and pitch: flat. With seven voices the
     * footprint still settles by 2.5-5.5 MB over the first such round (measured 2026-09-30: then +0.6 MB over 300 more), so
     * one warm-up round comes first and the check is over the second. */
    {
        double m1, m2;
        for (i = 0; i < NV; i++) {
            say(v[i], "Checking memory.", 0, 0, &a);
            free(a.pcm);
        }
        for (i = 0; i < 100; i++) {
            say(v[i % NV], "Checking memory once more, with a slightly longer sentence.", (i % 7) - 3, (i % 3) - 1, &a);
            free(a.pcm);
        }
        m1 = footprint_mb();
        for (i = 0; i < 100; i++) { /* every voice, every rate and pitch in turn */
            say(v[i % NV], "Checking memory once more, with a slightly longer sentence.", (i % 7) - 3, (i % 3) - 1, &a);
            free(a.pcm);
        }
        m2 = footprint_mb();
        snprintf(what, sizeof what, "memory: %.1f MB with all seven voices loaded, %.1f MB after 100 more utterances", m1, m2);
        check(m2 - m1 < 5, what);
    }
    for (i = 0; i < NV; i++) cvn_voice_close(v[i]);
}

typedef struct {
    const char *voice, *text;
    sink s;
    int reps, ok;
} thr;

static void *thread_main(void *p)
{
    thr *t = p;
    cvn_voice *v = open_voice(t->voice);
    int i;
    t->ok = v != NULL;
    for (i = 0; v && i < t->reps; i++) {
        sink s;
        say(v, t->text, 0, 0, &s);
        if (i == 0) t->s = s;
        else {
            if (!same(&s, &t->s)) t->ok = 0;
            free(s.pcm);
        }
    }
    cvn_voice_close(v);
    return NULL;
}

static void threads(int reps)
{
    thr t[2] = {{"Jenny", "Thread one is speaking with Jenny.", {0}, reps, 0},
                {"Ryan", "Thread two is speaking with Ryan, at the same time.", {0}, reps, 0}};
    pthread_t th[2];
    sink alone[2];
    char what[256];
    int i;
    for (i = 0; i < 2; i++) {
        cvn_voice *v = open_voice(t[i].voice);
        if (!v) {
            check(0, "open");
            return;
        }
        say(v, t[i].text, 0, 0, &alone[i]);
        cvn_voice_close(v);
    }
    for (i = 0; i < 2; i++) pthread_create(&th[i], NULL, thread_main, &t[i]);
    for (i = 0; i < 2; i++) pthread_join(th[i], NULL);
    for (i = 0; i < 2; i++) {
        snprintf(what, sizeof what, "%s on its own thread, %d times alongside the other: identical to alone", t[i].voice,
                 reps);
        check(t[i].ok && same(&t[i].s, &alone[i]), what);
    }
}

static void bench(void)
{
    int i;
    for (i = 0; i < NV; i++) {
        sink a;
        double t0 = now_s(), mem0 = footprint_mb(), t1;
        cvn_voice *v = open_voice(VOICES[i]);
        double open_ms = (now_s() - t0) * 1000;
        if (!v) continue;
        cvn_release_engine();
        say(v, "The quick brown fox jumps over the lazy dog.", 0, 0, &a);
        printf("%-6s open %.1f ms, first utterance (model load) first audio %.0f ms, footprint +%.1f MB\n", VOICES[i],
               open_ms, a.first_at * 1000, footprint_mb() - mem0);
        free(a.pcm);
        t1 = now_s();
        say(v, "The quick brown fox jumps over the lazy dog.", 0, 0, &a);
        printf("       warm: first audio %.1f ms, %.2f s of audio in %.1f ms = %.0fx real time\n", a.first_at * 1000,
               a.n / 22050.0, (now_s() - t1) * 1000, a.n / 22050.0 / (now_s() - t1));
        free(a.pcm);
        cvn_voice_close(v);
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* in order with anything the SDK writes to stderr */
    if (argc < 3) {
        fprintf(stderr, "usage: neural_test ROOT say|selftest|threads|bench ...\n");
        return 2;
    }
    snprintf(SDK, sizeof SDK, "%s/NeuralSDK/macos", argv[1]);
    snprintf(DATA, sizeof DATA, "%s/NeuralVoices", argv[1]);
    snprintf(ONECORE, sizeof ONECORE, "%s/OneCoreVoice", argv[1]);
    snprintf(WORK, sizeof WORK, "%s/build/neural-work", argv[1]);
    if (!strcmp(argv[2], "say") && argc >= 8) {
        sink s;
        cvn_voice *v = open_voice(argv[3]);
        int rc;
        if (!v) return 1;
        memset(&s, 0, sizeof s);
        rc = cvn_voice_speak(v, argv[6], atof(argv[4]), atof(argv[5]), !(argc > 8 && !strcmp(argv[8], "notrim")), on_pcm,
                             &s);
        write_wav(argv[7], s.pcm, s.n, 22050);
        printf("rc %d, %.2f s -> %s\n", rc, s.n / 22050.0, argv[7]);
        return rc < 0;
    }
    if (!strcmp(argv[2], "selftest") && argc >= 4) {
        selftest(argv[3]);
    } else if (!strcmp(argv[2], "threads")) {
        threads(argc > 3 ? atoi(argv[3]) : 3);
    } else if (!strcmp(argv[2], "bench")) {
        bench();
    } else {
        fprintf(stderr, "unknown command\n");
        return 2;
    }
    printf("%s: %d failure(s)\n", argv[2], failures);
    return failures != 0;
}
