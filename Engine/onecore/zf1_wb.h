/* zf1_wb.h - CWhiteSpaceBreaker (engine vt 0x180176410) with its CWordBreakerInfo resource 629aa5c4/9756c4e5
 * and the CVocabTrie matcher (FUN_18003cbe0 / FUN_18002c5f0).  Portable C99. */
#ifndef ZF1_WB_H
#define ZF1_WB_H
#include "zf1_int.h"

typedef struct zf1_trie {
    const uint8_t *base;                /* blob start (file offset alignment matters: see zf1_wb.c) */
    const uint8_t *T1, *T3, *T4;        /* threshold tables (u16) */
    const uint8_t *nodes;               /* {u16 char, u16 flags} */
    const uint8_t *T7, *T8;             /* u32 offset tables */
    const uint8_t *root;
    int ok;
} zf1_trie;

typedef struct zf1_wb {
    uint16_t lang;
    int n0, n1, n2;
    const uint8_t *t0, *t1, *t2;        /* sorted u32 tables: skip (whitespace/control), split (punctuation/symbols), trim ('-...) */
    zf1_trie trie0, trie1;              /* abbreviations+emoji / emoticons+fixed phrases */
} zf1_wb;

int zf1_wb_init(zf1_wb *wb, const zf1_dat *d, uint16_t lang);
/* longest vocabulary match at text[0..n): length, or -1 when nothing matches */
int zf1_trie_match(const zf1_trie *t, const zf_char *text, int n);
/* Break text[0..n) into tokens; emit(ctx, offset, len) is called per token in order (FUN_180043cd0). */
typedef int (*zf1_wb_emit)(void *ctx, int off, int len);
int zf1_wb_break(const zf1_wb *wb, const zf_char *text, int n, int flag, zf1_wb_emit emit, void *ctx);
int zf1_wb_is_skip(const zf1_wb *wb, uint32_t c);   /* table 0 */
int zf1_wb_is_split(const zf1_wb *wb, uint32_t c);  /* table 1 */
int zf1_wb_is_trim(const zf1_wb *wb, uint32_t c);   /* table 2 */
#endif
