#ifndef ANNA_INTERNAL_H
#define ANNA_INTERNAL_H

#include "anna.h"

typedef struct {
    unsigned char f[9]; /* candidate-side target features */
    unsigned char join; /* may continue its corpus predecessor */
    int32_t len;        /* samples */
    int32_t start;      /* first sample in the decoded recording */
    uint32_t wih_off;   /* pitch-epoch bytes in the .WIH file (after its 16-byte header) */
    uint16_t wih_n;
} anna_unit;

typedef struct {
    int feat;
    uint64_t mask; /* allowed feature values */
} anna_question;

typedef struct {
    int leaf;
    /* internal */
    int nexpr, root, yes, no;
    const unsigned char *expr; /* nexpr x {u16 op, u16 flags, i32 a, i32 b} */
    /* leaf */
    int lo, nbits;
    const unsigned char *bits;
} anna_node;

/* the engine's WSOLA object (lives as long as the voice): FUN_081a37c8 / FUN_081a39c8 and its core */
typedef struct {
    double factor;     /* current rate factor (0 = never set) */
    int ha;            /* analysis hop trunc(160 * factor) */
    int started;       /* this+4 */
    int cc;            /* this+0x10: carried samples; negative = samples of the next input to skip */
    int16_t carry[440];
    double tail[80];   /* core+0x28 */
    int k;             /* core+0x1c */
} anna_wsola;

struct anna_voice {
    anna_wsola ws;
    anna_decoder *dec;
    long nunits;
    anna_unit *units;
    long type0[262]; /* first unit of each type; [261] = nunits */
    /* APL */
    float *tab[9];
    int dim[9];
    float tw, jw, jt[24];
    /* CRT */
    unsigned char *crt;
    anna_question *qs;
    int nq;
    anna_node *nodes;
    int nnodes, capnodes;
    int tree[262];
    int default_rate; /* the token's DefaultRate (2 for Anna) */
    void *wih;        /* FILE* of the .WIH pitch-epoch file (pitch changes only) */
    double *hann;     /* 5000-point window of the pitch changer (read from MSTTSEngine.dll), or NULL */
};

#endif
