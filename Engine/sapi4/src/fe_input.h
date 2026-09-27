// The front end's text input: the application's (wide) text made single-byte, its SAPI tags turned into
// the engine's escape sequences with a numbered entry for each bookmark, word position and tag value,
// then prepared and cut into sentences for the word module.
#pragma once
#include "gptr.h"
#include "fe_reader.h"
#include "queue.h"

// one numbered entry (an escape "\x1bN<index> " in the text refers to it)
typedef struct FeEntry {
    int32_t pos;            // 0x00 (the slot after the last one allocated holds 2000000000)
    int32_t type;           // 0x04 1 word position, 0x13 bookmark, 0x15 its text, 0x14 its end, tags by code
    GPTR(char) text;        // 0x08 the value, as text (malloc)
} FeEntry;

// the front end object (only the fields the text input uses)
typedef struct FrontEnd {
    uint32_t _00;
    uint32_t msg;           // 0x04 the message posted at the end of the text
    uint32_t hwnd;          // 0x08 ... to this window
    GPTR(FeEntry) entries;  // 0x0c
    int32_t cap;            // 0x10
    int32_t last;           // 0x14 index of the last entry, -1 when none
    uint32_t _18;
    int32_t nindex;         // 0x1c index escapes seen in the part just read
    uint32_t _20;           // 0x20 the shared word module index (0x63738bd0)
    int32_t words;          // 0x24 the word module's voice index
    int32_t f28;            // 0x28 where the word module stopped (offset from cur)
    int32_t f2c;            // 0x2c its last character
    int32_t first_index;    // 0x30 the first index escape in that part
    GPTR(char) text;        // 0x34 the prepared text (malloc)
    GPTR(char) text_last;   // 0x38 its last character
    GPTR(char) cur;         // 0x3c where the word module is reading
    GPTR(char) next;        // 0x40 the next index escape ("\x1bI"), cut off with a NUL
    uint32_t flags;         // 0x44 0x100: an emphasis voice
    int32_t addr_mode;      // 0x48 2 inside an address tag
    int32_t voice;          // 0x4c
    int32_t f50;            // 0x50 < 0: a voice variant to check (negated)
    int32_t in_tp;          // 0x54 inside a "\Tp" tag's text (its phones replaced)
    uint8_t cs[0x18];       // 0x58 CRITICAL_SECTION
    uint32_t abort_event;   // 0x70 set: drop what is being spoken
    uint32_t done_event;    // 0x74 set when the thread ends
    GPTR(uint8_t) queue_in; // 0x78 the text queue (queue.h)
    GPTR(uint8_t) queue;    // 0x7c the output queue
    int32_t pitch;          // 0x80
    int32_t pitch_set;      // 0x84
    int32_t single_char;    // 0x88 the text was one character (spoken as its name)
    int32_t _8c;            // 0x8c
} FrontEnd;

void fe_entries_clear(FrontEnd *fe);                                    // @0x636803e4 thiscall
int32_t fe_entry_add_num(FrontEnd *fe, int32_t type, uint32_t value);   // @0x63680244 thiscall: its index
int32_t fe_entry_add_str(FrontEnd *fe, int32_t type, const char *s);    // @0x6368030a thiscall
GPTR(const char) tag_mode_name(int32_t v);                              // @0x63680f05 stdcall
int32_t tag_mode_value(const char *name);                               // @0x63680f2d stdcall
void tag_skip(GPTR(char) *pp);                                          // @0x636812b6 stdcall: past the closing backslash
void str_append(char **cur, const char *s);                             // @0x636812da stdcall: cur left on the NUL
// @0x6368043f thiscall: tags (when tags is set) into escapes; *out a new buffer (malloc); its length
int32_t fe_tags_to_escapes(FrontEnd *fe, int32_t tags, int32_t mode, char *text, GPTR(char) *out);
// @0x63680bb7 thiscall: keep a copy of the prepared text and hand its first part to the word module
int32_t fe_set_text(FrontEnd *fe, char *text);
// @0x636840f3 thiscall: one text from the application (mode 1: phonemes, converted first); *out what the
// word module returned; 0
int32_t fe_text_input(FrontEnd *fe, const uint16_t *text, int32_t *out, int32_t tags, int32_t mode);
// @0x636748d1 stdcall: phoneme symbols (1-based index in a table of 53) into their two-letter names,
// tag text copied as it is; *out a new wide buffer (calloc); 0, or -1 on an unknown symbol or a tag
// when tags are off
int32_t phonemes_to_names(const uint16_t *text, GPTR(uint16_t) *out, int32_t tags);
int32_t phoneme_index(uint16_t c);                                      // @0x63674978 stdcall
int32_t free_if2(void *p);                                              // @0x6367499a stdcall, as free_if

// @0x63680c6e thiscall: after the word module read a part: the first index escape in it, and (from
// where it stopped) the tags the index escapes stand for, queued as tags for the back end; r (0x6f at
// the end of the text), or 0x70 when the part held only escapes
int32_t fe_after_feed(FrontEnd *fe, int32_t r);
int32_t fe_continue(FrontEnd *fe);                                      // @0x63680c3f thiscall
int32_t fe_set_pitch_level(FrontEnd *fe, uint16_t v);                   // @0x636836b8 thiscall
int32_t fe_get_pitch(const FrontEnd *fe, uint16_t *v);                  // @0x63683791 thiscall: the pitch set; 0
// @0x636837a4 thiscall: a phrase of n phones into the phone list for the unit stage, index escapes back
// into tag records, "\Tp" pronunciations through the phone converter; queued
int32_t fe_phrase_out(FrontEnd *fe, int32_t n);
int32_t fe_thread(FrontEnd *fe);                                        // @0x63684233 thiscall
// the thread's frame (as the original's: the index escape's argument runs into the handles after it)
typedef struct FeThreadFrame {
    QItem item;             // -0x34
    char arg[0xc];          // -0x20
    uint32_t h[2];          // -0x14 abort, text queue not empty
    uint32_t h2[2];         // -0x0c abort, room in the output queue
    int32_t res;            // -0x04
} FeThreadFrame;
// fe_thread's work on one text item (split out so that a single-threaded driver can use it)
void fe_text_item(FrontEnd *fe, FeThreadFrame *f);
int32_t tag_char_value(const char *name);                               // @0x63680ecd stdcall
void fe_voice_flags(FrontEnd *fe, int32_t v);                           // @0x63683653 thiscall
int32_t fe_text_end(FrontEnd *fe);                                      // @0x63683765 thiscall
// @0x6367de02 stdcall: record i of a mode description (from +0xb1c: key, length, data); its data
const uint8_t *voice_rec(const uint8_t *v, uint32_t i, int32_t *key, int32_t *len);
uint32_t voice_rec_count(const uint8_t *v);                             // @0x6367de34 stdcall
// not decompiled yet (harness/guest.c)
int32_t text_from_other(char *text, GPTR(char) *out);                   // @0x636744c4 stdcall
