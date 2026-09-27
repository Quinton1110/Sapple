/*
 * Stage 2: timing.
 *
 * Stage 2 walks the phoneme nodes stage 1 produced and turns them into the
 * durations and boundary marks stage 3 needs.  It runs in steps like the
 * other stages, but it also looks ahead to the next comma or clause break:
 * when it reaches one it rewinds to where it started and makes a second pass
 * over the same nodes with the break now known, which is what the small
 * state machine in Stage2_Step is doing.
 */
#include "engine.h"

int tv_ext_rate = 0;

/* Which row to actually read.  The original runs off the end of the table
 * above row 25 and this reproduces that faithfully when the extension is
 * off; with it on the index has already been clamped by Engine_SetSpeed, so
 * the added rows reuse the fastest row the original had. */
static int32_t rate_row(int32_t i)
{
    return (tv_ext_rate && i >= TV_RATE_ROWS) ? TV_RATE_ROWS - 1 : i;
}

/* How much to shorten durations by, as a percentage.  Always 100 for every
 * row the original had, so nothing it could say changes; the rows OpenTV
 * adds run down to 40, which is where per-phoneme minimum durations stop it
 * making any further difference. */
static int32_t rate_pct(int32_t i)
{
    if (!tv_ext_rate || i < TV_RATE_ROWS)
        return 100;
    if (i > TV_RATE_ROW_MAX)
        i = TV_RATE_ROW_MAX;
    return 100 - (i - (TV_RATE_ROWS - 1)) * 60 / (TV_RATE_ROW_MAX - (TV_RATE_ROWS - 1));
}

/* Shorten a duration for the added rate rows.
 *
 * Only the part above the phoneme's own minimum is compressed.  Stage 2
 * works out every duration as minimum + a share of the range above it, and
 * Stage2_MinDur clamps to that same minimum, because below it a consonant
 * stops reading as one.  Scaling the whole figure walks straight through
 * that floor and is what made fast speech mumble: the steady parts and the
 * consonants were squeezed alike.  Real fast speech takes the time out of
 * the steady parts.
 *
 * Left exactly alone at 100, so the classic rows are bit-for-bit what they
 * always were rather than a multiply and divide that happens to round back. */
/* How much of a phoneme's *minimum* survives.  By row 25 every duration is
 * already sitting on its minimum -- that is why the original's fastest row
 * only reached 1.86x -- so going faster means taking some of the minimum
 * too.  Consonants are almost all minimum and vowels are mostly the part
 * above it, so compressing the two at different rates takes the time out of
 * the steady parts and leaves the consonants closer to intact. */
static int32_t rate_floor_pct(int32_t i)
{
    if (!tv_ext_rate || i < TV_RATE_ROWS)
        return 100;
    if (i > TV_RATE_ROW_MAX)
        i = TV_RATE_ROW_MAX;
    return 100 - (i - (TV_RATE_ROWS - 1)) * 30 / (TV_RATE_ROW_MAX - (TV_RATE_ROWS - 1));
}

/* How much of the pitch excursion survives at high rates.
 *
 * Stage2_Contour lays its contour out across the phrase's *phoneme count*
 * -- pitch -= (pos * v) / total -- so the shape is already rate-invariant:
 * it completes whatever the speed.  That is the opposite of TGSpeechBox,
 * whose declination is hertz per second and is multiplied by speed to make
 * it finish in time.  Nothing is missing here, then; the problem is the
 * other one.  The same excursions are traversed three times faster, and a
 * pitch movement crammed into a third of the time does not read as the same
 * intonation.  People flatten their pitch when they speak quickly rather
 * than moving it faster, so this takes the range down as the rate climbs. */
static int32_t rate_pitch_pct(int32_t i)
{
    if (!tv_ext_clarity || i < TV_RATE_ROWS)
        return 100;
    if (i > TV_RATE_ROW_MAX)
        i = TV_RATE_ROW_MAX;
    return 100 - (i - (TV_RATE_ROWS - 1)) * 30 / (TV_RATE_ROW_MAX - (TV_RATE_ROWS - 1));
}

static int32_t rate_apply(const Engine *self, int32_t i, int32_t v)
{
    int32_t pct = rate_pct(i), lo;

    if (pct == 100)
        return v;
    lo = self->s2_1ddc / 10;
    if (lo > v)
        lo = v;
    return lo * rate_floor_pct(i) / 100 + (v - lo) * pct / 100;
}


/* The look-ahead state is shared between engine objects, the way the
 * original has it. */
/* @0x101489e0 */ extern int32_t g_s2_state;
/* @0x101489ec */ extern int32_t g_s2_saved;

/* @0x100c8aa0 with the signed index the original uses; see stage0.c */
uint8_t Phone_Attr(int32_t idx);

/* @0x1002b2b0 */
uint8_t TV_THISCALL Stage2_Run(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    int32_t r, r2;
    uint8_t stop = 0;

    Engine_StageBegin(self, st);
    self->s2_1d58 = 2;
    if (self->s2_1d55 == 0 && self->s2_1d54 != 0 && st->rate_index < 0x13)
        self->s2_1d5c = 0x46;

    r = Stage2_Begin(self);
    switch (r) {
    case 1:
    case 4:
        self->s2_1d68 = (self->s2_1d60 >> 2) + 4;
        self->s2_1d60 += self->s2_1d68;
        /* fall through */
    case 3:
    case 5:
        st->ctl = st->cur;
        /* fall through */
    case 2:
        if (self->s2_1d55 == 0)
            self->s2_1d54 = 0;
        break;
    default:
        break;
    }

    while (r != 0) {
        r2 = Stage2_Next(self);
        if (r2 == 1) {
            r = 0;
        } else if (r2 == 2) {
            Engine_RunControl(self);
        } else if (r2 == 3) {
            Stage2_Emit(self);
        } else if (r2 == 4) {
            stop = 1;
            r = 0;
        }
    }

    if (Engine_StageEnd(self) != 0 || stop != 0)
        return 1;
    return 0;
}

/* Advance the stage 2 cursor, handling the rewind over a clause break.
 * Returns 1 when a rewind finished, 2 for a node the control executor should
 * see, 3 for a phoneme, 4 when the stage must stop for now. */
/* @0x1002a9b0 */
int32_t TV_THISCALL Stage2_Next(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *n, *stop;

    if (self->s2_1d6c != 2) {
        g_s2_state = 1;
        g_s2_saved = self->s2_1d6c;
    }
    self->s2_1d6c = 2;

    if (g_s2_state == 3) {
        /* second pass done: clear the pauses we marked and rewind */
        n = st->cur;
        stop = Engine_StageNext(self, st->scan);
        if (stop != n) {
            do {
                if (NODE_TYPE(n) == 4 && n->value == ' ')
                    n->flags &= ~0x18u;
                n = Engine_StageNext(self, n);
            } while (stop != n);
        }
        st->scan = stop;
        st->ctl = stop;
        st->cur = stop;
        self->s2_1d6c = g_s2_saved;
        g_s2_state = 1;
        return 1;
    }

    n = st->ctl;
    if (NODE_TYPE(n) == 3 && n->value == ',' && self->s2_1d55 == 0 &&
        self->s2_1d70 == 0 && g_s2_state != 1) {
        Node *p = st->cur;

        stop = Engine_StageNext(self, n);
        while (p != NULL && stop != p) {
            if (NODE_TYPE(p) == 4 && p->value == ' ')
                p->flags &= ~0x18u;
            p = Engine_StageNext(self, p);
        }
        st->cur = st->ctl;
        st->ctl = stop;
        g_s2_state = 1;
        return 4;
    }

    self->s2_1d58--;
    if (self->s2_1d58 < 0) {
        if (self->trk_0c - self->trk_04 - self->s3_1fe0 <= 0x20)
            return 4;
    }

    if (g_s2_state == 2)
        st->ctl = Engine_StageNext(self, st->ctl);
    else
        g_s2_state = 2;

    if (st->scan == st->ctl)
        g_s2_state = 3;

    return NODE_TYPE(st->ctl) == 3 ? 3 : 2;
}

/* A phoneme node: work out its duration and hand it to stage 3. */
/* @0x1002a910 */
void TV_THISCALL Stage2_Emit(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *n;

    Stage2_Context(self);
    Stage2_Push(self, 0);
    Stage2_Close(self);

    if (Phone_Attr((int8_t)self->s2_1db1) & 8) {
        if (st->ctl->arg == 0xc) {
            Stage2_Break(self, self->s2_1d88);
            self->s2_1d88 = 0;
        }
    }
    if (Phone_Attr((int16_t)((int16_t)(int8_t)self->s2_1db1 | 0x80)) & 0x40)
        Stage2_Break(self, 0);

    n = st->ctl;
    if (Phone_Attr((int8_t)n->value) & 0x80) {
        n->flags = (n->flags & ~3u) | 4u;
        self->s2_1d88++;
        Stage2_Push(self, 1);
    }
}

/* Test the attribute bits of a node's phoneme.  The mask's high byte picks
 * the bank; a negative mask, or a negative `neg`, inverts the answer. */
/* @0x1002a720 */
uint8_t TV_CDECL Phone_TestMask(Node *n, int32_t mask, int32_t neg)
{
    uint8_t r = 0;
    int32_t inv = 0;

    if (n == NULL)
        return 0;
    if (mask < 0) {
        mask = -mask;
        inv = 1;
    }
    r = (uint8_t)(Phone_Attr((int32_t)(int8_t)n->value |
                             ((mask & 0xff00) >> 1)) & (uint8_t)mask);
    if (inv)
        r = (uint8_t)(r == 0);
    if (neg < 0)
        r = (uint8_t)(r == 0);
    return r;
}

/* The next real phoneme after n, skipping the "&" and "%" markers. */
/* @0x1002b490 */
Node *TV_THISCALL Stage2_NextPhone(Engine *self, Node *n)
{
    uint8_t c;

    if (n == NULL)
        return NULL;
    n = Engine_StageNext(self, n);
    while (n != NULL) {
        if (NODE_TYPE(n) == 3) {
            c = n->value;
            if (c != '&' && c != '%')
                return n;
        }
        n = Engine_StageNext(self, n);
    }
    return NULL;
}

/* @0x10027c40 */
void TV_THISCALL Stage2_Close(Engine *self)
{
    Stage2_Pitch(self);
    Stage2_Contour(self);
    Stage2_Flush(self);
}

/* Nominal and minimum duration for each phoneme, two bytes each. */
/* @0x100ee710 */ extern const uint8_t g_phone_dur[];

/* @0x10027480 */
void TV_THISCALL Stage2_ResetRun(Engine *self)
{
    self->s2_1d90 = 0;
    self->s2_1d8c = 0;
    self->s2_1d94 = 0;
    self->s2_stressed = NULL;
    self->s2_1d9b = 0;
    self->s2_1d9a = 0;
    self->s2_1d99 = 0;
    self->s2_1d98 = 0;
    self->s2_1da0 = 4;
    self->s2_1da4 = 0;
}

/* Settle this phoneme's duration and store it in the node's arg. */
/* @0x10055fd0 */
void TV_THISCALL Stage2_Flush(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    int32_t i = (int32_t)(int8_t)self->s2_1db1 * 2;

    self->s2_1dd8 = (int32_t)g_phone_dur[i] * 10;
    self->s2_dur[0] = 0x64;
    self->s2_1ddc = (int32_t)g_phone_dur[i + 1] * 10;

    if (self->s2_1da8 == 0) {
        Stage2_Silence(self);
        return;
    }
    if (self->s2_1d50 != 0) {
        st->ctl->arg = (uint32_t)self->s2_1d50;
        return;
    }
    if (st->p_38 & 0x10) {
        st->ctl->arg = (uint32_t)Stage2_DurFast(self);
        return;
    }
    st->ctl->arg = (uint32_t)rate_apply(self, st->rate_index,
                                        Stage2_DurRules(self));
    st->ctl->arg = (uint32_t)Stage2_DurAdjust(self);
}

/* Pause length per speaking rate. */
/* @0x100ee698 */ extern const int32_t g_pause_rate[];

/* No duration rules wanted: just classify the node. */
/* @0x10058060 */
void TV_THISCALL Stage2_Silence(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *n = st->ctl;

    if (!(Phone_Attr((int8_t)n->value) & 8)) {
        n->arg = 0;
        return;
    }
    if (n->arg != 0xa) {
        self->s2_1e1c = 0;
        return;
    }
    if (n->b15 != 0 && n->b15 != 6)
        self->s2_1e1c = 2;
    else
        self->s2_1e1c = 1;
}

/* The short duration rule used when flags_A bit 4 is on. */
/* @0x1005aa90 */
int32_t TV_THISCALL Stage2_DurFast(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *n = st->ctl;
    uint8_t c = n->value;
    int32_t v = g_phone_dur[(int32_t)(int8_t)c * 2 + 1];

    if (c == ' ') {
        v >>= 2;
        if (v < 4)
            v = 4;
        return v;
    }
    v = (v + (v << 4)) / 20;
    if (!(n->flags & 0x20u))
        v >>= 1;
    if (v < 2)
        v = 2;
    return v;
}

/* A pause longer than 50 frames is split into several nodes. */
/* @0x100580b0 */
int32_t TV_THISCALL Stage2_Pause(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *n = st->ctl;
    Node *x;
    int32_t v, total, chunk;

    v = g_pause_rate[rate_row(st->rate_index)];
    v = (v + ((v >> 31) & 3)) >> 2;
    total = (int32_t)((uint32_t)(v * (int32_t)n->arg) / 100u) * 4;
    if (total < 4)
        total = 4;
    if (total > 0x32) {
        chunk = 0x32;
        while (self->free_nodes > 3) {
            total -= 0x32;
            x = Engine_NodeAlloc(self, n, 0, 4, ' ');
            x->arg = (uint32_t)chunk;
            x->b15 = (uint8_t)chunk;
            if (total <= chunk)
                break;
        }
    }
    n->arg = (uint32_t)(total < 0x32 ? total : 0x32);
    if (n->next->value != '&')
        n->b15 = 0x32;
    Stage2_Context(self);
    return total;
}

/* Minimum-duration percentages per speaking rate. */
/* @0x100ee630 */ extern const int32_t g_dur_rate[];

/* Scale the phoneme's minimum duration by a percentage, clamped to the
 * table's floor and to 55 frames. */
/* @0x1005bfc0 */
int32_t TV_THISCALL Stage2_MinDur(Engine *self, int32_t pct)
{
    StageCtx *st = &self->stage_ctx[2];
    int32_t lo = self->s2_1ddc / 10;
    int32_t base = g_dur_rate[rate_row(st->rate_index)];
    int32_t v;

    v = (int32_t)((uint32_t)(pct + pct * 9 * 5 * 2) / 100u);
    v = (int32_t)((uint32_t)(base * v) / 100u);
    if (v < lo)
        return lo;
    if (v >= 0x37)
        return 0x37;
    return v;
}

/* Walk up to `count` nodes in `dir` looking for one whose attributes match
 * `mask`; returns it, or NULL. */
/* @0x1002b680 */
Node *TV_THISCALL Stage2_Find(Engine *self, int32_t dir, int32_t count,
                              int32_t mask)
{
    Node *n = self->stage_ctx[2].ctl;

    if (count <= 0)
        return NULL;
    do {
        if ((uint8_t)dir == 1) {
            n = Node_NextWord(self, n);
            if (n == NULL)
                return NULL;
        } else {
            n = Node_PrevBoundary(self, n);
            if (n == NULL)
                return NULL;
        }
        if (Phone_TestMask(n, mask, 0))
            return n;
        count--;
    } while (count > 0);
    return NULL;
}

/* Does the run of up to `count` nodes in `dir` match the stress pattern the
 * two masks describe?  `mode` 1 and 2 replace the attribute test on the
 * respective mask with a test of the node's own stress bits. */
/* @0x1002b550 */
uint8_t TV_THISCALL Stage2_Scan(Engine *self, int32_t dir, int32_t count,
                                int32_t mask1, int32_t mask2, int32_t mode)
{
    Node *n = self->stage_ctx[2].ctl;
    uint8_t result = 0;
    uint32_t st;
    int32_t kind, v;

    if (count <= 0)
        return 0;
    for (;;) {
        if ((uint8_t)dir == 1) {
            n = Node_NextWord(self, n);
            if (n == NULL)
                break;
        } else {
            n = Node_PrevBoundary(self, n);
            if (n == NULL)
                break;
        }

        if ((uint8_t)mode != 1) {
            if (Phone_TestMask(n, mask1, 0)) {
                result = 1;
                break;
            }
        } else {
            kind = 0;
            st = n->flags & 0x18u;
            if (st == 0x18) {
                kind = 1;
            } else {
                v = mask1 < 0 ? -mask1 : mask1;
                if (v == 2 && st == 0x10)
                    kind = 2;
            }
            if ((kind != 0 && mask1 > 0) || (kind == 0 && mask1 < 0)) {
                result = 1;
                break;
            }
        }

        if ((uint8_t)mode != 2) {
            if (Phone_TestMask(n, mask2, -1))
                break;
        } else {
            kind = 0;
            st = n->flags & 0x18u;
            if (st == 0x18) {
                kind = 1;
            } else {
                v = mask2 < 0 ? -mask2 : mask2;
                if (v == 2 && st == 0x10)
                    kind = 2;
            }
            if ((kind != 0 && mask2 < 0) || (kind == 0 && mask2 > 0)) {
                result = 0;
                break;
            }
        }

        count--;
        if (count <= 0)
            break;
    }
    return result;
}

/* Give a stop its aspiration, or mark a vowel for the pitch pass. */
/* @0x100271c0 */
void TV_THISCALL Stage2_Aspirate(Engine *self, Node *n, int32_t which)
{
    StageCtx *st = &self->stage_ctx[2];
    uint8_t c;

    if (which == 0) {
        c = n->value;
        if (c != 'P' && c != 'T' && c != 'K' && c != 'B' && c != 'D' && c != 'G')
            return;
        if (st->rate_index >= 0x13)
            return;
        if (Phone_Attr((int16_t)((int16_t)(int8_t)c | 0x80)) & 8)
            return;
        Engine_NodeAlloc(self, n, 1, 3, 'p');
        self->s2_1d64++;
        return;
    }
    if (which != 1)
        return;
    c = n->value;
    if ((Phone_Attr((int16_t)((int16_t)(int8_t)c | 0x100)) & 2) && c != 'U' &&
        st->pitch == 0) {
        n->flags |= 0x40u;
        return;
    }
    if (n->value == 'd')
        n->value = 'H';
}

/* Break the run at one of the remembered marks: insert the phoneme, undo any
 * flapping either side of it, and put a pause command in front. */
/* @0x100270f0 */
void TV_THISCALL Stage2_Split(Engine *self, int32_t slot0, int32_t value,
                              int32_t slot2)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *a, *b, *p;

    if (self->free_nodes < 3)
        return;

    a = Engine_NodeAlloc(self, self->s2_marks[slot2 + 1], 0, 3, (uint8_t)value);
    p = a->next->next;
    if (p->value == 't')
        p->value = p->b18;
    p = a->prev;
    if (p->value == 't')
        p->value = p->b18;

    Stage2_Aspirate(self, Engine_StagePrev(self, a), 0);
    Stage2_Aspirate(self, Engine_StageNext(self, self->s2_marks[slot2 + 1]), 1);

    b = Engine_NodeAlloc(self, self->s2_marks[slot0 + 1], 0, 0, 'P');
    b->arg = 3;
    if (self->s2_1d45 != 0) {
        b->b15 = 0;
        self->s2_1d45 = 0;
    } else {
        b->b15 = 1;
    }
    if (b->next == st->cur)
        st->cur = b;
}

/* The node whose duration is being settled, and the values carried with it. */
/* ClassicVoices patch: a C global here, not a 4-byte slot of the data image. In the original this is a 32-bit
 * pointer at 0x101489f0, next to g_s2_total at 0x101489f4; laid out at those offsets, a 64-bit pointer would
 * overwrite g_s2_total (and g_s2_last, below, g_s2_value). Nothing else ever addresses it, so moving it changes no
 * table; on 32-bit it behaves exactly as before. */
Node *g_s2_node;
/* @0x101489e8 */ extern int32_t g_s2_b15;
/* @0x101489d8 */ extern uint8_t g_s2_value;

/* The previous real phoneme, skipping the markers and the silences. */
/* @0x1002b4e0 */
Node *TV_THISCALL Stage2_PrevPhone(Engine *self, Node *n)
{
    uint32_t t;
    uint8_t c;

    if (n == NULL)
        return NULL;
    n = Engine_StagePrev(self, n);
    while (n != NULL) {
        t = NODE_TYPE(n);
        if (t == 3 || t == 4) {
            c = n->value;
            if (c != '&' && c != '%' &&
                !(Phone_Attr((int16_t)((int16_t)(int8_t)c | 0x80)) & 0x40))
                return n;
        }
        n = Engine_StagePrev(self, n);
    }
    return NULL;
}

/* Cache everything the duration rules want to know about the phoneme under
 * the cursor and its neighbours. */
/* @0x10026a60 */
void TV_THISCALL Stage2_Context(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *n = st->ctl;
    int16_t cx = (int16_t)(int8_t)n->value;
    uint8_t lo, x;
    Node *p;

    self->s2_1da5 = (uint8_t)(Phone_Attr((int16_t)(cx | 0x80)) & 8);
    lo = Phone_Attr(cx);
    self->s2_1da6 = (uint8_t)(lo & 0x40);
    self->s2_1da7 = (uint8_t)(lo & 0x10);
    self->s2_1da8 = (uint8_t)(lo & 0x80);
    x = Phone_Attr((int16_t)(cx | 0x100));
    self->s2_1da9 = (uint8_t)(lo & 2);
    self->s2_1daa = (uint8_t)(x & 1);
    self->s2_1dac = (uint8_t)(lo & 1);
    self->s2_1dad = (uint8_t)(x & 2);
    self->s2_1dab = (uint8_t)((n->flags & 0x20u) >> 5);
    self->s2_1db1 = n->value;

    p = Node_NextWord(self, n);
    self->s2_next_word = p;
    self->s2_c_next_word = p != NULL ? p->value : (uint8_t)' ';

    p = Stage2_NextPhone(self, n);
    self->s2_next1 = p;
    self->s2_c_next1 = p != NULL ? p->value : (uint8_t)' ';

    self->s2_next2 = p != NULL ? Stage2_NextPhone(self, p) : NULL;
    p = self->s2_next2;
    self->s2_c_next2 = p != NULL ? p->value : (uint8_t)' ';

    p = Node_PrevBoundary(self, n);
    self->s2_prev_word = p;
    self->s2_c_prev_word = p != NULL ? p->value : (uint8_t)' ';

    p = Stage2_PrevPhone(self, n);
    self->s2_prev1 = p;
    self->s2_c_prev1 = p != NULL ? p->value : (uint8_t)' ';

    p = Stage2_PrevPhone(self, p);
    self->s2_prev2 = p;
    self->s2_c_prev2 = p != NULL ? p->value : (uint8_t)' ';

    if (self->s2_next_word != NULL && (self->s2_next_word->flags & 0x20u))
        self->s2_1daf = 1;
    else
        self->s2_1daf = 0;

    self->s2_1dae = (uint8_t)(Phone_Attr((int8_t)self->s2_c_prev1) & 1);
    self->s2_1db0 = (uint8_t)(Phone_Attr((int8_t)self->s2_c_next1) & 1);
}

/* Remember the node's duration and stress before the rules run (kind 0), and
 * write the adjusted values back afterwards (kind 1). */
/* @0x1002a770 */
void TV_THISCALL Stage2_Push(Engine *self, int32_t kind)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *n = st->ctl;
    int32_t v;

    if (kind == 0) {
        g_s2_node = n;
        g_s2_b15 = n->b15;
        self->s2_1d50 = (int32_t)n->arg;
        g_s2_value = n->value;
        if (g_s2_b15 & 0x80)
            g_s2_b15 |= 0xff00;
        if (self->s2_1d50 & 0x80)
            self->s2_1d50 |= 0xff00;
        v = self->s2_1d48;
        self->s2_1d48 = 0;
        self->s2_1d4c = v;
        if (n->flags & 0x80u)
            self->s2_1d48 = 4;
        if (self->s2_1d50 != 0)
            self->s2_1d48 |= 1;
        if (g_s2_b15 != 0)
            self->s2_1d48 |= 2;
        if (g_s2_value == ' ' && self->s2_1d50 > 0x32)
            self->s2_1d50 = 0x32;
        return;
    }

    if (g_s2_node != n || n->value != g_s2_value)
        return;

    v = self->s2_1d48;
    if (v & 1) {
        if (v & 4) {
            int32_t d = (int32_t)n->arg + self->s2_1d50;

            self->s2_1d50 = d;
            if (d < 2)
                self->s2_1d50 = 2;
            else if (d > 0x3c)
                self->s2_1d50 = 0x3c;
        }
        n->arg = (uint32_t)self->s2_1d50;
    }
    v = self->s2_1d48;
    if (!(v & 2))
        return;
    if (v & 4) {
        g_s2_b15 += n->b15;
        if (g_s2_b15 < 0x19) {
            g_s2_b15 = 0x19;
            n->b15 = (uint8_t)g_s2_b15;
            return;
        }
        if (g_s2_b15 > 0x64) {
            g_s2_b15 = 0x64;
            n->b15 = (uint8_t)g_s2_b15;
            return;
        }
    } else if (g_s2_b15 == 1) {
        g_s2_b15 = 0;
    }
    n->b15 = (uint8_t)g_s2_b15;
}

/* Scale a duration by the node's stress and the clause position. */
/* @0x10056200 */
int32_t TV_THISCALL Stage2_DurStress(Engine *self, int32_t dur)
{
    StageCtx *st = &self->stage_ctx[2];
    int32_t v = dur, s, k;

    if ((st->ctl->flags & 0x18u) == 0x18)
        v = (int32_t)((uint32_t)(v + (v << 7) + v) / 100u);

    s = self->s2_1e1c;
    if (s == 0)
        return v;

    if (self->s2_1dac != 0) {
        k = (self->s2_1dab == 1 ? 0 : -1);
        v = (int32_t)((uint32_t)(((k & ~0x4a) + 0xc8) * v) / 100u);
    } else if (self->s2_1da9 != 0) {
        k = (self->s2_1dab == 1 ? 0 : -1);
        v = (int32_t)((uint32_t)(((k & ~0x18) + 0x96) * v) / 100u);
    } else if (self->s2_1da6 != 0) {
        v = (int32_t)((uint32_t)(v * 125) / 100u);
    }
    if (s == 2)
        v = (int32_t)((uint32_t)(v * 150) / 100u);
    return v;
}

/* Two identical (or homorganic) consonants across a boundary become one
 * longer one.  Returns 1 when the previous node was absorbed. */
/* @0x10056070 */
uint8_t TV_THISCALL Stage2_Merge(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *prev;
    uint8_t pc = ' ', c, d;
    int16_t di;
    int32_t v, same;

    prev = Stage2_PrevPhone(self, st->ctl);
    if (prev != NULL)
        pc = prev->value;
    c = self->s2_1db1;
    di = (int16_t)(int8_t)c;
    self->s2_1e20 = 0;

    if (!(Phone_Attr((int16_t)(di | 0x80)) & 0x20))
        return 0;
    if (st->rate_index <= 6)
        return 0;

    if (pc == c && !(st->ctl->flags & 0x40u))
        goto pair;
    if ((pc == 'S' || pc == 'Z')) {
        if (c == 's')
            goto pair;
        if (pc == 'Z' && c == 'S')
            goto pair_s;
    }
    if (pc == 'V' && c == 'F')
        goto pair;
    if (pc == 'L' && c == 'j')
        goto pair;
    if (pc == 'B' && c == 'P')
        goto pair;
    return 0;

pair:
    if (c != 'S')
        goto merge;
pair_s:
    if (pc != 'S')
        goto merge;
    d = self->s2_c_prev_word;
    if (d == '%' || d == '&') {
        if (Phone_Attr((int16_t)((int16_t)(int8_t)self->s2_c_next_word | 0x80)) & 0x20)
            return 0;
    }

merge:
    same = 0;
    if (pc == c)
        same = 1;
    else if (pc == 'D' && c == 'T')
        same = 1;
    else if (pc == 'B' && c == 'P')
        same = 1;
    else if (c == 'S' && pc == 'Z')
        same = 1;
    if (same) {
        d = Phone_Attr(di);
        if ((d & 0x42) || ((d & 0x20) && c != 'C' && c != 'J' && c != 'q'))
            self->s2_1e20 = 1;
    }

    v = (int32_t)prev->arg + (int32_t)st->ctl->arg;
    if (pc == 'V' && c == 'F')
        v -= 4;
    if (v >= 0x37)
        v = 0x37;
    st->ctl->arg = (uint32_t)v;
    st->ctl->b18 = 1;
    if (self->s2_1dab != 0)
        st->ctl->flags |= 0x20u;
    st->ctl->b15 = prev->b15;
    Engine_NodeFree(self, prev, 0);
    return 1;
}

/* Place-of-articulation classes, used to spot a homorganic pair. */
/* @0x100ef6d8 */ extern const tv_ref g_phone_place;

/* The full duration rules: build up a list of percentage adjustments, apply
 * them all, and interpolate between the phoneme's minimum and nominal
 * duration.  The answer is in tenths of a frame. */
/* @0x100562c0 */
int32_t TV_THISCALL Stage2_DurRules(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *n = st->ctl;
    int32_t idx = (int32_t)(int8_t)self->s2_1db1;
    int32_t i = 0, pct = 100, acc, v, j, range;
    int16_t cx, bx;
    uint8_t a, cl;

    if (Phone_Attr(idx) & 1)
        return 0;
    if (self->s2_1ddc == self->s2_1dd8)
        return g_phone_dur[idx * 2];

    if (self->s2_1d55 == 0 && Stage2_Scan(self, 1, 6, 0x140, -1, 0)) {
        self->s2_dur[1] = (self->s2_1db1 == 'q') ? 150 : 160;
        i = 1;
    }
    if (Stage2_Scan(self, 0, 7, 1, -8, 0)) {
        self->s2_dur[i + 1] = 0x55;
        i++;
    }

    if (self->s2_1ddc >= self->s2_1dd8)
        return self->s2_1dd8 / 10;

    a = Phone_Attr((int16_t)((int16_t)(int8_t)self->s2_1db1 | 0x180));
    cx = (int16_t)(int8_t)self->s2_c_prev1;
    if (!(a & 2) && !(Phone_Attr(cx) & 1) &&
        !(Phone_Attr((int16_t)(cx | 0x80)) & 8) &&
        !(Phone_Attr((int16_t)(cx | 0x180)) & 2)) {
        pct = (a & 1) ? 0x3c : 0x5a;
        if (self->s2_1dab == 0 && pct > 0) {
            pct -= 0x14;
            if (self->s2_1daa != 0 &&
                (Phone_Attr((int16_t)(cx | 0x100)) & 1)) {
                const uint8_t *place = TV_REF(uint8_t, g_phone_place);

                if (place[(int32_t)(int8_t)self->s2_prev1->value] ==
                    place[(int32_t)(int8_t)n->value])
                    pct = 0x28;
            }
        }
        goto have_pct;
    }

    if (self->s2_1da9 == 0 && self->s2_1da5 == 0 && self->s2_next1 != NULL) {
        bx = (int16_t)(int8_t)self->s2_c_next1;
        cl = Phone_Attr(bx);
        if (!(cl & 1) && self->s2_c_prev1 != 'S' &&
            !(Phone_Attr((int16_t)(bx | 0x180)) & 2)) {
            pct = 0x46;
            if (self->s2_1da6 != 0) {
                if ((Phone_Attr((int16_t)(bx | 0x100)) & 1) ||
                    ((cl & 0x40) &&
                     !(Phone_Attr((int16_t)((int16_t)(int8_t)self->s2_c_prev1 |
                                            0x80)) & 8)))
                    pct = 0x1e;
            }
        }
    }

have_pct:
    self->s2_dur[i + 1] = pct;
    acc = 100;
    for (j = i + 1; j >= 0; j--)
        acc = (int32_t)((uint32_t)(self->s2_dur[j] * acc) / 100u);

    if (st->rate_index == 0xd)
        acc = (int32_t)((uint32_t)(g_dur_rate[st->rate_index] * (acc / 2)) /
                        100u) * 2;

    self->s2_dur[0] = acc;
    range = self->s2_1dd8 - self->s2_1ddc;
    v = (int32_t)((uint32_t)(range * acc) / 100u) + self->s2_1ddc;

    if (self->s2_1dab != 0 && self->s2_c_prev1 == 'S' &&
        self->s2_1db1 == 'q' &&
        (self->s2_c_next1 == 'L' || self->s2_c_next1 == 'R'))
        v = (int32_t)((uint32_t)(v * 50) / 100u);

    return v / 10;
}

/* Insert the silence for a phrase or sentence break.  `count` is non-zero
 * only for the syllable-count driven pause after a clause. */
/* @0x10027c60 */
void TV_THISCALL Stage2_Break(Engine *self, int32_t count)
{
    StageCtx *st = &self->stage_ctx[2];
    uint8_t c = self->s2_1db1;
    Node *x;
    int32_t split = 0, len, d;

    if (c == ')' && st->rate_index >= 6)
        return;

    if (st->rate_index >= 0x13 || self->s2_1d55 != 0) {
        x = Engine_NodeAlloc(self, st->ctl, 0, 4, ' ');
        if (count == 0 && self->s2_1db1 == ']') {
            x->arg = 0xc;
            x->b15 = 0x32;
        } else {
            d = (self->s2_1d55 != 0) ? 2 : 3;
            x->arg = (uint32_t)d;
            x->b15 = 0x32;
            x = Engine_NodeAlloc(self, st->ctl, 0, 4, ' ');
            x->arg = (uint32_t)d;
            x->b15 = 0x32;
        }
        Stage2_Context(self);
        return;
    }

    if (count > 0) {
        split = 1;
        if (count < 0x1e)
            len = 0x1e;
        else if (count > 0x50)
            len = 0x50;
        else
            len = count;
    } else {
        /* "!", "." and "?" get the long pause, "," a short one */
        if (c == '!' || c == '.' || c == '?') {
            len = 0x27;
            split = 1;
        } else if (c == ',') {
            len = 0xa;
        } else if (c == '\\') {
            len = 4;
        } else if (c == ']') {
            len = 0xc;
        } else {
            len = 8;
        }
    }
    if (count == 0 && c == ']')
        len = 0xc;

    while (len >= 4) {
        if (split != 0 && len < 0x32) {
            split = 0;
            x = Engine_NodeAlloc(self, st->ctl, 0, 4, ' ');
            x->arg = (uint32_t)(len / 3);
            x->b15 = 0x32;
            x = Engine_NodeAlloc(self, st->ctl, 0, 4, ' ');
            x->arg = (uint32_t)((len * 2) / 3);
            x->b15 = 0x32;
        } else {
            x = Engine_NodeAlloc(self, st->ctl, 0, 4, ' ');
            x->arg = (uint32_t)(len < 0x32 ? len : 0x32);
            x->b15 = 0x32;
        }
        len -= 0x32;
        if (len < 4 && len > 0)
            len = 4;
        if (self->free_nodes < 3)
            break;
    }
    Stage2_Context(self);
}

/* Work out where this phoneme sits in the clause's pitch contour. */
/* @0x10027270 */
void TV_THISCALL Stage2_Pitch(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *n, *e;
    int16_t ax, dx;
    uint32_t f;

    ax = (int16_t)(int8_t)self->s2_1db1;
    if (Phone_Attr(ax) & 8) {
        self->s2_1d98 = 0;
        return;
    }
    if (Phone_Attr((int16_t)(ax | 0x80)) & 0x40) {
        self->s2_1d94 = 0;
        self->s2_1d9b = 0;
        self->s2_1d9a = 0;
        self->s2_1d99 = 0;
        self->s2_1d98 = 0;
        self->s2_stressed = NULL;
        return;
    }
    if (self->s2_1dac == 0)
        return;

    if (self->s2_stressed == NULL) {
        n = st->ctl;
        for (;;) {
            f = n->flags;
            if (NODE_TYPE(n) == 3) {
                dx = (int16_t)(int8_t)n->value;
                if (Phone_Attr(dx) & 0x80) {
                    if ((f & 0x18u) > 8)
                        self->s2_stressed = n;
                } else if (Phone_Attr((int16_t)(dx | 0x80)) & 0x40) {
                    break;
                }
            }
            if (st->scan == n) {
                self->s2_stressed = NULL;
                break;
            }
            n = Engine_StageNext(self, n);
        }
    }

    if (Stage2_Scan(self, 1, 9, 0x140, -1, 0)) {
        self->s2_1d99 = 1;
        self->s2_1d98 = 1;
    } else if (Stage2_Scan(self, 1, 0xa, 8, -1, 0)) {
        self->s2_1d98 = 1;
    }
    if (st->ctl == self->s2_stressed)
        self->s2_1d9a = 1;

    e = self->s2_next_word;
    self->s2_1da0 = 4;
    if (Phone_TestMask(e, 2, 0)) {
        e = Node_NextWord(self, e);
        if (Phone_TestMask(e, 2, -1) && Phone_TestMask(e, 0x80, 0))
            e = self->s2_next_word;
    }

    if (Phone_TestMask(e, 4, 0)) {
        if (Phone_TestMask(e, 0x40, 0) || Phone_TestMask(e, 0x108, 0))
            self->s2_1da0 = 3;
    } else if (Phone_TestMask(e, 0x40, 0)) {
        self->s2_1da0 = 1;
    } else if (Phone_TestMask(e, 0x20, 0)) {
        self->s2_1da0 = 2;
    }
}

/* The consonant duration rules.  Each class of consonant gets an extra
 * duration that depends on what is either side of it; the answer goes through
 * the stress scaling and the rate floor before it is used. */
/* @0x1005c020 */
int32_t TV_THISCALL Stage2_DurAdjust(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *n = st->ctl;
    Node *p;
    uint8_t nx = self->s2_c_next1;      /* bl */
    uint8_t pv2 = self->s2_c_prev2;     /* [esp+0x13] */
    uint8_t at_pct = 0;                 /* [esp+0x12] */
    uint8_t c, geminate, a, cl;
    int32_t extra, v, ch;
    int16_t dx, bx;

    extra = (int32_t)n->arg;
    if (Stage2_Merge(self)) {
        Stage2_Context(self);
        extra = (int32_t)st->ctl->arg;
    }

    p = Node_PrevBoundary(self, self->s2_prev_word);
    geminate = self->s2_1e20;
    if (geminate != 0) {
        cl = p != NULL ? p->value : at_pct;
        if (cl != '&') {
            for (;;) {
                if (cl == '%')
                    break;
                if (p == NULL)
                    break;
                p = p->prev;
                if (p == NULL)
                    break;
                cl = p->value;
                if (cl == '&')
                    break;
            }
        }
        at_pct = (uint8_t)(cl == '%');
    }

    c = self->s2_1db1;
    ch = (int32_t)(int16_t)(int8_t)c;
    if (Phone_Attr((int16_t)(ch | 0x100)) & 2) {
        extra = Stage2_DurVowel(self);
        goto scale;
    }

    a = Phone_Attr(ch);
    if ((a & 0x20) && c != 'q') {
        /* a stop */
        extra = 0;
        if (geminate != 0 && st->ctl->prev->value != '%' && at_pct == 0) {
            extra = 6;
            if (c == 'P' || c == 'T' || c == 'K') {
                dx = (int16_t)(int8_t)pv2;
                if (Phone_Attr((int16_t)(dx | 0x100)) & 2) {
                    if (nx == 'R' || nx == 'L' || nx == 'W' || nx == 'Y') {
                        if (c == 'P' || c == 'T')
                            extra = 6;
                        else if (c == 'K')
                            extra = 9;
                    } else {
                        bx = (int16_t)(int8_t)nx;
                        if (Phone_Attr((int16_t)(bx | 0x100)) & 2) {
                            if (c == 'T') {
                                extra = 6;
                            } else if (Phone_Attr((int16_t)(bx | 0x200)) & 1) {
                                extra = 6 + (c == 'P' ? 2 : 0);
                            } else {
                                extra = 7 + (c == 'P' ? 3 : 0);
                            }
                        } else if (Phone_Attr(dx) & 0x10) {
                            if (c == 'P')
                                extra = 8;
                            else
                                extra = 9 + (c == 'T' ? -4 : 0);
                        } else if (Phone_Attr((int16_t)(dx | 0x80)) & 0x20) {
                            extra = 9;
                        }
                    }
                }
            } else if (c == 'D' || c == 'G') {
                dx = (int16_t)(int8_t)pv2;
                if (Phone_Attr((int16_t)(dx | 0x100)) & 2) {
                    bx = (int16_t)(int8_t)nx;
                    if (Phone_Attr((int16_t)(bx | 0x100)) & 2) {
                        extra = (c == 'D') ? 7 : 6;
                    } else if (nx == 'R' || nx == 'L' || nx == 'W' ||
                               nx == 'Y') {
                        extra = 6;
                    }
                    /* the rest of the original's chain needs the bit that
                     * has just been tested clear, so it cannot be reached */
                } else if (Phone_Attr(dx) & 0x10) {
                    extra = 6;
                }
            } else if (c == 'B') {
                int32_t strong;

                dx = (int16_t)(int8_t)pv2;
                a = (uint8_t)(Phone_Attr((int16_t)(dx | 0x100)) & 2);
                strong = (a != 0) || pv2 == 'R' || pv2 == 'L' || pv2 == 'W' ||
                         pv2 == 'Y';
                if (strong) {
                    dx = (int16_t)(int8_t)nx;
                    if ((Phone_Attr((int16_t)(dx | 0x100)) & 2) &&
                        (Phone_Attr((int16_t)(dx | 0x200)) & 1)) {
                        extra = 8;
                        goto stop_done;
                    }
                }
                if (a != 0) {
                    bx = (int16_t)(int8_t)nx;
                    if ((Phone_Attr((int16_t)(bx | 0x100)) & 2) &&
                        !(Phone_Attr((int16_t)(bx | 0x200)) & 1))
                        extra = 8;
                    else if (nx == 'R' || nx == 'L' || nx == 'W' || nx == 'Y')
                        extra = 5;
                }
            }
        }
stop_done:
        extra += Stage2_DurStop(self);
        goto scale;
    }

    if (!(a & 0x40) && c != 'H' && c != 'd') {
        if ((a & 2) && c != 'h') {
            extra = Stage2_DurNasal(self);
        } else if (c == ' ') {
            extra = Stage2_Pause(self);
        } else if (c == 'q') {
            if (Phone_Attr((int8_t)self->s2_c_next1) & 0x10)
                extra = 7;
        } else if (c == 't') {
            extra = 3;
        }
        goto scale;
    }

    /* a fricative */
    extra = 0;
    if (geminate != 0 && st->ctl->prev->value != '%') {
        extra = 4;
        if (c == 'F') {
            extra = 8;
        } else if (c == 'x') {
            extra = 6;
        } else if (c == 'S') {
            int32_t both = 0;

            if (Phone_Attr((int16_t)((int16_t)(int8_t)self->s2_c_next1 | 0x100)) & 2) {
                if (Phone_Attr((int16_t)((int16_t)(int8_t)self->s2_c_prev1 | 0x100)) & 2)
                    both = 1;
            }
            extra = both ? 6 : 5;
        }
    }
    extra += Stage2_DurFric(self);

scale:
    cl = self->s2_c_prev1;
    if (cl == 'P' || cl == 'T' || cl == 'K')
        extra += (int32_t)(self->s2_prev1->d10 & 0xffu);

    v = Stage2_DurStress(self, extra);
    if (self->s2_1d74 == 0x13) {
        if ((int32_t)(intptr_t)self->s2_marks[0] == 1)
            v = ((v * 9) << 4) / 100;
        else if ((int32_t)(intptr_t)self->s2_marks[0] == 2)
            v = (v * 123) / 100;
    }
    if (st->rate_index != 0xd)
        v = rate_apply(self, st->rate_index, Stage2_MinDur(self, v));
    if (v > 0x37)
        return 0x37;
    if (v < 2)
        return 2;
    return v;
}

/* The vowel duration rule tables, one set per word class (Node.b19). */
/* @0x100ee5b0 */ extern const tv_ref g_dur_rulesets[16];
/* @0x100ee5f0 */ extern const int32_t g_dur_rulecount[16];
/* Which condition each bit of DurRule.cond selects. */
/* @0x1005c960 */ extern const uint8_t g_dur_cond_idx[0x40];

/* Try the rules for this word class against the phonemes around the cursor.
 * Returns the matching rule's result, or 0. */
/* @0x1005c560 */
int32_t TV_THISCALL Stage2_DurTable(Engine *self, int32_t c0, int32_t a1,
                                    int32_t a2)
{
    StageCtx *st = &self->stage_ctx[2];
    int16_t ch16 = (int16_t)(int8_t)c0;
    const DurRule *r;
    const DurTest *t;
    Node *n, *e;
    int32_t flag_p = 0;
    uint8_t ch_next = 0;
    uint8_t ok, b19, c, f_stress, f_match, bit;
    int32_t i, j, k, set, mask, v;
    const char *s;

    if (Phone_Attr((int16_t)(ch16 | 0x180)) & 1) {
        n = st->ctl;
        b19 = n->b19;
        if ((b19 == 0xd && n->value == 'k') ||
            (b19 == 0xc && n->value == '3') ||
            (b19 == 7 && n->value == 'b'))
            return 0;
        e = n->prev;
        if (Phone_Attr((int8_t)e->value) & 8) {
            e = e->prev;
            if (Phone_Attr((int16_t)((int16_t)(int8_t)e->value | 0x180)) & 1) {
                if ((b19 == 0xc && n->value == 'r') ||
                    (b19 == 0xe && n->value == 'g') ||
                    (b19 == 7 && n->next->value == 'j'))
                    return 0;
            }
        }
    }

    e = Stage2_Find(self, 0, 0x10, 8);
    if (e != NULL) {
        e = e->prev;
        c = e->value;
        if ((c == 'P' && NODE_TYPE(e) == 0) || c == ',')
            flag_p = 1;
        else
            flag_p = 0;
    }
    e = Stage2_Find(self, 1, 0x10, 0x140);
    if (e != NULL)
        ch_next = e->value;

    set = 0;
    b19 = st->ctl->b19;
    r = TV_REF(DurRule, g_dur_rulesets[b19]);
    if (g_dur_rulecount[b19] <= 0)
        return 0;

    for (;;) {
        ok = 1;

        /* step back to where the rule's text starts */
        n = st->ctl;
        j = 0;
        while (n != NULL && (int32_t)r->back > j) {
            n = n->prev;
            if (n == NULL)
                ok = 0;
            j++;
            if (!ok)
                break;
        }

        /* match the text forwards */
        s = TV_REF(char, r->text);
        j = 0;
        if (ok) {
            while (n != NULL && (int32_t)r->len > j) {
                if (n->value != (uint8_t)*s)
                    ok = 0;
                s++;
                n = n->next;
                if (n == NULL)
                    ok = 0;
                j++;
                if (!ok)
                    break;
            }
        }

        /* the flag conditions */
        if (ok && r->cond != 0) {
            bit = 1;
            for (j = 0; j < 7; j++) {
                k = (int32_t)(uint8_t)(r->cond & bit) - 1;
                if ((uint32_t)k <= 0x3f) {
                    switch (g_dur_cond_idx[k]) {
                    case 0: if (flag_p == 0) ok = 0; break;
                    case 1: if (a1 == 0) ok = 0; break;
                    case 2: if (flag_p != 0) ok = 0; break;
                    case 3: if (a1 != 0) ok = 0; break;
                    case 4: if (ch_next == '?') ok = 0; break;
                    case 5: if ((uint8_t)a2 != 0) ok = 0; break;
                    case 6: if ((uint8_t)a2 == 0) ok = 0; break;
                    default: break;
                    }
                }
                bit = (uint8_t)(bit + bit);
                if (!ok)
                    break;
            }
        }

        /* the per-node tests */
        f_stress = 0;
        f_match = 0;
        if (r->ntests != 0 && ok) {
            t = TV_REF(DurTest, r->tests);
            for (i = 0; (int32_t)r->ntests > i; i++, t++) {
                if (t->dir != 0)
                    n = st->ctl;
                j = 0;
                while (n != NULL && (int32_t)t->count > j) {
                    if (t->dir == 1) {
                        n = Node_NextWord(self, n);
                        if (n == NULL)
                            ok = 0;
                    }
                    j++;
                    if (n == NULL)
                        break;
                }
                if (!ok)
                    break;
                if (t->kind > 5)
                    break;
                mask = t->arg;
                switch (t->kind) {
                case 0:
                    v = (int32_t)(int8_t)n->value | ((mask & 0xff00) >> 1);
                    if (!((uint8_t)(Phone_Attr(v) & mask)))
                        ok = 0;
                    break;
                case 1:
                    f_stress = 1;
                    if ((int32_t)n->b15 == mask)
                        f_match = 1;
                    break;
                case 2:
                    if ((int32_t)n->b15 == mask)
                        ok = 0;
                    break;
                case 3:
                    v = (int32_t)ch16 | ((mask & 0xff00) >> 1);
                    if (!((uint8_t)(Phone_Attr(v) & mask)))
                        ok = 0;
                    break;
                case 4:
                    v = (int32_t)ch16 | ((mask & 0xff00) >> 1);
                    if ((uint8_t)(Phone_Attr(v) & mask))
                        ok = 0;
                    break;
                default:
                    if ((uint8_t)mask == (uint8_t)c0)
                        ok = 0;
                    break;
                }
                if (!ok)
                    break;
            }
        }

        if (f_stress != 0 && f_match == 0)
            ok = 0;
        if (ok == 1)
            return r->result;

        r++;   /* 0x14 in the original, which is sizeof(DurRule) */
        set++;
        if (g_dur_rulecount[st->ctl->b19] <= set)
            return 0;
    }
}


/* Flag bits per index (shared with stage 0). */
/* @0x100f83a0 */ extern const uint32_t g_s0_flag_lo[16];

/* A counter the breath-noise rule uses to spread its insertions out. */
/* @0x101489c8 */ extern uint32_t g_s2_rnd;
/* Which of the 16 counter values allow a breath at each rate. */
/* @0x100ee700 */ extern const uint32_t g_breath_mask[];

/* Walk the marks the scan left behind and decide where the clause breaks
 * go, then put the pause command in and sprinkle in the breath noises. */
/* @0x10026c30 */
void TV_THISCALL Stage2_Phrase(Engine *self, int32_t punct)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *a, *m, *m1, *n, *p;
    int32_t i, last = 0, v;
    uint8_t bl, pc, al, dl, cl, f13, prev_c;

    if (self->s2_nmarks <= 0)
        return;

    i = 2;
    if (self->s2_nmarks - 2 > i) {
        do {
            int32_t split = 0;

            m = self->s2_marks[i];
            bl = m->b15;
            if (bl == 0) {
                m1 = self->s2_marks[i + 1];
                al = m1->b15;
                if ((al == 6 && self->s2_marks[i + 2]->prev->value != '~') ||
                    al == 9) {
                    if (!(m1->flags & 0x80u))
                        split = 1;
                }
                if (!split && (al == 8 || al == 0xb || al == 0xc))
                    split = 1;
            }

            m1 = self->s2_marks[i + 1];
            pc = m1->value;
            if (!split && pc == '%' && !(m1->flags & 0x80u) &&
                m->value == '&' && bl != 6) {
                cl = m1->b15;
                if (cl == 5)
                    split = 1;
                else if (cl == 0xe && self->s2_marks[i + 2]->b15 == 5)
                    split = 1;
            }

            /* The original tests the next mark's class against '%' and then
             * against the small class numbers, so this branch never fires. */

            dl = m1->b15;
            if (dl == 0xd) {
                f13 = 1;
            } else if (self->s2_marks[i - 1]->b15 == 0xa || bl == 6 ||
                       bl == 0xf) {
                f13 = 0;
            } else {
                f13 = 0;
                if (dl == 0xa) {
                    cl = m1->next->value;
                    if (m1->b18 != '-' && cl != 'T' && cl != 't' && cl != 'a')
                        f13 = 1;
                }
                if (f13 == 0 && dl == 2) {
                    cl = m1->next->value;
                    if (cl != 'x' && cl != 'B')
                        f13 = 1;
                }
                if (f13 == 0 && dl == 3) {
                    cl = m1->next->value;
                    if (cl == 'F' || cl == 's')
                        f13 = 1;
                }
            }

            if (!split) {
                if (pc == '%' && !(m1->flags & 0x80u) && m->value == '&' &&
                    f13 != 0) {
                    int32_t comma = 0;

                    if ((dl == 0xa || dl == 3) &&
                        self->s2_marks[i + 2]->b15 == 0xe)
                        comma = 1;
                    if (!comma && dl == 0xa &&
                        self->s2_marks[i + 2]->b15 == 0xa)
                        comma = 1;
                    if (comma) {
                        Stage2_Split(self, last, ',', i + 1);
                        last = i + 1;
                        i += 3;
                        goto next;
                    }
                    split = 1;
                } else if (bl == 0xf && !(m1->flags & 0x80u) &&
                           (dl == 0xa || dl == 5 || dl == 0xe)) {
                    split = 1;
                }
            }
            if (split) {
                Stage2_Split(self, last, ',', i);
                last = i;
                i += 2;
            }
next:
            i++;
        } while (self->s2_nmarks - 2 > i);
    }

    if (self->free_nodes < 3)
        return;

    a = Engine_NodeAlloc(self, self->s2_marks[last + 1], 0, 0, 'P');
    if (a->next == st->cur)
        st->cur = a;

    cl = (uint8_t)punct;
    if (cl == '.' || cl == '!') {
        a->arg = 1;
    } else if (cl == ',') {
        a->arg = 3;
    } else if (cl != '?') {
        a->arg = 2;
    } else if (self->s2_marks[1]->b15 == 0xd) {
        a->arg = 1;
        n = self->s2_marks[1]->next->next;
        if (NODE_TYPE(n) == 3 && (Phone_Attr((int8_t)n->value) & 0x80)) {
            n->flags |= 0x20u;
            n = self->s2_marks[1]->next->next;
            n->flags = (n->flags & ~0x10u) | 8u;
        }
    } else if (self->s2_marks[last + 1]->b15 != 0xd) {
        a->arg = 4;
    } else {
        a->arg = 1;
        n = self->s2_marks[last + 1]->next->next;
        if (NODE_TYPE(n) == 3 && (Phone_Attr((int8_t)n->value) & 0x80)) {
            n->flags |= 0x20u;
            n = self->s2_marks[last + 1]->next->next;
            n->flags = (n->flags & ~0x10u) | 8u;
        }
    }

    if (self->s2_1d45 != 0) {
        a->b15 = 0;
        self->s2_1d45 = 0;
    } else {
        a->b15 = 1;
    }

    if (self->s2_1d44 == 0)
        return;

    n = self->s2_marks[1];
    v = st->rate_index;
    prev_c = 0;
    if (self->s2_marks[self->s2_nmarks] == n)
        return;
    do {
        uint32_t t;

        if (self->free_nodes <= 3)
            return;
        t = n->flags & 7u;
        if (t == 0 && n->value == 'r') {
            v = (int32_t)(((n->arg & 0xfeu) - 0x2eu)) >> 3;
        } else if (t == 0 && n->value == 'v') {
            v = (int32_t)(n->arg & 0xffu);
        } else {
            al = n->value;
            if ((al == '%' || al == '&') && t == 3) {
                if (al == '%' && prev_c == '&' && v < 9) {
                    p = Engine_StagePrev(self, n);
                    if (p->value != ',') {
                        uint32_t k = g_s2_rnd & 0xf;

                        g_s2_rnd++;
                        if (g_breath_mask[v] & g_s0_flag_lo[k])
                            Engine_NodeAlloc(self, n, 0, 3, ')');
                    }
                }
                prev_c = n->value;
            }
        }
        n = Engine_StageNext(self, n);
    } while (self->s2_marks[self->s2_nmarks] != n);
}

/* The scan counters the stage entry keeps between steps.  g_s2_run counts the
 * voiced phonemes since the last break, g_s2_total the ones since the step
 * started, and g_s2_count every node the scan has touched. */
/* @0x101489cc */ extern int32_t g_s2_count;
/* @0x101489d0 */ extern uint8_t g_s2_voiced;
Node   *g_s2_last;   /* ClassicVoices patch: was the image's 4-byte slot at 0x101489d4, see g_s2_node */
/* @0x101489dc */ extern int32_t g_s2_run;
/* @0x101489e4 */ extern uint8_t g_s2_hold;
/* @0x101489f4 */ extern int32_t g_s2_total;

/* Appends one byte to the COM-allocated list the phoneme trace goes to.  The
 * engine only builds that list when w_212c is set, which the harness never
 * does, so this stays a call into the original for now. */
/* @0x100311d0 */
void TV_THISCALL ByteList_Append(void *self, int32_t b);

/*
 * The first thing Stage2_Run does on every step.  It walks forward from the
 * scan cursor until it has enough material to work on -- a sentence-final
 * punctuation mark, a clause break, 240 nodes or 180 voiced phonemes -- and
 * remembers every "%"/"&" boundary it passed in s2_marks.  Along the way it
 * notes where the three kinds of boundary sat relative to the phrase, so the
 * tail can pick the intonation contour (s2_1d74).
 *
 * The return value is the reason it stopped, which is the code Stage2_Run
 * switches on: 1 clause command, 3 sentence end, 4 buffer full, 5 comma.
 */
/* @0x1002ab70 */
int32_t TV_THISCALL Stage2_Begin(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *n, *p;
    int32_t v, slot;
    int16_t ch;
    /* The original reads this before it is written when the scan starts out
     * empty; it only matters for the "!" test at the end. */
    uint8_t c = 0;

    if (self->s2_1d6c == 2)
        return 2;

    if (self->s2_1d6c != 0) {
        self->s2_1d60 -= self->s2_1d68;
        self->s2_1d68 = 0;
        g_s2_run = 0;
        g_s2_total = 0;
        g_s2_count = 0;
        g_s2_voiced = 0;
        self->s2_nmarks = 0;
        self->s2_1d84 = 0;
        self->s2_1d80 = -1;
        self->s2_1d7c = -1;
        self->s2_1d78 = -1;
        if (self->s2_1d6c == 3 || self->s2_1d6c == 5) {
            Stage2_ResetRun(self);
            self->s2_1d60 = 0;
            self->s2_marks[0] = NULL;
            g_s2_hold = 0;
            self->s2_1d45 = 1;
        }
    }

    /* Run off any control nodes sitting on the cursor first. */
    if (st->cur != NULL) {
        while (st->cur == st->scan && NODE_TYPE(st->cur) != 3) {
            Engine_RunControl(self);
            n = st->cur;
            self->s2_1d55 = 0;
            self->s2_1d5c = 0xb4;
            n = Engine_StageNext(self, n);
            st->cur = n;
            st->ctl = n;
            st->scan = n;
            if (n == NULL)
                break;
        }
    }

    g_s2_last = st->scan;
    self->s2_1d6c = 0;
    n = st->scan;
    while (n != NULL && self->s2_1d6c == 0) {
        c = n->value;
        ch = (int16_t)(int8_t)c;
        g_s2_count++;

        if (NODE_TYPE(n) == 3 && (Phone_Attr(ch) & 0x80)) {
            /* a phoneme: just count it */
            g_s2_voiced = 1;
            g_s2_run++;
            if (self->w_212c != 0) {
                if (c != ' ' || self->w_212e != 1)
                    ByteList_Append(self->s2_bytes, c);
                if (Phone_Attr((int16_t)(ch | 0x100)) & 2) {
                    v = (int32_t)(n->flags & 0x18u);
                    if (v > 8)
                        ByteList_Append(self->s2_bytes, '1');
                    else if (v == 8)
                        ByteList_Append(self->s2_bytes, '2');
                }
            }
            goto advance;
        }

        g_s2_total += g_s2_run;
        self->s2_1d60 += g_s2_run;
        g_s2_run = 0;
        g_s2_last = n;

        if (NODE_TYPE(n) == 3) {
            if (c == '%' || c == '&') {
                slot = self->s2_nmarks;
                if (slot < 0x32) {
                    /* Note where this boundary sits, so the tail can tell a
                     * question apart from a list or a tag. */
                    switch (n->arg) {
                    case 6:    self->s2_1d78 = self->s2_1d60; break;
                    case 7:    self->s2_1d7c = self->s2_1d60; break;
                    case 8:    self->s2_1d80 = self->s2_1d60; break;
                    case 0x11: self->s2_1d84 = self->s2_1d60; break;
                    default:   break;
                    }
                    self->s2_marks[slot + 1] = n;
                    self->s2_nmarks++;
                    if (self->w_212c != 0)
                        ByteList_Append(self->s2_bytes, c);
                    goto advance;
                }
            }

            if (!(Phone_Attr((int16_t)(ch | 0x80)) & 0x40))
                goto advance;

            if (c == '.' || c == '?' || c == '!') {
                self->s2_1d64 = self->s2_1d60;
                self->s2_1d6c = 3;
                p = Engine_StagePrev(self, n);
                if (self->w_212c != 0)
                    ByteList_Append(self->s2_bytes, c);
                /* The punctuation may sit inside the last word; back the
                 * phoneme count up over anything past the break. */
                while (Phone_TestMask(p, 1, -1)) {
                    if (NODE_TYPE(p) == 3 &&
                        (Phone_Attr((int8_t)p->value) & 0x80))
                        self->s2_1d60--;
                    p = Engine_StagePrev(self, p);
                }
            } else if (c == ',' && self->s2_1d70 != 0) {
                self->s2_1d6c = 5;
            }

            if (g_s2_hold == 0 && self->s2_1d70 == 0 &&
                st->rate_index < 0x13 && c != '!')
                Stage2_Phrase(self, c);

            g_s2_voiced = 0;
            g_s2_hold = 0;
            v = self->s2_nmarks;
            self->s2_nmarks = 0;
            self->s2_marks[0] =
                (Node *)(intptr_t)((int32_t)(intptr_t)self->s2_marks[0] + v);
        } else if (NODE_TYPE(n) == 0) {
            switch (c) {
            case 'C':
                if (g_s2_hold == 0 && self->s2_1d70 == 0)
                    Stage2_Phrase(self, 0);
                self->s2_1d6c = 1;
                break;
            case 'N': {
                uint8_t t = (uint8_t)(n->b15 & 0x20u);

                self->s2_1d55 = t;
                self->s2_1d5c = (t == 0) ? 0xb4 : 0xf;
                break;
            }
            case 'P':
                self->s2_1d70 = (uint8_t)(n->arg == 0);
                if (g_s2_voiced == 0) {
                    g_s2_hold = 1;
                } else if (self->free_nodes > 3) {
                    /* a phrase break in the middle of speech: put the
                     * lengthening mark in and rescan from it */
                    p = Engine_NodeAlloc(self, n, 0, 3, '\\');
                    g_s2_count++;
                    st->scan = Engine_StagePrev(self, p);
                }
                break;
            case 'r':
            case 'v':
                self->s2_1d44 = (uint8_t)((uint32_t)n->arg < 9u);
                break;
            default:
                break;
            }
        }

advance:
        if (self->s2_1d6c == 0) {
            if (g_s2_count < 0xf0 && self->s2_1d5c > g_s2_total) {
                st->scan = Engine_StageNext(self, st->scan);
            } else {
                /* out of room: stop at the last boundary instead */
                st->scan = Engine_StagePrev(self, g_s2_last);
                c = g_s2_last->value;
                if (c == '%' || c == '&')
                    self->s2_nmarks--;
                if (g_s2_hold == 0 && self->s2_1d70 == 0)
                    Stage2_Phrase(self, 0);
                self->s2_1d6c = 4;
            }
        }
        n = st->scan;
    }

    /* Pick the contour from where the boundaries fell. */
    if (self->s2_1d78 < 0 && self->s2_1d7c < 0) {
        self->s2_1d74 = (self->s2_1d80 >= 0) ? 8 : 0;
    } else if (self->s2_1d7c < 0) {
        self->s2_1d74 = (self->s2_1d78 != 0) ? 6 : 0;
    } else {
        v = self->s2_1d7c;
        if (self->s2_1d78 < 0)
            self->s2_1d74 = (self->s2_1d60 == v) ? 0 : 7;
        else if (self->s2_1d78 < v)
            self->s2_1d74 = (self->s2_1d78 != 0) ? 0x10
                            : ((self->s2_1d60 == v) ? 0 : 7);
        else if (v != 0)
            self->s2_1d74 = 0xf;
        else
            self->s2_1d74 = (self->s2_1d78 != 0) ? 6 : 0;
    }

    if (c == '!') {
        /* An exclamation gets its own contour, anchored halfway back through
         * the vowels of the last stressed word. */
        self->s2_1d74 = 0x13;
        self->s2_1d78 = self->s2_1d64;
        n = st->scan;
        while (n != NULL && st->cur != n) {
            ch = (int16_t)(int8_t)n->value;
            if ((Phone_Attr((int16_t)(ch | 0x100)) & 2) && (n->flags & 0x20u))
                break;
            if (Phone_Attr(ch) & 0x80)
                self->s2_1d78--;
            n = Engine_StagePrev(self, n);
            if (self->s2_1d78 == 0)
                break;
        }
        v = self->s2_1d78 - 1;
        self->s2_1d78 = v;
        self->s2_1d7c = (v == 1) ? 0 : (v + 1) / 2;
    }

    return self->s2_1d6c;
}

/* The pitch range each voice is allowed, all ten the same in this build. */
/* @0x100b5440 */ extern const int32_t g_voice_pitch_scale[10];

/*
 * Give the node stage 2 just finished its pitch.
 *
 * The phrase's intonation contour (s2_1d74, picked by Stage2_Begin) says how
 * the pitch moves across the phrase: s2_1d8c counts the phonemes so far and
 * s2_1d60 holds the total, so the contour is a piecewise-linear ramp through
 * the turning points s2_1d78, s2_1d7c and s2_1d80.  On top of that come the
 * stress accents, a handful of segmental corrections, and the clause-final
 * fall the "P" command asks for.  The answer is halved into Node.b15.
 */
/* @0x100274d0 */
void TV_THISCALL Stage2_Contour(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *n = st->ctl;
    Node *nx, *pv, *stressed, *w;
    int32_t follow = 0;         /* the phoneme that opens the next word */
    int32_t contour, pos, total, a, b;
    int32_t pitch, v, step, span, quarter, lift;
    int16_t ch, c2;
    uint8_t attr, c;

    w = Node_NextWord(self, self->s2_next_word);
    if (w != NULL)
        follow = (int32_t)(int8_t)w->value;

    if (self->s2_1da8 == 0) {
        /* not a phoneme: only the pause commands mean anything here */
        n = st->ctl;
        if (!(Phone_Attr((int8_t)n->value) & 8))
            return;
        if (n->arg == 0xc)
            self->s2_1da4 = 1;
        else if (n->arg == 0x11)
            self->s2_1dd4 = 1;
        else if (self->s2_1dd4 != 0 && self->s2_1d9b != 0 &&
                 self->s2_1d99 == 0)
            self->s2_1dd4 = 0;
        return;
    }

    self->s2_1d8c++;
    n->b1a = (uint8_t)self->cur_bac;

    if (self->cur_bac == 1) {
        pitch = 0;
        goto store;
    }
    if (self->cur_bac == 2) {
        pitch = st->pitch;
        goto store;
    }
    if (self->s2_1d94 != 0) {
        /* running out the fall the last clause ended on */
        v = self->s2_1d94 - 8;
        self->s2_1d94 = v;
        if (v < 0x2e)
            self->s2_1d94 = 0x2e;
        pitch = self->s2_1d94;
        goto store;
    }

    pos = self->s2_1d8c;
    v = Synth_MulQ15(st->pitch / 3, g_voice_pitch_scale[st->voice]);
    /* Narrowing the range has to keep the voice where it was.  The contour
     * runs from st->pitch + v down towards st->pitch, so a smaller v on its
     * own drops the whole thing by half the difference; giving that half
     * back as a lift keeps the centre and takes only the excursion. */
    lift = 0;
    if (rate_pitch_pct(st->rate_index) != 100) {
        int32_t full = v;

        v = v * rate_pitch_pct(st->rate_index) / 100;
        lift = (full - v) / 2;
    }
    contour = self->s2_1d74;
    if (contour == 0x13)
        v = (self->s2_1d7c < pos) ? (v << 2) : (v * 3) * 2;
    if (self->s2_1d60 == 0)
        self->s2_1d60 = 1;
    total = self->s2_1d60;
    pitch = st->pitch + v + lift;

    switch (contour) {
    case 0:
        /* a plain statement: fall steadily to the end */
        pitch -= (pos * v) / total;
        break;
    case 6:
        /* rise to the mark, then fall away from it */
        if (self->s2_1d78 == 0)
            self->s2_1d78 = total;
        a = self->s2_1d78;
        if (a >= pos)
            pitch += ((pos - a) * v) / a;
        else
            pitch += ((a - pos) * v) / (total - a);
        break;
    case 7:
        b = self->s2_1d7c;
        if (pos <= b)
            pitch -= (v * pos) / b;
        else
            pitch += ((pos - total) * v) / (total - b);
        break;
    case 8:
        st->p_20 = 6;
        if (self->s2_1d80 == 0)
            self->s2_1d80 = total;
        a = self->s2_1d80;
        if (pos <= a)
            pitch += ((pos - a) * v) / a;
        else
            pitch += ((a - pos) * v) / (total - a);
        break;
    case 0x10:
        st->p_20 = 3;
        a = self->s2_1d78;
        if (a >= pos) {
            pitch += ((pos - a) * v) / a;
        } else {
            b = self->s2_1d7c;
            if (pos <= b)
                pitch += ((a - pos) * v) / (b - a);
            else
                pitch += ((pos - total) * v) / (total - b);
        }
        break;
    case 0xf:
    case 0x13:
        /* fall to the low mark, then track the high one */
        if (contour == 0xf)
            st->p_20 = 6;
        b = self->s2_1d7c;
        if (pos <= b) {
            pitch -= (v * pos) / b;
        } else {
            a = self->s2_1d78;
            if (a >= pos)
                pitch += ((pos - a) * v) / (a - b);
            else
                pitch += ((a - pos) * v) / (total - a);
        }
        break;
    default:
        break;
    }

    if (self->s2_1da4 != 0)
        pitch = (pitch * 108) / 100;

    /* The accent step, a quarter of the voice's range, trimmed by how much
     * of the phrase is left. */
    span = Synth_MulQ15(pitch, g_voice_pitch_scale[st->voice]) >> 2;
    quarter = span >> 2;
    step = span - quarter;
    if (self->s2_1d9b == 0) {
        if ((st->p_20 & 0xffffff00) == 0x100 || self->s2_1dd4 != 0)
            step -= step >> 2;
        else
            step += step >> 4;
    }

    stressed = self->s2_stressed;
    if (stressed != NULL && (stressed->flags & 0x18u) == 0x18)
        step <<= 3;

    nx = self->s2_next1;
    if (nx != NULL && (uint8_t)(nx->flags & 0x18u) > 8 && self->s2_1d9a == 0)
        self->s2_1d90 = 1;
    else if (pos == 1 && self->s2_1dad != 0 && self->s2_1dab != 0)
        self->s2_1d90 = 1;
    else if (self->s2_1d84 != 0 && pos >= self->s2_1d84 &&
             self->s2_1d99 != 0)
        self->s2_1d90 = 1;

    if (nx == stressed && nx != NULL && (uint8_t)st->p_20 != 6) {
        pitch += step;
        if ((nx->flags & 0x18u) == 0x18)
            pitch += step * 2;
    }

    switch ((n->flags & 0x18u) >> 3) {
    case 1:
        step >>= 1;
        pitch += step;
        break;
    case 2:
    case 3:
        self->s2_1d9b = 1;
        if (n == stressed && (uint8_t)st->p_20 != 6) {
            n->flags |= 0x80u;
            self->s2_1d90 = 0;
        } else {
            pitch += step;
            if ((n->flags & 0x18u) == 0x18)
                pitch += step * 8;
        }
        break;
    default:
        break;
    }

    if (self->s2_1d90 != 0)
        pitch += span;

    ch = (int16_t)(int8_t)self->s2_1db1;
    attr = Phone_Attr(ch);
    if (attr & 4) {
        /* a voiced consonant drops the pitch a little unless the phoneme
         * after it carries the accent */
        int32_t lower = 0;

        nx = self->s2_next1;
        if (self->s2_1dab == 0 && nx != NULL &&
            !(Phone_Attr((int16_t)((int16_t)(int8_t)nx->value | 0x100)) & 2) &&
            (uint8_t)(nx->flags & 0x18u) < 8 &&
            (!(Phone_Attr((int16_t)(ch | 0x80)) & 0x20) ||
             !(Phone_Attr((int16_t)(ch | 0x180)) & 1) ||
             !(attr & 0x10)))
            lower = 1;
        if (lower || self->s2_stressed == n) {
            pitch -= 3;
            if (self->s2_1da9 == 0)
                pitch -= 5;
        }
    } else {
        /* an aspirated stop at the start of a word lifts the vowel after it */
        nx = self->s2_next1;
        if (nx != NULL &&
            (Phone_Attr((int16_t)((int16_t)(int8_t)nx->value | 0x100)) & 2)) {
            pv = self->s2_prev1;
            if (pv == NULL || pv->value == ' ') {
                c = self->s2_1db1;
                if (c == 'T' || c == 'P' || c == 'K')
                    pitch += 0x14;
            }
        }
    }

    /* "N" closing a word before another vowel is a syllabic nasal. */
    if (self->s2_1db1 == 'N' &&
        (Phone_Attr((int16_t)((int16_t)(int8_t)self->s2_c_prev1 | 0x100)) & 2) &&
        !(self->s2_prev1->flags & 0x20u) &&
        (self->s2_c_next1 == 'T' || self->s2_c_next1 == 'q') &&
        (follow == '%' || follow == '&')) {
        c2 = (int16_t)(int8_t)self->s2_c_next2;
        if ((Phone_Attr(c2) & 2) ||
            ((Phone_Attr((int16_t)(c2 | 0x100)) & 2) &&
             (self->s2_next2->flags & 0x20u)))
            pitch -= 0x37;
    }

    if (self->s2_1d99 == 0) {
        if (Phone_Attr((int16_t)(ch | 0x200)) & 1)
            pitch += 3;
    } else if (self->s2_1dd4 == 0) {
        /* the last phoneme of the clause: the "P" command says how it ends */
        switch ((uint8_t)st->p_20) {
        case 1:
            pitch -= quarter;
            if (self->s2_1d9a != 0 &&
                (Phone_Attr((int16_t)(ch | 0x100)) & 2) &&
                (Phone_Attr((int16_t)((int16_t)(int8_t)self->s2_c_next_word |
                                      0x80)) & 0x40))
                pitch -= 0x16;
            break;
        case 3:
            pitch += span >> 3;
            break;
        case 4:
        case 5:
            pitch = st->pitch * 2 - (st->pitch >> 3);
            break;
        default:
            break;
        }
        if (pitch > 0x1f4)
            pitch = 0x1f4;
        else if (pitch < 0x32)
            pitch = 0x32;
        self->s2_1d94 = pitch;
        goto store;
    }

    if (pitch > 0x1f4)
        pitch = 0x1f4;
    else if (pitch < 0x32)
        pitch = 0x32;

store:
    n->b15 = (uint8_t)(pitch >> 1);
}

/* One duration table per sonorant, each a 20-wide grid of context by class. */
/* @0x100ed988 */ extern const tv_ref g_dur_tables[];
/* The floor for each phoneme, stressed then unstressed. */
/* @0x100ee710 */ extern const uint8_t g_dur_floor[];
/* Which table each phoneme, left letter and right letter selects. */
/* @0x100588ac */ extern const uint8_t g_nasal_base_idx[51];
/* @0x1005894c */ extern const uint8_t g_nasal_prev_idx[61];
/* @0x100589f4 */ extern const uint8_t g_nasal_next_idx[61];

/*
 * The duration of a sonorant -- the nasals, the liquids and the glides.
 *
 * The table for the phoneme is picked first, then a row in it from where the
 * phoneme sits in the word (ctx), whether a stressed vowel follows (scan) and
 * what the neighbouring phonemes are (klass).  What comes out is corrected
 * for a handful of clusters the table cannot express, and floored at 3.
 */
/* @0x10058150 */
int32_t TV_THISCALL Stage2_DurNasal(Engine *self)
{
    static const int32_t base_of[12] = {
        0x22, 0x1d, 0x1e, 0x20, 0x23, 0x21, 0x25, 0x24, 0x1d, 0x1e, 0x1f, 0x1d
    };
    static const int32_t prev_class[27] = {
        0xa, 8, 0xa, 9, 0xa, 9, 0xa, 8, 0xc, 8, 0xa, 9, 8, 0xb, 9,
        0xb, 0xb, 6, 7, 0xc, 0xa, 9, 0xa, 0xb, 0xb, 0xc, -1
    };
    static const int32_t next_class[26] = {
        0x11, 0xf, 0x11, 0x10, 0x11, 0x10, 0x11, 0xf, 0x13, 0xf, 0x11, 0xd,
        0x10, 0xf, 0x12, 0x10, 0x12, 0xe, 0x13, 0x11, 0x10, 0x11, 0x12, 0x12,
        0x13, -1
    };
    StageCtx *st = &self->stage_ctx[2];
    uint8_t c   = self->s2_1db1;
    uint8_t nx1 = self->s2_c_next1;
    uint8_t nx2 = self->s2_c_next2;
    uint8_t pv1 = self->s2_c_prev1;
    uint8_t pv2 = self->s2_c_prev2;
    int32_t nw  = (int32_t)(int8_t)self->s2_c_next_word;
    int32_t pw  = (int32_t)(int8_t)self->s2_c_prev_word;
    int32_t klass = -1;
    int32_t base, ctx, scan, v, i;
    const uint8_t *tab;
    uint8_t stress, a;
    int16_t ch, cn;

    i = (int32_t)(int8_t)c - 0x4c;
    base = ((uint32_t)i <= 0x32) ? base_of[g_nasal_base_idx[i]] : 0x1d;

    /* "s" and "z" after an affricate belong to the affricate. */
    if (pv1 == ' ' && (pv2 == 'S' || pv2 == 'Z') && c != '~')
        pv1 = pv2;

    stress = (uint8_t)((st->ctl->flags & 0x20u) != 0);
    scan = (Stage2_Scan(self, 1, 0x10, 0x140, -8, 0) != 0);

    /* where in the word this sonorant sits */
    if ((pv1 == ' ' && pv2 == ' ') || pw == '&' || pw == '%')
        ctx = 1;
    else if (nw == '&' || nw == '%' || (nx1 == ' ' && nx2 == ' ') ||
             nx1 == '.' || nx1 == '!' || nx1 == ',' || nx1 == '?' ||
             nx1 == '\\')
        ctx = 0;
    else
        ctx = 2;

    /* what is either side of it */
    ch = (int16_t)(int8_t)pv1;
    if (Phone_Attr((int16_t)(ch | 0x100)) & 2) {
        cn = (int16_t)(int8_t)nx1;
        a = (uint8_t)(Phone_Attr((int16_t)(cn | 0x100)) & 2);
        if (a != 0)
            klass = (Phone_Attr((int16_t)(cn | 0x180)) & 0x80) != 0;
        else if (Phone_Attr((int16_t)(cn | 0x180)) & 1)
            klass = 2;
    }
    if (klass < 0) {
        if (pv1 == ' ' && pv2 == ' ')
            klass = 3;
        else if ((nx1 == ' ' && nx2 == ' ') || nx1 == '.' || nx1 == '!' ||
                 nx1 == ',' || nx1 == '?' || nx1 == '\\')
            klass = 4;
        else if ((Phone_Attr((int8_t)pv2) & 0x10) && !(Phone_Attr(ch) & 0x10) &&
                 (Phone_Attr((int16_t)(ch | 0x80)) & 0x20))
            klass = 5;
    }
    if (klass < 0) {
        /* no context rule fitted: classify the letters themselves */
        if ((pv1 == 's' && pv2 == 'C') || (pv1 == 'z' && pv2 == 'J'))
            pv1 = pv2;
        i = (int32_t)(int8_t)pv1 - 0x42;
        if ((uint32_t)i <= 0x3c)
            klass = prev_class[g_nasal_prev_idx[i]];
        if (klass < 0) {
            i = (int32_t)(int8_t)nx1 - 0x42;
            if ((uint32_t)i <= 0x3c)
                klass = next_class[g_nasal_next_idx[i]];
            if (klass < 0)
                klass = 1;
        }
    }

    tab = TV_REF(uint8_t, g_dur_tables[base]);
    if (c == 'M' || c == 'N' || c == 'm' || c == 'n') {
        v = tab[stress ? (scan * 3 + ctx) * 20 + klass
                       : (scan * 3 + ctx + 6) * 20 + klass];
        if (c == 'n') {
            if (ctx != 0) {
                if (pv1 == 'q')
                    v = (scan == 0) ? 10 : 12;
                else
                    v += 5;
            } else if (klass == 4) {
                v = 0x17;
            } else if (pv1 == 'q' && scan == 0) {
                v = (Phone_Attr((int16_t)((int16_t)(int8_t)nx1 | 0x100)) & 2)
                    ? 9 : 14;
            }
        }
    } else {
        if (c == 'R' || c == 'L' || c == 'W' || c == 'Y')
            ctx = (ctx != 1);
        else if (ctx == 2)
            ctx = 1;
        v = tab[stress ? (ctx + scan * 2) * 20 + klass
                       : (ctx + scan * 2 + 4) * 20 + klass];
    }

    if (v == 0) {
        v = stress ? tab[1] : tab[0x51];
        if (v == 0)
            v = g_dur_floor[(int32_t)(int8_t)c * 2 + (stress ? 0 : 1)];
    }

    /* a sonorant between two stops loses a frame */
    if (c != 'l' && c != 'm' && c != 'n' &&
        klass != 5 && klass != 7 && klass != 0xd) {
        uint8_t fa = (uint8_t)(Phone_Attr((int16_t)(
            (int16_t)(int8_t)self->s2_c_prev1 | 0x80)) & 0x20);
        uint8_t fb = 0;
        int32_t drop = 0;

        if (fa != 0 && (Phone_Attr((int16_t)(
                (int16_t)(int8_t)self->s2_c_prev2 | 0x80)) & 0x20)) {
            drop = 1;
        } else {
            fb = (uint8_t)(Phone_Attr((int16_t)(
                (int16_t)(int8_t)self->s2_c_next1 | 0x80)) & 0x20);
            if (fb != 0 && (Phone_Attr((int16_t)(
                    (int16_t)(int8_t)self->s2_c_next2 | 0x80)) & 0x20))
                drop = 1;
            else if (fa != 0 && fb != 0)
                drop = 1;
        }
        if (drop)
            v--;
    }

    if (c == 'N' && (nx1 == 'L' || nx1 == 'l' || nx1 == 'j'))
        v--;

    if (self->s2_1e20 != 0) {
        int32_t vw = (Phone_Attr((int16_t)((int16_t)(int8_t)nx1 | 0x100)) & 2)
                     != 0;

        if (c == 'M') {
            if (scan == 0) {
                if (vw)
                    v += (self->s2_next1->flags & 0x20u) ? 5 : 8;
            } else if (vw) {
                v += (self->s2_next1->flags & 0x20u) ? 4 : 6;
            }
        } else if (c == 'N') {
            if (scan == 0) {
                if (vw)
                    v += (self->s2_next1->flags & 0x20u) ? 5 : 3;
            } else if (vw) {
                v += 5;
            }
        }
    }

    if (c == 'n' && scan == 0 && nx1 != ' ')
        v = 9;

    /* a word ending "-nds"/"-nts" keeps the sonorant longer */
    if ((nx1 == 'Z' || nx1 == 'D') && nx2 == '.') {
        if (nx1 == 'D') {
            if (c == 'l' || c == 'j')
                v += 5;
            else if (c == 'N' || c == '~')
                v += 3;
            else if (c == 'M')
                v += 4;
        } else {
            if (c == 'l' || c == 'j' || c == '~')
                v += 4;
            else if (c == 'N' || c == 'M')
                v += 2;
        }
    }

    if (c == 'R' && (pw == '4' || pw == '5') &&
        (Phone_Attr((int16_t)((int16_t)(int8_t)nx1 | 0x100)) & 2))
        v = 3;

    if (v < 3)
        v = 3;
    return v;
}

/* How much a liquid after the vowel takes off, by stress and vowel. */
/* @0x100eda20 */ extern const uint8_t g_vowel_liquid[];
/* Which rule each letter around the vowel selects. */
/* @0x1005b99c */ extern const uint8_t g_vw_glottal_idx[61];
/* @0x1005ba40 */ extern const uint8_t g_vw_vowel_idx[74];
/* @0x1005bb04 */ extern const uint8_t g_vw_next_idx[61];
/* The closed-syllable corrections, one table per stress/scan/position. */
/* @0x1005bb80 */ extern const uint8_t g_vw_t1_idx[74];
/* @0x1005bc00 */ extern const uint8_t g_vw_t2_idx[74];
/* @0x1005bc80 */ extern const uint8_t g_vw_t3_idx[74];
/* @0x1005bd04 */ extern const uint8_t g_vw_t4_idx[74];
/* @0x1005bd88 */ extern const uint8_t g_vw_t5_idx[74];
/* @0x1005be0c */ extern const uint8_t g_vw_t6_idx[74];
/* @0x1005be8c */ extern const uint8_t g_vw_t7_idx[74];
/* @0x1005bf04 */ extern const uint8_t g_vw_t8_idx[61];
/* @0x1005bf74 */ extern const uint8_t g_vw_t9_idx[61];

/* Is this one of the liquids and glides that colour the vowel before it? */
static int32_t is_liquid(uint8_t c)
{
    return c == 'R' || c == 'L' || c == 'W' || c == 'Y' || c == 'j';
}

/*
 * The duration of a vowel.
 *
 * As with the sonorants a table is picked for the vowel and read with the
 * word position, the following stress and a class taken from the phonemes
 * either side.  Vowels carry far more of the rhythm than consonants do, so
 * what follows is a long tail of corrections: the closed-syllable shortening
 * (nine small tables of their own), the word-class rules from
 * Stage2_DurTable, and a handful of lexical cases the tables cannot say.
 */
/* @0x1005aaf0 */
int32_t TV_THISCALL Stage2_DurVowel(Engine *self)
{
    static const signed char d_t1[15] = {
        -1, -1, -3, -3, -3, -3, -3, -3, -3, -4, -3, -1, -4, -1, -2 };
    static const signed char d_t2[13] = {
        -1, -1, -3, -3, -4, -4, -4, -3, -4, -3, -4, -1, -2 };
    static const signed char d_t3[13] = {
        -10, 0, -10, -10, -10, -10, -10, -7, -10, -7, -10, 0, -12 };
    /* t4 picks a group; each group then depends on scan and position. */
    static const signed char g_t4[14] = {
        1, 0, 2, 2, 2, 1, 2, 1, 1, 2, 1, 2, 0, 3 };
    static const signed char d_t5[14] = {
        -2, 0, -2, -2, -2, -2, -1, 0, -1, -2, -1, -2, 0, -3 };
    static const signed char d_t6[14] = {
        -2, 0, -2, -3, -2, -3, -1, 0, -1, -3, -1, -3, 0, -4 };
    static const signed char d_t7[13] = {
        -3, 0, -3, -3, -3, -3, -2, -2, -2, -3, -2, 0, -4 };
    static const signed char d_t8[11] = {
        -1, -2, -2, -2, -2, -2, -2, -2, -2, -1, -4 };
    static const signed char d_t9[12] = {
        0, -1, -1, -1, -1, -1, -1, -1, 0, -1, 0, -3 };
    static const signed char vowel_of[25] = {
        0x13, 0x14, 0x15, 0x16, 2, 0, 0xb, 7, 0xd, 4, 9, 0x11, 3,
        0xc, 0xf, 1, 0x10, 5, 0x12, 8, 0xa, 6, 0xe, 0x17, -1 };
    static const signed char next_class[30] = {
        5, 3, 5, 4, 5, 4, 5, 3, -2, -3, 3, 0xe, 4, 3, 6, 0xf, 4, 0xf,
        6, 6, -2, -2, -3, 3, 4, 5, 6, 6, -3, -1 };
    static const signed char glottal_of[9] = { 2, 4, 5, 8, 4, 4, -1, 6, 7 };

    StageCtx *st = &self->stage_ctx[2];
    Node *n, *p, *q, *r, *nn, *p3;
    uint8_t c    = self->s2_1db1;
    uint8_t pv1  = self->s2_c_prev1;
    uint8_t pv2  = self->s2_c_prev2;
    uint8_t nx1  = self->s2_c_next1;
    uint8_t nx2  = self->s2_c_next2;
    int32_t nw   = (int32_t)(int8_t)self->s2_c_next_word;
    int32_t pw   = (int32_t)(int8_t)self->s2_c_prev_word;
    int32_t follow = 0;
    int32_t klass = -1;
    int32_t at_p = 0;                  /* [esp+0x34] */
    int32_t ctx, scan, vidx, raw, v, i, level;
    const uint8_t *tab;
    uint8_t stress, a, b, d;
    int16_t ch, cn;

    n = Node_NextWord(self, self->s2_next_word);
    if (n != NULL)
        follow = (int32_t)(int8_t)n->value;

    /* a flapped "t" is really the stop it came from */
    if (nx1 == 't') {
        d = st->ctl->next->b18;
        if (d == 'E')
            nx1 = 'D';
        else if (d == 'T')
            nx1 = 'T';
    }
    if (nx2 == 't') {
        d = st->ctl->next->next->b18;
        if (d == 'E')
            nx2 = 'D';
        else if (d == 'T')
            nx2 = 'T';
    }

    if (c == 'p') {
        /* the glottal stop takes its length from the letter before it */
        i = (int32_t)(int8_t)pv1 - 0x42;
        v = ((uint32_t)i <= 0x3c) ? glottal_of[g_vw_glottal_idx[i]] : 7;
        if (v < 0)
            v = (Phone_Attr((int8_t)pv2) & 0x10) ? 4 : 6;
        return (v < 3) ? 3 : v;
    }

    stress = (uint8_t)((st->ctl->flags & 0x20u) >> 5);
    if ((Phone_Attr(nw) & 8) || nx1 == 'p' || nx1 == ' ' || nx1 == ',' ||
        (Phone_Attr(nw | 0x80) & 1))
        ctx = 0;
    else
        ctx = 1;

    scan = (Stage2_Scan(self, 1, 0x10, 0x140, -8, 0) != 0);

    if (nx1 == ' ') {
        /* look past the word boundary for the phoneme that really follows */
        nx1 = nx2;
        p = Stage2_NextPhone(self, self->s2_next2);
        nx2 = (p != NULL) ? p->value : 0;
    }

    if (scan == 1 && ctx != 0 && !(Phone_Attr(nw | 0x100) & 2) &&
        nw != 'l' && nw != 'm' && nw != 'n') {
        /* an unstressed function word after this one does not close it */
        uint8_t keep = 0;

        a = n->value;
        while (a != '&') {
            if (a == '%' || a == '.' || a == '?' || a == '!')
                break;
            if (keep)
                break;
            if ((Phone_Attr((int16_t)((int16_t)(int8_t)a | 0x100)) & 2) ||
                a == 'l' || a == 'm' || a == 'n') {
                if (a != 'p')
                    keep = 1;
            }
            n = n->next;
            if (n == NULL)
                break;
            a = n->value;
        }
        if (!keep)
            ctx = 0;
    }

    ch = (int16_t)(int8_t)c;
    raw = (int32_t)ch;
    i = raw - 0x33;
    vidx = ((uint32_t)i <= 0x49) ? vowel_of[g_vw_vowel_idx[i]] : -1;
    if (vidx < 0)
        vidx = raw;     /* the original reads a half-set local here */

    if (is_liquid(pv1)) {
        cn = (int16_t)(int8_t)nx1;
        a = Phone_Attr(cn);
        if ((a & 0x40) || (Phone_Attr((int16_t)(cn | 0x100)) & 1)) {
            /* vowel between two liquids */
            klass = (a & 4) ? 0x13 : 0x11;
            if (Phone_Attr((int16_t)(cn | 0x100)) & 1)
                klass--;
            goto have_class;
        }
    }

    if (pv1 == ' ' && pv2 == ' ' &&
        (Phone_Attr((int16_t)((int16_t)(int8_t)nx1 | 0x80)) & 1)) {
        klass = 1;
    } else {
        cn = (int16_t)(int8_t)nx1;
        if (!(Phone_Attr(cn) & 0x80)) {
            klass = 0;
        } else if ((Phone_Attr((int16_t)((int16_t)(int8_t)pv1 | 0x180)) & 1) &&
                   (Phone_Attr((int16_t)(cn | 0x180)) & 1)) {
            klass = 2;
        } else if (Phone_Attr((int16_t)(cn | 0x100)) & 2) {
            klass = 0xd;
        } else {
            i = (int32_t)cn - 0x42;
            if ((uint32_t)i <= 0x3c) {
                klass = next_class[g_vw_next_idx[i]];
                if (klass == -2 || klass == -3) {
                    /* a stop or fricative after the vowel, graded by what
                     * comes after that */
                    int32_t hi = (klass == -3);

                    a = Phone_Attr((int8_t)nx2);
                    if (!(a & 0x20) || follow == '&' || follow == '%')
                        klass = hi ? 0xc : 0xb;
                    else if (!(a & 4) || nx2 == 'q')
                        klass = hi ? 9 : 7;
                    else
                        klass = hi ? 0xa : 8;
                }
            }
        }
    }
have_class:

    if (nx1 == 't') {
        ctx = 1;
    } else if (c == 'y' && nw == 'l' &&
               (follow == '&' || follow == '%' ||
                (Phone_Attr(follow | 0x80) & 0x40))) {
        ctx = 0;
    }

    tab = TV_REF(uint8_t, g_dur_tables[vidx]);
    v = tab[stress ? (ctx + scan * 2) * 20 + klass
                   : (ctx + scan * 2 + 4) * 20 + klass];

    if (klass == 0x12 && (Phone_Attr(nw) & 0x10) &&
        (((Phone_Attr(follow | 0x280) & 0x40) &&
          (Phone_Attr(follow | 0x100) & 1)) || follow == 'q')) {
        if (stress == 1) {
            if (scan == 0) {
                i = raw - 0x33;
                v += ((uint32_t)i <= 0x49) ? d_t1[g_vw_t1_idx[i]] : -2;
            } else if (ctx != 0) {
                i = raw - 0x33;
                v += ((uint32_t)i <= 0x49) ? d_t2[g_vw_t2_idx[i]] : -2;
            } else {
                i = raw - 0x33;
                v += ((uint32_t)i <= 0x49) ? d_t3[g_vw_t3_idx[i]] : -12;
            }
        } else {
            static const signed char grp[4][3] = {
                {  0,  0,  0 },   /* group 0: no change */
                { -1, -2, -4 },   /* group 1 */
                { -2, -3, -6 },   /* group 2 */
                { -3, -4, -7 }    /* group 3 (the default) */
            };
            int32_t g;

            i = raw - 0x33;
            g = ((uint32_t)i <= 0x49) ? g_t4[g_vw_t4_idx[i]] : 3;
            v += grp[g][scan == 0 ? 0 : (ctx != 0 ? 1 : 2)];
        }
    }

    if (klass == 2 && (Phone_Attr(nw | 0x100) & 0x40) &&
        (((Phone_Attr(follow | 0x280) & 0x40) &&
          (Phone_Attr(follow | 0x100) & 1)) || follow == 'q')) {
        if (stress == 1) {
            i = raw - 0x33;
            if (scan == 0)
                v += ((uint32_t)i <= 0x49) ? d_t5[g_vw_t5_idx[i]] : -3;
            else if (ctx != 0)
                v += ((uint32_t)i <= 0x49) ? d_t6[g_vw_t6_idx[i]] : -4;
            else
                v += ((uint32_t)i <= 0x49) ? d_t7[g_vw_t7_idx[i]] : -4;
        } else if (scan == 1 && ctx == 0) {
            i = raw - 0x40;
            v += ((uint32_t)i <= 0x3c) ? d_t8[g_vw_t8_idx[i]] : -4;
        } else {
            i = raw - 0x40;
            v += ((uint32_t)i <= 0x3c) ? d_t9[g_vw_t9_idx[i]] : -3;
        }
    }

    if ((Phone_Attr((int16_t)(ch | 0x180)) & 0x40) &&
        (Phone_Attr(nw | 0x100) & 2))
        v += 3;

    if (c != 'i' && c != 'v' && c != '|' && c != '@' && is_liquid(pv1) &&
        (Phone_Attr((int16_t)((int16_t)(int8_t)nx1 | 0x100)) & 2)) {
        if (scan == 1)
            v -= stress ? 2 : 1;
        else if (stress)
            v--;
    }

    level = (int32_t)((st->ctl->flags & 0x18u) >> 3);
    if (v == 0) {
        /* the table has nothing: fall back on the vowel's own default */
        v = stress ? tab[0x23] : tab[0x73];
        if (v == 0)
            v = g_dur_floor[raw * 2 + (stress ? 0 : 1)];
        if (level == 1)
            v -= 2;
        return (v < 3) ? 3 : v;
    }
    if (level == 1)
        v -= 2;

    if ((c == 'i' || c == 'v' || c == '|' || c == '@') && is_liquid(pv1) &&
        ((Phone_Attr((int16_t)((int16_t)(int8_t)nx1 | 0x100)) & 2) ||
         is_liquid(nx1)) && stress != 0)
        v--;

    if (st->ctl->syllable == 1 && c != 'i' && c != 'v' && c != '|' &&
        c != '@' && is_liquid((uint8_t)pw)) {
        q = st->ctl->prev->prev;
        cn = (int16_t)(int8_t)q->value;
        if ((Phone_Attr((int16_t)(cn | 0x100)) & 1) &&
            (Phone_Attr((int16_t)(cn | 0x80)) & 0x20) &&
            q->prev->value == 'S')
            v -= (scan != 0) ? 2 : 1;
    }

    if ((Phone_Attr(nw | 0x80) & 0x40) && is_liquid(pv1))
        v -= g_vowel_liquid[stress * 23 + vidx];

    if (st->ctl->b19 != 0) {
        v -= Stage2_DurTable(self, nx1, scan, stress);

        n = st->ctl;
        if (n->b19 == 0xf && c == 'k' && pv1 == 'x' &&
            (Phone_Attr((int16_t)((int16_t)(int8_t)n->next->value | 0x80)) &
             0x40) && scan != 0 && stress != 0) {
            st->ctl = n->prev->prev->prev;
            r = Stage2_Find(self, 0, 0x10, 8);
            if (r != NULL) {
                b = r->b15;
                if (b == 2 || b == 8 || b == 9 || b == 0xb || b == 0xc)
                    v -= 0xa;
            }
            st->ctl = n;
        }

        if (n->b19 == 1 && c == 'v' && nx1 == 'S') {
            st->ctl = n->prev->prev;
            r = Stage2_Find(self, 0, 0x10, 8);
            if (r != NULL) {
                b = r->b15;
                if (b == 3 || b == 4 || b == 0xa || b == 0xc)
                    v -= 3;
            }
            st->ctl = n;
        }

        if (n->b19 == 5 && c == 'a' && pv1 == 'x') {
            /* the article "a": how long it is depends on the word after */
            r = Stage2_Find(self, 0, 0x10, 8);
            if (r != NULL) {
                p = r->prev;
                d = p->value;
                at_p = ((d == 'P' && NODE_TYPE(p) == 0) || d == ',');
            }
            if (scan == 0 && at_p == 0) {
                n = st->ctl;
                st->ctl = n->prev->prev;
                r = Stage2_Find(self, 0, 0x10, 8);
                if (r != NULL) {
                    int32_t plain, odd;

                    st->ctl = n;
                    nn = n->next->next;
                    p3 = n->prev->prev->prev;
                    if (r->b15 != 7)
                        plain = 0;
                    else if (r->next->value == 'w' &&
                             r->next->next->value == 'j')
                        plain = 0;
                    else if (p3->value == '3' && p3->prev->value == 'x' &&
                             p3->prev->prev->value == 'E')
                        plain = 0;
                    else
                        plain = 1;

                    odd = 0;
                    if (nn->b15 == 0xd &&
                        !(nn->next->value == 'W' &&
                          nn->next->next->value == 'i' &&
                          nn->next->next->next->value == 'C' &&
                          nn->next->next->next->next->value == 's'))
                        odd = 1;

                    if (plain || nn->b15 == 1 || nn->b15 == 5 || odd) {
                        v -= 4;
                    } else if (nn->b15 == 2 &&
                               !(nn->next->value == 'K' &&
                                 nn->next->next->value == 'a' &&
                                 nn->next->next->next->value == 'N')) {
                        v -= 4;
                    }
                }
                st->ctl = n;
            }
        }

        if (st->ctl->b19 == 1) {
            if (c == 'E' && pv1 != 'W' &&
                (nx1 == 'j' || nx1 == 'D' || nx1 == 't'))
                v -= 3;
            if (c == 'E' && pv1 == 'W' &&
                (Phone_Attr((int16_t)((int16_t)(int8_t)nx1 | 0x80)) & 0x20) &&
                !is_liquid(nx1))
                v -= 3;
        }
    }

    n = st->ctl;
    if (n->b19 == 0xa && c == 'u') {
        if (v >= 6)
            v = 6;
        if (nx1 == 'P' || nx1 == 'T' || nx1 == 'K')
            v = 3;
        else if (nx1 == 'S' && (nx2 == 'P' || nx2 == 'T' || nx2 == 'K' ||
                                nx2 == 'B' || nx2 == 'D' || nx2 == 'G'))
            v = 3;
    }

    if (n->value == 'v' && n->next->value == 'N' && n->prev->value == '&' &&
        v < 8)
        v = 8;

    return (v < 3) ? 3 : v;
}

/* Which rule the fricative, the phoneme before it and the one after select. */
/* @0x10057eb0 */ extern const uint8_t g_fric_base_idx[53];
/* @0x10057f64 */ extern const uint8_t g_fric_prev_idx[61];
/* @0x1005801c */ extern const uint8_t g_fric_next_idx[61];

/* The sign-extended index Phone_Attr wants for a phoneme byte. */
static int32_t px(uint8_t b)
{
    return (int32_t)(int8_t)b;
}

/*
 * The duration of a fricative.
 *
 * Fricatives are the one class the grid alone cannot size: how long they run
 * depends on voicing, on whether the phonemes either side are vowels, and on
 * the word the scan is heading into.  So most of this is a decision tree that
 * answers outright; what it leaves behind is a class (klass) and a table
 * (base) the shared grid at the end reads in the usual way.
 */
/* @0x10056590 */
int32_t TV_THISCALL Stage2_DurFric(Engine *self)
{
    /* jt2/jt3: which of the seven context rules applies, 0 for none. */
    static const unsigned char prev_grp[31] = {
        1, 2, 1, 3, 1, 3, 1, 2, 4, 5, 2, 1, 4, 3, 2, 6, 4, 3, 4, 6,
        6, 4, 4, 7, 1, 3, 1, 6, 6, 5, 0 };
    static const unsigned char next_grp[30] = {
        1, 2, 1, 3, 1, 3, 1, 2, 4, 5, 2, 1, 4, 3, 2, 6, 4, 3, 4, 6,
        4, 4, 5, 1, 3, 1, 6, 6, 5, 0 };

    StageCtx *st = &self->stage_ctx[2];
    Node *q;
    uint8_t c   = self->s2_1db1;
    uint8_t pv1 = self->s2_c_prev1;
    uint8_t pv2 = self->s2_c_prev2;
    uint8_t nx1 = self->s2_c_next1;
    uint8_t nx2 = self->s2_c_next2;
    int32_t nw  = px(self->s2_c_next_word);
    int32_t pw  = px(self->s2_c_prev_word);
    uint8_t fC = 0, fJ = 0, stress, f8, a_pv, a;
    int32_t base, ctx, scan, klass = 0, v = 0, i, grp;

    i = px(c) - 0x46;
    switch (((uint32_t)i <= 0x34) ? g_fric_base_idx[i] : 10) {
    case 0:  base = 0x18; break;
    case 2:  base = 0x1a; break;
    case 4:  base = 0x19; break;
    case 5:  base = 0x1c; break;
    case 7:  if (pv1 == 'C') fC = 1; base = 0x1b; break;
    case 9:  if (pv1 == 'J') fJ = 1; base = -1;   break;
    case 10: base = 0x18; break;
    default: base = -1;   break;
    }

    /* Across a word boundary the fricative really belongs with the phoneme
     * on the far side, so look through the space. */
    if (((c == 'S' || c == 'Z') && nx1 == ' ' &&
         (nx2 == 'M' || nx2 == 'N' || (Phone_Attr(px(nx2) | 0x180) & 1))) ||
        (c == 'S' && nx1 == ' ' && nx2 == 'Z'))
        nx1 = nx2;
    if (c == 'Z' && pv1 == ' ' && pv2 == 'S')
        pv1 = pv2;
    if (((c == 's' && pw == 'C') || (c == 'z' && pw == 'J')) && nx1 == ' ' &&
        ((Phone_Attr(px(nx2)) & 0x10) || nx2 == 'L') &&
        (Phone_Attr(px(st->ctl->next->next->next->value) | 0x100) & 2))
        nx1 = nx2;

    stress = (uint8_t)((st->ctl->flags & 0x20u) >> 5);
    scan = (Stage2_Scan(self, 1, 0x10, 0x140, -8, 0) != 0);
    f8 = (uint8_t)(Phone_Attr(nw) & 8);
    if (f8 != 0 || nx1 == 'p' || nx1 == ' ' || nx1 == ',' ||
        (Phone_Attr(nw | 0x80) & 1))
        ctx = 0;
    else
        ctx = 1;

    /* "S" before a long pause behaves as if a stress followed. */
    if (c == 'S') {
        q = st->ctl->next->next;
        if (q->value == ' ' && q->arg > 0x14)
            scan = 1;
    }

    if (pv1 == ' ') {
        klass = 0;
        v = 6;
        if (c == 'V') {
            v = stress ? 9 : 8;
        } else if (c == 'H') {
            if (scan != 0)
                v = 7;
            else if (stress != 0)
                v = 8;
        } else if (c == 'x') {
            if (stress == 0)
                v = 5;
            else if (scan != 0)
                v = 8;
        }
        goto done;
    }

    a_pv = (uint8_t)Phone_Attr(px(pv1) | 0x100);
    if ((a_pv & 2) && (Phone_Attr(px(nx1) | 0x100) & 2)) {
        /* between two vowels */
        if (Phone_Attr(pw) & 8) {
            klass = 0x13;
            v = 9;
            if (c == 'V') {
                if (scan == 0 && ctx == 0)
                    v = 5;
                else if (stress == 0)
                    v = 7;
            } else if (c == 'x') {
                v = 5;
            } else if (c == 'd') {
                if (stress == 0)
                    v = scan ? 7 : 6;
            } else if (c == 'z') {
                v = stress ? (scan ? 0xb : 9) : (scan ? 0xa : 8);
            } else if (fC != 0) {
                base = -1;
                v = 8;
            }
        } else {
            klass = 1;
            v = 8;
            if (fC != 0) {
                base = -1;
                v = 6;
            } else if (c == 'd') {
                v = stress ? (scan ? 9 : 8) : 7;
            } else if (c == 'z') {
                v = stress ? (scan ? 0xb : 9) : (scan ? 0xa : 0xb);
            } else if (c == 'V') {
                if (stress != 0)
                    v = scan ? 9 : 8;
                else if (scan == 0)
                    v = (ctx == 0) ? 6 : 5;
                else
                    v = (ctx == 0) ? 8 : 4;
            } else if (c == 'x') {
                if (scan == 0 && ctx != 0)
                    v = 5;
                else if (stress == 0 && scan != 0)
                    v = 6;
                else
                    v = 7;
            }
        }
        goto done;
    }

    if (scan != 0 && ctx == 0) {
        klass = 2;
        if (c == 'x') {
            v = stress ? 0xd : 0xf;
        } else if (c == 'z') {
            v = 0x12;
        } else if (c == 'V') {
            v = 0x13;
        } else if (fC != 0) {
            base = -1;
            v = 0x10;
        } else if (fJ != 0) {
            v = 0xe;
        }
        goto done;
    }

    /* Otherwise the phoneme before, then the one after, picks the rule. */
    i = px(pv1) - 0x42;
    grp = ((uint32_t)i <= 0x3c) ? prev_grp[g_fric_prev_idx[i]] : 0;
    switch (grp) {
    case 1:   /* L057163 */
        if (!(Phone_Attr(pw) & 8)) {
            klass = 5;
            if (fJ != 0 &&
                ((Phone_Attr(px(pv2) | 0x80) & 0x20) ||
                 (Phone_Attr(px(nx2) | 0x80) & 0x20))) {
                base = -1;
                if (stress != 0) {
                    if (ctx != 0)
                        v = 7;
                } else if (scan == 0) {
                    v = 6;
                } else if (ctx != 0) {
                    v = 7;
                } else if ((Phone_Attr(px(nx1) | 0x100) & 1) &&
                           (Phone_Attr(px(nx1) | 0x80) & 0x20) &&
                           (Phone_Attr(px(nx2) | 0x80) & 0x20)) {
                    v = 7;
                }
                goto done;
            }
            v = 6;
            if (c == 'x') {
                if (stress != 0)
                    v = ctx ? 7 : 6;
                else if (scan == 0 || ctx != 0)
                    v = 5;
            } else if (c == 'd') {
                v = stress ? (scan ? 8 : 7) : (scan ? 7 : 6);
            } else if (c == 'z') {
                v = (stress == 0 && ctx != 0) ? 7 : 8;
            } else if (c == 'V') {
                if (stress != 0)
                    v = ctx ? 8 : 6;
                else if (scan != 0 && ctx != 0)
                    v = 7;
            } else if (fC != 0) {
                base = -1;
                v = 8;
            }
            goto done;
        }
        klass = 6;
        if (c == 'V') {
            if (scan != 0 && ctx == 0)
                v = 6;
            else if (stress != 0 && scan != 0 && ctx != 0)
                v = 9;
            else if (stress == 0 && scan == 0)
                v = 7;
            else
                v = 8;
        } else if (c == 'd') {
            if ((stress != 0 && scan != 0) ||
                (stress == 0 && scan != 0 && ctx == 0))
                v = 7;
            else
                v = 6;
        } else if (c == 'x') {
            v = (stress != 0 && ctx != 0) ? 7 : 5;
        }
        goto done;

    case 2:   /* L057416 */
        if (!(Phone_Attr(pw) & 8)) {
            klass = 3;
            if (fC != 0 && ctx != 0) {
                v = stress ? 9 : (scan ? 8 : 7);
                base = -1;
                goto done;
            }
            if (c == 'S' && !(Phone_Attr(px(pv1)) & 4) && scan == 0 &&
                (a_pv & 1) && !(Phone_Attr(px(nx1)) & 4) &&
                (Phone_Attr(px(nx1) | 0x80) & 0x20) && stress == 0 &&
                ctx == 0) {
                base = -1;
                v = 6;
                goto done;
            }
            v = 6;
            if (c == 'H') {
                if (stress == 0)
                    v = 5;
                else if (fC != 0)
                    v = 9;
            }
            goto done;
        }
        klass = 4;
        if (c == 'V') {
            if ((a_pv & 1) && (Phone_Attr(px(pv1) | 0x80) & 0x20) &&
                (Phone_Attr(px(pv2) | 0x80) & 0x20) && ctx != 0) {
                v = stress ? (scan ? 9 : 7) : (scan ? 6 : 5);
                base = -1;
            } else if (scan != 0) {
                v = stress ? 0xb : 8;
            } else {
                v = 0xa;
            }
        } else if (c == 'H') {
            if (scan == 0)
                v = ctx ? 5 : 7;
            else if (stress == 0)
                v = 6;
            else
                v = ctx ? 7 : 6;
        } else if (c == 'x') {
            if (stress != 0 && scan != 0)
                v = 9;
            else if (stress == 0 && scan == 0)
                v = 5;
            else
                v = 6;
        }
        goto done;

    case 3:   /* L057688 */
        klass = 7;
        if (c == 'F' && (Phone_Attr(px(pv1)) & 0x40) &&
            (Phone_Attr(px(pv1) | 0x80) & 0x20) &&
            (Phone_Attr(px(pv2) | 0x100) & 1) &&
            (Phone_Attr(px(pv2) | 0x80) & 0x20) && stress == 0 &&
            scan != 0 && ctx != 0) {
            base = -1;
            v = 7;
        } else if (c == 'H') {
            if (stress == 0)
                v = 5;
            else
                v = scan ? 6 : 7;
        } else if (c == 'x') {
            if (stress == 0)
                v = 6;
            else if (scan == 0)
                v = 7;
            else
                v = ctx ? 8 : 6;
        } else if (c == 'V') {
            if (stress != 0 && ctx != 0)
                v = 7;
            else if (scan == 0 || ctx != 0)
                v = 5;
            else
                v = 8;
        } else if (fC != 0) {
            base = -1;
            v = 0xa;
        } else if (fJ != 0) {
            v = 7;
        }
        goto done;

    case 4:   /* L0577d2 */
        klass = 0xa;
        if (c == 'V') {
            v = (scan != 0 && ctx == 0) ? 9 : 6;
        } else if (c == 'd') {
            if (scan != 0 && ctx == 0)
                v = 9;
            else if (stress == 0)
                v = 5;
            else
                v = scan ? 8 : 7;
        } else if (c == 'x') {
            if (stress == 0)
                v = 5;
            else if (scan == 0)
                v = 6;
            else
                v = ctx ? 7 : 5;
        } else if (c == 'H') {
            v = 7;
        }
        goto done;

    case 5:   /* L057898 */
        goto both_sides;

    case 6:   /* L057abf */
        klass = 8;
        v = 7;
        if (fJ != 0) {
            v = 5;
        } else if (c == 'V') {
            if (stress == 0)
                v = 5;
            else if (scan != 0)
                v = 6;
        } else if (c == 'd') {
            if (ctx != 0 && stress == 0)
                v = 5;
            else
                v = (ctx != 0) ? 6 : 10;
        } else if (c == 'H') {
            v = 6;
        } else if (c == 'x') {
            if (stress != 0 && scan != 0 && ctx != 0)
                v = 8;
            else if (stress != 0 && scan == 0)
                v = 7;
            else
                v = 6;
        }
        goto done;

    case 7:   /* L057b7c */
        klass = 9;
        goto voiceless_tail;

    default:
        break;
    }

    i = px(nx1) - 0x42;
    grp = ((uint32_t)i <= 0x3c) ? next_grp[g_fric_next_idx[i]] : 0;
    switch (grp) {
    case 1:   /* L056c45 */
        klass = 0xc;
        if (c == 'V') {
            if (stress != 0 && scan != 0)
                v = 9;
            else if (stress == 0 && scan != 0)
                v = 8;
            else if (stress == 0 && ctx != 0)
                v = 7;
            else
                v = 6;
        } else if (fC != 0) {
            base = -1;
            v = 5;
        } else if (fJ != 0) {
            v = 6;
        } else if (c == 'z') {
            v = stress ? 0xa : 9;
        } else if (c == 'x') {
            v = stress ? (scan ? 8 : 7) : (scan ? 7 : 6);
        }
        goto done;

    case 2:   /* L056d23 */
        klass = 0xb;
        if (c == 'x') {
            v = stress ? (scan ? 8 : 7) : (scan ? 7 : 6);
        } else if (c == 'z') {
            v = stress ? (scan ? 9 : 8) : (scan ? 8 : 7);
        } else if (c == 'V') {
            if (stress == 0)
                v = 6;
            else
                v = scan ? 7 : 5;
        } else if (fC != 0 || fJ != 0) {
            base = -1;
            v = 5;
        }
        goto done;

    case 3:   /* L056dfd */
        klass = 0xe;
        if (c == 'F' && (Phone_Attr(px(nx1)) & 0x40) && ctx != 0 &&
            (Phone_Attr(px(nx2)) & 0x40) && stress == 0 && scan == 0) {
            base = -1;
            v = 5;
        } else if (c == 'x') {
            v = stress ? (scan ? 7 : 6) : (scan ? 6 : 5);
        } else if (c == 'z') {
            v = stress ? (scan ? 0xa : 9) : (scan ? 9 : 8);
        } else if (c == 'V') {
            if (stress != 0 && scan != 0)
                v = 5;
            else if (scan != 0 && ctx == 0)
                v = 5;
            else
                v = 6;
        } else if (fC != 0) {
            base = -1;
            v = 9;
        } else if (fJ != 0) {
            v = 8;
        }
        goto done;

    case 4:   /* L056f1a */
        klass = 0x11;
        if (c == 'V') {
            if (stress != 0)
                v = scan ? 8 : 6;
            else if (scan == 0 && ctx != 0)
                v = 5;
            else
                v = 7;
        } else if (c == 'z') {
            if (stress != 0 || scan != 0)
                v = 7;
            else
                v = (ctx == 0) ? 8 : 6;
        } else if (c == 'x') {
            v = stress ? 8 : 7;
        }
        goto done;

    case 5:   /* L056fc8 */
        klass = 0x10;
        if (c == 'x') {
            v = stress ? 8 : 7;
        } else if (c == 'z') {
            if (stress == 0)
                v = 8;
            else
                v = scan ? 0xa : 9;
        } else if (fC != 0) {
            base = -1;
            v = 6;
        } else if (c == 'V') {
            if (stress != 0)
                v = scan ? 8 : 5;
            else if (scan == 0 && ctx != 0)
                v = 6;
            else
                v = 7;
        }
        goto done;

    case 6:   /* L057080 */
        klass = 0xf;
        if (c == 'V') {
            if (stress == 0)
                v = 6;
            else if (scan != 0)
                v = 7;
            else if (ctx != 0)
                v = 0x12;
            else
                v = 8;
        } else if (fC != 0) {
            base = -1;
            v = 7;
        } else if (fJ != 0) {
            v = 6;
        } else if (c == 'z') {
            v = stress ? (scan ? 0xa : 9) : (scan ? 9 : 8);
        } else if (c == 'x') {
            v = (stress == 0 && ctx != 0) ? 7 : 8;
        }
        goto done;

    default:
        break;
    }

    /* neither side matched */
    klass = 3;
    if (c == 'H') {
        v = stress ? 6 : 5;
    } else if (c == 'x') {
        v = (scan != 0 && ctx == 0) ? 3 : (stress ? 6 : 5);
    } else if (c == 'V') {
        if (stress == 0)
            v = scan ? 0xb : 8;
        else if (scan == 0)
            v = 9;
        else
            v = (ctx == 0) ? 0xb : 0xa;
    } else if (fC != 0) {
        base = -1;
        v = 9;
    } else if (fJ != 0) {
        v = 6;
    }
    goto done;

both_sides:
    /* L057898: the phoneme after is a voiced stop of its own place */
    {
        uint8_t n_voice = (uint8_t)(Phone_Attr(px(nx1) | 0x100) & 1);
        uint8_t n_stop  = 0;

        if (n_voice != 0)
            n_stop = (uint8_t)(Phone_Attr(px(nx1) | 0x80) & 0x20);
        if (n_voice != 0 && n_stop != 0) {
            klass = 0xd;
            if (c == 'S' && (Phone_Attr(px(nx2) | 0x80) & 0x20) &&
                pv1 == 'N' && stress != 0 && ctx != 0 && scan == 0 &&
                !(Phone_Attr(pw) & 8) && f8 == 0) {
                base = -1;
                v = 5;
            } else if (c == 'z' && (Phone_Attr(px(nx2) | 0x80) & 0x20) &&
                       stress == 0 && ctx == 0) {
                base = -1;
                v = 7;
            }
            if (c == 'V')
                v = 6;
            goto done;
        }

        klass = 9;
        if (pv1 == 'N' && c == 'F' && !(Phone_Attr(pw) & 8) && ctx != 0 &&
            (Phone_Attr(px(nx1) | 0x80) & 0x20) && stress != 0 &&
            scan == 0) {
            base = -1;
            v = 9;
            goto done;
        }
    }

voiceless_tail:
    /* shared by the "N"-cluster rule above and the jt2 group that falls
     * through to it */
    if (c == 'V') {
        v = (stress != 0 && scan == 0) ? 8 : 7;
    } else if (fC != 0) {
        base = -1;
        v = 7;
    } else if (fJ != 0) {
        v = 4;
    } else if (c == 'd') {
        if (stress != 0 && ctx != 0)
            v = 7;
        else if (scan != 0 && ctx == 0)
            v = 9;
        else if (stress == 0 && scan == 0)
            v = 6;
        else
            v = 5;
    } else if (c == 'x') {
        if (stress != 0 && scan != 0)
            v = 6;
        else if (stress == 0 && scan != 0 && ctx == 0)
            v = 6;
        else
            v = 5;
    }

done:
    if ((klass == 3 || klass == 4) && fC != 0 &&
        (Phone_Attr(px(nx1) | 0x80) & 0x20) &&
        ((Phone_Attr(px(pv2) | 0x80) & 0x20) ||
         (Phone_Attr(px(nx2) | 0x80) & 0x20))) {
        if (scan == 0)
            v = stress ? 6 : 5;
        else if (ctx != 0)
            v = stress ? 7 : 6;
    }

    if (klass == 0xb || klass == 0xc) {
        if (c == 'S') {
            a = (uint8_t)Phone_Attr(px(nx1) | 0x100);
            if ((a & 4) && (a & 1) &&
                (Phone_Attr(px(nx2) | 0x80) & 0x20) && scan == 0) {
                if (stress == 0)
                    v = 6;
                else if (ctx != 0)
                    v = 9;
                base = -1;
            }
        } else if (c == 'V' && (Phone_Attr(px(nx1) | 0x100) & 1) &&
                   (Phone_Attr(px(nx1) | 0x80) & 0x20) &&
                   (Phone_Attr(px(nx2) | 0x80) & 0x20) && stress == 0 &&
                   scan == 0) {
            v = 5;
            base = -1;
        }
    }

    if (base >= 0) {
        const uint8_t *tab = TV_REF(uint8_t, g_dur_tables[base]);

        v = tab[stress ? (ctx + scan * 2) * 20 + klass
                       : (ctx + scan * 2 + 4) * 20 + klass];
    }

    if (c == 'V') {
        if (nx1 == 'X') {
            v += 4;
        } else {
            a = Phone_Attr(px(nx1));
            if ((a & 4) && (a & 0x40))
                v -= 2;
        }
    } else if (c == 'F' && klass == 0 && scan == 1 &&
               (pw == '&' || pw == '%')) {
        v += 2;
    }

    if (v != 0)
        return (v < 3) ? 3 : v;

    /* nothing said anything: fall back on the table's own default */
    if (base >= 0) {
        const uint8_t *tab = TV_REF(uint8_t, g_dur_tables[base]);

        v = stress ? tab[3] : tab[0x53];
    }
    if (v == 0)
        v = g_dur_floor[px(c) * 2 + (stress ? 0 : 1)];
    return (v < 3) ? 3 : v;
}

/* Which rule the phoneme before and after the stop select. */
/* @0x1005a7d0 */ extern const uint8_t g_stop_a_idx[31];
/* @0x1005a808 */ extern const uint8_t g_stop_b_idx[31];
/* @0x1005a850 */ extern const uint8_t g_stop_c_idx[49];
/* @0x1005a898 */ extern const uint8_t g_stop_d_idx[13];
/* @0x1005a900 */ extern const uint8_t g_stop_e_idx[61];
/* @0x1005a998 */ extern const uint8_t g_stop_f_idx[61];
/* And which release the vowel after it gets. */
/* @0x1005aa44 */ extern const uint8_t g_stop_rel_idx[74];

/* The release code lives in the low byte of the node's d10 word. */
static void set_rel(Node *n, uint32_t k)
{
    n->d10 = (n->d10 & 0xffffff00u) | k;
}

/*
 * The duration of a stop, and the release it gets.
 *
 * The closure of a stop is almost entirely a matter of what sits either side
 * of it, so instead of one grid there are six small tables, tried in turn,
 * each naming a class and a starting length.  A second pass then works out
 * how the stop is released -- aspirated, unreleased, flapped and so on --
 * and writes that into Node.d10 for stage 3 to read.
 */
/* @0x10058a40 */
int32_t TV_THISCALL Stage2_DurStop(Engine *self)
{
    StageCtx *st = &self->stage_ctx[2];
    Node *ctl;
    uint8_t c   = self->s2_1db1;
    uint8_t nx1 = self->s2_c_next1;
    uint8_t nx2 = self->s2_c_next2;
    uint8_t pv1 = self->s2_c_prev1;
    uint8_t pv2 = self->s2_c_prev2;
    uint8_t pw  = self->s2_c_prev_word;
    uint8_t nw  = self->s2_c_next_word;
    int32_t klass = -1, v = 0, scan, ctx, rel, i;
    uint8_t stress, c_stop, nx_stop, pv_stop, voiced;

    scan = (Stage2_Scan(self, 1, 0x10, 0x140, -8, 0) != 0);
    ctx = (pv1 != ' ' && pw != '&' && pw != '%');

    if (pv1 == ' ' && ctx == 0) {
        klass = 0xb;
        v = 0xa;
        if (c == 'K' || c == 'D' || c == 'J')
            v = 0xb;
        else if (c == 'G')
            v = 9;
        goto have_klass;
    }

    if (!(Phone_Attr(px(pv1) | 0x100) & 2) && !is_liquid(pv1)) {
        i = px(nx1) - 0x4c;
        if (Phone_Attr(px(pv1)) & 0x10) {
            if ((uint32_t)i <= 0x1e && g_stop_a_idx[i] != 5) {
                klass = 3;
                v = 5;
                if ((c == 'T' && ctx == 0) || (c == 'P' && scan) ||
                    (c == 'B' && scan && ctx == 0))
                    v = 6;
                else if (c == 'K' && scan && ctx == 0)
                    v = 7;
                else if (c == 'D' && !scan && ctx == 1)
                    v = 4;
            }
        } else if ((uint32_t)i <= 0x1e && g_stop_b_idx[i] != 5) {
            klass = 4;
            v = 6;
            if ((c == 'G' && ctx == 0) || (c == 'P' && ctx == 1) ||
                (c == 'K' && !scan && ctx == 0))
                v = 7;
            else if ((c == 'P' && ctx == 0) || (c == 'K' && scan) ||
                     (c == 'B' && scan == ctx))
                v = 8;
            else if ((c == 'D' || c == 'T') && !scan && ctx == 1)
                v = 5;
            else if (c == 'B' && !scan && ctx == 1)
                v = 9;
            else if (c == 'B' && scan && ctx == 0)
                v = 0xa;
        }
        goto have_klass;
    }

    /* the phoneme before is a vowel or a liquid */
    i = px(nx1) - 0x45;
    if ((uint32_t)i <= 0x30 && g_stop_c_idx[i] != 9) {
        if (g_stop_c_idx[i] == 0 || g_stop_c_idx[i] == 5 ||
            g_stop_c_idx[i] == 6 || g_stop_c_idx[i] == 8) {
            klass = 0;
            v = 8;
            if (ctx == 1) {
                if (c == 'G' || c == 'D' || c == 'B' || (c == 'K' && !scan))
                    v = 7;
                else if (c == 'P' && scan)
                    v = 9;
                else if (c == 'T' && !scan)
                    v = 6;
            } else {
                if (c == 'P' || (c == 'G' && scan))
                    v = 0xa;
                else if ((c == 'T' && scan) || (c == 'B' && !scan))
                    v = 7;
                else if (((c == 'K' || c == 'B') && scan) ||
                         (c == 'G' && !scan))
                    v = 9;
            }
        } else {
            klass = 2;
            v = 8;
            if ((c == 'P' && scan) || (c == 'B' && scan && ctx == 0))
                v = 9;
            else if (((c == 'T' || c == 'B' || c == 'G') && !scan &&
                      ctx == 1) ||
                     ((c == 'K' || c == 'G') && scan && ctx == 0))
                v = 7;
            else if ((c == 'K' && !scan && ctx == 1) ||
                     (c == 'G' && scan && ctx == 1))
                v = 6;
            else if (c == 'B' && !scan && ctx == 0)
                v = 0xa;
            else if (c == 'D' && !scan && ctx == 1)
                v = 5;
        }
        goto have_klass;
    }

    if ((Phone_Attr(px(nx1) | 0x100) & 2) && nx1 != 'p') {
        klass = 1;
        v = 7;
        if (((c == 'P' || c == 'G') && ctx == 0) || (c == 'D' && scan) ||
            (c == 'T' && scan && ctx == 0))
            v = 9;
        else if (((c == 'K' || c == 'B') && scan && ctx == 0) ||
                 (c == 'D' && !scan))
            v = 8;
        else if (c == 'B' && !scan && ctx == 0)
            v = 0xa;
        else if (c == 'G' && scan && ctx == 1)
            v = 6;
    }

have_klass:
    if (klass < 0) {
        /* a second table, keyed on the phoneme after */
        i = px(nx1) - 0x64;
        switch (((uint32_t)i <= 0xc) ? g_stop_d_idx[i] : 4) {
        case 0:
            klass = 6;
            v = 5;
            if (c == 'G' || (c == 'P' && scan))
                v = 6;
            else if ((c == 'T' || c == 'K') && !scan)
                v = 4;
            else if (c == 'B')
                v = 7;
            break;
        case 1:
            klass = 7;
            v = 6;
            if (c == 'P' || (c == 'K' && scan))
                v = 9;
            else if (c == 'T' || (c == 'D' && scan))
                v = 4;
            else if (c == 'K' && !scan)
                v = 8;
            else if (c == 'B' && scan)
                v = 7;
            else if (c == 'D' && !scan)
                v = 3;
            break;
        case 2:
            klass = 8;
            v = 6;
            if (c == 'B' || c == 'D')
                v = 7;
            else if (c == 'K')
                v = scan ? 9 : 8;
            else if (c == 'P')
                v = scan ? 0xa : 9;
            break;
        case 3:
            klass = 5;
            v = 0xb;
            if ((c == 'K' || c == 'B') && !scan)
                v = 7;
            else if ((c == 'T' || c == 'G') && !scan)
                v = 6;
            else if (c == 'D' && scan && pv1 == 'N')
                v = 5;
            else if ((c == 'P' && !scan) || (c == 'D' && scan))
                v = 8;
            else if (c == 'P' && scan)
                v = 0xc;
            else if (c == 'T' && scan)
                v = 0xa;
            else if (c == 'D' && !scan)
                v = 3;
            break;
        default:
            break;
        }
    }

    if (klass < 0) {
        /* a third, keyed on the phoneme before */
        i = px(pv1) - 0x42;
        switch (((uint32_t)i <= 0x3c) ? g_stop_e_idx[i] : 21) {
        case 0: case 2: case 4: case 6: case 17:
            klass = 0xc;
            v = 6;
            if ((c == 'B' && scan) || (c == 'D' && scan && ctx == 0) ||
                (c == 'P' && scan && ctx == 1))
                v = 8;
            else if ((c == 'B' && !scan) || c == 'D' ||
                     (c == 'G' && scan && ctx == 1) ||
                     (c == 'P' && ((!scan && ctx == 1) ||
                                   (scan && ctx == 0))))
                v = 7;
            else if ((c == 'K' && !scan && ctx == 1) ||
                     (c == 'G' && !scan && ctx == 0))
                v = 5;
            else if (c == 'G' && !scan)
                v = 4;
            else if (c == 'J')
                v = 2;
            else if (c == 'C')
                v = 3;
            break;
        case 1: case 7: case 9: case 11: case 15:
            klass = 9;
            v = 6;
            if ((c == 'P' && !scan) ||
                ((c == 'T' || c == 'D' || c == 'G') && scan && ctx == 0) ||
                (c == 'B' && !(!scan && ctx == 1)))
                v = 8;
            else if (((c == 'P' || c == 'T' || c == 'G') && scan &&
                      ctx == 1) ||
                     ((c == 'T' || c == 'K' || c == 'D' || c == 'G') &&
                      !scan && ctx == 0))
                v = 7;
            else if ((c == 'P' && scan && ctx == 0) ||
                     (c == 'B' && !scan && ctx == 1))
                v = 9;
            else if ((c == 'T' && !scan && ctx == 1) ||
                     (c == 'D' && ctx == 1))
                v = 5;
            else if ((c == 'J' && !scan && ctx == 1) ||
                     (c == 'C' && scan == ctx))
                v = 4;
            else if (c == 'C')
                v = 3;
            break;
        case 3: case 5: case 10: case 13: case 16:
            klass = 0xa;
            v = 6;
            if ((c == 'C' && ctx == 1) || (c == 'J' && !scan))
                v = 5;
            else if ((c == 'P' && !scan && ctx == 1) ||
                     (c == 'B' && ctx == 0))
                v = 9;
            else if (c == 'P' || ((c == 'K' || c == 'J') && scan &&
                                  ctx == 0) ||
                     (c == 'B' && ctx == 1) ||
                     (c == 'D' && scan && ctx == 0))
                v = 8;
            else if ((c == 'G' && ctx == 0) || (c == 'D' && scan == ctx) ||
                     (c == 'K' && scan == ctx))
                v = 7;
            break;
        case 8: case 20:
            if (pv1 == 'N' && pv2 == '|' && ctx == 0) {
                klass = 8;
                v = 6;
                if (c == 'B' || c == 'D')
                    v = 7;
                else if (c == 'K')
                    v = scan ? 9 : 8;
                else if (c == 'P')
                    v = scan ? 0xa : 9;
                break;
            }
            klass = 0xe;
            v = 3;
            if ((c == 'P' && !scan) || (c == 'K' && !scan && ctx == 0))
                v = 6;
            else if (c == 'P')
                v = 7;
            else if (c == 'T' || (c == 'K' && scan) ||
                     (c == 'B' && !scan && ctx == 0) ||
                     ((c == 'D' || c == 'G') && scan && ctx == 0))
                v = 5;
            else if (c == 'B' || c == 'K' ||
                     (c == 'D' && !scan && ctx == 0) ||
                     (c == 'G' && !scan))
                v = 4;
            else if (c == 'J' && scan && ctx == 1)
                v = 2;
            break;
        case 12: case 14: case 18: case 19:
            klass = 0xd;
            v = 7;
            if (c == 'C' || (c == 'J' && scan == ctx))
                v = 5;
            else if ((c == 'D' && !scan) || (c == 'T' && !scan && ctx == 0) ||
                     (c == 'J' && scan && ctx == 0))
                v = 6;
            else if ((c == 'K' && (scan || ctx == 1)) ||
                     ((c == 'T' || c == 'G') && scan && ctx == 1) ||
                     (c == 'B' && scan && ctx == 0))
                v = 8;
            else if ((c == 'P' && scan && ctx == 0) ||
                     (c == 'B' && scan && ctx == 1))
                v = 9;
            else if (c == 'P')
                v = 0xa;
            else if (c == 'J' && !scan && ctx == 1)
                v = 4;
            break;
        default:
            break;
        }
    }

    if (klass < 0) {
        /* and a fourth, on the phoneme after again */
        i = px(nx1) - 0x42;
        switch (((uint32_t)i <= 0x3c) ? g_stop_f_idx[i] : 21) {
        case 0: case 2: case 4: case 6: case 17:
            klass = 0x11;
            v = 7;
            if ((c == 'T' || c == 'D' || c == 'G' || c == 'B') && !scan)
                v = 6;
            else if (c == 'T')
                v = 5;
            else if (c == 'K' && scan)
                v = 8;
            else if (c == 'P')
                v = scan ? 0xa : 9;
            break;
        case 1: case 7: case 9: case 11: case 15:
            klass = 0xf;
            v = 7;
            if ((c == 'P' && !scan) || ((c == 'D' || c == 'G') && scan))
                v = 8;
            else if (c == 'P')
                v = 0xa;
            else if (c == 'T')
                v = 5;
            break;
        case 3: case 5: case 10: case 13: case 16:
            klass = 0x10;
            v = 6;
            if ((c == 'P' && scan) || ((c == 'K' || c == 'G') && !scan))
                v = 7;
            else if ((c == 'K' || c == 'B' || c == 'G') && scan)
                v = 8;
            else if (c == 'C' && !scan)
                v = 5;
            break;
        case 8: case 20:
            klass = 0x13;
            v = 6;
            if (c == 'P' || (c == 'K' && !scan))
                v = 0xa;
            else if ((c == 'B' || c == 'G') && scan)
                v = 8;
            else if (c == 'T' && !scan)
                v = 5;
            else if (c == 'K' && scan)
                v = 0xb;
            else if (c == 'D' && scan)
                v = 7;
            break;
        case 12: case 14: case 18: case 19:
            klass = 0x12;
            v = 8;
            if (c == 'P' || (c == 'G' && !scan))
                v = 7;
            else if (c == 'D' || (c == 'B' && !scan) ||
                     (c == 'J' && ctx == 0))
                v = 6;
            else if (c == 'J')
                v = scan ? 5 : 4;
            else if (c == 'T' && scan)
                v = 0xb;
            else if (c == 'G' && scan)
                v = 9;
            break;
        default:
            break;
        }
    }

    if (klass < 0) {
        v = 0xb;
        if ((c == 'K' || c == 'B') && !scan)
            v = 7;
        else if ((c == 'T' || c == 'G') && !scan)
            v = 6;
        else if ((c == 'P' && !scan) || (c == 'D' && scan))
            v = 8;
        else if (c == 'P' && scan)
            v = 0xc;
        else if (c == 'T' && scan)
            v = 0xa;
        else if (c == 'D' && !scan)
            v = 3;
    }

    /* an unstressed stop between a strong vowel and a weak one is shorter */
    if ((Phone_Attr(px(c) | 0x280) & 0x40) && (Phone_Attr(px(pw)) & 0x10) &&
        (Phone_Attr(px(nw)) & 0x60)) {
        if (!scan) {
            if (c == 'P')
                v -= 2;
            else if (c == 'T')
                v -= 1;
        } else if (c == 'P') {
            v -= 2;
        }
    }

    /* ---- the release ---- */
    ctl = st->ctl;
    stress = (uint8_t)((ctl->flags & 0x20u) >> 5);
    if (c == 'P' || c == 'T' || c == 'K') {
        set_rel(ctl, 0);
        rel = -1;
        if (pv1 == ' ' && ctx == 0) {
            rel = 6;
            set_rel(ctl, 4);
            if ((c == 'P' || c == 'K') && !stress)
                set_rel(ctl, 3);
            else if (c == 'T' && !stress)
                set_rel(ctl, 2);
            else if (c == 'K' && stress)
                set_rel(ctl, 5);
        }

        if (rel < 0 &&
            !((Phone_Attr(px(pv1) | 0x100) & 2) &&
              (st->cur->flags & 0x20u) &&
              (Phone_Attr(px(nx1) | 0x100) & 2) &&
              (st->scan->flags & 0x20u))) {
            i = px(nx1) - 0x33;
            switch (((uint32_t)i <= 0x49) ? g_stop_rel_idx[i] : 26) {
            case 0: case 2: case 5: case 15: case 19: case 20:
            case 22:
                rel = 3;
                set_rel(ctl, 2);
                if ((c == 'P' && stress && !scan) ||
                    (c == 'T' && stress && scan) ||
                    (c == 'K' && (stress || (!scan && ctx == 0))))
                    set_rel(ctl, 3);
                else if (c == 'T' && stress)
                    set_rel(ctl, 4);
                break;
            case 1: case 4: case 17: case 25:
                rel = 0;
                set_rel(ctl, 5);
                if ((c == 'P' && scan && stress && ctx == 1) ||
                    (c == 'T' && !scan && !stress && ctx == 1) ||
                    (c == 'K' && !stress && (scan || ctx == 0)))
                    set_rel(ctl, 3);
                else if (c == 'P' && scan && !stress && ctx == 1)
                    set_rel(ctl, 2);
                else if (c == 'P' && !stress && !scan && ctx == 0)
                    set_rel(ctl, 1);
                else if (c == 'P' && !stress && !scan && ctx == 1)
                    set_rel(ctl, 2);
                else if (c == 'K' && !stress && !scan && ctx == 1)
                    set_rel(ctl, 3);
                else if (c == 'K' && stress && !scan && ctx == 0)
                    set_rel(ctl, 4);
                else if (c == 'T' && !stress && scan == ctx)
                    set_rel(ctl, 4);
                else if (c == 'T' && stress && scan && ctx == 0)
                    set_rel(ctl, 6);
                break;
            case 3: case 12: case 14: case 18:
                rel = 2;
                set_rel(ctl, 2);
                if ((c == 'T' && stress) ||
                    (c == 'K' && ((!stress && ctx == 0) ||
                                  (stress && (!scan || ctx != 1)))))
                    set_rel(ctl, 4);
                else if (c == 'K' && stress)
                    set_rel(ctl, 5);
                else if ((c == 'P' && stress && !scan && ctx == 1) ||
                         (c == 'T' && !stress && scan && ctx == 0) ||
                         (c == 'K' && !stress && ctx == 1))
                    set_rel(ctl, 3);
                else if (c == 'P' && !stress && !scan && ctx == 0)
                    set_rel(ctl, 1);
                break;
            case 6: case 8: case 10: case 11:
                rel = 4;
                set_rel(ctl, 3);
                if (c == 'P' && stress && (scan || ctx != 0)) {
                    /* keeps the 3 it already has */
                } else if (c == 'K' && ((stress && ctx == 1) ||
                                        (!stress && scan && ctx == 0))) {
                    /* likewise */
                } else if (c == 'T' && (!stress || (!scan && ctx == 1))) {
                    set_rel(ctl, 3);
                } else if (c == 'K' && stress && scan && ctx == 0) {
                    set_rel(ctl, 3);
                } else if (c == 'P' && !stress && scan && ctx == 0) {
                    set_rel(ctl, 2);
                } else if (c == 'T' && stress && ctx == 0) {
                    set_rel(ctl, 7);
                } else if (c == 'T' && stress) {
                    set_rel(ctl, 9);
                } else if (c == 'K' && stress) {
                    set_rel(ctl, 5);
                }
                break;
            case 7: case 9: case 13: case 16: case 21: case 23:
            case 24:
                rel = 1;
                set_rel(ctl, 3);
                if ((c == 'P' && !stress) ||
                    (c == 'T' && !stress && scan == ctx))
                    set_rel(ctl, 2);
                else if ((c == 'T' && !scan &&
                          ((stress && ctx == 0) || (!stress && ctx == 1))) ||
                         (c == 'K' && ((stress && !scan && ctx == 0) ||
                                       (stress && scan && ctx == 1) ||
                                       (!stress && !scan && ctx == 1))))
                    set_rel(ctl, 4);
                else if ((c == 'T' || c == 'K') && stress)
                    set_rel(ctl, 5);
                break;
            default:
                rel = 5;
                set_rel(ctl, 1);
                if (c == 'P' && scan)
                    set_rel(ctl, 2);
                break;
            }
        }

        if (rel < 0) {
            set_rel(ctl, 0);
        } else if (c == 'T' && nx1 == 'l') {
            set_rel(ctl, 2);
        } else if (nx1 == 'N') {
            set_rel(ctl, (ctl->d10 - 1) & 0xffu);
        } else if (nw == '&' || nw == '%') {
            if (c == 'T' && pv1 == 'S' &&
                (Phone_Attr(px(nx1) | 0x100) & 2) &&
                (self->s2_next1->flags & 0x20u)) {
                set_rel(ctl, 0);
            } else if ((Phone_Attr(px(nx1) | 0x100) & 2) && c != 'K') {
                set_rel(ctl, 1);
            } else if (is_liquid(nx1)) {
                set_rel(ctl, 2);
            }
        } else if (pv1 == ' ' && ctx == 0) {
            if (is_liquid(nx1))
                set_rel(ctl, (ctl->d10 + 2) & 0xffu);
        } else if (c == 'T' && pv1 == 'S' && pw != '&' && pw != '%') {
            set_rel(ctl, 0);
        } else if ((c == 'P' || c == 'K') && pv1 == 'S' && pw != '&' &&
                   pw != '%') {
            set_rel(ctl, 1);
        }

        if (c == 'P' && nx1 == 'l' && (uint8_t)ctl->d10 != 0)
            set_rel(ctl, (ctl->d10 - 1) & 0xffu);
    }

    /* ---- the segmental corrections ---- */
    if (((Phone_Attr(px(pv1) | 0x100) & 2) || is_liquid(pv1)) &&
        (Phone_Attr(px(nx1) | 0x100) & 2) && !(st->scan->flags & 0x20u)) {
        if (c == 'D')
            v = 6;
        else if (c == 'T')
            v = 8;
        goto floor;
    }

    voiced  = (uint8_t)(Phone_Attr(px(c) | 0x100) & 1);
    c_stop  = (uint8_t)(Phone_Attr(px(c) | 0x80) & 0x20);
    pv_stop = 0;
    if (voiced == 0 || c_stop == 0 || c == 'J' || c == 'C')
        goto floor;

    pv_stop = (uint8_t)(Phone_Attr(px(pv1) | 0x80) & 0x20);
    if (!((pv_stop != 0 && !(Phone_Attr(px(pv1)) & 0x10)) ||
          pv1 == 'l' || pv1 == 'n'))
        goto floor;

    nx_stop = (uint8_t)(Phone_Attr(px(nx1) | 0x80) & 0x20);
    if (nx1 == 'l' || nx1 == 'n' || (nx_stop != 0 && !is_liquid(nx1))) {
        /* a stop cluster on both sides */
        if (((Phone_Attr(px(pv2) | 0x80) & 0x20) &&
             !(Phone_Attr(px(pv2)) & 0x10)) ||
            (Phone_Attr(px(nx2) | 0x80) & 0x20)) {
            v -= 2;
            goto floor;
        }
    }

    if ((c == 'P' || c == 'T' || c == 'K') && (Phone_Attr(px(pv1)) & 0x10) &&
        (nx_stop != 0 || nx1 == 'l' || nx1 == 'n') && !is_liquid(nx1)) {
        v -= 1;
        goto floor;
    }
    if ((pv_stop != 0 || pv1 == 'l' || pv1 == 'n') && !is_liquid(pv1) &&
        (Phone_Attr(px(pv2)) & 0x10)) {
        v -= 1;
        goto floor;
    }

    if ((c == 'B' || c == 'D' || c == 'G') && (Phone_Attr(px(pv1)) & 0x10) &&
        (nx_stop != 0 || nx1 == 'l' || nx1 == 'n') && !is_liquid(nx1)) {
        v -= 2;
        goto floor;
    }
    if (pv_stop != 0 && !is_liquid(pv1) && (Phone_Attr(px(pv2)) & 0x10)) {
        v -= 2;
        goto floor;
    }

    if ((nx_stop != 0 || nx1 == 'n' || nx1 == 'l') &&
        (Phone_Attr(px(nx2) | 0x80) & 0x20) && !is_liquid(nx2)) {
        v -= 2;
        goto floor;
    }
    if (nx2 == 'l' || nx2 == 'n') {
        v -= 2;
        goto floor;
    }
    if (pv_stop != 0 || pv1 == 'l' || pv1 == 'n') {
        if ((Phone_Attr(px(pv2) | 0x80) & 0x20) || pv2 == 'l' ||
            pv2 == 'n')
            v -= 1;
        else if (nx_stop != 0 && !is_liquid(nx1))
            v -= 1;
        else if (nx1 == 'n' || nx1 == 'l')
            v -= 1;
    }

floor:
    if (c == 'D' || c == 'T') {
        if (ctx == 0 && pv1 == 'S' && is_liquid(nx1))
            v += 1;
        if (c == 'T' && nx1 == 'p')
            v = 9;
    }
    if ((c == 'C' || c == 'J') && nx1 == 'p')
        v += 1;

    if (v != 0)
        return (v < 3) ? 3 : v;

    if (ctx != 0) {
        v = 0xb;
        if ((c == 'K' || c == 'B') && !scan)
            v = 7;
        else if ((c == 'T' || c == 'G') && !scan)
            v = 6;
        else if ((c == 'P' && !scan) || (c == 'D' && scan))
            v = 8;
        else if (c == 'P' && scan)
            v = 0xc;
        else if (c == 'T' && scan)
            v = 0xa;
        else if (c == 'D' && !scan)
            v = 3;
    }
    if (v == 0)
        v = g_dur_floor[px(c) * 2];
    return (v < 3) ? 3 : v;
}

/* ClassicVoices patch: Stage 2's working state lives in process globals (above), shared by every synth - in the
 * original each process loaded its own copy of the DLL, and our old path (tv_enua.dll in the x86 interpreter) gave
 * every voice its own image. The bridge saves / restores it around each utterance (under one lock), so every voice
 * keeps its own, starting from the image's initial values, and two voices never mix. */
struct tvtts_cv_s2 {
    int32_t state, saved, b15, count, run, total;
    uint32_t rnd;
    uint8_t value, voiced, hold;
    Node *node, *last;
};

size_t tvtts_cv_s2_size(void) { return sizeof(struct tvtts_cv_s2); }

void tvtts_cv_s2_save(void *out)
{
    struct tvtts_cv_s2 *o = (struct tvtts_cv_s2 *)out;
    o->state = g_s2_state; o->saved = g_s2_saved; o->b15 = g_s2_b15; o->count = g_s2_count; o->run = g_s2_run;
    o->total = g_s2_total; o->rnd = g_s2_rnd; o->value = g_s2_value; o->voiced = g_s2_voiced; o->hold = g_s2_hold;
    o->node = g_s2_node; o->last = g_s2_last;
}

void tvtts_cv_s2_load(const void *in)
{
    const struct tvtts_cv_s2 *o = (const struct tvtts_cv_s2 *)in;
    g_s2_state = o->state; g_s2_saved = o->saved; g_s2_b15 = o->b15; g_s2_count = o->count; g_s2_run = o->run;
    g_s2_total = o->total; g_s2_rnd = o->rnd; g_s2_value = o->value; g_s2_voiced = o->voiced; g_s2_hold = o->hold;
    g_s2_node = o->node; g_s2_last = o->last;
}
