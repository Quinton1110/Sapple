// The thread-safe message queue between the engine's stages (records of 20 bytes, a critical
// section, and four events: not empty, empty, full (above the high-water mark) and drained). Layout
// as in the original.
#pragma once
#include "gptr.h"

// a queue record: what, a value or a COM object, one more word, a malloc'd payload and its size
typedef struct QItem {
    uint32_t code;          // 0x00
    union { uint32_t value; GPTR(void) obj; } a;   // 0x04
    uint32_t v2;            // 0x08
    GPTR(void) data;        // 0x0c freed by whoever consumes the record
    uint32_t bytes;         // 0x10
} QItem;
typedef struct Queue {
    uint32_t count;         // 0x00
    uint32_t cap_bytes;     // 0x04
    GPTR(QItem) items;      // 0x08
    int32_t full;           // 0x0c set at the high-water mark, cleared at a quarter of it
    GPTR(const char) name;  // 0x10 for debugging (the original never reads it)
    uint32_t high;          // 0x14 high-water mark
    uint8_t cs[24];         // 0x18 CRITICAL_SECTION
    uint32_t ev_nonempty;   // 0x30
    uint32_t ev_full;       // 0x34
    uint32_t ev_empty;      // 0x38
    uint32_t ev_room;       // 0x3c
} Queue;

int queue_grow(Queue *q, uint32_t n);                   // @0x63684899 thiscall: room for n (x1.5); 1 = ok
void queue_update_events(Queue *q);                     // @0x63684905 thiscall
uint32_t queue_count(Queue *q);                         // @0x6368498f thiscall (locked)
int queue_is_full(Queue *q);                            // @0x636849ab thiscall: count >= high (locked)
int queue_push(Queue *q, const QItem *it);              // @0x636849cd thiscall
int queue_push_n(Queue *q, const QItem *its, uint32_t n);   // @0x63684a29 thiscall
int queue_pop(Queue *q, QItem *out);                    // @0x63684aa1 thiscall
// locked reads of the first field of three other objects (their critical sections at +0x58, +0x11c,
// +0x2cc)
uint32_t locked_get_58(void *obj);                      // @0x6368447a thiscall
uint32_t locked_get_11c(void *obj);                     // @0x6368225f thiscall
uint32_t locked_get_2cc(void *obj);                     // @0x63687cfd thiscall
// @0x636847ce thiscall: an empty queue, no high-water mark (-1), four manual-reset events, all clear
Queue *queue_ctor(Queue *q);
// @0x6368488f thiscall: set the name
void queue_set_name(Queue *q, const char *name);
// @0x63684967 thiscall: set the high-water mark and the events that depend on it (locked)
void queue_set_high(Queue *q, uint32_t high);
