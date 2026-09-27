/* ClassicVoices bridge: the small C layer between Swift and the Sam / Mike / Mary engine (Engine/sam).
 *
 * It adds the three things a screen-reader voice needs on top of the engine's library interface:
 *   - cv_sanitize: turns arbitrary Unicode into text the 2001 engine can speak (ASCII plus the
 *     Windows-1252 symbols its normalizer knows), so nothing unpronounceable reaches it;
 *   - a streaming silence trim, so short VoiceOver utterances do not carry the engine's padding;
 *   - one call that sets rate and pitch as continuous values.
 * Nothing here logs or stores the text it is given.
 */
#ifndef CV_BRIDGE_H
#define CV_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cv_voice cv_voice;

/* data_dir: folder with <spd_name>.spd / .sdf, LTTS1033.LXA and r1033tts.LXA.
 * spd_name: "Sam", "Mike" or "Mary".  effect: NULL / "none" or a SAPI 4 mode ("hall", "robosoft1", ...).
 * base_pitch_hz: 0 keeps the voice's own pitch (from its .sdf). Returns NULL and fills err on failure. */
cv_voice *cv_voice_open(const char *data_dir, const char *spd_name, const char *effect, double base_pitch_hz,
                        char *err, size_t errlen);
void cv_voice_close(cv_voice *v);
int cv_voice_sample_rate(const cv_voice *v); /* 22050 */

/* Receives 16-bit mono PCM as it is produced; return nonzero to stop. */
typedef int (*cv_pcm_fn)(const int16_t *pcm, size_t n, void *user);

/* Speaks UTF-8 text (sanitized first; SSML must already be stripped).
 * sapi_rate: -10..18, fractional allowed (0 = normal, speed factor 3^(rate/10)).
 * semitones: pitch shift, -12..12.  trim: nonzero = cut leading/trailing silence down to a few ms.
 * Returns 0 when done, 1 when stopped by the callback, -1 on error. Not reentrant per cv_voice. */
int cv_voice_speak(cv_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                   void *user);

/* Singing mode (the SAPI 5 voices only): sings the text instead of speaking it - an automatic melody, one note per
 * syllable of the actual pronunciation (literal = 0), or a literal score "word NOTE BEATS ..." (literal = 1;
 * *mismatches, may be NULL, counts words whose notes and syllables disagree). sapi_rate scales the tempo, semitones
 * and cfg->transpose (-24..12) transpose, cfg->vibrato_cents (0..200) / vibrato_rate (Hz, 0 = 5.5) add vibrato.
 * Trim, cancel (via fn) and the voice's effect as for cv_voice_speak. Returns 0, 1 when stopped, -1 on error. */
typedef struct {
    double vibrato_cents, vibrato_rate, transpose;
} cv_sing_settings;
int cv_voice_sing(cv_voice *v, const char *utf8, double sapi_rate, double semitones, const cv_sing_settings *cfg,
                  int literal, int trim, cv_pcm_fn fn, void *user, int *mismatches);
/* Song scores inside ordinary text (singing mode, DECtalk style: speak, sing the notation, speak on). A score run is
 * a sequence of "word NOTE BEATS [NOTE BEATS ...]" groups and rests "- BEATS", optionally led by "tempo N"; NOTE = an
 * upper-case A-G, optional # or b, one octave digit; BEATS = a number > 0 and <= 32; a word = letters with inner
 * hyphens / apostrophes; every token exact (no attached punctuation), whitespace-separated (line breaks or spaces).
 * A run counts only with at least 3 NOTE BEATS pairs in all, or when it starts with "tempo N" and has a pair, so
 * "Flight B6 2" or "Room C4 1" stay speech. Writes the byte ranges of the runs found (at most max), in order;
 * returns their number. */
typedef struct {
    size_t start, len;
} cv_score_run;
int cv_score_find(const char *utf8, cv_score_run *runs, int max);

/* 1 if the text looks like a literal score: it starts with "tempo " or has a line shaped "word NOTE BEATS". */
int cv_sing_is_score(const char *utf8);

/* Sanitizes UTF-8 into a malloc'd string (free with cv_free). Never returns NULL for non-NULL input
 * unless out of memory. */
char *cv_sanitize_alloc(const char *utf8);
void cv_free(void *p);

/* UTF-8 (already sanitized) -> Windows-1252, malloc'd; characters outside it become spaces. (The SAPI 4 and TruVoice
 * bridges: both engines read Windows-1252.) */
char *cv4_to_cp1252_alloc(const char *utf8);

/* The streaming silence trim on its own (cv_voice_speak uses it; so does the SAPI 4 bridge, cv4_bridge.h).
 * Feed 22050 Hz PCM in order; finish() emits the faded tail. Both return nonzero once fn asked to stop. */
typedef struct cv_trimmer cv_trimmer;
cv_trimmer *cv_trimmer_new(int trim, cv_pcm_fn fn, void *user);
int cv_trimmer_feed(cv_trimmer *t, const int16_t *pcm, size_t n);
int cv_trimmer_finish(cv_trimmer *t);
void cv_trimmer_free(cv_trimmer *t);

#ifdef __cplusplus
}
#endif
#endif
