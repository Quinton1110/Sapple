/* sapi4tts: the Microsoft SAPI 4 text-to-speech engine (msttssyn.dll: Sam, Mike, Mary and their 19
 * modes) as portable C, producing the same PCM as the original, sample for sample.
 *
 * The engine's code is this library; its data is not. It needs, at run time or baked in at build time:
 *   - the data sections of the user's copy of msttssyn.dll  (s4_init with its path, or NULL for a
 *     build that compiled the image in: see port/mkimage);
 *   - the voice files (*.vce, *.cfg) from a directory, or compiled in (see port/mkvoices).
 *
 * Threads: every call is thread-safe. Engine instances keep all of their own state (queues, front end,
 * unit stage, synthesizer, pitch, speed, random-number states), but the engine's shared data (its
 * voice table, lexicons, rule-machine tapes: process-wide in the original too) means the work of all
 * instances is serialized by one process-wide lock: two instances speaking at once take turns, each
 * utterance unaffected by the other. s4_stop is the one call that never waits. What instances share
 * is what engines of one process shared in the original: the voice table, the built-in and user
 * lexicons (the user lexicon is read once, by the first engine), the phone tables; none of it carries
 * state from one utterance into another (checked: the golden set rendered in one process, on one
 * thread and on four, is identical to renders made in separate processes).
 *
 * Output: 16-bit mono PCM at the voice's rate (s4_sample_rate: 22050 Hz, or 8000 Hz for the
 * telephone voices), delivered phrase by phrase to the callback while s4_speak runs.
 */
#ifndef SAPI4TTS_H
#define SAPI4TTS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct s4_engine s4_engine;

/* one voice mode (TTSMODEINFO) */
typedef struct s4_mode {
    char name[64];            /* "Sam", "Mary in Hall", ... (what s4_open takes) */
    char speaker[64];
    int gender;               /* SAPI: 1 female, 2 male */
    int age;
    uint32_t features;        /* SAPI TTSFEATURE_ bits */
    int sample_rate;
} s4_mode;

typedef struct s4_limits {
    unsigned pitch_default, pitch_min, pitch_max;   /* the engine's pitch units (Hz for these voices) */
    unsigned speed_default, speed_min, speed_max;   /* words per minute */
} s4_limits;

/* PCM for the caller: n samples. Return 0 to go on, nonzero to stop the utterance. Called with the
 * library's lock held: from here, call nothing in this library but s4_stop. */
typedef int (*s4_pcm_fn)(const int16_t *pcm, size_t n, void *user);

enum {
    S4_TAGGED = 1,            /* text contains SAPI 4 tags: \Pit=150\, \Spd=250\, \Chr="Whisper"\, \Mrk=1\, ... */
};

/* The engine's data: the path of msttssyn.dll, or NULL for the image compiled into this build. Once
 * per process: after a success, later calls return 0 and change nothing. 0 on success, -1 on failure. */
int s4_init(const char *dll_path);

/* The voice modes in a directory of voice files (NULL: the voices compiled into this build). Returns
 * the number found (up to max are written), or -1. */
int s4_list_modes(const char *voice_dir, s4_mode *modes, int max);

/* An engine for a voice mode, found by name. voice_dir as above; it is also where the engine looks for
 * a user lexicon (<voice_dir>/Lex/User.lex), loaded by the first engine of the process. NULL on failure
 * (with a message in err, if given). */
s4_engine *s4_open(const char *voice_dir, const char *mode_name, char *err, size_t errlen);
/* Closing releases the engine's queues and events. The engine's own teardown is not part of the
 * decompiled code, so some of its inner buffers stay allocated (tens of kilobytes per engine), and a
 * process can open at most 127 engines over its lifetime (the engine's table of word objects). The
 * voice data shared by engines of the same voice is loaded once and kept, as in the original. */
void s4_close(s4_engine *e);

const s4_mode *s4_mode_info(const s4_engine *e);
int s4_sample_rate(const s4_engine *e);
void s4_get_limits(const s4_engine *e, s4_limits *lim);

/* Pitch and speed as SAPI's ITTSAttributes sets them: clamped to the limits, applied from the next
 * utterance on (tags in the text change them within an utterance). */
unsigned s4_get_pitch(const s4_engine *e);
int s4_set_pitch(s4_engine *e, unsigned pitch);
unsigned s4_get_speed(const s4_engine *e);
int s4_set_speed(s4_engine *e, unsigned wpm);

/* Speak text (Windows-1252, NUL-terminated), delivering PCM to fn as it is made. Returns 0 when the
 * utterance is complete, 1 if it was stopped (s4_stop, or fn returned nonzero), -1 on error. */
int s4_speak(s4_engine *e, const char *text, unsigned flags, s4_pcm_fn fn, void *user);

/* Stop the utterance in progress as soon as possible (any thread; never blocks). The rest of it is
 * discarded. A stop that arrives when nothing is being spoken is forgotten. */
void s4_stop(s4_engine *e);

/* Back to the state s4_open left: the default pitch and speed, the mode's own character (after \Chr=
 * tags) and nothing pending. The engine's random-number sequences continue (as the original's). */
int s4_reset(s4_engine *e);

#ifdef __cplusplus
}
#endif
#endif
