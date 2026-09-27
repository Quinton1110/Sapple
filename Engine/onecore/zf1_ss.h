/* zf1_ss.h - CSentSepImpl (rule-based sentence separator of MSTTSEngine_OneCore.dll 10.3.21207.0, vtable
 * 0x18016d758, model "SENTSEP_en-US" = resource e67ab014/1df28780) and the CSentenceEnumerator piece loop.
 * Portable C99.  See notes/fe1_ss.md. */
#ifndef ZF1_SS_H
#define ZF1_SS_H
#include "zf1_int.h"

/* one SENTSEP word list (file order 0..11), strings converted to native zf_char */
typedef struct zf1_sslist {
    int n;
    zf_char **s;
} zf1_sslist;

/* CCharTable (f6e4f50a/80b9a5a3): only the code point + flags are needed here */
typedef struct zf1_sschartab {
    int n;
    const uint8_t *rec;                 /* {u32 codepoint, u32 flags, u32 name1Off, u32 name2Off}[n] */
} zf1_sschartab;

typedef struct zf1_ss {
    zf1_sslist list[12];
    zf1_sschartab ctab;
    int ok;
} zf1_ss;

int zf1_ss_init(zf1_ss *s, const zf1_dat *d);
void zf1_ss_free(zf1_ss *s);

/* One call of CSentSepImpl vt+8 (FUN_18005d050) on text[0..len).  The text must stay readable up to text[len]
 * (the engine's tokenizer may look at text[len], which is 0 for a 0-terminated fragment; pass the real following
 * character if the text continues).  *consumed = vt+0x10 (length of the piece that belongs to the current
 * sentence), *hard_end = vt+0x18 (1 = the sentence ends after this piece).  Returns 0. */
int zf1_ss_run(const zf1_ss *s, const zf_char *text, int len, int *consumed, int *hard_end);
/* FUN_180072d30: blank / control character test of the engine tokenizer */
int zf1_ss_ws(zf_char c);

/* ---- CSentenceEnumerator::Next piece loop (FUN_1800624e0) over the engine fragment list ----
 * frag.action = engine node action (0 speak, 1 silence, 2 pronounce, 3 bookmark, 4 spell, 5 section, 6/7 SSML
 * sentence/paragraph start, 8 other, 9 internal); only actions 0, 4, 9 carry text that is sentence-split.
 * sayas = node+0x20 InterpretAs (NULL = none): "address"/"name" fragments are not split (unless > 499 chars).
 * text[len] must be readable (the character that follows in the caller's buffer, 0 if none). */
typedef struct zf1_ssfrag {
    int action;
    const zf_char *text;
    int len;
    const zf_char *sayas;
} zf1_ssfrag;

/* position = (fragment index, character index in that fragment's text); frag = -1 means "no position" (NULL) */
typedef struct zf1_sspos {
    int frag;
    int pos;
} zf1_sspos;

/* Produce the next sentence starting at *cur (initialise cur = {0, 0}).  Returns 1 and sets *start / *end
 * (end = position after the last character of the sentence, in end->frag; end->frag = -1 when the sentence runs
 * to the end of the fragment list) and advances *cur; returns 0 when there is nothing left (cur->frag == -1);
 * < 0 on error (engine HRESULTs 0x80048028 for an over-long action-9 fragment, 0x8000FFFF for a NULL text). */
int zf1_ss_next(const zf1_ss *s, const zf1_ssfrag *f, int nfrag, zf1_sspos *cur, zf1_sspos *start, zf1_sspos *end);

#endif
