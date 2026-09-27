/* sapi4_tts: the original Microsoft SAPI 4 TTS engine (msttssyn.dll) as a small C library.
 *
 * The 32-bit Windows DLL runs inside the pure-C x86 interpreter (x86.c) with a tiny Win32/COM shim
 * (emu.c, crt.c, winapi.c, ole.c). Everything is plain C11: no JIT, no executable memory, no dylibs.
 *
 * One s4_engine = one loaded engine with one selected mode (voice), kept warm across utterances.
 * An engine must only be used by one host thread at a time; different engines may run concurrently
 * on different threads (no shared mutable state between them).
 * Nothing here logs or stores the text it is given.
 */
#ifndef SAPI4_TTS_H
#define SAPI4_TTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct s4_engine s4_engine;

typedef struct {
    char name[64];       /* the engine's own mode name: "Sam", "Mike in Hall", "Mary (for Telephone)" ... */
    char speaker[64];
    int gender;          /* 1 female, 2 male (SAPI 4 GENDER_*) */
    uint32_t features;
    uint8_t mode_id[16];
} s4_mode;

typedef struct {
    unsigned pitch_default, pitch_min, pitch_max;   /* the engine's pitch units (base frequency, Hz) */
    unsigned speed_default, speed_min, speed_max;   /* words per minute */
} s4_limits;

/* Lists the modes the engine in `dir` enumerates. Returns the count (at most max) or -1. */
int s4_list_modes(const char *dir, s4_mode *modes, int max, char *err, size_t errlen);

/* Loads the engine in <dir> and selects the mode by exact name: <dir>/tv_enua.dll (L&H TruVoice
 * American English, with <dir>/msvcp50.dll, its C++ runtime) if present, else <dir>/msttssyn.dll
 * (Microsoft, voice data from the same folder). NULL on failure, with a reason in err. */
s4_engine *s4_open(const char *dir, const char *mode_name, char *err, size_t errlen);
void s4_close(s4_engine *s);

int s4_sample_rate(const s4_engine *s);           /* native: 22050, or 8000 for the telephone modes */
#define S4_ENGINE_MSTTS 1        /* Microsoft msttssyn.dll */
#define S4_ENGINE_TRUVOICE 2     /* L&H TruVoice tv_enua.dll */
int s4_engine_kind(const s4_engine *s);
const s4_mode *s4_mode_info(const s4_engine *s);
void s4_get_limits(const s4_engine *s, s4_limits *out);

/* The engine's own attribute calls (ITTSAttributes::PitchSet / SpeedSet); values are clamped to the
 * limits. Only calls into the engine when the value changes. 0 ok, -1 failure. */
int s4_set_pitch(s4_engine *s, unsigned pitch);
int s4_set_speed(s4_engine *s, unsigned wpm);

/* Receives 16-bit mono PCM at s4_sample_rate() as the engine produces it; return nonzero to stop. */
typedef int (*s4_pcm_fn)(const int16_t *pcm, size_t n, void *user);

#define S4_TAGGED 1   /* interpret SAPI 4 \tags\ in the text - never for untrusted text */

/* Speaks Windows-1252 text. Returns 0 when done, 1 when stopped by the callback (the engine is reset
 * and ready for the next call), -1 on failure (the engine is dead: close it, see s4_error). */
int s4_speak(s4_engine *s, const char *cp1252, int flags, s4_pcm_fn fn, void *user);

const char *s4_error(const s4_engine *s);

typedef struct {
    uint64_t insns;          /* guest instructions for the last s4_speak */
    uint64_t host_calls;
    uint64_t heap_live;      /* guest heap bytes in use after it */
    uint32_t heap_top;       /* guest heap high-water mark (address) */
    int audio_stop_seen, textdata_done_seen;
} s4_stats;
void s4_last_stats(const s4_engine *s, s4_stats *out);

#ifdef __cplusplus
}
#endif
#endif
