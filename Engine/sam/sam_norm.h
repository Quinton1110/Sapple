/* Text normalizer of spttseng.dll: text -> sentences of tagged tokens (nodes) and words (entries).
 *   FUN_5ed55a9a  sentence driver (at most 50 tokens, unterminated sentences get a period)
 *   FUN_5ed5521e  tokenizer: whitespace tokens, leading/trailing punctuation, abbreviations, initials
 *   FUN_5ed4cb02 / FUN_5ed4c45e  token classification (numbers, dates, times, phone numbers, ...)
 *   FUN_5ed4b6e0  lookup + part-of-speech tagging (sam_pos.c)
 */
#ifndef SAM_NORM_H
#define SAM_NORM_H

#include "sam_pos.h"

typedef struct {
    sam_node *nodes;
    int nn, ncap;
    sam_tag_entry *e;
    int ne, ecap;
    int end_ofs; /* character offset of the end of the text */
} sam_sentence;

typedef struct sam_norm sam_norm;

sam_norm *sam_norm_new(const char *utf8);
void sam_norm_free(sam_norm *nm);
/* Produces the next sentence (nodes + tagged entries). Returns 1 for a sentence, 0 at the end of
 * the text, -1 on error. The sentence stays valid until the next call. */
int sam_norm_next(sam_norm *nm, const sam_lexicon *lex, const sam_lts *lts, sam_sentence *out);
void sam_sentence_free(sam_sentence *s);

#endif
