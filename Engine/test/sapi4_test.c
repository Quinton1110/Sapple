/* Mac test driver for the native SAPI 4 bridge (Shared/Bridge/cv4n_bridge.c on Engine/sapi4, msttssyn.dll decompiled to
 * C): the exact C the iOS targets compile - the engine library, cv4n_bridge and the SAPI 5 bridge's sanitizer / trimmer /
 * resampler - driven the way ClassicEngine.swift drives it. The checks are cv4_test's (SAPI4/test, the old path), plus
 * what is new: engines parked and reused, the engine cap, the library's one lock.
 *
 *   sapi4_test DATA_DIR say "Mode" RATE SEMITONES "text" out.wav [notrim]
 *   sapi4_test DATA_DIR selftest OUTDIR       every check below; exit status = failures
 *   sapi4_test DATA_DIR bench "Mode" [secs]   open time, first audio, real-time factor, memory
 *   sapi4_test DATA_DIR threads N             N rounds of the two-thread checks (for the TSan build)
 *   sapi4_test DATA_DIR memory                footprint over 19 voices and 600 utterances (plain build: ASan's quarantine
 *                                             inflates it)
 *   sapi4_test DATA_DIR fuzz N                N pseudo-random texts through four voices (the library aborts on the few
 *                                             paths it has no C for; none may be reachable from text)
 */
#include "cv4n_bridge.h"
#include "sapi4tts.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <mach/mach.h>

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static double footprint_mb(void)
{
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return info.phys_footprint / 1048576.0;
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
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static int speak(cv4n_voice *v, const char *text, double rate, double semi, int trim, sink *s)
{
    s->n = 0;
    s->t_first = s->t_stop = 0;
    s->calls = 0;
    return cv4n_voice_speak(v, text, rate, semi, trim, on_pcm, s);
}

static int same_pcm(const sink *a, const sink *b) { return a->n && a->n == b->n && !memcmp(a->pcm, b->pcm, a->n * 2); }

static double peak_abs(const sink *s)
{
    double m = 0;
    size_t i;
    for (i = 0; i < s->n; i++) if (fabs((double)s->pcm[i]) > m) m = fabs((double)s->pcm[i]);
    return m;
}

/* rough F0 (for "pitch moved" checks): median over loud 40 ms frames of the first strong autocorrelation peak */
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double f0_estimate(const sink *s)
{
    const size_t win = 882;
    double f0s[4096];
    int nf = 0;
    size_t at;
    for (at = 0; at + win + 400 < s->n && nf < 4096; at += win / 2) {
        double e = 0, best = 0;
        int lag, bl = 0;
        size_t i;
        for (i = 0; i < win; i++) e += (double)s->pcm[at + i] * s->pcm[at + i];
        if (e / win < 1e6) continue;
        for (lag = 40; lag < 400; lag++) {
            double r = 0;
            for (i = 0; i < win; i++) r += (double)s->pcm[at + i] * s->pcm[at + i + lag];
            if (r > best) { best = r; bl = lag; }
        }
        if (bl && best > 0.3 * e) f0s[nf++] = 22050.0 / bl;
    }
    if (!nf) return 0;
    qsort(f0s, (size_t)nf, sizeof *f0s, cmp_d);
    return f0s[nf / 2];
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
    cv4n_voice *v = cv4n_voice_open(j->dir, j->mode, err, sizeof err);
    j->rc = v ? speak(v, j->text, 0, 0, 1, &j->s) : -2;
    cv4n_voice_close(v);
    return NULL;
}

static const char *const MODES[19] = {
    "Sam", "Mike", "Mary", "Mike (for Telephone)", "Mary (for Telephone)", "Mike in Hall", "Mike in Stadium",
    "Mike in Space", "Mary in Hall", "Mary in Stadium", "Mary in Space", "RoboSoft One", "RoboSoft Two", "RoboSoft Three",
    "RoboSoft Four", "RoboSoft Five", "RoboSoft Six", "Male Whisper", "Female Whisper",
};
static const char *TAIL = "This sentence must still be spoken.";
static const char *LONG = "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.";

/* two voices at once on two threads: each identical to what it gives alone (fresh engines each round) */
static int two_threads(const char *dir, const char *ma, const char *mb)
{
    job a = { dir, ma, LONG, {0}, 0 }, b = { dir, mb, LONG, {0}, 0 };
    job a1 = a, b1 = b;
    pthread_t ta, tb;
    int same;
    cv4n_discard_parked();
    run_job(&a1);
    cv4n_discard_parked();
    run_job(&b1);
    cv4n_discard_parked();
    pthread_create(&ta, NULL, run_job, &a);
    pthread_create(&tb, NULL, run_job, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    cv4n_discard_parked();
    same = a.rc == 0 && b.rc == 0 && same_pcm(&a.s, &a1.s) && same_pcm(&b.s, &b1.s);
    free(a.s.pcm); free(b.s.pcm); free(a1.s.pcm); free(b1.s.pcm);
    return same;
}

static unsigned long long g_rng = 0x2545F4914F6CDD1Dull;
static unsigned rnd(unsigned n)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (unsigned)(g_rng % n);
}

int main(int argc, char **argv)
{
    char err[256], path[1200], what[1400];
    const char *dir = argc > 1 ? argv[1] : "SAPI4Voices";
    if (argc >= 8 && !strcmp(argv[2], "say")) {
        cv4n_voice *v = cv4n_voice_open(dir, argv[3], err, sizeof err);
        sink s = {0};
        int rc;
        double t0;
        if (!v) { fprintf(stderr, "open failed: %s\n", err); return 2; }
        t0 = now_s();
        rc = speak(v, argv[6], atof(argv[4]), atof(argv[5]), !(argc > 8 && !strcmp(argv[8], "notrim")), &s);
        printf("rc=%d samples=%zu (%.2f s) in %.3f s, wpm %u pitch %u\n", rc, s.n, s.n / 22050.0, now_s() - t0,
               cv4n_voice_wpm_for_rate(v, atof(argv[4])), cv4n_voice_pitch_for_semitones(v, atof(argv[5])));
        write_wav(argv[7], s.pcm, s.n);
        cv4n_voice_close(v);
        return rc < 0;
    }
    if (argc >= 4 && !strcmp(argv[2], "bench")) {
        double m0 = footprint_mb(), t0 = now_s(), t_open, secs = argc > 4 ? atof(argv[4]) : 2.0, audio = 0, wall = 0, t1;
        cv4n_voice *v = cv4n_voice_open(dir, argv[3], err, sizeof err);
        sink s = {0};
        int n = 0;
        if (!v) { fprintf(stderr, "open failed: %s\n", err); return 2; }
        t_open = now_s() - t0;
        t1 = now_s();
        speak(v, LONG, 0, 0, 0, &s);
        printf("%-22s first utterance: open %.1f ms, first audio %.1f ms after the call, %.2f s audio in %.1f ms; "
               "+%.1f MB footprint (library data + voice + engine)\n", argv[3], t_open * 1e3, (s.t_first - t1) * 1e3,
               s.n / 22050.0, (now_s() - t1) * 1e3, footprint_mb() - m0);
        t0 = now_s();
        while (now_s() - t0 < secs) {
            t1 = now_s();
            speak(v, LONG, 0, 0, 0, &s);
            wall += now_s() - t1;
            audio += s.n / 22050.0;
            n++;
        }
        printf("%-22s warm %6.0fx real time (%d utterances, %.1f s audio in %.2f s)\n", argv[3], audio / wall, n, audio, wall);
        cv4n_voice_close(v);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[2], "threads")) {
        int rounds = atoi(argv[3]), r, ok = 1;
        for (r = 0; r < rounds; r++) {
            ok &= two_threads(dir, "Mike in Hall", "Sam");
            ok &= two_threads(dir, "Mary (for Telephone)", "Female Whisper");
            ok &= two_threads(dir, "RoboSoft One", "RoboSoft One");   /* the same mode twice: two engines */
        }
        check(ok, "two voices on two threads (Mike in Hall + Sam, Mary for Telephone + Female Whisper, RoboSoft One twice): "
                  "identical to each alone");
        return failures != 0;
    }
    if (argc >= 3 && !strcmp(argv[2], "memory")) {
        sink s = {0};
        double m0 = footprint_mb(), m1, m2;
        int i, k;
        cv4n_voice *v;
        for (i = 0; i < 19; i++) {
            v = cv4n_voice_open(dir, MODES[i], err, sizeof err);
            if (v) speak(v, LONG, 0, 0, 1, &s);
            cv4n_voice_close(v);
        }
        m1 = footprint_mb();
        for (k = 0; k < 600; k++) {
            v = cv4n_voice_open(dir, MODES[k % 19], err, sizeof err);
            if (v) speak(v, k % 3 ? "Hello there, number 42." : LONG, (k % 7) - 3.0, (k % 5) - 2.0, 1, &s);
            cv4n_voice_close(v);
        }
        m2 = footprint_mb();
        snprintf(what, sizeof what, "memory: all 19 voices open and parked %.1f -> %.1f MB; 600 more utterances, voices opened "
                 "and closed each time: %.1f MB (engines opened in all: %d)", m0, m1, m2, cv4n_engines_opened());
        check(m2 - m1 < 2.0 && cv4n_engines_opened() == 19, what);
        return failures != 0;
    }
    if (argc >= 4 && !strcmp(argv[2], "fuzz")) {
        int n = atoi(argv[3]), k;
        static const char *const fm[4] = { "Sam", "Mary", "Mike (for Telephone)", "Female Whisper" };
        cv4n_voice *vs[4];
        sink s = {0};
        char text[2048];
        double t0 = now_s();
        for (k = 0; k < 4; k++) vs[k] = cv4n_voice_open(dir, fm[k], err, sizeof err);
        for (k = 0; k < n; k++) {
            size_t len = 1 + rnd(k % 10 == 0 ? 2000 : 120), i;
            int rc;
            for (i = 0; i < len; i++) {
                unsigned r = rnd(10);
                text[i] = (char)(r < 5 ? 32 + rnd(95) : r < 7 ? "\\=\"0123456789 .,;:!?-'"[rnd(22)] : r < 9 ? 1 + rnd(255) : 'a' + rnd(26));
            }
            text[len] = 0;
            rc = speak(vs[k % 4], text, (double)rnd(29) - 10, (double)rnd(25) - 12, (int)rnd(2), &s);
            if (rc < 0) { printf("FAIL fuzz %d: rc %d\n", k, rc); failures++; }
        }
        snprintf(what, sizeof what, "fuzz: %d pseudo-random texts (up to 2,000 bytes, any byte) through four voices in %.1f s", n,
                 now_s() - t0);
        check(failures == 0, what);
        return failures != 0;
    }
    if (argc >= 4 && !strcmp(argv[2], "selftest")) {
        const char *od = argv[3];
        s4_mode modes[64];
        int nm, i, rc;
        sink s = {0}, s2 = {0};
        cv4n_voice *v;
        size_t base = 0;

        /* 0. the library itself */
        {
            char p[1100];
            snprintf(p, sizeof p, "%s/msttssyn.dll", dir);
            check(s4_init(p) == 0, "the library loads msttssyn.dll's data");
            nm = s4_list_modes(dir, modes, 64);
            int found = 0, k;
            for (i = 0; i < 19; i++)
                for (k = 0; k < nm; k++) if (!strcmp(modes[k].name, MODES[i])) { found++; break; }
            snprintf(what, sizeof what, "all 19 modes the app uses are among the %d the library enumerates", nm);
            check(found == 19, what);
            v = cv4n_voice_open("/nonexistent", "Sam", err, sizeof err);
            snprintf(what, sizeof what, "a folder without the voice files fails cleanly (%s)", v ? "opened" : err);
            check(v == NULL, what);
            v = cv4n_voice_open(dir, "No Such Voice", err, sizeof err);
            snprintf(what, sizeof what, "an unknown mode fails cleanly (%s)", v ? "opened" : err);
            check(v == NULL, what);
        }

        /* 1. every mode: open, speak, sane audio, several utterances in a row */
        for (i = 0; i < 19; i++) {
            double t0 = now_s(), t_open, t_speak = 0;
            size_t first = 0;
            int k, ok = 1;
            v = cv4n_voice_open(dir, MODES[i], err, sizeof err);
            t_open = now_s() - t0;
            if (!v) { snprintf(what, sizeof what, "open '%s': %s", MODES[i], err); check(0, what); continue; }
            t0 = now_s();
            for (k = 0; k < 3; k++) {
                rc = speak(v, k == 1 ? "Second utterance, 12:30 PM, $4.99." : LONG, 0, 0, 1, &s);
                if (k == 0) {
                    first = s.n;
                    t_speak = now_s() - t0;
                    snprintf(path, sizeof path, "%s/mode_%02d.wav", od, i);
                    write_wav(path, s.pcm, s.n);
                }
                ok = ok && rc == 0 && s.n > 22050 && peak_abs(&s) > 1000;
            }
            snprintf(what, sizeof what, "%-22s open %4.1f ms, %.2f s audio in %.1f ms (%.0fx), 3 utterances in a row", MODES[i],
                     t_open * 1e3, first / 22050.0, t_speak * 1e3, first / 22050.0 / t_speak);
            check(ok, what);
            cv4n_voice_close(v);
        }

        v = cv4n_voice_open(dir, "Sam", err, sizeof err);
        check(v != NULL, "open Sam");
        if (!v) return 1;

        /* 2. robustness: cv4_test's hostile strings */
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
                snprintf(what, sizeof what, "hostile %zu: rc=%d, %zu samples (tail alone %zu), %.1f ms", k, rc, n, base, (now_s() - t0) * 1e3);
                check(rc == 0 && n >= base, what);
            }
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
            {
                char *big = malloc(20001);
                double t0 = now_s();
                for (k = 0; k < 20000; k++) big[k] = (char)('a' + k % 26);
                big[20000] = 0;
                rc = speak(v, big, 0, 0, 1, &s);
                snprintf(what, sizeof what, "20,000 letters: rc=%d, %.0f s of audio in %.2f s", rc, s.n / 22050.0, now_s() - t0);
                check(rc == 0 && s.n > 22050 * 60, what);
                free(big);
            }
        }

        /* 3. trim */
        {
            size_t trimmed, raw;
            speak(v, "5", 0, 0, 1, &s);
            trimmed = s.n;
            speak(v, "5", 0, 0, 0, &s);
            raw = s.n;
            snprintf(what, sizeof what, "digit trim (Sam): %zu -> %zu samples (%.0f ms saved)", raw, trimmed, (raw - trimmed) / 22.05);
            check(trimmed > 0 && trimmed < raw, what);
        }

        /* 4. rate and pitch through the engine's own attributes */
        {
            size_t len[5];
            static const double rates[5] = { -10, 0, 5.5, 10, 18 };
            int k;
            for (k = 0; k < 5; k++) {
                speak(v, TAIL, rates[k], 0, 1, &s);
                len[k] = s.n;
            }
            snprintf(what, sizeof what, "rate -10/0/5.5/10/18 -> %u/%u/%u/%u/%u wpm: %zu %zu %zu %zu %zu samples",
                     cv4n_voice_wpm_for_rate(v, -10), cv4n_voice_wpm_for_rate(v, 0), cv4n_voice_wpm_for_rate(v, 5.5),
                     cv4n_voice_wpm_for_rate(v, 10), cv4n_voice_wpm_for_rate(v, 18), len[0], len[1], len[2], len[3], len[4]);
            check(len[0] > len[1] && len[1] > len[2] && len[2] > len[3] && len[3] >= len[4], what);
            speak(v, TAIL, 0, 0, 1, &s);
            snprintf(what, sizeof what, "back at rate 0: %zu samples (was %zu)", s.n, len[1]);
            check(s.n > len[1] * 95 / 100 && s.n < len[1] * 105 / 100, what);
        }
        {
            double f[3], semis[3] = { -12, 0, 12 };
            int k;
            for (k = 0; k < 3; k++) {
                speak(v, "Aaaah.", 0, semis[k], 1, &s);
                f[k] = f0_estimate(&s);
            }
            snprintf(what, sizeof what, "pitch -12/0/+12 st -> %u/%u/%u: F0 about %.0f / %.0f / %.0f Hz",
                     cv4n_voice_pitch_for_semitones(v, -12), cv4n_voice_pitch_for_semitones(v, 0),
                     cv4n_voice_pitch_for_semitones(v, 12), f[0], f[1], f[2]);
            check(f[0] < f[1] && f[1] < f[2], what);
            speak(v, TAIL, 0, 0, 1, &s);
        }

        /* 5. cancel mid-utterance, then carry on */
        {
            char big[4000];
            int k;
            double t_ret;
            big[0] = 0;
            for (k = 0; k < 12; k++) strcat(big, "Alice was beginning to get very tired of sitting by her sister on the bank. ");
            s.stop_after = 22050;   /* one second in */
            rc = speak(v, big, 0, 0, 1, &s);
            t_ret = now_s();
            snprintf(what, sizeof what, "cancel after 1 s of a 60 s text: rc=%d, %.2f s delivered, returned %.2f ms after the stop "
                     "(%d chunks)", rc, s.n / 22050.0, (t_ret - s.t_stop) * 1e3, s.calls);
            check(rc == 1 && (t_ret - s.t_stop) < 0.25, what);
            s.stop_after = 0;
            rc = speak(v, TAIL, 0, 0, 1, &s);
            snprintf(what, sizeof what, "next utterance after a cancel: rc=%d, %zu samples (normal %zu)", rc, s.n, base);
            check(rc == 0 && s.n > base * 9 / 10 && s.n < base * 11 / 10, what);
            snprintf(path, sizeof path, "%s/after_cancel.wav", od);
            write_wav(path, s.pcm, s.n);
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
        cv4n_voice_close(v);

        /* 6. cancel on effect modes and a telephone mode */
        {
            static const char *const cm[3] = { "Mary in Hall", "Mike (for Telephone)", "Female Whisper" };
            int k;
            for (k = 0; k < 3; k++) {
                size_t normal;
                int rc2;
                v = cv4n_voice_open(dir, cm[k], err, sizeof err);
                if (!v) { check(0, cm[k]); continue; }
                speak(v, TAIL, 0, 0, 1, &s);
                normal = s.n;
                s.stop_after = 5000;
                rc = speak(v, LONG, 0, 0, 1, &s);
                s.stop_after = 0;
                rc2 = speak(v, TAIL, 0, 0, 1, &s);
                snprintf(what, sizeof what, "%s: cancel rc=%d, then rc=%d with %zu samples (normal %zu)", cm[k], rc, rc2, s.n, normal);
                check(rc == 1 && rc2 == 0 && s.n > normal * 9 / 10 && s.n < normal * 11 / 10, what);
                cv4n_voice_close(v);
            }
        }

        /* 7. parking: a closed voice's engine comes back warm; no new engine for a mode already opened */
        {
            int before = cv4n_engines_opened(), k;
            sink a = {0}, b = {0}, c = {0};
            cv4n_voice *v1, *v2;
            for (k = 0; k < 50; k++) {
                v1 = cv4n_voice_open(dir, "Mike in Hall", err, sizeof err);
                cv4n_voice_close(v1);
            }
            snprintf(what, sizeof what, "50 open / close of a mode already parked: %d new engines", cv4n_engines_opened() - before);
            check(cv4n_engines_opened() == before, what);
            v1 = cv4n_voice_open(dir, "RoboSoft Two", err, sizeof err);
            v2 = cv4n_voice_open(dir, "RoboSoft Two", err, sizeof err);
            snprintf(what, sizeof what, "two voices of one mode open at once get two engines (%d new)", cv4n_engines_opened() - before);
            check(v1 && v2 && cv4n_engines_opened() - before <= 2, what);
            cv4n_voice_close(v1);
            cv4n_voice_close(v2);
            /* fresh = discard, then open: deterministic */
            cv4n_discard_parked();
            v1 = cv4n_voice_open(dir, "Male Whisper", err, sizeof err);
            speak(v1, LONG, 0, 0, 1, &a);
            speak(v1, LONG, 0, 0, 1, &c);       /* warm: the engine's noise and filters continue */
            cv4n_voice_close(v1);
            cv4n_discard_parked();
            v1 = cv4n_voice_open(dir, "Male Whisper", err, sizeof err);
            speak(v1, LONG, 0, 0, 1, &b);
            cv4n_voice_close(v1);
            check(same_pcm(&a, &b), "fresh engines are deterministic (Male Whisper, after cv4n_discard_parked)");
            snprintf(what, sizeof what, "a warm engine continues its state: second utterance %s the first (as in the original)",
                     same_pcm(&a, &c) ? "IDENTICAL to" : "differs from");
            check(!same_pcm(&a, &c) && c.n > a.n * 9 / 10 && c.n < a.n * 11 / 10, what);
            free(a.pcm); free(b.pcm); free(c.pcm);
        }

        /* 8. two voices on two threads at once give exactly what they give alone */
        check(two_threads(dir, "Mike in Hall", "Sam") && two_threads(dir, "RoboSoft One", "RoboSoft One"),
              "two voices on two threads (and one mode twice): output identical to running them one at a time");

        /* 9. the engine cap: past it, open fails cleanly and parked voices still speak */
        {
            int opened = cv4n_engines_opened(), k, fails = 0;
            cv4n_voice *keep = cv4n_voice_open(dir, "Sam", err, sizeof err);
            for (k = 0; k < 140; k++) {
                cv4n_discard_parked();
                v = cv4n_voice_open(dir, "Mary", err, sizeof err);
                if (!v) { fails++; continue; }
                cv4n_voice_close(v);
            }
            snprintf(what, sizeof what, "engine cap: after %d engines, %d of 140 fresh opens refused (\"%s\"), no crash",
                     opened, fails, fails ? err : "");
            check(fails > 0 && cv4n_engines_opened() <= 127 + 2, what);
            rc = keep ? speak(keep, TAIL, 0, 0, 1, &s) : -2;
            check(rc == 0 && s.n > base * 9 / 10, "a voice held open still speaks after the cap is reached");
            cv4n_voice_close(keep);
        }

        printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
        return failures != 0;
    }
    fprintf(stderr, "usage: see the comment at the top of sapi4_test.c\n");
    return 1;
}
