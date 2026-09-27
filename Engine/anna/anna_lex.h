/* Anna text frontend, middle stage: pronunciation lookup (vendor lexicon M1033DSK.TTS, morphology,
 * letter-to-sound M1033DSK.LTS), POS tagging, homographs, word list, phrasing and accents.
 * Internal header of anna_lex.c / anna_lts.c / anna_morph.c / anna_pos.c / anna_prosody.c. */
#ifndef ANNA_LEX_H
#define ANNA_LEX_H

#include <stddef.h>
#include <stdint.h>

#include "anna_front.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ClassicVoices patch: the engine's scratch buffers were function-level statics, shared by every anna_tts, so two
 * voices speaking on two threads at once corrupted each other's words. They are per thread now (same contents,
 * same results; a thread that never speaks Anna never allocates them). */
#ifndef ANNA_TLS
#define ANNA_TLS __thread
#endif

#define ANNA_PRON_MAX 0x180 /* phone ids per pronunciation (work record +0x208: u16[0x180]) */
#define ANNA_WORD_MAX 0x80  /* chars per word (work record text u16[0x80]) */

/* SAPI en-US phone ids: 1 '-' 2 '!' 3 '&' 4 ',' 5 '.' 6 '?' 7 '_' 8 '1' 9 '2', 10 aa ... 49 zh */
typedef struct {
    uint16_t ph[ANNA_PRON_MAX];
    int n;
    uint32_t pos; /* SPPARTOFSPEECH, 0xFFFFFFFF when the lexicon gives none */
} anna_pron;

typedef struct anna_lexicon anna_lexicon;
typedef struct anna_lts anna_lts;

/* ---- vendor lexicon (SAPI compressed lexicon format, see notes/frontend.md section 2) ---- */
anna_lexicon *anna_lexicon_load(const char *path);
anna_lexicon *anna_lexicon_load_mem(const void *data, size_t size); /* the data is copied */
void anna_lexicon_free(anna_lexicon *x);
/* word: UTF-16, 0-terminated. Returns the number of (pronunciation, POS) entries written, 0 = not found. */
int anna_lexicon_lookup(const anna_lexicon *x, const uint16_t *word, anna_pron *out, int max);

/* ---- letter to sound (MSTTSCommon ILTS, M1033DSK.LTS) ---- */
anna_lts *anna_lts_load(const char *path);
anna_lts *anna_lts_load_mem(const void *data, size_t size);
void anna_lts_free(anna_lts *m);
/* ILTS::GetPronunciation: best candidate as a phone string ("h eh l  ow 1"); returns its length,
 * 0 = empty result */
int anna_lts_pronounce(const anna_lts *m, const uint16_t *word, char *out, size_t outsize);
/* all candidates (n-best, after the 10-best cut and the 0.005 threshold), best first; returns the count */
int anna_lts_nbest(const anna_lts *m, const uint16_t *word, double *score, char (*prons)[1024], int max);

/* SAPI phone string ("ey 1 - p r ih l", phones separated by blanks) -> phone ids; returns the count,
 * -1 when a phone name is unknown (SpPhoneConverter fails) */
int anna_phones_from_string(const char *s, uint16_t *out, int max);

/* ---- pronunciation work record (the engine's 0x848-byte record of the pronunciation stage 0x0D5E3DA7) ---- */
typedef struct {
    uint16_t text[ANNA_WORD_MAX];     /* +0x000 word (at most 0x7f chars) */
    uint16_t stem[ANNA_WORD_MAX];     /* +0x100 stem found by the morphology (upper case), or empty */
    uint32_t lextype;                 /* +0x200 0x1000 lexicon, 0x2000 LTS, |0x4000 morphology, 0 special table */
    int n1;                           /* +0x204 */
    uint16_t pron1[ANNA_PRON_MAX];    /* +0x208 SAPI phone ids, 0-terminated */
    int npos1;                        /* +0x508 */
    uint32_t pos1[4];                 /* +0x50C POS that use pron1 */
    int n2;                           /* +0x51C */
    uint16_t pron2[ANNA_PRON_MAX];    /* +0x520 first different pronunciation */
    int npos2;                        /* +0x820 */
    uint32_t pos2[4];                 /* +0x824 POS of the other pronunciations */
    uint32_t pos;                     /* +0x834 chosen POS */
    uint32_t reqpos;                  /* +0x838 POS requested by SAPI markup (0 none) */
    int hasalt;                       /* +0x83C pron2 may be chosen */
    int usealt;                       /* +0x840 pron2 chosen */
    int fixed;                        /* +0x844 set by the special-word tables */
} anna_entry;

/* lookup chain 0x0D5E2D08: vendor lexicon, morphology 0x0D5EDDA2, letter to sound 0x0D5F41FA.
 * Reads e->text and e->reqpos, fills the rest. Returns 0, or -1 when the word cannot be pronounced
 * (SPERR_NOT_IN_LEX: the engine then aborts the sentence). */
int anna_lookup_word(const anna_lexicon *lex, const anna_lts *lts, anna_entry *e);

/* 0x0D5F41FA: letter-to-sound for one word (spelled with letter names when 0x0D5F3C9D says so);
 * returns the number of phone ids, -1 on failure */
int anna_lts_word(const anna_lts *lts, const uint16_t *word, uint16_t *ph, int max);

/* ---- one sentence as the pronunciation stage sees it (anna_pos.c) ---- */

/* item of the sentence enumerator (0x1C bytes: text, len, ofs, words, nw, POS, type record) */
typedef struct {
    int type;              /* *type record */
    int pos;               /* +0x14 item POS (punctuation 0x400c..; post rules set it for read/live) */
    const uint16_t *text;  /* item text (NUL-terminated copy, at most 127 units) */
    int len;               /* full item text length */
    int ofs;               /* source offset */
    int first, nw;         /* its word records */
    const int *ti;         /* type record (anna_norm_types.h) */
    int abbrev;            /* abbreviation table index (types 0x1003/0x1004), -1 otherwise */
} anna_mnode;

/* word record (0x20 bytes) after the pronunciation stage */
typedef struct {
    const anna_token *tok;
    int action;            /* SPVSTATE eAction */
    int has_text;          /* +4 != NULL */
    int entry;             /* its work record, -1 if it was not looked up */
    int pron_set;          /* +0x14 != NULL */
    int npron;             /* pronunciation (+0x14): SAPI phone ids */
    uint16_t pron[ANNA_PRON_MAX];
    uint32_t pos;          /* +0x18 */
    uint32_t lextype;      /* +0x1c */
} anna_mword;

typedef struct {
    anna_mnode *nodes;
    int nn;
    anna_mword *words;
    int nw;
    anna_entry *entries;   /* work records in word order (words with text and action 0/2/4) */
    int ne;
    anna_entry *tagged;    /* copy of the work records before tagging (debug) */
} anna_msent;

/* build the item/word structure from the normalizer's tokens (allocates; free with anna_msent_free) */
int anna_msent_build(anna_msent *s, const anna_token *t, int n);
void anna_msent_free(anna_msent *s);
/* pronunciation stage 0x0D5E3DA7: special tables, lookup chain, tagger 0x0D5E2892, post-lexical rules.
 * Returns 0, or -1 when a word cannot be pronounced (the engine stops speaking). */
int anna_pronounce(const anna_lexicon *lex, const anna_lts *lts, anna_msent *s);

/* ---- the middle stage as one object: tokens of a sentence -> word list (anna_prosody.c) ---- */
typedef struct anna_mid anna_mid;

/* voice_base = voice path + name without extension (".../enu-dsk/M1033DSK"): loads .TTS and .LTS */
anna_mid *anna_mid_new(const char *voice_base, char *err, size_t errlen);
void anna_mid_free(anna_mid *m);
/* PrepareSpeech (0x0D5E4A60), once per Speak(): quote/parenthesis state, pitch offset and range. The rate
 * factor (frontend +0x44) and the rand() state are NOT reset: they live as long as the voice (thread). */
void anna_mid_reset(anna_mid *m);
/* msvcrt rand() state of the engine thread (x = x*214013+2531011; value (x>>16)&0x7fff), 1 at thread start */
void anna_mid_set_rand(anna_mid *m, uint32_t state);
uint32_t anna_mid_get_rand(const anna_mid *m);
/* frontend +0x44: never initialized by the engine (0 in fresh memory, heap garbage otherwise) until the first
 * parenthesis of the voice's lifetime sets it to 1.25 / 1.0. Default 0. */
void anna_mid_set_rate_factor(anna_mid *m, float f44);
/* frontend +0x38: read as a float by 0x0D5E4552 but it holds a pointer (e.g. 0x7724D0E0 = 3.34e33 as a float).
 * Only the dead word fields pause_f/rate_f (+0x678/+0x67C) depend on it and on +0x44. Default 3.342864e33. */
void anna_mid_set_f38(anna_mid *m, float f38);
/* one sentence of tokens (anna_norm_sentence output) -> word records (engine list frontend+0x54 after
 * accent placement 0x0D5E6112). Returns the number of words (out holds at most max), or -1 on failure
 * (a word that cannot be pronounced: the engine stops speaking). */
int anna_mid_sentence(anna_mid *m, const anna_token *t, int n, anna_word *out, int max);
/* items, word records and work records of the last sentence (tests) */
const anna_msent *anna_mid_last(const anna_mid *m);

/* ---- helpers on UTF-16 strings ---- */
size_t anna_wlen(const uint16_t *s);
int anna_wicmp(const uint16_t *a, const uint16_t *b);        /* _wcsicmp (ASCII + Latin-1 folding) */
int anna_wicmp_a(const uint16_t *a, const char *b);          /* same against an ASCII string */
uint16_t anna_wlower(uint16_t c);                             /* CharLowerW / towlower (Latin-1) */
uint16_t anna_wupper(uint16_t c);                             /* towupper (Latin-1) */
int anna_wisupper(uint16_t c);                                /* iswupper */
int anna_wislower(uint16_t c);                                /* iswlower */
int anna_utf8_to_w(const char *s, uint16_t *out, int max);    /* returns length */
void anna_w_to_utf8(const uint16_t *s, char *out, size_t max);

#ifdef __cplusplus
}
#endif
#endif
