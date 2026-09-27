/* zf.h - shared data structures of the portable OneCore en-US (Microsoft David / OneCore Zira) text frontend.
 *
 * Portable C99.  Owned jointly by the zf1_* (text -> words with pronunciations) and zf2_* (prosody, syllable /
 * phone tree, feature extraction) modules: KEEP THIS HEADER STABLE; add fields at the end of structs only.
 *
 * The structures mirror the engine's CTTSUtterance tree (MSTTSEngine_OneCore.dll 10.3.21207.0, see
 * notes/frontend.md section 3 and notes/fe1.md).  Every field of zf_word is named after the CTTSWord member
 * offset it mirrors (e.g. i180 = *(int*)(word+0x180)); fields with a known meaning also have a readable name.
 *
 * zf1 stage boundary = the tree as it is at the end of CTextProcessor::Process (handler 0x40000, engine+0x578
 * observer callback phase 1), i.e. BEFORE CLinguisticProsodyTagger (0x80000).  harness/zftap1.exe dumps exactly
 * these fields ("W" lines).
 *
 * Strings are UTF-16 code units (zf_char), 0-terminated, heap-owned by the word (free with zf_sentence_free).
 */
#ifndef ZF_H
#define ZF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t zf_char;               /* UTF-16 code unit (the engine works on Windows wchar_t) */

/* ---- word types (CTTSWord +0x88, getter vt+0x68) ---- */
enum {
    ZF_WT_WORD = 0,                     /* ordinary word (lexicon / LTS / TN output) */
    ZF_WT_PUNCT = 1,                    /* punctuation token (no pronunciation) */
    ZF_WT_SILENCE = 3,                  /* silence / break token, pron "-SIL-" (sentence-initial, <silence>, TN
                                           entity-internal pauses); silence_ms = +0x210 */
    ZF_WT_SPELL = 4                     /* letter produced by <spell> (SPVA_SpellOut) */
};

/* ---- the SAPI fragment state a word came from (engine fragment node, 0x88 bytes, word+0x90 points at it) ---- */
typedef struct zf_fragstate {
    int action;                         /* node+0x00: 0 speak, 1 silence, 2 pronounce, 3 bookmark, 4 spell-out */
    uint16_t lang;                      /* node+0x04: LangID (0x409) */
    int emph;                           /* node+0x08: 1 = <emph> (SAPI EmphAdj != 0) */
    int silence_ms;                     /* node+0x0c: <silence msec> */
    int has_prosody;                    /* node+0x28 != NULL (MiddleAdj != 0 || RateAdj != 0 || Volume != 100) */
    float pitch_middle;                 /* prosody+0x08: SAPI PitchAdj.MiddleAdj */
    float rate;                         /* prosody+0x28: SAPI RateAdj */
    float volume;                       /* prosody+0x3c: SAPI Volume (0..100) */
    uint16_t pos;                       /* node+0x38: SAPI part of speech converted (0xFFFF = none) */
    const zf_char *sayas;               /* node+0x20 -> InterpretAs (SAPI <context id>), NULL if none */
    int src_off, src_len;               /* node+0x70 / +0x74 (after whitespace trimming) */
} zf_fragstate;

/* ---- one CTTSWord (0x370 bytes in the engine) ---- */
typedef struct zf_word {
    int type;                           /* +0x88  ZF_WT_* */
    uint16_t lang;                      /* +0x80  LangID (1033) */
    int i84;                            /* +0x84  (0 in every observed case) */

    zf_char *text;                      /* +0x98  Text: token text as spoken (TN output word, or source token) */
    zf_char *regular;                   /* +0xb8  RegularText (lower-cased Text) */
    zf_char *ci;                        /* +0xd8  CaseInsensitiveText (upper-cased Text) */
    zf_char *prev_chars;                /* +0xf8  PreviousCharacters (homograph CRF context; mostly empty) */
    zf_char *next_chars;                /* +0x118 NextCharacters */

    uint16_t pos;                       /* +0x138 POS (after conversion; 0xFFFF = none), see zf1 notes table */
    uint16_t tpos;                      /* +0x13a POSTaggerPOS (raw tagger output) */

    zf_char *pron;                      /* +0x160 Pronunciation, phone names separated by ' ': "H EH - L OW 1"
                                           ('-' syllable boundary, '1'/'2' stress after the vowel); NULL/empty for
                                           punctuation */
    int nprons;                         /* +0x360 CTTSPronunciationList: all candidate pronunciations (sorted by the */
    zf_char **prons;                    /*        engine's u16 compare), each a 0-terminated phone-id string */
    int cur_pron;                       /*        index of the selected one (list+0x38), -1 if none */

    int i180;                           /* +0x180 (8 default; 1/3/4 near punctuation: boundary class, see notes) */
    zf_char *ne_type;                   /* +0x188 NamedEntityType: TN category of the entity the word came from
                                           ("sp:date", "sp:webUrl", ...), NULL if none */
    zf_char *s1b0;                      /* +0x1b0 domain of the entity ("none", "date", "time", ...) */
    zf_char *s1d0;                      /* +0x1d0 AcousticDomain ("none") */
    zf_char *s1f0;                      /* +0x1f0 */
    zf_char *s2f8;                      /* +0x2f8 */

    int silence_ms;                     /* +0x210 silence length for ZF_WT_SILENCE tokens (ms) */
    int i214;                           /* +0x214 (100 on the last word of some TN entities) */
    int i228;                           /* +0x228 punctuation class set by rules */
    int i22c, i230, i234;
    int tone;                           /* +0x238 ToBIFinalBoundaryTone (0 at zf1 boundary) */
    int tone_lock;                      /* +0x23c */
    int i240;
    int bi;                             /* +0x244 BreakIndex (2 default, 5 on the sentence-initial SIL token) */
    int i248;                           /* +0x248 (1 = inside a TN entity, not its last word) */
    int bi_lock;                        /* +0x24c */
    int pause_class;                    /* +0x250 index into SilenceLength table (7 on sentence-initial SIL) */
    int emphasis;                       /* +0x254 accent/emphasis flag */
    int emph_lock;                      /* +0x258 */
    int src_off;                        /* +0x270 source character offset (SAPI WORD_BOUNDARY lParam) */
    int src_len;                        /* +0x274 source length */
    int i278;                           /* +0x278 pronunciation source/kind (1 lexicon, 13 multi-pron, 8 spelled, 0 punct..) */
    int i27c;                           /* +0x27c 1 = produced by text normalization */
    int i2b8;                           /* +0x2b8 1 = last word of the sentence (before final punctuation) */
    int i2bc;                           /* +0x2bc 1 = first word of the sentence */
    int i2f0;                           /* +0x2f0 */
    int i368;                           /* +0x368 */
    int i36c;                           /* +0x36c */

    zf_fragstate fs;                    /* copy of *(word+0x90) */

    void *priv;                         /* zf1-internal scratch (lexicon entry etc.); NULL in the output */

    /* ---- added by zf2 (read by the prosody / rebuild code, notes/fe2.md); zf1 please fill ---- */
    int group;                          /* +0x2e8 grouping object (compound-word parts / TN multi-word unit): 0 = none,
                                           else any id equal for words sharing the same object (FUN_18009a808 sets
                                           BI 1 between consecutive words of one group) */
    int attached_pause;                 /* +0x2b0 index (in words[]) of the pause word inserted after this word by an
                                           earlier sentence rebuild, -1 = none (at the zf1 boundary: always -1 unless
                                           a TN silence +0x214 created one) */
    int ne_obj;                         /* +0x1a8 named-entity object: 0 none, 1 = word is the entity's first word,
                                           2 = inside an entity but not its first word (silence scaled by
                                           SilenceLength.RatioNameEntities, FUN_1800199a8) */
} zf_word;

/* ---- one CTTSSentence ---- */
typedef struct zf_sentence {
    int src_off;                        /* +0xa0 source offset of the sentence */
    int src_len;                        /* +0xa4 source length */
    int type;                           /* +0xa8 SentenceType (0 at the zf1 boundary in all observed cases) */
    int emotion;                        /* +0xac SentenceEmotion (1) */
    int nwords;
    zf_word *words;                     /* the +0x110 list in order */

    /* ---- added by zf2: structures found by CTextProcessor that the prosody rules use.  Heap-owned by the
     *      sentence; zf_sentence_free must free quotes, par[i].el and par. ---- */
    int nquotes;                        /* sentence+0x1e8 (vt+0x50): quote/bracket pairs (FUN_1800b8608) */
    struct zf_quote { int open, close; } *quotes;          /* word indices of the opening / closing token */
    int npar;                           /* sentence+0x248 (vt+0x60): CParallelStruct list (FUN_1800b90a8) */
    struct zf_parstruct {
        int nel;
        struct zf_parel { int a, b, type; } *el;           /* element {first word, last word, type} (indices) */
    } *par;

    /* ---- added by zf1 (fork E): sentence+0x278 segments (sentence vt+0xb0 = FUN_1800b5aa0): runs of words
     *      sharing a NamedEntityType or a say-as fragment; zf_word.ne_obj = 1 on a segment's first word, 2 on the
     *      others.  Heap-owned (zf_sentence_free frees segs; value points into a word / fragment string). ---- */
    int nsegs;
    struct zf_segment {
        int first, last;                /* word indices */
        const zf_char *value;           /* NamedEntityType of the run, or the say-as InterpretAs of the fragment */
    } *segs;
} zf_sentence;
typedef struct zf_segment zf_segment;

/* ---- zf1 API: text -> sentences ---- */
typedef struct zf1_engine zf1_engine;

/* dat_path: MSTTSLocEnUS.dat (NULL = C:\Windows\Speech_OneCore\Engines\TTS\en-US\MSTTSLocEnUS.dat).
 * Returns NULL on failure. */
zf1_engine *zf1_open(const char *dat_path);
/* ClassicVoices patch: the same for a locale's language data (1033 en-US = zf1_open; 2057 en-GB: MSTTSLocEnGB.dat
 * and the en-GB domain files next to it), and raw access to a resource of the language data (NULL if absent). */
zf1_engine *zf1_open_lcid(const char *dat_path, int lcid);
const uint8_t *zf1_resource(const zf1_engine *e, uint32_t id1, uint32_t id2, size_t *size);
void zf1_close(zf1_engine *e);

/* Start a new Speak call.  text: UTF-16, len code units (-1 = 0-terminated).
 * xml: 1 = SAPI XML, 0 = plain text, -1 = auto (SPF_DEFAULT: XML if the first non-blank char is '<'). */
int zf1_speak(zf1_engine *e, const zf_char *text, int len, int xml);
int zf1_speak_utf8(zf1_engine *e, const char *text, int xml);

/* Fetch the next sentence of the current Speak call.  Returns 1 and fills *out (caller frees with
 * zf_sentence_free), or 0 at the end of the text. */
int zf1_next_sentence(zf1_engine *e, zf_sentence *out);
void zf_sentence_free(zf_sentence *s);

#ifdef __cplusplus
}
#endif
#endif
