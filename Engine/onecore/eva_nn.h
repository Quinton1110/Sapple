/* eva_nn.h - Microsoft Eva (OneCore neural voice) acoustic network: TDAT loader and forward pass.
 *
 * Reconstructed from MSTTSEngine_OneCore.dll 10.3.21207 (x64).  See notes/eva_dnn.md for the file format and the
 * engine functions each piece comes from.  Portable C99; keep -ffp-contract=off (bit-exact goal).
 *
 * The network is a CMLP: a list of layers run in order on one input row (batch size 1, PredictorBatchSize=1):
 *   DNN layer   (CMLPDNNLayer, type < 0xff):  y = act(Lin(x))        Lin = int8 (SSE3Int8Impl) or scaling (CScaling)
 *   LSTM layer  (CMLPLSTMSVDLayer, type 0x101): projected LSTM with peepholes, gate matrix factored as V*U (SVD)
 * Eva's M1033Eva.TDAT:  631 -Lin8-> 80 -Lin8,sigmoid-> 512 -LSTMP(cell 512, proj 128, rank 168)-> 128
 *                        -LSTMP(rank 56)-> 128 -LSTMP(rank 64)-> 128 -Lin8-> 508 -Scaling(std, mean)-> 508
 */
#ifndef EVA_NN_H
#define EVA_NN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MLPLinearTransform::* chosen by FUN_180124f00 from the layer's linear type */
enum { EVA_LIN_FLOAT = 1, EVA_LIN_INT16 = 2, EVA_LIN_SCALING = 3, EVA_LIN_INT8 = 4 };
/* MLPNonLinearTransform::* chosen by FUN_1801359a4 */
enum { EVA_ACT_NOOP = 1, EVA_ACT_SIGMOID = 2, EVA_ACT_TANH = 3, EVA_ACT_SOFTMAX = 4, EVA_ACT_RELU = 5 };

typedef struct {
    int type;              /* EVA_LIN_* */
    int in, out;           /* logical sizes */
    int inpad;             /* (in + 63) & ~63: the int8 dot products run over this many columns */
    int stride;            /* bytes per weight row (int8) / unused for scaling */
    const int8_t *W;       /* int8: out rows of `stride` bytes */
    const float *rowscale; /* int8: per output row, 8-byte records {float scale; float unused} */
    const float *bias;     /* out floats (int8: added after scaling; scaling: the offset) */
    const float *scale;    /* scaling: out floats */
    int8_t *q;             /* int8: quantized input scratch, inpad bytes, zero padded */
    const int16_t *W16;    /* int16 (Sarah): out rows of `stride` bytes; bias NULL when the layer has none */
    float wdeq;            /* int16: weight scale (header +0x24) / 32767.5 */
    int16_t *q16;          /* int16: quantized input scratch */
} EvaLin;

typedef struct {
    int kind;              /* 0 = DNN, 1 = LSTM (SVD) */
    int in, out;
    /* DNN */
    EvaLin lin;
    int act;
    /* LSTM (SVD): gates = V(U([x ; h_prev])) + bias; cell 512, 4 gate blocks [i f g o] */
    int cell, rank, rankpad, catpad;
    EvaLin U, V, P;        /* U: catpad -> rank, V: rankpad -> 4*cell (+bias), P: cell -> out (projection) */
    const float *peep_i, *peep_f, *peep_o; /* header offsets 0x60, 0x64, 0x68 */
    float c0, h0;          /* initial state (header 0x18 / 0x1c; 0.1 in Eva) */
    float *c, *h, *cat, *urank, *gates, *tmp, *ig, *fg, *og, *cnew, *m;
    float *y;              /* output buffer (out, padded) */
} EvaLayer;

typedef struct {
    uint8_t *file; long size; size_t maplen;
    int in, out, nlayers;
    EvaLayer *L;
} EvaNet;

/* Read a whole data file: mapped read-only on Apple / Unix systems (like zf1_dat.c), else malloc'ed.
 * *maplen != 0 means mapped.  Release with eva_unmap_file. */
uint8_t *eva_map_file(const char *path, long *size, size_t *maplen);
void eva_unmap_file(uint8_t *p, size_t maplen);

/* Load M1033Eva.TDAT.  Returns NULL on error (err gets a message). */
EvaNet *eva_net_load(const char *path, char *err, int errlen);
void eva_net_free(EvaNet *n);
/* Reset the recurrent state (CMLP slot 4 FUN_180122f90 -> LSTM slot 2 FUN_1801243c0), once per utterance. */
void eva_net_reset(EvaNet *n);
/* One step: x[in] -> y[out] (CMLP forward FUN_180122b30).  Returns a pointer to the output (owned by the net). */
const float *eva_net_forward(EvaNet *n, const float *x);

#ifdef __cplusplus
}
#endif
#endif
