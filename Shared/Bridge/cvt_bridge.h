/* ClassicVoices TruVoice bridge: L&H / Centigram TruVoice American English (the Microsoft Agent / BonziBuddy voices)
 * on OpenTV (Engine/opentv, a portable-C decompilation of the engine), behind the same shape as cv4_bridge.h - which
 * ran the same ten voices as the original tv_enua.dll inside the x86 interpreter until 2026-09-26:
 *   - the same text sanitizer (cv_sanitize_alloc), then Windows-1252 (cv4_to_cp1252_alloc), always plain text;
 *   - the same streaming silence trim (cv_trimmer), after the same 15 Hz DC blocker;
 *   - rate and pitch on the same scales as before: the SAPI 4 words per minute the old path asked tv_enua.dll for
 *     (default x 3^(rate/10), 50..250) and its pitch (default x 2^(semitones/12), 50..400). The pitch goes to the
 *     engine as it is; the speed goes through a measured table (cvt_engine_wpm) that picks the OpenTV rate row whose
 *     timing matches tv_enua.dll's at that speed - the two builds differ there;
 *   - 11025 Hz resampled to 22050.
 * Nothing here logs or stores the text it is given. */
#ifndef CVT_BRIDGE_H
#define CVT_BRIDGE_H

#include "cv_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cvt_voice cvt_voice;

/* mode: the voice's SAPI 4 mode name, "Adult Male #2, American English (TruVoice)" (or just "Adult Male #2").
 * No data folder: the engine's tables are linked in (TruVoiceData/tvdata.s). NULL and err filled on failure. */
cvt_voice *cvt_voice_open(const char *mode, char *err, size_t errlen);
void cvt_voice_close(cvt_voice *v);
int cvt_voice_sample_rate(const cvt_voice *v); /* 22050 */

/* Same contract as cv4_voice_speak: sapi_rate -10..18, semitones -12..12, trim. Returns 0 done, 1 stopped by the
 * callback, -1 on error (close the voice then). Not reentrant per voice; voices on different threads are safe (the
 * engine keeps some state in process globals, so utterances of different voices are serialised by one lock and each
 * voice's share of that state is saved and restored around its own). */
int cvt_voice_speak(cvt_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                    void *user);

/* The SAPI 4 words per minute a rate maps to (50..250, exactly what cv4_voice_wpm_for_rate gave), the OpenTV words per
 * minute that speed is spoken at, and the pitch a semitone value maps to (tests, UI). */
unsigned cvt_voice_wpm_for_rate(const cvt_voice *v, double sapi_rate);
unsigned cvt_engine_wpm(unsigned sapi_wpm);
unsigned cvt_voice_pitch_for_semitones(const cvt_voice *v, double semitones);

#ifdef __cplusplus
}
#endif
#endif
