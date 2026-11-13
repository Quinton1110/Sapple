/* IBM ProTALKER 97 Japanese TTS engine (読み秀君 / 読み子ちゃん) running in the SAPI4/emu x86 interpreter. */
#ifndef PROTALKER_TTS_H
#define PROTALKER_TTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTALKER_SAMPLE_RATE 22050

typedef struct protalker_tts protalker_tts;

/* Return non-zero from the callback to stop synthesis immediately. */
typedef int (*protalker_audio_cb)(const int16_t *pcm, size_t samples, void *user);

/* Non-zero when dir holds Tts_mgr.dll, DICT/ACC2.DCT and UNIT/WFC{M,F}1000.DAT (matched case-insensitively). */
int protalker_tts_has_data(const char *dir);

/* voice_name: "読み秀君" / "ProTalker_Hide" (actor 1, WFCM1000) or "読み子ちゃん" / "ProTalker_Ko" (actor 2, WFCF1000). */
protalker_tts *protalker_tts_open(const char *dir, const char *voice_name, char *err, size_t errlen);
void protalker_tts_close(protalker_tts *t);

/* Synthesizes utf8_text as 22050 Hz 16-bit mono PCM. Returns 0 when finished, 1 when stopped by cb, -1 on error. */
int protalker_tts_speak(protalker_tts *t, const char *utf8_text, double sapi_rate, double semitones,
                        protalker_audio_cb cb, void *user);

#ifdef __cplusplus
}
#endif
#endif
