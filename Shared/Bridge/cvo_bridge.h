/* ClassicVoices OneCore bridge: Microsoft David, Zira and Mark (en-US), Hazel, George and Susan (en-GB), Catherine and
 * James (en-AU), Linda and Richard (en-CA), the neural Eva (en-US), Sarah (en-GB) and Matilda (en-AU) - the
 * Windows 10 / 11 "OneCore" voices, Engine/onecore -
 * behind the same shape as cv_bridge.h / cv4_bridge.h / cva_bridge.h, so Swift drives every engine alike:
 *   - the same text sanitizer (cv_sanitize_alloc), then plain text (never SAPI XML: "<rate>" is read, not obeyed);
 *   - rate on the same SAPI scale (-10..18, speed 3^(rate/10)) onto the engine's own rate (fractions through our
 *     zira_tts_set_rate_f patch); pitch in semitones onto its own pitch steps (half semitones, +-5 semitones: the
 *     engine's range);
 *   - the voice's hidden emotion settings ("happy", "sad", "angry" from its INI; NULL / "" = neutral);
 *   - 16 kHz resampled to 22050 Hz (cv_resample.h), then the same streaming silence trim (cv_trimmer).
 * Nothing here logs or stores the text it is given. */
#ifndef CVO_BRIDGE_H
#define CVO_BRIDGE_H

#include "cv_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cvo_voice cvo_voice;

/* data_dir: flat folder with MSTTSLocEnUS.dat and M1033<voice>.{APM,BEP,INI} (en-US), MSTTSLocEnGB.dat and
 * M2057<voice>.{APM,INI,BEP} (en-GB) and the domain files; voice: "David", "Zira", "Mark", "Hazel", "George", "Susan", "Eva", "Sarah",
 * "Catherine", "James", "Matilda" (M3081<voice>.*, MSTTSLocEnAU.dat), "Linda", "Richard" (M4105<voice>.*, MSTTSLocEnCA.dat).
 * NULL and err filled on failure (missing data, unknown voice). */
cvo_voice *cvo_voice_open(const char *data_dir, const char *voice, char *err, size_t errlen);
void cvo_voice_close(cvo_voice *v);
int cvo_voice_sample_rate(const cvo_voice *v); /* 22050 */

/* The emotion for the next speak calls: "happy", "sad", "angry", or NULL / "" for the voice's normal speech.
 * Returns 0, or -1 if the voice has no such emotion (then it speaks neutrally). */
int cvo_voice_set_emotion(cvo_voice *v, const char *emotion);

/* Same contract as cv_voice_speak: sapi_rate -10..18 (fractional allowed), semitones -12..12 (the engine
 * stops at +-5), trim. Returns 0 done, 1 stopped by the callback, -1 on error. Not reentrant per voice;
 * separate voices may speak on separate threads at the same time. */
int cvo_voice_speak(cvo_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                    void *user);

/* The engine pitch step (-10..10, half semitones) a semitone value maps to (tests, UI). */
int cvo_pitch_for_semitones(double semitones);

#ifdef __cplusplus
}
#endif
#endif
