/* Anna text frontend (MSTTSFrontendENU.dll) - shared types between the stages.
 *
 *   text --(anna_norm.c: tokenizer, sentence breaker, normalizer)--> anna_token[]      (one sentence)
 *        --(anna_lex.c / anna_lts.c / anna_pos.c / anna_prosody.c: lookup, POS, phrasing, accents)--> anna_word[]
 *        --(anna_units.c: syllables, UDT segmentation, features)--> anna_unitspec[]   (anna.h, to the backend)
 *
 * Field comments give the matching offset in the engine's own records so the stages can be checked against
 * hooks. Stage owners may add fields; keep the existing ones' meaning.
 */
#ifndef ANNA_FRONT_H
#define ANNA_FRONT_H

#include "anna.h"

#define ANNA_MAX_PH 384 /* word+0x34 int[0x180] */

/* SAPI-state-like controls carried by text fragments (SPVSTATE subset) */
typedef struct {
    int volume;   /* 0..100 */
    int rate;     /* RateAdj */
    int pitch;    /* PitchAdj.Middle */
    int emph;     /* EmphAdj */
} anna_state;

/* normalizer output (anna_norm.c): one entry per engine word record (0x20 bytes) of one sentence, in order.
 *
 * The engine's sentence is a list of ITEMS (0x1C bytes: text, len, src offset, word array, word count, POS,
 * type record); each item owns 1..n WORD records (0x20 bytes: SPVSTATE*, text, len, ...). This is exactly the
 * list 0x0D5E3DA7 (pronunciation stage) receives. A token is one word record plus a copy of its item's fields:
 *   - a plain word "Hello" is one item (type 0x1002) with one word;
 *   - "$12.50" is one item (type 0x100d) with the words twelve dollars and fifty cents;
 *   - punctuation is an item (type 1..16) with one word that has no text (wlen == -1);
 *   - <silence>, <bookmark>, <pron> fragments are items of type 0x1000 with one word without text whose
 *     action is the fragment's SPVSTATE eAction (1 silence, 3 bookmark, 2 pronounce).
 * 0x0D5E3DA7 looks up every word with text whose action is 0 (speak), 4 (spell) or 2 (pronounce). */
typedef struct {
    char text[256];     /* word text, UTF-8 (the engine text is at most 127 UTF-16 units when looked up);
                           "" when the record has no text (wlen == -1) */
    int kind;           /* 0 word (item type >= 0x1001), 1 punctuation (item type 1..16), 2 silence, 3 bookmark,
                           4 pronounce, 5 other special fragment (SAPI eAction 5/6) */
    int tok_type;       /* item type = *type record: 1..16 punctuation (see punct), 0x1000 special fragment,
                           0x1001 spelled/mixed token, 0x1002 word, 0x1003/0x1004 abbreviation (0x1004: date
                           abbreviation jan..sun), 0x1005 initials, 0x1006 cardinal, 0x1007 ordinal, 0x1008 decimal,
                           0x1009 percent, 0x100a degrees, 0x100b squared, 0x100c cubed, 0x100d currency,
                           0x100e fraction, 0x100f number + fraction, 0x1013 ZIP (ADDRESS context), 0x1014 year,
                           0x1015 numeric date, 0x1016 date with words, 0x1017 decade, 0x1018 clock time,
                           0x1019 duration, 0x101a <spell> text, 0x101b hyphenated, 0x101c state + ZIP,
                           0x101d time range, 0x101e number range, 0x1027 phone number, 0x1028 currency range,
                           0x1029 "-abc", 0x1010/0x1011/0x1012 SAPI <context> NUMBER types */
    int punct;          /* item type 1..16 for kind 1: ( [ { ) ] } ' " . ! ? , ; : - "..." (0 otherwise) */
    int pos_hint;       /* ePartOfSpeech of the word's SPVSTATE (0x1000 for initials and the "a m" of a.m.,
                           or from SAPI <partofspeech>), or -1 when that is 0 */
    const char *pron;   /* always NULL: forced pronunciations come as SAPI PhoneIDs, see phone_ids */
    int silence_ms;     /* SPVSTATE SilenceMSecs of the word (kind 2) */
    int src_pos, src_len; /* item span in the input text (UTF-16 units, as SAPI's ulTextSrcOffset) */
    anna_state st;      /* SPVSTATE of the word: Volume, RateAdj, PitchAdj.MiddleAdj, EmphAdj */
    int flags;          /* ANNA_TOK_FIRST: first word of its item; ANNA_TOK_NUL_AFTER: the engine text is
                           followed by a NUL (static strings, e.g. every <spell> letter) */
    /* ---- added by anna_norm.c ---- */
    uint16_t wtext[128]; /* word text exactly as the engine has it (UTF-16, NUL-terminated, <= 127 units) */
    int wlen;           /* word text length (full length, may exceed 127), -1 = no text (word+4 == NULL) */
    int action;         /* SPVSTATE eAction of the word: 0 speak, 1 silence, 2 pronounce, 3 bookmark, 4 spell, ... */
    int word_pos;       /* word record +0x18 before lookup: punctuation POS 0x400c..0x4010, else 0 */
    int item;           /* index of the item in the sentence */
    int item_nwords;    /* number of words of the item */
    int item_pos;       /* item POS (item +0x14): 0x400c open, 0x400d close, 0x400e . ! ?, 0x400f , ; : - ...,
                           0x4010 quotes, 0 otherwise */
    uint16_t item_text[128]; /* item text (e.g. "Dr.", "3:45 pm", bookmark name), NUL-terminated, <= 127 units */
    int item_len;       /* full item text length; -1 when the item has no text (silence, empty <pron>) */
    int abbrev;         /* index into anna_norm_abbrev() for tok_type 0x1003/0x1004, else -1 */
    const uint16_t *phone_ids; /* SPVSTATE pPhoneIds (SAPI PhoneIDs, 0-terminated) or NULL */
    int pitch_range;    /* SPVSTATE PitchAdj.RangeAdj */
    const int *ti;      /* the item's type record (layouts in anna_norm_types.h); valid until the next
                           anna_norm_sentence() call. Number, currency, time and phone records are what
                           0x0D5E7E2D reads for digit-group prosody. */
} anna_token;

#define ANNA_TOK_FIRST 1

/* abbreviation table entry (@0x0D602A30): the tag stage uses prons/pos and taghandler (table 0x0D60C4AC) */
typedef struct {
    const char *word;     /* lower case, without the period */
    const char *pron[3];  /* SAPI phone strings or NULL */
    unsigned pos[3];      /* POS of each pronunciation */
    int shandler;         /* sentence-end handler (0x0D60C48C): -1 none, 0..3 */
    int taghandler;       /* 0x0D60C4AC handler index, -1 = use pron[0]/pos[0] */
} anna_abbrev;

/* ---- anna_norm.c: tokenizer, sentence breaker, normalizer (0x0D5F5285, 0x0D5F469F, 0x0D5EA96F ...) ---- */

typedef struct anna_norm anna_norm;

#define ANNA_NORM_XML 1           /* the text is SAPI XML (SPF_IS_XML); otherwise plain text (SPF_IS_NOT_XML) */
#define ANNA_NORM_SPEAK_PUNC 0x40 /* SPF_NLP_SPEAK_PUNC: punctuation is spoken ("comma") */

/* UTF-8 text (invalid bytes are read as Windows-1252) or UTF-16 text; the text is copied */
anna_norm *anna_norm_new(const char *utf8, int flags);
anna_norm *anna_norm_new_w(const uint16_t *text, int len, int flags);
/* optional: SAPI user lexicon test (ISpLexicon::GetPronunciations type USER); a whole whitespace token found
 * there is kept as one word without punctuation peeling */
void anna_norm_set_userlex(anna_norm *nm, int (*in_user_lex)(void *ctx, const uint16_t *w, int len), void *ctx);
/* next sentence: 1 = toks and ntok filled (valid until the next call), 0 = end of text, -1 = error */
int anna_norm_sentence(anna_norm *nm, const anna_token **toks, int *ntok);
void anna_norm_free(anna_norm *nm);
const anna_abbrev *anna_norm_abbrev(int index); /* 0..177, NULL otherwise */

/* word record after pronunciation, tagging, phrasing and accents (engine word record, 0x698 bytes) */
typedef struct {
    char text[20];      /* +0x000 (<= 19 chars) */
    int type;           /* +0x694: 0 word, 1 punct, 3 bookmark, 6 emphasis break, 7 phrase break, 8 silence */
    int nph;            /* +0x030 */
    unsigned char ph[ANNA_MAX_PH]; /* +0x034 internal phone codes: 0..15 vowels, 17 '_', 18..41 consonants,
                                      43 stress '1', 44 stress '2', 46 syllable '-' */
    int pos;            /* +0x634 SAPI POS (0x1000 noun ...) */
    int pos_class;      /* +0x638: 2 content, 1 function, 3 = 0x4001, 0 other */
    int punct;          /* +0x674 sentence/punct code: 5 '.', 2 '!', 3 '?', 1 ", ; : -" ; break codes 0xD..0x12 */
    int emph;           /* +0x658 */
    int acc_type;       /* +0x664 */
    int acc_var;        /* +0x668 */
    int rule1, rule2;   /* +0x68C, +0x690 */
    int silence_ms;     /* for type 8 */
    int src_pos, src_len; /* +0x63C, +0x640 */
    anna_state st;      /* +0x64C volume, +0x650 rate, +0x654 pitch */
    int flags;          /* +0x02C (0x8000 = spell: the unit stage speaks it as letter units, see anna_units.c) */
    /* fields read by the unit stage (anna_units.c) */
    int len;            /* +0x028 text length in characters (letter count of a spelled word) */
    int sent_pos, sent_len; /* +0x644, +0x648 sentence span; only the first word with text has sent_len != 0 */
    int bnd;            /* +0x670 word-final boundary strength: 0 = word (break 2), 1..9 = phrase (break 3, or 4
                           if rule2 == 0x11), >= 10 = sentence end (break 5). Breaks 4/5 add 400/750 ms pauses */
    int nbookmarks;     /* +0x660 != 0: SAPI bookmarks attached in front of this word (they produce no UnitSpec) */
    /* remaining engine fields, filled by anna_prosody.c so the record can be compared field by field */
    int tone;           /* +0x66C boundary tone set with bnd: 0x3EA (1002) falling, 0x3EB (1003) continuation,
                           0x3EC (1004) question; 0 on emphasized words and the word before them */
    float pause_f;      /* +0x678 0.05 / (rate factor * frontend +0x38 * +0x44) when that product < 0.3333 (+inf
                           until the first parenthesis of the session, 0 afterwards and on breaks) */
    float rate_f;       /* +0x67C 3^(RateAdj/10), or 0.3333 when pause_f is set; 0 on inserted breaks */
    float rate_mul;     /* +0x680 frontend +0x44: 1.25 inside parentheses, 1.0 after one, 0 before the first */
    float pitch_off;    /* +0x684 frontend +0x78: -0.2 in parentheses, 0.1 in quotes */
    float range;        /* +0x688 frontend +0x7C: 0.75 in parentheses, 1.25 in quotes, else 1.0 */
} anna_word;

/* anna_token.flags (continued): the engine text of this spelled letter (action 4) is followed by a NUL, so the
 * next letter's source offset advances (0x0D5E8071) */
#define ANNA_TOK_NUL_AFTER 2

/* ---- anna_units.c: unit segmentation and UnitSpec building (0x0D5EA45E, 0x0D5E96EC, GetNextUnitSpecList) ---- */

/* UnitSpec fields the backend does not use, kept so the output can be compared with the engine byte for byte */
typedef struct {
    int punct;              /* +0x28 ePuncType: 1 after rule2 0xF/0x10, 3 after 0xE ('?'), else 0 */
    int ctrl;               /* +0x48 bit0 word start, bit1 sentence start */
    int voice;              /* +0x44 (always 0) */
    int src_pos, src_len;   /* +0x4C/+0x50 */
    int sent_pos, sent_len; /* +0x54/+0x58 */
} anna_unitinfo;

typedef struct anna_udt anna_udt;

/* load M1033DSK.UDT (the unit definition table) */
anna_udt *anna_udt_load(const char *path, char *err, size_t errlen);
void anna_udt_free(anna_udt *u);
int anna_udt_count(const anna_udt *u);
const char *anna_udt_name(const anna_udt *u, int index); /* "w+aa+n", "_a_"; NULL out of range */
int anna_udt_index(const anna_udt *u, int index);        /* the type index the UNT groups by (1..260), -1 if none */

/* one sentence of word records -> UnitSpecs, including the engine's leading copy at index 0.
 * Writes at most max entries to out (and info, if not NULL); returns the number of UnitSpecs, or -1. */
int anna_words_to_units(const anna_udt *u, const anna_word *w, int nw, anna_unitspec *out, anna_unitinfo *info, int max);

#endif
