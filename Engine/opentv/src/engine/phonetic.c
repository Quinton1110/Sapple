/*
 * Stage 0 in phonetic input mode.
 *
 * With the bracket syntax enabled (ESC[6;7A, then "[...]") the text inside
 * the brackets is read as phoneme names rather than words, and stage 0 runs
 * this instead of the rule interpreter.  Each character is rewritten in
 * place: a letter pair such as "AH", "IY" or "UW" collapses to the single
 * engine symbol for that phoneme, and the punctuation carries the stress and
 * boundary codes over to the node stage 0 is about to emit.
 */
#include "engine.h"

/* A two-letter phoneme name: drop the second letter and leave the first
 * holding the engine's symbol for it. */
static void merge(Engine *self, Node *nx, uint8_t v)
{
    StageCtx *st = &self->stage_ctx[0];
    Node *r = Engine_NodeFree(self, nx, 0);

    st->scan = r;
    r->value = v;
}

/* Is this the "]" that closes the bracket? */
static int32_t at_close(Node *nx)
{
    return NODE_TYPE(nx) == 0 && nx->value == 'I' && nx->arg == 0;
}

/* @0x10032230 */
void TV_THISCALL Stage0_Phonetic(Engine *self)
{
    StageCtx *st = &self->stage_ctx[0];
    Node *scan = st->scan;
    Node *nx = scan->next;
    Node *pv = scan->prev;
    Node *nn = nx->next;
    Node *pending = self->s0_pending_ptr;
    uint32_t pv_type = pv->flags & 7u;
    int32_t k = (int32_t)(int8_t)scan->value - 0x20;
    int32_t d;

    if ((uint32_t)k > 0x5e) {
        scan->value = 0;
        return;
    }
    d = (int8_t)nx->value;

    switch (k + 0x20) {
    case ' ': case '(': case ')': case ',': case '.': case ';': case '?':
        return;

    case '!': case '"': case '~':
        if (at_close(nx)) {
            if (pv_type == 3) {
                scan->value = '"';
                return;
            }
            pending->arg = 9;
            pending->flags |= 0x20u;
            st->scan->value = 0;
            return;
        }
        scan->value = '"';
        return;

    case '#':
        if (at_close(nx) && pv_type != 3) {
            pending->arg = 0xb;
            pending->flags |= 0x20u;
            st->scan->value = 0;
            return;
        }
        scan->value = ' ';
        st->scan->arg = 8;
        return;

    case '$':
        if (!at_close(nx))
            return;
        pending->arg = 0xd;
        pending->flags |= 0x20u;
        st->scan->value = 0;
        return;

    case '%': case '&':
        /* take the code the punctuation before left pending */
        if (!(pending->flags & 0x20u))
            return;
        scan->arg = pending->arg;
        st->scan->b15 = pending->b15;
        pending->flags &= ~0x20u;
        return;

    case '\'':
        if (!at_close(nx))
            return;
        if (pv_type == 3) {
            scan->value = '1';
            return;
        }
        pending->arg = 3;
        pending->flags |= 0x20u;
        return;

    case '*':
        if (at_close(nx)) {
            pending->arg = 4;
            pending->flags |= 0x20u;
        }
        st->scan->value = 0;
        return;

    case '+':
        scan->value = '[';
        return;

    case '-':
        if (at_close(nx)) {
            pending->arg = 5;
            pending->flags |= 0x20u;
            st->scan->value = 0;
            return;
        }
        scan->value = '1';
        return;

    case '/':
        if (at_close(nx)) {
            pending->arg = 7;
            pending->flags |= 0x20u;
            st->scan->value = 0;
        } else if (nx->value == '\\' && at_close(nn)) {
            pending->arg = 8;
            pending->flags |= 0x20u;
            st->scan = Engine_NodeFree(self, nx, 0);
        }
        st->scan->value = 0;
        return;

    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        if (d >= '0' && d <= '9') {
            Stage0_PhoneticPair(self);
            return;
        }
        if (d == 'I') {
            Stage0_PhoneticDigit(self);
            return;
        }
        scan->value = 0;
        return;

    case ':':
        if (at_close(nx)) {
            pending->arg = 0xa;
            pending->flags |= 0x20u;
        }
        st->scan->value = 0;
        return;

    case '<':
        if (at_close(nx))
            self->synth_hold = 1;
        scan->value = 0;
        return;

    case '=':
        if (at_close(nx)) {
            pending->arg = 1;
            pending->flags |= 0x20u;
        }
        st->scan->value = 0;
        return;

    case '>':
        if (at_close(nx)) {
            pv->value = 0;
            st->scan->prev->flags &= ~7u;
            st->scan->value = 0;
            st->scan->flags &= ~7u;
            st->scan->next->value = 'C';
            st->p_1c = 0;
            self->synth_hold = 0;
            return;
        }
        scan->value = 0;
        return;

    case '@':
        scan->value = 0;
        return;

    case 'A': case 'a':
        if (d == 'A' || d == 'H' || d == 'a' || d == 'h')
            merge(self, nx, 'o');
        else if (d == 'E' || d == 'e')
            merge(self, nx, 'a');
        else if (d == 'O' || d == 'o')
            merge(self, nx, 'w');
        else if (d == 'R' || d == 'r')
            merge(self, nx, 'r');
        else if (d == 'W' || d == 'w')
            merge(self, nx, 'f');
        else if (d == 'X' || d == 'x')
            merge(self, nx, '@');
        else if (d == 'Y' || d == 'y')
            merge(self, nx, 'I');
        else
            scan->value = 'o';
        return;

    case 'B': case 'b':
        scan->value = 'B';
        return;

    case 'C': case 'c':
        if (d == 'H' || d == 'h')
            merge(self, nx, 'C');
        else
            scan->value = 'C';
        return;

    case 'D': case 'd':
        if (d == 'H' || d == 'h')
            merge(self, nx, 'x');
        else if (d == 'T' || d == 't')
            merge(self, nx, 't');
        else
            scan->value = 'D';
        return;

    case 'E': case 'e':
        if (d == 'H' || d == 'h')
            merge(self, nx, 'e');
        else if (d == 'R' || d == 'r')
            merge(self, nx, 'k');
        else if (d == 'Y' || d == 'y')
            merge(self, nx, 'A');
        else
            scan->value = 'e';
        return;

    case 'F': case 'f':
        scan->value = 'F';
        return;

    case 'G': case 'g':
        scan->value = 'G';
        return;

    case 'H': case 'h':
        if (d == 'H' || d == 'h')
            merge(self, nx, 'd');
        else if (d == 'W' || d == 'w')
            merge(self, nx, 'h');
        else if (d == 'X' || d == 'x')
            merge(self, nx, 'H');
        else
            scan->value = 'H';
        return;

    case 'I': case 'i':
        if (d == 'H' || d == 'h')
            merge(self, nx, 'i');
        else if (d == 'R' || d == 'r')
            merge(self, nx, '4');
        else if (d == 'X' || d == 'x')
            merge(self, nx, '|');
        else if (d == 'Y' || d == 'y')
            merge(self, nx, 'E');
        else
            scan->value = 'i';
        return;

    case 'J': case 'j':
        if (d == 'H' || d == 'h')
            merge(self, nx, 'J');
        else
            scan->value = 'J';
        return;

    case 'K': case 'k':
        scan->value = 'K';
        return;

    case 'L': case 'l':
        if (d == 'X' || d == 'x')
            merge(self, nx, 'j');
        else
            scan->value = 'L';
        return;

    case 'M': case 'm':
        scan->value = 'M';
        return;

    case 'N': case 'n':
        if (d == 'G' || d == 'X' || d == 'g' || d == 'x')
            merge(self, nx, '~');
        else
            scan->value = 'N';
        return;

    case 'O': case 'o':
        if (d == 'R' || d == 'r')
            merge(self, nx, 'g');
        else if (d == 'W' || d == 'w')
            merge(self, nx, 'O');
        else if (d == 'Y' || d == 'y')
            merge(self, nx, 'y');
        else
            scan->value = 'o';
        return;

    case 'P': case 'p':
        scan->value = 'P';
        return;

    case 'Q': case 'q':
        if (d == 'Q' || d == 'q')
            merge(self, nx, 'q');
        else
            scan->value = 'Q';
        return;

    case 'R': case 'r':
        if (d == 'R' || d == 'r')
            merge(self, nx, '3');
        else
            scan->value = 'R';
        return;

    case 'S': case 's':
        if (d == 'H' || d == 'h')
            merge(self, nx, 's');
        else
            scan->value = 'S';
        return;

    case 'T': case 't':
        if (d == 'H' || d == 'h')
            merge(self, nx, 'X');
        else
            scan->value = 'T';
        return;

    case 'U': case 'u':
        if (d == 'H' || d == 'h')
            merge(self, nx, 'u');
        else if (d == 'L' || d == 'l')
            merge(self, nx, 'l');
        else if (d == 'M' || d == 'm')
            merge(self, nx, 'm');
        else if (d == 'N' || d == 'n')
            merge(self, nx, 'n');
        else if (d == 'R' || d == 'r')
            merge(self, nx, 'c');
        else if (d == 'W' || d == 'w')
            merge(self, nx, 'b');
        else if (d == 'X' || d == 'x')
            merge(self, nx, 'v');
        else
            scan->value = 'u';
        return;

    case 'V': case 'v':
        scan->value = 'V';
        return;

    case 'W': case 'w':
        scan->value = 'W';
        return;

    case 'X': case 'x':
        if (d == 'X' || d == 'x')
            merge(self, nx, 'p');
        else
            scan->value = 0;
        return;

    case 'Y': case 'y':
        if (d == 'R' || d == 'r')
            merge(self, nx, '5');
        else
            scan->value = 'Y';
        return;

    case 'Z': case 'z':
        if (d == 'H' || d == 'h') {
            Engine_NodeFree(self, nx, 0);
            st->scan->value = 'z';
        } else {
            scan->value = 'Z';
        }
        return;

    case '\\':
        if (at_close(nx)) {
            pending->arg = 6;
            pending->flags |= 0x20u;
        }
        st->scan->value = 0;
        return;

    case '_':
        if (at_close(nx)) {
            pending->arg = 0x11;
            pending->flags |= 0x20u;
        }
        st->scan->value = 0;
        return;

    case '`':
        if (at_close(nx) && pv_type != 3) {
            pending->arg = 2;
            pending->flags |= 0x20u;
            st->scan->value = 0;
            return;
        }
        scan->value = '2';
        return;

    case '{':
        if (at_close(nx)) {
            pending->arg = 0xc;
            pending->flags |= 0x20u;
        }
        st->scan->value = 0;
        return;

    case '|':
        scan->value = '\\';
        return;

    case '}':
        scan->value = ']';
        return;

    default: /* '[', ']', '^' */
        scan->value = 0;
        return;
    }
}

/* A digit before "]": the number becomes the pending code. */
/* @0x10033290 */
void TV_THISCALL Stage0_PhoneticDigit(Engine *self)
{
    StageCtx *st = &self->stage_ctx[0];
    Node *scan = st->scan;
    Node *nx = scan->next;

    if (NODE_TYPE(nx) == 0 && nx->arg == 0) {
        self->s0_pending_ptr->b15 = scan->value;
        self->s0_pending_ptr->b15 = (uint8_t)(self->s0_pending_ptr->b15 - 0x30);
        self->s0_pending_ptr->flags |= 0x20u;
    }
    st->scan->value = 0;
}

/* Two digits before "]": the pair becomes the pending code, capped at 63. */
/* @0x100332e0 */
void TV_THISCALL Stage0_PhoneticPair(Engine *self)
{
    StageCtx *st = &self->stage_ctx[0];
    Node *scan = st->scan;
    Node *nx = scan->next;
    Node *nn = nx->next;
    Node *r;

    if (NODE_TYPE(nn) == 0 && nn->value == 'I' && nn->arg == 0) {
        self->s0_pending_ptr->b15 = nx->value;
        self->s0_pending_ptr->b15 = (uint8_t)((int8_t)scan->value * 10);
        if (self->s0_pending_ptr->b15 > 0x3f)
            self->s0_pending_ptr->b15 = 0x3f;
        self->s0_pending_ptr->flags |= 0x20u;
    }
    r = Engine_NodeFree(self, scan->next, 0);
    r->value = 0;
}
