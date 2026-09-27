// The front end's rule modules. The DLL contains ten or so modules generated from the same templates
// (a multi-tape rewriting machine: four byte tapes shared by all modules, per-module state in .data),
// so one C function serves every instance; the instance is identified by its globals.
#pragma once
#include "gptr.h"

// the four shared tapes (global char pointers in .data)
#define TAPE_A 0x63739e7c
#define TAPE_B 0x63739e6c
#define TAPE_C 0x63739e78
#define TAPE_D 0x63739e70

// @0x63697558 (and 8 identical copies) stdcall: 1 if tape A[lo..hi] are all graphic (0x21..0xfe)
int tapes_graphic(int16_t lo, int16_t hi);
// @0x63697583 (and 9 more instances, one per module) stdcall: pad the four tapes with '~' up to the
// furthest of their ends (ends: the module's int16[4]) and a; returns that position. (Its second
// argument is overwritten before use.)
int16_t tapes_pad(const int16_t *ends, int16_t a);

// the key buffer the modules' table searches compare against (bytes at 0x63739e40)
#define RULE_KEY 0x63739e40
// @0x63697b42 (and 4 more instances, each with its own sorted table) stdcall: binary search for the
// key (width bytes at RULE_KEY) among entries lo..hi-1 of a table of width-byte records starting at
// record offset base; the entry index in AL, or 0xff if the key is not there. (Callers use AL only.)
uint8_t rule_bsearch(const uint8_t *table, uint32_t base, uint8_t lo, uint8_t hi, uint8_t width);
