/* Microsoft Sam / Mike / Mary text to speech: the library interface (for screen readers and other hosts).
 *
 * The same shape as the Microsoft Anna port's anna_tts.h, so one driver can talk to both engines.
 * Output is 22050 Hz (SAM_TTS_SAMPLE_RATE), 16-bit mono PCM, streamed through a callback as it is
 * produced.
 *
 *   sam_speech *t = sam_tts_open("path/to/voice-data", "Sam", err, sizeof err);
 *   sam_tts_speak(t, "Hello world.", 0, &callbacks);  // blocks until done or cancelled
 *   sam_tts_cancel(t);                                     // from any thread: stops the running speak
 *   sam_tts_close(t);
 *
 * The data folder holds <Voice>.spd and <Voice>.sdf (the voice and its base pitch) next to the shared
 * LTTS1033.LXA and r1033tts.LXA, as installed by the original Microsoft TTS voice package.
 *
 * Note on the name: the low-level engine header sam.h already has a sam_tts_speak_pcm() that takes a raw
 * PCM callback, so the library's is sam_tts_speak(). Everything else follows the anna_tts_*
 * spelling.
 */
#ifndef SAM_TTS_H
#define SAM_TTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAM_TTS_SAMPLE_RATE 22050

#if defined(_WIN32) && defined(SAM_BUILD_DLL)
#define SAM_API __declspec(dllexport)
#elif defined(_WIN32) && defined(SAM_USE_DLL)
#define SAM_API __declspec(dllimport)
#elif defined(__GNUC__) && defined(SAM_BUILD_DLL)
#define SAM_API __attribute__((visibility("default")))
#else
#define SAM_API
#endif

typedef struct sam_speech sam_speech;

enum {
    SAM_EV_SENTENCE = 1, /* a sentence starts (text_pos/text_len = its span) */
    SAM_EV_WORD = 2,     /* a word starts (text_pos/text_len = the word as written) */
    SAM_EV_BOOKMARK = 3, /* <bookmark mark="..."/> reached (name) */
    SAM_EV_END = 4       /* all text spoken (not sent when cancelled) */
};

typedef struct {
    int type;
    uint64_t audio_pos; /* samples since the start of this speak call */
    long text_pos;      /* byte offset into the UTF-8 text given to sam_tts_speak */
    long text_len;      /* bytes */
    const char *name;   /* bookmark name */
} sam_event;

typedef struct {
    /* PCM as it is produced; return nonzero to stop speaking */
    int (*audio)(const int16_t *pcm, size_t n, void *user);
    /* events, delivered just before the audio they belong to (may be NULL) */
    void (*event)(const sam_event *ev, void *user);
    void *user;
} sam_callbacks;

#define SAM_SPEAK_XML 1 /* the text is SAPI XML: <bookmark mark="x"/>, <silence msec="300"/>,
                           <rate speed="-3">, <volume level="50">, <pitch middle="5">, ... */

/* voice: "Sam" (the default when NULL), "Mike" or "Mary" - any <Voice>.spd in the folder. */
SAM_API sam_speech *sam_tts_open(const char *data_dir, const char *voice, char *err, size_t errlen);
SAM_API void sam_tts_close(sam_speech *t);

/* rate: SAPI scale -10..10 (0 = normal); up to 18 is allowed for fast listening, as the engine does.
 * volume: 0..100. pitch: -10..10 (+-10 = +-5 semitones), added to the XML pitch. */
SAM_API void sam_tts_set_rate(sam_speech *t, int rate);
SAM_API void sam_tts_set_volume(sam_speech *t, int volume);
SAM_API void sam_tts_set_pitch(sam_speech *t, int pitch);

/* ClassicVoices patch: continuous rate (SAPI scale -10..18, may be fractional) and pitch in semitones
 * (-12..12); once called these replace set_rate / set_pitch. */
SAM_API void sam_tts_set_rate_pitch_f(sam_speech *t, double sapi_rate, double semitones);
/* ClassicVoices patch: echo tail after the text in ms, faded out (-1 = upstream's full second). */
SAM_API void sam_tts_set_fx_tail(sam_speech *t, int ms);
/* ClassicVoices patch: override the voice's base pitch in Hz (the SAPI 4 RoboSoft voices); reloads. */
SAM_API int sam_tts_set_base_pitch(sam_speech *t, double hz);

/* One of the SAPI 4 voice modes: "none", "hall", "stadium", "space", "room", "whisper",
 * "robosoft1".."robosoft6", "robot", "monotone". Whisper and the monotone RoboSofts also change the
 * synthesis (noise excitation / flat pitch), so this reloads the voice. Returns 0, or -1 for an
 * unknown name (the effect is then left alone). */
SAM_API int sam_tts_set_effect(sam_speech *t, const char *name);

/* Speak UTF-8 text; returns 0 when done, 1 when cancelled/stopped, -1 on error.
 * Not reentrant for one sam_speech; use one handle per thread. */
SAM_API int sam_tts_speak(sam_speech *t, const char *utf8, int flags, const sam_callbacks *cb);

/* thread-safe: stop the current speak as soon as possible (within one audio chunk) */
SAM_API void sam_tts_cancel(sam_speech *t);

/* ClassicVoices patch (singing mode for the SAPI 5 voices): sing utf8 instead of speaking it. literal = 0: plain
 * text gets an automatic melody (one note per syllable, sam.h sam_tts_sing_text); literal = 1: the text is a
 * score ("word NOTE BEATS ...", "- BEATS", "tempo N") and *mismatches (may be NULL) counts words whose notes and
 * syllables disagree. The rate set above scales the tempo of the automatic melody, the pitch transposes it; cfg
 * (NULL = none) adds vibrato and a transposition. Cancel, volume and the voice's effect work as for speaking.
 * Returns 0, 1 when cancelled / stopped, -1 on error. No events are delivered. */
typedef struct {
    float vibrato_cents; /* depth in cents (0 = none) */
    float vibrato_rate;  /* Hz (0 = 5.5) */
    double transpose;    /* semitones */
} sam_sing_cfg;
SAM_API int sam_tts_sing_speech(sam_speech *t, const char *utf8, int literal, const sam_sing_cfg *cfg,
                                const sam_callbacks *cb, int *mismatches);

/* 22050, or whatever the loaded voice runs at */
SAM_API int sam_tts_sample_rate(const sam_speech *t);

#ifdef __cplusplus
}
#endif
#endif
