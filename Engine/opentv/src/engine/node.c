/*
 * The work list and its node pool, and the per-stage windows onto it.
 *
 * All text in flight is held as a doubly linked list of 32-byte nodes drawn
 * from a fixed pool of 618 inside the engine object.  The pipeline stages
 * each own a window [first, last] of that list; a stage consumes nodes up to
 * its `cur` cursor and Engine_StageEnd hands them on to the next stage's
 * window.  The final stage returns them to the free list.
 *
 * Nodes whose type is not in a stage's type mask are control commands; the
 * stage executes them (Engine_RunControl) as its cursor passes them, so
 * parameter changes take effect at the right point in each stage.
 */
#include "engine.h"

/* Insert n in front of `before`; returns n. */
/* @0x100295c0 */
Node *TV_CDECL List_InsertBefore(Node *n, Node *before)
{
    Node *p = before->prev;
    n->prev = p;
    p->next = n;
    n->next = before;
    before->prev = n;
    return n;
}

/* Remove n from its list; returns n (Engine_NodeAlloc relies on this). */
/* @0x100295e0 */
Node *TV_CDECL List_Unlink(Node *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
    return n;
}

/* Rebuild the free list and the empty work list, and reset every stage
 * window and its voice parameters from the engine defaults. */
/* @0x10029900 */
void TV_THISCALL Engine_ResetNodes(Engine *self)
{
    Node *prev, *n;
    int i;

    self->stage = NULL;
    self->free_nodes = TV_NODE_POOL;
    self->free_head = &self->free_head_node;
    self->free_head->prev = NULL;
    self->free_head->flags |= NODE_SENTINEL;
    self->free_tail = &self->free_tail_node;
    self->free_tail->next = NULL;
    self->free_tail->flags |= NODE_SENTINEL;

    prev = self->free_head;
    for (i = 0; i < TV_NODE_POOL; i++) {
        n = &self->pool[i];
        prev->next = n;
        n->prev = prev;
        prev = n;
        n->flags = (n->flags & ~1u) | NODE_FREE;
    }
    prev->next = self->free_tail;
    self->free_tail->prev = &self->pool[TV_NODE_POOL - 1];

    self->work_tail = &self->work_tail_node;
    self->work_head = &self->work_head_node;
    self->work_tail->next = NULL;
    self->work_tail->prev = self->work_head;
    self->work_tail->flags |= NODE_SENTINEL;
    self->work_head->next = self->work_tail;
    self->work_head->prev = NULL;
    self->work_head->flags |= NODE_SENTINEL;

    for (i = 0; i < 5; i++) {
        StageCtx *s = &self->stage_ctx[i];
        s->first = NULL;
        s->cur = NULL;
        s->ctl = NULL;
        s->scan = NULL;
        s->last = NULL;
        s->d14 = NULL;
        s->d18 = NULL;
        s->p_1c = self->mode_I;
        s->p_20 = self->mode_P;
        s->rate_index = self->rate_index;
        s->pitch = self->pitch;
        s->volume_atten = self->volume_atten;
        s->p_30 = self->rate_class;
        s->p_34 = self->flags_N;
        s->p_38 = self->flags_A;
        s->voice = self->voice;
    }
}

/* Append a node to the end of the work list; it joins the first stage's
 * window. */
/* @0x10029600 */
Node *TV_THISCALL Engine_AppendNode(Engine *self, int32_t type, int32_t value)
{
    StageCtx *s0 = &self->stage_ctx[0];
    Node *n = Engine_NodeAlloc(self, self->work_tail, 0, type, (uint8_t)value);
    s0->last = n;
    if (s0->scan == NULL)
        s0->scan = n;
    if (s0->ctl == NULL)
        s0->ctl = n;
    if (s0->cur == NULL)
        s0->cur = n;
    if (s0->first == NULL)
        s0->first = n;
    return n;
}

/* Take a node from the free list and link it next to `ref`: after it when
 * `after` is 1, before it otherwise.  If the running stage's window ends (or
 * starts) at `ref`, the window grows to include the new node. */
/* @0x10029670 */
Node *TV_THISCALL Engine_NodeAlloc(Engine *self, Node *ref, int32_t after, int32_t type,
                                   uint8_t value)
{
    Node *n = self->free_head->next;
    StageCtx *st;

    List_Unlink(n);
    self->free_nodes--;
    List_InsertBefore(n, after == 1 ? ref->next : ref);
    n->flags = (n->flags & ~NODE_TYPE_MASK) | ((uint32_t)type & NODE_TYPE_MASK);
    n->value = value;
    n->arg = 0;
    n->b15 = 0;
    n->b19 = 0;
    n->b14 = 0;
    n->flags &= ~0xf8u;

    st = self->stage;
    if (st != NULL) {
        if (st->last == ref && after == 1) {
            st->last = n;
            return n;
        }
        if (st->first == ref && after == 0)
            st->first = n;
    }
    return n;
}

/* Next node within the running stage's window, or NULL at its end. */
/* @0x100298e0 */
Node *TV_THISCALL Engine_StageNext(Engine *self, Node *n)
{
    if (self->stage->last == n)
        return NULL;
    return n->next;
}

/* Previous node within the running stage's window, or NULL at its start. */
/* @0x10029730 */
Node *TV_THISCALL Engine_StagePrev(Engine *self, Node *n)
{
    if (self->stage->first == n)
        return NULL;
    return n->prev;
}

/* Return a node to the free list, shrinking the running stage's window if
 * the node was at either end.  Returns the neighbour in direction `forward`
 * (1 = next, 0 = previous), or NULL if that would leave the window. */
/* @0x10029840 */
Node *TV_THISCALL Engine_NodeFree(Engine *self, Node *n, int32_t forward)
{
    Node *ret = forward == 1 ? n->next : n->prev;
    StageCtx *st = self->stage;

    if (st != NULL) {
        if (n == st->last) {
            if (st->first == st->last) {
                st->last = NULL;
                self->stage->first = NULL;
            } else {
                st->last = st->last->prev;
            }
            if (forward == 1)
                ret = NULL;
        }
        st = self->stage;
        if (n == st->first) {
            st->first = st->first->next;
            if (forward == 0)
                ret = NULL;
        }
    }
    List_Unlink(n);
    List_InsertBefore(n, self->free_tail);
    self->free_nodes++;
    n->flags = (n->flags & ~1u) | NODE_FREE;
    return ret;
}

/* Make `st` the running stage.  Advances its scan pointer to the first node
 * the stage handles and executes any control commands its control pointer
 * has reached.  Returns whether there is a node for the stage to work on. */
/* @0x10029a60 */
uint8_t TV_THISCALL Engine_StageBegin(Engine *self, StageCtx *st)
{
    Node *n, *c;

    self->stage = st;
    n = st->scan;
    for (; n != NULL; n = Engine_StageNext(self, n))
        if (self->stage->type_mask & g_node_type_bits[NODE_TYPE(n)])
            break;
    self->stage->scan = n;

    for (;;) {
        c = self->stage->ctl;
        if (c == NULL)
            break;
        if (self->stage->type_mask & g_node_type_bits[NODE_TYPE(c)])
            break;
        if (!Engine_RunControl(self))
            break;
        st = self->stage;
        c = st->ctl;
        st->ctl = Engine_StageNext(self, c);
        if (self->stage->cur == c)
            self->stage->cur = self->stage->ctl;
    }
    return n != NULL;
}

/* Finish the running stage: nodes before its cursor move into the next
 * stage's window (or, for the last stage, back to the free list), and the
 * window restarts at the cursor.  Returns whether anything was handed on. */
/* @0x10029750 */
uint8_t TV_THISCALL Engine_StageEnd(Engine *self)
{
    StageCtx *st = self->stage;
    StageCtx *nx = st + 1;
    Node *first = st->first;
    Node *cur, *done_last;
    uint8_t moved = 0;

    if (first != NULL && st->cur != first) {
        cur = st->cur;
        moved = 1;
        done_last = cur != NULL ? cur->prev : st->last;
        if (st != &self->stage_ctx[4]) {
            nx->last = done_last;
            if (nx->scan == NULL)
                nx->scan = self->stage->first;
            if (nx->ctl == NULL)
                nx->ctl = self->stage->first;
            if (nx->cur == NULL)
                nx->cur = self->stage->first;
            if (nx->first == NULL)
                nx->first = self->stage->first;
        } else if (cur != first) {
            do {
                StageCtx *s = self->stage;
                s->first = Engine_NodeFree(self, s->first, 1);
            } while (self->stage->cur != self->stage->first);
        }
        st = self->stage;
        if (st->cur == NULL) {
            st->first = NULL;
            self->stage->ctl = NULL;
            self->stage->scan = NULL;
            self->stage->last = NULL;
        } else {
            st->first = st->cur;
        }
    }
    self->stage = NULL;
    return moved;
}
