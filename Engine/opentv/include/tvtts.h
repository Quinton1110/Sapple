/*
 * TruVoice as a library.
 *
 * A flat C API over the decompiled Centigram TruVoice engine, meant for
 * screen readers and anything else that wants the synthesizer without
 * SAPI: no COM, no registry, no window messages, no threads, and no audio
 * device.  The caller drives synthesis on its own thread and plays the
 * samples itself, which is what a screen reader wants -- it already owns
 * its output device, its ducking and its cancellation.
 *
 * The shape is deliberately close to eSpeak's: one callback receives audio
 * and index marks interleaved in stream order, and returning non-zero from
 * it aborts synthesis at once.
 *
 *   tvtts_synth *s = tvtts_create(11025);
 *   tvtts_set_voice(s, 0);
 *   tvtts_speak_utf8(s, "Hello world.", on_event, NULL);
 *   tvtts_destroy(s);
 *
 * Thread safety: a tvtts_synth is not thread safe; give each thread its
 * own.  tvtts_add_lexicon is process-global (the engine's user lexicon is
 * a single static table) and is not thread safe against anything.
 */
#ifndef TVTTS_H
#define TVTTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(TVTTS_SHARED)
#  ifdef TVTTS_BUILD
#    define TVTTS_API __declspec(dllexport)
#  else
#    define TVTTS_API __declspec(dllimport)
#  endif
#else
#  define TVTTS_API
#endif

/* Everything is cdecl, and the DLL exports undecorated names, so ctypes
 * and P/Invoke can bind it without a mangled-name dance. */
#define TVTTS_CALL

typedef struct tvtts_synth tvtts_synth;

enum {
    TVTTS_AUDIO = 0,   /* samples are ready */
    TVTTS_MARK  = 1,   /* synthesis reached an index mark */
    TVTTS_END   = 2    /* end of this utterance; no samples follow */
};

/*
 * One event.  Audio and marks arrive in stream order, so when a mark
 * arrives the caller has already been handed every sample that precedes
 * it: queue the audio, remember sample_pos, and raise the bookmark when
 * the play cursor passes it.
 *
 * Every field is 32 bits or smaller and the struct has no padding holes on
 * any normal ABI, so it binds from other languages without surprises.
 */
typedef struct {
    int32_t        type;        /* TVTTS_AUDIO / TVTTS_MARK / TVTTS_END */
    uint32_t       count;       /* TVTTS_AUDIO: number of samples */
    const int16_t *samples;     /* TVTTS_AUDIO: signed 16-bit mono, host order */
    uint32_t       mark;        /* TVTTS_MARK: the value from the escape */
    uint32_t       sample_pos;  /* samples emitted before this event */
} tvtts_event;

/* Return non-zero to abort synthesis immediately. */
typedef int (TVTTS_CALL *tvtts_callback)(const tvtts_event *ev, void *user);

/* rate is 11025 (the engine's native rate) or 8000.  NULL on failure. */
TVTTS_API tvtts_synth *TVTTS_CALL tvtts_create(uint32_t sample_rate);

/*
 * Output rate, as an index rather than a number of hertz, because the
 * engine has exactly three and they are not interchangeable with arbitrary
 * rates: each needs its own resonator tables.
 *
 *   TVTTS_SR_8K    8000 Hz, the original's narrowband set, for telephony
 *   TVTTS_SR_11K  11025 Hz, what TruVoice shipped as its desktop rate
 *   TVTTS_SR_16K  16000 Hz, which the original never offered
 *
 * 11 kHz is the default and is what people know the voices to sound like;
 * the other two are the same voices resampled by the synthesiser itself
 * rather than afterwards, so 16 kHz is genuinely more bandwidth and not an
 * upsample.  Its tables are OpenTV's own, computed from formulas that
 * reproduce both of the original's sets exactly.
 *
 * Changing rate re-initialises the filters and the output stage, so it is
 * refused part way through an utterance: call it between them.  Voice,
 * pitch, rate and volume are preserved across it.  Returns 0, or -1 if the
 * synth is null, the index is not one of the three, or an utterance is in
 * progress.
 */
#define TVTTS_SR_8K   0
#define TVTTS_SR_11K  1
#define TVTTS_SR_16K  2

TVTTS_API int TVTTS_CALL tvtts_set_sample_rate(tvtts_synth *s, int which);
TVTTS_API int TVTTS_CALL tvtts_get_sample_rate(const tvtts_synth *s);
TVTTS_API uint32_t TVTTS_CALL tvtts_sample_rate_hz(int which);
TVTTS_API void TVTTS_CALL tvtts_destroy(tvtts_synth *s);

/*
 * Speak.  Blocks until the utterance finishes or the callback aborts it.
 * Returns 0 when it finished, 1 when the callback stopped it, negative on
 * error.  The synth is left ready for the next call either way.
 *
 * The engine reads cp1252, which is the encoding SAPI's WideCharToMultiByte
 * handed it; the utf8 and utf16 entry points convert the same way, so a
 * character the engine cannot represent becomes '?' exactly as it did then.
 * tvtts_speak_bytes takes cp1252 directly, for callers that already have it.
 */
TVTTS_API int TVTTS_CALL tvtts_speak_utf8(tvtts_synth *s, const char *text,
                                          tvtts_callback cb, void *user);
TVTTS_API int TVTTS_CALL tvtts_speak_utf16(tvtts_synth *s, const uint16_t *text,
                                           tvtts_callback cb, void *user);
TVTTS_API int TVTTS_CALL tvtts_speak_bytes(tvtts_synth *s, const void *text,
                                           uint32_t len, tvtts_callback cb,
                                           void *user);

/*
 * Phonemes, in the engine's own alphabet: one character per phoneme, with
 * "1" and "2" marking primary and secondary stress, so "hello" is "HeLO1".
 * The alphabet is the engine's internal one, not ARPABET and not the
 * bracket notation ("[HH AH L OW]") the rule interpreter also understands.
 *
 * This is the 5.1 builds' tts_SpeakPhoneme, which is a wrapper that puts
 * ESC[1I in front of the string and ESC[0I after it and speaks the result;
 * the escape is in this engine too.  Round-tripping is close but not exact:
 * speaking the phonemes tvtts_text_to_phonemes gives for "hello" is byte
 * for byte the same as speaking the word, while for some other words the
 * final intonation differs slightly.  See docs/VOICES.md.
 */
TVTTS_API int TVTTS_CALL tvtts_speak_phonemes(tvtts_synth *s,
                                              const char *phonemes,
                                              tvtts_callback cb, void *user);

/*
 * The other direction: what the engine would say for this text, in that
 * same alphabet.  Written NUL-terminated into buf, snprintf-style -- the
 * return is the number of bytes needed including the terminator, so a
 * short buffer is truncated but the return still says how much was wanted,
 * and buf may be NULL (with cap 0) purely to ask the size.  Returns -1 if
 * the arguments are bad or it ran out of memory.
 *
 * The engine only produces the trace as a side effect of synthesising, so
 * this runs the text through and throws the audio away: it costs what
 * speaking would, and it advances the synth exactly as a tvtts_speak call
 * does.  Settings are left alone.
 */
TVTTS_API int TVTTS_CALL tvtts_text_to_phonemes(tvtts_synth *s,
                                                const char *text,
                                                char *buf, uint32_t cap);

/*
 * Index marks.  The engine takes them inline in the text, so a caller with
 * a sequence of (text, bookmark) pieces writes the escape between them.
 * Writes at most 16 bytes including the terminator and returns the length,
 * or 0 if the buffer is too small.
 */
TVTTS_API int TVTTS_CALL tvtts_mark_sequence(char *buf, size_t cap, uint32_t mark);

/*
 * A pause, for the same reason: the engine takes one inline, in hundredths
 * of a second, biased by 49.  Rounded down to a hundredth and clamped to
 * the 2060 ms the engine can express.  Writes at most 16 bytes including
 * the terminator and returns the length, or 0 if the buffer is too small.
 */
TVTTS_API int TVTTS_CALL tvtts_break_sequence(char *buf, size_t cap, uint32_t ms);

/*
 * Speak punctuation rather than pause on it: with this on, the comma and
 * period of "Hi, there." are said aloud.  It is a flag, so it stays on
 * until turned off -- including into later utterances on the same synth.
 *
 * It does not spell anything.  There is no need to: the engine already
 * names a letter given on its own, so "A" alone says "ay" rather than the
 * article.  What loses that is putting any escape after the letter, which
 * makes it no longer the last word -- so a caller wanting a letter named
 * should end the text with the letter.
 */
TVTTS_API int TVTTS_CALL tvtts_punctuation_sequence(char *buf, size_t cap, int on);

/*
 * Pitch and rate part way through an utterance, for the prosody a caller
 * puts on a capital letter or an emphasised word.  Both take the same units
 * as tvtts_set_pitch and tvtts_set_rate and clamp to what the engine can
 * usefully do: pitch 50..400, rate 46..253 words per minute (see
 * TVTTS_RATE_MIN below for where those two numbers come from).
 */
TVTTS_API int TVTTS_CALL tvtts_pitch_sequence(char *buf, size_t cap, int pitch);
TVTTS_API int TVTTS_CALL tvtts_rate_sequence(char *buf, size_t cap, int wpm);

/*
 * The engine's rate is a 26-row table, picked by (wpm - 46) / 8, so it
 * takes 46..253 words per minute in steps of eight -- and only those.
 *
 * Below 46 the subtraction is done unsigned and wraps, giving an index
 * of about 0x1fffffff and a wild read: the original crashes there and so
 * does this, so tvtts_set_rate raises anything lower to 46.  Above 253
 * the index runs past the end of the table and the engine reads whatever
 * follows it, which sounds nothing like speeding up -- a sentence at 254
 * comes out ten times longer than at 253.  That is the original's own
 * behaviour, reproduced exactly, so it is left alone rather than clamped;
 * callers that want speech rather than fidelity should stay inside the
 * range.  46..76 all select the slowest row.
 */
#define TVTTS_RATE_MIN 46
#define TVTTS_RATE_MAX 253

/*
 * With TVTTS_EXT_RATE on -- which is the default -- the ceiling moves to
 * TVTTS_RATE_MAX_EXT.  The original's 26 rows are untouched, so 46..253
 * sounds exactly as it always did; above that OpenTV shortens durations
 * instead, which the original never did because it had no rows there at
 * all.  It tops out around three and a half times the speed of 150 wpm,
 * where the per-phoneme minimum durations in g_phone_dur stop it going any
 * further.
 */
#define TVTTS_RATE_MAX_EXT 400

/*
 * OpenTV extensions: behaviour this project adds that the 1997 engine did
 * not have.  All are on by default.  Clearing them gives the original
 * exactly, which is what the test corpus runs so that its byte-for-byte
 * comparison keeps proving the decompilation correct.
 *
 * Process-wide rather than per-synth, like tvtts_add_lexicon: the engine's
 * own Stage 2 keeps its working state in globals, so one synth was never
 * independent of another here.
 */
#define TVTTS_EXT_RATE  0x1u   /* rate above 253 wpm actually speeds up */

/*
 * TVTTS_EXT_CLARITY widens the formant bandwidths as the rate climbs, which
 * is what keeps fast speech from slurring: a narrow resonator rings for
 * longer than a shortened phoneme lasts, so its energy smears into the next
 * one.  A wider bandwidth settles sooner and each phoneme keeps its own
 * identity.
 *
 * It does nothing at or below 253 wpm, so the voices are unchanged
 * everywhere the original could reach; it only shapes the range OpenTV
 * added.  The approach and its constants come from Tamas Geczy's
 * TGSpeechBox (MIT) -- see NOTICE.
 */
#define TVTTS_EXT_CLARITY 0x2u

#define TVTTS_EXT_ALL   0x3u

TVTTS_API void TVTTS_CALL tvtts_set_extensions(uint32_t mask);
TVTTS_API uint32_t TVTTS_CALL tvtts_get_extensions(void);

/*
 * Pitch, in the engine's own units.  Stage 2 clamps every node it emits
 * to 50..500 and then stores the value halved in a byte, so 50..500 is
 * exactly what the engine can represent -- 500 is the largest pitch whose
 * half still fits.
 *
 * Outside it the base pitch is not ignored, because it is arithmetic on
 * the way in rather than the clamped value: a base below 50 still lifts
 * the accented nodes, and the audio keeps changing down to about 28 and
 * up to about 516 before it saturates.  None of that is a pitch the
 * engine can hold, though, so a caller wanting the range should use
 * these.  tvtts_set_pitch does not enforce them: the corpus checks the
 * original's behaviour at 40, which is outside.
 *
 * The inline escape is narrower still.  ESC[<n>p takes n 25..200 and
 * doubles it (preformat.c), so tvtts_pitch_sequence reaches 400 and no
 * further, whatever tvtts_set_pitch has been given.
 */
#define TVTTS_PITCH_MIN 50
#define TVTTS_PITCH_MAX 500

/* Settings.  These persist across utterances, as SAPI's did. */
TVTTS_API void TVTTS_CALL tvtts_set_voice(tvtts_synth *s, int voice);
TVTTS_API void TVTTS_CALL tvtts_set_rate(tvtts_synth *s, int wpm);
TVTTS_API void TVTTS_CALL tvtts_set_pitch(tvtts_synth *s, int pitch);
TVTTS_API void TVTTS_CALL tvtts_set_volume(tvtts_synth *s, uint32_t volume);

TVTTS_API int TVTTS_CALL tvtts_get_voice(const tvtts_synth *s);
TVTTS_API int TVTTS_CALL tvtts_get_rate(const tvtts_synth *s);
TVTTS_API int TVTTS_CALL tvtts_get_pitch(const tvtts_synth *s);

/* The voices, in the order the engine indexes them. */
TVTTS_API int TVTTS_CALL tvtts_voice_count(void);
TVTTS_API const char *TVTTS_CALL tvtts_voice_name(int voice);

/* The engine's default rate and pitch for a voice, as SAPI reported them. */
TVTTS_API int TVTTS_CALL tvtts_voice_rate(int voice);
TVTTS_API int TVTTS_CALL tvtts_voice_pitch(int voice);

/*
 * Add a word to the user lexicon, as ITTSDialogs' lexicon editor did.
 * Process-global: it affects every synth in the process, including ones
 * created later.  Returns 0 on success.
 */
TVTTS_API int TVTTS_CALL tvtts_add_lexicon(const char *word, const char *phonemes);

/*
 * Reproduction knobs, for callers that need to match the original exactly.
 *
 * preformat and textin are the two front-end passes the original let you
 * turn off through the registry.  terminators is how many NUL bytes SAPI
 * put after the text; the feed routine branches on the total length, so it
 * changes the output.  The defaults (1, 1, 2) are what SAPI produced.
 */
TVTTS_API void TVTTS_CALL tvtts_set_compat(tvtts_synth *s, int preformat,
                                           int textin, int terminators);

/*
 * ClassicVoices patch: Stage 2's process-wide working state (src/engine/stage2.c), saved and restored by the host so
 * each voice keeps its own. Not thread safe: call under the same lock as every tvtts_speak_*.
 */
TVTTS_API size_t TVTTS_CALL tvtts_cv_s2_size(void);
TVTTS_API void TVTTS_CALL tvtts_cv_s2_save(void *out);
TVTTS_API void TVTTS_CALL tvtts_cv_s2_load(const void *in);

#ifdef __cplusplus
}
#endif

#endif
