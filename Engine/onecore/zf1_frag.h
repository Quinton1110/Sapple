/* zf1_frag.h - SAPI text -> SPVTEXTFRAG list (as sapi.dll hands it to ISpTTSEngine::Speak) -> the engine's
 * internal fragment nodes (0x88-byte nodes built by FUN_18008b6e0 / FUN_18008bb98 / FUN_18008d958).
 * Portable C99. */
#ifndef ZF1_FRAG_H
#define ZF1_FRAG_H
#include "zf1_int.h"

/* SPVTEXTFRAG + SPVSTATE fields that matter */
typedef struct zf1_sfrag {
    int action;                 /* SPVACTIONS: 0 speak, 1 silence, 2 pronounce, 3 bookmark, 4 spell, 5 section, 6 unknown tag */
    int emph, rate, vol, pitch, range, sil;
    const uint16_t *phones;     /* pPhoneIds (SAPI en-US phone ids, 0-terminated) or NULL */
    int pos;                    /* ePartOfSpeech (SPPARTOFSPEECH) */
    const zf_char *ctx;         /* Context.pCategory */
    const zf_char *text;        /* pTextStart */
    int len, ofs;               /* ulTextLen, ulTextSrcOffset */
    int group;                  /* engine call index: <voice>/<lang> start a new ISpTTSEngine::Speak call */
} zf1_sfrag;

/* engine fragment node */
typedef struct zf1_node {
    int action;                 /* +0x00: 0 speak, 1 silence, 2 pronounce, 3 bookmark, 4 spell, 5 section, 8 other */
    uint16_t lang;              /* +0x04 */
    int emph;                   /* +0x08 */
    int silence_ms;             /* +0x0c */
    const zf_char *sayas;       /* +0x20 -> {InterpretAs,...}: copy of Context.pCategory (NULL if none) */
    int has_prosody;            /* +0x28 != NULL */
    float pitch, rate, vol;     /* prosody +0x08 / +0x28 / +0x3c */
    const uint16_t *sapi_phones;/* SAPI phone ids (engine converts them into +0x30 via FUN_18008b8f8) */
    uint16_t pos;               /* +0x38 (0xFFFF none) */
    zf_char *text;              /* +0x68 (owned copy, U+200B removed for actions 0/4/6) */
    int len;                    /* +0x74 */
    int src_off;                /* +0x70 */
    int group;
} zf1_node;

typedef struct zf1_frags {
    zf_char *buf;               /* the caller's text (XML: decoded in place) */
    int n;
    zf1_sfrag *sf; int nsf;     /* SAPI fragments */
    zf1_node *nd; int nnd;      /* engine nodes (all groups) */
    void *xmlmem;
    int group_next;
} zf1_frags;

/* xml: 1 XML, 0 plain, -1 auto (XML if the text starts with '<', like SPF_DEFAULT) */
int zf1_frags_build(zf1_frags *f, const zf_char *text, int len, int xml);
void zf1_frags_free(zf1_frags *f);
/* SAPI SPPARTOFSPEECH -> engine POS id (the converter at engine vt+0x18) */
uint16_t zf1_sapi_pos(int sapipos);
#endif
