/* Old path vs new path for Microsoft's SAPI 4 voices: the same requests rendered through
 *   OLD  cv4_bridge  + SAPI4/emu     (msttssyn.dll in the x86 interpreter: what the app ran until now)
 *   NEW  cv4n_bridge + Engine/sapi4  (the engine decompiled to C: what the app runs now)
 * The two engines export the same s4_* names, so this file is built twice (make sapi4-ab):
 *   build/mac/sapi4_ab_old render DATA_DIR OUTDIR     (-DCV_SAPI4_OLD)
 *   build/mac/sapi4_ab_new render DATA_DIR OUTDIR
 *   build/mac/sapi4_ab_new compare OLDDIR NEWDIR [WAVDIR]   identical / different per job, and for a difference: sample
 *                                                            counts, first differing sample, largest difference,
 *                                                            correlation; WAVs of the differing jobs (both paths)
 * Every job is written as OUTDIR/<id>.pcm (raw 16-bit, 22050 Hz through a bridge, the engine's own rate at engine
 * level) and listed in OUTDIR/manifest.txt with its return code.
 *
 * Job groups (each starts from a fresh engine where it says so; utterances after that continue the engine's state, as
 * the app's pooled voices do):
 *   seq    every mode: 12 texts in a row on one fresh voice (plain, numbers, hostile, tags as text, a song score)
 *   rate   every mode: the fox sentence at 11 SAPI rates, then 9 pitches, then rate x pitch pairs, one fresh voice
 *   notrim every mode: three texts with the silence trim off
 *   corpus seven modes: 60 texts (the self-tests' hostile strings, scores, digits, letters, symbols, long runs), warm
 *   cancel five modes: stop after 1 / 5,000 / 22,050 samples, then the next utterance
 *   fuzz   four modes: 250 pseudo-random strings (deterministic), warm
 *   engine every mode, no bridge: 30 texts with SAPI 4 tags OBEYED (S4_TAGGED) and 6 plain, at the engine's own rate,
 *          untrimmed; the speed / pitch attributes set at the extremes
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#ifdef CV_SAPI4_OLD
#include "cv4_bridge.h"
#include "sapi4_tts.h"
typedef cv4_voice voice;
#define voice_open cv4_voice_open
#define voice_close cv4_voice_close
#define voice_speak cv4_voice_speak
#define ENGINE_LIST(dir, m, max) s4_list_modes(dir, m, max, NULL, 0)
#define ENGINE_INIT(dir) 0
#define PATH_NAME "old (emulator)"
#else
#include "cv4n_bridge.h"
#include "sapi4tts.h"
typedef cv4n_voice voice;
#define voice_open cv4n_voice_open
#define voice_close cv4n_voice_close
#define voice_speak cv4n_voice_speak
#define ENGINE_LIST(dir, m, max) s4_list_modes(dir, m, max)
#define PATH_NAME "new (native)"
static int ENGINE_INIT(const char *dir)
{
    char p[1100];
    snprintf(p, sizeof p, "%s/msttssyn.dll", dir);
    return s4_init(p);
}
#endif


static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* the 19 modes the app ships (Voices.swift SAPI4_VOICES) */
static const char *const MODES[19] = {
    "Sam", "Mike", "Mary", "Mike (for Telephone)", "Mary (for Telephone)", "Mike in Hall", "Mike in Stadium",
    "Mike in Space", "Mary in Hall", "Mary in Stadium", "Mary in Space", "RoboSoft One", "RoboSoft Two", "RoboSoft Three",
    "RoboSoft Four", "RoboSoft Five", "RoboSoft Six", "Male Whisper", "Female Whisper",
};

typedef struct {
    int16_t *pcm;
    size_t n, cap;
    size_t stop_after;
} sink;

static int on_pcm(const int16_t *pcm, size_t n, void *user)
{
    sink *s = user;
    if (s->n + n > s->cap) {
        s->cap = (s->n + n) * 2 + 65536;
        s->pcm = realloc(s->pcm, s->cap * sizeof *s->pcm);
    }
    memcpy(s->pcm + s->n, pcm, n * sizeof *pcm);
    s->n += n;
    return s->stop_after && s->n >= s->stop_after;
}

static FILE *g_manifest;
static const char *g_out;
static int g_jobs;
static double g_audio;

static void record(const char *id, int rc, const sink *s, int rate)
{
    char path[1200];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.pcm", g_out, id);
    f = fopen(path, "wb");
    if (f) {
        if (s->n) fwrite(s->pcm, 2, s->n, f);
        fclose(f);
    }
    fprintf(g_manifest, "%s\t%d\t%zu\t%d\n", id, rc, s->n, rate);
    g_jobs++;
    g_audio += (double)s->n / rate;
}

static void say(voice *v, const char *id, const char *text, double rate, double semi, int trim, size_t stop_after)
{
    sink s = {0};
    int rc;
    s.stop_after = stop_after;
    rc = v ? voice_speak(v, text, rate, semi, trim, on_pcm, &s) : -2;
    record(id, rc, &s, 22050);
    free(s.pcm);
}

static const char *const SEQ[12] = {
    "Hello, my name is Microsoft Sam.",
    "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.",
    "Second utterance, 12:30 PM, $4.99, 1,234,567 and 3.14159.",
    "5",
    "Speech",
    "Button",
    "\\Pit=400\\ \\Spd=450\\ \\Chr=\"Whisper\"\\ \\Rst\\ C:\\Windows\\System32.",
    "tempo 120\ntwin-kle C4 1 C4 1 G4 1 G4 1\nlit-tle A4 1 A4 1 G4 2",
    "Caf\xC3\xA9 na\xC3\xAFve r\xC3\xA9sum\xC3\xA9, \xC2\xBD pound \xC2\xA3 and 20\xC2\xB0.",
    "Wait... what?! (Really.) \"Quoted\" - dashes \xE2\x80\x94 and so on; OK: yes.",
    "Dr. Smith lives at 221B Baker St., London NW1 6XE. Call +44 20 7224 3688.",
    "This sentence must still be spoken.",
};

static const char *const CORPUS[] = {
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
    "",
    " \xF0\x9F\x98\x80 \xE2\x80\x8B ",
    "!",
    "5",
    "a",
    "Z",
    "I",
    "Speech",
    "Heading level 2",
    "65% battery power, charging",
    "Tuesday, September 27, 2026 at 11:58 PM",
    "$1,000,000.00 and \xE2\x82\xAC""3,50 and 7 1/2 and 1st 2nd 3rd 4th 21st",
    "Mr. Mrs. Ms. Dr. St. Ave. Jan. Feb. etc. vs. e.g. i.e.",
    "NASA FBI USA IBM CPU GPU HTML XML JSON URL iPhone macOS VoiceOver",
    "can't won't shouldn't it's o'clock rock 'n' roll",
    "The the the the the the the the the the the the the the the the the the the the.",
    "Flight B6 2, Room C4 1., grade A5 3, e4 e5 Nf3 Nc6 Bb5 a6.",
    "tempo 120\ntwin-kle C4 1 C4 1 G4 1 G4 1\nlit-tle A4 1 A4 1 G4 2\nstar F4 1 F4 1 E4 1 E4 1 D4 1 D4 1 C4 2",
    "twin-kle C4 1 C4 1 G4 1 G4 1 lit-tle A4 1 A4 1 G4 2 - 1",
    "Happy birthday to you, happy birthday to you!",
    "Supercalifragilisticexpialidocious antidisestablishmentarianism pneumonoultramicroscopicsilicovolcanoconiosis.",
    "Is this a question? Yes! No. Maybe; perhaps: who knows...",
    "Line one\nLine two\r\nLine three\tTabbed.",
    "0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 30 40 50 60 70 80 90 100 1000 1000000 1000000000",
    "-5 +5 5% 5.5 .5 5, 5; 5: 5! 5? (5) [5] {5} <5> 5/5 5*5 5^5 5=5 5&5 5|5",
    "January February March April May June July August September October November December",
    "The year 1999, the year 2000, the '90s, 1990s, 12/25/1999, 25.12.1999, 1999-12-25.",
    "Q: what is 2+2? A: 4. 3 x 3 = 9. 10 / 2 = 5. 7 - 3 = 4.",
    "Phone (555) 123-4567 ext. 89, zip 97201-1234, SSN-like 123-45-6789.",
    "\"Hello,\" she said. 'Hi,' he replied. \xE2\x80\x9CSmart quotes\xE2\x80\x9D and \xE2\x80\x98single\xE2\x80\x99 ones.",
    "Menu \xE2\x8C\x98 command, \xE2\x8C\xA5 option, \xE2\x87\xA7 shift, \xE2\x9C\x93 check, \xE2\x9C\x97 cross.",
    "x\xC2\xB2 + y\xC2\xB3 = z, \xC2\xB1 1, \xC3\x97 2, \xC3\xB7 3, \xE2\x88\x9E, \xE2\x89\xA0, \xE2\x89\xA4, \xE2\x89\xA5.",
    "Zo\xC3\xAB, Bj\xC3\xB6rk, Fran\xC3\xA7ois, Dvo\xC5\x99\xC3\xA1k, \xC5\x81\xC3\xB3" "d\xC5\xBA, Stra\xC3\x9F" "e.",
    "OK OK OK OK. Hmm, uh-huh, mm-hmm, wow, oops, ouch, yay, hooray.",
    "Microsoft Sam, Mike and Mary; RoboSoft One through Six; Mary in Hall; Mike in Space.",
    "a b c d e f g h i j k l m n o p q r s t u v w x y z",
    "1.2.3.4 192.168.0.1 ::1 fe80::1 v1.2.3-beta+build.5",
    "It was the best of times, it was the worst of times, it was the age of wisdom, it was the age of foolishness, "
    "it was the epoch of belief, it was the epoch of incredulity, it was the season of Light, it was the season of Darkness.",
    "Alice was beginning to get very tired of sitting by her sister on the bank, and of having nothing to do: once or "
    "twice she had peeped into the book her sister was reading, but it had no pictures or conversations in it, 'and "
    "what is the use of a book,' thought Alice 'without pictures or conversations?'",
    "Short. Sentences. Every. Word. Is. One.",
    "The end.",
};
#define NCORPUS (sizeof CORPUS / sizeof *CORPUS)

/* SAPI 4 tags, OBEYED (engine level only: the app never sends them) */
static const char *const TAGGED[] = {
    "\\Pit=150\\ Higher now. \\Pit=60\\ Lower now.",
    "\\Spd=250\\ Faster speech here. \\Spd=80\\ And slower here.",
    "\\Chr=\"Whisper\"\\ Whispering now. \\Chr=\"Normal\"\\ Back again.",
    "\\Chr=\"Monotone\"\\ A monotone sentence.",
    "\\Mrk=1\\ A bookmark. \\Mrk=2\\ Another.",
    "\\Pau=500\\ After a pause. \\Pau=1500\\ A longer one.",
    "\\Emp\\ Emphasis on this word.",
    "\\Vol=20000\\ Quieter. \\Vol=65535\\ Louder.",
    "\\Rst\\ After a reset.",
    "\\Prn=HH AH L OW\\ is a pronunciation.",
    "\\Ctx=\"Address\"\\ 1600 Pennsylvania Ave NW.",
    "\\Ctx=\"E-Mail\"\\ someone@example.com",
    "\\Ctx=\"Unknown\"\\ 12:30",
    "\\RmS=1\\ ABC \\RmS=0\\ ABC",
    "\\RPit=150\\ Relative pitch. \\RSpd=150\\ Relative speed.",
    "\\PrO=100\\ Pitch offset.",
    "\\Prn=\\ empty pronunciation.",
    "\\Pit=\\ \\Spd=\\ empty values.",
    "\\Pit=99999\\ \\Spd=99999\\ out of range.",
    "\\Pit=-5\\ \\Spd=-5\\ negative.",
    "\\Bogus=1\\ unknown tag.",
    "\\ unterminated tag",
    "\\\\ double backslash \\\\",
    "\\Pit=150\\\\Spd=200\\\\Vol=30000\\ stacked tags.",
    "\\Com=a comment\\ after a comment.",
    "\\Eng;{00000000-0000-0000-0000-000000000000}:1\\ engine-specific.",
    "\\Chr=\"Excited\"\\ Excited! \\Chr=\"Monotone\"\\ Flat. \\Chr=\"Normal\"\\ Normal.",
    "\\Pit=200\\ \\Spd=300\\ \\Chr=\"Whisper\"\\ All at once: the quick brown fox.",
    "\\Mrk=4294967295\\ big bookmark \\Mrk=0\\ zero",
    "Plain text after all the tags.",
};
#define NTAGGED (sizeof TAGGED / sizeof *TAGGED)
static const char *const PLAIN_ENGINE[6] = {
    "Hello, my name is Microsoft Sam.",
    "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.",
    "Second utterance, 12:30 PM, $4.99.",
    "5",
    "\\Pit=400\\ read, not obeyed.",
    "Caf\xE9 na\xEFve \xBD \xA3.",   /* Windows-1252 bytes */
};

static unsigned long long g_rng = 0x9E3779B97F4A7C15ull;
static unsigned rnd(unsigned n)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (unsigned)(g_rng % n);
}
static void fuzz_text(char *out, size_t cap)
{
    static const char *const words[] = { "the", "quick", "Sam", "12", "3.5", "$4", "%", "\\Pit=400\\", "\\", "...", "-",
                                         "A5", "C4", "1", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80", "Mr.", "St.",
                                         "e.g.", "?", "!", ",", ";", ":", "(", ")", "\"", "'", "/", "@", "#", "&",
                                         "\n", "\t", "OK", "USA", "1999", "12:30", "1/2", "x", "zzzz", "aeiou",
                                         "\xC3\x9F", "\xCE\xB1", "\xE2\x80\x94", "\xE2\x80\xA6", "0", "000", "-1" };
    size_t len = 0;
    unsigned k, nw = 1 + rnd(24);
    out[0] = 0;
    for (k = 0; k < nw; k++) {
        char tok[64];
        if (rnd(5) == 0) {   /* a run of random printable / high bytes (valid UTF-8 not guaranteed) */
            unsigned j, n = 1 + rnd(12);
            for (j = 0; j < n; j++) tok[j] = (char)(rnd(3) ? 32 + rnd(95) : 128 + rnd(128));
            tok[n] = 0;
        } else snprintf(tok, sizeof tok, "%s", words[rnd(sizeof words / sizeof *words)]);
        if (len + strlen(tok) + 2 >= cap) break;
        if (k && rnd(4)) out[len++] = ' ';
        memcpy(out + len, tok, strlen(tok));
        len += strlen(tok);
        out[len] = 0;
    }
}

static const char *LONG = "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.";

static int render(const char *dir, const char *od)
{
    char id[256], path[1200], err[256];
    int m, k;
    double t0 = now_s();
    mkdir(od, 0755);
    g_out = od;
    snprintf(path, sizeof path, "%s/manifest.txt", od);
    g_manifest = fopen(path, "w");
    if (!g_manifest) return 2;
    if (ENGINE_INIT(dir)) { fprintf(stderr, "cannot load the engine from %s\n", dir); return 2; }

    /* seq */
    for (m = 0; m < 19; m++) {
        voice *v = voice_open(dir, MODES[m], err, sizeof err);
        if (!v) fprintf(stderr, "open %s: %s\n", MODES[m], err);
        for (k = 0; k < 12; k++) {
            snprintf(id, sizeof id, "seq_%02d_%02d", m, k);
            say(v, id, SEQ[k], 0, 0, 1, 0);
        }
        if (v) voice_close(v);
#ifndef CV_SAPI4_OLD
        cv4n_discard_parked();
#endif
    }
    /* rate / pitch */
    for (m = 0; m < 19; m++) {
        static const double rates[11] = { -10, -6, -3, 0, 1.5, 2.5, 5.5, 8, 10, 14, 18 };
        static const double semis[9] = { -12, -7.5, -3, -0.5, 0, 2, 5, 9.25, 12 };
        voice *v = voice_open(dir, MODES[m], err, sizeof err);
        for (k = 0; k < 11; k++) {
            snprintf(id, sizeof id, "rate_%02d_r%02d", m, k);
            say(v, id, LONG, rates[k], 0, 1, 0);
        }
        for (k = 0; k < 9; k++) {
            snprintf(id, sizeof id, "rate_%02d_p%02d", m, k);
            say(v, id, "Hello, my name is Microsoft Sam, and I can change my pitch.", 0, semis[k], 1, 0);
        }
        for (k = 0; k < 6; k++) {
            snprintf(id, sizeof id, "rate_%02d_x%02d", m, k);
            say(v, id, "Twelve, 12:30 PM.", rates[(k * 5 + 1) % 11], semis[(k * 4 + 2) % 9], 1, 0);
        }
        if (v) voice_close(v);
#ifndef CV_SAPI4_OLD
        cv4n_discard_parked();
#endif
    }
    /* notrim */
    for (m = 0; m < 19; m++) {
        voice *v = voice_open(dir, MODES[m], err, sizeof err);
        say(v, (snprintf(id, sizeof id, "notrim_%02d_0", m), id), "5", 0, 0, 0, 0);
        say(v, (snprintf(id, sizeof id, "notrim_%02d_1", m), id), "Speech, button.", 3, -2, 0, 0);
        say(v, (snprintf(id, sizeof id, "notrim_%02d_2", m), id), LONG, -4, 4, 0, 0);
        if (v) voice_close(v);
#ifndef CV_SAPI4_OLD
        cv4n_discard_parked();
#endif
    }
    /* corpus */
    {
        static const int cm[7] = { 0, 1, 2, 3, 5, 11, 18 };
        for (m = 0; m < 7; m++) {
            voice *v = voice_open(dir, MODES[cm[m]], err, sizeof err);
            for (k = 0; k < (int)NCORPUS; k++) {
                snprintf(id, sizeof id, "corpus_%02d_%02d", cm[m], k);
                say(v, id, CORPUS[k], (k % 5) * 2.0 - 2, (k % 3) - 1.0, 1, 0);
            }
            if (v) voice_close(v);
#ifndef CV_SAPI4_OLD
            cv4n_discard_parked();
#endif
        }
    }
    /* cancel */
    {
        static const int cm[5] = { 0, 4, 8, 13, 18 };
        static const size_t stops[3] = { 1, 5000, 22050 };
        char big[4000];
        big[0] = 0;
        for (k = 0; k < 8; k++) strcat(big, "Alice was beginning to get very tired of sitting by her sister on the bank. ");
        for (m = 0; m < 5; m++) {
            voice *v = voice_open(dir, MODES[cm[m]], err, sizeof err);
            for (k = 0; k < 3; k++) {
                snprintf(id, sizeof id, "cancel_%02d_%d_stopped", cm[m], k);
                say(v, id, big, 0, 0, 1, stops[k]);
                snprintf(id, sizeof id, "cancel_%02d_%d_next", cm[m], k);
                say(v, id, "This sentence must still be spoken.", 0, 0, 1, 0);
            }
            if (v) voice_close(v);
#ifndef CV_SAPI4_OLD
            cv4n_discard_parked();
#endif
        }
    }
    /* fuzz */
    {
        static const int fm[4] = { 0, 2, 4, 17 };
        char text[600];
        for (m = 0; m < 4; m++) {
            voice *v = voice_open(dir, MODES[fm[m]], err, sizeof err);
            for (k = 0; k < 250; k++) {
                fuzz_text(text, sizeof text);
                snprintf(id, sizeof id, "fuzz_%02d_%03d", fm[m], k);
                say(v, id, text, (double)(k % 29) - 10, (double)(k % 25) - 12, k % 7 != 0, 0);
            }
            if (v) voice_close(v);
#ifndef CV_SAPI4_OLD
            cv4n_discard_parked();
#endif
        }
    }
    /* engine level: tags obeyed, native rate, untrimmed, attribute extremes */
    for (m = 0; m < 19; m++) {
        s4_engine *e = s4_open(dir, MODES[m], err, sizeof err);
        s4_limits lim;
        if (!e) { fprintf(stderr, "engine open %s: %s\n", MODES[m], err); continue; }
        s4_get_limits(e, &lim);
        fprintf(g_manifest, "limits_%02d\t0\t0\t%u %u %u %u %u %u %d\n", m, lim.pitch_default, lim.pitch_min, lim.pitch_max,
                lim.speed_default, lim.speed_min, lim.speed_max, s4_sample_rate(e));
        for (k = 0; k < (int)NTAGGED + 6; k++) {
            sink s = {0};
            int rc;
            if (k == (int)NTAGGED + 3) { s4_set_pitch(e, lim.pitch_max); s4_set_speed(e, lim.speed_min); }
            if (k == (int)NTAGGED + 4) { s4_set_pitch(e, lim.pitch_min); s4_set_speed(e, lim.speed_max); }
            if (k < (int)NTAGGED) rc = s4_speak(e, TAGGED[k], 1 /* S4_TAGGED */, on_pcm, &s);
            else rc = s4_speak(e, PLAIN_ENGINE[k - NTAGGED], 0, on_pcm, &s);
            snprintf(id, sizeof id, "engine_%02d_%02d", m, k);
            record(id, rc, &s, s4_sample_rate(e));
            free(s.pcm);
        }
        s4_close(e);
    }
    fclose(g_manifest);
    printf("%s: %d jobs, %.0f s of audio rendered in %.1f s\n", PATH_NAME, g_jobs, g_audio, now_s() - t0);
    return 0;
}

/* ------------------------------------------------------------------ compare */

typedef struct {
    char id[128];
    int rc;
    size_t n;
    int rate;
} entry;

static entry *read_manifest(const char *dir, int *count)
{
    char path[1200], line[512];
    FILE *f;
    entry *es = NULL;
    int n = 0, cap = 0;
    snprintf(path, sizeof path, "%s/manifest.txt", dir);
    f = fopen(path, "r");
    if (!f) return NULL;
    while (fgets(line, sizeof line, f)) {
        entry e;
        memset(&e, 0, sizeof e);
        if (sscanf(line, "%127[^\t]\t%d\t%zu\t%d", e.id, &e.rc, &e.n, &e.rate) < 3) continue;
        if (!strncmp(e.id, "limits_", 7)) {
            char *tab = strrchr(line, '\t');
            snprintf(e.id + strlen(e.id), sizeof e.id - strlen(e.id), "|%s", tab ? tab + 1 : "");
            e.id[strcspn(e.id, "\n")] = 0;
        }
        if (n == cap) es = realloc(es, (size_t)(cap = cap * 2 + 256) * sizeof *es);
        es[n++] = e;
    }
    fclose(f);
    *count = n;
    return es;
}

static int16_t *read_pcm(const char *dir, const char *id, size_t n)
{
    char path[1200];
    FILE *f;
    int16_t *p = malloc((n ? n : 1) * 2);
    snprintf(path, sizeof path, "%s/%s.pcm", dir, id);
    f = fopen(path, "rb");
    if (!f || fread(p, 2, n, f) != n) {
        if (f) fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    return p;
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

static int compare(const char *da, const char *db, const char *wavdir)
{
    int na = 0, nb = 0, i, same = 0, differ = 0, missing = 0;
    entry *a = read_manifest(da, &na), *b = read_manifest(db, &nb);
    size_t samples = 0;
    char groups[16][16];
    int gsame[16] = {0}, gall[16] = {0}, ng = 0;
    if (!a || !b) { fprintf(stderr, "cannot read the manifests\n"); return 2; }
    if (wavdir) mkdir(wavdir, 0755);
    for (i = 0; i < na; i++) {
        int j, g;
        char grp[16];
        const entry *eb = NULL;
        snprintf(grp, sizeof grp, "%.*s", (int)strcspn(a[i].id, "_"), a[i].id);
        for (g = 0; g < ng && strcmp(groups[g], grp); g++) {}
        if (g == ng && ng < 16) snprintf(groups[ng++], 16, "%s", grp);
        for (j = 0; j < nb; j++)
            if (!strcmp(a[i].id, b[j].id)) { eb = &b[j]; break; }
        gall[g]++;
        if (!eb) { printf("MISSING in new: %s\n", a[i].id); missing++; continue; }
        if (!strncmp(a[i].id, "limits_", 7)) { same++; gsame[g]++; continue; }   /* the limits are in the id */
        {
            int16_t *pa = read_pcm(da, a[i].id, a[i].n), *pb = read_pcm(db, eb->id, eb->n);
            int eq = pa && pb && a[i].rc == eb->rc && a[i].n == eb->n && !memcmp(pa, pb, a[i].n * 2);
            if (eq) {
                same++;
                gsame[g]++;
                samples += a[i].n;
            } else {
                size_t k, first = (size_t)-1, m = a[i].n < eb->n ? a[i].n : eb->n;
                double sab = 0, saa = 0, sbb = 0;
                int maxd = 0;
                differ++;
                for (k = 0; pa && pb && k < m; k++) {
                    int d = abs(pa[k] - pb[k]);
                    if (d && first == (size_t)-1) first = k;
                    if (d > maxd) maxd = d;
                    sab += (double)pa[k] * pb[k];
                    saa += (double)pa[k] * pa[k];
                    sbb += (double)pb[k] * pb[k];
                }
                printf("DIFFERENT %-22s rc %d/%d samples %zu/%zu first diff at %zd, max |diff| %d, corr %.4f\n", a[i].id,
                       a[i].rc, eb->rc, a[i].n, eb->n, first == (size_t)-1 ? (ssize_t)-1 : (ssize_t)first, maxd,
                       saa > 0 && sbb > 0 ? sab / sqrt(saa * sbb) : 0.0);
                if (wavdir && pa && pb) {
                    char p[1400];
                    snprintf(p, sizeof p, "%s/%s_old.wav", wavdir, a[i].id);
                    write_wav(p, pa, a[i].n, (uint32_t)a[i].rate);
                    snprintf(p, sizeof p, "%s/%s_new.wav", wavdir, a[i].id);
                    write_wav(p, pb, eb->n, (uint32_t)eb->rate);
                }
            }
            free(pa);
            free(pb);
        }
    }
    for (i = 0; i < ng; i++) printf("  %-8s %5d / %5d identical\n", groups[i], gsame[i], gall[i]);
    printf("A/B: %d jobs, %d identical (%zu samples, %.0f s at 22050 Hz), %d different, %d missing; new has %d jobs\n", na,
           same, samples, samples / 22050.0, differ, missing, nb);
    return differ || missing || na != nb;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "render")) return render(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "compare")) return compare(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
    fprintf(stderr, "usage: see the comment at the top of sapi4_ab.c\n");
    return 2;
}
