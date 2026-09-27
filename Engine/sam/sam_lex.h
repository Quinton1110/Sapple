/* Pronunciation lookup for libsam: SAPI compressed lexicon + letter-to-sound model. */
#ifndef SAM_LEX_H
#define SAM_LEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAM_MAX_PRON 96

/* A pronunciation as SAPI American English phone ids (1 = syllable break, 8/9 = stress 1/2). */
typedef struct {
    uint16_t ph[SAM_MAX_PRON];
    int n;
    uint32_t pos; /* SAPI part of speech (0x1000 noun, ...), 0xFFFFFFFF = unknown */
} sam_pron;

typedef struct sam_lexicon sam_lexicon;
typedef struct sam_lts sam_lts;

sam_lexicon *sam_lexicon_load(const char *path);
sam_lexicon *sam_lexicon_load_mem(const void *data, size_t size); /* the data is copied */
void sam_lexicon_free(sam_lexicon *x);
/* Returns the number of pronunciations written (0 = not in the lexicon). */
int sam_lexicon_lookup(const sam_lexicon *x, const char *word, sam_pron *out, int max);

sam_lts *sam_lts_load(const char *path);
sam_lts *sam_lts_load_mem(const void *data, size_t size);
void sam_lts_free(sam_lts *m);
/* Returns the number of pronunciations (0 when the word cannot be pronounced, e.g. no known letters). */
int sam_lts_pronounce(const sam_lts *m, const char *word, sam_pron *out, int max);

/* Full lookup chain of the engine: lexicon, then morphology, then letter to sound. */
typedef struct {
    uint16_t ph[SAM_MAX_PRON];
    int n;
    uint32_t pos, lextype;
} sam_lookup_entry;

typedef struct {
    uint16_t pron[SAM_MAX_PRON]; /* first pronunciation */
    int n;
    uint32_t posa[4];            /* parts of speech that use the first pronunciation */
    int nposa;
    uint16_t alt[SAM_MAX_PRON];  /* first different pronunciation (homographs) */
    int nalt;
    uint32_t posb[4];            /* parts of speech of the other pronunciations */
    int nposb;
    uint32_t lextype;            /* 0x1000 lexicon, 0x2000 LTS, | 0x4000 derived by morphology */
} sam_lookup;

int sam_word_lookup(const sam_lexicon *lex, const sam_lts *lts, const char *word, sam_lookup *out);
/* Same with a forced part of speech (0 = none): a lexicon or morphology result that has it wins. */
int sam_word_lookup_pos(const sam_lexicon *lex, const sam_lts *lts, const char *word, uint32_t pos, sam_lookup *out);

#ifdef __cplusplus
}
#endif
#endif
