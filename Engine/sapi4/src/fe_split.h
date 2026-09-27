// The sentence splitter of the text input: runs the lexer over the text and cuts it into sentences,
// following brackets and quotes, abbreviations, initials, numbers and the words that usually follow a
// full stop, and counts the words of each sentence.
#pragma once
#include "gptr.h"
#include "fe_small.h"
#include "fe_lexer.h"

// the lexer with a saved copy of itself (tried ahead, then rewound)
typedef struct SplitLexer {
    Lexer lx;               // 0x00
    Lexer saved;            // 0x24
    int32_t has_saved;      // 0x48
} SplitLexer;

// open brackets: up to 50, each its kind (0 round, 1 curly, 2 square) and the token position
typedef struct Brackets {
    int32_t overflow;       // 0x00
    int32_t top;            // 0x04 -1 when empty
    int32_t kind[50];       // 0x08
    int32_t pos[50];        // 0xd0
} Brackets;

// open quotes and parentheses-like marks (class 0xc opens, 0xd closes)
typedef struct Quotes {
    int32_t dq;             // 0x00 open double quotes
    int32_t sq;             // 0x04 open single quotes
    int32_t possessive;     // 0x08 apostrophes after a plural s
    int32_t sq_word;        // 0x0c single quotes opened in a number
    int32_t marks;          // 0x10 open class-0xc marks
    int32_t sq_pos;         // 0x14 where the first open one started
    int32_t dq_pos;         // 0x18
    int32_t marks_pos;      // 0x1c
} Quotes;

// the text to split
typedef struct SplitText {
    int32_t total;          // 0x00 end offset
    int32_t start;          // 0x04 where to begin
    GPTR(char) base;        // 0x08
} SplitText;

// one sentence found
typedef struct Sentence {
    int32_t start;          // 0x00 offset from base
    int32_t len;            // 0x04 without trailing spaces
    int32_t raw_len;        // 0x08
    int32_t chars;          // 0x0c characters outside spaces and line breaks
    int32_t words;          // 0x10
    int32_t word_chars;     // 0x14
    int32_t word_f10;       // 0x18 sum of the words' third field
    int32_t after_semicolon;// 0x1c words starting a clause after ';'
    int32_t flag;           // 0x20
} Sentence;

void quotes_track(Quotes *q, const TextSpan *t, int32_t open, int32_t possessive, int32_t in_word); // @0x6369d0b6
int quotes_open(const Quotes *q);                                   // @0x6369d171 thiscall
int quotes_apostrophe(Quotes *q, const TextSpan *t);                // @0x6369d20c thiscall
void quotes_clamp(Quotes *q);                                       // @0x6369c248 stdcall(4), the 2nd
void brackets_update(Brackets *b, const TextSpan *t, const TextSpan *prev, char *closer); // @0x6369d4bc
int brackets_peek(const Brackets *b, int32_t *pos);                 // @0x6369d580 thiscall
void brackets_classify(const TextSpan *t, int32_t *kind, int32_t *dir); // @0x6369d5a8 stdcall
TextSpan *span_set_kind(TextSpan *s, int32_t kind);                 // @0x6369d379 thiscall
int span_differs(const TextSpan *a, const TextSpan *b);             // @0x6369d47b thiscall
char span_before_last(const TextSpan *s);                           // @0x6369d40d thiscall
int word_not_roman(const TextSpan *t);                              // @0x6369c0a2 stdcall
int word_initials(const TextSpan *t);                               // @0x6369c169 stdcall
int word_is_number(TextSpan t);                                     // @0x6369c1db stdcall, by value
int word_number_name(const char *s, uint32_t n);                    // @0x6369c047 stdcall
int word_equals(const char *ref, const char *s, int32_t n);         // @0x6369c287 stdcall
// @0x6369aaa4 stdcall(6): up to max sentences into out; *count found, *eot set when a line break or the
// end of the text ended one;
// 0, or 5 when out is full before the text ends
int32_t split_sentences(int32_t unused, SplitText *in, int32_t max, Sentence *out, int32_t *count,
                        int32_t *eot);
// @0x6367f5ef stdcall: the text after txt_rewrite, cut into sentences, each followed by " \r\n" and with
// its own line breaks turned into spaces; a new buffer (malloc)
char *text_sentences(char *text, int32_t spell);
// @0x6367f53b stdcall: line breaks made CR / CR LF, a lone line break inside text made a space (a blank
// line stays a break), escape sequences passed over; then text_sentences
char *text_prepare(char *text, int32_t spell);
