// Small containers of msttssyn.dll's unit stage: a string hash table, growable arrays, a byte blob.
// Layouts are the original's.
#pragma once
#include "gptr.h"

typedef struct HashEntry { int32_t value; GPTR(const char) key; } HashEntry;
typedef struct HashTable {
    uint32_t size;          // 0x00 buckets
    int32_t count;          // 0x04
    GPTR(HashEntry) entries;  // 0x08
    int32_t lookups;        // 0x0c statistics
    int32_t probes;         // 0x10
    int32_t regrows;        // 0x14
    int32_t moved;          // 0x18
} HashTable;

// @0x63675e95 stdcall: find key (hash: sum of char << (15, 14, ... 0, 15, ...), linear probing, strcmp).
// Found: *out = value, returns 0. Not found: *out = the free bucket, returns -1 (also on NULL args).
int hash_lookup(HashTable *t, const char *key, int32_t *out);

// a phone set (the voice's PhoneFile stream): names, their indices, and what each one is made of
typedef struct NamedTable {
    GPTR(const char) name;  // 0x00 the stream's name
    GPTR(HashTable) names;  // 0x04 name -> index
    GPTR(GPTR(void)) items; // 0x08 index -> name (malloc)
    GPTR(uint8_t) base;     // 0x0c the phone it is a context variant of (itself if it is none)
    GPTR(uint8_t) left;     // 0x10 "base(left,right)ctx": left context phone (0xff: none)
    GPTR(uint8_t) right;    // 0x14 right context phone
    GPTR(uint8_t) ctx;      // 0x18 the rest's first character
    GPTR(uint8_t) nparts;   // 0x1c parts a split phone has
    GPTR(int32_t) kind;     // 0x20 0, -2 (split), ...
    int32_t count;          // 0x24
    int32_t nzero;          // 0x28 phones of kind 0
    int32_t nsplit;         // 0x2c phones of kind -2
    int32_t _30, _34;
} NamedTable;
void *calloc_n(uint32_t n, uint32_t size);          // @0x63674478 stdcall: calloc, NULL when n is 0
char *str_dup0(const char *s);                      // @0x6368021c stdcall: a copy (calloc_n)
// @0x6367fbb1 stdcall: the smallest prime >= n (n at least 2)
int32_t next_prime(int32_t n);
HashTable *hash_new(int32_t n);                                 // @0x63675db1 stdcall: room for n
int hash_add(HashTable *t, const char *key, int32_t value);     // @0x63675f19 stdcall: 0, or -1 (clash)
int hash_add_grow(HashTable *t, const char *key, int32_t value);    // @0x63675de0 stdcall: rehashing at half full
NamedTable *phoneset_new(int32_t n);                            // @0x6367fab1 stdcall
void phoneset_set(NamedTable *ps, const char *name, int32_t idx, int32_t base, int32_t kind, int32_t nparts);  // @0x6367f9d0
int32_t phoneset_kind(const NamedTable *ps, int32_t i);         // @0x6367fb66 stdcall
int32_t phoneset_nparts(const NamedTable *ps, int32_t i);       // @0x6367fb77 stdcall
// @0x6367fa1d stdcall: "a(b,c)d" split into its parts; how many there were (1-4)
int32_t phone_spec_split(const char *s, char *a, char *b, char *c, char *d);
// @0x6367f73d stdcall: the phone set from a stream of lines "name kind a base b" (see the code)
int32_t phoneset_load(void *stg, const char *name, GPTR(NamedTable) *out);
// @0x6367fb1e stdcall: index of a name, or -1
int named_index(const NamedTable *t, const char *key);
// @0x6367fb43 stdcall: item i
GPTR(void) named_item(const NamedTable *t, int i);

// @0x63685bb9 stdcall: compare the first min(strlen) bytes of a and b (memcmp sign), storing that
// length in *n; -1 at once if b is longer than a
int prefix_cmp(const char *a, const char *b, int32_t *n);

typedef struct IntVec {
    int32_t cap;            // 0x00
    int32_t count;          // 0x04
    int32_t step;           // 0x08 growth step (10)
    GPTR(int32_t) data;     // 0x0c
} IntVec;
// @0x63688151 thiscall: constructor
IntVec *intvec_ctor(IntVec *v);
// @0x63688165 thiscall: free the data (the vector itself is freed by the caller)
void intvec_free_data(IntVec *v);
// @0x63688175 thiscall: make room for n (realloc when n > cap), set the growth step (<= 0: 10)
void intvec_reserve(IntVec *v, int n, int step);

typedef struct VecArray {
    float beam;             // 0x00 prune candidates scoring above this (0: off)
    uint32_t cost_fn;       // 0x04 code address of the transition cost function (cdecl, ST0)
    GPTR(int32_t) result;   // 0x08 best path, one candidate per stage (freed by vecarray_destroy)
    int32_t count;          // 0x0c
    GPTR(GPTR(IntVec)) items;  // 0x10
} VecArray;
// @0x636881ed thiscall: constructor (the first field is a float 0.0)
VecArray *vecarray_ctor(VecArray *a);
// @0x63688202 thiscall: free every vector, the array, and extra
void vecarray_destroy(VecArray *a);
// @0x6368824c thiscall: n vectors, each reserved (cap, step); 0, or -1 when out of memory (count is
// then the number made). C++ with an exception frame in the original; nothing here throws.
int vecarray_init(VecArray *a, int n, int cap, int step);

typedef struct Blob { int32_t type; int32_t _04, _08; GPTR(void) data; int32_t bytes; } Blob;
// @0x63685814 stdcall: copy 0x50 bytes into a new blob of type 4; returns 1
int blob_copy80(const void *src, Blob *out);

// @0x636881b0 thiscall: append, growing by `step`; returns the old count
int32_t intvec_append(IntVec *v, int32_t x);
// @0x636882d6 thiscall: append x to vector i (0..count); 0, or -1 if i is out of range
int32_t vecarray_append(VecArray *a, int32_t i, int32_t x);
// @0x63685d00 cdecl: the lattice's transition cost: 0.0 between equal candidates, else 1.0 (ST0)
double lattice_cost(int32_t a, int32_t b);
// @0x63685bf7 stdcall: 0 if v occurs in a[0..n-1], else 1
int array_lacks(int16_t v, int32_t n, const int16_t *a);
// @0x636882fc thiscall: Viterbi search over the candidate vectors (one per stage) with the cost
// function (cost_fn replaced when fn is nonzero); the best path into `result`. 0, or -1 when out of
// memory.
int32_t lattice_viterbi(VecArray *a, uint32_t fn);
// a cost function that is not lattice_cost (hook build: runs the original)
double lattice_cost_other(uint32_t fn, int32_t a, int32_t b);
