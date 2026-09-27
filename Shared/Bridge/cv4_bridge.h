/* ClassicVoices SAPI 4 bridge: the original SAPI 4 engines - Microsoft's msttssyn.dll and L&H TruVoice's
 * tv_enua.dll (SAPI4/emu/sapi4_tts.h, running in the x86 interpreter) - behind the same shape as
 * cv_bridge.h, so Swift drives every engine alike:
 *   - the same text sanitizer (cv_sanitize_alloc), then Windows-1252 for the 1999 engine;
 *   - the same streaming silence trim (cv_trimmer);
 *   - rate and pitch on the same scales as cv_voice_speak, mapped onto the engine's own attribute
 *     calls (speed in words per minute, pitch in its base-frequency units) - never time-stretched;
 *   - output always 22050 Hz: the 8 kHz "(for Telephone)" modes and TruVoice (11025 Hz) are resampled;
 *     TruVoice's DC offset is removed (15 Hz high-pass) so the silence trim works.
 * Nothing here logs or stores the text it is given. */
#ifndef CV4_BRIDGE_H
#define CV4_BRIDGE_H

#include "cv_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cv4_voice cv4_voice;

/* data_dir: folder with msttssyn.dll and the .vce/.cfg files, or with tv_enua.dll + msvcp50.dll.
 * mode: the engine's mode name ("Sam", "Mike in Hall", "Mary (for Telephone)", "Adult Male #2,
 * American English (TruVoice)" ...). NULL and err filled on failure. */
cv4_voice *cv4_voice_open(const char *data_dir, const char *mode, char *err, size_t errlen);
void cv4_voice_close(cv4_voice *v);
int cv4_voice_sample_rate(const cv4_voice *v); /* 22050 */

/* Same contract as cv_voice_speak: sapi_rate -10..18 (speed factor 3^(rate/10), clamped to the engine's
 * own range: Microsoft 30 wpm .. 3x its default, TruVoice 50 .. 250 wpm), semitones -12..12, trim. Returns 0 done, 1 stopped by the callback,
 * -1 on error - after -1 the voice is dead and must be closed, not reused. Not reentrant per voice. */
int cv4_voice_speak(cv4_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                    void *user);

/* The words per minute / pitch a rate / semitone value maps to for this voice (tests, UI). */
unsigned cv4_voice_wpm_for_rate(const cv4_voice *v, double sapi_rate);
unsigned cv4_voice_pitch_for_semitones(const cv4_voice *v, double semitones);

#ifdef __cplusplus
}
#endif
#endif
