/* zf1_fstpm.h - NE FST pattern matcher (fork B): the CFstMatcher pre-pass of CNEDetector::FindNext.
 *
 * Resource bfc4309d/d2343132 (key LangDataFstNE) is a Microsoft::BingNlPlatform::Fst binary (Xerox-style compact
 * transducer, compression 0x15, license PRIVATE = position-scrambled strings) holding one compiled pmatch network
 * "[ X EndTag(sp:cat%COST=n%POS=n%NOFST=x) | ... ]".  Matching (FUN_180075900 + FUN_180067e78 iteration), as
 * verified against the engine (harness/zftap1_tn.exe "M" mode):
 *   - the text (ASCII only; otherwise the engine's narrow conversion fails and FindNext defers to the network) is
 *     scanned left to right; a match may start at i / end at j unless the two characters around the cut are both
 *     PMATCH_ALPHA or both "other" (neither alpha nor PMATCH_DELIM);
 *   - at a start, the longest valid end reached through an EndTag arc wins; every tag reaching that end is a
 *     result (same offset, same length); scanning resumes at the end of the match, else at i + 1.
 * The input side is the lower side of the labels; tags are upper-side symbols "</...>" on EPS lower sides. */
#ifndef ZF1_FSTPM_H
#define ZF1_FSTPM_H

#include <stddef.h>
#include <stdint.h>

typedef struct zf1_pm zf1_pm;

typedef struct zf1_pm_match {
    int off, len;
    const char *tag;   /* tag text without the "</" ">" delimiters, e.g. "sp:cardinal%COST=8%POS=18%NOFST=0" */
} zf1_pm_match;

zf1_pm *zf1_pm_load(const uint8_t *blob, size_t size);
void zf1_pm_free(zf1_pm *pm);

/* matches of the first matching start at or after `from` (text s[0..n), ASCII bytes); returns the number of
 * matches written to out (all with the same off and len), 0 = none */
int zf1_pm_next(zf1_pm *pm, const char *s, int n, int from, zf1_pm_match *out, int cap);

#endif
