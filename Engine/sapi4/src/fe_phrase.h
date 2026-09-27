// Machine A's per-phrase driver (rule set runs, then the duration and pitch heuristics over the word
// object) and its helpers. The word object's fields are addressed by offset (fe_word.h).
#pragma once
#include "gptr.h"
#include "fe_small.h"

// the phrase event list (nodes of 0x1c bytes, allocated 32 at a time and recycled through the free
// list at 0x63738c9c)
typedef struct EvNode {
    int32_t kind;           // 1, 2, 4 (8: ...)
    int32_t a, b;           // 0x04, 0x08
    int32_t pos;            // 0x0c
    int32_t len;            // 0x10
    int16_t ch;             // 0x14
    int16_t _16;
    GPTR(struct EvNode) next;   // 0x18
} EvNode;
typedef GPTR(EvNode) EvLink;      // a link as stored (in the hook build a 32-bit guest address)

// the word object's pointer fields (event list head 0xb4ac and tail 0xb4b0): 32-bit slots in the hook
// build; in a portable build the object carries them in an extension past its end
#ifdef DECOMP_HOOK
#define WPTR(w, off) (*(GPTR(EvNode) *)(void *)((w) + (off)))
#else
#define WOBJ_PTRS 0xc000
#define WPTR(w, off) (((EvNode **)(void *)((w) + WOBJ_PTRS))[((off) - 0xb4ac) / 4])
#endif

EvNode *ev_alloc(void);                                 // @0x6368aca2
void ev_clear(uint8_t *w);                              // @0x6368abe3 stdcall
int32_t ev_place(uint8_t *w);                           // @0x6368af3e stdcall: events onto phrase positions

uint8_t ph_near_dash(const uint8_t *t, int32_t lo, int32_t hi, int32_t pos);    // @0x6368b87c stdcall
void ph_stress_marks(const uint8_t *t0, uint8_t *t2, int16_t lo, int16_t hi, uint8_t flag);  // @0x6368b7b1
void ph_marks(int16_t lo, int16_t hi, uint8_t flag);   // @0x6368b8e1 stdcall
uint8_t ph_vowel_factor(int32_t pos, int32_t *out);     // @0x6368c177 stdcall
void ph_final_factor(int32_t pos, int32_t *out);        // @0x6368c367 stdcall
uint8_t ph_cluster_factor(int32_t pos, int32_t *out);   // @0x6368c1e3 stdcall
void ph_split(int32_t pos);                             // @0x6368c3dc stdcall
void ph_durations(int16_t lo, int16_t hi, uint8_t flag);   // @0x6368bab2 stdcall
void ph_segments(int16_t lo, int16_t hi, uint8_t flag);    // @0x6368c6f1 stdcall
void ph_target(int32_t k, int32_t t);                   // @0x6368fed5 stdcall
void ph_insert_tag(int32_t k, int32_t j);               // @0x63690187 stdcall
void ph_pitch(uint8_t *w, int32_t lo, int32_t hi);      // @0x6368f732 stdcall
int32_t ph_run(uint8_t *w);                             // @0x6368c49b cdecl
