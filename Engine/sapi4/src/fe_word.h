// The front end's word object and the modules that work on it. The object (0xb5xx bytes, allocated
// by the front-end thread) holds the current phrase: its bounds, the rule machines' tapes (tape A at
// +0x585) and per-position tables the prosody modules fill. Fields are addressed by their offsets:
// they are plain integers, so the layout is the same in both builds.
#pragma once
#include "gptr.h"
#define W16(w, off) (*(int16_t *)(void *)((uint8_t *)(w) + (off)))
#define W8(w, off) (*(uint8_t *)((uint8_t *)(w) + (off)))
#define W32(w, off) (*(int32_t *)(void *)((uint8_t *)(w) + (off)))

// the word module's tape A pointer (its own global, set to w + 0x585)
#define WORD_TAPE 0x63739748

// @0x63697261 stdcall: the burst weight of a stop letter at position i (clamped to 0..255), 0 for others
void word_stop_weight(uint8_t *w, int16_t i, int16_t c);
// @0x6369713a stdcall: binary search among entries lo..hi of the letter-string table of width `len`
// for the letters of tape A at pos; the entry's value, or 0
int16_t word_cluster_find(int16_t lo, int16_t hi, int16_t len, int16_t pos);
// @0x63696f5f stdcall: the letter cluster starting at *pp (a pair class, then the longest listed
// cluster); moves *pp past it and returns its code
int16_t word_cluster(int16_t *pp, int16_t end);
// @0x63696dd8 stdcall: per-position duration increments of the phrase's letter clusters (w's tables
// at +0xa936, +0x9916, +0xa317); 0 if a cluster is not coded
uint8_t word_durations(uint8_t *w);
