/* zf1_pron.h - CPronouncer (engine 0x1800479e0) : lexicon pronunciation choice, OOV handling (LTA OOV splitter,
 * COMPOUNDPRON, CART LTS, letter spelling), syllabification (CSyllableSeg) and the "spelled" flag (+0x368).
 * Portable C99.  See notes/fe1_pron.md.
 *
 * CALL ORDER inside the analyzer (CSentenceAnalyzer::Analyze 0x180065980, step 10 = CPronouncer vt+0x48):
 *     zf1_pron_pass1(p, wl);      180047abc for every word: container lexicon, POS-matched pronunciation
 *     zf1_pron_pass2(p, wl);      180047eac for every word: OOV words (splitter / compound / LTS / spelling)
 *     zf1_poly_apply(pos, wl);    fork C: CPolyphony rules (1800c9e30) + CRF homographs (1800caca8, 180060158)
 *     zf1_pron_pass3(p, wl);      180017d1c for every word: syllabify pronunciations that are not syllabified
 *     zf1_pron_pass4(p, wl);      1800563dc: +0x368 "pronounced as its letters" flag (may re-spell)
 *     zf1_pron_finish(p, wl);     fills zf_word.pron (+0x160 names string) from the current pronunciation
 * or simply zf1_pron_sentence(p, wl) which does all of it (fork C's step via the p->poly callback, if set).
 *
 * Inputs read from each word: type, text (+0x98), pos (+0x138, set by the POS tagger, step 5), s1b0 (domain,
 * "none"), ne_type (+0x188), fs.action / fs.pos (fragment), existing prons (e.g. <pron sym> words, i278 == 20).
 * Outputs: prons / cur_pron (+0x360 list), i278 (+0x278 source), i368, pos (fragment POS override), pron. */
#ifndef ZF1_PRON_H
#define ZF1_PRON_H
#include "zf1_int.h"
#include "zf1_lex.h"
#include "zf1_lts.h"

/* ---- container-lexicon hit (one ITTSLexicon entry): list of pronunciations with their attribute sets ---- */
#define ZF1_LH_MAXSET 8
#define ZF1_LH_SETLEN 8
typedef struct zf1_lhpron {
    int nph;
    uint16_t ph[ZF1_LEX_MAXPH + 2];     /* 0-terminated phone ids */
    uint32_t source;                    /* lexicon priority value: 0x1000 main LEX, 0x4000 morph (CEnMorph) */
    int nset;
    uint16_t set[ZF1_LH_MAXSET][ZF1_LH_SETLEN];   /* attribute ids, each set 0-terminated */
} zf1_lhpron;

typedef struct zf1_lexhit {
    int n;
    zf1_lhpron p[ZF1_LEX_MAXPRON];
} zf1_lexhit;

typedef struct zf1_chartab {
    uint32_t n;
    const uint8_t *rec;                 /* {u32 cp, u32 flags, u32 name1Off, u32 name2Off}[n], sorted by cp */
    const uint8_t *pool;
} zf1_chartab;

struct zf1_morph;                       /* CEnMorph (zf1_morph.c) */

typedef struct zf1_pron {
    const zf1_lex *lx;
    const zf1_phoneset *ps;
    zf1_cart cart;
    int have_cart;
    zf1_chartab ct;
    uint32_t nonset;
    const uint8_t *onset;               /* CSyllableSeg: {u16 p1,p2,p3,0}[nonset] */
    struct zf1_morph *morph;                   /* NULL = no morph lexicon */
    void *compound;                     /* COMPOUNDPRON / LTA compound breaker state (zf1_pron_oov.c) */
    void *posobj;                       /* fork C context for the two callbacks below */
    void (*setpos)(void *posobj, zf_word *w, uint16_t pos);      /* word vt+0xe8 (zf1_pos_set) */
    int (*poly)(void *posobj, zf_wordlist *wl);                  /* CPolyphony + CRF (zf1_poly_apply) */
    int ndom;                           /* domain lexicons (enUS.<Domain>.dat, priority value 0x10000 -> i278 3) */
    struct { char name[32]; zf1_dat dat; zf1_lex lex; } dom[8];
    const zf_char *curdom;              /* domain (s1b0) of the word pass2 is working on (compound / breaker lookups) */
    uint16_t sylmark;                   /* phone id of '-' (2) */
    uint16_t sep;                       /* phone id of '&' (1) */
} zf1_pron;

int zf1_pron_init(zf1_pron *p, const zf1_dat *d, const zf1_lex *lx, const zf1_phoneset *ps);
void zf1_pron_free(zf1_pron *p);

/* Container lexicon lookup (CTTSContainerLexicon vt+8, 0x18001d850): main LEX, then the morph lexicon.
 * Returns 1 if found. */
int zf1_pron_lookup(zf1_pron *p, const zf_char *w, int n, zf1_lexhit *out);
/* container lookup with a domain (container vt+0, 0x1800c1350): the domain's lexicon first (words whose s1b0 is
 * not "none"/"general"), then the default lookup */
int zf1_pron_lookup_dom(zf1_pron *p, const zf_char *w, int n, const zf_char *domain, zf1_lexhit *out);
/* load the domain lexicons enUS.{Address,Name,Message,Computer,Media,CompanyName}.dat from dir (the directory of
 * MSTTSLocEnUS.dat, with trailing separator); zf1_pron_init calls it with the default Windows directory */
int zf1_pron_load_domains(zf1_pron *p, const char *dir);
/* ClassicVoices patch: the same for a locale (1033 = zf1_pron_load_domains; 2057: the en-GB domain files) */
int zf1_pron_load_domains_lcid(zf1_pron *p, const char *dir, int lcid);
/* Adapter for zf1_pos_set_lookup (fork C): writes the attribute sets of all prons, each 0-terminated. */
int zf1_pron_netlex_cb(void *ctx, const zf_char *word, int n, uint32_t *pairs, int *counts, int maxitems,
                       int maxpairs);
int zf1_pron_lexlookup_cb(void *ctx, const zf_char *w, int n, uint16_t *sets, int cap);

int zf1_pron_pass1(zf1_pron *p, zf_wordlist *wl);
int zf1_pron_pass2(zf1_pron *p, zf_wordlist *wl);
int zf1_pron_pass3(zf1_pron *p, zf_wordlist *wl);
int zf1_pron_pass4(zf1_pron *p, zf_wordlist *wl);
void zf1_pron_finish(zf1_pron *p, zf_wordlist *wl);
/* all passes; the fork C step runs through p->poly when set */
int zf1_pron_sentence(zf1_pron *p, zf_wordlist *wl);

/* ---- building blocks (also used by the unit tests) ---- */
/* word vt+0x88: add pronunciation (0-terminated ids) to the sorted list, make it current, set i278 = src
 * (unless src == 0x17); ignored when i278 == 20 (<pron sym>) and !force */
void zf1_word_setpron(zf_word *w, const uint16_t *ph, int src, int force);
const uint16_t *zf1_word_curpron(const zf_word *w);   /* current pron (0-terminated ids) or NULL */
/* CWordPronouncer mode 4 (FUN_1800c924c): should-spell test, CART LTS, vowel check, spelling fallback.
 * Returns 0 ok (*src = 6 LTS / 8 spelled), 1 = nothing. */
int zf1_pron_lts_word(zf1_pron *p, const zf_char *w, uint16_t *out, int cap, int *src);
int zf1_pron_lts_word2(zf1_pron *p, const zf_char *w, uint16_t *out, int cap, int *src, int vscan);
/* letter spelling FUN_18005bad4 (mode 3) / FUN_180058834 (mode 5, letters+digits only) */
int zf1_pron_spell(zf1_pron *p, const zf_char *w, uint16_t *out, int cap, int mode5);
/* CSyllableSeg: 1 = already syllabified (FUN_18004cce4); syllabify (FUN_1800cb2dc) returns count */
int zf1_pron_is_syllabified(zf1_pron *p, const uint16_t *ph, int n);
int zf1_pron_syllabify(zf1_pron *p, const uint16_t *ph, int n, uint16_t *out, int cap);
/* chars -> V/C/O class via the char table (FUN_1800cb5cc) */
int zf1_pron_charclass(zf1_pron *p, zf_char c);

/* <pron sym> (engine FUN_1800c6f58 / 1800bc5e0, see notes/fe1_pron.md): SAPI en-US phone ids (SPVSTATE.pPhoneIds,
 * 1 '-',2 '!',3 '&',4 ',',5 '.',6 '?',7 '_',8 '1',9 '2',10 aa .. 49 zh) -> engine pronunciation(s).
 * SAPI ',' '.' '?' '!' end a segment; the engine creates a punctuation word "," (type 1, i180=1) between the
 * segments' words (seg[i].comma_after = 1).  Each segment is syllabified with CSyllableSeg and must be set on its
 * word with zf1_word_setpron(w, seg.ph, 20, 1) (i278 = 20, locked).  Returns the number of segments. */
typedef struct zf1_sapiseg {
    int comma_after;
    uint16_t ph[0x182];
} zf1_sapiseg;
int zf1_pron_sapi(zf1_pron *p, const uint16_t *sapi_ids, zf1_sapiseg *seg, int maxseg);

/* internal (zf1_pron_oov.c): LTA OOV splitter (loc!18000a1d0) and COMPOUNDPRON (CWordPronouncer mode 8).
 * Return 0 = pronunciation written to out, 1 = not handled, <0 error. */
int zf1_pron_oov_split(zf1_pron *p, const zf_word *w, uint16_t *out, int cap);
int zf1_pron_compound(zf1_pron *p, const zf_char *text, uint16_t *out, int cap);

#endif
