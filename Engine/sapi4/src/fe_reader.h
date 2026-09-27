// The word module's reader: the prepared text (with the engine's escape sequences) read a word at a time
// into the word object's input tape (w + 0x585), escapes turned into events on the object's list and
// into settings, user lexicon words replaced by their pronunciations, phrase breaks decided by length.
// Its state is global (0x63738bd8..0x63738c98), as in the original; errors unwind with longjmp.
#pragma once
#include "gptr.h"

// a user lexicon: a sorted array of entries { word, pronunciation }
typedef struct LexEntry {
    GPTR(char) word;
    GPTR(char) pron;
} LexEntry;
typedef struct UserLex {
    int32_t count;
    GPTR(GPTR(LexEntry)) entries;   // 0x04
    int32_t cap;                    // 0x08
    int32_t _0c;
} UserLex;

// the phrase being turned into phones (part of WordInput)
// the alternative prosody (mode 0): one record per word of the phrase
typedef struct AltRec {
    char name[0x1e];            // 0x00 the word's phone characters (tape 1)
    char chars[0x1e];           // 0x1e its tape-7 characters, without '~' and ' ' (the key)
    char c3c, c3d, c3e, _3f;    // 0x3c the word's marks from tapes 5, 4 and 3 ('_': none)
    int32_t f40;                // 0x40 starts a phrase
    int32_t f44;                // 0x44 ends a phrase ('#')
    int32_t f48, f4c;           // 0x48 a pause (tape 5 blank) before it / after it
    int32_t f50, f54;           // 0x50 its first character is of class 1 / the next word's is of class 2
    int32_t _58, _5c;
    int32_t f60, f64;           // 0x60 start and end pitch (quarter semitones, then scaled)
} AltRec;
// a group of words between phrase starts, as the alternative prosody predicts them (0x340 bytes)
typedef struct AltGroup {
    int32_t n;                  // 0x00 words with a key
    int32_t count;              // 0x04 words
    char last[0xc];             // 0x08 the last word's key (strcpy: may run on into `str`, set after)
    GPTR(char) str;             // 0x14 malloc'd: the keys, runs of keyless words as counts
    uint8_t _18[0x340 - 0x14 - sizeof(GPTR(char))];
} AltGroup;

typedef struct Prosody {
    int32_t g0, g4;             // 0x00 copies of 0x63738b6c / 0x63738b70
    int32_t _08;
    GPTR(char) s[9];            // 0x0c copies of the word object's tapes, and (s[8]) its output string
    GPTR(int32_t) phone_pos;    // 0x30 per phone character: its position (kind 4 events)
    int32_t f34;                // 0x34 (mode 0: the index of the last word record)
    AltRec w[0x97];             // 0x38 (mode 0) the words (the code also writes one record further, over
                                //   the fields that follow: see fe_altpros.c)
    uint8_t _3d90[4];
    int32_t f3d94;              // 0x3d94 (mode 0) phrase starts seen
    int32_t _3d98;
    int32_t f3d9c;              // 0x3d9c (mode 0) phrase ends seen
    uint8_t _3da0[8];
    uint8_t z3da8[0x97];        // 0x3da8 cleared at each phrase start
    char marks[0x4640 - 0x3e3f];  // 0x3e3f (mode 0) the words' first tape-7 characters, and a last one
    GPTR(float) pitch;          // 0x4640 per phone: pitch (Hz or quarter semitones)
    GPTR(char) phones;          // 0x4644 the phone characters
    int32_t targets[1000][2];   // 0x4648 pitch targets: (time, value), ended by (0x7fffffff, 0)
    char phone_chars[1000];     // 0x6588
    int32_t phone_end[1000];    // 0x6970 cumulative duration at each phone's end
    int32_t phone_dur[1000];    // 0x7910
    GPTR(uint8_t) alt[7000];    // 0x88b0 the alternative prosody's data records (0x340 bytes each)
    int32_t nalt;               // 0xf610
    GPTR(AltGroup) recs;        // 0xf614 (mode 0) the word groups
    int32_t nrecs;              // 0xf618
    int32_t npitch;             // 0xf61c
    int32_t pitch_hz;           // 0xf620
    int32_t f624;               // 0xf624
    struct { char name[0x24]; int32_t v[4]; } alt_tab[25];   // 0xf628 the alternative prosody's table
    int32_t nalt_tab;           // 0xfb3c
    int32_t mode;               // 0xfb40 0: the alternative prosody
} Prosody;

// the per-voice input buffer (0x63738968 + 4 * index points to one)
typedef struct WordInput {
    GPTR(char) buf;         // 0x00 (malloc)
    int32_t _04;
    GPTR(char) rd;          // 0x08
    GPTR(char) wr;          // 0x0c
    int32_t len;            // 0x10
    GPTR(char) out[9];      // 0x14 the word object's output string and tape copies (malloc)
    int32_t _38;
    int32_t flag;           // 0x3c
    int32_t pitch;          // 0x40 12 * 4 * log2(pitch_hz / 50)
    GPTR(int32_t) marks;    // 0x44 index events: (position, value) pairs
    Prosody p;              // 0x48
} WordInput;

// events on the word object's list (a failure to allocate is -1; an event type the object does not
// collect, -2)
int32_t ev_add_text(uint8_t *w, int32_t pos, int32_t len);             // @0x6368ac44 stdcall, kind 1
int32_t ev_add_word(uint8_t *w, int32_t pos, int32_t len);             // @0x6368ad34 stdcall, kind 2
int32_t ev_add_index(uint8_t *w, int32_t v, int32_t pos, int32_t x);   // @0x6368ad9d stdcall, kind 8
int32_t ev_add_z(uint8_t *w, int32_t v, int32_t pos, int32_t x);       // @0x6368ae0d stdcall, kind 0x40
int32_t ev_add_value(uint8_t *w, int32_t pos, uint16_t v);             // @0x6368ae7d stdcall, kind 0x10
int32_t ev_add_mark(uint8_t *w, int32_t pos, int32_t v);               // @0x6368aee5 stdcall, kind 0x20
int32_t word_reset(uint8_t *w);                                         // @0x63688fb3 cdecl
int32_t word_voice(uint8_t *w, int32_t v);                              // @0x636890e2 cdecl

uint8_t rd_back(void);                                  // @0x63689fad: one character back
void rd_init(char *text, int32_t len);                  // @0x636896e3 stdcall
int32_t rd_phrase(uint8_t c);                           // @0x6368aa05 stdcall: a phrase break here? 0 or 1..4
int32_t rd_close_word(int32_t start, int32_t *end);     // @0x63689bc0 stdcall
// @0x63689fc4 next character (longjmp -1 at the end), @0x63689fee the character after escapes,
// @0x63689726 one word: only through word_run (they unwind to its setjmp)
int32_t word_run(uint8_t *w, char *text, int32_t *len, int32_t flag);   // @0x63689357 cdecl
int32_t word_dispatch(int32_t idx, char *text, int32_t *len, int32_t flag);  // @0x63688b27 cdecl
int32_t words_feed(int32_t idx, char *text, int32_t *consumed, int32_t *last);  // @0x63677575 stdcall
int32_t words_continue(int32_t idx, int32_t *consumed, int32_t *last);  // @0x636779a5 stdcall
void words_set_pitch(int32_t idx, int32_t hz);          // @0x63677af9 stdcall

// the user lexicon
uint8_t phone_code(const char *name);                  // @0x63675fb5 stdcall
char *phones_to_codes(const char *pron);                // @0x63675ffc stdcall (malloc, or NULL)
int lex_entry_cmp(const void *a, const void *b);        // @0x636758d1 cdecl (bsearch)
GPTR(char) user_lex_find(const char *word, const UserLex *t);   // @0x6367584b stdcall
int32_t user_lex_word(char *word, GPTR(char) *out);     // @0x63676ec7 cdecl
