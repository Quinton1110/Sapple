/* zf1_ana.h - sentence enumeration (CSentenceEnumerator, FUN_1800624e0) and CSentenceAnalyzer core
 * (FUN_180065980 Analyze: fragment walk 1800c51cc, word creation, commit 1800bd64c).  Portable C99. */
#ifndef ZF1_ANA_H
#define ZF1_ANA_H
#include "zf1_frag.h"
#include "zf1_wb.h"
#include "zf1_lex.h"

/* sentence span: [node n0, char p0) .. (node n1, char p1] as returned by the enumerator */
typedef struct zf1_span { int n0, p0, n1, p1; } zf1_span;

/* one CWordBreakingInfo in a CTTSWordSink */
typedef struct zf1_tok {
    int off, len;                       /* in the analyzer sentence buffer */
    int entity;                         /* +0x28 */
    const zf_char *cat;                 /* +0x30 category (entity) */
    uint16_t pos;                       /* +0x38 */
} zf1_tok;
typedef struct zf1_toklist { zf1_tok *t; int n, cap; } zf1_toklist;
int zf1_tok_add(zf1_toklist *l, int off, int len, int entity, const zf_char *cat, uint16_t pos);

typedef struct zf1_ana zf1_ana;

/* hooks the other zf1 modules plug into (NULL = stage not available yet) */
typedef struct zf1_hooks {
    void *ctx;
    /* text of one fragment piece -> CTTSWordSink tokens (NE/TN path 1800c4f10 -> 1800c6888 ...).
       If NULL the plain CWhiteSpaceBreaker path is used. */
    int (*text_tokens)(void *ctx, zf1_ana *a, const zf1_node *nd, const zf_char *text, int off, int len, zf1_toklist *out);
    int (*pos_tag)(void *ctx, zf_wordlist *wl);             /* step 5 */
    int (*acronyms)(void *ctx, zf_wordlist *wl);            /* step 7a */
    int (*tn_expand)(void *ctx, zf_wordlist *wl);           /* step 7b */
    int (*rules)(void *ctx, zf_wordlist *wl);               /* step 9 */
    int (*pronounce)(void *ctx, zf_wordlist *wl);           /* step 10 */
    int (*post)(void *ctx, zf_sentence *s);                 /* after Analyze (sentence type, quotes, ...) */
    /* <pron sym>: SAPI phone ids -> n word prons (0-terminated phone ids in ph[k], comma[k] = a "," punctuation
       word follows); set with setpron (i278 20) */
    int (*sapi_pron)(void *ctx, const uint16_t *ids, uint16_t (*ph)[0x182], int *comma, int max);
    void (*setpron)(zf_word *w, const uint16_t *ph);
} zf1_hooks;

/* CCharTable (resource f6e4f50a/80b9a5a3): {u32 code point (surrogate pair = hi<<16|lo), u32 flags,
 * u32 name offset, u32 pron offset} sorted by code point, UTF-16 pool behind the records (byte offsets).
 * name = reading as words ("exclamation point"), pron = phone-id string (letters, digits). */
typedef struct zf1_ctab {
    int n;
    const uint8_t *rec, *pool;
} zf1_ctab;
int zf1_ctab_init(zf1_ctab *ct, const zf1_dat *d);
/* returns 1 and name/pron pointers (UTF-16 LE, 0-terminated, possibly empty) */
int zf1_ctab_find(const zf1_ctab *ct, uint32_t cp, const uint8_t **name, const uint8_t **pron, uint32_t *flags);

struct zf1_ana {
    const zf1_wb *wb;
    const zf1_hooks *hk;
    const zf1_lex *lex;
    const zf1_phoneset *ps;
    const zf1_ctab *ct;
    zf1_frags *fr;
    zf1_span sp;
    zf_char sbuf[512];                  /* A+0x28 sentence text (<= 500) */
    int slen;                           /* A+0x418 */
    int mapn[512], mapi[512];           /* buffer index -> (node, index in node text) */
    zf_wordlist wl;                     /* A+0x420 */
    int bm_pending;                     /* A+0x450 */
    zf_char *bm_name;                   /* A+0x458 */
};

/* CSentenceEnumerator state */
typedef struct zf1_enum {
    int node, pos;                      /* +0x38 current node, +0x58 position */
    int group;
    /* sentence separator: returns consumed length and hard-end flag for text[0..len) */
    int (*sentsep)(void *ctx, const zf_char *text, int len, int *consumed, int *hard_end);
    void *ssctx;
} zf1_enum;

void zf1_enum_start(zf1_enum *en, const zf1_frags *fr, int group);
/* 1 = sentence span returned, 0 = no more text in this group */
int zf1_enum_next(zf1_enum *en, const zf1_frags *fr, zf1_span *sp);

/* Analyze one sentence -> fills s (words committed) */
int zf1_analyze(zf1_ana *a, zf1_frags *fr, const zf1_span *sp, zf_sentence *s);

/* default token path: whitespace breaker on buffer text [off, off+len) */
int zf1_ana_break(zf1_ana *a, int off, int len, zf1_toklist *out);
#endif
