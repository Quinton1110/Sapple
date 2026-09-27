/* zf1_lts.h - CART letter-to-sound (resource ac4aefcf/d18325ec, CLTSLexiconCART) and helpers.
 * Engine: FUN_18015f7b4 -> CLTSLexiconCART vt+0x18 (FUN_18015fb70) -> FUN_1800575d8 (beam search, FUN_180160300 per
 * letter, tree walk FUN_180047308, n-best FUN_180160c08) -> FUN_18015f884 (phone names -> ids).  See notes/fe1_pron.md. */
#ifndef ZF1_LTS_H
#define ZF1_LTS_H
#include "zf1_int.h"
#include "zf1_lex.h"

typedef struct zf1_cart_tree {
    uint32_t nnodes;
    const uint8_t *nodes;               /* u32 {u16 a, u16 b} */
    uint32_t nleaf;
    const uint8_t *leaf;                /* distributions */
    uint32_t nq;
    const uint8_t *q;                   /* u16 question terms */
} zf1_cart_tree;

typedef struct zf1_cart {
    uint32_t nlet;                      /* letters ('@', '\'', '-', a..z) */
    const uint8_t *letoff, *letpool;
    uint32_t nout;                      /* output symbols ("", "aa1", ...) */
    const uint8_t *outoff, *outpool;
    uint32_t mn[2], mm[2];              /* bitset matrices: [0] letters, [1] output labels */
    const uint8_t *mat[2];
    uint32_t nqd;
    const uint8_t *qd;                  /* u16 question definitions */
    zf1_cart_tree tree[64];             /* index = letter index (1..nlet-1) */
} zf1_cart;

int zf1_cart_init(zf1_cart *c, const uint8_t *res, size_t size);   /* 0 ok */
/* Raw prediction = CLTSLexiconCART vt+0x18: best candidate string (phone names + stress digits separated by
 * blanks, e.g. "h eh l  ow1").  Returns 0 ok (out may be empty), <0 error. */
int zf1_cart_predict(const zf1_cart *c, const zf_char *word, zf_char *out, int cap);
/* FUN_18015f884: textual phones -> phone ids (unknown names skipped).  Returns count. */
int zf1_phonestr_to_ids(const zf1_phoneset *ps, const zf_char *s, uint16_t *ph, int cap);
/* CPhoneme vt+0x138: 1 if any of ph[0..n) is a vowel (phone flags & 1) */
int zf1_has_vowel(const zf1_phoneset *ps, const uint16_t *ph, int n);
int zf1_phone_flags(const zf1_phoneset *ps, int id, uint32_t *flags);   /* 1 if known */

/* MSVC CRT qsort (same element order for equal keys as the engine's _o_qsort) */
void zf1_ms_qsort(void *base, size_t num, size_t width, int (*comp)(const void *, const void *));

#endif
