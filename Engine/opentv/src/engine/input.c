/*
 * Input stage: turns the preformatted character stream (mid_ring) into
 * nodes on the work list.
 *
 * Plain characters become type-1 nodes.  ESC introduces an embedded control
 * command:  ESC <letter> <n> <n argument bytes>  where n is 1..4; the command
 * becomes a type-0 node that the pipeline stages execute as they reach it.
 */
#include "engine.h"

/* Read one embedded control command after its ESC. */
static void read_control(Engine *self)
{
    Node *n = Engine_AppendNode(self, NODE_CONTROL, 0x1b);
    int32_t v;

    n->value = (uint8_t)Engine_MidGet(self);
    switch (Engine_MidGet(self)) {
    case 1:
        n->arg = (uint32_t)Engine_MidGet(self);
        n->b15 = 0;
        if (n->value == 'i')
            n->notify = self->item_notify;
        break;
    case 2:
        n->arg = (uint32_t)Engine_MidGet(self);
        n->b15 = (uint8_t)Engine_MidGet(self);
        if (n->value == 'A')
            self->in_flags_A = ((int32_t)n->b15 << 8) | (uint8_t)n->arg;
        if (n->value == 'N')
            self->in_flags_N = ((int32_t)n->b15 << 8) | (uint8_t)n->arg;
        break;
    case 3:
        /* first byte: flag bits for the node, then a 2-byte argument */
        v = Engine_MidGet(self);
        n->flags = (n->flags & ~0x18u) | (((uint32_t)v << 3) & 0x18u);
        if (v & 4)
            n->flags |= 0x20;
        if (v & 8)
            n->flags |= 0x40;
        n->arg = (uint32_t)Engine_MidGet(self);
        n->b15 = (uint8_t)Engine_MidGet(self);
        break;
    case 4:
        n->arg = (uint32_t)Engine_MidGet(self);
        n->arg |= (uint32_t)Engine_MidGet(self) << 8;
        n->arg |= (uint32_t)Engine_MidGet(self) << 16;
        n->arg |= (uint32_t)Engine_MidGet(self) << 24;
        if (n->value == 'i')
            n->notify = self->item_notify;
        break;
    default:
        n->arg = 0;
        n->b15 = 0;
        break;
    }
}

/* Append up to 10 characters, stopping at the start of the next word (the
 * first non-space after a space).  One character of lookahead is kept in
 * e_9180 (-1 when empty).  Returns whether anything was appended. */
/* @0x10027ef0 */
uint8_t TV_THISCALL Engine_InputStage(Engine *self)
{
    int32_t count = 0;
    uint8_t saw_space = 0;
    uint8_t appended = 0;
    int32_t c;
    Node *n;

    if (self->e_9180 == -1) {
        self->e_9180 = Engine_MidGet(self);
        if (self->e_9180 == -1)
            return 0;
    }
    for (;;) {
        c = self->e_9180;
        if (c == ' ')
            saw_space = 1;
        else if (saw_space)
            break;
        if (count++ >= 10)
            break;

        if (c == 0x1b) {
            read_control(self);
        } else if (c == '[' && (self->in_flags_A & 0x20) && !(self->in_flags_N & 3)) {
            n = Engine_AppendNode(self, NODE_CONTROL, 'I');
            n->arg = (self->in_flags_A & 0x40) ? 2 : 1;
        } else if (c == ']' && (self->in_flags_A & 0x20) && !(self->in_flags_N & 3)) {
            n = Engine_AppendNode(self, NODE_CONTROL, 'I');
            n->arg = 0;
        } else {
            Engine_AppendNode(self, 1, c);
        }

        self->e_9180 = -1;
        appended = 1;
        self->e_9180 = Engine_MidGet(self);
        if (self->e_9180 == -1)
            break;
    }
    return appended;
}
