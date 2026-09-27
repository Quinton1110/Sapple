/* zf1_post.h - the rule-based "sentence and punctuation" steps of CTextProcessor (fork E).  Portable C99.
 *
 * Covers (MSTTSEngine_OneCore.dll 10.3.21207.0; notes/fe1_post.md):
 *   - Analyze step 9  FUN_1800bb3e0 -> FUN_180060c48 -> FUN_1800200dc: per-word pattern machine ("W" mode) that
 *     marks punctuation tokens (type 1) and sets the boundary class +0x180 (i180) of the token and of the word
 *     before it.                                                                        -> zf1_post_words()
 *   - after Analyze (18005c794), on the committed sentence INCLUDING the sentence-initial / TN silence tokens that
 *     sentence vt+0xa0 (FUN_18001a644, first call) has already inserted:
 *       sentence vt+0xb0 = FUN_1800b5aa0   NE / say-as segments (sentence+0x278, word+0x1a8)
 *       FUN_1800b7358                       CSentAndPuncDetector: sentence type (sentence+0xa8), "S" mode
 *       FUN_18001bf24                       RegularText (+0xb8) with the suffix tables (5554ba64)
 *       FUN_1800b8608                       quote pairs (sentence+0x1e8)
 *       FUN_1800b90a8                       parallel structures (sentence+0x248)       -> zf1_post_sentence()
 *
 * Resources: 00a2359e/4d4d5073 "sPMM" pattern machine, 5554ba64/09397c04 suffix tables, b54490e3/e4c81da4 quote
 * pairs, d8951565/52a33b0b parallel-structure config.  POS names/classes = the engine POS table object
 * (sentence+0x2d8, observed at runtime; the names are the lexicon attribute names).
 */
#ifndef ZF1_POST_H
#define ZF1_POST_H
#include "zf1_int.h"

/* ---- sentence-level lists produced by zf1_post_sentence (proposed additions to zf_sentence) ---- */
/* zf_segment is defined in zf.h */

/* Quote pairs (+0x1e8) and parallel structures (+0x248) go to zf_sentence.quotes / .par (zf.h, fields added by
 * zf2): quote {open, close} word indices; parallel struct = elements {a = first word, b = last word, type}
 * where type 1 = first conjunct (list opener), 2 = middle conjunct, 3 = last conjunct, which starts at the
 * and/or/nor word.  zf_word.ne_obj (+0x1a8) = 1 on the first word of a segment, 2 on the others. */
typedef struct zf1_postout {
    int nsegs;   zf_segment *segs;      /* +0x278 */
    int *word_seg;                      /* [nwords]: word+0x1a8 as an index into segs, -1 = none */
} zf1_postout;

void zf1_postout_free(zf1_postout *o);

/* ---- engine objects ---- */
typedef struct zf1_post {
    const uint8_t *pm;                  /* sPMM blob (NULL = detector off) */
    size_t pmsize;
    const uint8_t *suf;                 /* suffix tables blob */
    int nsuf[4];                        /* 0 noun (class 1), 1 adj (class 3), 2 verb (class 2), 3 apostrophe */
    const uint8_t *sufrec[4];           /* 20-byte records: wchar[10] */
    int nquote;
    const uint8_t *quote;               /* {u16 open, u16 close, u32 typographic} */
    int quote_ok;
    int nsep, nconj;                    /* parallel config: separators (",") and conjunctions (and/or/nor) */
    const uint8_t *sep, *conj;          /* 0x2c-byte records {u32 tpos, wchar text[20]} */
    int par_ok;
} zf1_post;

int zf1_post_init(zf1_post *p, const zf1_dat *d);
void zf1_post_free(zf1_post *p);

/* Analyze step 9: runs on the analyzer word list after TN expansion (step 7) and before CPronouncer (step 10).
 * Reads text, type, src_off/src_len, ne_type; writes type (1 = punctuation) and i180. */
int zf1_post_words(zf1_post *p, zf_wordlist *wl);

/* Post-Analyze steps on one sentence (words = committed list incl. silence tokens, with pron set).
 * Reads type, text, pos (+0x138), tpos (+0x13a), i180, src_off/src_len, ne_type, fs.sayas (words of the same
 * fragment must share the same fs.sayas pointer), pron.  Writes s->type, s->quotes/nquotes, s->par/npar (any
 * previous content is freed), every word's regular text and ne_obj, and *out. */
int zf1_post_sentence(zf1_post *p, zf_sentence *s, zf1_postout *out);

/* POS table (engine POS object at sentence+0x2d8): name (vt+8) and class (vt+0x10) of a POS id. */
const char *zf1_pos_name(unsigned pos);
int zf1_pos_class(unsigned pos);
#define ZF1_POS_DEFAULT 50              /* vt+0x38 */

#endif
