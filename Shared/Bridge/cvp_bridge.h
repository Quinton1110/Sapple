/* ClassicVoices IBM ProTALKER 97 Japanese bridge (読み秀君 / 読み子ちゃん, Engine/protalker + SAPI4/emu) - same shape as
 * cv_bridge.h / cvo_bridge.h / cvn_bridge.h:
 *   - cv_sanitize_unicode_alloc text sanitizer (preserves Japanese kana/kanji, strips emoji and control bytes);
 *   - rate on the SAPI scale (-10..18) mapped to ProTALKER's spcSynCntl speed 4..16 (default 6 at SAPI rate 0);
 *   - pitch in semitones (-12..12) mapped to ProTALKER's spcSynCntl pitch 50..170 (default 97 at 0 semitones);
 *   - native 22050 Hz mono PCM streamed through cv_trimmer (no resampling needed).
 * Nothing here logs or stores the text it is given. */
#ifndef CVP_BRIDGE_H
#define CVP_BRIDGE_H

#include "cv_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cvp_voice cvp_voice;

/* Non-zero when ProTalkerVoice/ holds Tts_mgr.dll, DICT/ACC2.DCT and UNIT/WFC{M,F}1000.DAT. */
int cvp_voice_has_data(const char *dir);

/* dir: ProTalkerVoice/ (Tts_mgr.dll, DICT/, UNIT/).
 * voice: "ProTalker_Hide" / "読み秀君" (male) or "ProTalker_Ko" / "読み子ちゃん" (female).
 * Returns NULL and fills err on failure. */
cvp_voice *cvp_voice_open(const char *dir, const char *voice, char *err, size_t errlen);
void cvp_voice_close(cvp_voice *v);
int cvp_voice_sample_rate(const cvp_voice *v); /* 22050 */

/* Same contract as cv_voice_speak: returns 0 done, 1 stopped by the callback, -1 on error. */
int cvp_voice_speak(cvp_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                    void *user);

/* Measured speed factor relative to ProTALKER's default speed 6 (0.80x at rate -10 .. 5.95x at rate +18). */
double cvp_speed_factor(double sapi_rate);

#ifdef __cplusplus
}
#endif
#endif
