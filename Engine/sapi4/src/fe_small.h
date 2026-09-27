// Small helpers of the text front end (accessors of its C++ classes, allocator glue). Names describe
// what the code does; the classes they belong to are only partly identified.
#pragma once
#include "gptr.h"

// the text cursor class: a buffer, a position and a length
typedef struct TextSpan {
    GPTR(char) buf;         // 0x00
    int32_t f4;             // 0x04
    int32_t pos;            // 0x08
    int32_t len;            // 0x0c
    int32_t f10;            // 0x10
} TextSpan;

int32_t span_f4(const TextSpan *s);                 // @0x6369d3da thiscall
int32_t span_pos(const TextSpan *s);                // @0x6369d3e9
int32_t span_len(const TextSpan *s);                // @0x6369d3ed
int32_t span_f10(const TextSpan *s);                // @0x6369d41a
char *span_cur(const TextSpan *s);                  // @0x6369d3f1 buf + pos
char span_first(const TextSpan *s);                 // @0x6369d3f7 buf[pos] (in AL)
char span_last(const TextSpan *s);                  // @0x6369d400 buf[pos + len - 1] (in AL)
TextSpan *span_set_pos(TextSpan *s, int32_t v);     // @0x6369d3a0 returns s
TextSpan *span_set_len(TextSpan *s, int32_t v);     // @0x6369d3ac
TextSpan *span_set_f10(TextSpan *s, int32_t v);     // @0x6369d3b8
TextSpan *span_clear(TextSpan *s);                  // @0x6369d3c4 all five fields 0
TextSpan *span_pair_reset(TextSpan *s);             // @0x6369d013 pos = len, then clear the span after it (+0x10)

typedef struct Pair { int32_t a, b; } Pair;
void pair_reset(Pair *p);                           // @0x6369d5a0 a = 0, b = -1
typedef struct Block8 { int32_t v[8]; } Block8;
void block8_clear(Block8 *b);                       // @0x6369d258 eight fields 0

int no_op_false8(void);                             // @0x6369397f stdcall(2 args): AL = 0 (the rest of EAX untouched)
int no_op_zero8(void);                              // @0x6369d1d1 stdcall(2 args): 0
void free_cdecl(void *p);                           // @0x63677039 cdecl free
int free_if(void *p);                               // @0x636748ba stdcall: free unless NULL, returns 0
int is_digit_char(char c);                          // @0x63682302 stdcall: '0'..'9' (signed char compare)
uint16_t char_class16(uint16_t c);                  // @0x6369765f stdcall: a 16-bit table in .rdata (0x636c8080)
typedef struct FreeNode { uint8_t _00[0x18]; GPTR(struct FreeNode) next; } FreeNode;
void freelist_push(FreeNode *n);                    // @0x6368ac2f stdcall: onto the global list at 0x63738c9c
