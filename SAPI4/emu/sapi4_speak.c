// Command-line driver for the sapi4_tts library (Mac, development and `make check`).
//   sapi4_speak -d <engine dir> [-m "Mode name"] [-p pitch] [-s speed] [-t] [-o out.wav] [-l] [-v] "text"
//   -t: the text contains SAPI 4 tags (\Pit=\ ...);  -r N: speak it N times on one engine (last one saved)
#include "sapi4_tts.h"
#include "emu.h"
#ifdef X86_LOCKSTEP
void emu_lockstep_enable(Emu *e);
void emu_lockstep_report(void);
#endif
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static double now_s(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }

typedef struct { int16_t *pcm; size_t n, cap; double t_first; } Buf;
static int on_pcm(const int16_t *pcm, size_t n, void *user) {
    Buf *b = user;
    if (!b->t_first) b->t_first = now_s();
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 65536;
        b->pcm = realloc(b->pcm, b->cap * sizeof *b->pcm);
    }
    memcpy(b->pcm + b->n, pcm, n * sizeof *pcm);
    b->n += n;
    return 0;
}

static void write_wav(const char *path, const int16_t *pcm, size_t n, uint32_t rate) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    uint32_t datasz = (uint32_t)(n * 2), riff = 36 + datasz, br = rate * 2, fmtsz = 16;
    uint16_t pcmfmt = 1, ch = 1, ba = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtsz, 4, 1, f); fwrite(&pcmfmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&datasz, 4, 1, f); fwrite(pcm, 2, n, f);
    fclose(f);
}

static void usage(void) {
    fprintf(stderr, "usage: sapi4_speak -d engine_dir [-m mode] [-p pitch] [-s speed] [-t] [-r repeat] [-o out.wav] [-l] text\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *dir = NULL, *mode = "Sam", *out = "out.wav";
    int pitch = -1, speed = -1, list = 0, tagged = 0, repeat = 1, opt;
    while ((opt = getopt(argc, argv, "d:m:p:s:o:r:ltv")) != -1) {
        switch (opt) {
        case 'd': dir = optarg; break;
        case 'm': mode = optarg; break;
        case 'p': pitch = atoi(optarg); break;
        case 's': speed = atoi(optarg); break;
        case 'o': out = optarg; break;
        case 'r': repeat = atoi(optarg); break;
        case 'l': list = 1; break;
        case 't': tagged = 1; break;
        case 'v': setenv("SAPI4_VERBOSE", "1", 1); break;
        default: usage();
        }
    }
    if (!dir || (!list && optind >= argc)) usage();
    char err[256];
    if (list) {
        s4_mode modes[32];
        int n = s4_list_modes(dir, modes, 32, err, sizeof err);
        if (n < 0) { fprintf(stderr, "%s\n", err); return 1; }
        for (int i = 0; i < n; i++) printf("%-24s speaker=%-22s gender=%d features=%08x\n", modes[i].name, modes[i].speaker, modes[i].gender, modes[i].features);
        return 0;
    }
    const char *text = argv[optind];
    double t0 = now_s();
    s4_engine *s = s4_open(dir, mode, err, sizeof err);
    if (!s) { fprintf(stderr, "open failed: %s\n", err); return 1; }
    double t_open = now_s();
    s4_limits lim;
    s4_get_limits(s, &lim);
    fprintf(stderr, "mode '%s' rate %d Hz: pitch %u (%u..%u), speed %u (%u..%u) wpm\n", s4_mode_info(s)->name, s4_sample_rate(s),
            lim.pitch_default, lim.pitch_min, lim.pitch_max, lim.speed_default, lim.speed_min, lim.speed_max);
    if (pitch >= 0) s4_set_pitch(s, (unsigned)pitch);
    if (speed >= 0) s4_set_speed(s, (unsigned)speed);
    Buf b = { 0 };
    int rc = 0;
    double t1 = 0, t2 = 0;
    for (int r = 0; r < repeat; r++) {
        b.n = 0; b.t_first = 0;
        t1 = now_s();
        rc = s4_speak(s, text, tagged ? S4_TAGGED : 0, on_pcm, &b);
        t2 = now_s();
        if (rc < 0) { fprintf(stderr, "speak failed: %s\n", s4_error(s)); break; }
    }
    s4_stats st;
    s4_last_stats(s, &st);
    int rate = s4_sample_rate(s);
    double secs = (double)b.n / rate;
    fprintf(stderr, "audio: %zu samples, %.3f s at %d Hz; synth wall %.3f s (RTF %.1fx real time); open %.1f ms\n",
            b.n, secs, rate, t2 - t1, secs / (t2 - t1 + 1e-9), (t_open - t0) * 1e3);
    fprintf(stderr, "insns: %llu (%.1f M/s), host calls %llu, heap live %llu, heap top %08x; first PCM after %.1f ms; done flags AudioStop=%d TextDataDone=%d\n",
            (unsigned long long)st.insns, st.insns / (t2 - t1 + 1e-9) / 1e6, (unsigned long long)st.host_calls,
            (unsigned long long)st.heap_live, st.heap_top, b.t_first ? (b.t_first - t1) * 1e3 : -1.0, st.audio_stop_seen, st.textdata_done_seen);
    if (b.n) write_wav(out, b.pcm, b.n, (uint32_t)rate);
#ifdef X86_LOCKSTEP
    emu_lockstep_report();
#endif
    s4_close(s);
    free(b.pcm);
    return rc < 0 ? 1 : 0;
}
