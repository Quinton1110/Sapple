// The lexer for SAPI 4 inline tags (\Spd=200\, \Chr="Whisper"\, \Mrk=3\ ...) and the tag parser of
// msttssyn.dll. Token codes are the original's.
#pragma once
#include "gptr.h"

enum {
    TOK_EQ = 0x16, TOK_COMMA = 0x17, TOK_LT = 0x18, TOK_SEMI = 0x19, TOK_COLON = 0x1a,
    TOK_STRING = 0x1c, TOK_BRACE = 0x1d, TOK_WORD = 0x1e, TOK_NUMBER = 0x1f, TOK_BACKSLASH = 0x22,
    TOK_END = 0x23,
};

// the keyword table in .data (0x6371f218, 22 entries of 16 bytes)
typedef struct TagKeyword {
    GPTR(const char) name;  // "chr", "spd", ...
    int32_t token;          // 0x04 what the lexer returns for it
    int32_t code;           // 0x08 what a parsed tag reports
    uint32_t handler;       // 0x0c code address of its parser (tag_parse_*)
} TagKeyword;

// a parsed tag
typedef struct TagOut {
    int32_t code;           // 0x00 keyword code, or 0xb for "unknown tag: speak it"
    int32_t value;          // 0x04 numeric argument
    int32_t _08;
    GPTR(char) text;        // 0x0c string argument (malloc'd) or the tag's text as UTF-16
    int32_t text_bytes;     // 0x10
} TagOut;

// @0x63681002 stdcall: decimal digits at *pp into *out; TOK_NUMBER, or TOK_END if nothing to read
int tag_lex_number(GPTR(const char) *pp, int32_t *out);
// @0x6368104a stdcall: a word up to , : ; = \ into buf (at most n-1 chars); its keyword token, or
// TOK_WORD / TOK_END
int tag_lex_word(GPTR(const char) *pp, char *buf, int n);
// @0x636810a6 stdcall: keyword token of a word (case-insensitive, lstrcmpiA), TOK_WORD if none
int tag_keyword(const char *word);
// @0x636810de stdcall: a "quoted" string (\\ is an escaped backslash) into buf, quotes included
int tag_lex_string(GPTR(const char) *pp, char *buf, int n);
// @0x6368114d stdcall: text up to } into buf, brace included
int tag_lex_brace(GPTR(const char) *pp, char *buf, int n);
// @0x63680f65 stdcall: the next token
int tag_lex(GPTR(const char) *pp, char *buf, int n);
// tag argument parsers, from the keyword table (stdcall, HRESULT): all take the keyword's index
// @0x63681196 not supported
int32_t tag_parse_unsupported(int kw, GPTR(const char) *pp, TagOut *out, char *buf, int n);
// @0x6368119e \Kw\ (tokens up to the closing backslash are skipped)
int32_t tag_parse_flag(int kw, GPTR(const char) *pp, TagOut *out, char *buf, int n);
// @0x636811d9 tag with a number argument: Kw=123
int32_t tag_parse_number(int kw, GPTR(const char) *pp, TagOut *out, char *buf, int n);
// @0x63681237 tag with a string argument: Kw="text"
int32_t tag_parse_string(int kw, GPTR(const char) *pp, TagOut *out, char *buf, int n);
// @0x63681315 stdcall: parse one tag (lower-cases it in place); an unknown or malformed tag becomes
// code 0xb with the tag's text as UTF-16. 0 only when out of memory.
int tag_parse(char *tag, TagOut *out);
