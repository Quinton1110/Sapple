/* zf1_pos.h - POS tagging (CPOSTaggerImpl / CPosTagger, a Brill transformation-based tagger), CPolyphony context
 * pronunciation rules and the homograph CRF taggers (CPolyphonyHandler).  Portable C99.  See notes/fe1_pos.md.
 *
 * Call order inside the zf1 analyzer (engine CSentenceAnalyzer::Analyze 0x180065980 / CPronouncer 0x1800479e0):
 *   Analyze step 5  : zf1_pos_tag(p, wl, 0, wl->n)            (all tokens of the sentence, before TN)
 *   Analyze step 7  : for every TN expansion of one token (0x180018ed4): the new words are tagged on their own:
 *                     if the expansion has exactly 1 word it first gets pos = the original token's pos
 *                     (zf1_pos_set(p, w, orig->pos)); then zf1_pos_tag(p, expansion_list, 0, n).
 *   CPronouncer     : after lexicon/LTS (fork A):  zf1_poly_apply(p, wl)  (CPolyphony rules, then CRF homographs)
 */
#ifndef ZF1_POS_H
#define ZF1_POS_H
#include "zf1_int.h"
#include "zf1_lex.h"

/* POS ids = attribute ids of the lexicon attribute tree (the 22 "main" POS of resource f81fd1d1/ed1348b2) */
enum {
    ZF_POS_NOUN = 3, ZF_POS_VERB = 6, ZF_POS_VERBING = 7, ZF_POS_VERBPASTP = 8, ZF_POS_ADJ = 11, ZF_POS_DET = 14,
    ZF_POS_WHDET = 15, ZF_POS_NUM = 18, ZF_POS_VAUX = 21, ZF_POS_PRON = 24, ZF_POS_WHPRON = 25, ZF_POS_ADV = 28,
    ZF_POS_WHADV = 29, ZF_POS_CONJ = 32, ZF_POS_PREP = 35, ZF_POS_INTERJ = 38, ZF_POS_PARTICLE = 41,
    ZF_POS_SYMBOL = 44, ZF_POS_LISTITEM = 47, ZF_POS_POSSEND = 48, ZF_POS_CONTR = 49, ZF_POS_UNKNOWN = 50,
    ZF_POS_NONE = 0xFFFF
};

typedef struct zf1_brill_rule {
    uint16_t kind;                      /* template 0x101.. (lexical) / 0x201.. (contextual) */
    uint16_t from, to;                  /* POS (from 0 = any, lexical rules only) */
    int16_t arg;                        /* tag argument (-2 = sentence boundary) or split position */
    const uint8_t *str;                 /* UTF-16LE string in the pool (NULL if strOff == 0) */
} zf1_brill_rule;

/* Container-lexicon lookup (CTTSContainerLexicon vt+8 = 0x18001d850: main lexicon "LEX", then the priority list
 * incl. the CEnMorph morph lexicon of the Loc DLL).  Returns -1 if not found, else the number of u16 written to
 * sets: the entry's attribute sets (entry vt+0x28/0x30) in order, each as its attribute ids terminated by 0
 * (e.g. "read" -> 4,6,0, 1,3,0, 4,8,0; a morph compound like "widescreen" -> 50,0).  Default = main lexicon only;
 * the integrator (fork A's CEnMorph) installs the full container lookup with zf1_pos_set_lookup. */
typedef int (*zf1_lexlookup_fn)(void *ctx, const zf_char *w, int n, uint16_t *sets, int cap);

typedef struct zf1_pos {
    const zf1_lex *lx;
    zf1_lexlookup_fn lookup;
    void *lookup_ctx;
    /* optional: attribute sets of the lexicon entry already attached to a word (word vt+0x258, e.g. the domain
     * lexicon entry TN attached to its output words).  Return -1 if the word has none -> text lookup is used. */
    int (*word_sets)(void *ctx, const zf_word *w, uint16_t *sets, int cap);
    void *word_sets_ctx;
    const zf1_dat *dat;
    int nlex, nctx;
    zf1_brill_rule *lexr, *ctxr;
    /* POS table: main POS for every attribute id (0xFFFF if the id has no main POS) */
    int nattr;
    uint16_t mainpos[256];
    uint8_t ismain[256];                /* id itself is one of the 22 main POS */
    uint16_t unknown;                   /* 50 */
    void *poly;                         /* CPolyphony / CRF data (zf1_poly.c) */
    /* optional trace of every homograph CRF evaluation (word index, p(best) = the engine's word+0x320, label) */
    void (*crf_trace)(void *ctx, int wordidx, float prob, int label);
    void *crf_trace_ctx;
} zf1_pos;

int zf1_pos_init(zf1_pos *p, const zf1_dat *d, const zf1_lex *lx);   /* 0 ok */
void zf1_pos_free(zf1_pos *p);
void zf1_pos_set_lookup(zf1_pos *p, zf1_lexlookup_fn fn, void *ctx);

/* Tag words [from, to) of wl (CPOSTaggerImpl::Tag 0x1800569d0 with the OOV flag off).  Reads type, text, pos
 * (a preset pos other than 0xFFFF/50 fixes the word), writes pos (+0x138) and tpos (+0x13a). */
int zf1_pos_tag(zf1_pos *p, zf_wordlist *wl, int from, int to);
/* word vt+0xe8: pos + tpos (= main POS of pos, 50 if unknown to the POS table) */
void zf1_pos_set(const zf1_pos *p, zf_word *w, uint16_t pos);
/* main POS of an attribute id (POS table vt+0x20) */
uint16_t zf1_pos_main(const zf1_pos *p, uint16_t id);
/* candidate POS list of a word from the lexicon, in engine order (FUN_180067f40 + 1800c1dc8); returns count */
int zf1_pos_candidates(const zf1_pos *p, const zf_char *text, int n, uint16_t *out, int cap);

/* ---- low level (exported for the unit tests) ---- */
typedef struct zf1_posnode {
    const zf_char *text;                /* word text (NULL = L"") */
    uint16_t pos;                       /* +0x08 */
    int kind;                           /* +0x0c: 0 new, 1 ambiguous, 2 unknown, 3 fixed */
    int ncand;                          /* +0x18 converted candidates (+0x10) */
    uint16_t cand[32];
    int nlexc;                          /* +0x38 lexicon candidates (+0x30) */
    uint16_t lexc[32];
} zf1_posnode;
/* CPosTagger vt+0 (0x180062190) on a node chain */
void zf1_pos_run(const zf1_pos *p, zf1_posnode *nodes, int n);

/* RegularText (+0xb8) of the committed sentence words (0x18001bf24, called from 0x18005c794 right after Analyze):
 * lower-case Text; if it contains "'" keep the part from the last "'" ("doesn't" -> "'t"); else by POS category
 * (NOM / ADJ / VER) replace a matching suffix class from resource 5554ba64 ("implementation" -> "-tion"). */
int zf1_pos_regular(const zf1_pos *p, zf_wordlist *wl, int from, int to);

/* ---- zf1_poly.c ---- */
/* CPronouncer steps 3+4: CPolyphony rules (0x1800c9e30) over all words, then the homograph CRFs (0x180060158).
 * Must run after the lexicon/LTS pronunciations are set (fork A) and before the final pron pass (0x1800563dc).
 * Reads: type, text, ci, pos, fs.action, i278, ne_type, src_off/len, prons/cur_pron of all words.
 * Writes: prons/cur_pron/pron (selected or inserted, sorted), i278 = 13 (rule) / 22 (CRF),
 *         prev_chars/next_chars (words that have a CRF model). */
int zf1_poly_apply(zf1_pos *p, zf_wordlist *wl);
int zf1_poly_rules(zf1_pos *p, zf_wordlist *wl);   /* CPolyphony only */
int zf1_poly_crf(zf1_pos *p, zf_wordlist *wl);     /* CRF only */
void zf1_poly_free(zf1_pos *p);
/* Domain data (call once after zf1_pos_init): loads enUS.<Domain>.dat from datdir (NULL = the Windows OneCore en-US
 * folder) for address/name/message/media/computer/companyname: their CPolyphony tables (used first for words whose
 * domain +0x1b0 matches, pron source 14) and the acronym CRF list (enUS.Address.dat cea1be6f/0fbd602c: SR, ST., ST).
 * Returns the number of domain files loaded. */
int zf1_acro_init(zf1_pos *p, const char *datdir);
/* ClassicVoices patch: the same for a locale (1033 = zf1_acro_init; 2057: the en-GB domain files) */
int zf1_acro_init_lcid(zf1_pos *p, const char *datdir, int lcid);
/* Analyze step 7a (CAcronymHandler, FUN_180079408), after the domain step and before TN normalization:
 * entity tokens are re-broken with the word breaker, each token whose domain has an acronym model named by its
 * upper-cased text is scored; labels with p > 0.6 replace the token text inside the entity Text ("St." ->
 * "street"), or the Text of a plain word (split at ' ' into several words).  Updates text and ci. */
struct zf1_wb;
int zf1_acro_apply(zf1_pos *p, zf_wordlist *wl, const struct zf1_wb *wb);
/* word vt+0x88: select/insert pronunciation ids[n] (sorted u16 list), make it current, pron string = names;
 * source -> i278 unless 0x17; ignored when i278 == 0x14 */
void zf1_pron_set(const zf1_phoneset *ps, zf_word *w, const uint16_t *ids, int n, int source);

#endif
