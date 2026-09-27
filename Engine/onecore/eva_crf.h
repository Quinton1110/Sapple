/* eva_crf.h - Eva's statistical prosody models (M1033Eva.BR2 / .TON / .ACL): CRF model loader and decoder.
 *
 * Format and semantics from MSTTSLoc_OneCore.dll (Windows 11 26100.1742 copy), see notes/eva_prosody.md:
 *   CCRFModelManager parse FUN_180033c80 (header, labels, templates FUN_18000b550, CVocabTrie FUN_180055440,
 *   weights, transition matrix), feature strings FUN_180004ea0, taggers CCRFBreakTaggerImpl (FUN_18002fe90),
 *   CCRFBoundaryToneTaggerImpl (FUN_18005c2b0), CCRFPitchAccentLocTaggerImpl (FUN_180067360 + FUN_180067590). */
#ifndef EVA_CRF_H
#define EVA_CRF_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EvaCrf EvaCrf;

/* Load one CRF model file.  NULL on error (message in err). */
EvaCrf *eva_crf_load(const char *path, char *err, int errlen);
void eva_crf_free(EvaCrf *m);
int eva_crf_nlabels(const EvaCrf *m);
const char *eva_crf_label(const EvaCrf *m, int i);
int eva_crf_ntemplates(const EvaCrf *m);
const char *eva_crf_template(const EvaCrf *m, int i);   /* "U05:Word.PrevWord.RegularText/..." (ASCII) */
int eva_crf_nfeatures(const EvaCrf *m);
/* feature string ("U01:state") -> feature id, or -1 */
int eva_crf_lookup(const EvaCrf *m, const char *s);
/* Viterbi over T tokens: feats[t*ntempl + k] = feature id of template k at token t (-1 = none);
 * labels[t] receives the best label index. */
void eva_crf_decode(const EvaCrf *m, const int *feats, int T, int *labels);
/* self-check of the feature dictionary: every word's rank equals its depth-first position; returns the mismatches */
long eva_crf_selfcheck(const EvaCrf *m, long *count);

/* ---- the three taggers, applied to a zf2 sentence (Z2Sent, see zf2_int.h) ---- */
typedef struct {
    EvaCrf *br2, *ton, *acl;
    int verbose;           /* print the per-word labels to stderr (not in CV_NO_DEBUG_ENV builds) */
    int off;               /* debug: models off, the rules decide (A/B) */
} EvaProsody;
/* stage 1 = after the break rules, 2 = after the boundary-tone rules, 3 = after the pitch-accent rules */
void eva_prosody_hook(void *ctx, void *z2sent, int stage);

#ifdef __cplusplus
}
#endif
#endif
