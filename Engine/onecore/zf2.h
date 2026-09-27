/* zf2.h - second half of the portable OneCore en-US text frontend (Microsoft David / OneCore Zira / Mark).
 *
 * Portable C99.  Input: one zf_sentence (zf.h) as it is at the end of CTextProcessor::Process (the zf1 stage
 * boundary = engine handler 0x40000 "after" = 0x80000 "before").  Output: the backend input (zb.h ZbUtt: the
 * phones x features matrix of CTTSFeatureExtractionEngine plus the per-phone SAPI/pause controls) and the SAPI
 * events of CTTSUnitGenerator.
 *
 * Stages (engine functions in MSTTSEngine_OneCore.dll 10.3.21207, see notes/fe2.md):
 *   1. CLinguisticProsodyTagger::Process 0x180060010 - rule-based breaks / emphasis / boundary tones / pitch
 *      accents (no CRF/RNN prosody model is shipped for David, Zira or Mark) + ENU locale post steps.
 *   2. CTTSUnitGenerator::Process 0x180034b00 - sentence rebuild (FUN_18001a644: syllable/phone tree from the
 *      pronunciations, pause-class normalisation, pause-word insertion with [SilenceLength]), phrase /
 *      prosodic-word / intonation-phrase structure (FUN_180045c80), SAPI rate/pitch/volume/emph per phone, events.
 *   3. CTTSFeatureExtractionEngine 0x18005bfc0 - the APM feature paths evaluated on every phone.
 */
#ifndef ZF2_H
#define ZF2_H

#include "zf.h"
#include "zb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- per-voice settings (voice INI + engine defaults) ---- */
typedef struct zf2_voice zf2_voice;

/* voicepath: e.g. "C:/Windows/Speech_OneCore/Engines/TTS/en-US/M1033David" (".INI" is appended).
 * Returns NULL on failure. */
zf2_voice *zf2_voice_load(const char *voicepath);
void zf2_voice_free(zf2_voice *v);
/* Attach statistical prosody model runners (Eva); fn(ctx, zf2_sent*, stage 1 breaks / 2 tones / 3 accents). */
void zf2_voice_set_prosody_hook(zf2_voice *v, void (*fn)(void *ctx, void *sent, int stage), void *ctx);
/* ClassicVoices patch: take the phone set (ids, names, flags) from the language data's resource 29a5584b/153f1b64
 * instead of the built-in en-US one (identical to MSTTSLocEnUS.dat's).  Returns 0, or -1 (unchanged) if the
 * resource is missing or malformed. */
int zf2_voice_set_phoneset(zf2_voice *v, const uint8_t *res, size_t size);

/* ---- SAPI events produced for one sentence (audio offsets are filled by the backend from durations) ---- */
enum { ZF2_EV_WORD = 5, ZF2_EV_PHONEME = 6, ZF2_EV_SENTENCE = 7, ZF2_EV_VISEME = 8, ZF2_EV_BOOKMARK = 4 };
typedef struct {
    int id;             /* SPEI_* */
    int phone;          /* index of the phone (row of ZbUtt) the event is attached to (its start), -1 = sentence start */
    long long wparam;   /* WORD: length; SENTENCE: length; PHONEME/VISEME: next phone/viseme (duration bits added by the
                           backend: wParam = duration_ms << 16 | next) */
    long long lparam;   /* WORD/SENTENCE: source offset; PHONEME: SAPI phone id; VISEME: viseme */
    const zf_char *str; /* BOOKMARK: bookmark name (points into the input sentence), else NULL.  The engine attaches a
                           bookmark to the last phone of the preceding word (after its phoneme/viseme events), or to
                           the first phone when there is none; every other event to the start of its phone. */
    int sub, nsub;      /* VISEME: index / count of the visemes of this phone (a diphthong has 2; the engine splits the
                           phone's samples evenly between them); 0 / 0 for every other event */
} zf2_event;

typedef struct {
    int n, cap;
    zf2_event *ev;
} zf2_events;

/* ---- the processed sentence (internal tree, kept for tests / debugging) ---- */
typedef struct zf2_sent zf2_sent;

/* Run stages 1+2 on one sentence.  Returns NULL on failure.  The input is not modified. */
zf2_sent *zf2_run(const zf2_voice *v, const zf_sentence *in);
/* Stage 3: fill the feature matrix + phone controls (u->ph is malloc'ed, free with zf2_utt_free). */
int zf2_features(const zf2_sent *s, ZbUtt *u);
/* Events for the sentence (appends to ev). */
int zf2_make_events(const zf2_sent *s, zf2_events *ev);
void zf2_sent_free(zf2_sent *s);
void zf2_utt_free(ZbUtt *u);
void zf2_events_free(zf2_events *ev);

/* One-call convenience: zf2_run + zf2_features (+ events if ev != NULL). */
int zf2_process(const zf2_voice *v, const zf_sentence *in, ZbUtt *out, zf2_events *ev);

/* ---- per-word results of the prosody rules (stage 1+2): what decides phrasing, pauses and emphasis ---- */
typedef struct {
    int inserted;       /* 1 = pause word zf2 created, not one of the input sentence's words */
    int type;           /* ZF_WT_* of the word */
    int bi;             /* BreakIndex after the word: 2 word, 3 intermediate phrase, 4 intonation phrase, 5 sentence */
    int tone;           /* ToBI final boundary tone (0 = none) */
    int pause_class;    /* index into the voice's [SilenceLength] table */
    int pause_ms;       /* that entry's length in ms (0 when no voice is given) */
    int emphasis;       /* accent / emphasis flag */
} zf2_wordinfo;
/* Fills out[] in word order, including the pause words zf2 inserts.  Returns the count written, or -1. */
int zf2_word_prosody(const zf2_sent *s, const zf2_voice *v, zf2_wordinfo *out, int max);

/* ---- generic feature-path evaluation (any "Phone.Syllable.Word.NextWord.POS" style path of the engine's
 *      CTTSFeatureMeta grammar that uses the supported locations/properties) ---- */
typedef struct {
    int kind;           /* 0 int, 1 enum, -1 null */
    int value;
} zf2_value;
/* Evaluate path for every phone: vals[nphone].  Returns 0 ok, -1 unsupported path. */
int zf2_eval_path(const zf2_sent *s, const char *path, zf2_value *vals);
int zf2_nphones(const zf2_sent *s);

/* ---- debug dump of the internal tree in the harness/zftap2 text format ("W"/"Y"/"H"/"PH"/"IP" lines) ---- */
void zf2_dump(const zf2_sent *s, void *FILE_ptr, const char *stage);

#ifdef __cplusplus
}
#endif
#endif
