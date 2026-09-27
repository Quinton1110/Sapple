/* Part-of-speech tagging and homograph selection (spttseng FUN_5ed4b6e0 / FUN_5ed4a436). */
#ifndef SAM_POS_H
#define SAM_POS_H

#include "sam_lex.h"

/* Node (token) types of the engine's normalizer. */
enum {
    SAM_NODE_OPEN_PAREN = 1, SAM_NODE_OPEN_BRACKET, SAM_NODE_OPEN_BRACE, SAM_NODE_CLOSE_PAREN, SAM_NODE_CLOSE_BRACKET,
    SAM_NODE_CLOSE_BRACE, SAM_NODE_APOSTROPHE, SAM_NODE_QUOTE, SAM_NODE_PERIOD, SAM_NODE_EXCLAMATION,
    SAM_NODE_QUESTION, SAM_NODE_COMMA, SAM_NODE_SEMICOLON, SAM_NODE_COLON, SAM_NODE_HYPHEN,
    SAM_NODE_WORD = 0x1002, SAM_NODE_ABBREV = 0x1003, SAM_NODE_ABBREV2 = 0x1004, SAM_NODE_CARDINAL = 0x1006,
    SAM_NODE_ORDINAL = 0x1007, SAM_NODE_DECIMAL = 0x1008, SAM_NODE_FRACTION = 0x100e
};

/* One word as the tagger sees it (the engine's 0x848-byte entry). */
typedef struct {
    char text[128];
    uint16_t pron[2][SAM_MAX_PRON]; /* 0: pronunciation for posa, 1: alternate for posb (SAPI phone ids) */
    int npron[2];
    uint32_t posa[4], posb[4];
    int nposa, nposb;
    int altok; /* the alternate pronunciation may be selected */
    int idx;   /* selected pronunciation */
    int star;  /* pronunciation fixed by a special-word handler */
    uint32_t pos, lock;
} sam_tag_entry;

typedef struct {
    int type;
    uint32_t pos;   /* punctuation part of speech (0x400e sentence end, ...); 0 for words */
    char text[128]; /* token text as written */
    int first, count; /* entries of this node */
    int ofs, len;   /* character offset and length in the input text */
    const int *ti;  /* normalizer type record (first int = type), NULL if none */
} sam_node;

/* Abbreviation / special word record (tables @0x5eda8898, @0x5edaa180): up to three pronunciations
 * (SAPI phone names) with their parts of speech, and handler indices (-1 none). */
typedef struct {
    const char *word;
    const char *pron[3];
    uint32_t pos[3];
    int shandler; /* sentence-time handler (abbreviations: FUN_5ed4683a/469a4/46b27) */
    int thandler; /* tag-time handler */
} sam_abbrev;

/* type record of an abbreviation node (0x1003 / 0x1004) */
typedef struct {
    int type;
    const sam_abbrev *rec;
} sam_abbrev_ti;

/* Fills the entries of every node (special words or the lookup chain), runs the tagger and the
 * post-tagging rules. On return entry.pron[entry.idx] and entry.pos are the engine's choices.
 * Returns -1 when a word cannot be pronounced (the engine then stops speaking). */
int sam_pos_process(const sam_lexicon *lex, const sam_lts *lts, sam_node *nodes, int nn, sam_tag_entry *e);

/* Entry setup from the lookup chain (FUN_5ed4b1f9). */
void sam_pos_entry_from_lookup(sam_tag_entry *e, const sam_lookup *r);

/* Contextual rules only (FUN_5ed4a436). */
void sam_pos_tag(sam_tag_entry *e, int n);

#endif
