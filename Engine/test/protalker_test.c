/* Test driver for IBM ProTALKER 97 Japanese voices (読み秀君 / 読み子ちゃん) through Shared/Bridge/cvp_bridge.c:
 *
 *   protalker_test DATADIR say VOICE RATE SEMITONES "text" out.wav [notrim]
 *   protalker_test DATADIR selftest OUTDIR
 */
#include "cvp_bridge.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_s(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
#else
#include <pthread.h>
#include <sys/time.h>
static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}
#endif

static char DATA[1024];
static int failures;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

typedef struct {
    int16_t *pcm;
    size_t n, cap;
    size_t stop_after;
} sink;

static int on_pcm(const int16_t *pcm, size_t n, void *user)
{
    sink *s = user;
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

static int say(cvp_voice *v, const char *text, double rate, double st, sink *s)
{
    memset(s, 0, sizeof *s);
    return cvp_voice_speak(v, text, rate, st, 1, on_pcm, s);
}

static int same(const sink *a, const sink *b)
{
    return a->n > 0 && a->n == b->n && !memcmp(a->pcm, b->pcm, a->n * 2);
}

static double dbfs(const sink *s)
{
    double sum = 0;
    size_t i;
    if (!s->n) return -120.0;
    for (i = 0; i < s->n; i++) sum += (double)s->pcm[i] * (double)s->pcm[i];
    return 20.0 * log10(sqrt(sum / (double)s->n) / 32768.0 + 1e-12);
}

static double median_f0(const sink *s)
{
    const int fl = 882, lo = 22050 / 400, hi = 22050 / 60;
    double f[4096];
    int nf = 0, i, k, j;
    for (i = 0; i + fl + hi < (int)s->n && nf < 4096; i += fl / 2) {
        double e = 0, best = 0, mean = 0;
        int bk = 0;
        for (j = 0; j < fl; j++) mean += s->pcm[i + j];
        mean /= fl;
        for (j = 0; j < fl; j++) e += (s->pcm[i + j] - mean) * (s->pcm[i + j] - mean);
        if (sqrt(e / fl) < 400) continue;
        for (k = lo; k < hi; k++) {
            double c = 0;
            for (j = 0; j < fl; j++) c += (s->pcm[i + j] - mean) * (s->pcm[i + j + k] - mean);
            if (c > best) best = c, bk = k;
        }
        if (bk && best > 0.35 * e) f[nf++] = 22050.0 / bk;
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
    char err[256] = "", what[512], path[2048];
    cvp_voice *hide, *ko;
    sink a, b, c;
    int rc, i;
    double t0;
    static const char *const empty_cases[] = {"", "   ", "\xF0\x9F\x98\x80\xF0\x9F\x8E\x89", NULL};

    check(!cvp_voice_has_data("/nonexistent") && !cvp_voice_has_data("") && cvp_voice_has_data(DATA),
          "cvp_voice_has_data distinguishes staged ProTalkerVoice from missing folder");
    check(!cvp_voice_open("/nonexistent", "ProTalker_Hide", err, sizeof err) && err[0],
          "missing folder: open fails with a reason");
    check(!cvp_voice_open(DATA, "UnknownVoice", err, sizeof err), "unknown voice: open fails");
    check(fabs(cvp_speed_factor(0) - 1.0) < 1e-9 && cvp_speed_factor(-10) < 0.85 && cvp_speed_factor(18) > 5.0,
          "speed factor: 0.80x .. 5.95x around default speed 6");

    t0 = now_s();
    hide = cvp_voice_open(DATA, "ProTalker_Hide", err, sizeof err);
    snprintf(what, sizeof what, "open 読み秀君 (%.1f ms)", (now_s() - t0) * 1000);
    check(hide != NULL && cvp_voice_sample_rate(hide) == 22050, what);
    if (!hide) return;

    t0 = now_s();
    ko = cvp_voice_open(DATA, "ProTalker_Ko", err, sizeof err);
    snprintf(what, sizeof what, "open 読み子ちゃん (%.1f ms)", (now_s() - t0) * 1000);
    check(ko != NULL, what);
    if (!ko) {
        cvp_voice_close(hide);
        return;
    }

    rc = say(hide, "こんにちは、読み秀君です。今日はとてもいい天気ですね。", 0, 0, &a);
    snprintf(what, sizeof what, "読み秀君 speaks: rc %d, %.2f s, %.1f dBFS", rc, a.n / 22050.0, dbfs(&a));
    check(rc == 0 && a.n > 22050 * 2 && dbfs(&a) > -35.0, what);
    snprintf(path, sizeof path, "%s/Yomihide.wav", out);
    write_wav(path, a.pcm, a.n, 22050);

    rc = say(ko, "こんにちは、読み子ちゃんです。今日はとてもいい天気ですね。", 0, 0, &b);
    snprintf(what, sizeof what, "読み子ちゃん speaks: rc %d, %.2f s, %.1f dBFS", rc, b.n / 22050.0, dbfs(&b));
    check(rc == 0 && b.n > 22050 * 2 && dbfs(&b) > -35.0 && !same(&a, &b), what);
    snprintf(path, sizeof path, "%s/Yomiko.wav", out);
    write_wav(path, b.pcm, b.n, 22050);
    free(a.pcm);
    free(b.pcm);

    /* determinism: fresh handles are byte-identical; consecutive calls on one handle keep exact sample count (like SAPI 4/5,
     * Tts_mgr.dll advances its unvoiced excitation PRNG across calls on the same handle) */
    {
        cvp_voice *h1 = cvp_voice_open(DATA, "ProTalker_Hide", err, sizeof err);
        cvp_voice *h2 = cvp_voice_open(DATA, "ProTalker_Hide", err, sizeof err);
        say(h1, "音声合成のテストを行っています。", 0, 0, &a);
        say(h2, "音声合成のテストを行っています。", 0, 0, &b);
        check(same(&a, &b), "読み秀君 on two fresh handles: byte-identical");
        free(b.pcm);
        cvp_voice_close(h1);
        cvp_voice_close(h2);
    }
    say(hide, "音声合成のテストを行っています。", 0, 0, &b);
    check(b.n == a.n && dbfs(&b) > -35.0, "読み秀君 warm handle: exact same sample count");
    free(b.pcm);

    /* rate: -10, 0, +18 monotonic */
    {
        size_t n_slow, n_mid, n_fast;
        say(hide, "吾輩は猫である。名前はまだ無い。", -10, 0, &b);
        n_slow = b.n;
        free(b.pcm);
        say(hide, "吾輩は猫である。名前はまだ無い。", 0, 0, &b);
        n_mid = b.n;
        free(b.pcm);
        say(hide, "吾輩は猫である。名前はまだ無い。", 18, 0, &b);
        n_fast = b.n;
        free(b.pcm);
        snprintf(what, sizeof what, "rate -10 / 0 / +18: %.2f / %.2f / %.2f s", n_slow / 22050.0, n_mid / 22050.0,
                 n_fast / 22050.0);
        check(n_slow > n_mid && n_mid > n_fast && n_fast > 8000, what);
    }

    /* pitch: -6 / 0 / +6 semitones */
    {
        double f_lo, f_mid, f_hi;
        say(ko, "こんにちは、今日はいい天気ですね。", 0, -6, &b);
        f_lo = median_f0(&b);
        free(b.pcm);
        say(ko, "こんにちは、今日はいい天気ですね。", 0, 0, &b);
        f_mid = median_f0(&b);
        free(b.pcm);
        say(ko, "こんにちは、今日はいい天気ですね。", 0, 6, &b);
        f_hi = median_f0(&b);
        free(b.pcm);
        snprintf(what, sizeof what, "pitch -6 / 0 / +6 semitones: median F0 %.0f / %.0f / %.0f Hz", f_lo, f_mid, f_hi);
        check(f_lo > 0 && f_lo < f_mid && f_mid < f_hi, what);
    }

    /* empty / emoji-only -> 0 samples; mixed English + digits + invalid UTF-8 -> clean */
    for (i = 0; empty_cases[i]; i++) {
        rc = say(hide, empty_cases[i], 0, 0, &b);
        snprintf(what, sizeof what, "empty/emoji case %d: rc %d, %zu samples", i, rc, b.n);
        check(rc == 0 && b.n == 0, what);
        free(b.pcm);
    }
    rc = say(hide, "2026年11月12日、IBM ProTALKER 97のテストです。\xff\xfe", 0, 0, &b);
    snprintf(what, sizeof what, "mixed digits, English and invalid UTF-8: rc %d, %.2f s", rc, b.n / 22050.0);
    check(rc == 0 && b.n > 22050, what);
    free(b.pcm);

    /* cancel mid-utterance, then speak again identically */
    memset(&c, 0, sizeof c);
    c.stop_after = 11025;
    rc = cvp_voice_speak(hide, "昔々あるところに、おじいさんとおばあさんが住んでいました。おじいさんは山へ芝刈りに、おばあさんは川へ洗濯に行きました。",
                         0, 0, 1, on_pcm, &c);
    snprintf(what, sizeof what, "cancel after 0.5 s: rc %d, %.2f s delivered", rc, c.n / 22050.0);
    check(rc == 1 && c.n >= 11025 && c.n < 22050 * 2, what);
    free(c.pcm);

    say(hide, "音声合成のテストを行っています。", 0, 0, &b);
    check(b.n == a.n && dbfs(&b) > -35.0, "utterance after cancel: exact same sample count and level");
    free(a.pcm);
    free(b.pcm);

    cvp_voice_close(ko);
    cvp_voice_close(hide);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: protalker_test DATADIR say|selftest ...\n");
        return 2;
    }
    snprintf(DATA, sizeof DATA, "%s", argv[1]);
    if (!strcmp(argv[2], "say") && argc >= 8) {
        sink s;
        char err[256] = "";
        cvp_voice *v = cvp_voice_open(DATA, argv[3], err, sizeof err);
        int rc;
        if (!v) {
            fprintf(stderr, "open failed: %s\n", err);
            return 1;
        }
        memset(&s, 0, sizeof s);
        rc = cvp_voice_speak(v, argv[6], atof(argv[4]), atof(argv[5]), !(argc > 8 && !strcmp(argv[8], "notrim")), on_pcm,
                             &s);
        write_wav(argv[7], s.pcm, s.n, 22050);
        printf("rc %d, %.2f s -> %s\n", rc, s.n / 22050.0, argv[7]);
        free(s.pcm);
        cvp_voice_close(v);
        return rc < 0;
    }
    if (!strcmp(argv[2], "selftest") && argc >= 4) {
        selftest(argv[3]);
        printf("selftest: %d failure(s)\n", failures);
        return failures != 0;
    }
    fprintf(stderr, "unknown command\n");
    return 2;
}
