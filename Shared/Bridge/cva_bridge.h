/* ClassicVoices Anna bridge: Microsoft Anna (the Windows Vista / 7 SAPI 5 "TTS20" voice, Engine/anna) behind the
 * same shape as cv_bridge.h / cv4_bridge.h, so Swift drives every engine alike:
 *   - the same text sanitizer (cv_sanitize_alloc), then plain text (never SAPI XML: "<rate>" is read, not obeyed);
 *   - rate on the same SAPI scale (-10..18, speed 3^(rate/10)), onto the engine's own rate control - which is
 *     the original engine's WSOLA (Anna always talks through it: her DefaultRate 2 is a 1.25x squeeze), so
 *     nothing extra is ever stretched on top; pitch in semitones onto its own TD-PSOLA pitch (half-semitone
 *     steps, +-5 semitones: the engine's range);
 *   - 16 kHz resampled to 22050 Hz (cv_resample.h), then the same streaming silence trim (cv_trimmer).
 * Nothing here logs or stores the text it is given. */
#ifndef CVA_BRIDGE_H
#define CVA_BRIDGE_H

#include "cv_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cva_voice cva_voice;

/* data_dir: flat folder with M1033DSK.{CSD,IDX,UNT,CRT,APL,UDT,TTS,LTS,WIH,KEY,HAN}. NULL and err filled on
 * failure (missing data, or an engine built with FMA contraction - it would not be bit-exact). */
cva_voice *cva_voice_open(const char *data_dir, char *err, size_t errlen);
void cva_voice_close(cva_voice *v);
int cva_voice_sample_rate(const cva_voice *v); /* 22050 */

/* Same contract as cv_voice_speak: sapi_rate -10..18 (fractional allowed), semitones -12..12 (the engine
 * stops at +-5), trim. Returns 0 done, 1 stopped by the callback, -1 on error. Not reentrant per voice;
 * separate voices may speak on separate threads at the same time. */
int cva_voice_speak(cva_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                    void *user);

/* The engine pitch step (-10..10, half semitones) a semitone value maps to (tests, UI). */
int cva_pitch_for_semitones(double semitones);

#ifdef __cplusplus
}
#endif
#endif
