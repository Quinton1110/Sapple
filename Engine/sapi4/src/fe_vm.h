// The front end's rule machine. msttssyn.dll contains three copies of it (compiled from the same
// source with different globals): six byte tapes, a stack index into them and a program pointer into
// the rule bytecode. One C function per routine serves all three; the instance says which globals.
#pragma once
#include "gptr.h"

typedef struct RuleVM {
    uint32_t tape[6];       // globals holding the six tapes (char *)
    uint32_t top;           // int16 global: the stack top on the tapes
    uint32_t pc;            // global holding the bytecode pointer (unsigned char *)
} RuleVM;
extern const RuleVM rule_vm[3];

// @0x6368d725 (and copies): open a gap: tape[i] = tape[i-1] for i = hi..lo, then mark both ends
// (0x0e on tapes 0-4, 0 on tape 5)
void vm_shift_right(const RuleVM *vm, int16_t lo, int16_t hi);
// @0x6368d822: close a gap: tape[i-2] = tape[i-1] for i = lo..hi. Returns what the original leaves in
// EAX (callers pass it on): hi + 1, or lo with prev_eax's upper half when there is nothing to move.
uint32_t vm_shift_left(const RuleVM *vm, int16_t lo, int16_t hi, uint32_t prev_eax);
// @0x6368f4b2: move b..a (downwards) onto the stack below the top
void vm_pop_range(const RuleVM *vm, int16_t a, int16_t b);
// @0x6368f68c: copy from the stack back to s+1.. until a 0x0e marker; returns the last position
int32_t vm_push_until(const RuleVM *vm, int32_t s);
// @0x6368e4cf: skip the bytecode block opened by `tok` (nesting on tok,1 and 0xf5 pairs) up to tok,end;
// 1 in AL if found, 0 at the tok,0 terminator. Returns the full EAX of the original.
uint32_t vm_skip_block(const RuleVM *vm, uint8_t tok, uint8_t end);
// @0x6368f567: move tape positions a-1..b by d (d > 0: towards the end, clearing tape 5 behind; d <= 0:
// towards the start, adding tape 5 into the destination). Returns b + d. (32-bit arguments of which
// the original uses the low halves in places.)
int32_t vm_move(const RuleVM *vm, int32_t a, int32_t b, int32_t d);

// the condition interpreter's globals and tables (per instance)
typedef struct CondVM {
    uint32_t pc;            // bytecode pointer global
    uint32_t saved;         // the previous tape (char * global)
    uint32_t cur;           // the current tape
    uint32_t ch;            // the current character (byte global)
    uint32_t tapes;         // array of the tape pointer globals, indexed by operand
    uint32_t t1, t2;        // class tables, 0xa2 bytes per class, indexed by character
    uint32_t vars;          // variable bytes (0: unbound)
    uint32_t cls;           // per variable: kind (4 or 5) and class
    uint32_t t3, t4;        // the classes a variable of kind 5 / 4 must match
    uint32_t bound;         // int16 global set to 1 when a variable gets bound
    int pos16;              // instance A reads the position as int16, B and C as int32
} CondVM;
extern const CondVM cond_vm[3];
// @0x6368e11b (and copies): evaluate the condition bytecode at *pc up to the byte `end`, at tape
// position pos: select tapes, compare the current character with literals, classes and variables
// (binding unbound ones). AL = 1 if every test passed (pc left at `end`), 0 at the first failure (pc
// left at it); the rest of EAX is the pc, as in the original. Opcodes outside 0xea..0xfd, and
// 0xeb-0xed / 0xf4-0xf8, make the original loop forever; they never occur in the rules.
uint32_t vm_cond(const CondVM *vm, uint8_t end, int32_t pos);

// the rule runner's globals (per instance): a choice-point stack for backtracking over alternatives
typedef struct RunVM {
    uint32_t step;          // int16: position advance per executed rule
    uint32_t pc;            // the bytecode pointer global
    uint32_t nch;           // int16: choice points on the stack
    uint32_t pos0;          // int16
    uint32_t flag;          // byte
    uint32_t pos;           // int16: current position
    uint32_t tries;         // uint8[]: per choice point, how many alternatives were tried
    uint32_t cpos;          // int16[]: saved pos
    uint32_t cpc;           // ptr[]: saved pc
    uint32_t ctape;         // ptr[]: saved current tape
    uint32_t cpos0;         // int16[]: saved pos0
    uint32_t ckind;         // int16[]: 0xf5 retry once, 0xf6 next alternative block, 0xf7 bounded retry
    uint32_t cur;           // the current tape global (as in CondVM)
    uint32_t abort;         // int32: set when the stack overflows (A) / checked before trying (B, C)
    uint32_t pos2;          // int16: the rule's right context start (match end - rule offset)
    uint32_t bound;         // int16: a variable was bound
    uint32_t vars;          // the variable bytes, cleared after a rule was tried if any got bound
    uint32_t nvars;         // how many bytes that clear covers (8, 7, 30)
} RunVM;
extern const RunVM run_vm[3];
// @0x6368e53d / 0x63692568 / 0x63695aaf: execute one context opcode at *pc at position pos, moving in
// the direction `step`: tests (literals, classes, variables), tape switches, filler skips,
// alternatives and optional parts (pushing choice points for vm_run), and repetitions. AL = success
// (for the plain class tests 0xfa / 0xfc, the class table byte itself).
uint8_t vm_exec(int inst);
// @0x6368e30f and copies: run the rule list at pc+1 up to 0xff, backtracking through the choice
// points on failure; AL = 1 if it ran through
uint8_t vm_run(int inst);

// the segment a rule is tried on (what vm_try and vm_apply get; it lies 0x30 bytes into a larger
// object whose per-character counts, int32 indexed by the character, start at -0x30)
typedef struct RuleHdr {
    int16_t kind;           // 0x00 (rule opcode 0xfe,2 sets it: 0xb0 / 0x15f / 0x5bb per instance)
    int16_t left;           // 0x02 the segment's start on the tapes
    int16_t len;            // 0x04 its end
    uint8_t _6[0x290 - 6];
    int16_t max;            // 0x290 the largest advance so far (or the last one, if !track)
    uint8_t track;          // 0x292
    uint8_t applied;        // 0x293
    GPTR(char) tape;        // 0x294 the current tape after trying it
    GPTR(char) out;         // 0x298 the output tape
} RuleHdr;
// @0x6368f187 / 0x636931b1 / 0x636966f8: apply the rule's output: either a procedure from the
// instance's callback table (opcode 0xfe,n) or a replacement string written over the matched span
// (literals, variables, class maps, tape switches, filler), keeping the per-character counts
// current. AL = success.
uint8_t vm_apply(int inst, int32_t a0, RuleHdr *r);
// the rule procedures (0xfe,n; n >= 3) of the instance's table (fe_procs.c): AL = success
uint8_t vm_callback(int inst, uint8_t n, int16_t *left, int16_t *len);
// the same, with the EAX the original leaves (prev_eax: the caller's EAX, where the original keeps it)
uint32_t vm_proc(int inst, uint8_t n, int16_t *left, int16_t *len, uint32_t prev_eax);
// procedures not decompiled yet: run the original (harness/guest.c)
uint8_t vm_proc_guest(int inst, uint8_t n, int16_t *left, int16_t *len);
// @0x6368d896 / 0x636918d6 / 0x63694e1a: try one rule at position a1: match, run the left context
// backwards and the right context forwards, then apply. AL = 1 if applied. prev_eax is the caller's EAX
// (its upper half shows in the result and in what is passed on).
uint32_t vm_try(int inst, int32_t a0, int32_t a1, RuleHdr *r, uint32_t prev_eax);

// the rule matcher's own globals (per instance; the rest are the condition interpreter's)
typedef struct MatchVM {
    uint32_t adv;           // int16 (RunVM.pos0): nonzero = skip 0x7e filler before reading a character
    uint32_t valtab;        // per class and character: a value (class test 0xf8 stores it in `out`)
    uint32_t out;           // byte
    uint32_t last;          // int16: the last position a repeated test matched
    uint32_t maxcnt;        // int16: the repeat limit of the current repetition
    uint32_t clsb;          // byte: the class / literal of the current repetition
    uint32_t saved;         // the previous tape (the matcher's own copy)
} MatchVM;
extern const MatchVM match_vm[3];
// @0x6368d966 / 0x63691990 / 0x63694ed7: match the rule's input pattern at *pc against the current
// tape from position `at` (literals, classes, variables, tape switches, repetitions with a minimum
// and a maximum, embedded conditions). Returns the position after the match, 0 if it fails. `back` is
// where opcode 0xeb resumes (the caller passes the rule's input length).
int16_t vm_match(int inst, int32_t at, int32_t back);

// @0x6368c997 / 0x636909b7 / 0x63693ef8: run rule set `mode` over the segment s+1..*pend of the tapes
// (opened as a gap first, closed again at the end; *pend follows the segment's growth): per rule, its
// type says how to scan (forward / backward, anchored on a character or at every position, every match
// or the first, single rules or groups) and vm_try tries it. Keeps per-character counts of the segment
// so that rules whose characters do not occur are skipped. AL = whether any rule applied (the rest of
// EAX as the original leaves it).
uint32_t vm_main(int inst, int32_t mode, int32_t s, int16_t *pend);

// helpers of the rule procedures (fe_procs.c)
// @0x6368b468 / 0x6368b526 stdcall: machine A's prosody-tag numbers into the word object
void tag_number(int16_t a, int16_t b, int16_t c);
void tag_pair(int16_t a, int16_t b, int16_t c);
// @0x636905f9 stdcall: spaces minus 0x11 marks on tape 0 in [a, b)
int32_t count_words(int16_t a, int16_t b);
