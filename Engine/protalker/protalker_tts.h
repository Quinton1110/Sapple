/* IBM ProTALKER 97 Japanese TTS engine (読み秀君 / 読み子ちゃん) via SAPI4/emu x86 interpreter. */
#ifndef PROTALKER_TTS_H
#define PROTALKER_TTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTALKER_SAMPLE_RATE 22050

typedef struct protalker_tts protalker_tts;

typedef int (*protalker_audio_cb)(const int16_t *pcm, size_t samples, void *user);

void protalker_tts_set_bundle_dir(const char *dir);
void protalker_tts_set_extra_dir(const char *dir);
int protalker_tts_is_voice(const char *voice_name);

protalker_tts *protalker_tts_open(const char *dir, const char *voice_name, char *err, size_t errlen);
void protalker_tts_close(protalker_tts *t);

int protalker_tts_speak(protalker_tts *t, const char *utf8_text, double sapi_rate, double semitones,
                        protalker_audio_cb cb, void *user);

#ifdef __cplusplus
}
#endif
#endif
