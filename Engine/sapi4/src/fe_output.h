// The front end's per-word-object entry points (the text thread drives them by index, 1..128): run
// rule machines C and B over a new segment, run machine A's phrase driver, and format the result as
// the string the unit stage reads.
#pragma once
#include "gptr.h"

#define FE_MAGIC 0x63738bd4             // int32: 0xffff9bad once the front end is set up
#define FE_WOBJS 0x63739e9c             // the word objects by index (pointer per index)

int32_t fe_c_run(uint8_t *w);           // @0x63696cd4 stdcall: machine C (lexicon, letter to sound)
int32_t fe_b_run(uint8_t *w);           // @0x6369378d stdcall: machine B (phrasing)
int32_t fe_prepare(uint8_t *w);         // @0x6368b221 cdecl: both, then state 2
int32_t fe_word_prepare(int32_t idx);   // @0x63688d0a cdecl
// @0x63688d9c cdecl: phrase driver, then the output string (malloc'd, into *out); its length
int32_t fe_word_format(int32_t idx, GPTR(char) *out);
int32_t fe_event_count(uint8_t *w);     // @0x6368b17a cdecl
int32_t fe_word_event_count(int32_t idx);   // @0x63688964 cdecl
int32_t fe_events(uint8_t *w, int32_t *dst);    // @0x6368b198 cdecl: the events, 6 dwords each
int32_t fe_word_events(int32_t idx, int32_t *dst);  // @0x6368899b cdecl
