/*
 * The one place the engine still calls out to the layer above it.
 *
 * Stage 2 can write the phonemes it decided on to a byte list the SAPI layer
 * allocated, for ITTSDialogs to show.  That list was COM-allocated in the
 * original; here it is a tv_bytelist owned by the library, and the engine
 * only builds it when Engine.w_212c is set -- which is only while
 * tvtts_text_to_phonemes is running.  At every other time s2_bytes is NULL
 * and this does nothing, exactly as before.
 */
#include <stdlib.h>

#include "engine.h"
#include "bytelist.h"

void tv_bytelist_free(tv_bytelist *b)
{
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

/* @0x100311d0 */
void TV_THISCALL ByteList_Append(void *self, int32_t b)
{
    tv_bytelist *list = (tv_bytelist *)self;
    char *grown;
    uint32_t want;

    if (list == NULL || list->failed)
        return;
    /* Room for the byte and the terminator tvtts_text_to_phonemes adds. */
    if (list->len + 2 > list->cap) {
        want = list->cap ? list->cap * 2 : 256;
        grown = (char *)realloc(list->buf, want);
        if (grown == NULL) {
            list->failed = 1;
            return;
        }
        list->buf = grown;
        list->cap = want;
    }
    list->buf[list->len++] = (char)b;
}
