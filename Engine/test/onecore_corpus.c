/* onecore_corpus.c - render the upstream's three corpora (fe1/corpus.txt, fe2/corpus.txt, backend/corpus.txt of
 * Engine/ms-david-zira-decomp/tests) through the OneCore library and print one PCM hash per render, so an engine
 * change can be proven to leave the voices bit-identical:
 *     onecore_corpus <data dir> <voice> <corpus files...>  > hashes.txt
 * Every line plain; 1 in 10 also at rates -10 -5 5 10 15 18 20, fractional -7.3 2.5 12.7, pitch -10 / +10,
 * volume 40 and (voices with an [EmotionRecipe]) each emotion; 1 in 7 also as SAPI XML.  The same scheme the
 * en-GB locale patch was verified with.  Output: "<voice> <line> <variant> <samples> <fnv64>". */
#include "zira_tts.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t h; long n; } Acc;
static int on_audio(const short *pcm, size_t n, void *u) {
    Acc *a = (Acc *)u;
    const unsigned char *b = (const unsigned char *)pcm;
    for (size_t i = 0; i < n * 2; i++) { a->h ^= b[i]; a->h *= 1099511628211ull; }
    a->n += (long)n;
    return 0;
}

static void render(zira_tts *t, const char *voice, int line, const char *var, const char *text, int flags) {
    zira_callbacks cb;
    Acc a = {1469598103934665603ull, 0};
    memset(&cb, 0, sizeof cb);
    cb.audio = on_audio;
    cb.user = &a;
    int rc = zira_tts_speak(t, text, flags, &cb);
    printf("%s %d %s %ld %016llx rc=%d\n", voice, line, var, a.n, (unsigned long long)a.h, rc);
}

int main(int argc, char **argv) {
    char err[512], buf[65536];
    if (argc < 4) { fprintf(stderr, "usage: %s <data dir> <voice> <corpus files...>\n", argv[0]); return 2; }
    zira_tts *t = zira_tts_open(argv[1], argv[2], err, sizeof err);
    if (!t) { fprintf(stderr, "%s: %s\n", argv[2], err); return 1; }
    int line = 0;
    for (int fi = 3; fi < argc; fi++) {
        FILE *f = fopen(argv[fi], "r");
        if (!f) { fprintf(stderr, "cannot open %s\n", argv[fi]); return 1; }
        while (fgets(buf, sizeof buf, f)) {
            size_t n = strlen(buf);
            while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
            if (!n || buf[0] == '#') continue;
            const char *text = buf;
            if (!strncmp(text, "@rate=", 6) || !strncmp(text, "@vol=", 5)) { const char *sp = strchr(text, ' '); if (!sp) continue; text = sp + 1; }
            line++;
            zira_tts_set_rate(t, 0); zira_tts_set_pitch(t, 0); zira_tts_set_volume(t, 100); zira_tts_set_emotion(t, "");
            render(t, argv[2], line, "plain", text, 0);
            if (line % 10 == 0) {
                static const int rates[] = {-10, -5, 5, 10, 15, 18, 20};
                static const double frates[] = {-7.3, 2.5, 12.7};
                char var[32];
                for (size_t k = 0; k < sizeof rates / sizeof *rates; k++) {
                    zira_tts_set_rate(t, rates[k]); snprintf(var, sizeof var, "rate%d", rates[k]); render(t, argv[2], line, var, text, 0);
                }
                for (size_t k = 0; k < sizeof frates / sizeof *frates; k++) {
                    zira_tts_set_rate_f(t, frates[k]); snprintf(var, sizeof var, "rate%.1f", frates[k]); render(t, argv[2], line, var, text, 0);
                }
                zira_tts_set_rate(t, 0);
                zira_tts_set_pitch(t, -10); render(t, argv[2], line, "pitch-10", text, 0);
                zira_tts_set_pitch(t, 10); render(t, argv[2], line, "pitch10", text, 0);
                zira_tts_set_pitch(t, 0);
                zira_tts_set_volume(t, 40); render(t, argv[2], line, "vol40", text, 0);
                zira_tts_set_volume(t, 100);
                for (int k = 0; zira_tts_emotion_name(t, k); k++) {
                    const char *e = zira_tts_emotion_name(t, k);
                    zira_tts_set_emotion(t, e); snprintf(var, sizeof var, "emo_%s", e); render(t, argv[2], line, var, text, 0);
                }
                zira_tts_set_emotion(t, "");
            }
            if (line % 7 == 0) render(t, argv[2], line, "xml", text, ZIRA_SPEAK_XML);
            fflush(stdout);
        }
        fclose(f);
    }
    zira_tts_close(t);
    return 0;
}
