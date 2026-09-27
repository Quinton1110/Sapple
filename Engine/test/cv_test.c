/* Mac test driver for the ClassicVoices bridge: the same C the iOS targets compile.
 *
 *   cv_test DATA_DIR sanitize "text"                 print the sanitized text
 *   cv_test DATA_DIR say VOICE EFFECT PITCH RATE SEMI "text" out.wav [notrim]
 *   cv_test DATA_DIR selftest OUTDIR                  robustness checks (rc, sample counts) + WAVs
 */
#include "cv_bridge.h"
#include "sam_tts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE *f;
    size_t n;
} sink;

static void put(FILE *f, unsigned v, int n)
{
    int i;
    for (i = 0; i < n; i++) fputc((int)((v >> (8 * i)) & 0xFF), f);
}

static void header(FILE *f, unsigned bytes, unsigned rate)
{
    fwrite("RIFF", 1, 4, f);
    put(f, 36 + bytes, 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put(f, 16, 4);
    put(f, 1, 2);
    put(f, 1, 2);
    put(f, rate, 4);
    put(f, rate * 2, 4);
    put(f, 2, 2);
    put(f, 16, 2);
    fwrite("data", 1, 4, f);
    put(f, bytes, 4);
}

static int on_pcm(const int16_t *pcm, size_t n, void *user)
{
    sink *s = user;
    size_t i;
    if (s->f)
        for (i = 0; i < n; i++) put(s->f, (unsigned)(uint16_t)pcm[i], 2);
    s->n += n;
    return 0;
}

static int say(const char *dir, const char *voice, const char *effect, double pitch, double rate, double semi,
               const char *text, const char *out, int trim, size_t *samples)
{
    char err[256];
    cv_voice *v = cv_voice_open(dir, voice, effect, pitch, err, sizeof err);
    sink s = {0};
    int rc;
    if (!v) {
        fprintf(stderr, "open failed: %s\n", err);
        return -2;
    }
    if (out) {
        s.f = fopen(out, "wb");
        if (!s.f) {
            cv_voice_close(v);
            return -3;
        }
        header(s.f, 0, 22050);
    }
    rc = cv_voice_speak(v, text, rate, semi, trim, on_pcm, &s);
    if (s.f) {
        fseek(s.f, 0, SEEK_SET);
        header(s.f, (unsigned)(s.n * 2), 22050);
        fclose(s.f);
    }
    cv_voice_close(v);
    if (samples) *samples = s.n;
    return rc;
}

static int count_pcm(const int16_t *pcm, size_t n, void *user)
{
    *(size_t *)user += n;
    return 0;
}

/* straight through the engine, no sanitizer: the engine patch alone must keep the rest of the text */
static int raw_engine_fx(const char *dir, const char *voice, const char *effect, const char *text, size_t *samples)
{
    char err[256];
    sam_speech *t = sam_tts_open(dir, voice, err, sizeof err);
    sam_callbacks cb;
    int rc;
    *samples = 0;
    if (!t) return -2;
    if (effect) sam_tts_set_effect(t, effect);
    cb.audio = count_pcm;
    cb.event = NULL;
    cb.user = samples;
    rc = sam_tts_speak(t, text, 0, &cb);
    sam_tts_close(t);
    return rc;
}

static int raw_engine(const char *dir, const char *text, size_t *samples)
{
    return raw_engine_fx(dir, "Sam", NULL, text, samples);
}

static int failures;

static void check(int ok, const char *what)
{
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[2], "sanitize")) {
        char *c = cv_sanitize_alloc(argv[3]);
        printf("[%s]\n", c ? c : "(null)");
        cv_free(c);
        return 0;
    }
    if (argc >= 10 && !strcmp(argv[2], "say")) {
        size_t n = 0;
        int rc = say(argv[1], argv[3], argv[4], atof(argv[5]), atof(argv[6]), atof(argv[7]), argv[8], argv[9],
                     !(argc > 10 && !strcmp(argv[10], "notrim")), &n);
        printf("rc=%d samples=%zu (%.2f s)\n", rc, n, n / 22050.0);
        return rc < 0;
    }
    if (argc >= 4 && !strcmp(argv[2], "selftest")) {
        const char *dir = argv[1], *od = argv[3];
        char path[1024];
        size_t base = 0, n = 0;
        int rc;
        /* the sentence every hostile prefix must leave intact */
        const char *tail = "This sentence must still be spoken.";
        static const char *hostile[] = {
            "Party time \xF0\x9F\x98\x80\xF0\x9F\x8E\x89 yes.",          /* emoji */
            "Go \xE2\x86\x92 next.",                                   /* right arrow */
            "Copyright \xC2\xA9 2024.",                                /* copyright */
            "\xE4\xBD\xA0\xE5\xA5\xBD \xE4\xB8\x96\xE7\x95\x8C.",       /* Chinese */
            "\xC3\x9F.",                                               /* lone sharp s: killed the real engine */
            "\xC3\x9F\xC3\x9F \xC3\x9F x.",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.",
            "\xCE\xB1\xCE\xB2\xCE\xB3 \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82.", /* Greek, Cyrillic */
            "\xEF\xBF\xBD\xEF\xBB\xBF\xE2\x80\x8B.",                   /* U+FFFD, BOM, ZWSP */
            "\xFF\xFE\x80 bad bytes.",                                 /* invalid UTF-8 */
        };
        size_t i;
        rc = say(dir, "Sam", "none", 0, 0, 0, tail, NULL, 1, &base);
        check(rc == 0 && base > 22050, "baseline sentence speaks");
        for (i = 0; i < sizeof hostile / sizeof *hostile; i++) {
            char text[1024], what[1200];
            snprintf(text, sizeof text, "%s %s", hostile[i], tail);
            snprintf(path, sizeof path, "%s/hostile%zu.wav", od, i);
            rc = say(dir, "Sam", "none", 0, 0, 0, text, path, 1, &n);
            snprintf(what, sizeof what, "hostile %zu: rc=%d, %zu samples (tail alone %zu)", i, rc, n, base);
            check(rc == 0 && n >= base, what);
        }
        {
            /* the original engine rejects the whole call at a lone sharp s (SPERR_NOT_IN_LEX) and at a
             * word of more than 127 letters; the patched engine must speak everything around them */
            size_t with = 0, without = 0, longw = 0;
            char big[400];
            int r1 = raw_engine(dir, "Hello. \xC3\x9F. Must speak. Also this.", &with);
            int r2 = raw_engine(dir, "Hello. Must speak. Also this.", &without);
            memset(big, 'a', 200);
            snprintf(big + 200, sizeof big - 200, ". Must speak.");
            int r3 = raw_engine(dir, big, &longw);
            snprintf(path, sizeof path, "engine patch alone: rc %d/%d/%d, %zu samples with the sharp s, %zu without, %zu long word",
                     r1, r2, r3, with, without, longw);
            check(r1 == 0 && r2 == 0 && r3 == 0 && with >= without && longw > 0, path);
        }
        rc = say(dir, "Mike", "hall", 0, 0, 0, "", NULL, 1, &n);
        check(rc == 0 && n == 0, "empty text gives no audio and no stand-in text");
        rc = say(dir, "Mary", "none", 0, 0, 0, " \xF0\x9F\x98\x80 \xE2\x80\x8B ", NULL, 1, &n);
        check(rc == 0 && n == 0, "emoji-only text gives no audio");
        {
            size_t trimmed = 0, raw = 0;
            /* raw = the unpatched library path (full padding, full 1 s echo tail) */
            say(dir, "Sam", "none", 0, 0, 0, "5", NULL, 1, &trimmed);
            raw_engine_fx(dir, "Sam", NULL, "5", &raw);
            snprintf(path, sizeof path, "digit trim: %zu -> %zu samples (%.0f ms saved)", raw, trimmed,
                     (raw - trimmed) / 22.05);
            check(trimmed > 0 && trimmed < raw, path);
            say(dir, "Mike", "hall", 0, 0, 0, "5", NULL, 1, &trimmed);
            raw_engine_fx(dir, "Mike", "hall", "5", &raw);
            snprintf(path, sizeof path, "hall digit trim: %zu -> %zu samples (%.0f ms saved)", raw, trimmed,
                     (raw - trimmed) / 22.05);
            check(trimmed > 0 && trimmed < raw, path);
        }
        {
            size_t slow = 0, norm = 0, fast = 0, frac = 0;
            say(dir, "Sam", "none", 0, -10, 0, tail, NULL, 1, &slow);
            say(dir, "Sam", "none", 0, 0, 0, tail, NULL, 1, &norm);
            say(dir, "Sam", "none", 0, 10, 0, tail, NULL, 1, &fast);
            say(dir, "Sam", "none", 0, 5.5, 0, tail, NULL, 1, &frac);
            snprintf(path, sizeof path, "rate -10/0/5.5/10: %zu %zu %zu %zu samples", slow, norm, frac, fast);
            check(slow > norm && norm > frac && frac > fast, path);
        }
        printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
        return failures != 0;
    }
    fprintf(stderr, "usage: see the comment at the top of cv_test.c\n");
    return 1;
}
