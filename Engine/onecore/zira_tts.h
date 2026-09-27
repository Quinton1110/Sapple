/* Microsoft David / Zira / Mark (the Windows OneCore en-US voices) text to speech: the library interface
 * (for screen readers and other hosts).
 *
 * Output is 16 kHz, 16-bit mono PCM, streamed through a callback as it is produced.
 *
 *   zira_tts *t = zira_tts_open(NULL, "David", err, sizeof err);   // NULL = the installed voice folder
 *   zira_tts_speak(t, "Hello world.", 0, &callbacks);              // blocks until done or cancelled
 *   zira_tts_cancel(t);                                            // from any thread: stops the running speak
 *   zira_tts_close(t);
 *
 * data_dir holds the voice data as Windows installs it: MSTTSLocEnUS.dat and M1033<Voice>.{APM,BEP,INI}
 * (default C:/Windows/Speech_OneCore/Engines/TTS/en-US).
 *
 * One zira_tts is one speaking pipeline: zira_tts_speak is not reentrant for a single handle and the settings
 * apply to the next speak call.  Use one handle per thread; only zira_tts_cancel may be called concurrently.
 */
#ifndef ZIRA_TTS_H
#define ZIRA_TTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZIRA_SAMPLE_RATE 16000

#if defined(_WIN32) && defined(ZIRA_BUILD_DLL)
#define ZIRA_API __declspec(dllexport)
#elif defined(_WIN32) && defined(ZIRA_USE_DLL)
#define ZIRA_API __declspec(dllimport)
#elif defined(__GNUC__) && defined(ZIRA_BUILD_DLL)
#define ZIRA_API __attribute__((visibility("default")))
#else
#define ZIRA_API
#endif

typedef struct zira_tts zira_tts;

enum {
    ZIRA_EV_SENTENCE = 1, /* a sentence starts (text_pos/text_len = its span) */
    ZIRA_EV_WORD = 2,     /* a word starts (text_pos/text_len = the word) */
    ZIRA_EV_BOOKMARK = 3, /* <bookmark mark="..."/> reached (name) */
    ZIRA_EV_END = 4       /* all text spoken (not sent when cancelled) */
};

typedef struct {
    int type;
    uint64_t audio_pos; /* samples since the start of this speak call */
    long text_pos;      /* byte offset into the UTF-8 text given to zira_tts_speak (0 for ZIRA_EV_BOOKMARK: the
                           engine's TTS_BOOKMARK event carries no source offset, only the name) */
    long text_len;      /* bytes (0 for bookmarks; for ZIRA_EV_END, text_pos is the end of the text) */
    const char *name;   /* bookmark name (UTF-8), NULL for the other events */
} zira_event;

typedef struct {
    /* PCM as it is produced; return nonzero to stop speaking */
    int (*audio)(const int16_t *pcm, size_t n, void *user);
    /* events, delivered just before the audio they belong to (may be NULL) */
    void (*event)(const zira_event *ev, void *user);
    void *user;
} zira_callbacks;

#define ZIRA_SPEAK_XML 1    /* the text is SAPI XML: <emph>, <silence msec="300"/>, <pitch middle="5">,
                               <rate speed="-3">, <volume level="50">, <spell>, <partofsp>, <context>,
                               <bookmark mark="x"/>, <lang langid="..."> ...  Without this flag the text is
                               spoken literally (SAPI's SPF_IS_NOT_XML). */

/* voice: "David", "Zira" or "Mark" (case-insensitive; NULL = David); ClassicVoices patch: also the en-GB voices
 * "Hazel", "George" and "Susan", which read M2057<Voice>.{APM,INI,BEP}, MSTTSLocEnGB.dat and the en-GB domain files
 * from the same data_dir.
 * data_dir: NULL = C:/Windows/Speech_OneCore/Engines/TTS/en-US. */
ZIRA_API zira_tts *zira_tts_open(const char *data_dir, const char *voice, char *err, size_t errlen);
ZIRA_API void zira_tts_close(zira_tts *t);
/* ClassicVoices patch: the locale (LCID) of a voice name: 1033 en-US, 2057 en-GB, 0 unknown. */
ZIRA_API int zira_tts_voice_lcid(const char *voice);

/* The voice this handle speaks with, and the data folder it was loaded from. */
ZIRA_API const char *zira_tts_voice(const zira_tts *t);

/* rate: the SAPI scale -10..10 (0 = normal, each step x3^(1/10), so +10 = 3x).
 *   Fast extension (beyond the original engine, which clamps the engine-site rate to [-10,10]): 11..20 keep the
 *   site rate at 10 and add the remainder as a per-phone SAPI <rate> step, the same multiplier the engine applies
 *   to <rate speed="...">.  rate 20 is therefore 9x normal.  -10..10 is bit-identical to the engine.
 * volume: 0..100 (the engine's -25 dB range curve).
 * pitch: -10..10, added to the XML <pitch middle> of every phone and clamped there; +-10 = +-5 semitones. */
ZIRA_API void zira_tts_set_rate(zira_tts *t, int rate);
/* ClassicVoices patch: the same scale with fractions (-10..20). Whole values are exactly zira_tts_set_rate; the
 * fraction goes into the engine's own fine (1/1000 step) rate on the waveform and into the duration rate. */
ZIRA_API void zira_tts_set_rate_f(zira_tts *t, double rate);
ZIRA_API void zira_tts_set_volume(zira_tts *t, int volume);
ZIRA_API void zira_tts_set_pitch(zira_tts *t, int pitch);

/* The voice's hidden emotion settings ([EmotionRecipe] of M1033<Voice>.INI: "happy", "sad", "angry"; NULL or ""
 * = neutral).  Returns 0 on success, -1 if this voice has no such emotion (the setting is then unchanged).
 * See ZbStyle in zb.h for what each one does and for the (un)verified status of the formulas.
 * zira_tts_emotion_name enumerates the voice's emotions (NULL past the last one). */
ZIRA_API int zira_tts_set_emotion(zira_tts *t, const char *name);
ZIRA_API const char *zira_tts_emotion_name(const zira_tts *t, int i);

/* Speak UTF-8 text; returns 0 when done, 1 when cancelled/stopped, -1 on error. */
ZIRA_API int zira_tts_speak(zira_tts *t, const char *utf8, int flags, const zira_callbacks *cb);

/* thread-safe: stop the current zira_tts_speak as soon as possible (within one audio chunk) */
ZIRA_API void zira_tts_cancel(zira_tts *t);

/* ClassicVoices addition: Microsoft Eva only - her CRF prosody models on (1, default) or off (0: the rule-based
 * prosody), and printing the per-word labels to stderr (not in CV_NO_DEBUG_ENV builds).  -1 for other voices. */
int zira_tts_eva_debug(zira_tts *t, int models, int verbose);

#ifdef __cplusplus
}
#endif
#endif
