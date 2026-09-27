/* Microsoft Anna text to speech: the library interface (for screen readers and other hosts).
 *
 * Output is 16 kHz, 16-bit mono PCM, streamed through a callback as it is produced.
 *
 *   anna_tts *t = anna_tts_open("path/to/anna-data", err, sizeof err);
 *   anna_tts_speak(t, "Hello world.", 0, &callbacks);   // blocks until done or cancelled
 *   anna_tts_cancel(t);                                  // from any thread: stops the running speak
 *   anna_tts_close(t);
 *
 * The data folder holds the voice files: either the installed layout (the TTS20 folder, with
 * en-US/enu-dsk/M1033DSK.* and the MSTTSDecWrp.dll / MSTTSEngine.dll next to it) or a flat folder with
 * M1033DSK.{CSD,IDX,UNT,CRT,APL,UDT,TTS,LTS,WIH,KEY,HAN} made by the anna_extract tool.
 */
#ifndef ANNA_TTS_H
#define ANNA_TTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANNA_SAMPLE_RATE 16000

#if defined(_WIN32) && defined(ANNA_BUILD_DLL)
#define ANNA_API __declspec(dllexport)
#elif defined(_WIN32) && defined(ANNA_USE_DLL)
#define ANNA_API __declspec(dllimport)
#elif defined(__GNUC__) && defined(ANNA_BUILD_DLL)
#define ANNA_API __attribute__((visibility("default")))
#else
#define ANNA_API
#endif

typedef struct anna_tts anna_tts;

enum {
    ANNA_EV_SENTENCE = 1, /* a sentence starts (text_pos/text_len = its span) */
    ANNA_EV_WORD = 2,     /* a word starts (text_pos/text_len = the word) */
    ANNA_EV_BOOKMARK = 3, /* <bookmark mark="..."/> reached (name) */
    ANNA_EV_END = 4       /* all text spoken (not sent when cancelled) */
};

typedef struct {
    int type;
    uint64_t audio_pos; /* samples since the start of this speak call */
    long text_pos;      /* byte offset into the UTF-8 text given to anna_tts_speak */
    long text_len;      /* bytes */
    const char *name;   /* bookmark name */
} anna_event;

typedef struct {
    /* PCM as it is produced; return nonzero to stop speaking */
    int (*audio)(const int16_t *pcm, size_t n, void *user);
    /* events, delivered just before the audio they belong to (may be NULL) */
    void (*event)(const anna_event *ev, void *user);
    void *user;
} anna_callbacks;

#define ANNA_SPEAK_XML 1        /* the text is SAPI XML: <emph>, <silence msec="300"/>, <pitch middle="5">,
                                   <rate speed="-3">, <volume level="50">, <spell>, <bookmark mark="x"/> ... */
#define ANNA_SPEAK_PUNCTUATION 2 /* speak punctuation marks ("comma") */

ANNA_API anna_tts *anna_tts_open(const char *data_dir, char *err, size_t errlen);
ANNA_API void anna_tts_close(anna_tts *t);

/* rate: SAPI scale -10..10 (0 = normal); up to 18 is allowed for fast listening (beyond the original engine).
 * volume: 0..100. pitch: -10..10 (added to the XML pitch; +-10 = +-5 semitones). */
ANNA_API void anna_tts_set_rate(anna_tts *t, int rate);
/* ClassicVoices patch: the same scale with a fractional rate (-10..18); anna_tts_set_rate(t, n) == this with n. */
ANNA_API void anna_tts_set_rate_f(anna_tts *t, double rate);
ANNA_API void anna_tts_set_volume(anna_tts *t, int volume);
ANNA_API void anna_tts_set_pitch(anna_tts *t, int pitch);

/* speak UTF-8 text; returns 0 when done, 1 when cancelled/stopped, -1 on error.
 * Not reentrant for one anna_tts; use one anna_tts per thread (they can share nothing). */
ANNA_API int anna_tts_speak(anna_tts *t, const char *utf8, int flags, const anna_callbacks *cb);

/* thread-safe: stop the current anna_tts_speak as soon as possible (within one audio chunk) */
ANNA_API void anna_tts_cancel(anna_tts *t);

#ifdef __cplusplus
}
#endif
#endif
