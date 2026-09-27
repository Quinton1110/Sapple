/*
 * The byte list Stage 2 writes the phoneme trace to.
 *
 * When Engine.w_212c is set, Stage 2 reports every phoneme it settles on by
 * calling ByteList_Append.  In the original the list is COM-allocated and
 * belongs to the SAPI layer, which showed it through ITTSDialogs; here it is
 * this, so tvtts_text_to_phonemes can read the trace back out.
 *
 * Nothing in the engine reads the list, so collecting it cannot change what
 * is synthesised -- w_212c guards the calls and nothing else.
 */
#ifndef TV_BYTELIST_H
#define TV_BYTELIST_H

#include <stdint.h>

typedef struct {
    char    *buf;
    uint32_t len;
    uint32_t cap;
    int      failed;   /* an allocation failed; buf holds a prefix only */
} tv_bytelist;

/* Releases the buffer and leaves the list empty; safe on a zeroed list. */
void tv_bytelist_free(tv_bytelist *b);

#endif
