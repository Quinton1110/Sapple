#include "queue.h"
#include "crt_vc.h"
#include "x87.h"
#include <string.h>

LAYOUT(Queue, ev_room, 0x3c);
LAYOUT(QItem, bytes, 0x10);

int queue_grow(Queue *q, uint32_t n) {
    uint32_t size = (uint32_t)x87_ftol((double)n * 1.5) * (uint32_t)sizeof(QItem);
    if (size <= q->cap_bytes) return 1;
    if (q->items) {
        QItem *r = vc_realloc(GP(void, q->items), size);
        if (!r) return 0;
        GPSET(q->items, r);
        q->cap_bytes = size;
        return 1;
    }
    QItem *m = vc_malloc(size);
    GPSET(q->items, m);
    if (m) q->cap_bytes = size;
    return m != NULL;
}

void queue_update_events(Queue *q) {
    vc_ResetEvent(q->ev_nonempty);
    vc_ResetEvent(q->ev_empty);
    vc_ResetEvent(q->ev_full);
    vc_ResetEvent(q->ev_room);
    vc_SetEvent(q->count ? q->ev_nonempty : q->ev_empty);
    if (q->count >= q->high) {
        q->full = 1;
        vc_SetEvent(q->ev_full);
    } else if (q->full && q->count > q->high >> 2) {
        vc_SetEvent(q->ev_full);
    } else {
        if (q->full) q->full = 0;
        vc_SetEvent(q->ev_room);
    }
}

uint32_t queue_count(Queue *q) {
    vc_EnterCriticalSection(q->cs);
    uint32_t r = q->count;
    vc_LeaveCriticalSection(q->cs);
    return r;
}

int queue_is_full(Queue *q) {
    vc_EnterCriticalSection(q->cs);
    int r = q->count >= q->high;
    vc_LeaveCriticalSection(q->cs);
    return r;
}

int queue_push(Queue *q, const QItem *it) {
    vc_EnterCriticalSection(q->cs);
    if (!it || !queue_grow(q, q->count + 1)) {
        vc_LeaveCriticalSection(q->cs);
        return 0;
    }
    GP(QItem, q->items)[q->count] = *it;
    q->count++;
    queue_update_events(q);
    vc_LeaveCriticalSection(q->cs);
    return 1;
}

int queue_push_n(Queue *q, const QItem *its, uint32_t n) {
    vc_EnterCriticalSection(q->cs);
    if (!its || !queue_grow(q, q->count + n)) {
        vc_LeaveCriticalSection(q->cs);
        return 0;
    }
    for (; n; n--) {
        GP(QItem, q->items)[q->count] = *its++;
        q->count++;
    }
    queue_update_events(q);
    vc_LeaveCriticalSection(q->cs);
    return 1;
}

int queue_pop(Queue *q, QItem *out) {
    vc_EnterCriticalSection(q->cs);
    if (!q->count || !out) {
        vc_LeaveCriticalSection(q->cs);
        return 0;
    }
    QItem *items = GP(QItem, q->items);
    *out = items[0];
    memmove(items, items + 1, (q->count - 1) * (uint32_t)sizeof(QItem));
    q->count--;
    queue_update_events(q);
    vc_LeaveCriticalSection(q->cs);
    return 1;
}

static uint32_t locked_get(void *obj, uint32_t cs_off) {
    uint8_t *o = obj;
    vc_EnterCriticalSection(o + cs_off);
    uint32_t r;
    memcpy(&r, o, 4);
    vc_LeaveCriticalSection(o + cs_off);
    return r;
}
uint32_t locked_get_58(void *obj) { return locked_get(obj, 0x58); }
uint32_t locked_get_11c(void *obj) { return locked_get(obj, 0x11c); }
uint32_t locked_get_2cc(void *obj) { return locked_get(obj, 0x2cc); }

Queue *queue_ctor(Queue *q) {
    q->high = 0xffffffffu;
    q->count = 0;
    q->items = 0;
    q->cap_bytes = 0;
    q->full = 0;
    GPSET(q->name, DLLVAR(const char, 0x63721b64));
    memset(q->cs, 0, sizeof q->cs);
    vc_InitializeCriticalSection(q->cs);
    q->ev_nonempty = vc_CreateEventA(1, 0);
    q->ev_full = vc_CreateEventA(1, 0);
    q->ev_empty = vc_CreateEventA(1, 0);
    q->ev_room = vc_CreateEventA(1, 0);
    return q;
}

void queue_set_name(Queue *q, const char *name) { GPSET(q->name, name); }

void queue_set_high(Queue *q, uint32_t high) {
    vc_EnterCriticalSection(q->cs);
    q->high = high;
    queue_update_events(q);
    vc_LeaveCriticalSection(q->cs);
}
