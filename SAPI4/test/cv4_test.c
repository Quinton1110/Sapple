/* Mac test driver for the SAPI 4 bridge (Shared/Bridge/cv4_bridge.c): the exact C the iOS targets
 * compile - emulator, sapi4_tts, cv4_bridge and the SAPI 5 bridge's sanitizer/trimmer - driven the way
 * ClassicEngine.swift drives it from the extension.
 *
 *   cv4_test DATA_DIR say "Mode" RATE SEMITONES "text" out.wav [notrim]
 *   cv4_test DATA_DIR selftest OUTDIR [quick]     every check below; exit status = failures
 *                                                 (DATA_DIR with msttssyn.dll: Microsoft, 19 modes;
 *                                                  with tv_enua.dll: L&H TruVoice, 10 modes)
 *   cv4_test DATA_DIR bench "Mode" [seconds]      real-time factor on one warm voice
 */
#include "cv4_bridge.h"
#include "sapi4_tts.h"

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

typedef struct {
    int16_t *pcm;
    size_t n, cap;
    size_t stop_after;     /* return "stop" once this many samples arrived (0 = never) */
    double t_first, t_stop;
    int calls;
} sink;

static int on_pcm(const int16_t *pcm, size_t n, void *user)
{
    sink *s = user;
    if (!s->t_first) s->t_first = now_s();
    s->calls++;
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

static void write_wav(const char *path, const int16_t *pcm, size_t n)
{
    FILE *f = fopen(path, "wb");
    uint32_t rate = 22050, datasz = (uint32_t)(n * 2), riff = 36 + datasz, br = rate * 2, fmtsz = 16;
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

static int speak(cv4_voice *v, const char *text, double rate, double semi, int trim, sink *s)
{
    s->n = 0;
    s->t_first = s->t_stop = 0;
    s->calls = 0;
    return cv4_voice_speak(v, text, rate, semi, trim, on_pcm, s);
}

static double peak_abs(const sink *s)
{
    int m = 0;
    size_t i;
    for (i = 0; i < s->n; i++) m = abs(s->pcm[i]) > m ? abs(s->pcm[i]) : m;
    return m;
}

/* rough F0 (for "pitch moved" checks): median over loud 40 ms frames of the first strong
 * normalised-autocorrelation peak (the smallest lag within 85% of the best, the best above 0.5) */
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double f0_estimate(const sink *s)
{
    enum { W = 882, LMIN = 22050 / 500, LMAX = 22050 / 40 };
    static double est[4096], rr[LMAX + 1];
    int ne = 0;
    size_t st;
    for (st = 0; st + 2 * W + LMAX < s->n && ne < 4096; st += W / 2) {
        const int16_t *x = s->pcm + st;
        double e = 0, best = 0;
        int k, lag;
        for (k = 0; k < W; k++) e += (double)x[k] * x[k];
        if (e < (double)W * 1500.0 * 1500.0) continue;
        for (lag = LMIN; lag < LMAX; lag++) {
            double r = 0, e2 = 0;
            for (k = 0; k < W; k++) { r += (double)x[k] * x[k + lag]; e2 += (double)x[k + lag] * x[k + lag]; }
            rr[lag] = r / sqrt(e * e2 + 1e-9);
            if (rr[lag] > best) best = rr[lag];
        }
        if (best <= 0.5) continue;
        for (lag = LMIN; lag < LMAX; lag++) if (rr[lag] >= 0.85 * best) { est[ne++] = 22050.0 / lag; break; }
    }
    if (!ne) return 0;
    qsort(est, (size_t)ne, sizeof *est, cmp_d);
    return est[ne / 2];
}

/* ------------------------------------------------------------------ concurrency */
typedef struct {
    const char *dir, *mode, *text;
    sink s;
    int rc;
} job;

static void *run_job(void *p)
{
    job *j = p;
    char err[256];
    cv4_voice *v = cv4_voice_open(j->dir, j->mode, err, sizeof err);
    int k;
    j->rc = -2;
    if (!v) return NULL;
    for (k = 0; k < 3; k++) j->rc = speak(v, j->text, 0, 0, 1, &j->s);
    cv4_voice_close(v);
    return NULL;
}

static const char *TAIL = "This sentence must still be spoken.";

/* what the self-test uses per engine */
typedef struct {
    int nmodes;
    const char *main_voice;          /* robustness, trim, rate, pitch, cancel */
    const char *cancel_modes[3];
    const char *thread_a, *thread_b; /* two engines on two threads */
    const char *determinism;         /* close and reopen: identical output */
    double rates[5];                 /* strictly slower -> faster within the engine's own range */
} engine_cfg;
static const engine_cfg CFG_MS = { 19, "Sam", { "Mary in Hall", "Mike (for Telephone)", "Female Whisper" }, "Mike in Hall", "Sam",
                                   "Male Whisper", { -10, 0, 5.5, 10, 18 } };
static const engine_cfg CFG_TV = { 10, "Adult Male #1, American English (TruVoice)",
                                   { "Adult Male #2, American English (TruVoice)", "Adult Female #1, American English (TruVoice)",
                                     "Adult Male #8, American English (TruVoice)" },
                                   "Adult Female #2, American English (TruVoice)", "Adult Male #2, American English (TruVoice)",
                                   "Adult Male #5, American English (TruVoice)", { -10, -4, 0, 2.5, 18 } };
static const char *LONG = "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.";

int main(int argc, char **argv)
{
    char err[256], path[1200], what[1400];
    if (argc >= 8 && !strcmp(argv[2], "say")) {
        cv4_voice *v = cv4_voice_open(argv[1], argv[3], err, sizeof err);
        sink s = {0};
        int rc;
        double t0;
        if (!v) { fprintf(stderr, "open failed: %s\n", err); return 2; }
        t0 = now_s();
        rc = speak(v, argv[6], atof(argv[4]), atof(argv[5]), !(argc > 8 && !strcmp(argv[8], "notrim")), &s);
        printf("rc=%d samples=%zu (%.2f s) in %.3f s, wpm %u pitch %u\n", rc, s.n, s.n / 22050.0, now_s() - t0,
               cv4_voice_wpm_for_rate(v, atof(argv[4])), cv4_voice_pitch_for_semitones(v, atof(argv[5])));
        write_wav(argv[7], s.pcm, s.n);
        cv4_voice_close(v);
        return rc < 0;
    }
    if (argc >= 4 && !strcmp(argv[2], "bench")) {
        cv4_voice *v = cv4_voice_open(argv[1], argv[3], err, sizeof err);
        sink s = {0};
        double secs = argc > 4 ? atof(argv[4]) : 3.0, t0, audio = 0, wall = 0;
        int n = 0;
        if (!v) { fprintf(stderr, "open failed: %s\n", err); return 2; }
        speak(v, "Warm up.", 0, 0, 0, &s);
        t0 = now_s();
        while (now_s() - t0 < secs) {
            double t1 = now_s();
            speak(v, LONG, 0, 0, 0, &s);
            wall += now_s() - t1;
            audio += s.n / 22050.0;
            n++;
        }
        printf("%-22s %6.2fx real time (%d utterances, %.1f s audio in %.2f s)\n", argv[3], audio / wall, n, audio, wall);
        cv4_voice_close(v);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[2], "selftest")) {
        const char *dir = argv[1], *od = argv[3];
        int quick = argc > 4 && !strcmp(argv[4], "quick");
        s4_mode modes[32];
        int nm = s4_list_modes(dir, modes, 32, err, sizeof err), i, rc;
        sink s = {0}, s2 = {0};
        cv4_voice *v;
        size_t base = 0;
        const engine_cfg *cfg = nm > 0 && strstr(modes[0].name, "(TruVoice)") ? &CFG_TV : &CFG_MS;

        snprintf(what, sizeof what, "the engine enumerates %d modes (%d)", cfg->nmodes, nm);
        check(nm == cfg->nmodes, what);

        /* 1. every mode: open, speak, sane audio, several utterances in a row */
        for (i = 0; i < nm; i++) {
            double t0 = now_s(), t_open, t_speak;
            size_t first = 0;
            int k, ok = 1;
            v = cv4_voice_open(dir, modes[i].name, err, sizeof err);
            t_open = now_s() - t0;
            if (!v) { snprintf(what, sizeof what, "open '%s': %s", modes[i].name, err); check(0, what); continue; }
            t0 = now_s();
            for (k = 0; k < (quick ? 1 : 3); k++) {
                rc = speak(v, k == 1 ? "Second utterance, 12:30 PM, $4.99." : LONG, 0, 0, 1, &s);
                if (k == 0) {
                    first = s.n;
                    t_speak = now_s() - t0;
                    snprintf(path, sizeof path, "%s/mode_%02d.wav", od, i);
                    write_wav(path, s.pcm, s.n);
                }
                ok = ok && rc == 0 && s.n > 22050 && peak_abs(&s) > 1000;
            }
            snprintf(what, sizeof what, "%-22s open %4.0f ms, %.2f s audio in %.3f s (%.1fx), %s", modes[i].name, t_open * 1e3,
                     first / 22050.0, t_speak, first / 22050.0 / t_speak, quick ? "1 utterance" : "3 utterances in a row");
            check(ok, what);
            cv4_voice_close(v);
        }

        v = cv4_voice_open(dir, cfg->main_voice, err, sizeof err);
        snprintf(what, sizeof what, "open %s", cfg->main_voice);
        check(v != NULL, what);
        if (!v) return 1;

        /* 2. robustness: the SAPI 5 self-test's hostile prefixes, plus SAPI 4 specifics */
        {
            static const char *hostile[] = {
                "Party time \xF0\x9F\x98\x80\xF0\x9F\x8E\x89 yes.",
                "Go \xE2\x86\x92 next.",
                "Copyright \xC2\xA9 2024.",
                "\xE4\xBD\xA0\xE5\xA5\xBD \xE4\xB8\x96\xE7\x95\x8C.",
                "\xC3\x9F.",
                "\xC3\x9F\xC3\x9F \xC3\x9F x.",
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.",
                "\xCE\xB1\xCE\xB2\xCE\xB3 \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82.",
                "\xEF\xBF\xBD\xEF\xBB\xBF\xE2\x80\x8B.",
                "\xFF\xFE\x80 bad bytes.",
                /* SAPI 4 specific: tag syntax must be read as text (the bridge never sets TAGGED) */
                "\\Pit=400\\ \\Spd=450\\ \\Vce=Speaker=Mary\\ \\Rst\\ C:\\Windows\\System32.",
                "\\\\\\\\ \\ \\\\.",
                "~!@#$%^&*()_+{}|:\"<>?`-=[];',./ \x7F.",
                "Euro \xE2\x82\xAC 5, trade mark \xE2\x84\xA2, per mille \xE2\x80\xB0, degrees \xC2\xB0, pound \xC2\xA3, half \xC2\xBD.",
                "Caf\xC3\xA9 na\xC3\xAFve r\xC3\xA9sum\xC3\xA9 \xC3\x85ngstr\xC3\xB6m S\xC3\xA3o Paulo.",
                "1234567890123456789012345678901234567890 3.14159265358979 -0.5 1e10 0x1F 10/10/2020 12:00:00.",
                "http://www.example.com/path?query=1&x=2 user@example.com.",
                "A. B. C. D. E. F. G. H. I. J. K. L. M. N. O. P. Q. R. S. T. U. V. W. X. Y. Z.",
                "...!!!???,,,;;;:::---",
            };
            size_t k, n = 0;
            rc = speak(v, TAIL, 0, 0, 1, &s);
            base = s.n;
            check(rc == 0 && base > 22050, "baseline sentence speaks");
            for (k = 0; k < sizeof hostile / sizeof *hostile; k++) {
                char text[1024];
                double t0 = now_s();
                snprintf(text, sizeof text, "%s %s", hostile[k], TAIL);
                rc = speak(v, text, 0, 0, 1, &s);
                n = s.n;
                snprintf(path, sizeof path, "%s/hostile%02zu.wav", od, k);
                write_wav(path, s.pcm, s.n);
                snprintf(what, sizeof what, "hostile %zu: rc=%d, %zu samples (tail alone %zu), %.2f s", k, rc, n, base, now_s() - t0);
                check(rc == 0 && n >= base, what);
            }
            /* after all that, the voice must be exactly as before (tags must not have changed it) */
            rc = speak(v, TAIL, 0, 0, 1, &s2);
            snprintf(what, sizeof what, "voice unchanged after the hostile strings: rc=%d, %zu vs %zu samples", rc, s2.n, base);
            check(rc == 0 && s2.n > base * 9 / 10 && s2.n < base * 11 / 10, what);
            rc = speak(v, "", 0, 0, 1, &s);
            check(rc == 0 && s.n == 0, "empty text gives no audio and no stand-in text");
            rc = speak(v, " \xF0\x9F\x98\x80 \xE2\x80\x8B ", 0, 0, 1, &s);
            check(rc == 0 && s.n == 0, "emoji-only text gives no audio");
            rc = speak(v, "!", 0, 0, 1, &s);
            snprintf(what, sizeof what, "punctuation-only text: rc=%d, %zu samples", rc, s.n);
            check(rc == 0, what);
        }

        /* 3. trim */
        {
            size_t trimmed, raw;
            speak(v, "5", 0, 0, 1, &s);
            trimmed = s.n;
            speak(v, "5", 0, 0, 0, &s);
            raw = s.n;
            snprintf(what, sizeof what, "digit trim (%s): %zu -> %zu samples (%.0f ms saved)", cfg->main_voice, raw, trimmed, (raw - trimmed) / 22.05);
            check(trimmed > 0 && trimmed < raw, what);
        }

        /* 4. rate and pitch through the engine's own attributes */
        {
            size_t len[5];
            const double *rates = cfg->rates;
            int k;
            for (k = 0; k < 5; k++) {
                speak(v, TAIL, rates[k], 0, 1, &s);
                len[k] = s.n;
            }
            snprintf(what, sizeof what, "rate %g/%g/%g/%g/%g -> %u/%u/%u/%u/%u wpm: %zu %zu %zu %zu %zu samples", rates[0], rates[1],
                     rates[2], rates[3], rates[4], cv4_voice_wpm_for_rate(v, rates[0]), cv4_voice_wpm_for_rate(v, rates[1]),
                     cv4_voice_wpm_for_rate(v, rates[2]), cv4_voice_wpm_for_rate(v, rates[3]), cv4_voice_wpm_for_rate(v, rates[4]),
                     len[0], len[1], len[2], len[3], len[4]);
            check(len[0] > len[1] && len[1] > len[2] && len[2] > len[3] && len[3] >= len[4], what);
            speak(v, TAIL, 0, 0, 1, &s);
            {
                size_t at0 = rates[1] == 0 ? len[1] : len[2];
                snprintf(what, sizeof what, "back at rate 0: %zu samples (was %zu)", s.n, at0);
                check(s.n > at0 * 95 / 100 && s.n < at0 * 105 / 100, what);
            }
        }
        {
            double f[3], semis[3] = { -12, 0, 12 };
            int k;
            for (k = 0; k < 3; k++) {
                speak(v, "Aaaah.", 0, semis[k], 1, &s);
                f[k] = f0_estimate(&s);
            }
            snprintf(what, sizeof what, "pitch -12/0/+12 st -> %u/%u/%u: F0 about %.0f / %.0f / %.0f Hz",
                     cv4_voice_pitch_for_semitones(v, -12), cv4_voice_pitch_for_semitones(v, 0), cv4_voice_pitch_for_semitones(v, 12),
                     f[0], f[1], f[2]);
            check(f[0] < f[1] && f[1] < f[2], what);
            speak(v, TAIL, 0, 0, 1, &s);
        }

        /* 5. cancel mid-utterance, then carry on */
        {
            char big[4000];
            int k;
            big[0] = 0;
            for (k = 0; k < 12; k++) strcat(big, "Alice was beginning to get very tired of sitting by her sister on the bank. ");
            s.stop_after = 22050;   /* one second in */
            double t0 = now_s();
            rc = speak(v, big, 0, 0, 1, &s);
            double t_ret = now_s();
            snprintf(what, sizeof what, "cancel after 1 s of a 60 s text: rc=%d, returned %.1f ms after the stop (%d chunks)", rc,
                     (t_ret - s.t_stop) * 1e3, s.calls);
            check(rc == 1 && (t_ret - s.t_stop) < 0.25, what);
            s.stop_after = 0;
            (void)t0;
            rc = speak(v, TAIL, 0, 0, 1, &s);
            snprintf(what, sizeof what, "next utterance after a cancel: rc=%d, %zu samples (normal %zu)", rc, s.n, base);
            check(rc == 0 && s.n > base * 9 / 10 && s.n < base * 11 / 10, what);
            snprintf(path, sizeof path, "%s/after_cancel.wav", od);
            write_wav(path, s.pcm, s.n);
            /* cancel on the very first chunk, several times in a row */
            for (k = 0; k < 5; k++) {
                s.stop_after = 1;
                rc = speak(v, big, 0, 0, 1, &s);
                if (rc != 1) break;
            }
            s.stop_after = 0;
            snprintf(what, sizeof what, "5 immediate cancels in a row: last rc=%d", rc);
            check(rc == 1, what);
            rc = speak(v, TAIL, 0, 0, 1, &s);
            check(rc == 0 && s.n > base * 9 / 10 && s.n < base * 11 / 10, "speaks normally after them");
        }
        cv4_voice_close(v);

        /* 6. cancel on an effect mode and the telephone mode */
        {
            const char *const *cm = cfg->cancel_modes;
            int k;
            for (k = 0; k < 3; k++) {
                size_t normal;
                v = cv4_voice_open(dir, cm[k], err, sizeof err);
                if (!v) { check(0, cm[k]); continue; }
                speak(v, TAIL, 0, 0, 1, &s);
                normal = s.n;
                s.stop_after = 5000;
                rc = speak(v, LONG, 0, 0, 1, &s);
                s.stop_after = 0;
                int rc2 = speak(v, TAIL, 0, 0, 1, &s);
                snprintf(what, sizeof what, "%s: cancel rc=%d, then rc=%d with %zu samples (normal %zu)", cm[k], rc, rc2, s.n, normal);
                check(rc == 1 && rc2 == 0 && s.n > normal * 9 / 10 && s.n < normal * 11 / 10, what);
                cv4_voice_close(v);
            }
        }

        /* 7. two voices on two threads at once give exactly what they give alone */
        {
            job a = { dir, cfg->thread_a, LONG, {0}, 0 }, b = { dir, cfg->thread_b, LONG, {0}, 0 };
            job a1 = a, b1 = b;
            pthread_t ta, tb;
            run_job(&a1);
            run_job(&b1);
            pthread_create(&ta, NULL, run_job, &a);
            pthread_create(&tb, NULL, run_job, &b);
            pthread_join(ta, NULL);
            pthread_join(tb, NULL);
            int same = a.rc == 0 && b.rc == 0 && a.s.n == a1.s.n && b.s.n == b1.s.n &&
                       !memcmp(a.s.pcm, a1.s.pcm, a.s.n * 2) && !memcmp(b.s.pcm, b1.s.pcm, b.s.n * 2);
            check(same, "two engines on two threads: output identical to running them one at a time");
        }

        /* 8. an engine that is closed while idle and reopened speaks the same (no state leaks) */
        {
            sink x = {0}, y = {0};
            cv4_voice *v1 = cv4_voice_open(dir, cfg->determinism, err, sizeof err);
            if (v1) { speak(v1, LONG, 0, 0, 1, &x); cv4_voice_close(v1); }
            v1 = cv4_voice_open(dir, cfg->determinism, err, sizeof err);
            if (v1) { speak(v1, LONG, 0, 0, 1, &y); cv4_voice_close(v1); }
            snprintf(what, sizeof what, "fresh engines are deterministic (%s)", cfg->determinism);
            check(x.n && x.n == y.n && !memcmp(x.pcm, y.pcm, x.n * 2), what);
        }

        printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
        return failures != 0;
    }
    fprintf(stderr, "usage: see the comment at the top of cv4_test.c\n");
    return 1;
}
