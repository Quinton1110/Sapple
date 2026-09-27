/* zf1_lex.h - main lexicon (resource 7bd71f46/f9a99c02, also the small lexicons of the enUS.*.dat domain files)
 * and the phone set (29a5584b/153f1b64).  Format: notes/frontend.md 1.3.  Portable C99. */
#ifndef ZF1_LEX_H
#define ZF1_LEX_H
#include "zf1_int.h"

typedef struct zf1_huff {
    uint32_t nsym, nnodes, root;
    const uint8_t *sym;                 /* u16[nsym] */
    const uint8_t *node;                /* {u16 child0, u16 child1}[nnodes] */
    const uint8_t *words;               /* u32 LSB-first bitstream */
} zf1_huff;

#define ZF1_LEX_MAXPH   0x182           /* engine limit for one pronunciation (phone ids incl. markers) */
#define ZF1_LEX_MAXPRON 16
#define ZF1_LEX_MAXATTR 16

typedef struct zf1_lexpron {
    int nph;
    uint16_t ph[ZF1_LEX_MAXPH];         /* phone ids (phone set ids: 2 '-', 5 '1', 6 '2', 7.. phones) */
    int has_val;
    uint16_t val;                       /* record type 2 (codec2 symbol; 0x6000 seen) stored at pron+0x50 */
    int nattr;
    uint16_t attrset[ZF1_LEX_MAXATTR];  /* attribute-set indices k (1-based as stored) */
} zf1_lexpron;

typedef struct zf1_lexent {
    int n;
    zf1_lexpron p[ZF1_LEX_MAXPRON];
} zf1_lexent;

typedef struct zf1_lex {
    const uint8_t *L;                   /* resource start */
    size_t size;
    uint32_t nslots, bits;
    const uint8_t *hash;                /* packed MSB-first slot table */
    zf1_huff c[4];                      /* codec0 chars, codec1 phones, codec2 val, codec3 attrset ids */
    const uint8_t *words;               /* main bitstream */
    size_t nwords;
    int nattrnames;
    char **attrname;                    /* attribute names (decoded from codec4), index = attr id */
    int nsets;
    const uint8_t *setstart;            /* u32[nsets] index into the u16 id list */
    const uint8_t *ids;                 /* u16 id lists, 0-terminated */
    const uint8_t *attrdefs;            /* 10-byte records {u16 id, u16 isValue, u16 parent, u16 nameBitOff, u16 0} */
    int nattrdefs;
} zf1_lex;

int zf1_lex_init(zf1_lex *lx, const uint8_t *res, size_t size);   /* 0 ok */
void zf1_lex_free(zf1_lex *lx);
/* Look up word (n code units, <=128 used) -> 1 found (entry decoded into *out), 0 not found.
 * Same as FUN_180034f50 + FUN_180035180 + FUN_180044304: lower-case, hash, probe, compare ignoring case. */
int zf1_lex_lookup(const zf1_lex *lx, const zf_char *word, int n, zf1_lexent *out);
/* attribute ids (0-terminated list) of attribute set k (1-based, as stored in zf1_lexpron.attrset) */
const uint8_t *zf1_lex_attrset(const zf1_lex *lx, int k);   /* returns pointer to u16 LE list, ends with 0 */
const char *zf1_lex_attrname(const zf1_lex *lx, int id);

/* ---- phone set ---- */
typedef struct zf1_phoneset {
    int n;
    uint16_t id[64];
    char name[64][24];
    uint32_t flags[64];
    int byid[128];                      /* id -> index or -1 */
} zf1_phoneset;
int zf1_phoneset_init(zf1_phoneset *ps, const uint8_t *res, size_t size);
const char *zf1_phone_name(const zf1_phoneset *ps, int id);    /* NULL if unknown */
int zf1_phone_id(const zf1_phoneset *ps, const char *name, int n); /* case-insensitive; -1 if unknown */
/* phone-id string -> "H EH - L OW 1" (names upper-case as in the phone set, ' ' separated) */
zf_char *zf1_phones_to_str(const zf1_phoneset *ps, const uint16_t *ph, int n);

#endif
