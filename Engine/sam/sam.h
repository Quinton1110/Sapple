/* libsam - portable reimplementation of Microsoft Sam (SAPI5 "Microsoft TTS" engine, spttseng.dll).
 *
 * Plain C99, no platform dependencies. The voice data (Sam.spd) is loaded at runtime; it is not
 * part of this code.
 *
 * Back end: a segment list (one entry per acoustic unit, with duration, pitch and amplitude
 * contours) is rendered to 22050 Hz 16-bit mono PCM, matching the original engine sample-for-sample
 * (within float rounding).
 */
#ifndef SAM_H
#define SAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAM_MAX_KNOTS 20

typedef struct sam_voice sam_voice;
typedef struct sam_synth sam_synth;

/* One back-end segment, as produced by the engine's front end (the "segment record"). */
typedef struct {
    int unit;                   /* unit index into the voice; 0 = silence */
    float dur;                  /* target duration in seconds */
    int n_knots;                /* number of contour knots (<= SAM_MAX_KNOTS) */
    float t[SAM_MAX_KNOTS];     /* knot times, in output samples from segment start */
    float f0[SAM_MAX_KNOTS];    /* pitch in Hz at each knot */
    float amp[SAM_MAX_KNOTS];   /* excitation amplitude at each knot */
    int hold;                   /* 1 = when lengthened, stretch only the steadiest frames (singing), 2 = same in the second half; 0 = engine */
} sam_segment;

/* Knobs that are not part of the original engine's defaults - for experiments. */
typedef struct {
    float gain;          /* output gain applied in the LPC filter (engine default 1.0) */
    float pitch_scale;   /* multiplies every f0 knot (1.0 = original) */
    float speed;         /* >1 speaks faster (scales segment durations by 1/speed) */
    float vibrato;       /* engine's built-in vibrato depth (Hz per table step, default 0) */
    float vibrato_rate;  /* vibrato speed in Hz */
    int no_reverse;      /* 1 = disable time-reversal of repeated unvoiced frames */
    int reverse_units;   /* 1 = play every acoustic unit backwards (words stay in order) */
    int out_rate;        /* run the vocoder at this sample rate (0 = the voice's 22050); formants scale with it */
    int smooth;          /* 1 = blend neighbouring frames (filters and pulses) instead of stepping; for long notes */
    float sing_vibrato;  /* singer's vibrato depth in cents (0 = off); fades in 0.15 s into each segment */
    float sing_vibrato_rate; /* its speed in Hz */
    float transpose;     /* sam_tts_sing: shift every note by this many semitones */
    float base_pitch;    /* the voice's base pitch in Hz, from its .sdf (Sam 100, Mike 110, Mary 189) */
    int whisper;         /* SAPI 4 Whisper: every frame gets noise excitation */
    int monotone;        /* SAPI 4 RoboSoft: every pitch point = the base pitch */
} sam_params;

void sam_params_default(sam_params *p);

/* Load a .spd voice file. On failure returns NULL and writes a message to err. */
sam_voice *sam_voice_load(const char *path, char *err, size_t errlen);
/* Same from a buffer (copied), e.g. a voice file compiled into the program. */
sam_voice *sam_voice_load_mem(const void *data, size_t size, char *err, size_t errlen);
void sam_voice_free(sam_voice *v);
int sam_voice_rate(const sam_voice *v);
int sam_voice_units(const sam_voice *v);

/* Receives rendered 16-bit samples. */
typedef void (*sam_pcm_cb)(const int16_t *pcm, size_t n, void *user);

sam_synth *sam_synth_new(const sam_voice *v, const sam_params *p);
void sam_synth_free(sam_synth *s);
/* Render one segment. Returns 0 on success. */
int sam_synth_segment(sam_synth *s, const sam_segment *seg, sam_pcm_cb cb, void *user);

/* ---- text to speech ---- */

typedef struct sam_tts sam_tts;

/* spd: Sam.spd, lex: LTTS1033.LXA, lts: r1033tts.LXA (all from the original installation). */
sam_tts *sam_tts_new(const char *spd, const char *lex, const char *lts, const sam_params *p, char *err, size_t errlen);
/* Same with the three files already in memory (copied). */
sam_tts *sam_tts_new_mem(const void *spd, size_t spd_size, const void *lex, size_t lex_size, const void *lts,
                         size_t lts_size, const sam_params *p, char *err, size_t errlen);
void sam_tts_free(sam_tts *t);
/* Speak ASCII/UTF-8 text; audio is delivered through cb (22050 Hz, 16-bit mono). */
int sam_tts_speak_pcm(sam_tts *t, const char *text, sam_pcm_cb cb, void *user);

/* ---- marks and options, for library hosts (sam_tts.c); the plain sam_tts_speak above is
 * sam_tts_speak_ex with no options, and behaves exactly as before. ---- */

typedef struct {
    int flags;              /* 1 = first sound of a word, 2 = first sound of a sentence */
    int word_pos, word_len; /* UTF-16 offset and length of the word in the text */
    int sent_pos, sent_len; /* ... of the sentence it belongs to */
    long long audio_pos;    /* output samples produced before this sound */
} sam_mark;

typedef struct {
    double sapi_rate;           /* divides every duration: 3^(SAPI rate/10); 0 or 1 = normal */
    float pitch_offset;         /* added to every item's log2 pitch offset (0 = none) */
    volatile const int *cancel; /* checked between items: nonzero stops the call (returns 1) */
    void (*mark_cb)(void *user, const sam_mark *m); /* word / sentence starts, may be NULL */
    void *user;
} sam_speak_opts;

/* Speak with options; opts may be NULL. Returns 0, 1 when cancelled, -1 on error. */
int sam_tts_speak_ex(sam_tts *t, const char *text, const sam_speak_opts *opts, sam_pcm_cb cb, void *user);

/* Sing a score: one line per word, "twin-kle C4 1 C4 1" (a note and a length in beats per syllable),
 * "- 2" for a rest, "tempo 120" to set beats per minute. Not part of the original engine. */
int sam_tts_sing(sam_tts *t, const char *score, sam_pcm_cb cb, void *user);

/* ClassicVoices patch (singing mode for screen readers). Syllables are counted from the word's actual
 * pronunciation - its vowel nuclei, which win over the lexicon's syllable marks when the two disagree
 * ("stylish" has no mark but two vowels) - and every syllable gets exactly one note. */
typedef struct {
    volatile const int *cancel; /* checked between sounds: nonzero stops the call (returns 1) */
    double tempo_scale;         /* sam_tts_sing_text: multiplies its tempo (e.g. 3^(SAPI rate/10)); 0 = 1 */
    double transpose;           /* semitones added to every note (on top of sam_params.transpose) */
    float vibrato_cents;        /* singer's vibrato depth (0 = none) and speed in Hz */
    float vibrato_rate;
    int mismatches;             /* out, sam_tts_sing_ex: words whose note count differed from their syllables */
} sam_sing_opts;

/* A literal score (as sam_tts_sing), with options. A word with more notes than syllables has the extra notes
 * merged into its last syllable, one with fewer has its last note held over the rest; both are counted in
 * o->mismatches. Returns 0, 1 when cancelled, -1 on error. */
int sam_tts_sing_ex(sam_tts *t, const char *score, sam_sing_opts *o, sam_pcm_cb cb, void *user);

/* Sing plain text: the normalizer and lexicon as for speech, then an automatic melody (one note per syllable,
 * see sam_front.c "the melody"). Returns 0, 1 when cancelled, -1 on error. */
int sam_tts_sing_text(sam_tts *t, const char *text, sam_sing_opts *o, sam_pcm_cb cb, void *user);

/* 1 if the text looks like a literal score (starts with "tempo " or has a line "word NOTE BEATS"). */
int sam_sing_is_score(const char *text);

/* ---- lower-level access, useful for tools and experiments ---- */

typedef struct {
    int nframes;
    int order;
    int total;          /* total excitation samples = sum |period| */
    float *periods;     /* [nframes], <= 0 means unvoiced */
    float *lpc;         /* [nframes * (order+1)], a[0] = 1 */
    float *gains;       /* [nframes] */
    float *exc;         /* [total] excitation (voiced pulses / scaled noise) */
    float *lsf;         /* [nframes * order] line spectral frequencies (normalized), before sorting */
} sam_unit;

/* Decode a unit. noise_pos is the running position in the voice's noise buffer (engine state). */
int sam_unit_decode(const sam_voice *v, int index, int *noise_pos, sam_unit *out);
void sam_unit_free(sam_unit *u);

#ifdef __cplusplus
}
#endif
#endif
