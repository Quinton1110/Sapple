// The unit stage of msttssyn.dll (its own worker thread): phones -> phone indices -> senones, then
// durations and pitch. The stage object is large; only the fields decompiled code touches are named.
#pragma once
#include "gptr.h"
#include "containers.h"
#include "senone.h"
#include "queue.h"

typedef struct UnitRec {       // one phone of the phrase (0x4c bytes)
    int32_t phone;             // 0x00 phone index
    int32_t senone;            // 0x04
    float dur;                 // 0x08 duration factor (0: none given)
    int32_t nstates;           // 0x0c
    float a[5];                // 0x10 per state (copied through)
    float b[5];                // 0x24 per state (rounded to int16 on output)
    float c[5];                // 0x38 per state (scaled on output)
} UnitRec;

// a phone as the front end hands it over (100 bytes)
typedef struct PhoneIn {
    char name[16];             // 0x00
    float dur;                 // 0x10
    int32_t nstates;           // 0x14
    float a[5], b[5], c[5];    // 0x18, 0x2c, 0x40
    uint8_t _54[0x10];
} PhoneIn;

// a unit as the stage hands it to the synthesizer (0x50 bytes)
typedef struct UnitOut {
    char name[16];             // 0x00
    int16_t unit;              // 0x10 unit index in the voice (0: silence)
    int16_t _12;
    float dur;                 // 0x14
    int32_t nstates;           // 0x18
    float a[5];                // 0x1c
    int16_t b[5];              // 0x30
    int16_t _3a;
    float c[5];                // 0x3c
} UnitOut;

typedef struct UnitOutBuf { GPTR(UnitOut) units; int32_t bytes; } UnitOutBuf;

typedef struct SenStat { float scale; float _4, _8; float var; } SenStat;   // per senone

typedef struct UnitStage {
    int32_t _000;
    GPTR(const SenStat) stats[150];   // 0x004 per phone: per-senone statistics
    GPTR(UnitRec) recs;        // 0x25c
    GPTR(int16_t) ids;         // 0x260 phone index per unit
    GPTR(int16_t) senones;     // 0x264 senone per unit
    int32_t cap;               // 0x268 capacity of the three arrays
    uint32_t notify_msg;       // 0x26c window message for notifications
    uint32_t notify_hwnd;      // 0x270
    uint32_t rate;             // 0x274 speaking rate (wpm)
    uint32_t default_rate;     // 0x278
    uint32_t fast_rate;        // 0x27c at or below this rate: final silences and the fitted scale
    float dur_scale;           // 0x280
    float sil_dur;             // 0x284
    int32_t final_sil;         // 0x288 append a silence unit when the next phrase does not start with one
    uint32_t amp;              // 0x28c output scale of c[] (x 1/65535)

    GPTR(const NamedTable) phone_names;   // 0x290
    GPTR(const SenoneTree) tree;          // 0x294
    int32_t tree_mode;         // 0x298 1: senone_lookup; otherwise the older query (0x6367233c)
    int32_t senone_base;       // 0x29c
    int32_t sil_phone;         // 0x2a0 phone index used outside the phrase
    uint32_t wait_in[2];       // 0x2a4 stop event, input queue not empty
    uint32_t wait_out[2];      // 0x2ac stop event, output queue has room
    int32_t last_b;            // 0x2b4 last b[] value carried across phrases (-1: none)
    float last_c;              // 0x2b8 the same for c[] (-1: none)
    uint32_t rate_flags;       // 0x2bc bit 1: scale by the alternative default rate
    int32_t rate_mode;         // 0x2c0 0: default rate; otherwise see rate_flags
    uint32_t alt_rate;         // 0x2c4
    GPTR(const struct AltRules) lattice;   // 0x2c8 alternative-unit rules (NULL: search off)
    uint8_t cs[24];            // 0x2cc CRITICAL_SECTION guarding `_000` (the abort flag)
    uint32_t ev_stop;          // 0x2e4
    uint32_t ev_done;          // 0x2e8
    GPTR(struct Queue) in;     // 0x2ec
    GPTR(struct Queue) out;    // 0x2f0
} UnitStage;

// the alternative-unit rules (one bucket per first phone byte): a rule matches a phone string, with
// optional left/right context sets of senones, and proposes a unit id for each covered phone
typedef struct AltBucket {
    int32_t first_id;          // 0x00 unit ids of this bucket start here (+ index + 1)
    int32_t count;             // 0x04 rules
    GPTR(const uint8_t) flags; // 0x08 per rule: 1 no left context, 2 keep the last phone, 0xc context
    GPTR(GPTR(const char)) keys;      // 0x0c the phone-index string each rule matches
    GPTR(const uint8_t) nleft; // 0x10
    GPTR(const uint8_t) nright;// 0x14
    GPTR(GPTR(const int16_t)) left;   // 0x18 context senone sets
    GPTR(GPTR(const int16_t)) right;  // 0x1c
    GPTR(const int16_t) nunits;       // 0x20 per rule: units it proposes
    GPTR(GPTR(const int16_t)) units;  // 0x24
    GPTR(GPTR(const char)) tails;     // 0x28
} AltBucket;
typedef struct AltRules { GPTR(const AltBucket) bucket[256]; } AltRules;

// ---- loading (vload.c)
// a Senone stream record: a senone's statistics
typedef struct SenoneRec { char name[8]; int32_t idx; int32_t a, b; float c, d; } SenoneRec;
// @0x63685847 stdcall: if flag, a count byte then that many int16 at *pp (skipped); else none
void alt_arg(int32_t flag, const char **pp, uint8_t *count, GPTR(const int16_t) *ptr);
// @0x6368587f stdcall: n buckets of rules from the AltUnits stream's bytes; 0, or -1 out of memory
int32_t alt_parse(const char *buf, int32_t n, GPTR(AltBucket) *out);
// @0x63685b25 stdcall: the AltUnits stream into *out (n buckets), or 0 if the voice has none
void alt_units_load(void *stg, const uint16_t *name, int32_t n, GPTR(AltRules) *out);
// @0x636856db thiscall: the voice variant's records looked through (nothing is kept)
void unit_voice_scan(UnitStage *u, int32_t voice, int32_t variant);
// @0x63685471 thiscall: the unit stage from the voice file (VcHeader, PhoneFile, TreeImage, Senone,
// AltUnits); 1
int32_t unit_init(UnitStage *u, uint8_t *qin, uint8_t *qout, void *stg, const uint8_t *voice, int32_t voice_idx,
                  int32_t variant, uint32_t hwnd, uint32_t msg);
// not decompiled yet (harness/guest.c): the older tree format's loader (0x636727fd)
int32_t senone_tree_load_old(void *stg, const char *name, const void *ps, GPTR(SenoneTree) *out);
// @0x63685c20 stdcall: the next rule of key[0]'s bucket at or after *pos that matches key (and the
// senone context around position i); its unit id is proposed for the covered positions. 1 = found
// (*pos after it), 0 = no more.
int lattice_match(const char *key, const int16_t *sen, int32_t n_rest, int32_t *pos, int32_t i,
                  const AltRules *rules, VecArray *va);
// @0x63685d18 stdcall: the rule behind unit id x + 1 (buckets sorted by first id): its proposed units;
// *tail = its tail string (one phone index per byte), *nunits = how many units it proposes
const int16_t *unit_span_lookup(int32_t x, int32_t n, GPTR(const AltBucket) const *table, GPTR(const char) *tail,
                               int32_t *nunits);
// @0x63685d72 thiscall: rewrite the phrase with the units the search chose: per rule, phones are
// swapped for other units, replaced, inserted, deleted or merged, and the senones next to an edit are
// looked up again in their new context. Returns the new number of units.
int32_t unit_lattice_apply(UnitStage *u, GPTR(PhoneIn) *ph, int32_t *n, int32_t m, VecArray *va,
                           uint8_t *bounds);
// @0x63686614 thiscall: the alternative-unit search (on when `lattice`): may rewrite the phrase and
// its length; returns the number of units
int32_t unit_lattice(UnitStage *u, GPTR(PhoneIn) *ph, int32_t *n);
// @0x6368571f thiscall: hand a phrase's units to the synthesizer queue (as 20-byte blob records),
// waiting for room; dropped if the stage is aborted or stopped
void unit_send(UnitStage *u, UnitOutBuf *units);
// @0x63687974 thiscall: one phrase from the front end: tags are executed or forwarded, runs of phones
// between marks are selected, timed and sent
int32_t unit_phrase(UnitStage *u, PhoneIn *ph, int32_t n);
// @0x63687ba4 thiscall: the unit stage's thread loop; @0x63687d3f, its stdcall thread procedure, is
// the same call with the stage as the thread argument
int32_t unit_thread_loop(UnitStage *u);

// @0x63687e26 thiscall: derive dur_scale / sil_dur / final_sil from the rate
void unit_rate_update(UnitStage *u);
// @0x63687d4b thiscall: set the rate (-1: 3 x default, 0: 30; otherwise 30..3 x default, else
// E_UNEXPECTED), notify (message wparam 2) and update
int32_t unit_set_rate(UnitStage *u, uint32_t r);
// @0x63687e15 thiscall: the rate; 0
int32_t unit_get_rate(const UnitStage *u, uint32_t *r);
// @0x63687de5 thiscall: back to the default rate
int32_t unit_reset_rate(UnitStage *u);
// @0x63687fbb thiscall: set the output scale (low 16 bits), notify (wparam 3)
int32_t unit_set_amp(UnitStage *u, uint32_t v);

// the query of the older tree interface (0x6367233c, not decompiled; never reached by the corpus)
typedef struct TreeQuery {
    int16_t cur, left, right;  // 0x00
    uint8_t _06[4];
    int8_t pos;                // 0x0a
    uint8_t _0b[0x21];
    int8_t _2c;                // 0x2c set to 0
    uint8_t _2d[3];
} TreeQuery;
// @0x6367233c stdcall (not decompiled yet: the hook build calls the original)
int32_t tree_query(const SenoneTree *t, TreeQuery *q);

// @0x63686c7d, @0x63686905 thiscall (not decompiled yet)
void unit_prosody_a(UnitStage *u, int n);
void unit_prosody_b(UnitStage *u, int n);

// @0x636871a4 thiscall: durations and state values of the n units just selected (from the phones'
// own values and the per-senone statistics), then the two contour passes, then the output units
// (plus a final silence if configured and the next phone is not silence). *last = the last phone.
int unit_prosody(UnitStage *u, const PhoneIn *ph, int n, int32_t *last, int next_phone, UnitOutBuf *out);

// @0x63687751 thiscall: look up the phone index and the senone of every phone of a phrase. ph: n names
// of 100 bytes; "\..." (tags) and "#..." (boundaries) are not phones, they mark word edges.
void unit_select(UnitStage *u, const char *ph, int n);
// @0x636851da thiscall: constructor: no voice, no statistics, a lock, the stop and done events,
// room for 10 units
UnitStage *unit_ctor(UnitStage *u);
