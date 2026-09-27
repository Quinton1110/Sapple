/* Mac test driver for the singing mode of the SAPI 5 voices ("Enable singing mode for SAPI 5 voices"): the engine
 * (Engine/sam) and its bridge (Shared/Bridge/cv_bridge.c) exactly as the iOS targets compile them.
 *
 *   sing_test DATA_DIR selftest OUTDIR     every check below; exit status = failures (WAVs in OUTDIR)
 *   sing_test DATA_DIR threads [reps]      two voices singing on two threads (run under TSan: make sing-tsan)
 *   sing_test DATA_DIR say VOICE EFFECT RATE "text" out.wav [literal]   one render
 */
#include "cv_bridge.h"
#include "sam.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static int failures;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
    fflush(stdout);
}

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

typedef struct {
    int16_t *pcm;
    size_t n, cap;
    size_t stop_at; /* 0 = never stop */
} buf;

static int on_pcm(const int16_t *pcm, size_t n, void *user)
{
    buf *b = user;
    if (b->n + n > b->cap) {
        size_t c = (b->n + n) * 2 + 4096;
        int16_t *p = realloc(b->pcm, c * sizeof *p);
        if (!p) return 1;
        b->pcm = p;
        b->cap = c;
    }
    memcpy(b->pcm + b->n, pcm, n * sizeof *pcm);
    b->n += n;
    return b->stop_at && b->n >= b->stop_at;
}

static void write_wav(const char *path, const buf *b)
{
    FILE *f = fopen(path, "wb");
    unsigned n = (unsigned)b->n * 2, v;
    unsigned short s;
    if (!f) return;
    fwrite("RIFF", 1, 4, f); v = 36 + n; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); s = 1; fwrite(&s, 2, 1, f); fwrite(&s, 2, 1, f);
    v = 22050; fwrite(&v, 4, 1, f); v = 44100; fwrite(&v, 4, 1, f); s = 2; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&n, 4, 1, f); fwrite(b->pcm, 2, b->n, f);
    fclose(f);
}

static int same(const buf *a, const buf *b) { return a->n == b->n && !memcmp(a->pcm, b->pcm, a->n * 2); }

static const cv_sing_settings DEF = {30.0, 5.5, 0.0};

/* one render from a fresh voice */
static int sing(const char *dir, const char *voice, const char *effect, double rate, const cv_sing_settings *cfg,
                const char *text, int literal, buf *out, int *mism)
{
    char err[256];
    cv_voice *v = cv_voice_open(dir, voice, effect, 0, err, sizeof err);
    int rc;
    if (!v) {
        fprintf(stderr, "open %s: %s\n", voice, err);
        return -2;
    }
    rc = cv_voice_sing(v, text, rate, 0, cfg, literal, 1, on_pcm, out, mism);
    cv_voice_close(v);
    return rc;
}

static int speak(const char *dir, const char *voice, const char *effect, const char *text, buf *out)
{
    char err[256];
    cv_voice *v = cv_voice_open(dir, voice, effect, 0, err, sizeof err);
    int rc;
    if (!v) return -2;
    rc = cv_voice_speak(v, text, 0, 0, 1, on_pcm, out);
    cv_voice_close(v);
    return rc;
}

/* median pitch estimate of the loud part (autocorrelation, 60-500 Hz) - enough to tell transpositions apart */
static double median_f0(const buf *b)
{
    double f[4096];
    int nf = 0;
    size_t i;
    for (i = 0; i + 1024 < b->n && nf < 4096; i += 441) {
        const int16_t *x = b->pcm + i;
        double e = 0, best = 0;
        int lag, bl = 0, k;
        for (k = 0; k < 1024; k++) e += (double)x[k] * x[k];
        if (e / 1024 < 1e6) continue;
        for (lag = 44; lag <= 367; lag++) {
            double c = 0;
            for (k = 0; k + lag < 1024; k++) c += (double)x[k] * x[k + lag];
            if (c > best) { best = c; bl = lag; }
        }
        if (bl && best > 0.3 * e) f[nf++] = 22050.0 / bl;
    }
    if (!nf) return 0;
    for (i = 1; i < (size_t)nf; i++) { /* insertion sort */
        double t = f[i];
        size_t j = i;
        while (j > 0 && f[j - 1] > t) { f[j] = f[j - 1]; j--; }
        f[j] = t;
    }
    return f[nf / 2];
}

static int selftest(const char *dir, const char *od)
{
    char path[1024], what[512];
    static const struct { const char *voice, *effect, *slug; } V[] = {
        {"Sam", "none", "sam"}, {"Mike", "none", "mike"}, {"Mary", "none", "mary"}, {"Mike", "hall", "mike_hall"},
        {"Mary", "stadium", "mary_stadium"}, {"Mike", "space", "mike_space"}, {"Sam", "robosoft1", "robosoft1"},
        {"Mary", "robosoft4", "robosoft4"}};
    const char *hello = "Hello, my name is Sam, and I can sing whatever VoiceOver says. Is that not stylish?";
    size_t i;
    /* 1. every kind of SAPI 5 voice sings, and singing is not speaking */
    for (i = 0; i < sizeof V / sizeof *V; i++) {
        buf s = {0}, p = {0};
        int rc = sing(dir, V[i].voice, V[i].effect, 0, &DEF, hello, 0, &s, NULL);
        int rp = speak(dir, V[i].voice, V[i].effect, hello, &p);
        snprintf(path, sizeof path, "%s/sing_%s.wav", od, V[i].slug);
        write_wav(path, &s);
        snprintf(what, sizeof what, "%-13s sings: rc=%d, %.2f s (speech %.2f s), differs from speech", V[i].slug, rc,
                 s.n / 22050.0, p.n / 22050.0);
        check(rc == 0 && rp == 0 && s.n > 22050 * 3 && !same(&s, &p), what);
        free(s.pcm);
        free(p.pcm);
    }
    /* 2. one note per syllable of the pronunciation: literal scores with the right and wrong note counts */
    {
        static const struct { const char *score; int want; } L[] = {
            {"styl-ish C4 1 E4 1\n", 0},              /* "stylish": no syllable mark in the lexicon, two vowels */
            {"rof-fel-cop-ter G4 1 E4 1 C4 1 D4 1\n", 0},
            {"hello C4 1 D4 1\n", 0},
            {"rofl C4 1 D4 1\n", 1},                  /* r aa f l: one vowel, two notes */
            {"hello C4 1\n", 1},                      /* two syllables, one note */
            {"tempo 120\nbeau-ti-ful C4 1 D4 1 E4 1\n- 1\nwater G4 1 C4 2\n", 0}};
        for (i = 0; i < sizeof L / sizeof *L; i++) {
            buf s = {0};
            int m = -1, rc = sing(dir, "Sam", "none", 0, &DEF, L[i].score, 1, &s, &m);
            snprintf(what, sizeof what, "literal score %zu: rc=%d, %d word(s) with notes != syllables (want %d)", i, rc, m, L[i].want);
            check(rc == 0 && m == L[i].want && s.n > 0, what);
            free(s.pcm);
        }
        check(cv_sing_is_score("tempo 100\nhi C4 1") && cv_sing_is_score("twin-kle C4 1 C4 1\nlit-tle A4 1 A4 1") &&
                  !cv_sing_is_score("Hello there. The meeting is at 10 30.") && !cv_sing_is_score("Battery 65 percent"),
              "score detection: \"tempo \" and \"word NOTE BEATS\" lines only");
    }
    /* 3. deterministic: two fresh voices sing the same thing identically */
    {
        buf a = {0}, b = {0};
        sing(dir, "Mary", "none", 0, &DEF, hello, 0, &a, NULL);
        sing(dir, "Mary", "none", 0, &DEF, hello, 0, &b, NULL);
        check(a.n > 0 && same(&a, &b), "deterministic: the same text sings bit-identically twice");
        free(a.pcm);
        free(b.pcm);
    }
    /* 4. transpose, vibrato and rate do what they say */
    {
        cv_sing_settings c = DEF;
        buf base = {0}, down = {0}, up = {0}, novib = {0}, fast = {0};
        const char *t = "Twinkle twinkle little star, how I wonder what you are.";
        double f0, fd, fu;
        sing(dir, "Mike", "none", 0, &c, t, 0, &base, NULL);
        c.transpose = -12;
        sing(dir, "Mike", "none", 0, &c, t, 0, &down, NULL);
        c.transpose = 12;
        sing(dir, "Mike", "none", 0, &c, t, 0, &up, NULL);
        c = DEF;
        c.vibrato_cents = 0;
        sing(dir, "Mike", "none", 0, &c, t, 0, &novib, NULL);
        sing(dir, "Mike", "none", 10, &DEF, t, 0, &fast, NULL);
        f0 = median_f0(&base);
        fd = median_f0(&down);
        fu = median_f0(&up);
        snprintf(path, sizeof path, "%s/transpose_down12.wav", od);
        write_wav(path, &down);
        {   /* the bottom of the range still sings */
            buf d24 = {0};
            int r24;
            c = DEF;
            c.transpose = -24;
            r24 = sing(dir, "Mike", "none", 0, &c, t, 0, &d24, NULL);
            snprintf(path, sizeof path, "%s/transpose_down24.wav", od);
            write_wav(path, &d24);
            snprintf(what, sizeof what, "transpose -24: rc=%d, %.2f s", r24, d24.n / 22050.0);
            check(r24 == 0 && d24.n > 22050, what);
            free(d24.pcm);
        }
        snprintf(path, sizeof path, "%s/transpose_up12.wav", od);
        write_wav(path, &up);
        snprintf(what, sizeof what, "transpose: median pitch %.0f Hz, -12 -> %.0f Hz, +12 -> %.0f Hz", f0, fd, fu);
        check(fd > 0 && fu > 0 && fd < f0 * 0.65 && fu > f0 * 1.5, what);
        snprintf(what, sizeof what, "vibrato changes the sound, hardly the timing: %.2f s with, %.2f s without", base.n / 22050.0, novib.n / 22050.0);
        check(!same(&base, &novib) && fabs((double)base.n - (double)novib.n) < 0.02 * base.n, what);
        snprintf(what, sizeof what, "rate 10 sings faster: %.2f s -> %.2f s", base.n / 22050.0, fast.n / 22050.0);
        check(fast.n > 0 && fast.n < base.n * 0.6, what);
        free(base.pcm); free(down.pcm); free(up.pcm); free(novib.pcm); free(fast.pcm);
    }
    /* 5. hostile text */
    {
        static const char *hostile[] = {
            "Party time \xF0\x9F\x98\x80\xF0\x9F\x8E\x89 yes.", "Go \xE2\x86\x92 next.", "\xE4\xBD\xA0\xE5\xA5\xBD.",
            "\xC3\x9F.", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.",
            "\xFF\xFE\x80 bad bytes.", "1234567890123456789012345678901234567890", "5555-5555-5555-5555-5555",
            "!!!???...,,,;;;", "tempo 0\n- -5\nx ZZ9 -1\nhello C99 1e9\n", "( [ { \" ' ) ] } \" '", "Mr. Dr. St. etc. e.g. i.e."};
        const char *tail = "This sentence must still be sung.";
        buf b0 = {0};
        sing(dir, "Sam", "none", 0, &DEF, tail, 0, &b0, NULL);
        for (i = 0; i < sizeof hostile / sizeof *hostile; i++) {
            char text[1024];
            buf b = {0};
            int rc;
            snprintf(text, sizeof text, "%s %s", hostile[i], tail);
            rc = sing(dir, "Sam", "none", 0, &DEF, text, 0, &b, NULL);
            snprintf(what, sizeof what, "hostile %zu: rc=%d, %.2f s (the tail alone %.2f s)", i, rc, b.n / 22050.0, b0.n / 22050.0);
            check(rc == 0 && b.n >= b0.n, what);
            free(b.pcm);
        }
        {   /* a hostile literal score */
            buf b = {0};
            int m = 0, rc = sing(dir, "Sam", "none", 0, &DEF, "tempo -5\nhello C4 1e30\n- 1e9\nx Q4 1\nhi C4 0 C4 -3\n", 1, &b, &m);
            snprintf(what, sizeof what, "hostile score: rc=%d, %.2f s", rc, b.n / 22050.0);
            check(rc >= 0 && b.n < 22050 * 600, what);
            free(b.pcm);
        }
        free(b0.pcm);
    }
    /* 6. long text: 20,000 letters */
    {
        char *big = malloc(20100);
        buf b = {0};
        double t0;
        int rc;
        size_t k = 0;
        static const char *s = "The quick brown fox jumps over the lazy dog, and then it sings a little song. ";
        while (k + strlen(s) < 20000) { memcpy(big + k, s, strlen(s)); k += strlen(s); }
        big[k] = 0;
        t0 = now_s();
        rc = sing(dir, "Mary", "hall", 5, &DEF, big, 0, &b, NULL);
        snprintf(what, sizeof what, "long text (%zu letters): rc=%d, %.1f s sung in %.1f s", k, rc, b.n / 22050.0, now_s() - t0);
        check(rc == 0 && b.n > 22050 * 60, what);
        free(b.pcm);
        free(big);
    }
    /* 7. cancel: stop after 1 s of audio; the voice sings normally afterwards */
    {
        char err[256];
        cv_voice *v = cv_voice_open(dir, "Sam", "none", 0, err, sizeof err);
        buf b = {0}, c = {0}, ref = {0};
        double t0 = now_s(), dt;
        int rc, rc2;
        b.stop_at = 22050;
        rc = cv_voice_sing(v, hello, 0, 0, &DEF, 0, 1, on_pcm, &b, NULL);
        dt = now_s() - t0;
        rc2 = cv_voice_sing(v, "Is that not stylish?", 0, 0, &DEF, 0, 1, on_pcm, &c, NULL);
        sing(dir, "Sam", "none", 0, &DEF, "Is that not stylish?", 0, &ref, NULL);
        snprintf(what, sizeof what, "cancel after 1 s: rc=%d, %.2f s delivered, returned in %.0f ms; next sing rc=%d %.2f s (fresh %.2f s)",
                 rc, b.n / 22050.0, dt * 1000, rc2, c.n / 22050.0, ref.n / 22050.0);
        /* audio arrives a sung sound at a time (a long note is one piece), so the stop lands within a note */
        check(rc == 1 && b.n < 22050 * 2.2 && dt < 0.5 && rc2 == 0 && fabs((double)c.n - (double)ref.n) < 0.03 * ref.n, what);
        cv_voice_close(v);
        free(b.pcm); free(c.pcm); free(ref.pcm);
    }
    /* 8. the voice speaks normally after singing (the synth's smoothing / vibrato settings are restored; its noise
     *    and filter state carry over as they do between any two utterances, so not bit-identical) */
    {
        char err[256];
        cv_voice *v = cv_voice_open(dir, "Mike", "none", 0, err, sizeof err);
        buf a = {0}, s = {0}, b = {0};
        cv_voice_speak(v, "Battery 65 percent.", 0, 0, 1, on_pcm, &a);
        cv_voice_sing(v, hello, 0, 0, &DEF, 0, 1, on_pcm, &s, NULL);
        cv_voice_speak(v, "Battery 65 percent.", 0, 0, 1, on_pcm, &b);
        snprintf(what, sizeof what, "speech after singing: %.2f s vs %.2f s before", a.n / 22050.0, b.n / 22050.0);
        check(a.n > 0 && fabs((double)a.n - (double)b.n) < 0.03 * a.n, what);
        cv_voice_close(v);
        free(a.pcm); free(s.pcm); free(b.pcm);
    }
    /* 10. finding song scores in ordinary text (cv_score_find): what sings, and what must stay speech */
    {
        static const struct { const char *text, *want; } S[] = {
            {"twin-kle C4 1 C4 1 twin-kle G4 1 G4 1", "[twin-kle C4 1 C4 1 twin-kle G4 1 G4 1]"},
            {"Here is a song: twin-kle C4 1 C4 1 twin-kle G4 1 G4 1 and that was it.",
             "[twin-kle C4 1 C4 1 twin-kle G4 1 G4 1]"},
            {"tempo 120 star G4 2", "[tempo 120 star G4 2]"},
            {"Listen tempo 90\ntwin-kle C4 1 C4 1\n- 1\nstar G4 2\nnice?", "[tempo 90\ntwin-kle C4 1 C4 1\n- 1\nstar G4 2]"},
            {"first how F4 1 I F4 1 won-der E4 1 E4 1 then words then are C4 2 you D4 1 what D4 1 end",
             "[how F4 1 I F4 1 won-der E4 1 E4 1][are C4 2 you D4 1 what D4 1]"},
            {"sharp F#3 1 flat Bb5 0.5 half A4 1.5", "[sharp F#3 1 flat Bb5 0.5 half A4 1.5]"},
            {"la C4 1 - 2 la D4 1 - 1 la E4 1", "[la C4 1 - 2 la D4 1 - 1 la E4 1]"}};
        /* everyday text that must never sing (at least 30) */
        static const char *const NO[] = {
            "Flight B6 2 is boarding at gate 14.", "Room C4 1 is on the left.", "My grade A5 3 is fine.",
            "Take vitamin B6 2 times a day.", "Gate B4 1 hour delay", "Bus A7 3 minutes away",
            "Row D5 4 seat 12", "Apartment C3 2 bedroom for rent", "A4 paper 2 sheets", "E3 2026 starts Monday",
            "1. e4 e5 2. Nf3 Nc6 3. Bb5 a6", "Qe2 Nf6 O-O Be7", "Model G7 3 and model F2 5", "Seats A1 2 and B2 3",
            "Meet at 10 30 or 11 45", "The time is 9 15 pm", "Exit D2 3 miles ahead", "Section B1 2 of the manual",
            "Part A 1 and Part B 2 and Part C 3", "Vitamin D3 1000 IU", "Call 555 1234 now", "C4 explosive 2 kg",
            "tempo 120 beats per minute", "set the tempo 90", "The B2 bomber flew 2 missions", "Use a C 4 cable",
            "note C4 1", "Keys C4 1, D4 1, E4 1.", "c4 1 d4 1 e4 1 f4 1", "Grade: A4 3; B5 2; C6 1",
            "hello C4 1 D4 1", "wifi 5 GHz channel 36", "Level E4 2 unlocked", "Coordinates B6 2 and C7 1",
            "It is 5 - 3 = 2", "pages 10 - 12", "Price $5 - 2 for members", "- 2 - 3 - 4", "Twinkle twinkle little star", "Room C4 1. Seats A1 2 and B2 3.", "Take B6 2. Then C4 1. Then D4 1."};
        for (i = 0; i < sizeof S / sizeof *S; i++) {
            cv_score_run r[8];
            char got[512] = "";
            int n = cv_score_find(S[i].text, r, 8), k;
            for (k = 0; k < n; k++) {
                size_t l = strlen(got);
                snprintf(got + l, sizeof got - l, "[%.*s]", (int)r[k].len, S[i].text + r[k].start);
            }
            snprintf(what, sizeof what, "score found %zu: %s", i, got);
            check(!strcmp(got, S[i].want), what);
        }
        {
            int bad = 0;
            for (i = 0; i < sizeof NO / sizeof *NO; i++) {
                cv_score_run r[4];
                if (cv_score_find(NO[i], r, 4)) {
                    printf("     sings but should not: \"%s\"\n", NO[i]);
                    bad++;
                }
            }
            snprintf(what, sizeof what, "%zu everyday texts (flights, rooms, grades, chess, model numbers, times...) never sing: %d do",
                     sizeof NO / sizeof *NO, bad);
            check(bad == 0 && sizeof NO / sizeof *NO >= 30, what);
        }
        {   /* hostile: 300,000 bytes of near-scores, linear */
            size_t L = 300000, k;
            char *h = malloc(L + 1);
            cv_score_run *r = malloc(sizeof *r * 1000);
            int n;
            double t0 = now_s();
            static const char pat[] = "la C4 1 - 2 tempo x B6 2 ";
            for (k = 0; k < L; k++) h[k] = pat[k % (sizeof pat - 1)];
            h[L] = 0;
            n = cv_score_find(h, r, 1000);
            snprintf(what, sizeof what, "hostile near-scores: 300,000 bytes scanned in %.1f ms, %d runs", (now_s() - t0) * 1000, n);
            check(now_s() - t0 < 0.5 && n >= 0, what);
            free(h);
            free(r);
        }
    }
    /* 11. written rests at the edges of a sung score survive the silence trim, at their written length */
    {
        buf core = {0}, tail = {0}, head = {0}, both = {0}, only = {0};
        const char *c = "tempo 120\nla C4 1\nla D4 1\nla E4 1\n";
        int r0 = sing(dir, "Sam", "none", 0, &DEF, c, 1, &core, NULL);
        int r1 = sing(dir, "Sam", "none", 0, &DEF, "tempo 120\nla C4 1\nla D4 1\nla E4 1\n- 4\n", 1, &tail, NULL);
        int r2 = sing(dir, "Sam", "none", 0, &DEF, "tempo 120\n- 2\nla C4 1\nla D4 1\nla E4 1\n", 1, &head, NULL);
        int r3 = sing(dir, "Sam", "none", 0, &DEF, "tempo 120 - 1 la C4 1 la D4 1 la E4 1 - 3", 1, &both, NULL);
        int r4 = sing(dir, "Sam", "none", 0, &DEF, "tempo 60\n- 2\n", 1, &only, NULL);
        double dt = (tail.n - (double)core.n) / 22050, dh = (head.n - (double)core.n) / 22050, db = (both.n - (double)core.n) / 22050;
        size_t k, lead0 = 0;
        for (k = 0; k < head.n && head.pcm[k] == 0; k++) lead0++;
        snprintf(what, sizeof what, "edge rests kept: trailing - 4 at 120 adds %.3f s (want 2), leading - 2 adds %.3f s (want 1, %.3f s of silence first), both (- 1 / - 3, flat) %.3f s (want 2), a rest alone %.2f s (want 2)",
                 dt, dh, lead0 / 22050.0, db, only.n / 22050.0);
        check(!r0 && !r1 && !r2 && !r3 && !r4 && fabs(dt - 2.0) < 0.04 && fabs(dh - 1.0) < 0.04 && lead0 >= 22050 && fabs(db - 2.0) < 0.04 &&
                  fabs(only.n / 22050.0 - 2.0) < 0.01, what);
        free(core.pcm); free(tail.pcm); free(head.pcm); free(both.pcm); free(only.pcm);
    }
    /* 12. a word with more syllables than notes ("succ-ess A3 2"): sung on its one note, counted, nothing breaks */
    {
        buf b = {0};
        int m = 0, rc = sing(dir, "Sam", "none", 0, &DEF, "succ-ess A3 2\nbeau-ti-ful C4 1\n", 1, &b, &m);
        snprintf(what, sizeof what, "more syllables than notes: rc=%d, %.2f s, %d mismatch(es)", rc, b.n / 22050.0, m);
        check(rc == 0 && b.n > 22050 && m == 2, what);
        free(b.pcm);
    }
    /* 9. empty / unsingable text: no audio, no stand-in */
    {
        buf b = {0};
        int rc = sing(dir, "Sam", "none", 0, &DEF, "\xF0\x9F\x98\x80", 0, &b, NULL);
        check(rc == 0 && b.n == 0, "emoji-only text sings nothing");
        rc = sing(dir, "Sam", "none", 0, &DEF, "", 0, &b, NULL);
        check(rc == 0 && b.n == 0, "empty text sings nothing");
        free(b.pcm);
    }
    printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
    return failures;
}

typedef struct {
    const char *dir, *voice, *effect, *text;
    int reps;
    unsigned long long hash;
    int ok;
} thr;

static void *thr_main(void *p)
{
    thr *a = p;
    char err[256];
    cv_voice *v = cv_voice_open(a->dir, a->voice, a->effect, 0, err, sizeof err);
    int i;
    a->hash = 1469598103934665603ull;
    a->ok = v != NULL;
    for (i = 0; v && i < a->reps; i++) {
        buf b = {0};
        cv_sing_settings c = DEF;
        size_t k;
        c.transpose = i % 3 - 1;
        if (cv_voice_sing(v, a->text, i % 2 ? 4 : 0, 0, &c, 0, 1, on_pcm, &b, NULL) != 0) a->ok = 0;
        for (k = 0; k < b.n * 2; k++) { a->hash ^= ((unsigned char *)b.pcm)[k]; a->hash *= 1099511628211ull; }
        free(b.pcm);
    }
    cv_voice_close(v);
    return NULL;
}

static int threads(const char *dir, int reps)
{
    thr pairs[2][2] = {{{dir, "Sam", "none", "Twinkle twinkle little star, how I wonder what you are.", reps, 0, 0},
                        {dir, "Mary", "hall", "Hello, my name is Mary. Is this not a lovely song?", reps, 0, 0}},
                       {{dir, "Mike", "none", "The quick brown fox jumps over the lazy dog.", reps, 0, 0},
                        {dir, "Mike", "robosoft3", "One two three four five six seven.", reps, 0, 0}}};
    int k;
    for (k = 0; k < 2; k++) {
        thr alone[2] = {pairs[k][0], pairs[k][1]};
        pthread_t th[2];
        char what[256];
        thr_main(&alone[0]);
        thr_main(&alone[1]);
        pthread_create(&th[0], NULL, thr_main, &pairs[k][0]);
        pthread_create(&th[1], NULL, thr_main, &pairs[k][1]);
        pthread_join(th[0], NULL);
        pthread_join(th[1], NULL);
        snprintf(what, sizeof what, "%s + %s singing on two threads, %d songs each: identical to each alone", pairs[k][0].voice,
                 pairs[k][1].voice, reps);
        check(pairs[k][0].ok && pairs[k][1].ok && pairs[k][0].hash == alone[0].hash && pairs[k][1].hash == alone[1].hash, what);
    }
    printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
    return failures;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[2], "selftest")) return selftest(argv[1], argv[3]) != 0;
    if (argc >= 3 && !strcmp(argv[2], "threads")) return threads(argv[1], argc > 3 ? atoi(argv[3]) : 2) != 0;
    if (argc >= 8 && !strcmp(argv[2], "say")) {
        buf b = {0};
        int m = 0, rc = sing(argv[1], argv[3], argv[4], atof(argv[5]), &DEF, argv[6], argc > 8, &b, &m);
        write_wav(argv[7], &b);
        printf("rc=%d %.2f s, mismatches %d\n", rc, b.n / 22050.0, m);
        return rc < 0;
    }
    fprintf(stderr, "usage: see the top of sing_test.c\n");
    return 2;
}
