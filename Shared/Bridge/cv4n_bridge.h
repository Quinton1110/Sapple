/* ClassicVoices SAPI 4 bridge, native: Microsoft's SAPI 4 engine (msttssyn.dll, 1999: Sam, Mike, Mary and their modes)
 * as the portable C library of Engine/sapi4 (the engine decompiled to C, sample for sample the original's output),
 * behind exactly the shape of cv4_bridge.h - which ran the same voices as the original DLL inside the x86 interpreter
 * until this bridge replaced it:
 *   - the same text sanitizer (cv_sanitize_alloc), then Windows-1252 (cv4_to_cp1252_alloc), always plain text (never
 *     S4_TAGGED: a backslash is read as "backslash", \Pit=400\ is spoken, not obeyed);
 *   - the same speed / pitch mapping onto the engine's own attributes (words per minute = default x 3^(rate/10), pitch =
 *     default x 2^(semitones/12), clamped to the limits the engine reports);
 *   - the 8 kHz "(for Telephone)" modes resampled to 22050 (cv_resample), then the same streaming silence trim.
 * The library needs the data of the user's msttssyn.dll (read at run time from <data_dir>/msttssyn.dll, never compiled in)
 * and the .vce / .cfg voice files next to it: the SAPI4Voices folder, as before.
 *
 * Engines are kept for the life of the process. The library cannot release an engine's inner buffers (~0.4 MB each; its
 * teardown is not part of the decompilation) and a process can open at most 127 over its lifetime, so closing a voice
 * PARKS its engine, still warm, and the next open of the same mode takes it back instead of opening another. A process
 * therefore opens at most one engine per mode, plus one per extra voice of that mode speaking at the same moment.
 * All engines share one process-wide lock inside the library: utterances of different voices take turns.
 * Nothing here logs or stores the text it is given. */
#ifndef CV4N_BRIDGE_H
#define CV4N_BRIDGE_H

#include "cv_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cv4n_voice cv4n_voice;

/* data_dir: the folder with msttssyn.dll and the .vce / .cfg files (SAPI4Voices). mode: the engine's own mode name
 * ("Sam", "Mike in Hall", "Mary (for Telephone)" ...). NULL and err filled on failure. */
cv4n_voice *cv4n_voice_open(const char *data_dir, const char *mode, char *err, size_t errlen);
/* Parks the voice's engine for the next open of its mode (a failed engine is dropped instead). */
void cv4n_voice_close(cv4n_voice *v);
int cv4n_voice_sample_rate(const cv4n_voice *v); /* 22050 */

/* Exactly cv4_voice_speak's contract: sapi_rate -10..18 (speed factor 3^(rate/10), clamped to the engine's own range,
 * 30 wpm .. 3x its default), semitones -12..12, trim. Returns 0 done, 1 stopped by the callback, -1 on error - after -1
 * the voice must be closed, not reused. Not reentrant per voice; voices on different threads are safe. */
int cv4n_voice_speak(cv4n_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                     void *user);

/* The words per minute / pitch a rate / semitone value maps to for this voice (tests, UI). */
unsigned cv4n_voice_wpm_for_rate(const cv4n_voice *v, double sapi_rate);
unsigned cv4n_voice_pitch_for_semitones(const cv4n_voice *v, double semitones);

/* Closes every parked engine for good, so the next open of each mode starts a fresh one - the engine carries its noise
 * and filter state from one utterance to the next, so renders compare only from fresh engines. For tests and the
 * self-test (ClassicEngine.releaseIdle): each fresh engine costs ~0.4 MB that is never returned, and counts against the
 * 127. Returns how many were closed. */
int cv4n_discard_parked(void);

/* Engines this process has opened so far (tests). */
int cv4n_engines_opened(void);

#ifdef __cplusplus
}
#endif
#endif
