/* zf1_morph.h - CEnMorph morphological lexicon (MSTTSLoc_OneCore.dll loc!18005c5c4 / 18005c8e0 / 180008e50).
 * Registered in the container lexicon after the main lexicon (priority value 0x4000 -> i278 = 2).
 * Portable C99.  See notes/fe1_pron.md. */
#ifndef ZF1_MORPH_H
#define ZF1_MORPH_H
#include "zf1_pron.h"

struct zf1_morph *zf1_morph_create(const zf1_dat *d, const zf1_lex *lx, const zf1_phoneset *ps);
void zf1_morph_destroy(struct zf1_morph *m);
/* CEnMorph vt+8: 1 = analysed (out filled with source 0x4000 prons), 0 = not found */
int zf1_morph_lookup(struct zf1_morph *m, const zf_char *w, int n, zf1_lexhit *out);
#endif
