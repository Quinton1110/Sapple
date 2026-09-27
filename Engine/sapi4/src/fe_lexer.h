// The front end's lexer: cuts the text into tokens (words, numbers, spaces with embedded escapes,
// punctuation, emoticons, abbreviations), each a TextSpan with a kind; and the word lists it checks
// (abbreviations, titles, common sentence-initial words).
#pragma once
#include "gptr.h"
#include "fe_small.h"

// the lexer object (thiscall)
typedef struct Lexer {
    GPTR(char) buf;         // 0x00
    int32_t end;            // 0x04
    int32_t start;          // 0x08 where the next token starts
    int32_t cur;            // 0x0c
    TextSpan pending;       // 0x10 the token found last (returned again while its kind is not 0)
} Lexer;

#define LEX_CLASS 0x637399e8        // int32 per character: its class

// @0x6369d620 stdcall: binary search of the len chars at key in a sorted table of strings (signed char
// compare; a table entry matches if it ends there); the index or -1
int32_t lex_find(const char *key, uint32_t len, GPTR(char) *table, int32_t count);
int lex_is_dash(char c);                                    // @0x6369c505 stdcall
int lex_abbrev(char *s, uint32_t len);                      // @0x6369c637 stdcall
int lex_title(const char *s, uint32_t len, int32_t upper);  // @0x6369c7ea stdcall
int lex_common(const char *s, uint32_t len);                // @0x6369c8c0 / 0x6369aa58 stdcall
int lex_emoticon(const char *s, int32_t i, int32_t *len);   // @0x6369c5bc stdcall
void lex_next(Lexer *lx, TextSpan *out);                    // @0x6369c9e6 thiscall
void lex_init(Lexer *lx, GPTR(char) buf, int32_t end);      // @0x6369c9c3 thiscall
// @0x6369c2bd: the character classes from their defaults and the configured characters (0x637399d8..e1)
void lex_classes(void);
