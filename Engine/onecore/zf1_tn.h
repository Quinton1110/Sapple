/* zf1_tn.h - named-entity detection and text normalization (CNEDetector / CTextNormalizer), portable C99.
 *
 * Engine (MSTTSEngine_OneCore.dll 10.3.21207.0):
 *   CNEDetector::FindNext   0x180061910  (NE FST pre-pass + TN network in "detect" mode, lattice tokenizer 3)
 *   fragment NE loop        0x1800c6888  (TNScope=FullContext)
 *   CTextNormalizer::Normalize 0x1800c3e50 (TN network with the category, "sp:default" fallback, tokenizer 0)
 *   Analyze step 7          0x180018ed4 -> 0x18004b06c (entity token -> normalized words)
 * See notes/fe1_tn.md for the call order the integration must follow.
 */
#ifndef ZF1_TN_H
#define ZF1_TN_H
#include "zf1_int.h"
#include "zf1_fst.h"
#include "zf1_lex.h"

typedef struct zf1_tn {
    zf1_net *net;                       /* TN network 7d5841ab/ea8d0701 ("TRANSDUCER_en-US") */
    zf1_lex lex;                        /* main lexicon (kind-2 labels "Any_Word"/"Any_NOUN_WORD") */
    int haslex;
    void *fst;                          /* NE FST (bfc4309d/d2343132, CFstMatcher), zf1_nefst.c; NULL = off */
    uint16_t posattr;                   /* lexicon attribute id named "POS" (0) */
    void *lexbuf;                       /* scratch zf1_lexent */
} zf1_tn;

int zf1_tn_init(zf1_tn *t, const zf1_dat *d);      /* 0 ok */
void zf1_tn_free(zf1_tn *t);

/* CTextNormalizer::Normalize.  Returns 0 and a malloc'd array of malloc'd words (*words, *nwords; the engine's
 * output string split on ' ', "[break=NNNms]" markers included as words) or 1 when the text could not be
 * parsed completely (engine S_FALSE: the caller keeps the token unchanged). */
int zf1_tn_normalize(zf1_tn *t, const zf_char *text, int len, const zf_char *category, zf_char ***words,
                     int *nwords);
void zf1_tn_words_free(zf_char **words, int n);

/* CNEDetector::FindNext on text[0..len): returns 0 (entity found: *pre = offset of the entity, *elen = length,
 * category copied to cat (catcap units incl. NUL), *attr = POS attribute value of the category rule or 0xFFFF)
 * or 1 (no entity).  category: SAPI say-as category or NULL. */
int zf1_ne_findnext(zf1_tn *t, const zf_char *text, int len, const zf_char *category, int *pre, int *elen,
                    zf_char *cat, int catcap, uint16_t *attr);

/* SAPI <context id> / say-as -> TN category (FUN_1800c4f10: "ml:" + InterpretAs [+ "_" + Format], kept only if
 * the TN network can produce it in mode 2, FUN_180136700).  Returns 1 and writes the category, else 0. */
int zf1_tn_sayas_category(zf1_tn *t, const zf_char *interpret_as, const zf_char *format, zf_char *out, int cap);

/* Analyze step 4 (FUN_18002bf08), for EVERY word: RegularDomain (+0x1b0, zf_word.s1b0) from the fragment's say-as
 * (w->fs.sayas) and/or the NE type ("sp:date" -> "date", "sp:webUrl" -> "url", ...). */
void zf1_tn_domain(zf_word *w);

/* ---- fragment segmentation (FUN_1800c6888, FullContext) ---- */
typedef struct zf1_tnseg {
    int off, len;                       /* span inside the fragment text */
    int entity;                         /* 1 = entity token (one CWordBreakingInfo), 0 = plain text to word-break */
    zf_char cat[64];                    /* entity category ("sp:date"), empty for plain text */
    uint16_t attr;                      /* POS attribute of the category rule (FindNext param_8), 0xFFFF none */
} zf1_tnseg;

/* Splits a speak-fragment's text into plain spans and entity tokens.  Returns the number of segments written
 * to *segs (malloc'd; free()).  sayas: the fragment's say-as category (SAPI <context id>) or NULL. */
int zf1_tn_segment(zf1_tn *t, const zf_char *text, int len, const zf_char *sayas, zf1_tnseg **segs);

/* ---- Analyze step 7 (FUN_180018ed4 without the acronym part FUN_180079408) ----
 * For every word whose ne_type is set (entity token created from a zf1_tnseg with entity=1, see notes), run
 * Normalize(text, ne_type) and replace the word by the normalized words (FUN_18004b06c):
 *   type 0, lang/fs/src_off/src_len copied from the token, text (+ ci upper-case, regular lower-case),
 *   i27c = 1, ne_type = token's ne_type, s1b0 = token's s1b0, bi = 2 and i248 = 1 on every word that is not the
 *   last output word, "[break=Nms]" adds N to the previous new word's i214 and clears its i248.
 * If exactly one word results it gets the token's pos (vt+0xe8).  Normalize S_FALSE: the token is kept.
 * The engine then POS-tags the new words (CPOSTaggerImpl, fork C): postag(ctx, wl, first, count) is called for
 * each replaced range when non-NULL (oldpos = the token pos, to be set with zf1_pos_set when count == 1). */
typedef void (*zf1_tn_postag_fn)(void *ctx, zf_wordlist *wl, int first, int count, uint16_t oldpos);
int zf1_tn_expand(zf1_tn *t, zf_wordlist *wl, zf1_tn_postag_fn postag, void *ctx);

#endif
