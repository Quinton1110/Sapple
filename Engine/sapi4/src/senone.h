// Senone selection of msttssyn.dll: the decision trees (TreeImage stream) that map a phone in its
// left/right context, word position and HMM state to a senone. Layouts are the original's; fields
// not read by the decompiled code are padding.
#pragma once
#include "gptr.h"

#include "containers.h"

// a question of the trees: a row of bits over (position/state, left, right[, centre]) and its name
typedef struct SenoneQ { GPTR(uint32_t) row; GPTR(char) name; } SenoneQ;

typedef struct SenoneHeader {
    int32_t count;          // 0x00 questions
    int32_t right_base;     // 0x04 words per question row (the right-context bits start there)
    int32_t v08[5];         // 0x08 (each + 400 when read)
    GPTR(int32_t) state_q;  // 0x1c STATEID questions by state (-1: none), or 0
    int32_t bit_offset;     // 0x20 state bits before the phone bits (the number of states, if any)
    GPTR(char) name;        // 0x24
    GPTR(SenoneQ) q;        // 0x28 -> qs
    SenoneQ qs[];           // 0x2c then the rows and the names
} SenoneHeader;

// the phone set (containers.h); the trees use its first nzero phones
typedef NamedTable PhoneSet;

typedef struct TreeNode { uint16_t qlist; int16_t yes; int16_t no; int16_t _6; } TreeNode;   // yes < 0: leaf, value in no
typedef struct TreeRoot { uint16_t present; uint16_t _2; GPTR(TreeNode) nodes; } TreeRoot;

typedef struct SenoneTree {
    GPTR(const SenoneHeader) hdr;   // 0x00
    GPTR(const PhoneSet) phones;    // 0x04
    int32_t max_senone;             // 0x08
    int32_t f0c;                    // 0x0c
    int32_t total_states;           // 0x10
    int32_t ignore_position;        // 0x14
    int32_t sil;                    // 0x18 phone used for silence / "+" contexts
    int32_t word_end_right;         // 0x1c right context at a word end (< 0: keep)
    uint32_t flags;                 // 0x20 bit 1: questions also test the centre phone
    int32_t _24, _28;
    int32_t nmodels;                // 0x2c
    GPTR(const uint16_t) phone_units;  // 0x30 per phone (4 bytes each): first unit of the phone
    GPTR(GPTR(char)) model_names;   // 0x34
    GPTR(const int8_t) nstates;     // 0x38 per model
    GPTR(int8_t) tree_states;       // 0x3c per model: states with trees
    GPTR(const int8_t) model;       // 0x40 per phone
    GPTR(char) str_a;               // 0x44
    GPTR(char) str_b;               // 0x48
    GPTR(GPTR(const TreeRoot)) roots;  // 0x4c per model, per state
    int32_t nrows;                  // 0x50
    GPTR(const uint32_t) qbits;     // 0x54 question rows
    int32_t qstride;                // 0x58 words per row
    GPTR(TreeNode) nodes;           // 0x5c
    GPTR(const uint16_t) qlists;    // 0x60 question lists, 0xffff-terminated
} SenoneTree;

// @0x63673dc2 stdcall: the trees' header (questions and their rows); *out 0 when the stream has none
int32_t senone_header_load(void *stm, SenoneTree *t, GPTR(SenoneHeader) *out);
// @0x63673312 (via 0x636732fa) stdcall: the senone trees from a TreeImage stream of the storage
int32_t senone_tree_load(void *stg, const char *name, const PhoneSet *ps, GPTR(SenoneTree) *out);

// @0x63674184 stdcall: senone of phone `cur` between `left` and `right` in HMM state `state`; `pos` is
// the position in the word ('b'egin, 'e'nd, 's'ingle, 0 = middle; either case). -2 for bad arguments,
// -1 when the model has no tree for that state.
int senone_lookup(const SenoneTree *t, int cur, int left, int right, int pos, int state);

// @0x636876b2 stdcall: phone fix-ups before selection: DX -> T, and the unstressed diphthongs
// AW0 AY0 EY0 OY0 -> stressed. ph: n names of 100 bytes each.
void phone_fixups(char *ph, int n);
