/* zf1_eng.h - zf1 engine object (internal).  Portable C99. */
#ifndef ZF1_ENG_H
#define ZF1_ENG_H
#include "zf1_ana.h"

struct zf1_engine {
    zf1_dat dat;
    zf1_lex lex;
    zf1_phoneset ps;
    zf1_wb wb;
    zf1_ctab ct;
    zf1_frags fr;
    zf1_enum en;
    zf1_ana ana;
    zf1_hooks hooks;
    int group, ngroups;
    int (*sentsep)(void *ctx, const zf_char *text, int len, int *consumed, int *hard_end);
    void *ssctx;
    void *mod;                          /* zf1_modules.c private state (component objects) */
    char datdir[1024];                  /* folder of the main .dat (domain files enUS.<Domain>.dat live there); "" = default */
    int lcid;                           /* ClassicVoices patch: 1033 en-US, 2057 en-GB (picks the domain files) */
};

int zf1_modules_init(zf1_engine *e);   /* zf1_modules.c: wires the component modules into e->hooks */
void zf1_modules_free(zf1_engine *e);
/* end of CTextProcessor for one sentence: first-pass word-level part of sentence vt+0xa0 (FUN_18001a644):
 * sentence-initial SIL word and SIL words after words with a +0x214 pause */
void zf1_sentence_finish(zf1_engine *e, zf_sentence *s);
void zf1_sentence_post(zf1_engine *e, zf_sentence *s);
#endif
