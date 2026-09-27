// The front end's lexicon modules: ten dictionaries (the rule procedures of rule machine C that look a
// word up), each with its own tables, and their shared helpers.
#pragma once
#include "gptr.h"

// the letter codes of the word being looked up (bytes at 0x63739e00) and the packed search key built
// from them (0x63739e40, compared by rule_bsearch)
#define LEX_CODES 0x63739e00
#define LEX_KEY 0x63739e40

// @0x6369a9c8 stdcall: pack `count` letter codes of `bits` bits each (from LEX_CODES) into LEX_KEY,
// most significant bit first. Returns what the original leaves in EAX (the last 16-bit window).
uint32_t lex_pack(uint8_t bits, uint16_t count);

// @0x6369a87e and 7 more, one per dictionary (n = 9..13): entry idx of an array of n-bit fields packed
// into 16-bit words (most significant first) at `table`; the low 16 bits are the field (the rest of EAX
// as the original leaves it)
uint32_t lex_field(uint32_t table, uint16_t idx, int n);
// @0x6369a47b: the same for 18-bit fields (the dictionary with its table in .data at 0x63722090)
uint32_t lex_field18(uint16_t idx);

// the word-list searches of the dictionaries that have more than 255 entries per column: the same
// binary search as rule_bsearch, returning the entry or -1 (0xffff)
// @0x63698968 / 0x63699322 / 0x63698e44: byte bounds
int16_t lex_bsearch8w(uint32_t table, uint32_t base, uint8_t lo, uint8_t hi, uint8_t width);
// @0x63697675: 16-bit bounds
int16_t lex_bsearch16(uint32_t table, uint32_t base, uint16_t lo, uint16_t hi, uint8_t width);
// @0x63697e71: 1 if tape A[lo..hi] are all printable (0x20..0xfe; tapes_graphic without the space)
int tapes_printable(int16_t lo, int16_t hi);

enum { LEX_RANGE_SBYTE, LEX_RANGE_UBYTE, LEX_RANGE_WORD };      // how a row stores a column's entries
enum { LEX_SEARCH_BYTE, LEX_SEARCH_WORD8, LEX_SEARCH_WORD16 };
enum { LEX_FIELD_BITS, LEX_FIELD_TABLE16, LEX_FIELD_18 };

// one dictionary (its tables in .rdata, its buffers in .data)
typedef struct LexDict {
    uint32_t first_map;     // byte per character: the column of words starting with it (0xff: none)
    uint32_t rows;          // per word length (row_stride bytes): the first entry (int16, or a byte if
                            // base_byte), the key-table base (uint16), then per column [lo, hi)
    int row_stride, base_byte, range;
    uint32_t code_map;      // byte per character: its letter code
    uint32_t widths;        // byte per word length: key bytes
    int search;
    uint32_t keys;          // the sorted keys
    int field;              // how an entry maps to its pronunciation's offset:
    uint32_t fields;        //   packed nbits-bit fields / a uint16 table
    int nbits;
    uint32_t prons;         // pronunciations: symbols up to a 5
    uint32_t pairs;         // per symbol: the two it expands to (0x20 first: a terminal)
    uint32_t buf;           // .data: the expanded pronunciation
    uint32_t ends;          // .data int16[4]: where the output ends on each tape
    uint32_t flag;          // .data byte: the last result (0: this dictionary does not keep it)
    uint8_t codebits;       // bits per letter code in the key
    int16_t maxlen;         // longest word (letters - 1)
    uint8_t minchar;        // lowest character a word may contain
    uint8_t sep;            // separates the output fields in the pronunciation
    int nout;               // output fields: tapes A, C, D in that order
    int c_first;            // the first two fields go to C, A instead
} LexDict;
// @0x6369a4ea and its copies (the rule procedures of rule machine C that look the segment up in a
// dictionary): if the word left..len-1 of tape A is in the dictionary, write its three fields
// ('!'-separated in the expanded pronunciation) to tapes A, C and D, pad, and move len. AL = found.
uint8_t lex_lookup(const LexDict *d, int16_t *left, int16_t *len);
extern const LexDict lex_dicts[];

// The main lexicon (@0x6369986b, rule procedure 22 of machine C): a letter trie coded as Huffman-
// compressed fields in a bit stream (16-bit words at 0x636fdbb8, bits taken least significant first),
// read through a global cursor.
#define LEX_CUR_WORD 0x63739950     // uint32: word index of the cursor
#define LEX_CUR_BIT 0x63739940      // byte: bit index 0..15
// the Huffman-coded fields: @0x63699d94 (branches), 0x63699e19 (first letter), 0x63699e9e (letter
// step), 0x63699f23 (subtree size); returns as the originals leave EAX
uint32_t lex_huff_branches(void);
uint32_t lex_huff_letter(void);
uint32_t lex_huff_step(void);
uint32_t lex_huff_size(void);
// @0x63699fae: a Huffman-coded index into a table of 19-bit values (0: see lex_raw19); returns the
// value, doubled if above 3
uint32_t lex_huff_value(void);
// @0x6369a0c9: 19 bits straight from the stream, doubled
uint32_t lex_raw19(void);
// @0x63699cd6: skip one trie node with its subtrees
void lex_skip_node(void);
// @0x6369a15f stdcall: at the node `start` (cursor as word << 4 | bit), find the branch for letter code
// ch (0x0e: end of word), counting in *acc the entries it passes; returns the branch's cursor, 1 for an
// end-of-word match, or 0. (count and acc are the caller's dwords, of which this updates the low halves.)
uint32_t lex_trie_step(uint32_t start, uint8_t ch, uint32_t *count, uint32_t *acc);
// @0x63699bd5 stdcall: the entry number of the word tape A[lo..hi] in the trie, or -1
int32_t lex_trie_lookup(uint16_t lo, uint16_t hi);
// @0x6369986b: the rule procedure (as lex_lookup, with the pronunciation from the trie entry)
uint8_t lex_main(int16_t *left, int16_t *len);
