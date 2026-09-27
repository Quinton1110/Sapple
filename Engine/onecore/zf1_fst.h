/* zf1_fst.h - "TransducerNetwork" runtime of the OneCore TTS engine (portable C99).
 *
 * The TN/NE grammar (7d5841ab), the compound-pronunciation grammar (19a6569a) and the phone converter (388b0327)
 * are weighted recursive transition networks (rules = small automata whose arcs are either rule calls or
 * terminal arcs with input/output label sets).  The engine parses them with a uniform-cost agenda chart parser
 * (MSTTSEngine_OneCore.dll FUN_18000f2d4 / FUN_18000fc40, edge store FUN_180011ad0), keeps the longest (then
 * cheapest) complete top-rule edge, turns the primary derivation into a result tree (FUN_18000e440/e708,
 * FUN_1800433c8) and its leaves into text (output labels, FUN_18013ef00).  Format and algorithm: notes/fe1_tn.md.
 *
 * API: load a network once, then parse text from a start position.  Results are plain arrays.
 */
#ifndef ZF1_FST_H
#define ZF1_FST_H
#include "zf1_int.h"

typedef struct zf1_net zf1_net;

/* optional lexicon for kind-2 labels ("Any_Word", "Any_NOUN_WORD", FUN_18013897c): look the word up; return -1
 * if it is not in the lexicon, else the number of attribute lists (one per lexicon attribute set, at most
 * maxitems); list i has counts[i] (<= maxpairs) attribute pairs at pairs[i*maxpairs ...], each pair =
 * (u16 attribute id, u16 value) packed as id | value << 16, sorted ascending as the engine's lexicon returns them */
typedef int (*zf1_net_lexfn)(void *ctx, const zf_char *word, int n, uint32_t *pairs, int *counts, int maxitems,
                             int maxpairs);

zf1_net *zf1_net_load(const uint8_t *blob, size_t size);
void zf1_net_free(zf1_net *net);
void zf1_net_set_lexicon(zf1_net *net, zf1_net_lexfn fn, void *ctx);
/* network rule mask (engine: 3rd arg of FUN_180066224, 1 for the TN network) */
void zf1_net_set_mask(zf1_net *net, int mask);

/* one output leaf (a terminal arc of the primary derivation) */
typedef struct zf1_leaf {
    int rule, arc;                      /* rule index, arc index inside the rule (edge child2) */
    int start, end;                     /* input span */
    int glue;                           /* 0, 1 (attach to previous), 2 (attach to next), 3 (both): label type 3/4/5 */
    int cost;                           /* output label cost */
    const zf_char *text;                /* output text (points into the network string pool or the input) */
    int len;                            /* text length (0 = empty output) */
} zf1_leaf;

typedef struct zf1_netres {
    int found;                          /* 1 if a complete top-rule parse was found */
    int err;                            /* engine HRESULT-like error (edge store full etc.), 0 = ok */
    int start, len;                     /* parsed span */
    int cost;
    int nleaves, capleaves;
    zf1_leaf *leaves;
    /* category of the outermost category rule (rules with the +0x20 flag, e.g. "sp:date") */
    const zf_char *category;
    int catrule;
    int nattrs;
    uint32_t attrs[16];                 /* raw attribute words of that rule (low u16 = id, high u16 = value) */
    int special;                        /* 1 = lattice "special token" (/../..; syntax) path */
    int nedges;                         /* number of chart edges created (debug) */
    int nores;                          /* parse found but no consistent 1-best (MultiResult 0x80041004) */
} zf1_netres;

/* Parse text[0..len) from position start.  mode: engine "param_4" (1 = NE detection, 2 = normalization with a
 * category).  category: NULL (any) or a category name ("sp:date").  tokmode: lattice tokenizer (0 for
 * CTextNormalizer::Normalize, 3 for CNEDetector::FindNext, see zf1_lat_*).  Returns 0 on success (res->found tells
 * whether something was parsed), <0 on error. */
int zf1_net_parse(zf1_net *net, const zf_char *text, int len, int start, int mode, const zf_char *category,
                  int tokmode, zf1_netres *res);
void zf1_netres_free(zf1_netres *res);

/* network vt+0x38 (FUN_180136700): 1 if category `name` can be produced from the top rule in `mode` */
int zf1_net_category_supported(zf1_net *net, const zf_char *name, int mode);

/* output text of a result: leaf texts joined by ' ' honouring the glue flags (malloc'd, 0-terminated) */
zf_char *zf1_netres_text(const zf1_netres *res);

/* lattice helpers shared with the NE detector (mode-0 tokenizer, FUN_1801427e0 / FUN_1801425f0 / FUN_180138c84) */
int zf1_lat_start_ok(int tokmode, const zf_char *text, int len, int pos);
int zf1_lat_end_ok(int tokmode, const zf_char *text, int len, int pos);
int zf1_lat_next(int tokmode, const zf_char *text, int len, int pos);

/* debug: trace processed edges to stderr */
extern int zf1_net_trace;

/* the engine's CRT character predicates (iswspace / iswalpha of the UCRT "C" locale for UTF-16) */
int zf1_iswspace(zf_char c);
int zf1_iswalpha(zf_char c);
int zf1_iswupper(zf_char c);   /* UCRT iswupper / iswlower (e.g. U+00BA is lower without a case mapping) */
int zf1_iswlower(zf_char c);

#endif
