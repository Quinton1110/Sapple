/* zf1_int.h - internal helpers shared by the zf1_* modules (text -> words with pronunciations).
 * Portable C99.  Component headers: zf1_lex.h (lexicon/LTS/morph/pronouncer), zf1_tn.h (NE + text
 * normalization transducer), zf1_pos.h (POS tagger, CRF, polyphony).  See notes/fe1.md. */
#ifndef ZF1_INT_H
#define ZF1_INT_H

#include "zf.h"
#include <stdint.h>
#include <stddef.h>

/* ---------------- little-endian readers (the .dat is LE; never cast unaligned pointers) ---------------- */
static inline uint16_t zf_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t zf_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t zf_rd64(const uint8_t *p) { return zf_rd32(p) | ((uint64_t)zf_rd32(p + 4) << 32); }

/* ---------------- .dat container (zf1_dat.c) ----------------
 * chunk = GUID type; GUID id; u64 size; data; next chunk at (data+size+7)&~7.  Resources are looked up by the
 * first dword (Data1) of the type and id GUIDs, as printed in notes/frontend.md 1.2 (e.g. 0x7bd71f46/0xf9a99c02). */
typedef struct zf1_res {
    uint32_t type1, id1;                /* Data1 of the GUIDs */
    uint8_t type[16], id[16];           /* full GUIDs (little-endian bytes as stored) */
    const uint8_t *p;                   /* payload (8-byte aligned inside the file buffer) */
    size_t n;                           /* payload size */
} zf1_res;

typedef struct zf1_dat {
    uint8_t *buf;                       /* whole file (malloc, 8-byte aligned) */
    size_t size;
    size_t maplen;                      /* ClassicVoices patch: nonzero = buf is a read-only mmap of this length */
    int nres;
    zf1_res res[64];
} zf1_dat;

int zf1_dat_load(zf1_dat *d, const char *path);     /* 0 ok, <0 error */
/* ClassicVoices patch: the domain .dat files of a non-en-US locale (en-GB: MSTTSLocEnGB.INI [Domain], in its order,
 * named as they are on disk - "EnGB.Address.dat" but "enGB.Name.dat").  Returns the count and sets *tab, or 0 for
 * en-US and unknown locales (the en-US callers keep the upstream's own lists). */
typedef struct { const char *domain; const char *file; } zf1_domfile;
int zf1_domain_files(int lcid, const zf1_domfile **tab);
/* zf1_dat_load, then (file systems that match case) again with the first letter of the file name's case flipped */
int zf1_dat_load_ci(zf1_dat *d, char *path);
void zf1_dat_free(zf1_dat *d);
/* returns payload or NULL (= engine HRESULT 0x8004801A "not present") */
const uint8_t *zf1_dat_get(const zf1_dat *d, uint32_t type1, uint32_t id1, size_t *size);

/* ---------------- UTF-16 string helpers (zf1_util.c) ---------------- */
size_t zf_strlen(const zf_char *s);
zf_char *zf_strdup(const zf_char *s);                    /* NULL -> NULL */
zf_char *zf_strndup(const zf_char *s, size_t n);
zf_char *zf_strdup_a(const char *ascii);                 /* ASCII/Latin-1 -> UTF-16 */
int zf_strcmp(const zf_char *a, const zf_char *b);       /* NULL == "" */
int zf_strcmp_a(const zf_char *a, const char *ascii);
void zf_setstr(zf_char **dst, const zf_char *src);       /* free old, dup new ("" -> NULL) */
void zf_setstr_a(zf_char **dst, const char *ascii);
/* UTF-8 <-> UTF-16 (malloc'd, 0-terminated; *outlen optional) */
zf_char *zf_from_utf8(const char *s, int n, int *outlen);
char *zf_to_utf8(const zf_char *s, int n);

/* Case mapping.  The engine uses CharLowerW / CharUpperW / LCMapStringW(0x409) (Windows NLS tables).  The
 * portable versions below cover ASCII + Latin-1 + Latin Extended-A + Greek + Cyrillic with the Windows results;
 * anything outside those ranges is returned unchanged (documented limitation). */
zf_char zf_tolower(zf_char c);
zf_char zf_toupper(zf_char c);

/* ---------------- word helpers (zf1_util.c) ---------------- */
void zf_word_init(zf_word *w);                /* engine CTTSWord ctor defaults (i180=8? no: 0; bi=2 ...) */
void zf_word_free(zf_word *w);                /* frees owned strings */
void zf_word_copy(zf_word *dst, const zf_word *src);   /* deep copy */

/* growable word array used during analysis (the engine's A+0x420 linked list) */
typedef struct zf_wordlist {
    zf_word *w;
    int n, cap;
} zf_wordlist;
zf_word *zf_wl_insert(zf_wordlist *l, int at);   /* returns new zeroed/initialised slot */
zf_word *zf_wl_push(zf_wordlist *l);
void zf_wl_remove(zf_wordlist *l, int at);        /* frees the word */
void zf_wl_free(zf_wordlist *l);

#endif
