/* Mac test driver for the TruVoice bridge (Shared/Bridge/cvt_bridge.c on OpenTV, Engine/opentv): the exact C the iOS
 * targets compile - the engine, its data image (TruVoiceData/tvdata.s), cvt_bridge and the SAPI 5 bridge's sanitizer /
 * trimmer / resampler - driven the way ClassicEngine.swift drives it.
 *
 *   truvoice_test say "Mode" RATE SEMITONES "text" out.wav [notrim]
 *   truvoice_test selftest OUTDIR          every check below; exit status = failures
 *   truvoice_test bench "Mode" [seconds]   real-time factor on one warm voice
 *   truvoice_test threads N                N rounds of the two-thread checks (for the TSan build)
 *   truvoice_test memory                   footprint over 300 utterances (plain build: ASan's quarantine inflates it)
 *
 * Built with -DCV_TRUVOICE_AB (make truvoice-ab) it also links the old path - tv_enua.dll in the SAPI 4 interpreter
 * through cv4_bridge - and adds:
 *   truvoice_ab ab TV_ENUA_DIR OUTDIR      the same requests through both bridges (every voice, rates, pitches, texts):
 *                                          identical / different, sample counts, correlation; before/after WAVs
 *   truvoice_ab ratefit TV_ENUA_DIR        re-measures cvt_bridge.c's RATE_ROWS table (engine level, both builds)
 */
#include "cvt_bridge.h"
#include "tvtts.h"
#ifdef CV_TRUVOICE_AB
#include "cv4_bridge.h"
#include "sapi4_tts.h"
#endif

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

static int speak(cvt_voice *v, const char *text, double rate, double semi, int trim, sink *s)
{
    s->n = 0;
    s->t_first = s->t_stop = 0;
    s->calls = 0;
    return cvt_voice_speak(v, text, rate, semi, trim, on_pcm, s);
}

static int same_pcm(const sink *a, const sink *b)
{
    return a->n == b->n && (!a->n || !memcmp(a->pcm, b->pcm, a->n * 2));
}

static double peak_abs(const sink *s)
{
    int m = 0;
    size_t i;
    for (i = 0; i < s->n; i++) m = abs(s->pcm[i]) > m ? abs(s->pcm[i]) : m;
    return m;
}

/* rough F0: median over loud 40 ms frames of the first strong normalised-autocorrelation peak (as cv4_test.c) */
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

static const char *const MODES[10] = {
    "Adult Male #1, American English (TruVoice)", "Adult Male #2, American English (TruVoice)",
    "Adult Male #3, American English (TruVoice)", "Adult Male #4, American English (TruVoice)",
    "Adult Male #5, American English (TruVoice)", "Adult Male #6, American English (TruVoice)",
    "Adult Male #7, American English (TruVoice)", "Adult Male #8, American English (TruVoice)",
    "Adult Female #1, American English (TruVoice)", "Adult Female #2, American English (TruVoice)",
};
static const char *TAIL = "This sentence must still be spoken.";
static const char *LONG = "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.";

/* ------------------------------------------------------------------ concurrency */
typedef struct {
    const char *mode, *text;
    int reps;
    sink s;
    int rc;
} job;

static void *run_job(void *p)
{
    job *j = p;
    char err[256];
    cvt_voice *v = cvt_voice_open(j->mode, err, sizeof err);
    int k;
    j->rc = -2;
    if (!v) return NULL;
    for (k = 0; k < j->reps; k++) j->rc = speak(v, j->text, 0, 0, 1, &j->s);
    cvt_voice_close(v);
    return NULL;
}

static int threads_round(void)
{
    /* two different voices, and the same voice twice, on two threads: each identical to running alone */
    static const char *pairs[][2] = { { MODES[9], MODES[1] }, { MODES[0], MODES[0] }, { MODES[8], MODES[5] } };
    int ok = 1;
    size_t p;
    for (p = 0; p < 3; p++) {
        job a = { pairs[p][0], LONG, 3, {0}, 0 }, b = { pairs[p][1], "Second utterance, 12:30 PM, $4.99.", 3, {0}, 0 };
        job a1 = a, b1 = b;
        pthread_t ta, tb;
        run_job(&a1);
        run_job(&b1);
        pthread_create(&ta, NULL, run_job, &a);
        pthread_create(&tb, NULL, run_job, &b);
        pthread_join(ta, NULL);
        pthread_join(tb, NULL);
        ok = ok && a.rc == 0 && b.rc == 0 && same_pcm(&a.s, &a1.s) && same_pcm(&b.s, &b1.s) && a.s.n > 0 && b.s.n > 0;
        free(a.s.pcm); free(b.s.pcm); free(a1.s.pcm); free(b1.s.pcm);
    }
    return ok;
}

#ifdef CV_TRUVOICE_AB
/* ------------------------------------------------------------------ old path vs new path */
static int speak4(cv4_voice *v, const char *text, double rate, double semi, int trim, sink *s)
{
    s->n = 0;
    s->t_first = s->t_stop = 0;
    s->calls = 0;
    return cv4_voice_speak(v, text, rate, semi, trim, on_pcm, s);
}

static double corr(const sink *a, const sink *b)
{
    size_t n = a->n < b->n ? a->n : b->n, i;
    double sab = 0, saa = 0, sbb = 0;
    for (i = 0; i < n; i++) {
        sab += (double)a->pcm[i] * b->pcm[i];
        saa += (double)a->pcm[i] * a->pcm[i];
        sbb += (double)b->pcm[i] * b->pcm[i];
    }
    return saa > 0 && sbb > 0 ? sab / sqrt(saa * sbb) : (saa == sbb ? 1 : 0);
}

static int ab(const char *dir, const char *od)
{
    static const char *texts[] = {
        "Hello, my name is Microsoft Sam.",
        "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.",
        "On March 3rd, 2021, I paid $45.99 for 3 items at 10:30 AM; call 555-1234. Dr. Smith's e-mail isn't working!",
        "Speech", "button", "5", "A", "65% battery power, charging.",
        "Caf\xC3\xA9 na\xC3\xAFve r\xC3\xA9sum\xC3\xA9, Euro \xE2\x82\xAC 5 \xE2\x80\x94 \xE2\x80\x9Cquoted\xE2\x80\x9D.",
        "\\Pit=400\\ \\Spd=450\\ C:\\Windows <rate speed=\"10\"> ~!@#$%^&*()",
        "Party time \xF0\x9F\x98\x80 yes. \xE4\xBD\xA0\xE5\xA5\xBD.",
    };
    /* VoiceOver's default (no rate: 0), the ends of the scale, and rates on both sides of the identical band */
    static const double rates[] = { 0, -10, -6, -3, 1.5, 2.5, 4, 6, 8, 18 };
    static const double semis[] = { 0, -12, -5, 3, 12 };
    size_t t, r, p;
    int i, tot = 0, same = 0, same_len = 0, samples_equal_default = 0, tot_default = 0;
    double worst_len = 0, min_corr_same_len = 1;
    enum { NR = sizeof rates / sizeof *rates };
    int r_tot[NR] = {0}, r_same[NR] = {0};
    double r_dur_old[NR] = {0}, r_dur_new[NR] = {0}, r_worst[NR] = {0};
    char err[256], path[1200];
    for (i = 0; i < 10; i++) {
        cv4_voice *o = cv4_voice_open(dir, MODES[i], err, sizeof err);
        cvt_voice *n = cvt_voice_open(MODES[i], err, sizeof err);
        if (!o || !n) { printf("FAIL open %s: %s\n", MODES[i], err); return 1; }
        for (t = 0; t < sizeof texts / sizeof *texts; t++)
            for (r = 0; r < sizeof rates / sizeof *rates; r++)
                for (p = 0; p < sizeof semis / sizeof *semis; p++) {
                    sink a = {0}, b = {0};
                    int ra, rb, eq;
                    if (p && (r % 3) && t != 1) continue;   /* pitch variants on a subset of rates / texts */
                    ra = speak4(o, texts[t], rates[r], semis[p], 1, &a);
                    rb = speak(n, texts[t], rates[r], semis[p], 1, &b);
                    eq = ra == rb && same_pcm(&a, &b);
                    tot++;
                    same += eq;
                    r_tot[r]++;
                    r_same[r] += eq;
                    r_dur_old[r] += a.n;
                    r_dur_new[r] += b.n;
                    if (a.n > 2205 && fabs((double)b.n / a.n - 1) > r_worst[r]) r_worst[r] = fabs((double)b.n / a.n - 1);
                    if (rates[r] == 0) { tot_default++; samples_equal_default += eq; }
                    if (a.n == b.n) {
                        same_len++;
                        if (!eq && corr(&a, &b) < min_corr_same_len) min_corr_same_len = corr(&a, &b);
                    } else if (a.n) {
                        double d = fabs((double)b.n / a.n - 1);
                        if (d > worst_len) worst_len = d;
                    }
                    if (!eq && (rates[r] == 0 || getenv("AB_VERBOSE")))
                        printf("diff: %s rate %g st %g \"%.24s\": old %zu new %zu samples, corr %.4f (rc %d/%d)\n", MODES[i],
                               rates[r], semis[p], texts[t], a.n, b.n, corr(&a, &b), ra, rb);
                    if (od && t == 1 && p == 0 && (r == 0 || r == 6) && (i == 0 || i == 1 || i == 8)) {
                        snprintf(path, sizeof path, "%s/%s_rate%g_old_tv_enua.wav", od, i == 8 ? "female1" : i ? "male2" : "male1",
                                 rates[r]);
                        write_wav(path, a.pcm, a.n, 22050);
                        snprintf(path, sizeof path, "%s/%s_rate%g_new_opentv.wav", od, i == 8 ? "female1" : i ? "male2" : "male1",
                                 rates[r]);
                        write_wav(path, b.pcm, b.n, 22050);
                    }
                    free(a.pcm);
                    free(b.pcm);
                }
        cv4_voice_close(o);
        cvt_voice_close(n);
    }
    for (r = 0; r < NR; r++)
        printf("rate %5g (SAPI %3u wpm -> OpenTV %3u): %3d / %3d identical, total duration new/old %.3f, worst single render "
               "(> 0.1 s) %+.1f%%\n", rates[r], (unsigned)lrint(fmin(250, fmax(50, 150 * pow(3, rates[r] / 10)))),
               cvt_engine_wpm((unsigned)lrint(fmin(250, fmax(50, 150 * pow(3, rates[r] / 10))))), r_same[r], r_tot[r],
               r_dur_new[r] / r_dur_old[r], r_worst[r] * 100);
    printf("A/B through both bridges (22050 Hz, trimmed): %d / %d renders byte-identical; at VoiceOver's default rate "
           "%d / %d; same length %d / %d (lowest correlation among same-length differing renders %.4f); worst length "
           "difference %.1f%%\n", same, tot, samples_equal_default, tot_default, same_len, tot, min_corr_same_len,
           worst_len * 100);
    return samples_equal_default != tot_default;
}

/* engine level, like the old path's tv_enua.dll SpeedSet and OpenTV's rate rows: prints the RATE_ROWS table */
static size_t s4_len_cb_n;
static int s4_count(const int16_t *pcm, size_t n, void *u) { (void)pcm; (void)u; s4_len_cb_n += n; return 0; }
static size_t tv_len_n;
static int tv_count(const tvtts_event *ev, void *u) { (void)u; if (ev->type == TVTTS_AUDIO) tv_len_n += ev->count; return 0; }
static int ratefit(const char *dir)
{
    static const char *T[] = { "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.",
                               "On March 3rd, 2021, I paid $45.99 for 3 items at 10:30 AM; call 555-1234.",
                               "Speech button. 65% battery power, charging.",
                               "Hello, my name is Microsoft Sam. Is that not stylish?" };
    char err[256];
    size_t rows[26], t;
    int w, r, last = -1;
    s4_engine *s = s4_open(dir, MODES[0], err, sizeof err);
    tvtts_synth *n;
    if (!s) { printf("open: %s\n", err); return 1; }
    tvtts_set_extensions(0);
    n = tvtts_create(11025);
    tvtts_set_voice(n, 0);
    tvtts_set_pitch(n, 85);
    for (r = 0; r < 26; r++) {
        tvtts_set_rate(n, 46 + 8 * r);
        tv_len_n = 0;
        for (t = 0; t < 4; t++) tvtts_speak_bytes(n, T[t], (uint32_t)strlen(T[t]), tv_count, NULL);
        rows[r] = tv_len_n;
    }
    printf("{from SAPI wpm, row}: ");
    for (w = 50; w <= 250; w++) {
        size_t o = 0;
        int best = 0;
        s4_set_speed(s, (unsigned)w);
        for (t = 0; t < 4; t++) {
            s4_len_cb_n = 0;
            s4_speak(s, T[t], 0, s4_count, NULL);
            o += s4_len_cb_n;
        }
        for (r = 1; r < 26; r++)
            if (fabs(log((double)rows[r] / o)) < fabs(log((double)rows[best] / o))) best = r;
        if (best != last) printf("{%d, %d}, ", w, best);
        last = best;
    }
    printf("\n");
    tvtts_destroy(n);
    s4_close(s);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    char err[256], path[1200], what[1400];
    if (argc >= 7 && !strcmp(argv[1], "say")) {
        cvt_voice *v = cvt_voice_open(argv[2], err, sizeof err);
        sink s = {0};
        int rc;
        double t0;
        if (!v) { fprintf(stderr, "open failed: %s\n", err); return 2; }
        t0 = now_s();
        rc = speak(v, argv[5], atof(argv[3]), atof(argv[4]), !(argc > 7 && !strcmp(argv[7], "notrim")), &s);
        printf("rc=%d samples=%zu (%.2f s) in %.3f s, SAPI wpm %u -> OpenTV wpm %u, pitch %u\n", rc, s.n, s.n / 22050.0,
               now_s() - t0, cvt_voice_wpm_for_rate(v, atof(argv[3])), cvt_engine_wpm(cvt_voice_wpm_for_rate(v, atof(argv[3]))),
               cvt_voice_pitch_for_semitones(v, atof(argv[4])));
        write_wav(argv[6], s.pcm, s.n, 22050);
        cvt_voice_close(v);
        return rc < 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "bench")) {
        double t0 = now_s(), secs = argc > 3 ? atof(argv[3]) : 3.0, audio = 0, wall = 0, t_open, mem0 = footprint_mb();
        cvt_voice *v = cvt_voice_open(argv[2], err, sizeof err);
        sink s = {0};
        int n = 0;
        if (!v) { fprintf(stderr, "open failed: %s\n", err); return 2; }
        t_open = now_s() - t0;
        t0 = now_s();
        speak(v, LONG, 0, 0, 1, &s);
        printf("%-44s open %.2f ms, first audio %.1f ms after the call", argv[2], t_open * 1e3, (s.t_first - t0) * 1e3);
        t0 = now_s();
        while (now_s() - t0 < secs) {
            double t1 = now_s();
            speak(v, LONG, 0, 0, 0, &s);
            wall += now_s() - t1;
            audio += s.n / 22050.0;
            n++;
        }
        printf(", %.1fx real time (%d utterances), footprint +%.1f MB\n", audio / wall, n, footprint_mb() - mem0);
        cvt_voice_close(v);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "memory")) {
        /* plain build only: under ASan the footprint measures the sanitizer's quarantine */
        cvt_voice *v = cvt_voice_open(MODES[0], err, sizeof err);
        sink s = {0};
        double m0, m1, m2;
        int k;
        if (!v) return 2;
        for (k = 0; k < 10; k++) speak(v, LONG, k % 5, 0, 1, &s);
        m0 = footprint_mb();
        for (k = 0; k < 60; k++) speak(v, k % 2 ? LONG : "Second utterance, 12:30 PM, $4.99.", k % 7 - 3, k % 5 - 2, 1, &s);
        m1 = footprint_mb();
        for (k = 0; k < 240; k++) speak(v, k % 2 ? LONG : "Second utterance, 12:30 PM, $4.99.", k % 7 - 3, k % 5 - 2, 1, &s);
        m2 = footprint_mb();
        snprintf(what, sizeof what, "memory: footprint %.1f MB after 10 utterances, %.1f after 60 more, %.1f after 240 more", m0, m1, m2);
        check(m2 - m0 < 1.0, what);
        cvt_voice_close(v);
        return failures != 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "threads")) {
        int k, rounds = atoi(argv[2]), ok = 1;
        for (k = 0; k < rounds; k++) ok = ok && threads_round();
        check(ok, "two voices on two threads (3 pairs, one of them the same voice twice), identical to alone");
        return !ok;
    }
#ifdef CV_TRUVOICE_AB
    if (argc >= 4 && !strcmp(argv[1], "ab")) return ab(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "ratefit")) return ratefit(argv[2]);
#endif
    if (argc >= 3 && !strcmp(argv[1], "selftest")) {
        const char *od = argv[2];
        int i, rc;
        sink s = {0}, s2 = {0};
        cvt_voice *v;
        size_t base = 0;

        /* 1. every voice: open, speak, sane audio, several utterances in a row; the warm third utterance equals a fresh one */
        for (i = 0; i < 10; i++) {
            double t0 = now_s(), t_open, t_speak = 0;
            size_t first = 0;
            int k, ok = 1;
            sink f = {0};
            v = cvt_voice_open(MODES[i], err, sizeof err);
            t_open = now_s() - t0;
            if (!v) { snprintf(what, sizeof what, "open '%s': %s", MODES[i], err); check(0, what); continue; }
            t0 = now_s();
            for (k = 0; k < 3; k++) {
                rc = speak(v, k == 1 ? "Second utterance, 12:30 PM, $4.99." : LONG, 0, 0, 1, k == 0 ? &f : &s);
                if (k == 0) {
                    first = f.n;
                    t_speak = now_s() - t0;
                    snprintf(path, sizeof path, "%s/voice_%02d.wav", od, i);
                    write_wav(path, f.pcm, f.n, 22050);
                    ok = ok && rc == 0 && f.n > 22050 && peak_abs(&f) > 1000;
                } else {
                    ok = ok && rc == 0 && s.n > 22050 && peak_abs(&s) > 1000;
                }
            }
            snprintf(what, sizeof what, "%-44s open %.2f ms, %.2f s audio in %.4f s (%.0fx), 3 utterances, warm 3rd = fresh 1st: %s",
                     MODES[i], t_open * 1e3, first / 22050.0, t_speak, first / 22050.0 / t_speak, same_pcm(&s, &f) ? "yes" : "NO");
            check(ok && same_pcm(&s, &f), what);
            free(f.pcm);
            cvt_voice_close(v);
        }
        v = cvt_voice_open("Adult Male #1", err, sizeof err);
        check(v != NULL, "the short mode name opens too (\"Adult Male #1\")");
        cvt_voice_close(v);
        v = cvt_voice_open("Adult Male #9, American English (TruVoice)", err, sizeof err);
        check(v == NULL, "an unknown mode name is refused");
        v = cvt_voice_open("Sam", err, sizeof err);
        check(v == NULL, "a Microsoft SAPI 4 mode name is refused");

        v = cvt_voice_open(MODES[0], err, sizeof err);
        check(v != NULL, "open Adult Male #1");
        if (!v) return 1;

        /* 2. robustness: the SAPI 4 bridge's hostile strings (the engine reads Windows-1252 and its own ESC [ escapes) */
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
                /* OpenTV specific: its escapes are ESC [ ...; a literal "[5p" etc. must stay text, an ESC never gets through */
                "\x1B[200p\x1B[400r\x1B[1I HeLO1 \x1B[0I [5p] [HH AH L OW] \x1B",
                "5555-5555-5555-5555-5555 aaaa1111aaaa1111aaaa1111aaaa1111 999999999999999999999999999999.",
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
                write_wav(path, s.pcm, s.n, 22050);
                snprintf(what, sizeof what, "hostile %zu: rc=%d, %zu samples (tail alone %zu), %.3f s", k, rc, n, base, now_s() - t0);
                check(rc == 0 && n >= base, what);
            }
            rc = speak(v, TAIL, 0, 0, 1, &s2);
            snprintf(what, sizeof what, "voice unchanged after the hostile strings: rc=%d, %zu vs %zu samples, identical: %s", rc,
                     s2.n, base, same_pcm(&s2, &s) || s2.n == base ? "yes" : "no");
            check(rc == 0 && s2.n == base, what);
            rc = speak(v, "", 0, 0, 1, &s);
            check(rc == 0 && s.n == 0, "empty text gives no audio and no stand-in text");
            rc = speak(v, " \xF0\x9F\x98\x80 \xE2\x80\x8B ", 0, 0, 1, &s);
            check(rc == 0 && s.n == 0, "emoji-only text gives no audio");
            rc = speak(v, "!", 0, 0, 1, &s);
            snprintf(what, sizeof what, "punctuation-only text: rc=%d, %zu samples", rc, s.n);
            check(rc == 0, what);
            {
                char *big = malloc(20001);
                double t0;
                for (k = 0; k < 20000; k++) big[k] = (char)('a' + k % 26);
                big[20000] = 0;
                t0 = now_s();
                rc = speak(v, big, 18, 0, 1, &s);
                snprintf(what, sizeof what, "20,000 letters: rc=%d, %.1f s of audio in %.2f s", rc, s.n / 22050.0, now_s() - t0);
                check(rc == 0 && s.n > 0, what);
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
            snprintf(what, sizeof what, "digit trim: %zu -> %zu samples (%.0f ms saved)", raw, trimmed, (raw - trimmed) / 22.05);
            check(trimmed > 0 && trimmed < raw, what);
        }

        /* 4. rate and pitch */
        {
            static const double rates[5] = { -10, -4, 0, 2.5, 18 };
            size_t len[5];
            int k;
            for (k = 0; k < 5; k++) {
                speak(v, TAIL, rates[k], 0, 1, &s);
                len[k] = s.n;
            }
            snprintf(what, sizeof what, "rate %g/%g/%g/%g/%g -> SAPI %u/%u/%u/%u/%u -> OpenTV %u/%u/%u/%u/%u wpm: %zu %zu %zu %zu %zu samples",
                     rates[0], rates[1], rates[2], rates[3], rates[4], cvt_voice_wpm_for_rate(v, rates[0]),
                     cvt_voice_wpm_for_rate(v, rates[1]), cvt_voice_wpm_for_rate(v, rates[2]), cvt_voice_wpm_for_rate(v, rates[3]),
                     cvt_voice_wpm_for_rate(v, rates[4]), cvt_engine_wpm(cvt_voice_wpm_for_rate(v, rates[0])),
                     cvt_engine_wpm(cvt_voice_wpm_for_rate(v, rates[1])), cvt_engine_wpm(cvt_voice_wpm_for_rate(v, rates[2])),
                     cvt_engine_wpm(cvt_voice_wpm_for_rate(v, rates[3])), cvt_engine_wpm(cvt_voice_wpm_for_rate(v, rates[4])),
                     len[0], len[1], len[2], len[3], len[4]);
            check(len[0] > len[1] && len[1] > len[2] && len[2] > len[3] && len[3] > len[4], what);
            speak(v, TAIL, 0, 0, 1, &s);
            snprintf(what, sizeof what, "back at rate 0: %zu samples (was %zu)", s.n, len[2]);
            check(s.n == len[2], what);
            {
                unsigned w;
                int mono = 1;
                for (w = 50; w < 250; w++) mono = mono && cvt_engine_wpm(w + 1) >= cvt_engine_wpm(w);
                check(mono && cvt_engine_wpm(150) == 150 && cvt_engine_wpm(120) == 118 && cvt_engine_wpm(250) <= 253,
                      "SAPI -> OpenTV speed table: monotonic, 150 -> 150, 120 -> 118 (Adult Male #6's default), top <= 253");
            }
        }
        {
            double f[3], semis[3] = { -12, 0, 12 };
            int k;
            for (k = 0; k < 3; k++) {
                speak(v, "Aaaah.", 0, semis[k], 1, &s);
                f[k] = f0_estimate(&s);
            }
            snprintf(what, sizeof what, "pitch -12/0/+12 st -> %u/%u/%u: F0 about %.0f / %.0f / %.0f Hz", cvt_voice_pitch_for_semitones(v, -12),
                     cvt_voice_pitch_for_semitones(v, 0), cvt_voice_pitch_for_semitones(v, 12), f[0], f[1], f[2]);
            check(f[0] < f[1] && f[1] < f[2], what);
        }

        /* 5. cancel mid-utterance, then carry on */
        {
            char big[4000];
            int k;
            big[0] = 0;
            for (k = 0; k < 12; k++) strcat(big, "Alice was beginning to get very tired of sitting by her sister on the bank. ");
            s.stop_after = 22050;
            rc = speak(v, big, 0, 0, 1, &s);
            {
                double t_ret = now_s();
                snprintf(what, sizeof what, "cancel after 1 s of a 60 s text: rc=%d, %.2f s delivered, returned %.2f ms after the stop", rc,
                         s.n / 22050.0, (t_ret - s.t_stop) * 1e3);
                check(rc == 1 && (t_ret - s.t_stop) < 0.05, what);
            }
            s.stop_after = 0;
            rc = speak(v, TAIL, 0, 0, 1, &s);
            snprintf(what, sizeof what, "next utterance after a cancel: rc=%d, %zu samples (normal %zu)", rc, s.n, base);
            check(rc == 0 && s.n == base, what);
            for (k = 0; k < 5; k++) {
                s.stop_after = 1;
                rc = speak(v, big, 0, 0, 1, &s);
                if (rc != 1) break;
            }
            s.stop_after = 0;
            snprintf(what, sizeof what, "5 immediate cancels in a row: last rc=%d", rc);
            check(rc == 1, what);
            rc = speak(v, TAIL, 0, 0, 1, &s);
            check(rc == 0 && s.n == base, "speaks exactly as before after them");
        }

        /* 6. another voice speaking in between changes nothing (each voice keeps its own engine state) */
        {
            sink x = {0}, y = {0}, z = {0};
            cvt_voice *a = cvt_voice_open(MODES[3], err, sizeof err), *b = cvt_voice_open(MODES[8], err, sizeof err);
            cvt_voice *a2 = cvt_voice_open(MODES[3], err, sizeof err);
            if (a && b && a2) {
                speak(a, LONG, 0, 0, 1, &x);
                speak(a, "Second utterance, 12:30 PM, $4.99.", 0, 0, 1, &x);   /* a warm */
                speak(a2, LONG, 0, 0, 1, &y);
                speak(b, "Something else entirely, with a comma, and 7 numbers 1 2 3.", 4, 5, 1, &z);
                speak(a2, "Second utterance, 12:30 PM, $4.99.", 0, 0, 1, &y);
                check(same_pcm(&x, &y), "a voice's utterance is the same whether or not another voice spoke in between");
            } else {
                check(0, "open three voices");
            }
            cvt_voice_close(a); cvt_voice_close(b); cvt_voice_close(a2);
            free(x.pcm); free(y.pcm); free(z.pcm);
        }

        cvt_voice_close(v);

        /* 7. threads */
        check(threads_round(), "two voices on two threads (3 pairs, one of them the same voice twice), identical to alone");

        /* 8. fresh voices are deterministic */
        {
            sink x = {0}, y = {0};
            cvt_voice *v1 = cvt_voice_open(MODES[4], err, sizeof err);
            if (v1) { speak(v1, LONG, 0, 0, 1, &x); cvt_voice_close(v1); }
            v1 = cvt_voice_open(MODES[4], err, sizeof err);
            if (v1) { speak(v1, LONG, 0, 0, 1, &y); cvt_voice_close(v1); }
            check(x.n && same_pcm(&x, &y), "fresh voices are deterministic (Adult Male #5)");
        }

        printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
        return failures != 0;
    }
    fprintf(stderr, "usage: see the comment at the top of truvoice_test.c\n");
    return 1;
}
